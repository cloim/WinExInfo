#include "probe/uia_observer_worker.h"

#include "probe/event_observer.h"
#include "probe/uia_probe.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <UIAutomation.h>
#include <wrl/client.h>

namespace winexinfo {
namespace {

ObserverOperationResult WorkerSuccess() noexcept {
    return {
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        std::nullopt,
    };
}

ObserverOperationResult WorkerContractFailure(
    const HRESULT hresult = S_FALSE,
    const DWORD win32 = ERROR_SUCCESS) noexcept {
    return {
        {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, win32},
        ObserverFailureOrigin::Contract,
    };
}

ObserverOperationResult WorkerTransportFailure(const HRESULT hresult) noexcept {
    return {
        {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            hresult,
            HRESULT_FACILITY(hresult) == FACILITY_WIN32
                ? static_cast<DWORD>(HRESULT_CODE(hresult))
                : ERROR_SUCCESS,
        },
        ObserverFailureOrigin::Transport,
    };
}

bool IsExactOperation(const ObserverOperationResult& operation) noexcept {
    const bool success = operation.status.code == ErrorCode::OK &&
        operation.status.hresult == S_OK &&
        operation.status.win32 == ERROR_SUCCESS &&
        !operation.failure_origin.has_value();
    const bool failure =
        operation.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (operation.status.hresult != S_OK ||
         operation.status.win32 != ERROR_SUCCESS) &&
        operation.failure_origin.has_value() &&
        (*operation.failure_origin == ObserverFailureOrigin::Contract ||
         *operation.failure_origin == ObserverFailureOrigin::Transport);
    return success || failure;
}

ObserverCleanupOutcome CleanupFromOperation(
    const ObserverOperationResult& operation) noexcept {
    return {
        operation.status,
        operation.failure_origin,
        operation.failure_origin == ObserverFailureOrigin::Transport,
    };
}

}  // namespace

bool IsIgnorableObserverUiaCallbackStatus(const Status& status) noexcept {
    return status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        status.win32 == ERROR_SUCCESS &&
        (status.hresult ==
             static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE) ||
         status.hresult == S_FALSE);
}

struct ObserverUiaMtaWorker::Impl final {
    enum class Phase {
        Created,
        Starting,
        Running,
        Stopping,
        Joined,
    };

    explicit Impl(ObserverUiaWorkerOperations value)
        : operations(std::move(value)) {}

    ObserverUiaWorkerOperations operations;
    std::mutex mutex;
    std::condition_variable condition;
    std::thread thread;
    Phase phase = Phase::Created;
    HANDLE response_event = nullptr;
    std::uint64_t next_command_id = 1;
    std::deque<ObserverUiaCommand> commands;
    std::deque<ObserverUiaResponse> responses;
    std::optional<ObserverUiaResponse> terminal_response;
    bool startup_complete = false;
    ObserverOperationResult startup = WorkerContractFailure();
    ObserverCleanupOutcome cleanup{
        {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            S_FALSE,
            ERROR_SUCCESS,
        },
        ObserverFailureOrigin::Contract,
        false,
    };

    void Run() noexcept {
        ObserverOperationResult initialized = WorkerSuccess();
        try {
            initialized = operations.initialize();
        } catch (const std::bad_alloc&) {
            initialized = WorkerTransportFailure(E_OUTOFMEMORY);
        } catch (...) {
            initialized = WorkerTransportFailure(E_FAIL);
        }
        if (!IsExactOperation(initialized)) {
            initialized = WorkerContractFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        {
            std::lock_guard lock(mutex);
            startup = initialized;
            startup_complete = true;
            phase = initialized.ok() ? Phase::Running : Phase::Stopping;
        }
        condition.notify_all();

        if (initialized.ok()) {
            for (;;) {
                std::optional<ObserverUiaCommand> command;
                {
                    std::unique_lock lock(mutex);
                    condition.wait(lock, [&]() {
                        return phase == Phase::Stopping || !commands.empty();
                    });
                    if (commands.empty() && phase == Phase::Stopping) {
                        break;
                    }
                    command = commands.front();
                    commands.pop_front();
                }

                ObserverUiaResponse response{
                    command->id,
                    command->kind,
                    command->target,
                    WorkerTransportFailure(E_FAIL),
                    0,
                };
                try {
                    response = operations.process(*command);
                } catch (const std::bad_alloc&) {
                    response.operation = WorkerTransportFailure(E_OUTOFMEMORY);
                } catch (...) {
                    response.operation = WorkerTransportFailure(E_FAIL);
                }
                if (response.id != command->id ||
                    response.kind != command->kind ||
                    response.target != command->target ||
                    !IsExactOperation(response.operation) ||
                    (command->kind != ObserverUiaCommandKind::Reenumerate &&
                     response.direct_child_count != 0)) {
                    response = {
                        command->id,
                        command->kind,
                        command->target,
                        WorkerContractFailure(),
                        0,
                    };
                }

                std::lock_guard lock(mutex);
                try {
                    operations.append_response(
                        &responses, std::move(response));
                } catch (const std::bad_alloc&) {
                    terminal_response = ObserverUiaResponse{
                        command->id,
                        command->kind,
                        command->target,
                        WorkerTransportFailure(E_OUTOFMEMORY),
                        0,
                    };
                    phase = Phase::Stopping;
                    static_cast<void>(operations.set_event(response_event));
                    continue;
                }
                if (!operations.set_event(response_event)) {
                    const DWORD error = operations.get_last_error();
                    responses.back().operation = error == ERROR_SUCCESS
                        ? WorkerContractFailure()
                        : WorkerTransportFailure(HRESULT_FROM_WIN32(error));
                    phase = Phase::Stopping;
                }
            }
        }

        ObserverCleanupOutcome cleanupResult{
            {ErrorCode::OK, S_OK, ERROR_SUCCESS},
            std::nullopt,
            false,
        };
        if (initialized.ok()) {
            try {
                cleanupResult = operations.cleanup();
            } catch (const std::bad_alloc&) {
                cleanupResult = CleanupFromOperation(
                    WorkerTransportFailure(E_OUTOFMEMORY));
            } catch (...) {
                cleanupResult = CleanupFromOperation(
                    WorkerTransportFailure(E_FAIL));
            }
            try {
                operations.uninitialize();
            } catch (...) {
                if (cleanupResult.status.ok()) {
                    cleanupResult = CleanupFromOperation(
                        WorkerTransportFailure(E_FAIL));
                } else {
                    cleanupResult.any_transport_failure = true;
                }
            }
        }
        {
            std::lock_guard lock(mutex);
            cleanup = cleanupResult;
            phase = Phase::Joined;
        }
        condition.notify_all();
    }
};

ObserverUiaMtaWorker::ObserverUiaMtaWorker(
    ObserverUiaWorkerOperations operations)
    : impl_(std::make_unique<Impl>(std::move(operations))) {}

ObserverUiaMtaWorker::~ObserverUiaMtaWorker() {
    if (impl_ != nullptr && impl_->thread.joinable()) {
        static_cast<void>(BeginStopping());
        impl_->thread.join();
    }
}

ObserverOperationResult ObserverUiaMtaWorker::Start(
    const HANDLE responseEvent) {
    if (impl_ == nullptr || responseEvent == nullptr ||
        !impl_->operations.initialize || !impl_->operations.process ||
        !impl_->operations.cleanup || !impl_->operations.uninitialize ||
        !impl_->operations.set_event || !impl_->operations.reset_event ||
        !impl_->operations.get_last_error ||
        !impl_->operations.append_response) {
        return WorkerContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->phase != Impl::Phase::Created) {
            return WorkerContractFailure();
        }
        impl_->response_event = responseEvent;
        impl_->phase = Impl::Phase::Starting;
    }
    try {
        impl_->thread = std::thread([this]() { impl_->Run(); });
    } catch (const std::bad_alloc&) {
        std::lock_guard lock(impl_->mutex);
        impl_->phase = Impl::Phase::Created;
        impl_->response_event = nullptr;
        return WorkerTransportFailure(E_OUTOFMEMORY);
    } catch (...) {
        std::lock_guard lock(impl_->mutex);
        impl_->phase = Impl::Phase::Created;
        impl_->response_event = nullptr;
        return WorkerTransportFailure(E_FAIL);
    }

    std::unique_lock lock(impl_->mutex);
    impl_->condition.wait(lock, [&]() { return impl_->startup_complete; });
    return impl_->startup;
}

ObserverOperationResult ObserverUiaMtaWorker::Submit(
    const ObserverUiaCommandKind kind,
    const ObserverUiaTarget& target,
    std::uint64_t* const commandId) {
    const bool validKind = kind == ObserverUiaCommandKind::AddTarget ||
        kind == ObserverUiaCommandKind::RemoveTarget ||
        kind == ObserverUiaCommandKind::Reenumerate;
    if (impl_ == nullptr || commandId == nullptr || *commandId != 0 ||
        !validKind || target.top_level == nullptr || target.generation == 0 ||
        target.active_view == nullptr || target.shell_tab == nullptr) {
        return WorkerContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->phase != Impl::Phase::Running ||
        impl_->next_command_id == 0 ||
        impl_->next_command_id == UINT64_MAX) {
        return WorkerContractFailure();
    }
    const std::uint64_t id = impl_->next_command_id;
    try {
        impl_->commands.push_back({id, kind, target});
    } catch (const std::bad_alloc&) {
        return WorkerTransportFailure(E_OUTOFMEMORY);
    }
    ++impl_->next_command_id;
    *commandId = id;
    impl_->condition.notify_one();
    return WorkerSuccess();
}

ObserverOperationResult ObserverUiaMtaWorker::Consume(
    const std::uint64_t expectedCommandId,
    ObserverUiaResponse* const output) {
    if (impl_ == nullptr || output == nullptr) {
        return WorkerContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->responses.empty() && !impl_->terminal_response.has_value()) {
        return WorkerContractFailure();
    }
    const ObserverUiaResponse& next = !impl_->responses.empty()
        ? impl_->responses.front()
        : *impl_->terminal_response;
    if (expectedCommandId != 0 && next.id != expectedCommandId) {
        return WorkerContractFailure();
    }
    const bool lastResponse =
        impl_->responses.empty() ||
        (impl_->responses.size() == 1 &&
         !impl_->terminal_response.has_value());
    if (lastResponse &&
        !impl_->operations.reset_event(impl_->response_event)) {
        const DWORD error = impl_->operations.get_last_error();
        return error == ERROR_SUCCESS
            ? WorkerContractFailure()
            : WorkerTransportFailure(HRESULT_FROM_WIN32(error));
    }
    if (!impl_->responses.empty()) {
        *output = std::move(impl_->responses.front());
        impl_->responses.pop_front();
    } else {
        *output = std::move(*impl_->terminal_response);
        impl_->terminal_response.reset();
    }
    return WorkerSuccess();
}

ObserverOperationResult ObserverUiaMtaWorker::BeginStopping() {
    if (impl_ == nullptr) {
        return WorkerContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->phase == Impl::Phase::Joined ||
        impl_->phase == Impl::Phase::Created) {
        return WorkerContractFailure();
    }
    if (impl_->phase == Impl::Phase::Starting ||
        impl_->phase == Impl::Phase::Running) {
        impl_->phase = Impl::Phase::Stopping;
        impl_->condition.notify_all();
    }
    return WorkerSuccess();
}

ObserverCleanupOutcome ObserverUiaMtaWorker::Join() {
    if (impl_ == nullptr || !impl_->thread.joinable()) {
        return CleanupFromOperation(
            WorkerContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER));
    }
    static_cast<void>(BeginStopping());
    impl_->thread.join();
    std::lock_guard lock(impl_->mutex);
    return impl_->cleanup;
}

namespace {

ObserverOperationResult UiaOperationFromStatus(const Status& status) noexcept {
    if (status.ok()) {
        return WorkerSuccess();
    }
    return {
        status,
        FAILED(status.hresult) || status.win32 != ERROR_SUCCESS
            ? std::optional{ObserverFailureOrigin::Transport}
            : std::optional{ObserverFailureOrigin::Contract},
    };
}

ObserverOperationResult UiaOperationFromHresult(const HRESULT hresult) noexcept {
    return UiaOperationFromStatus(
        ClassifyExactUiaCallResult(hresult, true));
}

ObservedStructureChangeType MapStructureChange(
    const StructureChangeType changeType) noexcept {
    switch (changeType) {
        case StructureChangeType_ChildAdded:
            return ObservedStructureChangeType::ChildAdded;
        case StructureChangeType_ChildRemoved:
            return ObservedStructureChangeType::ChildRemoved;
        case StructureChangeType_ChildrenInvalidated:
            return ObservedStructureChangeType::ChildrenInvalidated;
        case StructureChangeType_ChildrenBulkAdded:
            return ObservedStructureChangeType::ChildrenBulkAdded;
        case StructureChangeType_ChildrenBulkRemoved:
            return ObservedStructureChangeType::ChildrenBulkRemoved;
        case StructureChangeType_ChildrenReordered:
            return ObservedStructureChangeType::ChildrenReordered;
        default:
            return ObservedStructureChangeType::None;
    }
}

class SelectionObserverHandler final : public IUIAutomationEventHandler {
public:
    SelectionObserverHandler(
        ObserverCallbackQueue* const queue,
        const ObserverUiaTarget target,
        IUIAutomation* const automation,
        IUIAutomationTreeWalker* const walker,
        IUIAutomationElement* const tabList,
        const DWORD processId)
        : queue_(queue),
          target_(target),
          automation_(automation),
          walker_(walker),
          tab_list_(tabList),
          process_id_(processId) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** const output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (iid != IID_IUnknown && iid != IID_IUIAutomationEventHandler) {
            return E_NOINTERFACE;
        }
        *output = static_cast<IUIAutomationEventHandler*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE HandleAutomationEvent(
        IUIAutomationElement* const sender,
        const EVENTID eventId) override {
        ObserverCallbackTicket ticket{};
        const Status admitted = queue_->Admit(&ticket);
        if (!admitted.ok() || !ticket.admitted) {
            return S_OK;
        }
        const auto fail = [&](const Status& status) {
            if (IsIgnorableObserverUiaCallbackStatus(status)) {
                static_cast<void>(queue_->CompleteIgnored(ticket));
                return S_OK;
            }
            const ObserverFailureOrigin origin =
                FAILED(status.hresult) || status.win32 != ERROR_SUCCESS
                ? ObserverFailureOrigin::Transport
                : ObserverFailureOrigin::Contract;
            static_cast<void>(queue_->CompleteFailure(
                ticket, {origin, status}));
            return S_OK;
        };
        if (sender == nullptr) {
            return fail({
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER,
            });
        }

        UiaEventEvidence evidence{
            eventId,
            TreeScope_Children,
            target_.generation,
            target_.generation,
            process_id_,
            0,
            UiaSenderRelation::Outside,
            L"",
            0,
            L"",
            L"",
            false,
            std::nullopt,
            false,
            0,
            VT_EMPTY,
            UiaStructureSenderRole::NotApplicable,
        };
        const auto readString = [&](const int property, std::wstring* output) {
            BSTR value = nullptr;
            HRESULT hresult = E_INVALIDARG;
            if (property == 0) {
                hresult = sender->get_CachedFrameworkId(&value);
            } else if (property == 1) {
                hresult = sender->get_CachedClassName(&value);
            } else {
                hresult = sender->get_CachedAutomationId(&value);
            }
            const Status status = ClassifyExactUiaCallResult(hresult, true);
            if (!status.ok()) {
                SysFreeString(value);
                return status;
            }
            if (value != nullptr) {
                output->assign(value, SysStringLen(value));
            }
            SysFreeString(value);
            return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
        };
        Status status = readString(0, &evidence.framework_id);
        if (!status.ok()) {
            return fail(status);
        }
        status = readString(1, &evidence.class_name);
        if (!status.ok()) {
            return fail(status);
        }
        status = readString(2, &evidence.automation_id);
        if (!status.ok()) {
            return fail(status);
        }
        HRESULT hresult = sender->get_CachedControlType(&evidence.control_type);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return fail(status);
        }
        int processId = 0;
        hresult = sender->get_CachedProcessId(&processId);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return fail(status);
        }
        evidence.sender_process_id = static_cast<DWORD>(processId);
        BOOL offscreen = FALSE;
        hresult = sender->get_CachedIsOffscreen(&offscreen);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return fail(status);
        }
        evidence.sender_is_offscreen = offscreen != FALSE;

        Microsoft::WRL::ComPtr<IUIAutomationElement> parent;
        hresult = walker_->GetParentElement(sender, &parent);
        status = ClassifyExactUiaCallResult(hresult, parent != nullptr);
        if (!status.ok()) {
            return fail(status);
        }
        BOOL same = FALSE;
        hresult = automation_->CompareElements(parent.Get(), tab_list_.Get(), &same);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return fail(status);
        }
        evidence.sender_relation = same != FALSE
            ? UiaSenderRelation::DirectChild
            : UiaSenderRelation::Outside;

        bool accepted = false;
        ObservedEventKind kind{};
        status = ClassifyUiaEvent(evidence, &accepted, &kind);
        if (!status.ok()) {
            return fail(status);
        }
        if (!accepted) {
            static_cast<void>(queue_->CompleteIgnored(ticket));
            return S_OK;
        }
        static_cast<void>(queue_->Complete(ticket, {
            ObserverCallbackSource::UiaSelection,
            kind,
            0,
            target_.top_level,
            target_.generation,
            target_.shell_tab,
            ObservedStructureChangeType::None,
        }));
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    ObserverCallbackQueue* queue_;
    ObserverUiaTarget target_;
    Microsoft::WRL::ComPtr<IUIAutomation> automation_;
    Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker_;
    Microsoft::WRL::ComPtr<IUIAutomationElement> tab_list_;
    DWORD process_id_;
};

class StructureObserverHandler final :
    public IUIAutomationStructureChangedEventHandler {
public:
    StructureObserverHandler(
        ObserverCallbackQueue* const queue,
        const ObserverUiaTarget target,
        const DWORD processId)
        : queue_(queue),
          target_(target),
          process_id_(processId) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** const output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (iid != IID_IUnknown &&
            iid != IID_IUIAutomationStructureChangedEventHandler) {
            return E_NOINTERFACE;
        }
        *output = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(
        IUIAutomationElement* const sender,
        const StructureChangeType changeType,
        SAFEARRAY* const runtimeId) override {
        ObserverCallbackTicket ticket{};
        const Status admitted = queue_->Admit(&ticket);
        if (!admitted.ok() || !ticket.admitted) {
            return S_OK;
        }
        const auto fail = [&](const Status& status) {
            if (IsIgnorableObserverUiaCallbackStatus(status)) {
                static_cast<void>(queue_->CompleteIgnored(ticket));
                return S_OK;
            }
            const ObserverFailureOrigin origin =
                FAILED(status.hresult) || status.win32 != ERROR_SUCCESS
                ? ObserverFailureOrigin::Transport
                : ObserverFailureOrigin::Contract;
            static_cast<void>(queue_->CompleteFailure(
                ticket, {origin, status}));
            return S_OK;
        };
        if (sender == nullptr) {
            return fail({
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER,
            });
        }
        int processId = 0;
        HRESULT hresult = sender->get_CachedProcessId(&processId);
        Status status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return fail(status);
        }
        const UiaSenderRelation relation =
            UiaSenderRelation::RegisteredSubtree;
        UINT dimensions = 0;
        VARTYPE vartype = VT_EMPTY;
        if (runtimeId != nullptr) {
            dimensions = SafeArrayGetDim(runtimeId);
            hresult = SafeArrayGetVartype(runtimeId, &vartype);
            status = ClassifyExactUiaCallResult(hresult, true);
            if (!status.ok()) {
                return fail(status);
            }
        }
        const bool childAdded =
            changeType == StructureChangeType_ChildAdded;
        UiaEventEvidence evidence{
            UIA_StructureChangedEventId,
            TreeScope_Subtree,
            target_.generation,
            target_.generation,
            process_id_,
            static_cast<DWORD>(processId),
            relation,
            L"",
            0,
            L"",
            L"",
            false,
            changeType,
            runtimeId != nullptr,
            dimensions,
            vartype,
            childAdded ? UiaStructureSenderRole::AddedChild
                       : UiaStructureSenderRole::Container,
        };
        bool accepted = false;
        ObservedEventKind kind{};
        status = ClassifyUiaEvent(evidence, &accepted, &kind);
        if (!status.ok()) {
            return fail(status);
        }
        if (!accepted) {
            static_cast<void>(queue_->CompleteIgnored(ticket));
            return S_OK;
        }
        const ObservedStructureChangeType mapped =
            MapStructureChange(changeType);
        if (mapped == ObservedStructureChangeType::None) {
            return fail({
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                S_FALSE,
                ERROR_SUCCESS,
            });
        }
        static_cast<void>(queue_->Complete(ticket, {
            ObserverCallbackSource::UiaStructure,
            kind,
            0,
            target_.top_level,
            target_.generation,
            target_.shell_tab,
            mapped,
        }));
        return S_OK;
    }

private:
    std::atomic<ULONG> references_{1};
    ObserverCallbackQueue* queue_;
    ObserverUiaTarget target_;
    DWORD process_id_;
};

struct ProductionUiaResource final {
    ObserverUiaTarget target;
    RetainedUiaContractCapture capture;
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cache;
    Microsoft::WRL::ComPtr<IUIAutomationEventHandler> selection;
    Microsoft::WRL::ComPtr<IUIAutomationStructureChangedEventHandler> structure;
    bool selection_registered = false;
    bool structure_registered = false;
};

struct ProductionUiaState final {
    ObserverCallbackQueue* queue = nullptr;
    DWORD owner_thread_id = 0;
    bool com_initialized = false;
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    Microsoft::WRL::ComPtr<IUIAutomationTreeWalker> walker;
    std::vector<ProductionUiaResource> resources;
    ObserverFailureState rollback_failures;
    std::optional<ObserverFailureOrigin> first_rollback_origin;
};

}  // namespace

ObserverCleanupOutcome RemoveObserverUiaHandlerRegistrations(
    const ObserverUiaHandlerRemovalOperations& operations,
    ObserverUiaHandlerRemovalState* const state) noexcept {
    const bool invalidState = state == nullptr ||
        (state != nullptr &&
         (state->owner_identity == 0 ||
          (state->selection_registered &&
           (state->selection_handler_identity == 0 ||
            !operations.remove_selection)) ||
          (state->structure_registered &&
           (state->structure_handler_identity == 0 ||
            !operations.remove_structure))));
    if (invalidState) {
        return {
            {
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER,
            },
            ObserverFailureOrigin::Contract,
            false,
        };
    }
    ObserverFailureState failures;
    std::optional<ObserverFailureOrigin> firstOrigin;
    const auto remove = [&](
        const std::function<ObserverOperationResult(
            std::uintptr_t, std::uintptr_t)>& operation,
        const std::uintptr_t handlerIdentity,
        bool* const registered) {
        ObserverOperationResult result = WorkerTransportFailure(E_FAIL);
        try {
            result = operation(state->owner_identity, handlerIdentity);
        } catch (const std::bad_alloc&) {
            result = WorkerTransportFailure(E_OUTOFMEMORY);
        } catch (...) {
            result = WorkerTransportFailure(E_FAIL);
        }
        if (!IsExactOperation(result)) {
            result = WorkerContractFailure(
                E_INVALIDARG, ERROR_INVALID_PARAMETER);
        }
        if (result.ok()) {
            *registered = false;
            return;
        }
        if (!failures.has_runtime_failure()) {
            firstOrigin = result.failure_origin;
        }
        static_cast<void>(failures.RecordRuntimeFailure({
            *result.failure_origin,
            result.status,
        }));
    };
    if (state->structure_registered) {
        remove(
            operations.remove_structure,
            state->structure_handler_identity,
            &state->structure_registered);
    }
    if (state->selection_registered) {
        remove(
            operations.remove_selection,
            state->selection_handler_identity,
            &state->selection_registered);
    }
    if (!failures.has_runtime_failure()) {
        return {
            {ErrorCode::OK, S_OK, ERROR_SUCCESS},
            std::nullopt,
            false,
        };
    }
    return {
        failures.runtime_status(),
        firstOrigin,
        failures.any_transport_failure(),
    };
}

ObserverUiaWorkerOperations CreateProductionObserverUiaWorkerOperations(
    ObserverCallbackQueue* const queue) {
    auto state = std::make_shared<ProductionUiaState>();
    state->queue = queue;
    const auto removeHandlers = [state](
        ProductionUiaResource* const resource) {
        if (resource == nullptr) {
            return ObserverCleanupOutcome{
                {
                    ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                    E_INVALIDARG,
                    ERROR_INVALID_PARAMETER,
                },
                ObserverFailureOrigin::Contract,
                false,
            };
        }
        ObserverUiaHandlerRemovalState removalState{
            reinterpret_cast<std::uintptr_t>(
                resource->capture.tab_list_element.Get()),
            reinterpret_cast<std::uintptr_t>(resource->selection.Get()),
            reinterpret_cast<std::uintptr_t>(resource->structure.Get()),
            resource->selection_registered,
            resource->structure_registered,
        };
        const ObserverUiaHandlerRemovalOperations operations{
            [state, resource](
                const std::uintptr_t ownerIdentity,
                const std::uintptr_t handlerIdentity) {
                if (ownerIdentity != reinterpret_cast<std::uintptr_t>(
                        resource->capture.tab_list_element.Get()) ||
                    handlerIdentity != reinterpret_cast<std::uintptr_t>(
                        resource->selection.Get())) {
                    return WorkerContractFailure(
                        E_INVALIDARG, ERROR_INVALID_PARAMETER);
                }
                return UiaOperationFromHresult(
                    state->automation->RemoveAutomationEventHandler(
                        UIA_SelectionItem_ElementSelectedEventId,
                        resource->capture.tab_list_element.Get(),
                        resource->selection.Get()));
            },
            [state, resource](
                const std::uintptr_t ownerIdentity,
                const std::uintptr_t handlerIdentity) {
                if (ownerIdentity != reinterpret_cast<std::uintptr_t>(
                        resource->capture.tab_list_element.Get()) ||
                    handlerIdentity != reinterpret_cast<std::uintptr_t>(
                        resource->structure.Get())) {
                    return WorkerContractFailure(
                        E_INVALIDARG, ERROR_INVALID_PARAMETER);
                }
                return UiaOperationFromHresult(
                    state->automation->RemoveStructureChangedEventHandler(
                        resource->capture.tab_list_element.Get(),
                        resource->structure.Get()));
            },
        };
        const ObserverCleanupOutcome outcome =
            RemoveObserverUiaHandlerRegistrations(
                operations, &removalState);
        resource->selection_registered =
            removalState.selection_registered;
        resource->structure_registered =
            removalState.structure_registered;
        return outcome;
    };
    const auto cleanupResources = [state, removeHandlers]() {
        ObserverFailureState failures = state->rollback_failures;
        std::optional<ObserverFailureOrigin> firstOrigin =
            state->first_rollback_origin;
        for (std::size_t remaining = state->resources.size();
             remaining > 0;
             --remaining) {
            ProductionUiaResource& resource = state->resources[remaining - 1];
            const ObserverCleanupOutcome removed = removeHandlers(&resource);
            if (!removed.status.ok()) {
                if (!failures.has_runtime_failure()) {
                    firstOrigin = removed.failure_origin;
                }
                static_cast<void>(failures.RecordRuntimeFailure({
                    *removed.failure_origin,
                    removed.status,
                }));
            }
            if (!resource.structure_registered &&
                !resource.selection_registered) {
                state->resources.erase(
                    state->resources.begin() +
                    static_cast<std::ptrdiff_t>(remaining - 1));
            }
        }
        if (!failures.has_runtime_failure()) {
            return ObserverCleanupOutcome{
                {ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        }
        return ObserverCleanupOutcome{
            failures.runtime_status(),
            firstOrigin,
            failures.any_transport_failure(),
        };
    };

    return {
        [state]() {
            if (state->queue == nullptr || state->com_initialized ||
                state->automation != nullptr || state->walker != nullptr) {
                return WorkerContractFailure(
                    E_INVALIDARG, ERROR_INVALID_PARAMETER);
            }
            HRESULT hresult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (hresult != S_OK) {
                return FAILED(hresult)
                    ? WorkerTransportFailure(hresult)
                    : WorkerContractFailure(hresult);
            }
            state->com_initialized = true;
            hresult = CoCreateInstance(
                CLSID_CUIAutomation8,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&state->automation));
            ObserverOperationResult result = UiaOperationFromStatus(
                ClassifyExactUiaCallResult(
                    hresult, state->automation != nullptr));
            if (result.ok()) {
                hresult = state->automation->get_ControlViewWalker(
                    &state->walker);
                result = UiaOperationFromStatus(
                    ClassifyExactUiaCallResult(
                        hresult, state->walker != nullptr));
            }
            if (!result.ok()) {
                state->walker.Reset();
                state->automation.Reset();
                CoUninitialize();
                state->com_initialized = false;
                return result;
            }
            state->owner_thread_id = GetCurrentThreadId();
            return WorkerSuccess();
        },
        [state, removeHandlers](const ObserverUiaCommand& command) {
            ObserverUiaResponse response{
                command.id,
                command.kind,
                command.target,
                WorkerSuccess(),
                0,
            };
            if (!state->com_initialized ||
                state->owner_thread_id != GetCurrentThreadId()) {
                response.operation = WorkerContractFailure(RPC_E_WRONG_THREAD);
                return response;
            }
            const auto existing = std::find_if(
                state->resources.begin(),
                state->resources.end(),
                [&](const ProductionUiaResource& resource) {
                    return resource.target.top_level ==
                            command.target.top_level &&
                        resource.target.generation == command.target.generation;
                });
            if (command.kind == ObserverUiaCommandKind::AddTarget) {
                if (existing != state->resources.end()) {
                    response.operation = WorkerContractFailure();
                    return response;
                }
                try {
                    state->resources.reserve(state->resources.size() + 1);
                } catch (const std::bad_alloc&) {
                    response.operation = WorkerTransportFailure(E_OUTOFMEMORY);
                    return response;
                }
                ProductionUiaResource resource{};
                resource.target = command.target;
                Status status = CaptureRetainedUiaContract(
                    state->automation.Get(),
                    command.target.top_level,
                    command.target.active_view,
                    &resource.capture);
                if (!status.ok()) {
                    response.operation = UiaOperationFromStatus(status);
                    return response;
                }
                status = CreateUiaEventCacheRequest(
                    state->automation.Get(), &resource.cache);
                if (!status.ok()) {
                    response.operation = UiaOperationFromStatus(status);
                    return response;
                }
                resource.selection.Attach(new (std::nothrow)
                    SelectionObserverHandler(
                        state->queue,
                        command.target,
                        state->automation.Get(),
                        state->walker.Get(),
                        resource.capture.tab_list_element.Get(),
                        resource.capture.snapshot.tab_list.process_id));
                resource.structure.Attach(new (std::nothrow)
                    StructureObserverHandler(
                        state->queue,
                        command.target,
                        resource.capture.snapshot.tab_list.process_id));
                if (resource.selection == nullptr ||
                    resource.structure == nullptr) {
                    response.operation = WorkerTransportFailure(E_OUTOFMEMORY);
                    return response;
                }
                HRESULT hresult = state->automation->AddAutomationEventHandler(
                    UIA_SelectionItem_ElementSelectedEventId,
                    resource.capture.tab_list_element.Get(),
                    TreeScope_Children,
                    resource.cache.Get(),
                    resource.selection.Get());
                response.operation = UiaOperationFromHresult(hresult);
                if (!response.operation.ok()) {
                    return response;
                }
                resource.selection_registered = true;
                hresult = state->automation->AddStructureChangedEventHandler(
                    resource.capture.tab_list_element.Get(),
                    TreeScope_Subtree,
                    resource.cache.Get(),
                    resource.structure.Get());
                response.operation = UiaOperationFromHresult(hresult);
                if (!response.operation.ok()) {
                    const ObserverCleanupOutcome removed =
                        removeHandlers(&resource);
                    if (!removed.status.ok()) {
                        if (!state->first_rollback_origin.has_value()) {
                            state->first_rollback_origin =
                                removed.failure_origin;
                        }
                        static_cast<void>(
                            state->rollback_failures.RecordRuntimeFailure({
                                *removed.failure_origin,
                                removed.status,
                            }));
                    }
                    if (resource.selection_registered) {
                        state->resources.push_back(std::move(resource));
                    }
                    return response;
                }
                resource.structure_registered = true;
                std::size_t childCount = 0;
                status = ReenumerateRetainedTabListDirectChildren(
                    resource.capture, &childCount);
                if (!status.ok()) {
                    const ObserverCleanupOutcome removed =
                        removeHandlers(&resource);
                    if (!removed.status.ok()) {
                        if (!state->first_rollback_origin.has_value()) {
                            state->first_rollback_origin =
                                removed.failure_origin;
                        }
                        static_cast<void>(
                            state->rollback_failures.RecordRuntimeFailure({
                                *removed.failure_origin,
                                removed.status,
                            }));
                    }
                    if (resource.structure_registered ||
                        resource.selection_registered) {
                        state->resources.push_back(std::move(resource));
                    }
                    response.operation = UiaOperationFromStatus(status);
                    return response;
                }
                state->resources.push_back(std::move(resource));
                return response;
            }
            if (existing == state->resources.end() ||
                existing->target != command.target) {
                response.operation = WorkerContractFailure();
                return response;
            }
            if (command.kind == ObserverUiaCommandKind::Reenumerate) {
                Status status = ReenumerateRetainedTabListDirectChildren(
                    existing->capture, &response.direct_child_count);
                response.operation = UiaOperationFromStatus(status);
                return response;
            }

            ProductionUiaResource& resource = *existing;
            const ObserverCleanupOutcome removed = removeHandlers(&resource);
            if (removed.status.ok()) {
                state->resources.erase(existing);
            } else {
                response.operation = {
                    removed.status, removed.failure_origin};
            }
            return response;
        },
        cleanupResources,
        [state]() {
            state->resources.clear();
            state->walker.Reset();
            state->automation.Reset();
            state->owner_thread_id = 0;
            if (state->com_initialized) {
                CoUninitialize();
                state->com_initialized = false;
            }
        },
        [](const HANDLE event) { return SetEvent(event); },
        [](const HANDLE event) { return ResetEvent(event); },
        []() { return GetLastError(); },
        [](std::deque<ObserverUiaResponse>* const responses,
           ObserverUiaResponse&& response) {
            responses->push_back(std::move(response));
        },
    };
}

}  // namespace winexinfo
