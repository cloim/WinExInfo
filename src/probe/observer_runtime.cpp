#include "probe/observer_runtime.h"

#include <ExDisp.h>
#include <ExDispid.h>
#include <olectl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <new>
#include <utility>

namespace winexinfo {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ContractFailure(
    const HRESULT hresult = S_FALSE,
    const DWORD win32 = ERROR_SUCCESS) noexcept {
    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, win32};
}

Status TransportFailure(const HRESULT hresult) noexcept {
    const DWORD win32 = HRESULT_FACILITY(hresult) == FACILITY_WIN32
        ? static_cast<DWORD>(HRESULT_CODE(hresult))
        : ERROR_SUCCESS;
    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, win32};
}

ObserverOperationResult OperationSuccess() noexcept {
    return {Success(), std::nullopt};
}

ObserverOperationResult OperationFailure(
    const ObserverFailureOrigin origin,
    const Status status) noexcept {
    return {status, origin};
}

ObserverOperationResult ContractOperationFailure(
    const HRESULT hresult = S_FALSE,
    const DWORD win32 = ERROR_SUCCESS) noexcept {
    return OperationFailure(
        ObserverFailureOrigin::Contract,
        ContractFailure(hresult, win32));
}

ObserverOperationResult TransportOperationFailure(
    const HRESULT hresult) noexcept {
    return OperationFailure(
        ObserverFailureOrigin::Transport,
        TransportFailure(hresult));
}

ObserverFailure ToFailure(const ObserverOperationResult& result) noexcept {
    return {*result.failure_origin, result.status};
}

bool IsCoherentOperationResult(
    const ObserverOperationResult& result) noexcept {
    const bool exactSuccess = result.status.code == ErrorCode::OK &&
        result.status.hresult == S_OK &&
        result.status.win32 == ERROR_SUCCESS &&
        !result.failure_origin.has_value();
    const bool validOrigin = result.failure_origin.has_value() &&
        (*result.failure_origin == ObserverFailureOrigin::Contract ||
         *result.failure_origin == ObserverFailureOrigin::Transport);
    const bool exactFailure =
        result.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (result.status.hresult != S_OK ||
         result.status.win32 != ERROR_SUCCESS) &&
        validOrigin;
    return exactSuccess || exactFailure;
}

enum class ObserverDispatchSinkKind {
    ShellLifecycle,
    BrowserNavigate,
};

class ObserverDispatchSink final : public IDispatch {
public:
    ObserverDispatchSink(
        ObserverCallbackQueue* const queue,
        const ObserverDispatchSinkKind kind,
        REFIID eventInterface,
        IUnknown* const canonicalSource,
        const HWND topLevel,
        const std::uint64_t generation,
        const HWND shellTab)
        : queue_(queue),
          kind_(kind),
          event_interface_(eventInterface),
          canonical_source_(canonicalSource),
          top_level_(topLevel),
          generation_(generation),
          shell_tab_(shellTab) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** const output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IDispatch &&
            iid != event_interface_) {
            return E_NOINTERFACE;
        }
        *output = static_cast<IDispatch*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return references_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining =
            references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* const count) override {
        if (count == nullptr) {
            return E_POINTER;
        }
        *count = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(
        UINT,
        LCID,
        ITypeInfo**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(
        REFIID,
        LPOLESTR*,
        UINT,
        LCID,
        DISPID*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        const DISPID dispatchId,
        REFIID invokeIid,
        LCID,
        const WORD flags,
        DISPPARAMS* const parameters,
        VARIANT*,
        EXCEPINFO*,
        UINT* const argumentError) override {
        ObserverCallbackTicket ticket{};
        const Status admitted = queue_->Admit(&ticket);
        if (!admitted.ok() || !ticket.admitted) {
            return S_OK;
        }

        bool accepted = false;
        ObservedEventKind eventKind = ObservedEventKind::WindowRegistered;
        if (kind_ == ObserverDispatchSinkKind::ShellLifecycle) {
            LONG cookie = 0;
            const Status classified = ClassifyShellWindowsEvent(
                dispatchId,
                invokeIid,
                flags,
                parameters,
                &accepted,
                &eventKind,
                &cookie,
                argumentError);
            if (!classified.ok()) {
                (void)queue_->CompleteFailure(
                    ticket,
                    {ObserverFailureOrigin::Contract, classified});
                return classified.hresult;
            }
            if (!accepted) {
                (void)queue_->CompleteIgnored(ticket);
                return S_OK;
            }
            (void)queue_->Complete(
                ticket,
                {
                    ObserverCallbackSource::ShellLifecycle,
                    eventKind,
                    cookie,
                    nullptr,
                    0,
                    nullptr,
                    ObservedStructureChangeType::None,
                });
            return S_OK;
        }

        const Status classified = ClassifyBrowserEvent(
            dispatchId,
            invokeIid,
            flags,
            parameters,
            &accepted,
            &eventKind,
            argumentError);
        if (!classified.ok()) {
            (void)queue_->CompleteFailure(
                ticket,
                {ObserverFailureOrigin::Contract, classified});
            return classified.hresult;
        }
        if (!accepted) {
            (void)queue_->CompleteIgnored(ticket);
            return S_OK;
        }

        Microsoft::WRL::ComPtr<IUnknown> callbackSource;
        const HRESULT queryResult = parameters->rgvarg[1].pdispVal->QueryInterface(
            IID_PPV_ARGS(callbackSource.ReleaseAndGetAddressOf()));
        const ObserverOperationResult queryClassified =
            ClassifyObserverConnectionPointResult(
                queryResult,
                callbackSource != nullptr);
        if (!queryClassified.ok()) {
            (void)queue_->CompleteFailure(ticket, ToFailure(queryClassified));
            return S_OK;
        }
        if (callbackSource.Get() != canonical_source_.Get()) {
            const Status mismatch = ContractFailure();
            (void)queue_->CompleteFailure(
                ticket,
                {ObserverFailureOrigin::Contract, mismatch});
            return S_OK;
        }
        (void)queue_->Complete(
            ticket,
            {
                ObserverCallbackSource::BrowserNavigate,
                eventKind,
                0,
                top_level_,
                generation_,
                shell_tab_,
                ObservedStructureChangeType::None,
            });
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    ObserverCallbackQueue* queue_;
    ObserverDispatchSinkKind kind_;
    IID event_interface_;
    Microsoft::WRL::ComPtr<IUnknown> canonical_source_;
    HWND top_level_;
    std::uint64_t generation_;
    HWND shell_tab_;
};

}  // namespace

ObserverOperationResult ClassifyObserverWin32Failure(
    const DWORD lastError) noexcept {
    if (lastError == ERROR_SUCCESS) {
        return ContractOperationFailure();
    }
    return OperationFailure(
        ObserverFailureOrigin::Transport,
        {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            HRESULT_FROM_WIN32(lastError),
            lastError,
        });
}

Status ClassifyObserverShellSetTransition(
    const ObserverShellTransitionKind kind,
    const std::span<const ObserverShellEntryMetadata> previous,
    const std::span<const ObserverShellEntryMetadata> current,
    const std::uintptr_t resolvedAddedIdentity,
    ObserverShellSetTransition* const output) {
    const bool validKind = kind == ObserverShellTransitionKind::Stable ||
        kind == ObserverShellTransitionKind::Registered ||
        kind == ObserverShellTransitionKind::Revoked;
    if (!validKind || output == nullptr ||
        (kind == ObserverShellTransitionKind::Registered &&
         resolvedAddedIdentity == 0) ||
        (kind != ObserverShellTransitionKind::Registered &&
         resolvedAddedIdentity != 0)) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }

    const std::array<std::span<const ObserverShellEntryMetadata>, 2> sets{
        previous,
        current,
    };
    for (const auto set : sets) {
        for (std::size_t index = 0; index < set.size(); ++index) {
            const ObserverShellEntryMetadata& entry = set[index];
            if (entry.canonical_identity == 0 || entry.top_level == nullptr ||
                entry.target_matched != (entry.shell_tab != nullptr)) {
                return ContractFailure();
            }
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (set[prior].canonical_identity == entry.canonical_identity ||
                    (entry.target_matched && set[prior].target_matched &&
                     set[prior].top_level == entry.top_level &&
                     set[prior].shell_tab == entry.shell_tab)) {
                    return ContractFailure();
                }
            }
        }
    }

    ObserverShellSetTransition candidate{};
    for (const ObserverShellEntryMetadata& oldEntry : previous) {
        const auto match = std::find_if(
            current.begin(),
            current.end(),
            [&oldEntry](const ObserverShellEntryMetadata& entry) {
                return entry.canonical_identity == oldEntry.canonical_identity;
            });
        if (match == current.end()) {
            if (candidate.removed.has_value()) {
                return ContractFailure();
            }
            candidate.removed = oldEntry;
        } else if (*match != oldEntry) {
            return ContractFailure();
        }
    }
    for (const ObserverShellEntryMetadata& newEntry : current) {
        const auto match = std::find_if(
            previous.begin(),
            previous.end(),
            [&newEntry](const ObserverShellEntryMetadata& entry) {
                return entry.canonical_identity == newEntry.canonical_identity;
            });
        if (match == previous.end()) {
            if (candidate.added.has_value()) {
                return ContractFailure();
            }
            candidate.added = newEntry;
        }
    }

    const bool exactStable = !candidate.added.has_value() &&
        !candidate.removed.has_value();
    const bool exactRegistered = candidate.added.has_value() &&
        !candidate.removed.has_value() &&
        candidate.added->canonical_identity == resolvedAddedIdentity;
    const bool exactRevoked = !candidate.added.has_value() &&
        candidate.removed.has_value();
    if ((kind == ObserverShellTransitionKind::Stable && !exactStable) ||
        (kind == ObserverShellTransitionKind::Registered && !exactRegistered) ||
        (kind == ObserverShellTransitionKind::Revoked && !exactRevoked)) {
        return ContractFailure();
    }
    *output = candidate;
    return Success();
}

ObserverOperationResult CreateShellLifecycleEventSink(
    ObserverCallbackQueue* const queue,
    Microsoft::WRL::ComPtr<IDispatch>& output) {
    if (queue == nullptr) {
        return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    if (output != nullptr) {
        return ContractOperationFailure();
    }
    auto* const sink = new (std::nothrow) ObserverDispatchSink(
        queue,
        ObserverDispatchSinkKind::ShellLifecycle,
        DIID_DShellWindowsEvents,
        nullptr,
        nullptr,
        0,
        nullptr);
    if (sink == nullptr) {
        return TransportOperationFailure(E_OUTOFMEMORY);
    }
    output.Attach(sink);
    return OperationSuccess();
}

ObserverOperationResult CreateBrowserNavigateEventSink(
    ObserverCallbackQueue* const queue,
    IUnknown* const canonicalSource,
    const HWND topLevel,
    const std::uint64_t generation,
    const HWND shellTab,
    Microsoft::WRL::ComPtr<IDispatch>& output) {
    if (queue == nullptr || canonicalSource == nullptr || topLevel == nullptr ||
        generation == 0 || shellTab == nullptr) {
        return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    if (output != nullptr) {
        return ContractOperationFailure();
    }
    auto* const sink = new (std::nothrow) ObserverDispatchSink(
        queue,
        ObserverDispatchSinkKind::BrowserNavigate,
        DIID_DWebBrowserEvents2,
        canonicalSource,
        topLevel,
        generation,
        shellTab);
    if (sink == nullptr) {
        return TransportOperationFailure(E_OUTOFMEMORY);
    }
    output.Attach(sink);
    return OperationSuccess();
}

ObserverShellCleanupOutcome CleanupObserverShellSubscriptions(
    const ObserverShellStartupOperations& operations,
    ObserverShellStartupState* const state) noexcept {
    if (state == nullptr) {
        return {
            ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER),
            ObserverFailureOrigin::Contract,
            false,
        };
    }
    bool hasActiveRegistration = state->lifecycle_registration_id != 0 ||
        !state->browser_registrations.empty();
    if (hasActiveRegistration && !operations.unadvise) {
        return {
            ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER),
            ObserverFailureOrigin::Contract,
            false,
        };
    }
    for (std::size_t index = 0;
         index < state->browser_registrations.size();
         ++index) {
        const ObserverShellBrowserRegistration& registration =
            state->browser_registrations[index];
        if (registration.canonical_identity == 0 ||
            registration.registration_id == 0 ||
            (state->lifecycle_registration_id != 0 &&
             registration.registration_id ==
                 state->lifecycle_registration_id)) {
            return {
                ContractFailure(),
                ObserverFailureOrigin::Contract,
                false,
            };
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (state->browser_registrations[prior].canonical_identity ==
                    registration.canonical_identity ||
                state->browser_registrations[prior].registration_id ==
                    registration.registration_id) {
                return {
                    ContractFailure(),
                    ObserverFailureOrigin::Contract,
                    false,
                };
            }
        }
    }

    ObserverFailureState failures;
    std::optional<ObserverFailureOrigin> firstOrigin;
    for (std::size_t remaining = state->browser_registrations.size();
         remaining > 0;
         --remaining) {
        const std::size_t index = remaining - 1;
        ObserverOperationResult result = OperationSuccess();
        try {
            result = operations.unadvise(
                state->browser_registrations[index].registration_id);
        } catch (const std::bad_alloc&) {
            result = TransportOperationFailure(E_OUTOFMEMORY);
        } catch (...) {
            result = TransportOperationFailure(E_FAIL);
        }
        if (!IsCoherentOperationResult(result)) {
            result = ContractOperationFailure(
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER);
        }
        if (result.ok()) {
            state->browser_registrations.erase(
                state->browser_registrations.begin() +
                static_cast<std::ptrdiff_t>(index));
        } else {
            if (!failures.has_runtime_failure()) {
                firstOrigin = result.failure_origin;
            }
            (void)failures.RecordRuntimeFailure(ToFailure(result));
        }
    }

    if (state->lifecycle_registration_id != 0) {
        ObserverOperationResult result = OperationSuccess();
        try {
            result = operations.unadvise(
                state->lifecycle_registration_id);
        } catch (const std::bad_alloc&) {
            result = TransportOperationFailure(E_OUTOFMEMORY);
        } catch (...) {
            result = TransportOperationFailure(E_FAIL);
        }
        if (!IsCoherentOperationResult(result)) {
            result = ContractOperationFailure(
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER);
        }
        if (result.ok()) {
            state->lifecycle_registration_id = 0;
        } else {
            if (!failures.has_runtime_failure()) {
                firstOrigin = result.failure_origin;
            }
            (void)failures.RecordRuntimeFailure(ToFailure(result));
        }
    }

    state->baseline.clear();
    if (!failures.has_runtime_failure()) {
        return {Success(), std::nullopt, false};
    }
    return {
        failures.runtime_status(),
        firstOrigin,
        failures.any_transport_failure(),
    };
}

Status StartObserverShellSubscriptions(
    ObserverCallbackQueue* const queue,
    const ObserverShellStartupOperations& operations,
    ObserverShellStartupState* const state,
    ObserverShellStartupOutcome* const output) noexcept {
    if (queue == nullptr || state == nullptr || output == nullptr ||
        state->lifecycle_registration_id != 0 ||
        !state->browser_registrations.empty() || !state->baseline.empty() ||
        !operations.advise_lifecycle || !operations.capture ||
        !operations.advise_browser || !operations.unadvise) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }

    const Status startingState = queue->ValidateStartingState();
    if (!startingState.ok()) {
        return startingState;
    }

    ObserverShellStartupOutcome candidate{
        OperationSuccess(),
        false,
        Success(),
        {Success(), std::nullopt, false},
    };
    const auto recordTransportStatus = [&](const Status& status) {
        if (!candidate.any_setup_transport_failure) {
            candidate.first_setup_transport_status = status;
        }
        candidate.any_setup_transport_failure = true;
    };
    const auto captureEmergency = [&]() {
        const auto emergency = queue->emergency();
        if (!emergency.has_value()) {
            return false;
        }
        if (emergency->any_transport_failure) {
            recordTransportStatus(emergency->first_transport_status);
        }
        candidate.setup = {
            emergency->first_failure.status,
            emergency->first_failure.origin,
        };
        return true;
    };
    const auto mergeOperationResult = [&](ObserverOperationResult result) {
        if (!IsCoherentOperationResult(result)) {
            result = ContractOperationFailure(
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER);
        }
        const bool emergencyPresent = captureEmergency();
        if (!emergencyPresent) {
            candidate.setup = result;
        }
        if (!result.ok() && result.failure_origin ==
                                ObserverFailureOrigin::Transport) {
            recordTransportStatus(result.status);
        }
        return result;
    };

    try {
        std::uint64_t lifecycleRegistration = 0;
        ObserverOperationResult operation =
            operations.advise_lifecycle(&lifecycleRegistration);
        operation = mergeOperationResult(operation);
        if (operation.ok()) {
            if (lifecycleRegistration == 0) {
                if (candidate.setup.ok()) {
                    candidate.setup = ContractOperationFailure();
                }
            } else {
                state->lifecycle_registration_id = lifecycleRegistration;
            }
        }

        std::vector<ObserverShellEntryMetadata> first;
        if (candidate.setup.ok()) {
            operation = operations.capture(&first);
            operation = mergeOperationResult(operation);
        }

        ObserverShellSetTransition validatedFirst{};
        bool hasTarget = false;
        if (candidate.setup.ok()) {
            const Status validation = ClassifyObserverShellSetTransition(
                ObserverShellTransitionKind::Stable,
                first,
                first,
                0,
                &validatedFirst);
            for (const ObserverShellEntryMetadata& entry : first) {
                hasTarget = hasTarget || entry.target_matched;
            }
            if (!validation.ok() || first.empty() || !hasTarget) {
                candidate.setup = ContractOperationFailure();
            }
            (void)captureEmergency();
        }

        if (candidate.setup.ok()) {
            std::size_t targetCount = 0;
            for (const ObserverShellEntryMetadata& entry : first) {
                if (entry.target_matched) {
                    ++targetCount;
                }
            }
            state->browser_registrations.reserve(targetCount);
            for (const ObserverShellEntryMetadata& entry : first) {
                if (!entry.target_matched) {
                    continue;
                }
                std::uint64_t browserRegistration = 0;
                operation = operations.advise_browser(
                    entry,
                    &browserRegistration);
                operation = mergeOperationResult(operation);
                if (operation.ok() && browserRegistration == 0) {
                    if (candidate.setup.ok()) {
                        candidate.setup = ContractOperationFailure();
                    }
                } else if (operation.ok()) {
                    bool duplicateRegistration =
                        browserRegistration ==
                        state->lifecycle_registration_id;
                    for (const ObserverShellBrowserRegistration& existing :
                         state->browser_registrations) {
                        duplicateRegistration = duplicateRegistration ||
                            existing.registration_id == browserRegistration;
                    }
                    if (duplicateRegistration) {
                        if (candidate.setup.ok()) {
                            candidate.setup = ContractOperationFailure();
                        }
                    } else {
                        state->browser_registrations.push_back(
                            {entry.canonical_identity, browserRegistration});
                    }
                }
                if (!candidate.setup.ok()) {
                    break;
                }
            }
        }

        std::vector<ObserverShellEntryMetadata> second;
        if (candidate.setup.ok()) {
            operation = operations.capture(&second);
            operation = mergeOperationResult(operation);
        }
        if (candidate.setup.ok()) {
            ObserverShellSetTransition stable{};
            const Status stability = ClassifyObserverShellSetTransition(
                ObserverShellTransitionKind::Stable,
                first,
                second,
                0,
                &stable);
            if (!stability.ok()) {
                candidate.setup = {
                    stability,
                    ObserverFailureOrigin::Contract,
                };
            } else {
                state->baseline = std::move(second);
            }
            (void)captureEmergency();
        }
    } catch (const std::bad_alloc&) {
        const ObserverOperationResult exceptionFailure =
            TransportOperationFailure(E_OUTOFMEMORY);
        const bool emergencyPresent = captureEmergency();
        if (!emergencyPresent) {
            candidate.setup = exceptionFailure;
        }
        recordTransportStatus(exceptionFailure.status);
    } catch (...) {
        const ObserverOperationResult exceptionFailure =
            TransportOperationFailure(E_FAIL);
        const bool emergencyPresent = captureEmergency();
        if (!emergencyPresent) {
            candidate.setup = exceptionFailure;
        }
        recordTransportStatus(exceptionFailure.status);
    }

    if (!candidate.setup.ok()) {
        (void)queue->BeginStopping();
        candidate.rollback =
            CleanupObserverShellSubscriptions(operations, state);
        *output = candidate;
        return candidate.setup.status;
    }
    *output = candidate;
    return Success();
}

ObserverCallbackQueue::ObserverCallbackQueue(
    const HANDLE wakeEvent,
    ObserverCallbackQueueOperations operations)
    : wake_event_(wakeEvent), operations_(std::move(operations)) {}

void ObserverCallbackQueue::RecordEmergencyLocked(
    const ObserverFailure& failure,
    const std::uint64_t fatalRawSequence) noexcept {
    if (!first_failure_.has_value()) {
        first_failure_ = failure;
        fatal_raw_sequence_ = fatalRawSequence;
    } else if (!fatal_raw_sequence_.has_value() ||
               fatalRawSequence < *fatal_raw_sequence_) {
        fatal_raw_sequence_ = fatalRawSequence;
    }
    if (phase_ == Phase::Starting || phase_ == Phase::Running) {
        phase_ = Phase::Failed;
    }
    if (failure.origin == ObserverFailureOrigin::Transport) {
        any_transport_failure_ = true;
        if (first_transport_status_.ok()) {
            first_transport_status_ = failure.status;
        }
    }
}

Status ObserverCallbackQueue::SignalWakeLocked(
    const std::uint64_t fatalRawSequence) noexcept {
    ObserverOperationResult primary = OperationSuccess();
    if (wake_event_ == nullptr || !operations_.set_event ||
        !operations_.get_last_error) {
        primary = ContractOperationFailure();
    } else {
        try {
            if (operations_.set_event(wake_event_) != FALSE) {
                return Success();
            }
            primary = ClassifyObserverWin32Failure(
                operations_.get_last_error());
        } catch (const std::bad_alloc&) {
            primary = TransportOperationFailure(E_OUTOFMEMORY);
        } catch (...) {
            primary = TransportOperationFailure(E_FAIL);
        }
    }
    RecordEmergencyLocked(ToFailure(primary), fatalRawSequence);

    ObserverOperationResult fallback = OperationSuccess();
    if (!operations_.post_emergency || !operations_.get_last_error) {
        fallback = ContractOperationFailure();
    } else {
        try {
            if (operations_.post_emergency() != FALSE) {
                return primary.status;
            }
            fallback = ClassifyObserverWin32Failure(
                operations_.get_last_error());
        } catch (const std::bad_alloc&) {
            fallback = TransportOperationFailure(E_OUTOFMEMORY);
        } catch (...) {
            fallback = TransportOperationFailure(E_FAIL);
        }
    }
    RecordEmergencyLocked(ToFailure(fallback), fatalRawSequence);
    return primary.status;
}

ObserverCallbackSlot* ObserverCallbackQueue::FindPendingSlotLocked(
    const ObserverCallbackTicket& ticket) noexcept {
    if (!ticket.admitted || ticket.sequence == 0) {
        return nullptr;
    }
    const auto iterator = std::find_if(
        queue_.begin(),
        queue_.end(),
        [&ticket](const ObserverCallbackSlot& slot) {
            return slot.sequence == ticket.sequence;
        });
    if (iterator == queue_.end() ||
        iterator->state != ObserverCallbackSlotState::Pending ||
        iterator->payload.has_value() || iterator->failure.has_value()) {
        return nullptr;
    }
    return &*iterator;
}

Status ObserverCallbackQueue::BeginRunning(
    const std::function<ObserverDeadline::Clock::time_point()>& now,
    ObserverDeadline::Clock::time_point* const readyTime) {
    std::scoped_lock lock{mutex_};
    if (phase_ != Phase::Starting || wake_event_ == nullptr || !now ||
        readyTime == nullptr || !operations_.reserve ||
        !operations_.set_event || !operations_.post_emergency ||
        !operations_.get_last_error || !queue_.empty() || in_flight_ != 0 ||
        first_failure_.has_value()) {
        return ContractFailure();
    }

    ObserverDeadline::Clock::time_point candidate{};
    try {
        candidate = now();
    } catch (const std::bad_alloc&) {
        const ObserverFailure failure{
            ObserverFailureOrigin::Transport,
            TransportFailure(E_OUTOFMEMORY),
        };
        RecordEmergencyLocked(failure, next_sequence_);
        (void)SignalWakeLocked(next_sequence_);
        return failure.status;
    } catch (...) {
        const ObserverFailure failure{
            ObserverFailureOrigin::Transport,
            TransportFailure(E_FAIL),
        };
        RecordEmergencyLocked(failure, next_sequence_);
        (void)SignalWakeLocked(next_sequence_);
        return failure.status;
    }
    phase_ = Phase::Running;
    *readyTime = candidate;
    return Success();
}

Status ObserverCallbackQueue::ValidateStartingState() const noexcept {
    std::scoped_lock lock{mutex_};
    if (phase_ != Phase::Starting || wake_event_ == nullptr ||
        !operations_.reserve || !operations_.set_event ||
        !operations_.post_emergency || !operations_.get_last_error ||
        !queue_.empty() || in_flight_ != 0 || first_failure_.has_value() ||
        fatal_raw_sequence_.has_value()) {
        return ContractFailure();
    }
    return Success();
}

Status ObserverCallbackQueue::BeginStopping() {
    std::scoped_lock lock{mutex_};
    if (phase_ == Phase::Stopping) {
        return ContractFailure();
    }
    phase_ = Phase::Stopping;
    return Success();
}

Status ObserverCallbackQueue::EnsureStoppingState() noexcept {
    std::scoped_lock lock{mutex_};
    phase_ = Phase::Stopping;
    return Success();
}

Status ObserverCallbackQueue::Admit(
    ObserverCallbackTicket* const output) noexcept {
    if (output == nullptr) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::scoped_lock lock{mutex_};
    if (phase_ == Phase::Starting) {
        const Status status = ContractFailure();
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Contract, status},
            next_sequence_);
        (void)SignalWakeLocked(next_sequence_);
        return status;
    }
    if (phase_ == Phase::Stopping || phase_ == Phase::Failed) {
        if (late_event_count_ == std::numeric_limits<std::size_t>::max()) {
            const Status status = ContractFailure();
            RecordEmergencyLocked(
                {ObserverFailureOrigin::Contract, status},
                fatal_raw_sequence_.value_or(next_sequence_));
            return status;
        }
        ++late_event_count_;
        *output = {0, false};
        return Success();
    }
    if (!operations_.reserve || next_sequence_ == 0 ||
        next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        const Status status = ContractFailure();
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Contract, status},
            next_sequence_);
        (void)SignalWakeLocked(next_sequence_);
        return status;
    }

    const std::uint64_t sequence = next_sequence_;
    const std::size_t previousSize = queue_.size();
    ObserverOperationResult failure = OperationSuccess();
    try {
        operations_.reserve(&queue_, sequence);
    } catch (const std::bad_alloc&) {
        failure = TransportOperationFailure(E_OUTOFMEMORY);
    } catch (...) {
        failure = TransportOperationFailure(E_FAIL);
    }
    const bool exactReservation = failure.ok() &&
        queue_.size() == previousSize + 1 &&
        queue_.back().sequence == sequence &&
        queue_.back().state == ObserverCallbackSlotState::Pending &&
        !queue_.back().payload.has_value() &&
        !queue_.back().failure.has_value();
    if (!exactReservation) {
        while (queue_.size() > previousSize) {
            queue_.pop_back();
        }
        if (failure.ok()) {
            failure = ContractOperationFailure();
        }
        RecordEmergencyLocked(ToFailure(failure), sequence);
        (void)SignalWakeLocked(sequence);
        return failure.status;
    }

    ++next_sequence_;
    ++in_flight_;
    *output = {sequence, true};
    return Success();
}

Status ObserverCallbackQueue::Complete(
    const ObserverCallbackTicket& ticket,
    ObserverCallbackPayload payload) noexcept {
    std::scoped_lock lock{mutex_};
    ObserverCallbackSlot* const slot = FindPendingSlotLocked(ticket);
    if (slot == nullptr || in_flight_ == 0) {
        const Status status = ContractFailure();
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Contract, status},
            ticket.sequence == 0 ? next_sequence_ : ticket.sequence);
        (void)SignalWakeLocked(fatal_raw_sequence_.value_or(next_sequence_));
        return status;
    }
    slot->payload = std::move(payload);
    slot->state = ObserverCallbackSlotState::Payload;
    --in_flight_;
    return SignalWakeLocked(fatal_raw_sequence_.value_or(next_sequence_));
}

Status ObserverCallbackQueue::CompleteFailure(
    const ObserverCallbackTicket& ticket,
    const ObserverFailure& failure) noexcept {
    std::scoped_lock lock{mutex_};
    ObserverCallbackSlot* const slot = FindPendingSlotLocked(ticket);
    if (slot == nullptr || in_flight_ == 0) {
        const Status status = ContractFailure();
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Contract, status},
            ticket.sequence == 0 ? next_sequence_ : ticket.sequence);
        (void)SignalWakeLocked(fatal_raw_sequence_.value_or(next_sequence_));
        return status;
    }
    const bool validOrigin =
        failure.origin == ObserverFailureOrigin::Contract ||
        failure.origin == ObserverFailureOrigin::Transport;
    const bool exactActiveFailure =
        failure.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (failure.status.hresult != S_OK ||
         failure.status.win32 != ERROR_SUCCESS);
    const bool validFailure = validOrigin && exactActiveFailure;
    const ObserverFailure terminalFailure = validFailure
        ? failure
        : ObserverFailure{
              ObserverFailureOrigin::Contract,
              ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER),
          };
    slot->failure = terminalFailure;
    slot->state = ObserverCallbackSlotState::Failure;
    --in_flight_;
    RecordEmergencyLocked(terminalFailure, ticket.sequence);
    const Status signalStatus = SignalWakeLocked(ticket.sequence);
    return validFailure ? signalStatus : terminalFailure.status;
}

Status ObserverCallbackQueue::CompleteIgnored(
    const ObserverCallbackTicket& ticket) noexcept {
    std::scoped_lock lock{mutex_};
    ObserverCallbackSlot* const slot = FindPendingSlotLocked(ticket);
    if (slot == nullptr || in_flight_ == 0) {
        const Status status = ContractFailure();
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Contract, status},
            ticket.sequence == 0 ? next_sequence_ : ticket.sequence);
        (void)SignalWakeLocked(fatal_raw_sequence_.value_or(next_sequence_));
        return status;
    }
    slot->state = ObserverCallbackSlotState::Ignored;
    --in_flight_;
    return SignalWakeLocked(fatal_raw_sequence_.value_or(next_sequence_));
}

Status ObserverCallbackQueue::RecordCoordinatorFailure(
    const ObserverFailure& failure,
    const std::optional<std::uint64_t> rawCutoff) noexcept {
    const bool validOrigin =
        failure.origin == ObserverFailureOrigin::Contract ||
        failure.origin == ObserverFailureOrigin::Transport;
    const bool exactActiveFailure =
        failure.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (failure.status.hresult != S_OK ||
         failure.status.win32 != ERROR_SUCCESS);
    if (!validOrigin || !exactActiveFailure) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::scoped_lock lock{mutex_};
    if (rawCutoff.has_value() &&
        (*rawCutoff == 0 || *rawCutoff >= next_sequence_)) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    const std::uint64_t cutoff = rawCutoff.value_or(next_sequence_);
    RecordEmergencyLocked(failure, cutoff);
    return Success();
}

Status ObserverCallbackQueue::Drain(
    std::vector<ObserverCallbackEnvelope>* const output) noexcept {
    if (output == nullptr) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::scoped_lock lock{mutex_};
    const std::uint64_t cutoff = fatal_raw_sequence_.value_or(
        std::numeric_limits<std::uint64_t>::max());
    std::vector<ObserverCallbackEnvelope> candidate;
    std::size_t popCount = 0;
    std::size_t ignoredCount = 0;
    try {
        for (const ObserverCallbackSlot& slot : queue_) {
            if (slot.state == ObserverCallbackSlotState::Pending) {
                break;
            }
            ++popCount;
            if (slot.sequence >= cutoff) {
                continue;
            }
            if (slot.state == ObserverCallbackSlotState::Payload &&
                slot.payload.has_value()) {
                candidate.push_back({slot.sequence, *slot.payload});
            } else if (slot.state == ObserverCallbackSlotState::Ignored) {
                ++ignoredCount;
            } else {
                const Status status = ContractFailure();
                RecordEmergencyLocked(
                    {ObserverFailureOrigin::Contract, status},
                    slot.sequence);
                (void)SignalWakeLocked(slot.sequence);
                return status;
            }
        }
    } catch (const std::bad_alloc&) {
        const Status status = TransportFailure(E_OUTOFMEMORY);
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Transport, status},
            next_sequence_);
        (void)SignalWakeLocked(next_sequence_);
        return status;
    } catch (...) {
        const Status status = TransportFailure(E_FAIL);
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Transport, status},
            next_sequence_);
        (void)SignalWakeLocked(next_sequence_);
        return status;
    }
    if (ignored_event_count_ >
        std::numeric_limits<std::size_t>::max() - ignoredCount) {
        const Status status = ContractFailure();
        RecordEmergencyLocked(
            {ObserverFailureOrigin::Contract, status},
            next_sequence_);
        (void)SignalWakeLocked(next_sequence_);
        return status;
    }
    for (std::size_t index = 0; index < popCount; ++index) {
        queue_.pop_front();
    }
    ignored_event_count_ += ignoredCount;
    *output = std::move(candidate);
    return Success();
}

Status ObserverCallbackQueue::IsQuiescent(bool* const output) const noexcept {
    if (output == nullptr) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::scoped_lock lock{mutex_};
    *output = in_flight_ == 0 && queue_.empty();
    return Success();
}

std::optional<ObserverCallbackEmergency>
ObserverCallbackQueue::emergency() const noexcept {
    std::scoped_lock lock{mutex_};
    if (!first_failure_.has_value() || !fatal_raw_sequence_.has_value()) {
        return std::nullopt;
    }
    return ObserverCallbackEmergency{
        *first_failure_,
        any_transport_failure_,
        first_transport_status_,
        *fatal_raw_sequence_,
    };
}

std::size_t ObserverCallbackQueue::late_event_count() const noexcept {
    std::scoped_lock lock{mutex_};
    return late_event_count_;
}

std::size_t ObserverCallbackQueue::ignored_event_count() const noexcept {
    std::scoped_lock lock{mutex_};
    return ignored_event_count_;
}

HANDLE ObserverCallbackQueue::wake_event() const noexcept {
    return wake_event_;
}

ObserverOperationResult WaitForObserverActivity(
    const ObserverDeadline& deadline,
    const std::span<const HANDLE> waitHandles,
    const ObserverWaitOperations& operations,
    ObserverWaitResult* const output) noexcept {
    if (output == nullptr || !operations.now || !operations.wait ||
        !operations.get_last_error || waitHandles.empty() ||
        waitHandles.size() > MAXIMUM_WAIT_OBJECTS - 1) {
        return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    for (std::size_t index = 0; index < waitHandles.size(); ++index) {
        if (waitHandles[index] == nullptr) {
            return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (waitHandles[prior] == waitHandles[index]) {
                return ContractOperationFailure(
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER);
            }
        }
    }

    for (;;) {
        std::uint32_t remaining = 0;
        try {
            const Status remainingStatus = deadline.RemainingWaitMilliseconds(
                operations.now(),
                &remaining);
            if (!remainingStatus.ok()) {
                return OperationFailure(
                    ObserverFailureOrigin::Contract,
                    remainingStatus);
            }
        } catch (const std::bad_alloc&) {
            return TransportOperationFailure(E_OUTOFMEMORY);
        } catch (...) {
            return TransportOperationFailure(E_FAIL);
        }
        if (remaining == 0) {
            *output = {
                ObserverWaitOutcome::DeadlineReached,
                std::numeric_limits<std::size_t>::max(),
            };
            return OperationSuccess();
        }

        DWORD result = WAIT_FAILED;
        try {
            result = operations.wait(
                static_cast<DWORD>(waitHandles.size()),
                waitHandles.data(),
                remaining,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE);
        } catch (const std::bad_alloc&) {
            return TransportOperationFailure(E_OUTOFMEMORY);
        } catch (...) {
            return TransportOperationFailure(E_FAIL);
        }
        const DWORD count = static_cast<DWORD>(waitHandles.size());
        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + count) {
            *output = {
                ObserverWaitOutcome::HandleReady,
                static_cast<std::size_t>(result - WAIT_OBJECT_0),
            };
            return OperationSuccess();
        }
        if (result == WAIT_OBJECT_0 + count) {
            *output = {
                ObserverWaitOutcome::WindowMessageReady,
                std::numeric_limits<std::size_t>::max(),
            };
            return OperationSuccess();
        }
        if (result == WAIT_TIMEOUT) {
            continue;
        }
        if (result == WAIT_FAILED) {
            try {
                return ClassifyObserverWin32Failure(
                    operations.get_last_error());
            } catch (const std::bad_alloc&) {
                return TransportOperationFailure(E_OUTOFMEMORY);
            } catch (...) {
                return TransportOperationFailure(E_FAIL);
            }
        }
        return ContractOperationFailure();
    }
}

Status ValidateObserverEventProcessingResult(
    const ObserverEventProcessingResult& result,
    const ObserverCallbackEnvelope& expected,
    const std::uint64_t nextRecordSequence,
    const bool allowPending) noexcept {
    if (!IsCoherentOperationResult(result.operation) ||
        !result.operation.ok() || expected.sequence == 0 ||
        nextRecordSequence == 0 || result.raw_sequence != expected.sequence) {
        return ContractFailure();
    }
    const ObserverCallbackPayload& payload = expected.payload;
    bool payloadShapeValid = false;
    switch (payload.source) {
        case ObserverCallbackSource::ShellLifecycle:
            payloadShapeValid =
                (payload.kind == ObservedEventKind::WindowRegistered ||
                 payload.kind == ObservedEventKind::WindowRevoked) &&
                payload.shell_cookie != 0 && payload.top_level == nullptr &&
                payload.generation == 0 && payload.shell_tab == nullptr &&
                payload.structure_change_type ==
                    ObservedStructureChangeType::None;
            break;
        case ObserverCallbackSource::BrowserNavigate:
            payloadShapeValid =
                payload.kind == ObservedEventKind::NavigateComplete2 &&
                payload.shell_cookie == 0 && payload.top_level != nullptr &&
                payload.generation != 0 && payload.shell_tab != nullptr &&
                payload.structure_change_type ==
                    ObservedStructureChangeType::None;
            break;
        case ObserverCallbackSource::UiaSelection:
            payloadShapeValid =
                payload.kind == ObservedEventKind::TabSelected &&
                payload.shell_cookie == 0 && payload.top_level != nullptr &&
                payload.generation != 0 && payload.shell_tab != nullptr &&
                payload.structure_change_type ==
                    ObservedStructureChangeType::None;
            break;
        case ObserverCallbackSource::UiaStructure:
            payloadShapeValid =
                payload.kind == ObservedEventKind::TabStructureChanged &&
                payload.shell_cookie == 0 && payload.top_level != nullptr &&
                payload.generation != 0 && payload.shell_tab != nullptr &&
                (payload.structure_change_type ==
                     ObservedStructureChangeType::ChildAdded ||
                 payload.structure_change_type ==
                     ObservedStructureChangeType::ChildRemoved ||
                 payload.structure_change_type ==
                     ObservedStructureChangeType::ChildrenInvalidated ||
                 payload.structure_change_type ==
                     ObservedStructureChangeType::ChildrenBulkAdded ||
                 payload.structure_change_type ==
                     ObservedStructureChangeType::ChildrenBulkRemoved ||
                 payload.structure_change_type ==
                     ObservedStructureChangeType::ChildrenReordered);
            break;
    }
    if (!payloadShapeValid) {
        return ContractFailure();
    }
    if (result.disposition == ObserverEventDisposition::Completed) {
        if (!result.record.has_value()) {
            return ContractFailure();
        }
        const ObservedEventRecord& record = *result.record;
        const bool lifecycleCorrelated =
            payload.source == ObserverCallbackSource::ShellLifecycle
            ? record.shell_cookie_present &&
                record.shell_cookie == payload.shell_cookie
            : !record.shell_cookie_present && record.shell_cookie == 0;
        const bool sourceValuesCorrelated =
            payload.source == ObserverCallbackSource::ShellLifecycle ||
            (record.source_top_level == payload.top_level &&
             record.generation == payload.generation &&
             record.source_shell_tab_present &&
             record.source_shell_tab == payload.shell_tab);
        return record.sequence == nextRecordSequence &&
                record.kind == payload.kind &&
                record.structure_change_type ==
                    payload.structure_change_type &&
                lifecycleCorrelated && sourceValuesCorrelated
            ? Success()
            : ContractFailure();
    }
    if (result.disposition == ObserverEventDisposition::Ignored) {
        return !result.record.has_value() ? Success() : ContractFailure();
    }
    if (result.disposition == ObserverEventDisposition::Pending) {
        return allowPending && !result.record.has_value()
            ? Success()
            : ContractFailure();
    }
    return ContractFailure();
}

Status RunObserverCoordinator(
    const std::uint32_t durationMs,
    const std::uint32_t shutdownGraceMs,
    ObserverCallbackQueue* const queue,
    const HANDLE responseEvent,
    const ObserverCoordinatorOperations& operations,
    ObserverRuntimeOutcome* const output) noexcept {
    if (queue == nullptr || output == nullptr || durationMs < 1000 ||
        durationMs > 60000 || shutdownGraceMs != 5000 ||
        responseEvent == nullptr || queue->wake_event() == nullptr ||
        responseEvent == queue->wake_event() || !operations.startup ||
        !operations.process_event || !operations.pump_messages ||
        !operations.process_response || !operations.begin_stop ||
        !operations.cleanup || !operations.evaluate_gate ||
        !operations.wait.now || !operations.wait.wait ||
        !operations.wait.get_last_error) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    const Status startingState = queue->ValidateStartingState();
    if (!startingState.ok()) {
        return startingState;
    }

    ObserverRuntimeOutcome candidate{
        {
            durationMs,
            0,
            0,
            0,
            {0, 0, 0, 0, 0},
            Success(),
            Success(),
            {},
        },
        {},
        {Success(), std::nullopt, false, false},
    };
    ObserverCleanupOutcome cleanupAggregate{
        Success(),
        std::nullopt,
        false,
    };
    std::size_t processIgnoredCount = 0;
    std::deque<ObserverCallbackEnvelope> backlog;
    std::optional<ObserverCallbackEnvelope> pendingEnvelope;
    std::optional<std::uint64_t> processingFatalRawSequence;
    const std::array<HANDLE, 2> waitHandles{
        queue->wake_event(),
        responseEvent,
    };

    const auto recordQueueEmergency = [&]() {
        const auto emergency = queue->emergency();
        if (!emergency.has_value()) {
            return;
        }
        (void)candidate.failures.RecordRuntimeFailure(
            emergency->first_failure);
        if (emergency->any_transport_failure) {
            (void)candidate.failures.RecordRuntimeFailure({
                ObserverFailureOrigin::Transport,
                emergency->first_transport_status,
            });
        }
    };
    const auto recordOperation = [
        &queue,
        &recordQueueEmergency](
        ObserverOperationResult result,
        const std::optional<std::uint64_t> rawCutoff = std::nullopt) {
        if (!IsCoherentOperationResult(result)) {
            result = ContractOperationFailure(
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER);
        }
        if (!result.ok()) {
            const Status recorded = queue->RecordCoordinatorFailure(
                ToFailure(result),
                rawCutoff);
            if (!recorded.ok()) {
                result = ContractOperationFailure(
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER);
                (void)queue->RecordCoordinatorFailure(ToFailure(result));
            }
            recordQueueEmergency();
        }
        return result;
    };
    const auto applyProcessingResult = [
        &candidate,
        &processIgnoredCount,
        &pendingEnvelope,
        &recordOperation](
        ObserverEventProcessingResult result,
        const ObserverCallbackEnvelope& expected,
        const bool allowPending) {
        result.operation = recordOperation(
            result.operation,
            expected.sequence);
        if (!result.operation.ok()) {
            pendingEnvelope.reset();
            return false;
        }
        const Status shape = ValidateObserverEventProcessingResult(
            result,
            expected,
            candidate.snapshot.events.size() + 1,
            allowPending);
        if (!shape.ok()) {
            (void)recordOperation(
                ContractOperationFailure(),
                expected.sequence);
            pendingEnvelope.reset();
            return false;
        }
        if (result.disposition == ObserverEventDisposition::Pending) {
            pendingEnvelope = expected;
            return true;
        }
        pendingEnvelope.reset();
        if (result.disposition == ObserverEventDisposition::Ignored) {
            if (processIgnoredCount ==
                std::numeric_limits<std::size_t>::max()) {
                (void)recordOperation(
                    ContractOperationFailure(),
                    expected.sequence);
                return false;
            }
            ++processIgnoredCount;
            return true;
        }

        std::size_t* count = nullptr;
        switch (result.record->kind) {
            case ObservedEventKind::WindowRegistered:
                count = &candidate.snapshot.kind_counts.window_registered;
                break;
            case ObservedEventKind::WindowRevoked:
                count = &candidate.snapshot.kind_counts.window_revoked;
                break;
            case ObservedEventKind::NavigateComplete2:
                count = &candidate.snapshot.kind_counts.navigate_complete2;
                break;
            case ObservedEventKind::TabSelected:
                count = &candidate.snapshot.kind_counts.tab_selected;
                break;
            case ObservedEventKind::TabStructureChanged:
                count = &candidate.snapshot.kind_counts.tab_structure_changed;
                break;
        }
        if (count == nullptr ||
            *count == std::numeric_limits<std::size_t>::max()) {
            (void)recordOperation(
                ContractOperationFailure(),
                expected.sequence);
            return false;
        }
        try {
            candidate.snapshot.events.push_back(std::move(*result.record));
            ++*count;
            return true;
        } catch (const std::bad_alloc&) {
            (void)recordOperation(
                TransportOperationFailure(E_OUTOFMEMORY),
                expected.sequence);
        } catch (...) {
            (void)recordOperation(
                TransportOperationFailure(E_FAIL),
                expected.sequence);
        }
        return false;
    };
    const auto processBacklog = [
        &backlog,
        &pendingEnvelope,
        &processingFatalRawSequence,
        &operations,
        &applyProcessingResult,
        &recordOperation]() {
        while (!pendingEnvelope.has_value() &&
               !backlog.empty() &&
               (!processingFatalRawSequence.has_value() ||
                backlog.front().sequence < *processingFatalRawSequence)) {
            ObserverCallbackEnvelope envelope = std::move(backlog.front());
            backlog.pop_front();
            ObserverEventProcessingResult result{
                ContractOperationFailure(),
                ObserverEventDisposition::Ignored,
                envelope.sequence,
                std::nullopt,
            };
            try {
                result = operations.process_event(envelope);
            } catch (const std::bad_alloc&) {
                result.operation = TransportOperationFailure(E_OUTOFMEMORY);
            } catch (...) {
                result.operation = TransportOperationFailure(E_FAIL);
            }
            if (!applyProcessingResult(
                    std::move(result),
                    envelope,
                    true)) {
                processingFatalRawSequence = envelope.sequence;
                backlog.clear();
                break;
            }
        }
    };
    const auto drainCallbacks = [
        &backlog,
        &processingFatalRawSequence,
        &processBacklog,
        &queue,
        &recordOperation,
        &recordQueueEmergency]() {
        std::vector<ObserverCallbackEnvelope> drained;
        const Status drainedStatus = queue->Drain(&drained);
        recordQueueEmergency();
        if (!drainedStatus.ok()) {
            if (!queue->emergency().has_value()) {
                (void)recordOperation(OperationFailure(
                    ObserverFailureOrigin::Contract,
                    drainedStatus));
            }
            return;
        }
        if (processingFatalRawSequence.has_value()) {
            return;
        }
        for (ObserverCallbackEnvelope& envelope : drained) {
            try {
                backlog.push_back(std::move(envelope));
            } catch (const std::bad_alloc&) {
                processingFatalRawSequence = envelope.sequence;
                (void)recordOperation(
                    TransportOperationFailure(E_OUTOFMEMORY),
                    envelope.sequence);
                processBacklog();
                return;
            } catch (...) {
                processingFatalRawSequence = envelope.sequence;
                (void)recordOperation(
                    TransportOperationFailure(E_FAIL),
                    envelope.sequence);
                processBacklog();
                return;
            }
        }
        processBacklog();
    };

    ObserverStartupOutcome startup{
        ContractOperationFailure(),
        false,
        Success(),
        {Success(), std::nullopt, false},
    };
    try {
        startup = operations.startup();
    } catch (const std::bad_alloc&) {
        startup = {
            TransportOperationFailure(E_OUTOFMEMORY),
            true,
            TransportFailure(E_OUTOFMEMORY),
            {Success(), std::nullopt, false},
        };
    } catch (...) {
        startup = {
            TransportOperationFailure(E_FAIL),
            true,
            TransportFailure(E_FAIL),
            {Success(), std::nullopt, false},
        };
    }
    const bool exactStartupTransportSuccess =
        startup.first_transport_status.code == ErrorCode::OK &&
        startup.first_transport_status.hresult == S_OK &&
        startup.first_transport_status.win32 == ERROR_SUCCESS;
    const bool exactStartupTransportFailure =
        startup.first_transport_status.code ==
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (startup.first_transport_status.hresult != S_OK ||
         startup.first_transport_status.win32 != ERROR_SUCCESS);
    const bool coherentRollbackSuccess = startup.rollback.status.code ==
            ErrorCode::OK &&
        startup.rollback.status.hresult == S_OK &&
        startup.rollback.status.win32 == ERROR_SUCCESS &&
        !startup.rollback.failure_origin.has_value() &&
        !startup.rollback.any_transport_failure;
    const bool coherentRollbackFailure = startup.rollback.status.code ==
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (startup.rollback.status.hresult != S_OK ||
         startup.rollback.status.win32 != ERROR_SUCCESS) &&
        startup.rollback.failure_origin.has_value() &&
        (*startup.rollback.failure_origin == ObserverFailureOrigin::Contract ||
         *startup.rollback.failure_origin == ObserverFailureOrigin::Transport) &&
        (*startup.rollback.failure_origin != ObserverFailureOrigin::Transport ||
         startup.rollback.any_transport_failure);
    const bool setupTransportIsFirst =
        startup.setup.failure_origin != ObserverFailureOrigin::Transport ||
        (startup.first_transport_status.code == startup.setup.status.code &&
         startup.first_transport_status.hresult == startup.setup.status.hresult &&
         startup.first_transport_status.win32 == startup.setup.status.win32);
    const bool coherentTransportShape =
        (!startup.any_transport_failure && exactStartupTransportSuccess &&
         startup.setup.failure_origin != ObserverFailureOrigin::Transport) ||
        (startup.any_transport_failure && !startup.setup.ok() &&
         exactStartupTransportFailure && setupTransportIsFirst);
    const bool coherentStartup = IsCoherentOperationResult(startup.setup) &&
        coherentTransportShape &&
        (coherentRollbackSuccess || coherentRollbackFailure) &&
        (!startup.setup.ok() || coherentRollbackSuccess);
    if (!coherentStartup) {
        startup = {
            ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER),
            false,
            Success(),
            {Success(), std::nullopt, false},
        };
    }

    startup.setup = recordOperation(startup.setup);
    recordQueueEmergency();
    if (startup.any_transport_failure) {
        (void)recordOperation(OperationFailure(
            ObserverFailureOrigin::Transport,
            startup.first_transport_status));
    }
    if (!startup.rollback.status.ok()) {
        cleanupAggregate = startup.rollback;
    }

    ObserverDeadline observationDeadline;
    if (!candidate.failures.has_runtime_failure()) {
        ObserverDeadline::Clock::time_point ready{};
        const Status readyStatus = queue->BeginRunning(operations.wait.now, &ready);
        recordQueueEmergency();
        if (!readyStatus.ok() && !queue->emergency().has_value()) {
            (void)recordOperation(OperationFailure(
                ObserverFailureOrigin::Contract,
                readyStatus));
        } else if (readyStatus.ok()) {
            const Status deadlineStatus =
                observationDeadline.Start(ready, durationMs);
            if (!deadlineStatus.ok()) {
                (void)recordOperation(OperationFailure(
                    ObserverFailureOrigin::Contract,
                    deadlineStatus));
            }
        }
    }

    bool observationFinished = candidate.failures.has_runtime_failure();
    while (!observationFinished) {
        recordQueueEmergency();
        drainCallbacks();
        if (candidate.failures.has_runtime_failure()) {
            observationFinished = true;
            break;
        }
        ObserverWaitResult waitResult{
            ObserverWaitOutcome::DeadlineReached,
            std::numeric_limits<std::size_t>::max(),
        };
        const ObserverOperationResult exactWait = WaitForObserverActivity(
            observationDeadline,
            waitHandles,
            operations.wait,
            &waitResult);
        if (!exactWait.ok()) {
            (void)recordOperation(exactWait);
            observationFinished = true;
        } else if (waitResult.outcome == ObserverWaitOutcome::DeadlineReached) {
            observationFinished = true;
        } else if (
            waitResult.outcome == ObserverWaitOutcome::WindowMessageReady) {
            ObserverOperationResult pumped = OperationSuccess();
            try {
                pumped = operations.pump_messages();
            } catch (const std::bad_alloc&) {
                pumped = TransportOperationFailure(E_OUTOFMEMORY);
            } catch (...) {
                pumped = TransportOperationFailure(E_FAIL);
            }
            (void)recordOperation(pumped);
        } else if (waitResult.ready_handle_index == 1) {
            if (!pendingEnvelope.has_value()) {
                ObserverEventProcessingResult spurious{
                    ContractOperationFailure(),
                    ObserverEventDisposition::Ignored,
                    0,
                    std::nullopt,
                };
                (void)recordOperation(ContractOperationFailure());
                try {
                    spurious = operations.process_response(1, 0);
                } catch (const std::bad_alloc&) {
                    spurious.operation =
                        TransportOperationFailure(E_OUTOFMEMORY);
                } catch (...) {
                    spurious.operation = TransportOperationFailure(E_FAIL);
                }
                if (!spurious.operation.ok()) {
                    (void)recordOperation(spurious.operation);
                }
                observationFinished = true;
            } else {
                const ObserverCallbackEnvelope expected = *pendingEnvelope;
                ObserverEventProcessingResult response{
                    ContractOperationFailure(),
                    ObserverEventDisposition::Ignored,
                    expected.sequence,
                    std::nullopt,
                };
                try {
                    response = operations.process_response(
                        1,
                        expected.sequence);
                } catch (const std::bad_alloc&) {
                    response.operation =
                        TransportOperationFailure(E_OUTOFMEMORY);
                } catch (...) {
                    response.operation = TransportOperationFailure(E_FAIL);
                }
                if (applyProcessingResult(
                        std::move(response),
                        expected,
                        false)) {
                    processBacklog();
                } else {
                    processingFatalRawSequence = expected.sequence;
                    backlog.clear();
                }
            }
        } else if (waitResult.ready_handle_index != 0) {
            (void)recordOperation(ContractOperationFailure());
            observationFinished = true;
        }
    }

    ObserverDeadline shutdownDeadline;
    bool shutdownDeadlineReady = false;
    ObserverDeadline::Clock::time_point shutdownReady{};
    try {
        shutdownReady = operations.wait.now();
        const Status shutdownStarted =
            shutdownDeadline.Start(shutdownReady, shutdownGraceMs);
        if (!shutdownStarted.ok()) {
            (void)recordOperation(OperationFailure(
                ObserverFailureOrigin::Contract,
                shutdownStarted));
        } else {
            shutdownDeadlineReady = true;
        }
    } catch (const std::bad_alloc&) {
        (void)recordOperation(TransportOperationFailure(E_OUTOFMEMORY));
    } catch (...) {
        (void)recordOperation(TransportOperationFailure(E_FAIL));
    }

    (void)queue->EnsureStoppingState();
    ObserverOperationResult stopped = OperationSuccess();
    try {
        stopped = operations.begin_stop();
    } catch (const std::bad_alloc&) {
        stopped = TransportOperationFailure(E_OUTOFMEMORY);
    } catch (...) {
        stopped = TransportOperationFailure(E_FAIL);
    }
    (void)recordOperation(stopped);
    if (shutdownDeadlineReady) {
        std::uint32_t shutdownRemaining = 0;
        try {
            const Status remainingStatus =
                shutdownDeadline.RemainingWaitMilliseconds(
                    operations.wait.now(),
                    &shutdownRemaining);
            if (!remainingStatus.ok()) {
                (void)recordOperation(OperationFailure(
                    ObserverFailureOrigin::Contract,
                    remainingStatus));
                shutdownDeadlineReady = false;
            } else if (shutdownRemaining == 0) {
                (void)recordOperation(OperationFailure(
                    ObserverFailureOrigin::Transport,
                    {
                        ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                        HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                        ERROR_TIMEOUT,
                    }));
                shutdownDeadlineReady = false;
            }
        } catch (const std::bad_alloc&) {
            (void)recordOperation(TransportOperationFailure(E_OUTOFMEMORY));
            shutdownDeadlineReady = false;
        } catch (...) {
            (void)recordOperation(TransportOperationFailure(E_FAIL));
            shutdownDeadlineReady = false;
        }
    }
    drainCallbacks();

    bool quiescent = false;
    Status quiescentStatus = queue->IsQuiescent(&quiescent);
    if (!quiescentStatus.ok()) {
        (void)recordOperation(OperationFailure(
            ObserverFailureOrigin::Contract,
            quiescentStatus));
    }
    if (!quiescent || pendingEnvelope.has_value() || !backlog.empty()) {
        if (shutdownDeadlineReady) {
            for (;;) {
                recordQueueEmergency();
                drainCallbacks();
                quiescentStatus = queue->IsQuiescent(&quiescent);
                if (!quiescentStatus.ok()) {
                    (void)recordOperation(OperationFailure(
                        ObserverFailureOrigin::Contract,
                        quiescentStatus));
                    break;
                }
                if (quiescent && !pendingEnvelope.has_value() &&
                    backlog.empty()) {
                    break;
                }
                ObserverWaitResult waitResult{
                    ObserverWaitOutcome::DeadlineReached,
                    std::numeric_limits<std::size_t>::max(),
                };
                const ObserverOperationResult waited = WaitForObserverActivity(
                    shutdownDeadline,
                    waitHandles,
                    operations.wait,
                    &waitResult);
                if (!waited.ok()) {
                    (void)recordOperation(waited);
                    continue;
                }
                if (waitResult.outcome ==
                    ObserverWaitOutcome::DeadlineReached) {
                    (void)recordOperation(OperationFailure(
                        ObserverFailureOrigin::Transport,
                        {
                            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                            HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                            ERROR_TIMEOUT,
                        }));
                    break;
                }
                if (waitResult.outcome ==
                    ObserverWaitOutcome::WindowMessageReady) {
                    ObserverOperationResult pumped = OperationSuccess();
                    try {
                        pumped = operations.pump_messages();
                    } catch (const std::bad_alloc&) {
                        pumped = TransportOperationFailure(E_OUTOFMEMORY);
                    } catch (...) {
                        pumped = TransportOperationFailure(E_FAIL);
                    }
                    (void)recordOperation(pumped);
                } else if (waitResult.ready_handle_index == 1) {
                    if (!pendingEnvelope.has_value()) {
                        ObserverEventProcessingResult spurious{
                            ContractOperationFailure(),
                            ObserverEventDisposition::Ignored,
                            0,
                            std::nullopt,
                        };
                        (void)recordOperation(ContractOperationFailure());
                        try {
                            spurious = operations.process_response(1, 0);
                        } catch (const std::bad_alloc&) {
                            spurious.operation =
                                TransportOperationFailure(E_OUTOFMEMORY);
                        } catch (...) {
                            spurious.operation =
                                TransportOperationFailure(E_FAIL);
                        }
                        if (!spurious.operation.ok()) {
                            (void)recordOperation(spurious.operation);
                        }
                        continue;
                    }
                    const ObserverCallbackEnvelope expected = *pendingEnvelope;
                    ObserverEventProcessingResult response{
                        ContractOperationFailure(),
                        ObserverEventDisposition::Ignored,
                        expected.sequence,
                        std::nullopt,
                    };
                    try {
                        response = operations.process_response(
                            1,
                            expected.sequence);
                    } catch (const std::bad_alloc&) {
                        response.operation =
                            TransportOperationFailure(E_OUTOFMEMORY);
                    } catch (...) {
                        response.operation = TransportOperationFailure(E_FAIL);
                    }
                    if (applyProcessingResult(
                            std::move(response),
                            expected,
                            false)) {
                        processBacklog();
                    } else {
                        processingFatalRawSequence = expected.sequence;
                        backlog.clear();
                    }
                } else if (waitResult.ready_handle_index != 0) {
                    (void)recordOperation(ContractOperationFailure());
                    continue;
                }
            }
        }
    }

    ObserverCleanupOutcome finalCleanup{
        ContractFailure(),
        ObserverFailureOrigin::Contract,
        false,
    };
    try {
        finalCleanup = operations.cleanup();
    } catch (const std::bad_alloc&) {
        finalCleanup = {
            TransportFailure(E_OUTOFMEMORY),
            ObserverFailureOrigin::Transport,
            true,
        };
    } catch (...) {
        finalCleanup = {
            TransportFailure(E_FAIL),
            ObserverFailureOrigin::Transport,
            true,
        };
    }
    const bool coherentCleanupSuccess = finalCleanup.status.code == ErrorCode::OK &&
        finalCleanup.status.hresult == S_OK &&
        finalCleanup.status.win32 == ERROR_SUCCESS &&
        !finalCleanup.failure_origin.has_value() &&
        !finalCleanup.any_transport_failure;
    const bool coherentCleanupFailure = finalCleanup.status.code ==
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (finalCleanup.status.hresult != S_OK ||
         finalCleanup.status.win32 != ERROR_SUCCESS) &&
        finalCleanup.failure_origin.has_value() &&
        (*finalCleanup.failure_origin == ObserverFailureOrigin::Contract ||
         *finalCleanup.failure_origin == ObserverFailureOrigin::Transport) &&
        (*finalCleanup.failure_origin != ObserverFailureOrigin::Transport ||
         finalCleanup.any_transport_failure);
    if (!coherentCleanupSuccess && !coherentCleanupFailure) {
        finalCleanup = {
            ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER),
            ObserverFailureOrigin::Contract,
            false,
        };
    }
    if (!finalCleanup.status.ok() && cleanupAggregate.status.ok()) {
        cleanupAggregate.status = finalCleanup.status;
        cleanupAggregate.failure_origin = finalCleanup.failure_origin;
    }
    cleanupAggregate.any_transport_failure =
        cleanupAggregate.any_transport_failure ||
        finalCleanup.any_transport_failure;

    const std::size_t queueIgnored = queue->ignored_event_count();
    if (queueIgnored > std::numeric_limits<std::size_t>::max() -
            processIgnoredCount) {
        (void)recordOperation(ContractOperationFailure());
        candidate.snapshot.ignored_event_count = queueIgnored;
    } else {
        candidate.snapshot.ignored_event_count =
            queueIgnored + processIgnoredCount;
    }
    candidate.snapshot.late_event_count = queue->late_event_count();
    candidate.snapshot.event_count = candidate.snapshot.events.size();
    candidate.snapshot.runtime_status = candidate.failures.has_runtime_failure()
        ? candidate.failures.runtime_status()
        : Success();
    candidate.snapshot.cleanup_status = cleanupAggregate.status;
    candidate.completion.cleanup_status = cleanupAggregate.status;
    candidate.completion.cleanup_failure_origin =
        cleanupAggregate.failure_origin;
    candidate.completion.any_cleanup_transport_failure =
        cleanupAggregate.any_transport_failure;

    bool gatePassed = false;
    ObserverOperationResult gate = OperationSuccess();
    try {
        gate = operations.evaluate_gate(candidate.snapshot, &gatePassed);
    } catch (const std::bad_alloc&) {
        gate = TransportOperationFailure(E_OUTOFMEMORY);
    } catch (...) {
        gate = TransportOperationFailure(E_FAIL);
    }
    gate = recordOperation(gate);
    candidate.completion.gate_passed = gate.ok() && gatePassed;
    candidate.snapshot.runtime_status = candidate.failures.has_runtime_failure()
        ? candidate.failures.runtime_status()
        : Success();

    *output = std::move(candidate);
    if (output->failures.has_runtime_failure()) {
        return output->failures.runtime_status();
    }
    if (!output->completion.cleanup_status.ok()) {
        return output->completion.cleanup_status;
    }
    if (!output->completion.gate_passed) {
        return ContractFailure();
    }
    return Success();
}

ObserverOperationResult FindRegisteredShellDispatch(
    const LONG lifecycleCookie,
    const ShellWindowResolverOperations& operations,
    Microsoft::WRL::ComPtr<IDispatch>& output) {
    if (!operations.find_window) {
        return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }

    VARIANT location{};
    VariantInit(&location);
    location.vt = VT_I4;
    location.lVal = lifecycleCookie;
    long legacyHwnd = 0;
    IDispatch* rawDispatch = nullptr;
    const HRESULT hresult = operations.find_window(
        &location,
        nullptr,
        SWC_EXPLORER,
        &legacyHwnd,
        SWFO_COOKIEPASSED | SWFO_NEEDDISPATCH,
        &rawDispatch);
    Microsoft::WRL::ComPtr<IDispatch> dispatch;
    dispatch.Attach(rawDispatch);
    if (FAILED(hresult)) {
        if (hresult == E_NOINTERFACE || hresult == E_PENDING) {
            return ContractOperationFailure(hresult);
        }
        return TransportOperationFailure(hresult);
    }
    if (hresult != S_OK) {
        return ContractOperationFailure(hresult);
    }
    if (dispatch == nullptr) {
        return ContractOperationFailure();
    }
    output = std::move(dispatch);
    return OperationSuccess();
}

ObserverOperationResult ClassifyObserverConnectionPointResult(
    const HRESULT hresult,
    const bool requiredObjectPresent) {
    if (FAILED(hresult)) {
        if (hresult == E_NOINTERFACE ||
            hresult == CONNECT_E_NOCONNECTION || hresult == E_POINTER ||
            hresult == E_INVALIDARG) {
            return ContractOperationFailure(hresult);
        }
        return TransportOperationFailure(hresult);
    }
    if (hresult != S_OK) {
        return ContractOperationFailure(hresult);
    }
    return requiredObjectPresent
        ? OperationSuccess()
        : ContractOperationFailure();
}

ObserverOperationResult ClassifyObserverAdviseResult(
    const HRESULT hresult,
    const DWORD subscriptionCookie) {
    if (FAILED(hresult)) {
        if (hresult == CONNECT_E_CANNOTCONNECT || hresult == E_POINTER ||
            hresult == E_INVALIDARG) {
            return ContractOperationFailure(hresult);
        }
        return TransportOperationFailure(hresult);
    }
    if (hresult != S_OK) {
        return ContractOperationFailure(hresult);
    }
    return subscriptionCookie != 0
        ? OperationSuccess()
        : ContractOperationFailure();
}

ObserverOperationResult ClassifyObserverUnadviseResult(
    const HRESULT hresult) {
    if (FAILED(hresult)) {
        if (hresult == E_POINTER || hresult == E_INVALIDARG ||
            hresult == CONNECT_E_NOCONNECTION) {
            return ContractOperationFailure(hresult);
        }
        return TransportOperationFailure(hresult);
    }
    return hresult == S_OK
        ? OperationSuccess()
        : ContractOperationFailure(hresult);
}

ObserverOperationResult AdviseObserverConnectionPoint(
    IUnknown* const source,
    REFIID eventInterface,
    IUnknown* const sink,
    ObserverConnectionPointRegistration* const output) {
    if (output == nullptr) {
        return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    if (output->connection_point != nullptr || output->sink != nullptr ||
        output->subscription_cookie != 0 || output->owner_thread_id != 0) {
        return ContractOperationFailure();
    }
    if (source == nullptr || sink == nullptr) {
        return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }

    Microsoft::WRL::ComPtr<IConnectionPointContainer> container;
    HRESULT hresult = source->QueryInterface(IID_PPV_ARGS(&container));
    ObserverOperationResult result = ClassifyObserverConnectionPointResult(
        hresult,
        container != nullptr);
    if (!result.ok()) {
        return result;
    }
    Microsoft::WRL::ComPtr<IConnectionPoint> connectionPoint;
    hresult = container->FindConnectionPoint(eventInterface, &connectionPoint);
    result = ClassifyObserverConnectionPointResult(
        hresult,
        connectionPoint != nullptr);
    if (!result.ok()) {
        return result;
    }
    DWORD cookie = 0;
    hresult = connectionPoint->Advise(sink, &cookie);
    result = ClassifyObserverAdviseResult(hresult, cookie);
    if (!result.ok()) {
        return result;
    }

    ObserverConnectionPointRegistration candidate{};
    candidate.connection_point = std::move(connectionPoint);
    candidate.sink = sink;
    candidate.subscription_cookie = cookie;
    candidate.owner_thread_id = GetCurrentThreadId();
    *output = std::move(candidate);
    return OperationSuccess();
}

ObserverOperationResult UnadviseObserverConnectionPoint(
    ObserverConnectionPointRegistration* const registration) {
    if (registration == nullptr || registration->connection_point == nullptr ||
        registration->sink == nullptr || registration->subscription_cookie == 0 ||
        registration->owner_thread_id == 0) {
        return ContractOperationFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    if (registration->owner_thread_id != GetCurrentThreadId()) {
        return ContractOperationFailure(RPC_E_WRONG_THREAD);
    }
    const HRESULT hresult = registration->connection_point->Unadvise(
        registration->subscription_cookie);
    const ObserverOperationResult result =
        ClassifyObserverUnadviseResult(hresult);
    if (!result.ok()) {
        return result;
    }
    registration->connection_point.Reset();
    registration->sink.Reset();
    registration->subscription_cookie = 0;
    registration->owner_thread_id = 0;
    return OperationSuccess();
}

ObserverShellStaOperations CreateProductionObserverShellStaOperations() {
    const auto fromStatus = [](const Status& status) {
        if (status.ok()) {
            return OperationSuccess();
        }
        const ObserverFailureOrigin origin =
            FAILED(status.hresult) || status.win32 != ERROR_SUCCESS
            ? ObserverFailureOrigin::Transport
            : ObserverFailureOrigin::Contract;
        return OperationFailure(origin, status);
    };
    return {
        []() {
            MSG message{};
            static_cast<void>(PeekMessageW(
                &message, nullptr, WM_USER, WM_USER, PM_NOREMOVE));
            return OperationSuccess();
        },
        [fromStatus](std::vector<ExplorerWindowRecord>* const output) {
            return fromStatus(EnumerateExplorerWindows(output));
        },
        [](Microsoft::WRL::ComPtr<IShellWindows>& output) {
            if (output != nullptr) {
                return ContractOperationFailure(
                    E_INVALIDARG, ERROR_INVALID_PARAMETER);
            }
            const HRESULT hresult = CoCreateInstance(
                CLSID_ShellWindows,
                nullptr,
                CLSCTX_LOCAL_SERVER,
                IID_PPV_ARGS(&output));
            return ClassifyObserverConnectionPointResult(
                hresult, output != nullptr);
        },
        [fromStatus](
            IShellWindows* const shellWindows,
            const std::span<const HWND> targets,
            ObserverShellStaCapture* const output) {
            if (output == nullptr || !output->browsers.empty() ||
                !output->browser_set.entries.empty()) {
                return ContractOperationFailure(
                    E_INVALIDARG, ERROR_INVALID_PARAMETER);
            }
            ObserverShellStaCapture candidate{};
            const Status captured = CaptureShellBrowserSet(
                shellWindows, targets, &candidate.browser_set);
            if (!captured.ok()) {
                return fromStatus(captured);
            }
            try {
                candidate.browsers.reserve(candidate.browser_set.entries.size());
                for (const ShellBrowserEntryCapture& entry :
                     candidate.browser_set.entries) {
                    Microsoft::WRL::ComPtr<IUnknown> browser;
                    const HRESULT queried = entry.browser.As(&browser);
                    if (queried != S_OK || browser == nullptr) {
                        return queried == S_OK
                            ? ContractOperationFailure()
                            : TransportOperationFailure(queried);
                    }
                    candidate.browsers.push_back({
                        {
                            reinterpret_cast<std::uintptr_t>(
                                entry.canonical_identity.Get()),
                            entry.target_matched,
                            entry.top_level,
                            entry.shell_tab,
                        },
                        entry.canonical_identity,
                        std::move(browser),
                    });
                }
            } catch (const std::bad_alloc&) {
                return TransportOperationFailure(E_OUTOFMEMORY);
            }
            *output = std::move(candidate);
            return OperationSuccess();
        },
        [](ObserverCallbackQueue* const queue,
           Microsoft::WRL::ComPtr<IDispatch>& output) {
            return CreateShellLifecycleEventSink(queue, output);
        },
        [](ObserverCallbackQueue* const queue,
           IUnknown* const canonicalSource,
           const HWND topLevel,
           const std::uint64_t generation,
           const HWND shellTab,
           Microsoft::WRL::ComPtr<IDispatch>& output) {
            return CreateBrowserNavigateEventSink(
                queue,
                canonicalSource,
                topLevel,
                generation,
                shellTab,
                output);
        },
        [](IUnknown* const source,
           REFIID eventInterface,
           IUnknown* const sink,
           ObserverConnectionPointRegistration* const output) {
            return AdviseObserverConnectionPoint(
                source, eventInterface, sink, output);
        },
        [](ObserverConnectionPointRegistration* const registration) {
            return UnadviseObserverConnectionPoint(registration);
        },
        [](MSG* const message,
           const HWND window,
           const UINT minimum,
           const UINT maximum,
           const UINT remove) {
            return PeekMessageW(message, window, minimum, maximum, remove);
        },
        [](const MSG* const message) { return TranslateMessage(message); },
        [](const MSG* const message) { return DispatchMessageW(message); },
        []() { return GetCurrentThreadId(); },
    };
}

Status StartObserverShellStaResources(
    ObserverCallbackQueue* const queue,
    const ObserverShellStaOperations& operations,
    ObserverShellStaResourceGraph* const graph,
    ObserverShellStartupOutcome* const output) noexcept {
    if (queue == nullptr || graph == nullptr || output == nullptr ||
        graph->owner_thread_id != 0 || graph->shell_windows != nullptr ||
        !graph->browser_set.entries.empty() ||
        !graph->captured_browsers.empty() ||
        graph->startup_state.lifecycle_registration_id != 0 ||
        !graph->startup_state.browser_registrations.empty() ||
        !graph->startup_state.baseline.empty() ||
        graph->lifecycle_connection.connection_point != nullptr ||
        !graph->browser_resources.empty() ||
        !graph->target_top_levels.empty() || graph->next_registration_id != 1 ||
        !operations.prepare_message_queue || !operations.enumerate_windows ||
        !operations.create_shell_windows || !operations.capture ||
        !operations.create_lifecycle_sink || !operations.create_browser_sink ||
        !operations.advise || !operations.unadvise ||
        !operations.peek_message || !operations.translate_message ||
        !operations.dispatch_message || !operations.current_thread_id) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    const Status queueState = queue->ValidateStartingState();
    if (!queueState.ok()) {
        return queueState;
    }

    ObserverShellStartupOutcome candidate{
        OperationSuccess(),
        false,
        Success(),
        {Success(), std::nullopt, false},
    };
    const auto recordFailure = [&](ObserverOperationResult result) {
        if (!IsCoherentOperationResult(result)) {
            result = ContractOperationFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        candidate.setup = result;
        if (!result.ok() &&
            result.failure_origin == ObserverFailureOrigin::Transport) {
            candidate.any_setup_transport_failure = true;
            candidate.first_setup_transport_status = result.status;
        }
        return result;
    };
    const auto invoke = [&](const std::function<ObserverOperationResult()>& call) {
        try {
            return recordFailure(call());
        } catch (const std::bad_alloc&) {
            return recordFailure(TransportOperationFailure(E_OUTOFMEMORY));
        } catch (...) {
            return recordFailure(TransportOperationFailure(E_FAIL));
        }
    };
    const auto releaseNonConnections = [&]() {
        graph->browser_set = {};
        graph->captured_browsers.clear();
        graph->shell_windows.Reset();
        graph->target_top_levels.clear();
        graph->owner_thread_id = 0;
        graph->next_registration_id = 1;
    };

    DWORD ownerThread = 0;
    try {
        ownerThread = operations.current_thread_id();
    } catch (...) {
        recordFailure(TransportOperationFailure(E_FAIL));
    }
    if (ownerThread == 0 && candidate.setup.ok()) {
        recordFailure(ContractOperationFailure());
    }
    if (candidate.setup.ok()) {
        static_cast<void>(invoke(operations.prepare_message_queue));
    }

    std::vector<ExplorerWindowRecord> windows;
    if (candidate.setup.ok()) {
        static_cast<void>(invoke([&]() {
            return operations.enumerate_windows(&windows);
        }));
    }
    if (candidate.setup.ok()) {
        try {
            for (const ExplorerWindowRecord& window : windows) {
                if (window.hwnd == nullptr || window.process_id == 0 ||
                    window.thread_id == 0 ||
                    std::find(
                        graph->target_top_levels.begin(),
                        graph->target_top_levels.end(),
                        window.hwnd) != graph->target_top_levels.end()) {
                    recordFailure(ContractOperationFailure());
                    break;
                }
                graph->target_top_levels.push_back(window.hwnd);
            }
            if (windows.empty()) {
                recordFailure(ContractOperationFailure());
            }
        } catch (const std::bad_alloc&) {
            recordFailure(TransportOperationFailure(E_OUTOFMEMORY));
        }
    }
    if (candidate.setup.ok()) {
        static_cast<void>(invoke([&]() {
            return operations.create_shell_windows(graph->shell_windows);
        }));
        if (candidate.setup.ok() && graph->shell_windows == nullptr) {
            recordFailure(ContractOperationFailure());
        }
    }
    if (!candidate.setup.ok()) {
        releaseNonConnections();
        *output = candidate;
        return candidate.setup.status;
    }
    graph->owner_thread_id = ownerThread;

    ObserverShellStartupOperations startupOperations{};
    startupOperations.advise_lifecycle = [&](std::uint64_t* const id) {
        if (id == nullptr || *id != 0) {
            return ContractOperationFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        Microsoft::WRL::ComPtr<IDispatch> sink;
        ObserverOperationResult result = operations.create_lifecycle_sink(
            queue, sink);
        if (!IsCoherentOperationResult(result) ||
            (result.ok() && sink == nullptr)) {
            return ContractOperationFailure();
        }
        if (!result.ok()) {
            return result;
        }
        result = operations.advise(
            graph->shell_windows.Get(),
            DIID_DShellWindowsEvents,
            sink.Get(),
            &graph->lifecycle_connection);
        if (!IsCoherentOperationResult(result) ||
            (result.ok() &&
             (graph->lifecycle_connection.connection_point == nullptr ||
              graph->lifecycle_connection.sink == nullptr ||
              graph->lifecycle_connection.subscription_cookie == 0 ||
              graph->lifecycle_connection.owner_thread_id != ownerThread))) {
            return ContractOperationFailure();
        }
        if (!result.ok()) {
            return result;
        }
        *id = graph->next_registration_id++;
        return OperationSuccess();
    };
    startupOperations.capture = [&](
        std::vector<ObserverShellEntryMetadata>* const metadata) {
        if (metadata == nullptr || !metadata->empty()) {
            return ContractOperationFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        ObserverShellStaCapture capture{};
        ObserverOperationResult result = operations.capture(
            graph->shell_windows.Get(), graph->target_top_levels, &capture);
        if (!IsCoherentOperationResult(result)) {
            return ContractOperationFailure();
        }
        if (!result.ok()) {
            return result;
        }
        try {
            std::vector<ObserverShellEntryMetadata> capturedMetadata;
            capturedMetadata.reserve(capture.browsers.size());
            for (const ObserverShellStaCapturedBrowser& browser :
                 capture.browsers) {
                if (browser.metadata.canonical_identity == 0 ||
                    browser.canonical_identity == nullptr ||
                    browser.browser == nullptr ||
                    reinterpret_cast<std::uintptr_t>(
                        browser.canonical_identity.Get()) !=
                        browser.metadata.canonical_identity ||
                    browser.metadata.top_level == nullptr ||
                    browser.metadata.shell_tab == nullptr) {
                    return ContractOperationFailure();
                }
                if (std::find(
                        capturedMetadata.begin(),
                        capturedMetadata.end(),
                        browser.metadata) != capturedMetadata.end()) {
                    return ContractOperationFailure();
                }
                capturedMetadata.push_back(browser.metadata);
            }
            if (capturedMetadata.empty()) {
                return ContractOperationFailure();
            }
            graph->browser_set = std::move(capture.browser_set);
            graph->captured_browsers = std::move(capture.browsers);
            *metadata = std::move(capturedMetadata);
        } catch (const std::bad_alloc&) {
            return TransportOperationFailure(E_OUTOFMEMORY);
        }
        return OperationSuccess();
    };
    startupOperations.advise_browser = [&]
        (const ObserverShellEntryMetadata& metadata, std::uint64_t* const id) {
        if (id == nullptr || *id != 0 || !metadata.target_matched) {
            return ContractOperationFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        const auto browser = std::find_if(
            graph->captured_browsers.begin(),
            graph->captured_browsers.end(),
            [&](const ObserverShellStaCapturedBrowser& candidateBrowser) {
                return candidateBrowser.metadata == metadata;
            });
        if (browser == graph->captured_browsers.end()) {
            return ContractOperationFailure();
        }
        Microsoft::WRL::ComPtr<IDispatch> sink;
        ObserverOperationResult result = operations.create_browser_sink(
            queue,
            browser->canonical_identity.Get(),
            metadata.top_level,
            1,
            metadata.shell_tab,
            sink);
        if (!IsCoherentOperationResult(result) ||
            (result.ok() && sink == nullptr)) {
            return ContractOperationFailure();
        }
        if (!result.ok()) {
            return result;
        }
        ObserverShellStaBrowserResource resource{
            graph->next_registration_id,
            metadata.canonical_identity,
            browser->browser,
            {},
        };
        result = operations.advise(
            resource.browser.Get(),
            DIID_DWebBrowserEvents2,
            sink.Get(),
            &resource.connection);
        if (!IsCoherentOperationResult(result) ||
            (result.ok() &&
             (resource.connection.connection_point == nullptr ||
              resource.connection.sink == nullptr ||
              resource.connection.subscription_cookie == 0 ||
              resource.connection.owner_thread_id != ownerThread))) {
            return ContractOperationFailure();
        }
        if (!result.ok()) {
            return result;
        }
        try {
            graph->browser_resources.push_back(std::move(resource));
        } catch (const std::bad_alloc&) {
            static_cast<void>(operations.unadvise(&resource.connection));
            return TransportOperationFailure(E_OUTOFMEMORY);
        }
        *id = graph->next_registration_id++;
        return OperationSuccess();
    };
    startupOperations.unadvise = [&](const std::uint64_t id) {
        const auto browser = std::find_if(
            graph->browser_resources.begin(),
            graph->browser_resources.end(),
            [id](const ObserverShellStaBrowserResource& resource) {
                return resource.registration_id == id;
            });
        if (browser != graph->browser_resources.end()) {
            ObserverOperationResult result = operations.unadvise(
                &browser->connection);
            if (result.ok()) {
                graph->browser_resources.erase(browser);
            }
            return result;
        }
        if (graph->lifecycle_connection.connection_point == nullptr) {
            return ContractOperationFailure();
        }
        return operations.unadvise(&graph->lifecycle_connection);
    };

    ObserverShellStartupOutcome coreOutcome{};
    const Status started = StartObserverShellSubscriptions(
        queue,
        startupOperations,
        &graph->startup_state,
        &coreOutcome);
    candidate = coreOutcome;
    if (!started.ok() && graph->browser_resources.empty() &&
        graph->lifecycle_connection.connection_point == nullptr) {
        releaseNonConnections();
    }
    *output = candidate;
    return started;
}

ObserverShellCleanupOutcome CleanupObserverShellStaResources(
    const ObserverShellStaOperations& operations,
    ObserverShellStaResourceGraph* const graph) noexcept {
    if (graph == nullptr || !operations.unadvise ||
        !operations.current_thread_id || graph->owner_thread_id == 0) {
        return {
            ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER),
            ObserverFailureOrigin::Contract,
            false,
        };
    }
    DWORD currentThread = 0;
    try {
        currentThread = operations.current_thread_id();
    } catch (...) {
        return {
            TransportFailure(E_FAIL),
            ObserverFailureOrigin::Transport,
            true,
        };
    }
    if (currentThread != graph->owner_thread_id) {
        return {
            ContractFailure(RPC_E_WRONG_THREAD),
            ObserverFailureOrigin::Contract,
            false,
        };
    }
    ObserverShellStartupOperations cleanupOperations{};
    cleanupOperations.unadvise = [&](const std::uint64_t id) {
        const auto browser = std::find_if(
            graph->browser_resources.begin(),
            graph->browser_resources.end(),
            [id](const ObserverShellStaBrowserResource& resource) {
                return resource.registration_id == id;
            });
        if (browser != graph->browser_resources.end()) {
            ObserverOperationResult result = operations.unadvise(
                &browser->connection);
            if (!IsCoherentOperationResult(result)) {
                return ContractOperationFailure();
            }
            if (result.ok()) {
                graph->browser_resources.erase(browser);
            }
            return result;
        }
        if (graph->lifecycle_connection.connection_point == nullptr) {
            return ContractOperationFailure();
        }
        ObserverOperationResult result = operations.unadvise(
            &graph->lifecycle_connection);
        return IsCoherentOperationResult(result)
            ? result
            : ContractOperationFailure();
    };
    ObserverShellCleanupOutcome outcome = CleanupObserverShellSubscriptions(
        cleanupOperations, &graph->startup_state);
    if (graph->startup_state.lifecycle_registration_id == 0 &&
        graph->startup_state.browser_registrations.empty() &&
        graph->browser_resources.empty() &&
        graph->lifecycle_connection.connection_point == nullptr) {
        graph->browser_set = {};
        graph->captured_browsers.clear();
        graph->shell_windows.Reset();
        graph->target_top_levels.clear();
        graph->owner_thread_id = 0;
        graph->next_registration_id = 1;
    }
    return outcome;
}

ObserverOperationResult PumpObserverShellStaMessages(
    const ObserverShellStaOperations& operations,
    const ObserverShellStaResourceGraph& graph) noexcept {
    if (graph.owner_thread_id == 0 || !operations.peek_message ||
        !operations.translate_message || !operations.dispatch_message ||
        !operations.current_thread_id) {
        return ContractOperationFailure(
            E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    try {
        if (operations.current_thread_id() != graph.owner_thread_id) {
            return ContractOperationFailure(RPC_E_WRONG_THREAD);
        }
        MSG message{};
        while (operations.peek_message(
            &message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                return ContractOperationFailure();
            }
            static_cast<void>(operations.translate_message(&message));
            static_cast<void>(operations.dispatch_message(&message));
        }
        return OperationSuccess();
    } catch (const std::bad_alloc&) {
        return TransportOperationFailure(E_OUTOFMEMORY);
    } catch (...) {
        return TransportOperationFailure(E_FAIL);
    }
}

}  // namespace winexinfo
