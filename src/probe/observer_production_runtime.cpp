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

bool IsPendingContractResult(
    const ObserverOperationResult& operation) noexcept {
    return !operation.ok() &&
        operation.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        operation.status.hresult == S_FALSE &&
        operation.status.win32 == ERROR_SUCCESS &&
        operation.failure_origin ==
            std::optional{ObserverFailureOrigin::Contract};
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
        ObserverOperationResult initialized = InitializeCurrentTabState();
        if (!initialized.ok()) {
            return FailAfterShellStartup(initialized);
        }

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
                try {
                    if (!initial_shell_entries_
                             .emplace(entry.canonical_identity, entry)
                             .second) {
                        return FailAfterShellStartup(
                            RuntimeContractFailure());
                    }
                } catch (const std::bad_alloc&) {
                    return FailAfterShellStartup(
                        RuntimeTransportFailure(E_OUTOFMEMORY));
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
        ObserverCurrentReconcileOutcome currentReconciliation{};
        runtime_stage_ = "event.reconcile_current_tabs";
        ObserverOperationResult currentReconciled = RefreshCurrentTabState(
            envelope.payload.kind,
            envelope.payload.source == ObserverCallbackSource::ShellLifecycle
                ? envelope.payload.shell_cookie
                : 0,
            &currentReconciliation);
        if (!currentReconciled.ok()) {
            if (IsIgnorableObserverTabRefreshResult(currentReconciled)) {
                return {
                    RuntimeSuccessOperation(),
                    ObserverEventDisposition::Ignored,
                    envelope.sequence,
                    std::nullopt,
                };
            }
            return FailureResult(envelope.sequence, currentReconciled);
        }
        if (envelope.payload.source == ObserverCallbackSource::ShellLifecycle) {
            runtime_stage_ = "lifecycle.reconciled";
            const ObserverTabIdentity* representative = nullptr;
            if (!currentReconciliation.tabs.added.empty()) {
                representative = &currentReconciliation.tabs.added.front();
            } else if (!currentReconciliation.tabs.removed.empty()) {
                representative = &currentReconciliation.tabs.removed.front();
            } else if (!currentReconciliation.tabs.current.empty()) {
                representative = &currentReconciliation.tabs.current.front();
            }
            if (representative == nullptr) {
                return {
                    RuntimeSuccessOperation(),
                    ObserverEventDisposition::Ignored,
                    envelope.sequence,
                    std::nullopt,
                };
            }
            ObservedEventRecord record{
                next_event_sequence_++,
                representative->top_level_generation,
                envelope.payload.kind,
                ObservedEventTransition::Reconciled,
                representative->top_level,
                true,
                representative->shell_tab,
                false,
                true,
                envelope.payload.shell_cookie,
                ObservedStructureChangeType::None,
                nullptr,
                nullptr,
                0,
                false,
                {},
                false,
                {},
                RuntimeSuccessStatus(),
            };
            AttachReconciliation(currentReconciliation.tabs, &record);
            return {
                RuntimeSuccessOperation(),
                ObserverEventDisposition::Completed,
                envelope.sequence,
                std::move(record),
            };
        }
        if (envelope.payload.top_level == nullptr) {
            runtime_stage_ = "event.invalid_target";
            return FailureResult(
                envelope.sequence,
                RuntimeContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER));
        }
        if (IsWindow(envelope.payload.top_level) == FALSE) {
            runtime_stage_ = "event.stale_target";
            return {
                RuntimeSuccessOperation(),
                ObserverEventDisposition::Ignored,
                envelope.sequence,
                std::nullopt,
            };
        }
        if (envelope.payload.source == ObserverCallbackSource::BrowserNavigate) {
            Win32ClassTree tree{};
            const Status capturedTree = CaptureWin32ClassTree(
                envelope.payload.top_level, &tree);
            if (!capturedTree.ok() &&
                (FAILED(capturedTree.hresult) ||
                 capturedTree.win32 != ERROR_SUCCESS)) {
                return FailureResult(
                    envelope.sequence,
                    RuntimeOperationFromStatus(capturedTree));
            }
            Win32ContractResult win32{};
            bool exactWin32 = false;
            if (capturedTree.ok()) {
                win32 = ValidateWin32Contract(tree);
                if (!win32.status.ok() &&
                    (FAILED(win32.status.hresult) ||
                     win32.status.win32 != ERROR_SUCCESS)) {
                    return FailureResult(
                        envelope.sequence,
                        RuntimeOperationFromStatus(win32.status));
                }
                exactWin32 = win32.status.ok();
            }
            if (exactWin32 &&
                envelope.payload.shell_tab != win32.active_shell_tab) {
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
        ObserverOperationResult refreshed = RuntimeSuccessOperation();
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
            logical.status.ok() &&
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
            const bool pendingTarget =
                IsPendingContractResult(response.operation);
            if (!consumed.ok() ||
                (!response.operation.ok() && !pendingTarget)) {
                return FailureResult(
                    envelope.sequence,
                    consumed.ok() ? response.operation : consumed);
            }
            if (response.operation.ok()) {
                uia_targets_.push_back(target);
            }
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
        record.sequence = next_event_sequence_++;
        AttachReconciliation(currentReconciliation.tabs, &record);
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
        runtime_stage_ = "response.reconcile_current_tabs";
        ObserverCurrentReconcileOutcome currentReconciliation{};
        ObserverOperationResult refreshed = RefreshCurrentTabState(
            ObservedEventKind::TabStructureChanged,
            0,
            &currentReconciliation);
        if (!refreshed.ok()) {
            return FailureResult(rawSequence, refreshed);
        }
        const std::size_t shellEntryCount = static_cast<std::size_t>(
            std::count_if(
                shell_graph_.captured_browsers.begin(),
                shell_graph_.captured_browsers.end(),
                [&](const ObserverShellStaCapturedBrowser& browser) {
                    return browser.metadata.target_matched &&
                        browser.metadata.top_level == pending_top_level_;
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
        record.sequence = next_event_sequence_++;
        AttachReconciliation(currentReconciliation.tabs, &record);
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
    static void AttachReconciliation(
        const ObserverTabSetReconciliation& reconciliation,
        ObservedEventRecord* const record) {
        record->previous_tab_count =
            reconciliation.retained.size() + reconciliation.removed.size();
        record->current_tab_count = reconciliation.current.size();
        record->added_tab_count = reconciliation.added.size();
        record->removed_tab_count = reconciliation.removed.size();
        record->retained_tab_count = reconciliation.retained.size();
        const auto active = reconciliation.active_shell_tabs.find(
            record->source_top_level);
        if (active != reconciliation.active_shell_tabs.end()) {
            record->reconciled_active_shell_tab = active->second;
        } else if (!reconciliation.active_shell_tabs.empty()) {
            record->reconciled_active_shell_tab =
                reconciliation.active_shell_tabs.begin()->second;
        } else {
            record->reconciled_active_shell_tab = nullptr;
        }
    }

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

    ObserverOperationResult CaptureCurrentTabOrders(
        const std::vector<HWND>& topLevels,
        std::vector<ObserverTopLevelTabOrder>* const output) {
        if (output == nullptr || !output->empty()) {
            return RuntimeContractFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        try {
            output->reserve(topLevels.size());
            for (const HWND topLevel : topLevels) {
                Win32ClassTree tree{};
                const Status captured = CaptureWin32ClassTree(topLevel, &tree);
                if (!captured.ok()) {
                    return RuntimeOperationFromStatus(captured);
                }
                const Win32ContractResult win32 = ValidateWin32Contract(tree);
                if (!win32.status.ok()) {
                    return RuntimeOperationFromStatus(win32.status);
                }
                ObserverTopLevelTabOrder order{topLevel, {}};
                order.tabs.reserve(win32.ordered_shell_tabs.size());
                for (const HWND shellTab : win32.ordered_shell_tabs) {
                    const auto node = std::find_if(
                        tree.nodes.begin(),
                        tree.nodes.end(),
                        [shellTab](const Win32ClassNode& candidate) {
                            return candidate.hwnd == shellTab;
                        });
                    if (node == tree.nodes.end() ||
                        node->parent != topLevel ||
                        node->class_name != L"ShellTabWindowClass") {
                        return RuntimeContractFailure();
                    }
                    order.tabs.push_back({shellTab, node->visible});
                }
                output->push_back(std::move(order));
            }
        } catch (const std::bad_alloc&) {
            return RuntimeTransportFailure(E_OUTOFMEMORY);
        }
        return RuntimeSuccessOperation();
    }

    ObserverOperationResult InitializeCurrentTabState() {
        std::vector<ObserverTopLevelTabOrder> orders;
        ObserverOperationResult capturedOrders = CaptureCurrentTabOrders(
            shell_graph_.target_top_levels, &orders);
        if (!capturedOrders.ok()) {
            return capturedOrders;
        }
        ObserverTabSetReconciliation reconciliation{};
        const Status reconciled = ReconcileObserverTabSet(
            {},
            current_shell_metadata_,
            orders,
            {},
            &reconciliation);
        if (!reconciled.ok()) {
            return RuntimeOperationFromStatus(reconciled);
        }
        try {
            current_tab_state_.tabs = reconciliation.current;
            current_tab_state_.generations = reconciliation.generations;
            current_tab_state_.subscriptions.next_registration_id =
                shell_graph_.next_registration_id;
            current_tab_state_.subscriptions.subscriptions.reserve(
                shell_graph_.browser_resources.size());
            for (ObserverShellStaBrowserResource& resource :
                 shell_graph_.browser_resources) {
                const auto identity = std::find_if(
                    current_tab_state_.tabs.begin(),
                    current_tab_state_.tabs.end(),
                    [&](const ObserverTabIdentity& candidate) {
                        return candidate.canonical_identity ==
                            resource.canonical_identity;
                    });
                if (identity == current_tab_state_.tabs.end() ||
                    resource.registration_id == 0 ||
                    resource.canonical_source == nullptr) {
                    return RuntimeContractFailure();
                }
                resource.identity = *identity;
                current_tab_state_.subscriptions.subscriptions.push_back({
                    *identity,
                    resource.registration_id,
                });
            }
            if (current_tab_state_.subscriptions.subscriptions.size() !=
                current_tab_state_.tabs.size()) {
                return RuntimeContractFailure();
            }
        } catch (const std::bad_alloc&) {
            return RuntimeTransportFailure(E_OUTOFMEMORY);
        }
        return RuntimeSuccessOperation();
    }

    ObserverOperationResult RefreshCurrentTabState(
        const ObservedEventKind wakeKind,
        const LONG lifecycleCookie,
        ObserverCurrentReconcileOutcome* const output) {
        if (output == nullptr) {
            return RuntimeContractFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        std::vector<ExplorerWindowRecord> windows;
        ObserverOperationResult enumerated =
            shell_operations_.enumerate_windows(&windows);
        if (!enumerated.ok()) {
            return enumerated;
        }
        std::vector<HWND> targets;
        try {
            targets.reserve(windows.size());
            for (const ExplorerWindowRecord& window : windows) {
                if (window.hwnd == nullptr || window.process_id == 0 ||
                    window.thread_id == 0 ||
                    std::find(targets.begin(), targets.end(), window.hwnd) !=
                        targets.end()) {
                    return RuntimeContractFailure();
                }
                targets.push_back(window.hwnd);
            }
        } catch (const std::bad_alloc&) {
            return RuntimeTransportFailure(E_OUTOFMEMORY);
        }

        std::vector<ObserverTopLevelTabOrder> orders;
        ObserverOperationResult capturedOrders =
            CaptureCurrentTabOrders(targets, &orders);
        if (!capturedOrders.ok()) {
            return capturedOrders;
        }

        ObserverShellStaCapture refreshedCapture{};
        if (!targets.empty()) {
            ObserverOperationResult captured = shell_operations_.capture(
                shell_graph_.shell_windows.Get(), targets, &refreshedCapture);
            if (!captured.ok()) {
                return captured;
            }
        }
        std::vector<ObserverShellEntryMetadata> metadata;
        try {
            metadata.reserve(refreshedCapture.browsers.size());
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
                    return RuntimeContractFailure();
                }
                metadata.push_back(browser.metadata);
            }
            shell_graph_.browser_resources.reserve(
                shell_graph_.browser_resources.size() + metadata.size());
            shell_graph_.startup_state.browser_registrations.reserve(
                shell_graph_.startup_state.browser_registrations.size() +
                metadata.size());
        } catch (const std::bad_alloc&) {
            return RuntimeTransportFailure(E_OUTOFMEMORY);
        }

        const auto findSource = [&](const ObserverTabIdentity& identity)
            -> const ObserverShellStaCapturedBrowser* {
            const auto findIn = [&](const auto& browsers)
                -> const ObserverShellStaCapturedBrowser* {
                const auto found = std::find_if(
                    browsers.begin(),
                    browsers.end(),
                    [&](const ObserverShellStaCapturedBrowser& browser) {
                        return browser.metadata.canonical_identity ==
                            identity.canonical_identity &&
                            browser.metadata.top_level == identity.top_level &&
                            browser.metadata.shell_tab == identity.shell_tab;
                    });
                return found == browsers.end() ? nullptr : &*found;
            };
            const ObserverShellStaCapturedBrowser* source =
                findIn(refreshedCapture.browsers);
            return source != nullptr
                ? source
                : findIn(shell_graph_.captured_browsers);
        };

        const ObserverCurrentReconcileOperations operations{
            [&](ObserverCurrentTabCapture* const capture) {
                if (capture == nullptr || !capture->shell_entries.empty() ||
                    !capture->orders.empty()) {
                    return RuntimeContractFailure(
                        E_INVALIDARG, ERROR_INVALID_PARAMETER);
                }
                try {
                    capture->shell_entries = metadata;
                    capture->orders = orders;
                } catch (const std::bad_alloc&) {
                    return RuntimeTransportFailure(E_OUTOFMEMORY);
                }
                return RuntimeSuccessOperation();
            },
            {
                [&](const ObserverTabIdentity& identity,
                    const std::uint64_t registrationId) {
                    const ObserverShellStaCapturedBrowser* source =
                        findSource(identity);
                    if (source == nullptr || source->canonical_identity == nullptr ||
                        source->browser == nullptr || registrationId == 0) {
                        return RuntimeContractFailure();
                    }
                    Microsoft::WRL::ComPtr<IDispatch> sink;
                    ObserverOperationResult created =
                        shell_operations_.create_browser_sink(
                            queue_,
                            source->canonical_identity.Get(),
                            identity.top_level,
                            identity.top_level_generation,
                            identity.shell_tab,
                            sink);
                    if (!created.ok() || sink == nullptr) {
                        return created.ok()
                            ? RuntimeContractFailure()
                            : created;
                    }
                    ObserverShellStaBrowserResource resource{
                        registrationId,
                        identity.canonical_identity,
                        source->browser,
                        {},
                        identity,
                        source->canonical_identity,
                    };
                    ObserverOperationResult advised = shell_operations_.advise(
                        resource.browser.Get(),
                        DIID_DWebBrowserEvents2,
                        sink.Get(),
                        &resource.connection);
                    if (!advised.ok()) {
                        return advised;
                    }
                    if (resource.connection.connection_point == nullptr ||
                        resource.connection.sink == nullptr ||
                        resource.connection.subscription_cookie == 0 ||
                        resource.connection.owner_thread_id !=
                            shell_graph_.owner_thread_id) {
                        return RuntimeContractFailure();
                    }
                    shell_graph_.browser_resources.push_back(
                        std::move(resource));
                    shell_graph_.startup_state.browser_registrations.push_back({
                        identity.canonical_identity,
                        registrationId,
                    });
                    return RuntimeSuccessOperation();
                },
                [&](const std::uint64_t registrationId) {
                    const auto resource = std::find_if(
                        shell_graph_.browser_resources.begin(),
                        shell_graph_.browser_resources.end(),
                        [registrationId](
                            const ObserverShellStaBrowserResource& candidate) {
                            return candidate.registration_id == registrationId;
                        });
                    const auto registration = std::find_if(
                        shell_graph_.startup_state.browser_registrations.begin(),
                        shell_graph_.startup_state.browser_registrations.end(),
                        [registrationId](
                            const ObserverShellBrowserRegistration& candidate) {
                            return candidate.registration_id == registrationId;
                        });
                    if (resource == shell_graph_.browser_resources.end() ||
                        registration ==
                            shell_graph_.startup_state.browser_registrations.end()) {
                        return RuntimeContractFailure();
                    }
                    ObserverOperationResult unadvised =
                        shell_operations_.unadvise(&resource->connection);
                    if (!unadvised.ok()) {
                        return unadvised;
                    }
                    shell_graph_.browser_resources.erase(resource);
                    shell_graph_.startup_state.browser_registrations.erase(
                        registration);
                    return RuntimeSuccessOperation();
                },
            },
        };
        const Status reconciled = ReconcileObserverCurrentTabState(
            wakeKind,
            lifecycleCookie,
            operations,
            &current_tab_state_,
            output);
        if (!reconciled.ok()) {
            return RuntimeOperationFromStatus(reconciled);
        }
        shell_graph_.target_top_levels = std::move(targets);
        shell_graph_.browser_set = std::move(refreshedCapture.browser_set);
        shell_graph_.captured_browsers = std::move(refreshedCapture.browsers);
        shell_graph_.next_registration_id =
            current_tab_state_.subscriptions.next_registration_id;
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
        Win32ContractResult win32{};
        Status win32Status = treeStatus;
        if (treeStatus.ok()) {
            win32 = ValidateWin32Contract(tree);
            win32Status = win32.status;
        }
        if (!win32Status.ok()) {
            if (FAILED(win32Status.hresult) ||
                win32Status.win32 != ERROR_SUCCESS) {
                return RuntimeOperationFromStatus(win32Status);
            }
            *output = {
                nullptr,
                0,
                false,
                {},
                {
                    ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                    win32Status.hresult,
                    win32Status.win32,
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
    ObserverCurrentTabState current_tab_state_{};
    std::map<HWND, std::uint64_t> active_generations_;
    std::map<HWND, std::uint64_t> latest_generations_;
    std::map<std::uintptr_t, ObserverShellEntryMetadata>
        initial_shell_entries_;
    std::vector<ObserverUiaTarget> uia_targets_;
    std::optional<std::uint64_t> pending_command_id_;
    std::uint64_t pending_raw_sequence_ = 0;
    std::uint64_t next_event_sequence_ = 1;
    HWND pending_top_level_ = nullptr;
    bool cleanup_complete_ = false;
    const char* runtime_stage_ = "created";
};

}  // namespace

bool IsIgnorableObserverTabRefreshResult(
    const ObserverOperationResult& result) noexcept {
    return result.status.code ==
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        result.status.hresult == S_FALSE &&
        result.status.win32 == ERROR_INVALID_DATA &&
        result.failure_origin ==
            std::optional{ObserverFailureOrigin::Transport};
}

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
    bool hasTabAddition = false;
    bool hasTabRemoval = false;
    bool hasSelectionRemap = false;
    bool hasLifecycleRemoval = false;
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
            case ObservedEventTransition::Reconciled:
                transitionShapeValid =
                    (event.kind == ObservedEventKind::WindowRegistered ||
                     event.kind == ObservedEventKind::WindowRevoked) &&
                    event.current_active_view == nullptr &&
                    event.active_view_count == 0 &&
                    !event.current_filesystem_path_available;
                break;
        }
        cleanEvents = cleanEvents && event.status.ok() &&
            transitionShapeValid;
        const bool reconciliationCountsValid =
            event.removed_tab_count <= event.previous_tab_count &&
            event.added_tab_count <= event.current_tab_count &&
            event.retained_tab_count ==
                event.previous_tab_count - event.removed_tab_count &&
            event.retained_tab_count ==
                event.current_tab_count - event.added_tab_count &&
            ((event.current_tab_count == 0) ==
             (event.reconciled_active_shell_tab == nullptr));
        cleanEvents = cleanEvents && reconciliationCountsValid;
        hasTabAddition = hasTabAddition || event.added_tab_count > 0;
        hasTabRemoval = hasTabRemoval || event.removed_tab_count > 0;
        hasSelectionRemap = hasSelectionRemap ||
            (event.kind == ObservedEventKind::TabSelected &&
             event.transition == ObservedEventTransition::Remapped &&
             event.active_view_count == 1);
        hasLifecycleRemoval = hasLifecycleRemoval ||
            (event.kind == ObservedEventKind::WindowRevoked &&
             event.removed_tab_count > 0);
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
        cleanEvents && pending.empty() && hasTabAddition && hasTabRemoval &&
        hasSelectionRemap && hasLifecycleRemoval && navigatePathTransition &&
        counts.window_registered > 0 &&
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
