#include "probe/observer_runtime.h"

#include "common/win32_handle.h"
#include "probe/uia_observer_worker.h"
#include "probe/win32_probe.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

namespace winexinfo {
namespace {

Status RuntimeSuccessStatus() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status RuntimeFailureStatus(
    const HRESULT hresult = S_FALSE,
    const DWORD win32 = ERROR_SUCCESS) noexcept {
    return {
        ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        hresult,
        win32,
    };
}

ObserverOperationResult RuntimeSuccessOperation() noexcept {
    return {RuntimeSuccessStatus(), std::nullopt};
}

ObserverOperationResult RuntimeContractFailure(
    const HRESULT hresult = S_FALSE,
    const DWORD win32 = ERROR_SUCCESS) noexcept {
    return {
        RuntimeFailureStatus(hresult, win32),
        ObserverFailureOrigin::Contract,
    };
}

ObserverOperationResult RuntimeTransportFailure(
    const HRESULT hresult) noexcept {
    const DWORD win32 = HRESULT_FACILITY(hresult) == FACILITY_WIN32
        ? static_cast<DWORD>(HRESULT_CODE(hresult))
        : ERROR_SUCCESS;
    return {
        RuntimeFailureStatus(hresult, win32),
        ObserverFailureOrigin::Transport,
    };
}

ObserverOperationResult RuntimeOperationFromStatus(
    const Status& status) noexcept {
    if (status.ok()) {
        return RuntimeSuccessOperation();
    }
    return {
        status,
        FAILED(status.hresult) || status.win32 != ERROR_SUCCESS
            ? std::optional{ObserverFailureOrigin::Transport}
            : std::optional{ObserverFailureOrigin::Contract},
    };
}

ObserverCleanupOutcome RuntimeCleanupFromShell(
    const ObserverShellCleanupOutcome& shell) noexcept {
    return {
        shell.status,
        shell.failure_origin,
        shell.any_transport_failure,
    };
}

ObserverCleanupOutcome RuntimeMergeCleanup(
    const ObserverCleanupOutcome& first,
    const ObserverCleanupOutcome& second) noexcept {
    if (!first.status.ok()) {
        return {
            first.status,
            first.failure_origin,
            first.any_transport_failure || second.any_transport_failure,
        };
    }
    return {
        second.status,
        second.failure_origin,
        first.any_transport_failure || second.any_transport_failure,
    };
}

class ProductionObserverRuntime final {
public:
    ProductionObserverRuntime(
        ObserverCallbackQueue* const queue,
        const HANDLE responseEvent)
        : queue_(queue),
          response_event_(responseEvent),
          shell_operations_(CreateProductionObserverShellStaOperations()),
          uia_worker_(CreateProductionObserverUiaWorkerOperations(queue)) {}

    ObserverStartupOutcome Startup() {
        runtime_stage_ = "startup.shell";
        ObserverShellStartupOutcome shellOutcome{};
        const Status shellStarted = StartObserverShellStaResources(
            queue_,
            shell_operations_,
            &shell_graph_,
            &shellOutcome);
        ObserverStartupOutcome outcome{
            shellOutcome.setup,
            shellOutcome.any_setup_transport_failure,
            shellOutcome.first_setup_transport_status,
            RuntimeCleanupFromShell(shellOutcome.rollback),
        };
        if (!shellStarted.ok()) {
            return outcome;
        }
        current_shell_metadata_ = shell_graph_.startup_state.baseline;

        std::vector<ObserverUiaTarget> initialTargets;
        for (const HWND topLevel : shell_graph_.target_top_levels) {
            const std::size_t entryCount = static_cast<std::size_t>(
                std::count_if(
                    current_shell_metadata_.begin(),
                    current_shell_metadata_.end(),
                    [topLevel](const ObserverShellEntryMetadata& entry) {
                        return entry.target_matched &&
                            entry.top_level == topLevel;
                    }));
            LogicalActiveViewState logical{};
            HWND selectedShellTab = nullptr;
            HWND uiaActiveView = nullptr;
            ObserverOperationResult captured = CaptureLogicalState(
                topLevel, &selectedShellTab, &uiaActiveView, &logical);
            if (!captured.ok() || !logical.status.ok() || entryCount == 0) {
                if (captured.ok()) {
                    captured = RuntimeContractFailure();
                }
                return FailAfterShellStartup(captured);
            }
            const std::uint64_t generation = 1;
            const Status seeded = reducer_.SeedWindow(
                topLevel, generation, entryCount, logical);
            if (!seeded.ok()) {
                return FailAfterShellStartup(
                    RuntimeOperationFromStatus(seeded));
            }
            for (const ObserverShellEntryMetadata& entry :
                 current_shell_metadata_) {
                if (!entry.target_matched || entry.top_level != topLevel) {
                    continue;
                }
                const Status entrySeeded = reducer_.SeedInitialShellEntry({
                    static_cast<std::uint64_t>(entry.canonical_identity),
                    topLevel,
                    generation,
                    entry.shell_tab,
                });
                if (!entrySeeded.ok()) {
                    return FailAfterShellStartup(
                        RuntimeOperationFromStatus(entrySeeded));
                }
            }
            const Status finalized = reducer_.FinalizeInitialShellEntries(
                topLevel, generation);
            if (!finalized.ok()) {
                return FailAfterShellStartup(
                    RuntimeOperationFromStatus(finalized));
            }
            active_generations_[topLevel] = generation;
            latest_generations_[topLevel] = generation;
            initialTargets.push_back({
                topLevel,
                generation,
                uiaActiveView,
                selectedShellTab,
            });
        }
        const Status baselineFinalized = reducer_.FinalizeInitialBaseline();
        if (!baselineFinalized.ok()) {
            return FailAfterShellStartup(
                RuntimeOperationFromStatus(baselineFinalized));
        }

        ObserverOperationResult workerStarted = uia_worker_.Start(response_event_);
        if (!workerStarted.ok()) {
            return FailAfterShellStartup(workerStarted);
        }
        uia_started_ = true;
        for (const ObserverUiaTarget& target : initialTargets) {
            std::uint64_t commandId = 0;
            ObserverOperationResult submitted = uia_worker_.Submit(
                ObserverUiaCommandKind::AddTarget,
                target,
                &commandId);
            if (!submitted.ok()) {
                return FailAfterShellStartup(submitted);
            }
            const DWORD waited = WaitForSingleObject(response_event_, 5000);
            if (waited != WAIT_OBJECT_0) {
                return FailAfterShellStartup(
                    waited == WAIT_FAILED
                        ? RuntimeTransportFailure(
                            HRESULT_FROM_WIN32(GetLastError()))
                        : RuntimeTransportFailure(
                            HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
            }
            ObserverUiaResponse response{};
            ObserverOperationResult consumed = uia_worker_.Consume(
                commandId, &response);
            if (!consumed.ok()) {
                return FailAfterShellStartup(consumed);
            }
            if (!response.operation.ok()) {
                return FailAfterShellStartup(response.operation);
            }
            uia_targets_.push_back(target);
        }
        const auto emergency = queue_->emergency();
        if (emergency.has_value()) {
            return FailAfterShellStartup({
                emergency->first_failure.status,
                emergency->first_failure.origin,
            });
        }
        runtime_stage_ = "startup.complete";
        return outcome;
    }

    [[nodiscard]] const char* runtime_stage() const noexcept {
        return runtime_stage_;
    }

    ObserverEventProcessingResult ProcessEvent(
        const ObserverCallbackEnvelope& envelope) {
        if (envelope.payload.source == ObserverCallbackSource::ShellLifecycle) {
            runtime_stage_ = "lifecycle.begin";
            std::uintptr_t resolvedAddedIdentity = 0;
            if (envelope.payload.kind != ObservedEventKind::WindowRegistered &&
                envelope.payload.kind != ObservedEventKind::WindowRevoked) {
                switch (envelope.payload.kind) {
                    case ObservedEventKind::NavigateComplete2:
                        runtime_stage_ =
                            "lifecycle.invalid.navigate_complete2";
                        break;
                    case ObservedEventKind::TabSelected:
                        runtime_stage_ = "lifecycle.invalid.tab_selected";
                        break;
                    case ObservedEventKind::TabStructureChanged:
                        runtime_stage_ =
                            "lifecycle.invalid.tab_structure_changed";
                        break;
                    default:
                        runtime_stage_ = "lifecycle.invalid.unknown";
                        break;
                }
                return FailureResult(
                    envelope.sequence, RuntimeContractFailure());
            }
            if (envelope.payload.kind ==
                ObservedEventKind::WindowRegistered) {
                runtime_stage_ = "lifecycle.registered.validate_cookie";
                if (registered_shell_entries_.contains(
                        envelope.payload.shell_cookie)) {
                    return FailureResult(
                        envelope.sequence, RuntimeContractFailure());
                }
            } else {
                runtime_stage_ = "lifecycle.revoked.begin";
            }

            std::vector<ExplorerWindowRecord> windows;
            runtime_stage_ = "lifecycle.enumerate_windows";
            ObserverOperationResult enumerated =
                shell_operations_.enumerate_windows(&windows);
            if (!enumerated.ok()) {
                return FailureResult(envelope.sequence, enumerated);
            }
            std::vector<HWND> refreshedTargets;
            for (const ExplorerWindowRecord& window : windows) {
                if (window.hwnd == nullptr || window.process_id == 0 ||
                    window.thread_id == 0 ||
                    std::find(
                        refreshedTargets.begin(),
                        refreshedTargets.end(),
                        window.hwnd) != refreshedTargets.end()) {
                    return FailureResult(
                        envelope.sequence, RuntimeContractFailure());
                }
                refreshedTargets.push_back(window.hwnd);
            }
            const std::vector<HWND>& captureTargets = refreshedTargets.empty()
                ? shell_graph_.target_top_levels
                : refreshedTargets;
            if (captureTargets.empty()) {
                return FailureResult(
                    envelope.sequence, RuntimeContractFailure());
            }
            ObserverShellStaCapture refreshedCapture{};
            runtime_stage_ = "lifecycle.capture_shell";
            ObserverOperationResult refreshed = shell_operations_.capture(
                shell_graph_.shell_windows.Get(),
                captureTargets,
                &refreshedCapture);
            if (!refreshed.ok()) {
                return FailureResult(envelope.sequence, refreshed);
            }
            std::vector<ObserverShellEntryMetadata> refreshedMetadata;
            refreshedMetadata.reserve(refreshedCapture.browsers.size());
            for (const ObserverShellStaCapturedBrowser& browser :
                 refreshedCapture.browsers) {
                if (browser.metadata.canonical_identity == 0 ||
                    browser.metadata.top_level == nullptr ||
                    browser.canonical_identity == nullptr ||
                    browser.browser == nullptr ||
                    browser.metadata.target_matched !=
                        (browser.metadata.shell_tab != nullptr) ||
                    reinterpret_cast<std::uintptr_t>(
                        browser.canonical_identity.Get()) !=
                        browser.metadata.canonical_identity) {
                    return FailureResult(
                        envelope.sequence, RuntimeContractFailure());
                }
                refreshedMetadata.push_back(browser.metadata);
            }

            ObserverShellLifecycleCorrelation correlation{};
            runtime_stage_ = "lifecycle.correlate";
            const Status correlated = CorrelateObserverShellLifecycle(
                envelope.payload.kind,
                current_shell_metadata_,
                refreshedMetadata,
                resolvedAddedIdentity,
                active_generations_,
                latest_generations_,
                &correlation);
            if (!correlated.ok()) {
                return FailureResult(
                    envelope.sequence, RuntimeOperationFromStatus(correlated));
            }
            if (!correlation.target_matched) {
                shell_graph_.target_top_levels = captureTargets;
                shell_graph_.browser_set =
                    std::move(refreshedCapture.browser_set);
                shell_graph_.captured_browsers =
                    std::move(refreshedCapture.browsers);
                current_shell_metadata_ = std::move(refreshedMetadata);
                runtime_stage_ = "lifecycle.ignored";
                return {
                    RuntimeSuccessOperation(),
                    ObserverEventDisposition::Ignored,
                    envelope.sequence,
                    std::nullopt,
                };
            }

            if (envelope.payload.kind ==
                ObservedEventKind::WindowRegistered) {
                const auto capturedBrowser = std::find_if(
                    refreshedCapture.browsers.begin(),
                    refreshedCapture.browsers.end(),
                    [&](const ObserverShellStaCapturedBrowser& browser) {
                        return browser.metadata.canonical_identity ==
                            correlation.entry.canonical_identity;
                    });
                if (capturedBrowser == refreshedCapture.browsers.end() ||
                    shell_graph_.next_registration_id == 0 ||
                    shell_graph_.next_registration_id == UINT64_MAX) {
                    return FailureResult(
                        envelope.sequence, RuntimeContractFailure());
                }
                shell_graph_.browser_resources.reserve(
                    shell_graph_.browser_resources.size() + 1);
                shell_graph_.startup_state.browser_registrations.reserve(
                    shell_graph_.startup_state.browser_registrations.size() +
                    1);
                Microsoft::WRL::ComPtr<IDispatch> sink;
                runtime_stage_ = "lifecycle.create_browser_sink";
                ObserverOperationResult created =
                    shell_operations_.create_browser_sink(
                        queue_,
                        capturedBrowser->canonical_identity.Get(),
                        correlation.entry.top_level,
                        correlation.generation,
                        correlation.entry.shell_tab,
                        sink);
                if (!created.ok() || sink == nullptr) {
                    return FailureResult(
                        envelope.sequence,
                        created.ok() ? RuntimeContractFailure() : created);
                }
                const std::uint64_t registrationId =
                    shell_graph_.next_registration_id;
                ObserverShellStaBrowserResource resource{
                    registrationId,
                    correlation.entry.canonical_identity,
                    capturedBrowser->browser,
                    {},
                };
                runtime_stage_ = "lifecycle.advise_browser";
                ObserverOperationResult advised = shell_operations_.advise(
                    resource.browser.Get(),
                    DIID_DWebBrowserEvents2,
                    sink.Get(),
                    &resource.connection);
                if (!advised.ok() ||
                    resource.connection.connection_point == nullptr ||
                    resource.connection.sink == nullptr ||
                    resource.connection.subscription_cookie == 0 ||
                    resource.connection.owner_thread_id !=
                        shell_graph_.owner_thread_id) {
                    return FailureResult(
                        envelope.sequence,
                        advised.ok() ? RuntimeContractFailure() : advised);
                }
                shell_graph_.browser_resources.push_back(std::move(resource));
                shell_graph_.startup_state.browser_registrations.push_back({
                    correlation.entry.canonical_identity,
                    registrationId,
                });
                ++shell_graph_.next_registration_id;
            } else {
                const auto resource = std::find_if(
                    shell_graph_.browser_resources.begin(),
                    shell_graph_.browser_resources.end(),
                    [&](const ObserverShellStaBrowserResource& browser) {
                        return browser.canonical_identity ==
                            correlation.entry.canonical_identity;
                    });
                if (resource == shell_graph_.browser_resources.end()) {
                    return FailureResult(
                        envelope.sequence, RuntimeContractFailure());
                }
                const std::uint64_t registrationId =
                    resource->registration_id;
                runtime_stage_ = "lifecycle.unadvise_browser";
                ObserverOperationResult unadvised =
                    shell_operations_.unadvise(&resource->connection);
                if (!unadvised.ok()) {
                    return FailureResult(envelope.sequence, unadvised);
                }
                shell_graph_.browser_resources.erase(resource);
                const auto startupRegistration = std::find_if(
                    shell_graph_.startup_state.browser_registrations.begin(),
                    shell_graph_.startup_state.browser_registrations.end(),
                    [registrationId](
                        const ObserverShellBrowserRegistration& registration) {
                        return registration.registration_id == registrationId;
                    });
                if (startupRegistration ==
                    shell_graph_.startup_state.browser_registrations.end()) {
                    return FailureResult(
                        envelope.sequence, RuntimeContractFailure());
                }
                shell_graph_.startup_state.browser_registrations.erase(
                    startupRegistration);
            }

            shell_graph_.target_top_levels = captureTargets;
            shell_graph_.browser_set = std::move(refreshedCapture.browser_set);
            shell_graph_.captured_browsers =
                std::move(refreshedCapture.browsers);
            current_shell_metadata_ = std::move(refreshedMetadata);

            const ObservedEventTrigger trigger{
                envelope.payload.kind,
                correlation.entry.top_level,
                correlation.generation,
                true,
                correlation.entry.shell_tab,
                envelope.payload.kind == ObservedEventKind::WindowRevoked,
                true,
                envelope.payload.shell_cookie,
                ObservedStructureChangeType::None,
            };
            if (envelope.payload.kind ==
                ObservedEventKind::WindowRegistered) {
                registered_shell_entries_.emplace(
                    envelope.payload.shell_cookie,
                    correlation.entry.canonical_identity);
                if (correlation.new_top_level) {
                    active_generations_[correlation.entry.top_level] =
                        correlation.generation;
                    latest_generations_[correlation.entry.top_level] =
                        correlation.generation;
                }
                LogicalActiveViewState logical{};
                HWND selectedShellTab = nullptr;
                HWND uiaActiveView = nullptr;
                runtime_stage_ = "lifecycle.capture_logical";
                ObserverOperationResult captured = CaptureLogicalState(
                    correlation.entry.top_level,
                    &selectedShellTab,
                    &uiaActiveView,
                    &logical);
                if (!captured.ok()) {
                    return FailureResult(envelope.sequence, captured);
                }
                if (correlation.new_top_level && logical.status.ok()) {
                    const ObserverUiaTarget target{
                        correlation.entry.top_level,
                        correlation.generation,
                        uiaActiveView,
                        selectedShellTab,
                    };
                    std::uint64_t commandId = 0;
                    ObserverOperationResult submitted = uia_worker_.Submit(
                        ObserverUiaCommandKind::AddTarget,
                        target,
                        &commandId);
                    if (!submitted.ok()) {
                        return FailureResult(envelope.sequence, submitted);
                    }
                    const DWORD waited = WaitForSingleObject(
                        response_event_, 5000);
                    if (waited != WAIT_OBJECT_0) {
                        return FailureResult(
                            envelope.sequence,
                            waited == WAIT_FAILED
                                ? RuntimeTransportFailure(
                                    HRESULT_FROM_WIN32(GetLastError()))
                                : RuntimeTransportFailure(
                                    HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
                    }
                    ObserverUiaResponse response{};
                    ObserverOperationResult consumed = uia_worker_.Consume(
                        commandId, &response);
                    if (!consumed.ok() || !response.operation.ok()) {
                        return FailureResult(
                            envelope.sequence,
                            consumed.ok() ? response.operation : consumed);
                    }
                    uia_targets_.push_back(target);
                }
                ObservedEventRecord record{};
                const LogicalActiveViewState registrationState =
                    logical.status.ok()
                    ? LogicalActiveViewState{
                          nullptr,
                          0,
                          false,
                          {},
                          {
                              ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                              S_FALSE,
                              ERROR_SUCCESS,
                          },
                      }
                    : logical;
                runtime_stage_ = "lifecycle.record_registered";
                const Status recorded = reducer_.RecordRegistered(
                    trigger,
                    correlation.top_level_entry_count,
                    registrationState,
                    &record);
                if (!recorded.ok()) {
                    return FailureResult(
                        envelope.sequence,
                        RuntimeOperationFromStatus(recorded));
                }
                runtime_stage_ = "lifecycle.complete";
                return {
                    RuntimeSuccessOperation(),
                    ObserverEventDisposition::Completed,
                    envelope.sequence,
                    std::move(record),
                };
            }

            const LogicalActiveViewState terminal{
                nullptr,
                0,
                false,
                {},
                RuntimeSuccessStatus(),
            };
            ObservedEventRecord record{};
            const auto registered = registered_shell_entries_.find(
                envelope.payload.shell_cookie);
            if (registered == registered_shell_entries_.end() ||
                registered->second != correlation.entry.canonical_identity) {
                return FailureResult(
                    envelope.sequence, RuntimeContractFailure());
            }
            const Status recorded = reducer_.RecordRevoked(
                trigger,
                correlation.top_level_entry_count,
                terminal,
                &record);
            if (!recorded.ok()) {
                return FailureResult(
                    envelope.sequence, RuntimeOperationFromStatus(recorded));
            }
            registered_shell_entries_.erase(registered);

            if (correlation.top_level_entry_count == 0) {
                const auto target = std::find_if(
                    uia_targets_.begin(),
                    uia_targets_.end(),
                    [&](const ObserverUiaTarget& candidate) {
                        return candidate.top_level ==
                                correlation.entry.top_level &&
                            candidate.generation == correlation.generation;
                    });
                if (target != uia_targets_.end()) {
                    std::uint64_t commandId = 0;
                    ObserverOperationResult submitted = uia_worker_.Submit(
                        ObserverUiaCommandKind::RemoveTarget,
                        *target,
                        &commandId);
                    if (!submitted.ok()) {
                        return FailureResult(envelope.sequence, submitted);
                    }
                    const DWORD waited = WaitForSingleObject(
                        response_event_, 5000);
                    if (waited != WAIT_OBJECT_0) {
                        return FailureResult(
                            envelope.sequence,
                            waited == WAIT_FAILED
                                ? RuntimeTransportFailure(
                                    HRESULT_FROM_WIN32(GetLastError()))
                                : RuntimeTransportFailure(
                                    HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
                    }
                    ObserverUiaResponse response{};
                    ObserverOperationResult consumed = uia_worker_.Consume(
                        commandId, &response);
                    if (!consumed.ok() || !response.operation.ok()) {
                        return FailureResult(
                            envelope.sequence,
                            consumed.ok() ? response.operation : consumed);
                    }
                    uia_targets_.erase(target);
                }
                active_generations_.erase(correlation.entry.top_level);
            }
            runtime_stage_ = "lifecycle.complete";
            return {
                RuntimeSuccessOperation(),
                ObserverEventDisposition::Completed,
                envelope.sequence,
                std::move(record),
            };
        }
        if (envelope.payload.source == ObserverCallbackSource::BrowserNavigate) {
            Win32ClassTree tree{};
            const Status capturedTree = CaptureWin32ClassTree(
                envelope.payload.top_level, &tree);
            if (!capturedTree.ok()) {
                return FailureResult(
                    envelope.sequence,
                    RuntimeOperationFromStatus(capturedTree));
            }
            const Win32ContractResult win32 = ValidateWin32Contract(tree);
            if (!win32.status.ok()) {
                return FailureResult(
                    envelope.sequence,
                    RuntimeContractFailure(
                        win32.status.hresult,
                        win32.status.win32));
            }
            if (envelope.payload.shell_tab != win32.active_shell_tab) {
                return {
                    RuntimeSuccessOperation(),
                    ObserverEventDisposition::Ignored,
                    envelope.sequence,
                    std::nullopt,
                };
            }
        }
        const ObservedEventTrigger trigger{
            envelope.payload.kind,
            envelope.payload.top_level,
            envelope.payload.generation,
            true,
            envelope.payload.shell_tab,
            envelope.payload.source == ObserverCallbackSource::BrowserNavigate,
            false,
            0,
            envelope.payload.structure_change_type,
        };
        bool accepted = false;
        runtime_stage_ = "event.begin_remap";
        const Status began = reducer_.BeginRemap(trigger, &accepted);
        if (!began.ok()) {
            return FailureResult(
                envelope.sequence, RuntimeOperationFromStatus(began));
        }
        if (!accepted) {
            return {
                RuntimeSuccessOperation(),
                ObserverEventDisposition::Ignored,
                envelope.sequence,
                std::nullopt,
            };
        }
        runtime_stage_ = "event.refresh_shell";
        ObserverOperationResult refreshed = RefreshShellCapture();
        if (!refreshed.ok()) {
            return FailureResult(envelope.sequence, refreshed);
        }
        LogicalActiveViewState logical{};
        HWND selectedShellTab = nullptr;
        HWND uiaActiveView = nullptr;
        runtime_stage_ = "event.capture_logical";
        ObserverOperationResult captured = CaptureLogicalState(
            envelope.payload.top_level,
            &selectedShellTab,
            &uiaActiveView,
            &logical);
        if (!captured.ok()) {
            return FailureResult(envelope.sequence, captured);
        }
        if (envelope.payload.source == ObserverCallbackSource::BrowserNavigate &&
            envelope.payload.shell_tab != selectedShellTab) {
            return FailureResult(
                envelope.sequence, RuntimeContractFailure());
        }
        if (envelope.payload.source == ObserverCallbackSource::BrowserNavigate &&
            logical.status.ok() &&
            std::none_of(
                uia_targets_.begin(),
                uia_targets_.end(),
                [&](const ObserverUiaTarget& target) {
                    return target.top_level == envelope.payload.top_level &&
                        target.generation == envelope.payload.generation;
                })) {
            const auto active = active_generations_.find(
                envelope.payload.top_level);
            if (active == active_generations_.end() ||
                active->second != envelope.payload.generation) {
                return FailureResult(
                    envelope.sequence, RuntimeContractFailure());
            }
            const ObserverUiaTarget target{
                envelope.payload.top_level,
                envelope.payload.generation,
                uiaActiveView,
                selectedShellTab,
            };
            std::uint64_t commandId = 0;
            runtime_stage_ = "event.add_uia_target";
            ObserverOperationResult submitted = uia_worker_.Submit(
                ObserverUiaCommandKind::AddTarget,
                target,
                &commandId);
            if (!submitted.ok()) {
                return FailureResult(envelope.sequence, submitted);
            }
            const DWORD waited = WaitForSingleObject(response_event_, 5000);
            if (waited != WAIT_OBJECT_0) {
                return FailureResult(
                    envelope.sequence,
                    waited == WAIT_FAILED
                        ? RuntimeTransportFailure(
                            HRESULT_FROM_WIN32(GetLastError()))
                        : RuntimeTransportFailure(
                            HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
            }
            ObserverUiaResponse response{};
            ObserverOperationResult consumed = uia_worker_.Consume(
                commandId, &response);
            if (!consumed.ok() || !response.operation.ok()) {
                return FailureResult(
                    envelope.sequence,
                    consumed.ok() ? response.operation : consumed);
            }
            uia_targets_.push_back(target);
        }
        if (envelope.payload.source == ObserverCallbackSource::UiaStructure) {
            const auto target = std::find_if(
                uia_targets_.begin(),
                uia_targets_.end(),
                [&](const ObserverUiaTarget& candidate) {
                    return candidate.top_level == envelope.payload.top_level &&
                        candidate.generation == envelope.payload.generation;
                });
            if (target == uia_targets_.end() ||
                pending_command_id_.has_value()) {
                return FailureResult(
                    envelope.sequence, RuntimeContractFailure());
            }
            std::uint64_t commandId = 0;
            runtime_stage_ = "event.reenumerate_uia";
            ObserverOperationResult submitted = uia_worker_.Submit(
                ObserverUiaCommandKind::Reenumerate,
                *target,
                &commandId);
            if (!submitted.ok()) {
                return FailureResult(envelope.sequence, submitted);
            }
            pending_command_id_ = commandId;
            pending_raw_sequence_ = envelope.sequence;
            pending_top_level_ = envelope.payload.top_level;
            return {
                RuntimeSuccessOperation(),
                ObserverEventDisposition::Pending,
                envelope.sequence,
                std::nullopt,
            };
        }
        ObservedEventRecord record{};
        runtime_stage_ = "event.complete_remap";
        const Status completed = reducer_.CompleteRemap(logical, &record);
        if (!completed.ok()) {
            return FailureResult(
                envelope.sequence, RuntimeOperationFromStatus(completed));
        }
        runtime_stage_ = "event.complete";
        return {
            RuntimeSuccessOperation(),
            ObserverEventDisposition::Completed,
            envelope.sequence,
            std::move(record),
        };
    }

    ObserverEventProcessingResult ProcessResponse(
        const std::uint64_t rawSequence) {
        runtime_stage_ = "response.consume";
        if (rawSequence == 0) {
            ObserverUiaResponse discarded{};
            const ObserverOperationResult consumed =
                uia_worker_.Consume(0, &discarded);
            return {
                consumed,
                ObserverEventDisposition::Ignored,
                0,
                std::nullopt,
            };
        }
        if (!pending_command_id_.has_value() ||
            pending_raw_sequence_ != rawSequence) {
            return FailureResult(rawSequence, RuntimeContractFailure());
        }
        ObserverUiaResponse response{};
        ObserverOperationResult consumed = uia_worker_.Consume(
            *pending_command_id_, &response);
        if (!consumed.ok()) {
            return FailureResult(rawSequence, consumed);
        }
        pending_command_id_.reset();
        pending_raw_sequence_ = 0;
        if (!response.operation.ok()) {
            return FailureResult(rawSequence, response.operation);
        }
        runtime_stage_ = "response.refresh_shell";
        ObserverOperationResult refreshed = RefreshShellCapture();
        if (!refreshed.ok()) {
            return FailureResult(rawSequence, refreshed);
        }
        const std::size_t shellEntryCount = static_cast<std::size_t>(
            std::count_if(
                current_shell_metadata_.begin(),
                current_shell_metadata_.end(),
                [&](const ObserverShellEntryMetadata& entry) {
                    return entry.target_matched &&
                        entry.top_level == pending_top_level_;
                }));
        if (response.direct_child_count != shellEntryCount) {
            return FailureResult(rawSequence, RuntimeContractFailure());
        }
        LogicalActiveViewState logical{};
        HWND selectedShellTab = nullptr;
        runtime_stage_ = "response.capture_logical";
        ObserverOperationResult captured = CaptureLogicalState(
            pending_top_level_, &selectedShellTab, nullptr, &logical);
        if (!captured.ok()) {
            return FailureResult(rawSequence, captured);
        }
        ObservedEventRecord record{};
        runtime_stage_ = "response.complete_remap";
        const Status completed = reducer_.CompleteRemap(logical, &record);
        if (!completed.ok()) {
            return FailureResult(
                rawSequence, RuntimeOperationFromStatus(completed));
        }
        runtime_stage_ = "response.complete";
        return {
            RuntimeSuccessOperation(),
            ObserverEventDisposition::Completed,
            rawSequence,
            std::move(record),
        };
    }

    ObserverOperationResult PumpMessages() {
        return PumpObserverShellStaMessages(
            shell_operations_, shell_graph_);
    }

    ObserverOperationResult BeginStop() {
        return uia_started_
            ? uia_worker_.BeginStopping()
            : RuntimeSuccessOperation();
    }

    ObserverCleanupOutcome Cleanup() {
        if (cleanup_complete_) {
            return {RuntimeSuccessStatus(), std::nullopt, false};
        }
        ObserverCleanupOutcome uiaCleanup{
            RuntimeSuccessStatus(), std::nullopt, false};
        if (uia_started_) {
            uiaCleanup = uia_worker_.Join();
            uia_started_ = false;
        }
        const ObserverCleanupOutcome shellCleanup = RuntimeCleanupFromShell(
            CleanupObserverShellStaResources(
                shell_operations_, &shell_graph_));
        const ObserverCleanupOutcome merged =
            RuntimeMergeCleanup(uiaCleanup, shellCleanup);
        if (merged.status.ok()) {
            cleanup_complete_ = true;
        }
        return merged;
    }

private:
    ObserverStartupOutcome FailAfterShellStartup(
        const ObserverOperationResult& failure) {
        static_cast<void>(queue_->BeginStopping());
        ObserverCleanupOutcome cleanup{
            RuntimeSuccessStatus(), std::nullopt, false};
        if (uia_started_) {
            static_cast<void>(uia_worker_.BeginStopping());
            cleanup = uia_worker_.Join();
            uia_started_ = false;
        }
        cleanup = RuntimeMergeCleanup(
            cleanup,
            RuntimeCleanupFromShell(CleanupObserverShellStaResources(
                shell_operations_, &shell_graph_)));
        if (cleanup.status.ok()) {
            cleanup_complete_ = true;
        }
        return {
            failure,
            failure.failure_origin == ObserverFailureOrigin::Transport ||
                cleanup.any_transport_failure,
            failure.failure_origin == ObserverFailureOrigin::Transport
                ? failure.status
                : RuntimeSuccessStatus(),
            cleanup,
        };
    }

    ObserverOperationResult RefreshShellCapture() {
        ObserverShellStaCapture capture{};
        ObserverOperationResult refreshed = shell_operations_.capture(
            shell_graph_.shell_windows.Get(),
            shell_graph_.target_top_levels,
            &capture);
        if (!refreshed.ok()) {
            return refreshed;
        }
        std::vector<ObserverShellEntryMetadata> metadata;
        try {
            metadata.reserve(capture.browsers.size());
            for (const ObserverShellStaCapturedBrowser& browser :
                 capture.browsers) {
                metadata.push_back(browser.metadata);
            }
        } catch (const std::bad_alloc&) {
            return RuntimeTransportFailure(E_OUTOFMEMORY);
        }
        shell_graph_.browser_set = std::move(capture.browser_set);
        shell_graph_.captured_browsers = std::move(capture.browsers);
        current_shell_metadata_ = std::move(metadata);
        return RuntimeSuccessOperation();
    }

    ObserverOperationResult CaptureLogicalState(
        const HWND topLevel,
        HWND* const selectedShellTab,
        HWND* const uiaActiveView,
        LogicalActiveViewState* const output) {
        if (topLevel == nullptr || selectedShellTab == nullptr ||
            output == nullptr) {
            return RuntimeContractFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        Win32ClassTree tree{};
        const Status treeStatus = CaptureWin32ClassTree(topLevel, &tree);
        if (!treeStatus.ok()) {
            return RuntimeOperationFromStatus(treeStatus);
        }
        const Win32ContractResult win32 = ValidateWin32Contract(tree);
        if (!win32.status.ok()) {
            if (FAILED(win32.status.hresult) ||
                win32.status.win32 != ERROR_SUCCESS) {
                return RuntimeOperationFromStatus(win32.status);
            }
            *output = {
                nullptr,
                0,
                false,
                {},
                {
                    ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                    win32.status.hresult,
                    win32.status.win32,
                },
            };
            *selectedShellTab = nullptr;
            if (uiaActiveView != nullptr) {
                *uiaActiveView = nullptr;
            }
            return RuntimeSuccessOperation();
        }
        ActiveShellViewSnapshot snapshot{};
        const Status shellStatus = CaptureActiveShellViewFromBrowserSet(
            shell_graph_.browser_set,
            topLevel,
            win32.active_shell_tab,
            &snapshot);
        if (!shellStatus.ok() &&
            (FAILED(shellStatus.hresult) ||
             shellStatus.win32 != ERROR_SUCCESS)) {
            return RuntimeOperationFromStatus(shellStatus);
        }
        *selectedShellTab = win32.active_shell_tab;
        if (uiaActiveView != nullptr) {
            *uiaActiveView = win32.active_view;
        }
        *output = {
            shellStatus.ok() ? snapshot.active_view : nullptr,
            snapshot.active_view_count,
            shellStatus.ok() && snapshot.filesystem_path_available,
            shellStatus.ok() ? snapshot.filesystem_path : std::wstring{},
            shellStatus,
        };
        return RuntimeSuccessOperation();
    }

    static ObserverEventProcessingResult FailureResult(
        const std::uint64_t rawSequence,
        const ObserverOperationResult& operation) {
        return {
            operation,
            ObserverEventDisposition::Ignored,
            rawSequence,
            std::nullopt,
        };
    }

    ObserverCallbackQueue* queue_;
    HANDLE response_event_;
    ObserverShellStaOperations shell_operations_;
    ObserverShellStaResourceGraph shell_graph_{};
    EventCorrelationReducer reducer_{};
    ObserverUiaMtaWorker uia_worker_;
    bool uia_started_ = false;
    std::vector<ObserverShellEntryMetadata> current_shell_metadata_;
    std::map<HWND, std::uint64_t> active_generations_;
    std::map<HWND, std::uint64_t> latest_generations_;
    std::map<LONG, std::uintptr_t> registered_shell_entries_;
    std::vector<ObserverUiaTarget> uia_targets_;
    std::optional<std::uint64_t> pending_command_id_;
    std::uint64_t pending_raw_sequence_ = 0;
    HWND pending_top_level_ = nullptr;
    bool cleanup_complete_ = false;
    const char* runtime_stage_ = "created";
};

}  // namespace

ObserverOperationResult EvaluateEventObservationGate(
    const EventObservationSnapshot& snapshot,
    bool* const output) noexcept {
    if (output == nullptr) {
        return RuntimeContractFailure(
            E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    *output = false;
    if (snapshot.event_count != snapshot.events.size()) {
        return RuntimeContractFailure();
    }
    ObservedEventKindCounts counts{};
    bool terminalRevoke = false;
    bool navigatePathTransition = false;
    bool cleanEvents = true;
    std::set<std::tuple<std::uintptr_t, std::uint64_t, LONG>> pending;
    for (const ObservedEventRecord& event : snapshot.events) {
        switch (event.kind) {
            case ObservedEventKind::WindowRegistered:
                ++counts.window_registered;
                break;
            case ObservedEventKind::WindowRevoked:
                ++counts.window_revoked;
                break;
            case ObservedEventKind::NavigateComplete2:
                ++counts.navigate_complete2;
                break;
            case ObservedEventKind::TabSelected:
                ++counts.tab_selected;
                break;
            case ObservedEventKind::TabStructureChanged:
                ++counts.tab_structure_changed;
                break;
            default:
                return RuntimeContractFailure();
        }
        bool transitionShapeValid = false;
        switch (event.transition) {
            case ObservedEventTransition::Pending:
                transitionShapeValid =
                    event.kind == ObservedEventKind::WindowRegistered &&
                    event.current_active_view == nullptr &&
                    event.active_view_count == 0 &&
                    !event.current_filesystem_path_available &&
                    event.current_filesystem_path.empty();
                break;
            case ObservedEventTransition::Remapped:
                transitionShapeValid = event.current_active_view != nullptr &&
                    event.active_view_count == 1 &&
                    (event.current_filesystem_path_available !=
                     event.current_filesystem_path.empty());
                break;
            case ObservedEventTransition::Revoked:
                transitionShapeValid =
                    event.kind == ObservedEventKind::WindowRevoked &&
                    event.current_active_view == nullptr &&
                    event.active_view_count == 0 &&
                    !event.current_filesystem_path_available &&
                    event.current_filesystem_path.empty();
                break;
            case ObservedEventTransition::Mismatch:
                transitionShapeValid = false;
                break;
        }
        cleanEvents = cleanEvents && event.status.ok() &&
            transitionShapeValid;
        terminalRevoke = terminalRevoke ||
            (event.kind == ObservedEventKind::WindowRevoked &&
             event.transition == ObservedEventTransition::Revoked);
        navigatePathTransition = navigatePathTransition ||
            (event.kind == ObservedEventKind::NavigateComplete2 &&
             event.transition == ObservedEventTransition::Remapped &&
             event.previous_filesystem_path_available &&
             event.current_filesystem_path_available &&
             event.previous_filesystem_path != event.current_filesystem_path);
        const auto key = std::tuple{
            reinterpret_cast<std::uintptr_t>(event.source_top_level),
            event.generation,
            event.shell_cookie,
        };
        if (event.transition == ObservedEventTransition::Pending) {
            pending.insert(key);
        } else if (event.kind == ObservedEventKind::WindowRevoked) {
            pending.erase(key);
        } else if (
            event.transition == ObservedEventTransition::Remapped &&
            (event.kind == ObservedEventKind::NavigateComplete2 ||
             event.kind == ObservedEventKind::TabStructureChanged)) {
            std::erase_if(
                pending,
                [&](const auto& pendingEntry) {
                    return std::get<0>(pendingEntry) ==
                            reinterpret_cast<std::uintptr_t>(
                                event.source_top_level) &&
                        std::get<1>(pendingEntry) == event.generation;
                });
        }
    }
    if (counts.window_registered != snapshot.kind_counts.window_registered ||
        counts.window_revoked != snapshot.kind_counts.window_revoked ||
        counts.navigate_complete2 != snapshot.kind_counts.navigate_complete2 ||
        counts.tab_selected != snapshot.kind_counts.tab_selected ||
        counts.tab_structure_changed !=
            snapshot.kind_counts.tab_structure_changed) {
        return RuntimeContractFailure();
    }
    *output = snapshot.runtime_status.ok() && snapshot.cleanup_status.ok() &&
        cleanEvents && pending.empty() && terminalRevoke &&
        navigatePathTransition && counts.window_registered > 0 &&
        counts.window_revoked > 0 && counts.navigate_complete2 > 0 &&
        counts.tab_selected > 0 && counts.tab_structure_changed > 0;
    return RuntimeSuccessOperation();
}

Status RunObserverRuntime(
    const std::uint32_t durationMs,
    ObserverRuntimeOutcome* const output) {
    if (output == nullptr) {
        return RuntimeFailureStatus(
            E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    ObserverRuntimeOutcome earlyOutcome{
        {
            durationMs,
            0,
            0,
            0,
            {0, 0, 0, 0, 0},
            RuntimeSuccessStatus(),
            RuntimeSuccessStatus(),
            {},
        },
        {},
        {RuntimeSuccessStatus(), std::nullopt, false, false},
    };
    const auto failHandleCreation = [&](const DWORD error) {
        const Status failure = RuntimeFailureStatus(
            HRESULT_FROM_WIN32(error), error);
        static_cast<void>(earlyOutcome.failures.RecordRuntimeFailure({
            ObserverFailureOrigin::Transport,
            failure,
        }));
        earlyOutcome.snapshot.runtime_status = failure;
        *output = std::move(earlyOutcome);
        return failure;
    };
    UniqueHandle wakeEvent(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!wakeEvent) {
        return failHandleCreation(GetLastError());
    }
    UniqueHandle responseEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!responseEvent) {
        return failHandleCreation(GetLastError());
    }
    const DWORD staThread = GetCurrentThreadId();
    ObserverCallbackQueue queue(
        wakeEvent.get(),
        {
            [](std::deque<ObserverCallbackSlot>* slots,
               const std::uint64_t sequence) {
                slots->push_back({
                    sequence,
                    ObserverCallbackSlotState::Pending,
                    std::nullopt,
                    std::nullopt,
                });
            },
            [](const HANDLE event) { return SetEvent(event); },
            [staThread]() {
                return PostThreadMessageW(
                    staThread, WM_APP + 0x51, 0, 0);
            },
            []() { return GetLastError(); },
        });
    ProductionObserverRuntime runtime(&queue, responseEvent.get());
    const ObserverCoordinatorOperations operations{
        [&runtime]() { return runtime.Startup(); },
        [&runtime](const ObserverCallbackEnvelope& envelope) {
            return runtime.ProcessEvent(envelope);
        },
        [&runtime]() { return runtime.PumpMessages(); },
        [&runtime](std::size_t, const std::uint64_t rawSequence) {
            return runtime.ProcessResponse(rawSequence);
        },
        [&runtime]() { return runtime.BeginStop(); },
        [&runtime]() { return runtime.Cleanup(); },
        [](const EventObservationSnapshot& snapshot, bool* const passed) {
            return EvaluateEventObservationGate(snapshot, passed);
        },
        {
            []() { return ObserverDeadline::Clock::now(); },
            [](const DWORD count,
               const HANDLE* handles,
               const DWORD timeout,
               const DWORD wakeMask,
               const DWORD flags) {
                return MsgWaitForMultipleObjectsEx(
                    count, handles, timeout, wakeMask, flags);
            },
            []() { return GetLastError(); },
        },
    };
    const Status coordinated = RunObserverCoordinator(
        durationMs,
        5000,
        &queue,
        responseEvent.get(),
        operations,
        output);
    output->snapshot.runtime_stage = queue.has_callback_failure()
        ? "callback.failure"
        : runtime.runtime_stage();
    return coordinated;
}

}  // namespace winexinfo
