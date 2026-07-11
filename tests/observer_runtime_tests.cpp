#include "test_framework.h"

#include "probe/observer_runtime.h"

#include <ExDisp.h>
#include <ExDispid.h>
#include <olectl.h>

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <new>
#include <thread>
#include <vector>

namespace {

template <typename Operations>
concept HasRegisteredShellResolver = requires(
    Operations operations,
    IShellWindows* shellWindows,
    LONG lifecycleCookie,
    Microsoft::WRL::ComPtr<IUnknown>& canonicalIdentity) {
    operations.resolve_registered(
        shellWindows, lifecycleCookie, canonicalIdentity);
};

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

winexinfo::ObserverCallbackPayload LifecyclePayload(const LONG cookie) {
    return {
        winexinfo::ObserverCallbackSource::ShellLifecycle,
        winexinfo::ObservedEventKind::WindowRegistered,
        cookie,
        nullptr,
        0,
        nullptr,
        winexinfo::ObservedStructureChangeType::None,
    };
}

winexinfo::ObserverCallbackQueueOperations QueueOperations(
    DWORD* const lastError,
    std::vector<int>* const wakeCalls,
    std::vector<int>* const fallbackCalls) {
    return {
        [](std::deque<winexinfo::ObserverCallbackSlot>* const queue,
           const std::uint64_t sequence) {
            queue->push_back({
                sequence,
                winexinfo::ObserverCallbackSlotState::Pending,
                std::nullopt,
                std::nullopt,
            });
        },
        [wakeCalls](const HANDLE) {
            wakeCalls->push_back(1);
            return TRUE;
        },
        [fallbackCalls]() {
            fallbackCalls->push_back(1);
            return TRUE;
        },
        [lastError]() { return *lastError; },
    };
}

winexinfo::Status PushCallback(
    winexinfo::ObserverCallbackQueue* const queue,
    winexinfo::ObserverCallbackPayload payload) {
    winexinfo::ObserverCallbackTicket ticket{};
    const winexinfo::Status admitted = queue->Admit(&ticket);
    if (!admitted.ok() || !ticket.admitted) {
        return admitted;
    }
    return queue->Complete(ticket, std::move(payload));
}

winexinfo::Status StartQueue(
    winexinfo::ObserverCallbackQueue* const queue,
    winexinfo::ObserverDeadline::Clock::time_point* const readyOutput = nullptr) {
    using namespace std::chrono_literals;
    winexinfo::ObserverDeadline::Clock::time_point localReady{};
    auto* const output = readyOutput == nullptr ? &localReady : readyOutput;
    return queue->BeginRunning(
        []() {
            return winexinfo::ObserverDeadline::Clock::time_point{12345ms};
        },
        output);
}

class FakeDispatch final : public IDispatch {
public:
    explicit FakeDispatch(std::vector<int>* const queryLog = nullptr)
        : query_log_(queryLog) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** const output) override {
        if (query_log_ != nullptr) {
            query_log_->push_back(2);
        }
        if (output == nullptr) {
            return E_POINTER;
        }
        if (iid == IID_IUnknown || iid == IID_IDispatch) {
            *output = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *output = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return --references_;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* const count) override {
        if (count != nullptr) {
            *count = 0;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(
        const UINT,
        const LCID,
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
        DISPID,
        REFIID,
        LCID,
        WORD,
        DISPPARAMS*,
        VARIANT*,
        EXCEPINFO*,
        UINT*) override {
        return E_NOTIMPL;
    }

    [[nodiscard]] ULONG references() const noexcept {
        return references_;
    }

private:
    ULONG references_ = 1;
    std::vector<int>* query_log_;
};

class FakeShellWindows final : public IShellWindows {
public:
    HRESULT find_hresult = E_NOTIMPL;
    IDispatch* find_dispatch = nullptr;
    std::size_t find_calls = 0;
    VARTYPE find_location_vartype = VT_EMPTY;
    LONG find_location_cookie = 0;
    bool find_root_present = false;
    VARTYPE find_root_vartype = VT_NULL;
    int find_shell_class = 0;
    int find_flags = 0;

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** const output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch ||
            iid == IID_IShellWindows) {
            *output = static_cast<IShellWindows*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return --references_;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* const count) override {
        if (count != nullptr) {
            *count = 0;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override {
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
        DISPID,
        REFIID,
        LCID,
        WORD,
        DISPPARAMS*,
        VARIANT*,
        EXCEPINFO*,
        UINT*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE get_Count(long*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Item(VARIANT, IDispatch**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Register(IDispatch*, long, int, long*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE RegisterPending(
        long,
        VARIANT*,
        VARIANT*,
        int,
        long*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Revoke(long) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OnNavigate(long, VARIANT*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OnActivated(long, VARIANT_BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE FindWindowSW(
        VARIANT* const location,
        VARIANT* const root,
        const int shellClass,
        long*,
        const int flags,
        IDispatch** const output) override {
        ++find_calls;
        if (location != nullptr) {
            find_location_vartype = location->vt;
            find_location_cookie = location->lVal;
        }
        find_root_present = root != nullptr;
        if (root != nullptr) {
            find_root_vartype = root->vt;
        }
        find_shell_class = shellClass;
        find_flags = flags;
        if (output != nullptr) {
            *output = nullptr;
            if (find_hresult == S_OK && find_dispatch != nullptr) {
                find_dispatch->AddRef();
                *output = find_dispatch;
            }
        }
        return find_hresult;
    }

    HRESULT STDMETHODCALLTYPE OnCreated(long, IUnknown*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override {
        return E_NOTIMPL;
    }

    [[nodiscard]] ULONG references() const noexcept {
        return references_;
    }

private:
    ULONG references_ = 1;
};

class FakeConnectionSource final :
    public IConnectionPointContainer,
    public IConnectionPoint {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        void** const output) override {
        ++query_calls;
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (FAILED(query_hresult)) {
            return query_hresult;
        }
        if (iid == IID_IUnknown || iid == IID_IConnectionPointContainer) {
            *output = static_cast<IConnectionPointContainer*>(this);
        } else if (iid == IID_IConnectionPoint) {
            *output = static_cast<IConnectionPoint*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return --references;
    }

    HRESULT STDMETHODCALLTYPE EnumConnectionPoints(
        IEnumConnectionPoints** const output) override {
        if (output != nullptr) {
            *output = nullptr;
        }
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE FindConnectionPoint(
        REFIID iid,
        IConnectionPoint** const output) override {
        ++find_calls;
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = nullptr;
        if (iid != expected_event_interface) {
            return CONNECT_E_NOCONNECTION;
        }
        if (FAILED(find_hresult)) {
            return find_hresult;
        }
        *output = static_cast<IConnectionPoint*>(this);
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetConnectionInterface(IID* const output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = expected_event_interface;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetConnectionPointContainer(
        IConnectionPointContainer** const output) override {
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = static_cast<IConnectionPointContainer*>(this);
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Advise(
        IUnknown* const sink,
        DWORD* const output) override {
        ++advise_calls;
        observed_sink = sink;
        if (output == nullptr) {
            return E_POINTER;
        }
        *output = advise_cookie;
        return advise_hresult;
    }

    HRESULT STDMETHODCALLTYPE Unadvise(const DWORD cookie) override {
        ++unadvise_calls;
        observed_unadvise_cookie = cookie;
        return unadvise_hresult;
    }

    HRESULT STDMETHODCALLTYPE EnumConnections(
        IEnumConnections** const output) override {
        if (output != nullptr) {
            *output = nullptr;
        }
        return E_NOTIMPL;
    }

    IID expected_event_interface = DIID_DShellWindowsEvents;
    HRESULT query_hresult = S_OK;
    HRESULT find_hresult = S_OK;
    HRESULT advise_hresult = S_OK;
    HRESULT unadvise_hresult = S_OK;
    DWORD advise_cookie = 17;
    DWORD observed_unadvise_cookie = 0;
    IUnknown* observed_sink = nullptr;
    ULONG references = 1;
    std::size_t query_calls = 0;
    std::size_t find_calls = 0;
    std::size_t advise_calls = 0;
    std::size_t unadvise_calls = 0;
};

void RequireActiveFailure(
    const winexinfo::Status& status,
    const HRESULT hresult) {
    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, hresult);
}

winexinfo::ObserverOperationResult RuntimeOperationSuccess() {
    return {
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
        std::nullopt,
    };
}

winexinfo::ObserverOperationResult RuntimeOperationFailure(
    const winexinfo::ObserverFailureOrigin origin,
    const HRESULT hresult) {
    return {
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            hresult,
            HRESULT_FACILITY(hresult) == FACILITY_WIN32
                ? static_cast<DWORD>(HRESULT_CODE(hresult))
                : ERROR_SUCCESS,
        },
        origin,
    };
}

winexinfo::EventObservationSnapshot PassingProductionGateSnapshot() {
    const HWND topLevel = Handle(0x100);
    const HWND shellTab = Handle(0x101);
    const HWND previousView = Handle(0x102);
    const HWND currentView = Handle(0x103);
    const winexinfo::Status success{
        winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
    return {
        45000,
        5,
        0,
        0,
        {1, 1, 1, 1, 1},
        success,
        success,
        {
            {1, 1, winexinfo::ObservedEventKind::WindowRegistered,
             winexinfo::ObservedEventTransition::Reconciled, topLevel, true,
             shellTab, false, true, 42,
             winexinfo::ObservedStructureChangeType::None, nullptr, nullptr, 0,
             false, L"", false, L"", success, 1, 2, 1, 0, 1, shellTab},
            {2, 1, winexinfo::ObservedEventKind::TabStructureChanged,
             winexinfo::ObservedEventTransition::Remapped, topLevel, true,
             shellTab, false, false, 0,
             winexinfo::ObservedStructureChangeType::ChildAdded, nullptr,
             previousView, 1, false, L"", true, L"C:\\A", success,
             2, 2, 0, 0, 2, shellTab},
            {3, 1, winexinfo::ObservedEventKind::TabSelected,
             winexinfo::ObservedEventTransition::Remapped, topLevel, true,
             shellTab, false, false, 0,
             winexinfo::ObservedStructureChangeType::None, previousView,
             currentView, 1, true, L"C:\\A", true, L"C:\\B", success,
             2, 2, 0, 0, 2, shellTab},
            {4, 1, winexinfo::ObservedEventKind::NavigateComplete2,
             winexinfo::ObservedEventTransition::Remapped, topLevel, true,
             shellTab, true, false, 0,
             winexinfo::ObservedStructureChangeType::None, currentView,
             previousView, 1, true, L"C:\\B", true, L"C:\\C", success,
             2, 2, 0, 0, 2, shellTab},
            {5, 1, winexinfo::ObservedEventKind::WindowRevoked,
             winexinfo::ObservedEventTransition::Reconciled, Handle(0x200), true,
             Handle(0x201), false, true, 43,
             winexinfo::ObservedStructureChangeType::None, Handle(0x202),
             nullptr, 0, true, L"D:\\A", false, L"", success,
             2, 1, 0, 1, 1, shellTab},
        },
    };
}

}  // namespace

WXI_TEST(
    observer_runtime_ignores_only_transient_tab_refresh_mismatch,
    "observer_runtime.transient_tab_refresh_mismatch") {
    const winexinfo::ObserverOperationResult transient{
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            S_FALSE,
            ERROR_INVALID_DATA,
        },
        winexinfo::ObserverFailureOrigin::Transport,
    };
    WXI_REQUIRE(winexinfo::IsIgnorableObserverTabRefreshResult(transient));
    auto otherWin32 = transient;
    otherWin32.status.win32 = ERROR_INVALID_PARAMETER;
    WXI_REQUIRE(!winexinfo::IsIgnorableObserverTabRefreshResult(otherWin32));
    auto otherHresult = transient;
    otherHresult.status.hresult = E_FAIL;
    WXI_REQUIRE(!winexinfo::IsIgnorableObserverTabRefreshResult(otherHresult));
    auto otherOrigin = transient;
    otherOrigin.failure_origin = winexinfo::ObserverFailureOrigin::Contract;
    WXI_REQUIRE(!winexinfo::IsIgnorableObserverTabRefreshResult(otherOrigin));
}

WXI_TEST(
    observer_runtime_callback_queue_assigns_gapless_fifo_after_emplace,
    "observer_runtime.callback_queue_gapless_fifo") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());

    WXI_REQUIRE(PushCallback(&queue, LifecyclePayload(41)).ok());
    WXI_REQUIRE(PushCallback(&queue, LifecyclePayload(42)).ok());
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{2});
    WXI_REQUIRE_EQ(drained[0].sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(drained[0].payload.shell_cookie, LONG{41});
    WXI_REQUIRE_EQ(drained[1].sequence, std::uint64_t{2});
    WXI_REQUIRE_EQ(drained[1].payload.shell_cookie, LONG{42});
    WXI_REQUIRE_EQ(wakeCalls.size(), std::size_t{2});
}

WXI_TEST(
    observer_runtime_callback_queue_does_not_reorder_slow_first_callback,
    "observer_runtime.callback_queue_entry_order") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket first{};
    winexinfo::ObserverCallbackTicket second{};
    WXI_REQUIRE(queue.Admit(&first).ok());
    WXI_REQUIRE(queue.Admit(&second).ok());
    WXI_REQUIRE_EQ(first.sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(second.sequence, std::uint64_t{2});
    WXI_REQUIRE(queue.Complete(second, LifecyclePayload(42)).ok());

    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    WXI_REQUIRE(queue.Complete(first, LifecyclePayload(41)).ok());
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{2});
    WXI_REQUIRE_EQ(drained[0].sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(drained[0].payload.shell_cookie, LONG{41});
    WXI_REQUIRE_EQ(drained[1].sequence, std::uint64_t{2});
    WXI_REQUIRE_EQ(drained[1].payload.shell_cookie, LONG{42});
}

WXI_TEST(
    observer_runtime_setup_callback_blocks_ready_cutoff,
    "observer_runtime.setup_callback_instability") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));

    winexinfo::ObserverCallbackTicket setupTicket{};
    RequireActiveFailure(queue.Admit(&setupTicket), S_FALSE);
    WXI_REQUIRE(!setupTicket.admitted);
    winexinfo::ObserverDeadline::Clock::time_point unchanged{
        std::chrono::milliseconds{77}};
    RequireActiveFailure(queue.BeginRunning(
        []() { return winexinfo::ObserverDeadline::Clock::now(); },
        &unchanged), S_FALSE);
    WXI_REQUIRE_EQ(
        unchanged,
        winexinfo::ObserverDeadline::Clock::time_point{
            std::chrono::milliseconds{77}});
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE(!emergency->any_transport_failure);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(wakeCalls.size(), std::size_t{1});
    WXI_REQUIRE(fallbackCalls.empty());
    WXI_REQUIRE(queue.BeginStopping().ok());
    winexinfo::ObserverCallbackTicket lateTicket{};
    WXI_REQUIRE(queue.Admit(&lateTicket).ok());
    WXI_REQUIRE(!lateTicket.admitted);
    WXI_REQUIRE_EQ(queue.late_event_count(), std::size_t{1});
}

WXI_TEST(
    observer_runtime_queue_allocation_failure_uses_transport_emergency_without_gap,
    "observer_runtime.callback_queue_allocation_emergency") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    bool failFirst = true;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.reserve =
        [&failFirst](std::deque<winexinfo::ObserverCallbackSlot>* const queue,
                     const std::uint64_t sequence) {
            if (failFirst) {
                failFirst = false;
                throw std::bad_alloc{};
            }
            queue->push_back({
                sequence,
                winexinfo::ObserverCallbackSlotState::Pending,
                std::nullopt,
                std::nullopt,
            });
        };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());

    winexinfo::ObserverCallbackTicket failedTicket{};
    RequireActiveFailure(queue.Admit(&failedTicket), E_OUTOFMEMORY);
    WXI_REQUIRE(!failedTicket.admitted);
    winexinfo::ObserverCallbackTicket droppedTicket{};
    WXI_REQUIRE(queue.Admit(&droppedTicket).ok());
    WXI_REQUIRE(!droppedTicket.admitted);
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE(emergency->any_transport_failure);
    WXI_REQUIRE_EQ(emergency->first_transport_status.hresult, E_OUTOFMEMORY);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(wakeCalls.size(), std::size_t{1});
    WXI_REQUIRE(fallbackCalls.empty());
}

WXI_TEST(
    observer_runtime_queue_setevent_failure_preserves_enqueued_sequence,
    "observer_runtime.callback_queue_setevent_emergency") {
    DWORD lastError = ERROR_ACCESS_DENIED;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.set_event = [](const HANDLE) { return FALSE; };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());

    winexinfo::ObserverCallbackTicket ticket{};
    WXI_REQUIRE(queue.Admit(&ticket).ok());
    const winexinfo::Status enqueued =
        queue.Complete(ticket, LifecyclePayload(41));
    WXI_REQUIRE_EQ(enqueued.win32, DWORD{ERROR_ACCESS_DENIED});
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{1});
    WXI_REQUIRE_EQ(drained[0].sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(drained[0].payload.shell_cookie, LONG{41});
    WXI_REQUIRE(queue.emergency()->any_transport_failure);
    WXI_REQUIRE_EQ(
        queue.emergency()->fatal_raw_sequence,
        std::uint64_t{2});
    WXI_REQUIRE_EQ(fallbackCalls.size(), std::size_t{1});
}

WXI_TEST(
    observer_runtime_queue_preserves_setevent_and_fallback_failures,
    "observer_runtime.callback_queue_fallback_emergency") {
    DWORD lastError = ERROR_ACCESS_DENIED;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.set_event = [](const HANDLE) { return FALSE; };
    operations.post_emergency = [&]() {
        fallbackCalls.push_back(1);
        lastError = ERROR_INVALID_THREAD_ID;
        return FALSE;
    };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());

    winexinfo::ObserverCallbackTicket ticket{};
    WXI_REQUIRE(queue.Admit(&ticket).ok());
    const winexinfo::Status enqueued =
        queue.Complete(ticket, LifecyclePayload(41));
    WXI_REQUIRE_EQ(enqueued.win32, DWORD{ERROR_ACCESS_DENIED});
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_transport_status.win32,
        DWORD{ERROR_ACCESS_DENIED});
    WXI_REQUIRE(emergency->any_transport_failure);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, std::uint64_t{2});
}

WXI_TEST(
    observer_runtime_stopping_queue_counts_late_without_enqueuing,
    "observer_runtime.callback_queue_late_drop") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    WXI_REQUIRE(queue.BeginStopping().ok());
    winexinfo::ObserverCallbackTicket ticket{};
    WXI_REQUIRE(queue.Admit(&ticket).ok());
    WXI_REQUIRE(!ticket.admitted);
    WXI_REQUIRE_EQ(queue.late_event_count(), std::size_t{1});
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    WXI_REQUIRE(wakeCalls.empty());
}

WXI_TEST(
    observer_runtime_callback_failure_and_ignore_close_reserved_gaps,
    "observer_runtime.callback_queue_terminal_slots") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket ignored{};
    winexinfo::ObserverCallbackTicket failed{};
    winexinfo::ObserverCallbackTicket payload{};
    WXI_REQUIRE(queue.Admit(&ignored).ok());
    WXI_REQUIRE(queue.Admit(&failed).ok());
    WXI_REQUIRE(queue.Admit(&payload).ok());
    WXI_REQUIRE(queue.Complete(payload, LifecyclePayload(43)).ok());
    WXI_REQUIRE(queue.CompleteIgnored(ignored).ok());
    const winexinfo::ObserverFailure failure{
        winexinfo::ObserverFailureOrigin::Contract,
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    WXI_REQUIRE(queue.CompleteFailure(failed, failure).ok());
    WXI_REQUIRE(queue.has_callback_failure());

    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    WXI_REQUIRE_EQ(queue.ignored_event_count(), std::size_t{1});
    WXI_REQUIRE_EQ(
        queue.emergency()->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE_EQ(
        queue.emergency()->fatal_raw_sequence,
        failed.sequence);
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    bool quiescent = false;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_callback_failure_emits_only_prior_raw_sequences,
    "observer_runtime.callback_queue_fatal_cutoff") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket prior{};
    winexinfo::ObserverCallbackTicket failed{};
    winexinfo::ObserverCallbackTicket later{};
    WXI_REQUIRE(queue.Admit(&prior).ok());
    WXI_REQUIRE(queue.Admit(&failed).ok());
    WXI_REQUIRE(queue.Admit(&later).ok());
    WXI_REQUIRE(queue.Complete(later, LifecyclePayload(43)).ok());
    const winexinfo::ObserverFailure failure{
        winexinfo::ObserverFailureOrigin::Contract,
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    WXI_REQUIRE(queue.CompleteFailure(failed, failure).ok());
    winexinfo::ObserverCallbackTicket dropped{};
    WXI_REQUIRE(queue.Admit(&dropped).ok());
    WXI_REQUIRE(!dropped.admitted);

    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    WXI_REQUIRE(queue.Complete(prior, LifecyclePayload(41)).ok());
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{1});
    WXI_REQUIRE_EQ(drained[0].sequence, prior.sequence);
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, failed.sequence);
    bool quiescent = false;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_win32_failure_zero_is_contract_without_fabrication,
    "observer_runtime.win32_failure_origin") {
    const auto inconsistent =
        winexinfo::ClassifyObserverWin32Failure(ERROR_SUCCESS);
    WXI_REQUIRE_EQ(
        inconsistent.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(inconsistent.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(inconsistent.status.win32, DWORD{ERROR_SUCCESS});

    const auto denied =
        winexinfo::ClassifyObserverWin32Failure(ERROR_ACCESS_DENIED);
    WXI_REQUIRE_EQ(
        denied.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Transport});
    WXI_REQUIRE_EQ(
        denied.status.hresult,
        HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    WXI_REQUIRE_EQ(denied.status.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(
    observer_runtime_setevent_zero_last_error_is_contract_and_keeps_payload,
    "observer_runtime.callback_queue_zero_last_error") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.set_event = [](const HANDLE) { return FALSE; };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket ticket{};
    WXI_REQUIRE(queue.Admit(&ticket).ok());
    const winexinfo::Status completed =
        queue.Complete(ticket, LifecyclePayload(41));
    WXI_REQUIRE_EQ(completed.hresult, S_FALSE);
    WXI_REQUIRE_EQ(completed.win32, DWORD{ERROR_SUCCESS});
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE(!emergency->any_transport_failure);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, std::uint64_t{2});
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{1});
    WXI_REQUIRE_EQ(drained[0].sequence, ticket.sequence);
}

WXI_TEST(
    observer_runtime_fatal_cutoff_lowers_without_replacing_first_failure,
    "observer_runtime.callback_queue_fatal_cutoff_min") {
    DWORD lastError = ERROR_ACCESS_DENIED;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    std::size_t setEventCalls = 0;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.set_event = [&setEventCalls](const HANDLE) {
        ++setEventCalls;
        return setEventCalls == 1 ? FALSE : TRUE;
    };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket first{};
    winexinfo::ObserverCallbackTicket second{};
    winexinfo::ObserverCallbackTicket third{};
    WXI_REQUIRE(queue.Admit(&first).ok());
    WXI_REQUIRE(queue.Admit(&second).ok());
    WXI_REQUIRE(queue.Admit(&third).ok());
    RequireActiveFailure(
        queue.Complete(third, LifecyclePayload(43)),
        HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    const winexinfo::ObserverFailure laterEarlierFailure{
        winexinfo::ObserverFailureOrigin::Contract,
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    WXI_REQUIRE(queue.CompleteFailure(second, laterEarlierFailure).ok());
    WXI_REQUIRE(queue.Complete(first, LifecyclePayload(41)).ok());

    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{1});
    WXI_REQUIRE_EQ(drained[0].sequence, first.sequence);
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Transport);
    WXI_REQUIRE_EQ(
        emergency->first_failure.status.hresult,
        HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, second.sequence);
    bool quiescent = false;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_invalid_callback_failure_closes_ticket_as_contract,
    "observer_runtime.callback_queue_invalid_failure_terminal") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket ticket{};
    WXI_REQUIRE(queue.Admit(&ticket).ok());
    const winexinfo::ObserverFailure invalidFailure{
        static_cast<winexinfo::ObserverFailureOrigin>(99),
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
    };
    const winexinfo::Status status =
        queue.CompleteFailure(ticket, invalidFailure);
    RequireActiveFailure(status, E_INVALIDARG);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_INVALID_PARAMETER});
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, ticket.sequence);
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    bool quiescent = false;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_coordinator_failure_linearizes_cutoff_and_drops_future,
    "observer_runtime.callback_queue_coordinator_failure") {
    DWORD lastError = ERROR_ACCESS_DENIED;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    bool failWake = true;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.set_event = [&failWake](HANDLE) {
        return failWake ? FALSE : TRUE;
    };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket current{};
    WXI_REQUIRE(queue.Admit(&current).ok());
    const winexinfo::ObserverFailure localFailure{
        winexinfo::ObserverFailureOrigin::Contract,
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    WXI_REQUIRE(queue.RecordCoordinatorFailure(
                    localFailure,
                    current.sequence)
                    .ok());
    WXI_REQUIRE(!queue.has_callback_failure());
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE(!emergency->any_transport_failure);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, current.sequence);
    WXI_REQUIRE(wakeCalls.empty());
    WXI_REQUIRE(fallbackCalls.empty());
    failWake = false;

    winexinfo::ObserverCallbackTicket future{};
    WXI_REQUIRE(queue.Admit(&future).ok());
    WXI_REQUIRE(!future.admitted);
    WXI_REQUIRE_EQ(queue.late_event_count(), std::size_t{1});
    WXI_REQUIRE(queue.Complete(current, LifecyclePayload(41)).ok());
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
    bool quiescent = false;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_callback_failure_precedes_later_coordinator_transport,
    "observer_runtime.callback_queue_coordinator_failure_order") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    winexinfo::ObserverCallbackTicket callback{};
    RequireActiveFailure(queue.Admit(&callback), S_FALSE);
    const winexinfo::ObserverFailure laterTransport{
        winexinfo::ObserverFailureOrigin::Transport,
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            RPC_E_DISCONNECTED,
            ERROR_SUCCESS,
        },
    };
    WXI_REQUIRE(queue.RecordCoordinatorFailure(laterTransport).ok());
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE_EQ(emergency->first_failure.status.hresult, S_FALSE);
    WXI_REQUIRE(emergency->any_transport_failure);
    WXI_REQUIRE_EQ(
        emergency->first_transport_status.hresult,
        RPC_E_DISCONNECTED);
    RequireActiveFailure(
        queue.RecordCoordinatorFailure(laterTransport, 1),
        E_INVALIDARG);
}

WXI_TEST(
    observer_runtime_stopping_waits_for_pre_admitted_callback_quiescence,
    "observer_runtime.callback_queue_stopping_quiescence") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    winexinfo::ObserverCallbackTicket ticket{};
    WXI_REQUIRE(queue.Admit(&ticket).ok());
    WXI_REQUIRE(queue.BeginStopping().ok());
    bool quiescent = true;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(!quiescent);
    WXI_REQUIRE(queue.CompleteIgnored(ticket).ok());
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_ready_cutoff_returns_clock_inside_queue_transition,
    "observer_runtime.ready_cutoff") {
    using namespace std::chrono_literals;
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const auto exactReady =
        winexinfo::ObserverDeadline::Clock::time_point{424242ms};
    winexinfo::ObserverDeadline::Clock::time_point ready{};
    WXI_REQUIRE(queue.BeginRunning(
                    [exactReady]() { return exactReady; }, &ready)
                    .ok());
    WXI_REQUIRE_EQ(ready, exactReady);
    winexinfo::ObserverDeadline deadline;
    WXI_REQUIRE(deadline.Start(ready, 1000).ok());
    WXI_REQUIRE_EQ(deadline.deadline(), exactReady + 1000ms);
}

WXI_TEST(
    observer_runtime_wait_uses_absolute_remaining_and_exact_msgwait_flags,
    "observer_runtime.absolute_msgwait") {
    using namespace std::chrono_literals;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    winexinfo::ObserverDeadline deadline;
    WXI_REQUIRE(deadline.Start(ready, 1000).ok());
    std::vector<DWORD> observedWaits;
    DWORD observedWakeMask = 0;
    DWORD observedFlags = 0;
    const std::array<HANDLE, 2> waitHandles{
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        reinterpret_cast<HANDLE>(std::uintptr_t{8}),
    };
    std::size_t nowCall = 0;
    std::size_t waitCall = 0;
    const winexinfo::ObserverWaitOperations operations{
        [&]() {
            ++nowCall;
            return ready + (nowCall == 1 ? 250ms : 300ms);
        },
        [&](const DWORD count,
            const HANDLE* const handles,
            const DWORD wait,
            const DWORD wakeMask,
            const DWORD flags) {
            WXI_REQUIRE_EQ(count, DWORD{2});
            WXI_REQUIRE_EQ(handles[0], waitHandles[0]);
            WXI_REQUIRE_EQ(handles[1], waitHandles[1]);
            observedWaits.push_back(wait);
            observedWakeMask = wakeMask;
            observedFlags = flags;
            ++waitCall;
            return waitCall == 1 ? WAIT_TIMEOUT : WAIT_OBJECT_0 + 1;
        },
        []() { return DWORD{ERROR_SUCCESS}; },
    };
    winexinfo::ObserverWaitResult result{
        winexinfo::ObserverWaitOutcome::DeadlineReached,
        99,
    };

    WXI_REQUIRE(winexinfo::WaitForObserverActivity(
                    deadline, waitHandles, operations, &result)
                    .ok());
    WXI_REQUIRE_EQ(result.outcome, winexinfo::ObserverWaitOutcome::HandleReady);
    WXI_REQUIRE_EQ(result.ready_handle_index, std::size_t{1});
    WXI_REQUIRE_EQ(observedWaits.size(), std::size_t{2});
    WXI_REQUIRE_EQ(observedWaits[0], DWORD{750});
    WXI_REQUIRE_EQ(observedWaits[1], DWORD{700});
    WXI_REQUIRE_EQ(observedWakeMask, DWORD{QS_ALLINPUT});
    WXI_REQUIRE_EQ(observedFlags, DWORD{MWMO_INPUTAVAILABLE});
}

WXI_TEST(
    observer_runtime_wait_rejects_invalid_handles_and_unexpected_apc,
    "observer_runtime.msgwait_invalid_contract") {
    using namespace std::chrono_literals;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    winexinfo::ObserverDeadline deadline;
    WXI_REQUIRE(deadline.Start(ready, 1000).ok());
    std::size_t waitCalls = 0;
    winexinfo::ObserverWaitOperations operations{
        [ready]() { return ready + 100ms; },
        [&waitCalls](DWORD, const HANDLE*, DWORD, DWORD, DWORD) {
            ++waitCalls;
            return WAIT_IO_COMPLETION;
        },
        []() { return DWORD{ERROR_SUCCESS}; },
    };
    winexinfo::ObserverWaitResult output{
        winexinfo::ObserverWaitOutcome::HandleReady,
        77,
    };
    auto result = winexinfo::WaitForObserverActivity(
        deadline,
        std::span<const HANDLE>{},
        operations,
        &output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    const std::array<HANDLE, 1> nullHandle{nullptr};
    result = winexinfo::WaitForObserverActivity(
        deadline, nullHandle, operations, &output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    const std::array<HANDLE, 2> duplicateHandles{
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
    };
    result = winexinfo::WaitForObserverActivity(
        deadline, duplicateHandles, operations, &output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    std::array<HANDLE, MAXIMUM_WAIT_OBJECTS> tooMany{};
    for (std::size_t index = 0; index < tooMany.size(); ++index) {
        tooMany[index] = reinterpret_cast<HANDLE>(index + 1);
    }
    result = winexinfo::WaitForObserverActivity(
        deadline, tooMany, operations, &output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(waitCalls, std::size_t{0});
    WXI_REQUIRE_EQ(output.ready_handle_index, std::size_t{77});

    const std::array<HANDLE, 1> exactHandles{
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
    };
    result = winexinfo::WaitForObserverActivity(
        deadline, exactHandles, operations, &output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(result.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(waitCalls, std::size_t{1});
    WXI_REQUIRE_EQ(output.ready_handle_index, std::size_t{77});
}

WXI_TEST(
    observer_runtime_wait_failed_preserves_exact_origin_and_output,
    "observer_runtime.msgwait_failure_origin") {
    using namespace std::chrono_literals;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    winexinfo::ObserverDeadline deadline;
    WXI_REQUIRE(deadline.Start(ready, 1000).ok());
    DWORD lastError = ERROR_SUCCESS;
    const std::array<HANDLE, 1> handles{
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
    };
    const winexinfo::ObserverWaitOperations operations{
        [ready]() { return ready + 100ms; },
        [](DWORD, const HANDLE*, DWORD, DWORD, DWORD) {
            return WAIT_FAILED;
        },
        [&lastError]() { return lastError; },
    };
    winexinfo::ObserverWaitResult output{
        winexinfo::ObserverWaitOutcome::HandleReady,
        77,
    };
    auto result = winexinfo::WaitForObserverActivity(
        deadline, handles, operations, &output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(result.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(result.status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(output.ready_handle_index, std::size_t{77});

    lastError = ERROR_ACCESS_DENIED;
    result = winexinfo::WaitForObserverActivity(
        deadline, handles, operations, &output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Transport});
    WXI_REQUIRE_EQ(
        result.status.hresult,
        HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    WXI_REQUIRE_EQ(result.status.win32, DWORD{ERROR_ACCESS_DENIED});
    WXI_REQUIRE_EQ(output.ready_handle_index, std::size_t{77});
}

WXI_TEST(
    observer_runtime_connection_point_results_require_exact_success,
    "observer_runtime.connection_point_exact_results") {
    WXI_REQUIRE(winexinfo::ClassifyObserverConnectionPointResult(S_OK, true).ok());
    const auto noInterface =
        winexinfo::ClassifyObserverConnectionPointResult(E_NOINTERFACE, true);
    WXI_REQUIRE_EQ(
        noInterface.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(noInterface.status.hresult, E_NOINTERFACE);
    const auto disconnected = winexinfo::ClassifyObserverConnectionPointResult(
        RPC_E_DISCONNECTED, true);
    WXI_REQUIRE_EQ(
        disconnected.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Transport});
    WXI_REQUIRE(winexinfo::ClassifyObserverAdviseResult(S_OK, 1).ok());
    const auto zeroCookie = winexinfo::ClassifyObserverAdviseResult(S_OK, 0);
    WXI_REQUIRE_EQ(
        zeroCookie.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE(winexinfo::ClassifyObserverUnadviseResult(S_OK).ok());
    const auto noConnection =
        winexinfo::ClassifyObserverUnadviseResult(CONNECT_E_NOCONNECTION);
    WXI_REQUIRE_EQ(noConnection.status.hresult, CONNECT_E_NOCONNECTION);
    WXI_REQUIRE_EQ(
        noConnection.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
}

WXI_TEST(
    observer_runtime_advise_rejects_nonempty_output_before_side_effect,
    "observer_runtime.advise_output_transaction") {
    winexinfo::ObserverConnectionPointRegistration registration{};
    registration.subscription_cookie = 99;
    registration.owner_thread_id = 88;
    const auto result = winexinfo::AdviseObserverConnectionPoint(
        reinterpret_cast<IUnknown*>(std::uintptr_t{1}),
        DIID_DShellWindowsEvents,
        reinterpret_cast<IUnknown*>(std::uintptr_t{2}),
        &registration);
    WXI_REQUIRE_EQ(result.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(registration.subscription_cookie, DWORD{99});
    WXI_REQUIRE_EQ(registration.owner_thread_id, DWORD{88});
}

WXI_TEST(
    observer_runtime_connection_point_advise_unadvise_and_wrong_thread,
    "observer_runtime.connection_point_lifetime") {
    FakeConnectionSource source;
    FakeDispatch sink;
    winexinfo::ObserverConnectionPointRegistration registration{};
    const auto advised = winexinfo::AdviseObserverConnectionPoint(
        static_cast<IConnectionPointContainer*>(&source),
        DIID_DShellWindowsEvents,
        &sink,
        &registration);
    WXI_REQUIRE(advised.ok());
    WXI_REQUIRE_EQ(source.query_calls, std::size_t{1});
    WXI_REQUIRE_EQ(source.find_calls, std::size_t{1});
    WXI_REQUIRE_EQ(source.advise_calls, std::size_t{1});
    WXI_REQUIRE_EQ(source.observed_sink, static_cast<IUnknown*>(&sink));
    WXI_REQUIRE_EQ(registration.subscription_cookie, DWORD{17});
    WXI_REQUIRE_EQ(registration.owner_thread_id, GetCurrentThreadId());

    winexinfo::ObserverOperationResult wrongThread = RuntimeOperationSuccess();
    std::thread worker([&]() {
        wrongThread =
            winexinfo::UnadviseObserverConnectionPoint(&registration);
    });
    worker.join();
    WXI_REQUIRE_EQ(wrongThread.status.hresult, RPC_E_WRONG_THREAD);
    WXI_REQUIRE_EQ(
        wrongThread.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(source.unadvise_calls, std::size_t{0});
    WXI_REQUIRE_EQ(registration.subscription_cookie, DWORD{17});

    source.unadvise_hresult = CONNECT_E_NOCONNECTION;
    const auto firstRemoval =
        winexinfo::UnadviseObserverConnectionPoint(&registration);
    WXI_REQUIRE_EQ(firstRemoval.status.hresult, CONNECT_E_NOCONNECTION);
    WXI_REQUIRE_EQ(
        firstRemoval.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(source.unadvise_calls, std::size_t{1});
    WXI_REQUIRE_EQ(source.observed_unadvise_cookie, DWORD{17});
    WXI_REQUIRE_EQ(registration.subscription_cookie, DWORD{17});
    WXI_REQUIRE(registration.connection_point != nullptr);
    WXI_REQUIRE(registration.sink != nullptr);

    source.unadvise_hresult = S_OK;
    const auto retried =
        winexinfo::UnadviseObserverConnectionPoint(&registration);
    WXI_REQUIRE(retried.ok());
    WXI_REQUIRE_EQ(source.unadvise_calls, std::size_t{2});
    WXI_REQUIRE(registration.connection_point == nullptr);
    WXI_REQUIRE(registration.sink == nullptr);
    WXI_REQUIRE_EQ(registration.subscription_cookie, DWORD{0});
    WXI_REQUIRE_EQ(registration.owner_thread_id, DWORD{0});
    WXI_REQUIRE_EQ(source.references, ULONG{1});
}

WXI_TEST(
    observer_runtime_connection_point_failed_advise_discards_cookie_and_object,
    "observer_runtime.connection_point_failed_output") {
    FakeConnectionSource source;
    source.advise_hresult = RPC_E_DISCONNECTED;
    source.advise_cookie = 99;
    FakeDispatch sink;
    const ULONG sinkReferences = sink.references();
    winexinfo::ObserverConnectionPointRegistration registration{};
    const auto result = winexinfo::AdviseObserverConnectionPoint(
        static_cast<IConnectionPointContainer*>(&source),
        DIID_DShellWindowsEvents,
        &sink,
        &registration);
    WXI_REQUIRE_EQ(result.status.hresult, RPC_E_DISCONNECTED);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Transport});
    WXI_REQUIRE(registration.connection_point == nullptr);
    WXI_REQUIRE(registration.sink == nullptr);
    WXI_REQUIRE_EQ(registration.subscription_cookie, DWORD{0});
    WXI_REQUIRE_EQ(registration.owner_thread_id, DWORD{0});
    WXI_REQUIRE_EQ(source.references, ULONG{1});
    WXI_REQUIRE_EQ(sink.references(), sinkReferences);
}

WXI_TEST(
    observer_runtime_shell_sink_query_interface_has_exact_same_identity,
    "observer_runtime.shell_sink_query_interface") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    Microsoft::WRL::ComPtr<IDispatch> lifecycle;
    WXI_REQUIRE(
        winexinfo::CreateShellLifecycleEventSink(&queue, lifecycle).ok());
    void* unknown = nullptr;
    void* dispatchIdentity = nullptr;
    void* outgoing = nullptr;
    WXI_REQUIRE_EQ(
        lifecycle->QueryInterface(IID_IUnknown, &unknown),
        S_OK);
    WXI_REQUIRE_EQ(
        lifecycle->QueryInterface(IID_IDispatch, &dispatchIdentity),
        S_OK);
    WXI_REQUIRE_EQ(
        lifecycle->QueryInterface(DIID_DShellWindowsEvents, &outgoing),
        S_OK);
    WXI_REQUIRE_EQ(unknown, static_cast<void*>(lifecycle.Get()));
    WXI_REQUIRE_EQ(dispatchIdentity, static_cast<void*>(lifecycle.Get()));
    WXI_REQUIRE_EQ(outgoing, static_cast<void*>(lifecycle.Get()));
    static_cast<IUnknown*>(unknown)->Release();
    static_cast<IUnknown*>(dispatchIdentity)->Release();
    static_cast<IUnknown*>(outgoing)->Release();
    void* unsupported = reinterpret_cast<void*>(std::uintptr_t{1});
    WXI_REQUIRE_EQ(
        lifecycle->QueryInterface(DIID_DWebBrowserEvents2, &unsupported),
        E_NOINTERFACE);
    WXI_REQUIRE(unsupported == nullptr);

    FakeDispatch source;
    Microsoft::WRL::ComPtr<IDispatch> browser;
    WXI_REQUIRE(winexinfo::CreateBrowserNavigateEventSink(
                    &queue,
                    &source,
                    Handle(0x100),
                    7,
                    Handle(0x200),
                    browser)
                    .ok());
    outgoing = nullptr;
    unknown = nullptr;
    dispatchIdentity = nullptr;
    WXI_REQUIRE_EQ(browser->QueryInterface(IID_IUnknown, &unknown), S_OK);
    WXI_REQUIRE_EQ(
        browser->QueryInterface(IID_IDispatch, &dispatchIdentity),
        S_OK);
    WXI_REQUIRE_EQ(
        browser->QueryInterface(DIID_DWebBrowserEvents2, &outgoing),
        S_OK);
    WXI_REQUIRE_EQ(outgoing, static_cast<void*>(browser.Get()));
    WXI_REQUIRE_EQ(unknown, static_cast<void*>(browser.Get()));
    WXI_REQUIRE_EQ(dispatchIdentity, static_cast<void*>(browser.Get()));
    static_cast<IUnknown*>(unknown)->Release();
    static_cast<IUnknown*>(dispatchIdentity)->Release();
    static_cast<IUnknown*>(outgoing)->Release();
    unsupported = reinterpret_cast<void*>(std::uintptr_t{1});
    WXI_REQUIRE_EQ(
        browser->QueryInterface(DIID_DShellWindowsEvents, &unsupported),
        E_NOINTERFACE);
    WXI_REQUIRE(unsupported == nullptr);
}

WXI_TEST(
    observer_runtime_sink_factories_preserve_nonempty_output_and_reject_shape,
    "observer_runtime.shell_sink_factory_transaction") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    FakeDispatch sentinel;
    Microsoft::WRL::ComPtr<IDispatch> output = &sentinel;
    auto result = winexinfo::CreateShellLifecycleEventSink(&queue, output);
    WXI_REQUIRE_EQ(
        result.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(output.Get(), static_cast<IDispatch*>(&sentinel));

    Microsoft::WRL::ComPtr<IDispatch> empty;
    result = winexinfo::CreateShellLifecycleEventSink(nullptr, empty);
    WXI_REQUIRE_EQ(result.status.hresult, E_INVALIDARG);
    WXI_REQUIRE(empty == nullptr);
    result = winexinfo::CreateBrowserNavigateEventSink(
        &queue,
        &sentinel,
        Handle(0x100),
        0,
        Handle(0x200),
        empty);
    WXI_REQUIRE_EQ(result.status.hresult, E_INVALIDARG);
    WXI_REQUIRE(empty == nullptr);
}

WXI_TEST(
    observer_runtime_shell_sink_valid_and_unsupported_callbacks_close_tickets,
    "observer_runtime.shell_sink_invoke") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    VARIANT argument{};
    argument.vt = VT_I4;
    argument.lVal = 73;
    DISPPARAMS parameters{&argument, nullptr, 1, 0};
    WXI_REQUIRE_EQ(
        sink->Invoke(
            DISPID_WINDOWREGISTERED,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            &parameters,
            nullptr,
            nullptr,
            nullptr),
        S_OK);
    WXI_REQUIRE_EQ(
        sink->Invoke(
            999,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            nullptr,
            nullptr,
            nullptr,
            nullptr),
        S_OK);
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{1});
    WXI_REQUIRE_EQ(
        drained[0].payload.source,
        winexinfo::ObserverCallbackSource::ShellLifecycle);
    WXI_REQUIRE_EQ(
        drained[0].payload.kind,
        winexinfo::ObservedEventKind::WindowRegistered);
    WXI_REQUIRE_EQ(drained[0].payload.shell_cookie, LONG{73});
    WXI_REQUIRE_EQ(queue.ignored_event_count(), std::size_t{1});
}

WXI_TEST(
    observer_runtime_shell_sink_malformed_callback_returns_exact_dispatch_error,
    "observer_runtime.shell_sink_malformed") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    WXI_REQUIRE_EQ(
        sink->Invoke(
            DISPID_WINDOWREGISTERED,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            nullptr,
            nullptr,
            nullptr,
            nullptr),
        DISP_E_BADPARAMCOUNT);
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE_EQ(
        emergency->first_failure.status.hresult,
        DISP_E_BADPARAMCOUNT);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, std::uint64_t{1});
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE(drained.empty());
}

WXI_TEST(
    observer_runtime_browser_sink_admits_before_qi_and_hides_internal_failure,
    "observer_runtime.browser_sink_admit_first") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> order;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.reserve = [&order](
                             std::deque<winexinfo::ObserverCallbackSlot>* const queue,
                             const std::uint64_t sequence) {
        order.push_back(1);
        queue->push_back({
            sequence,
            winexinfo::ObserverCallbackSlotState::Pending,
            std::nullopt,
            std::nullopt,
        });
    };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());
    FakeDispatch registeredSource;
    FakeDispatch wrongSource(&order);
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateBrowserNavigateEventSink(
                    &queue,
                    &registeredSource,
                    Handle(0x100),
                    7,
                    Handle(0x200),
                    sink)
                    .ok());
    VARIANT url{};
    VARIANT arguments[2]{};
    arguments[0].vt = VT_BYREF | VT_VARIANT;
    arguments[0].pvarVal = &url;
    arguments[1].vt = VT_DISPATCH;
    arguments[1].pdispVal = &wrongSource;
    DISPPARAMS parameters{arguments, nullptr, 2, 0};
    WXI_REQUIRE_EQ(
        sink->Invoke(
            DISPID_NAVIGATECOMPLETE2,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            &parameters,
            nullptr,
            nullptr,
            nullptr),
        S_OK);
    WXI_REQUIRE_EQ(order.size(), std::size_t{2});
    WXI_REQUIRE_EQ(order[0], 1);
    WXI_REQUIRE_EQ(order[1], 2);
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, std::uint64_t{1});
}

WXI_TEST(
    observer_runtime_browser_sink_valid_payload_malformed_and_unrelated_paths,
    "observer_runtime.browser_sink_invoke") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    FakeDispatch source;
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateBrowserNavigateEventSink(
                    &queue,
                    &source,
                    Handle(0x100),
                    7,
                    Handle(0x200),
                    sink)
                    .ok());
    VARIANT url{};
    VARIANT arguments[2]{};
    arguments[0].vt = VT_BYREF | VT_VARIANT;
    arguments[0].pvarVal = &url;
    arguments[1].vt = VT_DISPATCH;
    arguments[1].pdispVal = &source;
    DISPPARAMS parameters{arguments, nullptr, 2, 0};
    WXI_REQUIRE_EQ(
        sink->Invoke(
            DISPID_NAVIGATECOMPLETE2,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            &parameters,
            nullptr,
            nullptr,
            nullptr),
        S_OK);
    WXI_REQUIRE_EQ(
        sink->Invoke(
            999,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            nullptr,
            nullptr,
            nullptr,
            nullptr),
        S_OK);
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{1});
    WXI_REQUIRE_EQ(
        drained[0].payload.source,
        winexinfo::ObserverCallbackSource::BrowserNavigate);
    WXI_REQUIRE_EQ(
        drained[0].payload.kind,
        winexinfo::ObservedEventKind::NavigateComplete2);
    WXI_REQUIRE_EQ(drained[0].payload.top_level, Handle(0x100));
    WXI_REQUIRE_EQ(drained[0].payload.generation, std::uint64_t{7});
    WXI_REQUIRE_EQ(drained[0].payload.shell_tab, Handle(0x200));
    WXI_REQUIRE_EQ(queue.ignored_event_count(), std::size_t{1});

    DWORD secondLastError = ERROR_SUCCESS;
    std::vector<int> secondWakeCalls;
    std::vector<int> secondFallbackCalls;
    winexinfo::ObserverCallbackQueue malformedQueue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(
            &secondLastError,
            &secondWakeCalls,
            &secondFallbackCalls));
    WXI_REQUIRE(StartQueue(&malformedQueue).ok());
    Microsoft::WRL::ComPtr<IDispatch> malformedSink;
    WXI_REQUIRE(winexinfo::CreateBrowserNavigateEventSink(
                    &malformedQueue,
                    &source,
                    Handle(0x100),
                    7,
                    Handle(0x200),
                    malformedSink)
                    .ok());
    WXI_REQUIRE_EQ(
        malformedSink->Invoke(
            DISPID_NAVIGATECOMPLETE2,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            nullptr,
            nullptr,
            nullptr,
            nullptr),
        DISP_E_BADPARAMCOUNT);
}

WXI_TEST(
    observer_runtime_valid_sink_callback_hides_wake_failure_and_closes_ticket,
    "observer_runtime.shell_sink_wake_failure") {
    DWORD lastError = ERROR_ACCESS_DENIED;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    auto operations = QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    operations.set_event = [](const HANDLE) { return FALSE; };
    operations.post_emergency = [&fallbackCalls]() {
        fallbackCalls.push_back(1);
        return FALSE;
    };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}), std::move(operations));
    WXI_REQUIRE(StartQueue(&queue).ok());
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    VARIANT argument{};
    argument.vt = VT_I4;
    argument.lVal = 73;
    DISPPARAMS parameters{&argument, nullptr, 1, 0};
    WXI_REQUIRE_EQ(
        sink->Invoke(
            DISPID_WINDOWREGISTERED,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            &parameters,
            nullptr,
            nullptr,
            nullptr),
        S_OK);
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE(emergency->any_transport_failure);
    std::vector<winexinfo::ObserverCallbackEnvelope> drained;
    WXI_REQUIRE(queue.Drain(&drained).ok());
    WXI_REQUIRE_EQ(drained.size(), std::size_t{1});
    bool quiescent = false;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_startup_callback_is_fatal_before_payload_inspection,
    "observer_runtime.shell_sink_startup_callback") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    WXI_REQUIRE_EQ(
        sink->Invoke(
            DISPID_WINDOWREGISTERED,
            IID_NULL,
            0,
            DISPATCH_METHOD,
            nullptr,
            nullptr,
            nullptr,
            nullptr),
        S_OK);
    const auto emergency = queue.emergency();
    WXI_REQUIRE(emergency.has_value());
    WXI_REQUIRE_EQ(
        emergency->first_failure.origin,
        winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE_EQ(emergency->fatal_raw_sequence, std::uint64_t{1});
}

WXI_TEST(
    observer_runtime_shell_startup_is_lifecycle_first_double_capture_and_reverse_cleanup,
    "observer_runtime.shell_startup_order") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const std::vector<winexinfo::ObserverShellEntryMetadata> first{
        {11, true, Handle(0x100), Handle(0x101)},
        {22, false, Handle(0x200), nullptr},
    };
    const std::vector<winexinfo::ObserverShellEntryMetadata> second{
        first[1], first[0]};
    std::vector<std::uint64_t> order;
    std::size_t captureCall = 0;
    const winexinfo::ObserverShellStartupOperations operations{
        [&order](std::uint64_t* const registrationId) {
            order.push_back(1);
            *registrationId = 100;
            return RuntimeOperationSuccess();
        },
        [&](std::vector<winexinfo::ObserverShellEntryMetadata>* const output) {
            order.push_back(captureCall == 0 ? 2 : 4);
            *output = captureCall++ == 0 ? first : second;
            return RuntimeOperationSuccess();
        },
        [&order](
            const winexinfo::ObserverShellEntryMetadata& entry,
            std::uint64_t* const registrationId) {
            order.push_back(3);
            WXI_REQUIRE_EQ(entry.canonical_identity, std::uintptr_t{11});
            *registrationId = 200;
            return RuntimeOperationSuccess();
        },
        [&order](const std::uint64_t registrationId) {
            order.push_back(registrationId);
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    WXI_REQUIRE(winexinfo::StartObserverShellSubscriptions(
                    &queue, operations, &state, &outcome)
                    .ok());
    WXI_REQUIRE(outcome.setup.ok());
    WXI_REQUIRE(outcome.rollback.status.ok());
    const std::vector<std::uint64_t> expectedStart{1, 2, 3, 4};
    WXI_REQUIRE_EQ(order, expectedStart);
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{100});
    WXI_REQUIRE_EQ(state.browser_registrations.size(), std::size_t{1});
    WXI_REQUIRE_EQ(
        state.browser_registrations[0],
        (winexinfo::ObserverShellBrowserRegistration{11, 200}));
    WXI_REQUIRE_EQ(state.baseline, second);

    const auto cleanup =
        winexinfo::CleanupObserverShellSubscriptions(operations, &state);
    WXI_REQUIRE(cleanup.status.ok());
    const std::vector<std::uint64_t> expectedAll{1, 2, 3, 4, 200, 100};
    WXI_REQUIRE_EQ(order, expectedAll);
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{0});
    WXI_REQUIRE(state.browser_registrations.empty());
    WXI_REQUIRE(state.baseline.empty());
}

WXI_TEST(
    observer_runtime_shell_startup_rolls_back_reverse_on_unstable_second_capture,
    "observer_runtime.shell_startup_rollback") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const std::vector<winexinfo::ObserverShellEntryMetadata> first{
        {11, true, Handle(0x100), Handle(0x101)},
        {22, true, Handle(0x200), Handle(0x201)},
    };
    auto unstable = first;
    unstable[0].shell_tab = Handle(0x102);
    std::vector<std::uint64_t> order;
    std::size_t captureCall = 0;
    const winexinfo::ObserverShellStartupOperations operations{
        [&order](std::uint64_t* const registrationId) {
            order.push_back(1);
            *registrationId = 100;
            return RuntimeOperationSuccess();
        },
        [&](std::vector<winexinfo::ObserverShellEntryMetadata>* const output) {
            order.push_back(captureCall == 0 ? 2 : 5);
            *output = captureCall++ == 0 ? first : unstable;
            return RuntimeOperationSuccess();
        },
        [&order](
            const winexinfo::ObserverShellEntryMetadata& entry,
            std::uint64_t* const registrationId) {
            order.push_back(entry.canonical_identity == 11 ? 3 : 4);
            *registrationId = entry.canonical_identity == 11 ? 201 : 202;
            return RuntimeOperationSuccess();
        },
        [&order](const std::uint64_t registrationId) {
            order.push_back(registrationId);
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(
        outcome.setup.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE(outcome.rollback.status.ok());
    const std::vector<std::uint64_t> expected{
        1, 2, 3, 4, 5, 202, 201, 100};
    WXI_REQUIRE_EQ(order, expected);
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{0});
    WXI_REQUIRE(state.browser_registrations.empty());
    WXI_REQUIRE(state.baseline.empty());
}

WXI_TEST(
    observer_runtime_shell_startup_callback_aborts_and_cleanup_failure_is_retained,
    "observer_runtime.shell_startup_callback_cleanup") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    std::vector<std::uint64_t> order;
    bool cleanupFails = true;
    const winexinfo::ObserverShellStartupOperations operations{
        [&](std::uint64_t* const registrationId) {
            order.push_back(1);
            *registrationId = 100;
            VARIANT argument{};
            argument.vt = VT_I4;
            argument.lVal = 73;
            DISPPARAMS parameters{&argument, nullptr, 1, 0};
            WXI_REQUIRE_EQ(
                sink->Invoke(
                    DISPID_WINDOWREGISTERED,
                    IID_NULL,
                    0,
                    DISPATCH_METHOD,
                    &parameters,
                    nullptr,
                    nullptr,
                    nullptr),
                S_OK);
            return RuntimeOperationSuccess();
        },
        [&order](std::vector<winexinfo::ObserverShellEntryMetadata>*) {
            order.push_back(2);
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            return RuntimeOperationSuccess();
        },
        [&](const std::uint64_t registrationId) {
            order.push_back(registrationId);
            if (cleanupFails) {
                return RuntimeOperationFailure(
                    winexinfo::ObserverFailureOrigin::Transport,
                    RPC_E_DISCONNECTED);
            }
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(order, (std::vector<std::uint64_t>{1, 100}));
    WXI_REQUIRE_EQ(
        outcome.setup.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(
        outcome.rollback.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Transport});
    WXI_REQUIRE(outcome.rollback.any_transport_failure);
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{100});

    cleanupFails = false;
    const auto retried =
        winexinfo::CleanupObserverShellSubscriptions(operations, &state);
    WXI_REQUIRE(retried.status.ok());
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{0});
}

WXI_TEST(
    observer_runtime_shell_startup_partial_browser_failure_rolls_back_prior_only,
    "observer_runtime.shell_startup_partial_advise") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const std::vector<winexinfo::ObserverShellEntryMetadata> capture{
        {11, true, Handle(0x100), Handle(0x101)},
        {22, true, Handle(0x200), Handle(0x201)},
    };
    std::vector<std::uint64_t> order;
    const winexinfo::ObserverShellStartupOperations operations{
        [&order](std::uint64_t* const registrationId) {
            order.push_back(1);
            *registrationId = 100;
            return RuntimeOperationSuccess();
        },
        [&order, &capture](
            std::vector<winexinfo::ObserverShellEntryMetadata>* const output) {
            order.push_back(2);
            *output = capture;
            return RuntimeOperationSuccess();
        },
        [&order](
            const winexinfo::ObserverShellEntryMetadata& entry,
            std::uint64_t* const registrationId) {
            order.push_back(entry.canonical_identity);
            if (entry.canonical_identity == 22) {
                return RuntimeOperationFailure(
                    winexinfo::ObserverFailureOrigin::Contract,
                    CONNECT_E_CANNOTCONNECT);
            }
            *registrationId = 201;
            return RuntimeOperationSuccess();
        },
        [&order](const std::uint64_t registrationId) {
            order.push_back(registrationId);
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        CONNECT_E_CANNOTCONNECT);
    const std::vector<std::uint64_t> expected{1, 2, 11, 22, 201, 100};
    WXI_REQUIRE_EQ(order, expected);
    WXI_REQUIRE_EQ(
        outcome.setup.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE(outcome.rollback.status.ok());
    WXI_REQUIRE(state.browser_registrations.empty());
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{0});
}

WXI_TEST(
    observer_runtime_shell_startup_rejects_zero_duplicate_and_nontarget_shapes,
    "observer_runtime.shell_startup_shapes") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue zeroQueue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    std::size_t sideEffects = 0;
    const winexinfo::ObserverShellStartupOperations zeroLifecycle{
        [&sideEffects](std::uint64_t*) {
            ++sideEffects;
            return RuntimeOperationSuccess();
        },
        [](std::vector<winexinfo::ObserverShellEntryMetadata>*) {
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            return RuntimeOperationSuccess();
        },
        [&sideEffects](std::uint64_t) {
            ++sideEffects;
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &zeroQueue, zeroLifecycle, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(sideEffects, std::size_t{1});

    DWORD duplicateLastError = ERROR_SUCCESS;
    std::vector<int> duplicateWakeCalls;
    std::vector<int> duplicateFallbackCalls;
    winexinfo::ObserverCallbackQueue duplicateQueue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(
            &duplicateLastError,
            &duplicateWakeCalls,
            &duplicateFallbackCalls));
    const std::vector<winexinfo::ObserverShellEntryMetadata> targets{
        {11, true, Handle(0x100), Handle(0x101)},
        {22, true, Handle(0x200), Handle(0x201)},
    };
    std::vector<std::uint64_t> cleaned;
    const winexinfo::ObserverShellStartupOperations duplicateIds{
        [](std::uint64_t* const registrationId) {
            *registrationId = 100;
            return RuntimeOperationSuccess();
        },
        [&targets](
            std::vector<winexinfo::ObserverShellEntryMetadata>* const output) {
            *output = targets;
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t* const id) {
            *id = 200;
            return RuntimeOperationSuccess();
        },
        [&cleaned](const std::uint64_t id) {
            cleaned.push_back(id);
            return RuntimeOperationSuccess();
        },
    };
    state = {};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &duplicateQueue, duplicateIds, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(cleaned, (std::vector<std::uint64_t>{200, 100}));
    WXI_REQUIRE(state.browser_registrations.empty());

    DWORD nonTargetLastError = ERROR_SUCCESS;
    std::vector<int> nonTargetWakeCalls;
    std::vector<int> nonTargetFallbackCalls;
    winexinfo::ObserverCallbackQueue nonTargetQueue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(
            &nonTargetLastError,
            &nonTargetWakeCalls,
            &nonTargetFallbackCalls));
    const winexinfo::ObserverShellStartupOperations nonTargetOnly{
        [](std::uint64_t* const registrationId) {
            *registrationId = 100;
            return RuntimeOperationSuccess();
        },
        [](std::vector<winexinfo::ObserverShellEntryMetadata>* const output) {
            *output = {{22, false, Handle(0x200), nullptr}};
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            return RuntimeOperationSuccess();
        },
        [](std::uint64_t) { return RuntimeOperationSuccess(); },
    };
    state = {};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &nonTargetQueue, nonTargetOnly, &state, &outcome),
        S_FALSE);
}

WXI_TEST(
    observer_runtime_shell_startup_accepts_empty_stable_baseline,
    "observer_runtime.shell_startup_empty_baseline") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    std::size_t captureCalls = 0;
    std::vector<std::uint64_t> cleanup;
    const winexinfo::ObserverShellStartupOperations operations{
        [](std::uint64_t* const registrationId) {
            *registrationId = 100;
            return RuntimeOperationSuccess();
        },
        [&captureCalls](
            std::vector<winexinfo::ObserverShellEntryMetadata>* const output) {
            ++captureCalls;
            WXI_REQUIRE(output->empty());
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            WXI_REQUIRE(false);
            return RuntimeOperationSuccess();
        },
        [&cleanup](const std::uint64_t registrationId) {
            cleanup.push_back(registrationId);
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    WXI_REQUIRE(winexinfo::StartObserverShellSubscriptions(
                    &queue, operations, &state, &outcome)
                    .ok());
    WXI_REQUIRE_EQ(captureCalls, std::size_t{2});
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{100});
    WXI_REQUIRE(state.browser_registrations.empty());
    WXI_REQUIRE(state.baseline.empty());
    WXI_REQUIRE(winexinfo::CleanupObserverShellSubscriptions(
                    operations, &state)
                    .status
                    .ok());
    WXI_REQUIRE_EQ(cleanup, (std::vector<std::uint64_t>{100}));
}

WXI_TEST(
    observer_runtime_shell_cleanup_continues_and_preserves_first_plus_transport,
    "observer_runtime.shell_cleanup_partial") {
    std::vector<std::uint64_t> calls;
    const winexinfo::ObserverShellStartupOperations operations{
        {},
        {},
        {},
        [&calls](const std::uint64_t id) {
            calls.push_back(id);
            if (id == 203) {
                return RuntimeOperationFailure(
                    winexinfo::ObserverFailureOrigin::Contract,
                    CONNECT_E_NOCONNECTION);
            }
            if (id == 201) {
                return RuntimeOperationFailure(
                    winexinfo::ObserverFailureOrigin::Transport,
                    RPC_E_DISCONNECTED);
            }
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{
        100,
        {{11, 201}, {22, 202}, {33, 203}},
        {{11, true, Handle(0x100), Handle(0x101)}},
    };
    const auto cleanup =
        winexinfo::CleanupObserverShellSubscriptions(operations, &state);
    WXI_REQUIRE_EQ(calls, (std::vector<std::uint64_t>{203, 202, 201, 100}));
    WXI_REQUIRE_EQ(cleanup.status.hresult, CONNECT_E_NOCONNECTION);
    WXI_REQUIRE_EQ(
        cleanup.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE(cleanup.any_transport_failure);
    WXI_REQUIRE_EQ(
        state.browser_registrations,
        (std::vector<winexinfo::ObserverShellBrowserRegistration>{
            {11, 201}, {33, 203}}));
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{0});
    WXI_REQUIRE(state.baseline.empty());
}

WXI_TEST(
    observer_runtime_shell_startup_stops_admission_before_rollback,
    "observer_runtime.shell_startup_stop_before_rollback") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    const std::vector<winexinfo::ObserverShellEntryMetadata> baseline{
        {11, true, Handle(0x100), Handle(0x101)},
    };
    std::size_t captureCall = 0;
    const winexinfo::ObserverShellStartupOperations operations{
        [](std::uint64_t* const id) {
            *id = 100;
            return RuntimeOperationSuccess();
        },
        [&](std::vector<winexinfo::ObserverShellEntryMetadata>* const output) {
            *output = baseline;
            if (++captureCall == 2) {
                output->front().shell_tab = Handle(0x102);
            }
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t* const id) {
            *id = 200;
            return RuntimeOperationSuccess();
        },
        [&sink](std::uint64_t) {
            VARIANT argument{};
            argument.vt = VT_I4;
            argument.lVal = 73;
            DISPPARAMS parameters{&argument, nullptr, 1, 0};
            WXI_REQUIRE_EQ(
                sink->Invoke(
                    DISPID_WINDOWREGISTERED,
                    IID_NULL,
                    0,
                    DISPATCH_METHOD,
                    &parameters,
                    nullptr,
                    nullptr,
                    nullptr),
                S_OK);
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE(!queue.emergency().has_value());
    WXI_REQUIRE_EQ(queue.late_event_count(), std::size_t{2});
}

WXI_TEST(
    observer_runtime_shell_startup_reentrant_callback_precedes_operation_failure,
    "observer_runtime.shell_startup_first_tuple") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    std::size_t captureCalls = 0;
    const winexinfo::ObserverShellStartupOperations operations{
        [&sink](std::uint64_t*) {
            VARIANT argument{};
            argument.vt = VT_I4;
            argument.lVal = 73;
            DISPPARAMS parameters{&argument, nullptr, 1, 0};
            WXI_REQUIRE_EQ(
                sink->Invoke(
                    DISPID_WINDOWREGISTERED,
                    IID_NULL,
                    0,
                    DISPATCH_METHOD,
                    &parameters,
                    nullptr,
                    nullptr,
                    nullptr),
                S_OK);
            return RuntimeOperationFailure(
                winexinfo::ObserverFailureOrigin::Transport,
                RPC_E_DISCONNECTED);
        },
        [&captureCalls](std::vector<winexinfo::ObserverShellEntryMetadata>*) {
            ++captureCalls;
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            return RuntimeOperationSuccess();
        },
        [](std::uint64_t) { return RuntimeOperationSuccess(); },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(captureCalls, std::size_t{0});
    WXI_REQUIRE_EQ(
        outcome.setup.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE(outcome.any_setup_transport_failure);
    WXI_REQUIRE_EQ(
        outcome.first_setup_transport_status.hresult,
        RPC_E_DISCONNECTED);
}

WXI_TEST(
    observer_runtime_shell_startup_queue_transport_precedes_return_transport,
    "observer_runtime.shell_startup_transport_order") {
    DWORD lastError = ERROR_ACCESS_DENIED;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    auto queueOperations =
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls);
    queueOperations.set_event = [](const HANDLE) { return FALSE; };
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        std::move(queueOperations));
    Microsoft::WRL::ComPtr<IDispatch> sink;
    WXI_REQUIRE(winexinfo::CreateShellLifecycleEventSink(&queue, sink).ok());
    const winexinfo::ObserverShellStartupOperations operations{
        [&sink](std::uint64_t*) {
            VARIANT argument{};
            argument.vt = VT_I4;
            argument.lVal = 73;
            DISPPARAMS parameters{&argument, nullptr, 1, 0};
            WXI_REQUIRE_EQ(
                sink->Invoke(
                    DISPID_WINDOWREGISTERED,
                    IID_NULL,
                    0,
                    DISPATCH_METHOD,
                    &parameters,
                    nullptr,
                    nullptr,
                    nullptr),
                S_OK);
            return RuntimeOperationFailure(
                winexinfo::ObserverFailureOrigin::Transport,
                RPC_E_DISCONNECTED);
        },
        [](std::vector<winexinfo::ObserverShellEntryMetadata>*) {
            return RuntimeOperationSuccess();
        },
        [](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            return RuntimeOperationSuccess();
        },
        [](std::uint64_t) { return RuntimeOperationSuccess(); },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE(outcome.any_setup_transport_failure);
    WXI_REQUIRE_EQ(
        outcome.first_setup_transport_status.hresult,
        HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    WXI_REQUIRE_EQ(
        outcome.first_setup_transport_status.win32,
        DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(
    observer_runtime_shell_startup_rejects_nonstarting_queue_before_side_effect,
    "observer_runtime.shell_startup_queue_phase") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{1}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    WXI_REQUIRE(StartQueue(&queue).ok());
    std::size_t calls = 0;
    const winexinfo::ObserverShellStartupOperations operations{
        [&calls](std::uint64_t* const id) {
            ++calls;
            *id = 100;
            return RuntimeOperationSuccess();
        },
        [&calls](std::vector<winexinfo::ObserverShellEntryMetadata>* const out) {
            ++calls;
            *out = {{11, true, Handle(0x100), Handle(0x101)}};
            return RuntimeOperationSuccess();
        },
        [&calls](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            ++calls;
            return RuntimeOperationSuccess();
        },
        [&calls](std::uint64_t) {
            ++calls;
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{
        RuntimeOperationFailure(
            winexinfo::ObserverFailureOrigin::Transport,
            E_OUTOFMEMORY),
        true,
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_OUTOFMEMORY,
            ERROR_SUCCESS,
        },
        {
            {
                winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                E_OUTOFMEMORY,
                ERROR_SUCCESS,
            },
            winexinfo::ObserverFailureOrigin::Transport,
            true,
        },
    };
    const auto unchanged = outcome;
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(calls, std::size_t{0});
    WXI_REQUIRE_EQ(outcome.setup.status.hresult, unchanged.setup.status.hresult);
    WXI_REQUIRE_EQ(
        outcome.any_setup_transport_failure,
        unchanged.any_setup_transport_failure);
    WXI_REQUIRE_EQ(
        outcome.first_setup_transport_status.code,
        unchanged.first_setup_transport_status.code);
    WXI_REQUIRE_EQ(
        outcome.first_setup_transport_status.hresult,
        unchanged.first_setup_transport_status.hresult);
    WXI_REQUIRE_EQ(
        outcome.first_setup_transport_status.win32,
        unchanged.first_setup_transport_status.win32);
    WXI_REQUIRE_EQ(
        outcome.rollback.status.hresult,
        unchanged.rollback.status.hresult);
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{0});
    WXI_REQUIRE(state.browser_registrations.empty());
    WXI_REQUIRE(state.baseline.empty());
}

WXI_TEST(
    observer_runtime_shell_cleanup_rejects_lifecycle_token_collision,
    "observer_runtime.shell_cleanup_token_collision") {
    std::size_t calls = 0;
    const winexinfo::ObserverShellStartupOperations operations{
        {},
        {},
        {},
        [&calls](std::uint64_t) {
            ++calls;
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{
        100,
        {{11, 100}},
        {{11, true, Handle(0x100), Handle(0x101)}},
    };
    const auto cleanup =
        winexinfo::CleanupObserverShellSubscriptions(operations, &state);
    WXI_REQUIRE_EQ(cleanup.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(
        cleanup.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE_EQ(calls, std::size_t{0});
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{100});
    WXI_REQUIRE_EQ(state.browser_registrations.size(), std::size_t{1});
}

WXI_TEST(
    observer_runtime_shell_startup_rejects_invalid_callback_infrastructure,
    "observer_runtime.shell_startup_queue_infrastructure") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        nullptr,
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    std::size_t calls = 0;
    const winexinfo::ObserverShellStartupOperations operations{
        [&calls](std::uint64_t* const id) {
            ++calls;
            *id = 100;
            return RuntimeOperationSuccess();
        },
        [&calls](std::vector<winexinfo::ObserverShellEntryMetadata>* const out) {
            ++calls;
            *out = {{11, true, Handle(0x100), Handle(0x101)}};
            return RuntimeOperationSuccess();
        },
        [&calls](const winexinfo::ObserverShellEntryMetadata&, std::uint64_t*) {
            ++calls;
            return RuntimeOperationSuccess();
        },
        [&calls](std::uint64_t) {
            ++calls;
            return RuntimeOperationSuccess();
        },
    };
    winexinfo::ObserverShellStartupState state{};
    winexinfo::ObserverShellStartupOutcome outcome{};
    RequireActiveFailure(
        winexinfo::StartObserverShellSubscriptions(
            &queue, operations, &state, &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(calls, std::size_t{0});
    WXI_REQUIRE_EQ(state.lifecycle_registration_id, std::uint64_t{0});
    WXI_REQUIRE(state.browser_registrations.empty());
}

WXI_TEST(
    observer_runtime_coordinator_runs_five_raw_kinds_to_deadline_and_cleanup,
    "observer_runtime.coordinator_end_to_end") {
    using namespace std::chrono_literals;
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    std::size_t nowCall = 0;
    std::size_t waitCall = 0;
    std::uint64_t reportedSequence = 0;
    std::vector<int> order;
    bool stopObservedStopping = false;
    const HANDLE responseEvent =
        reinterpret_cast<HANDLE>(std::uintptr_t{8});
    const std::vector<winexinfo::ObserverCallbackPayload> payloads{
        {
            winexinfo::ObserverCallbackSource::ShellLifecycle,
            winexinfo::ObservedEventKind::WindowRegistered,
            70,
            nullptr,
            0,
            nullptr,
            winexinfo::ObservedStructureChangeType::None,
        },
        {
            winexinfo::ObserverCallbackSource::ShellLifecycle,
            winexinfo::ObservedEventKind::WindowRegistered,
            71,
            nullptr,
            0,
            nullptr,
            winexinfo::ObservedStructureChangeType::None,
        },
        {
            winexinfo::ObserverCallbackSource::ShellLifecycle,
            winexinfo::ObservedEventKind::WindowRevoked,
            72,
            nullptr,
            0,
            nullptr,
            winexinfo::ObservedStructureChangeType::None,
        },
        {
            winexinfo::ObserverCallbackSource::BrowserNavigate,
            winexinfo::ObservedEventKind::NavigateComplete2,
            0,
            Handle(0x100),
            1,
            Handle(0x101),
            winexinfo::ObservedStructureChangeType::None,
        },
        {
            winexinfo::ObserverCallbackSource::UiaSelection,
            winexinfo::ObservedEventKind::TabSelected,
            0,
            Handle(0x100),
            1,
            Handle(0x101),
            winexinfo::ObservedStructureChangeType::None,
        },
        {
            winexinfo::ObserverCallbackSource::UiaStructure,
            winexinfo::ObservedEventKind::TabStructureChanged,
            0,
            Handle(0x100),
            1,
            Handle(0x101),
            winexinfo::ObservedStructureChangeType::ChildAdded,
        },
    };
    const winexinfo::ObserverCoordinatorOperations operations{
        [&order]() {
            order.push_back(0);
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationSuccess(),
                false,
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                {
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                    std::nullopt,
                    false,
                },
            };
        },
        [&order, &reportedSequence](
            const winexinfo::ObserverCallbackEnvelope& envelope) {
            order.push_back(static_cast<int>(10 + envelope.sequence));
            if (envelope.sequence == 2) {
                return winexinfo::ObserverEventProcessingResult{
                    RuntimeOperationSuccess(),
                    winexinfo::ObserverEventDisposition::Ignored,
                    envelope.sequence,
                    std::nullopt,
                };
            }
            ++reportedSequence;
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Completed,
                envelope.sequence,
                winexinfo::ObservedEventRecord{
                    reportedSequence,
                    envelope.payload.generation == 0
                        ? std::uint64_t{1}
                        : envelope.payload.generation,
                    envelope.payload.kind,
                    winexinfo::ObservedEventTransition::Remapped,
                    envelope.payload.top_level == nullptr
                        ? Handle(0x100)
                        : envelope.payload.top_level,
                    envelope.payload.shell_tab != nullptr,
                    envelope.payload.shell_tab,
                    true,
                    envelope.payload.shell_cookie != 0,
                    envelope.payload.shell_cookie,
                    envelope.payload.structure_change_type,
                    Handle(0x300),
                    Handle(0x301),
                    1,
                    false,
                    {},
                    false,
                    {},
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                },
            };
        },
        [&order]() {
            order.push_back(1);
            return RuntimeOperationSuccess();
        },
        [](const std::size_t, const std::uint64_t) {
            WXI_REQUIRE(false);
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Ignored,
                0,
                std::nullopt,
            };
        },
        [&]() {
            order.push_back(8);
            stopObservedStopping = !queue.BeginStopping().ok();
            return RuntimeOperationSuccess();
        },
        [&order]() {
            order.push_back(9);
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [&order](
            const winexinfo::EventObservationSnapshot& snapshot,
            bool* const passed) {
            order.push_back(10);
            WXI_REQUIRE_EQ(snapshot.event_count, std::size_t{5});
            *passed = true;
            return RuntimeOperationSuccess();
        },
        {
            [&]() {
                ++nowCall;
                if (nowCall == 1) {
                    return ready;
                }
                if (nowCall == 2) {
                    return ready + 50ms;
                }
                return ready + (nowCall == 3 ? 100ms : 1000ms);
            },
            [&](const DWORD count,
                const HANDLE*,
                DWORD,
                DWORD,
                DWORD) {
                ++waitCall;
                if (waitCall == 1) {
                    return WAIT_OBJECT_0 + count;
                }
                winexinfo::ObserverCallbackTicket ignored{};
                WXI_REQUIRE(queue.Admit(&ignored).ok());
                WXI_REQUIRE(queue.CompleteIgnored(ignored).ok());
                for (const auto& payload : payloads) {
                    WXI_REQUIRE(PushCallback(&queue, payload).ok());
                }
                return WAIT_OBJECT_0;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome outcome{};
    WXI_REQUIRE(winexinfo::RunObserverCoordinator(
                    1000,
                    5000,
                    &queue,
                    responseEvent,
                    operations,
                    &outcome)
                    .ok());
    WXI_REQUIRE(!outcome.failures.has_runtime_failure());
    WXI_REQUIRE(outcome.completion.cleanup_status.ok());
    WXI_REQUIRE(outcome.completion.gate_passed);
    WXI_REQUIRE(stopObservedStopping);
    WXI_REQUIRE_EQ(outcome.snapshot.duration_ms, std::uint32_t{1000});
    WXI_REQUIRE_EQ(outcome.snapshot.events.size(), std::size_t{5});
    WXI_REQUIRE_EQ(outcome.snapshot.event_count, std::size_t{5});
    WXI_REQUIRE_EQ(outcome.snapshot.ignored_event_count, std::size_t{2});
    WXI_REQUIRE_EQ(outcome.snapshot.events[0].sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(outcome.snapshot.events[4].sequence, std::uint64_t{5});
    WXI_REQUIRE_EQ(
        outcome.snapshot.kind_counts.window_registered,
        std::size_t{1});
    WXI_REQUIRE_EQ(
        outcome.snapshot.kind_counts.window_revoked,
        std::size_t{1});
    WXI_REQUIRE_EQ(
        outcome.snapshot.kind_counts.navigate_complete2,
        std::size_t{1});
    WXI_REQUIRE_EQ(
        outcome.snapshot.kind_counts.tab_selected,
        std::size_t{1});
    WXI_REQUIRE_EQ(
        outcome.snapshot.kind_counts.tab_structure_changed,
        std::size_t{1});
    const std::vector<int> expectedOrder{
        0, 1, 12, 13, 14, 15, 16, 17, 8, 9, 10};
    WXI_REQUIRE_EQ(order, expectedOrder);
    bool quiescent = false;
    WXI_REQUIRE(queue.IsQuiescent(&quiescent).ok());
    WXI_REQUIRE(quiescent);
}

WXI_TEST(
    observer_runtime_coordinator_blocks_later_raw_event_until_pending_response,
    "observer_runtime.coordinator_pending_fifo") {
    using namespace std::chrono_literals;
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const HANDLE responseEvent =
        reinterpret_cast<HANDLE>(std::uintptr_t{8});
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    std::size_t nowCall = 0;
    std::size_t waitCall = 0;
    std::vector<int> order;
    const winexinfo::ObserverCoordinatorOperations operations{
        [&order]() {
            order.push_back(0);
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationSuccess(),
                false,
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                {
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                    std::nullopt,
                    false,
                },
            };
        },
        [&order](const winexinfo::ObserverCallbackEnvelope& envelope) {
            order.push_back(static_cast<int>(10 + envelope.sequence));
            if (envelope.sequence == 1) {
                return winexinfo::ObserverEventProcessingResult{
                    RuntimeOperationSuccess(),
                    winexinfo::ObserverEventDisposition::Pending,
                    envelope.sequence,
                    std::nullopt,
                };
            }
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Completed,
                envelope.sequence,
                winexinfo::ObservedEventRecord{
                    2,
                    1,
                    winexinfo::ObservedEventKind::NavigateComplete2,
                    winexinfo::ObservedEventTransition::Remapped,
                    Handle(0x100),
                    true,
                    Handle(0x101),
                    true,
                    false,
                    0,
                    winexinfo::ObservedStructureChangeType::None,
                    Handle(0x300),
                    Handle(0x301),
                    1,
                    false,
                    {},
                    false,
                    {},
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                },
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [&order](const std::size_t index, const std::uint64_t pendingSequence) {
            order.push_back(21);
            WXI_REQUIRE_EQ(index, std::size_t{1});
            WXI_REQUIRE_EQ(pendingSequence, std::uint64_t{1});
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Completed,
                1,
                winexinfo::ObservedEventRecord{
                    1,
                    1,
                    winexinfo::ObservedEventKind::TabStructureChanged,
                    winexinfo::ObservedEventTransition::Remapped,
                    Handle(0x100),
                    true,
                    Handle(0x101),
                    true,
                    false,
                    0,
                    winexinfo::ObservedStructureChangeType::ChildAdded,
                    Handle(0x300),
                    Handle(0x301),
                    1,
                    false,
                    {},
                    false,
                    {},
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                },
            };
        },
        [&order]() {
            order.push_back(8);
            return RuntimeOperationSuccess();
        },
        [&order]() {
            order.push_back(9);
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [&order](const winexinfo::EventObservationSnapshot& snapshot, bool* pass) {
            order.push_back(10);
            WXI_REQUIRE_EQ(snapshot.events.size(), std::size_t{2});
            WXI_REQUIRE_EQ(snapshot.events[0].sequence, std::uint64_t{1});
            WXI_REQUIRE_EQ(snapshot.events[1].sequence, std::uint64_t{2});
            *pass = true;
            return RuntimeOperationSuccess();
        },
        {
            [&]() {
                ++nowCall;
                if (nowCall == 1) {
                    return ready;
                }
                if (nowCall == 2) {
                    return ready + 50ms;
                }
                if (nowCall <= 5) {
                    return ready + 1000ms;
                }
                return ready + 1100ms;
            },
            [&](DWORD count, const HANDLE*, DWORD, DWORD, DWORD) {
                ++waitCall;
                if (waitCall == 1) {
                    WXI_REQUIRE(PushCallback(
                                    &queue,
                                    {
                                        winexinfo::ObserverCallbackSource::UiaStructure,
                                        winexinfo::ObservedEventKind::TabStructureChanged,
                                        0,
                                        Handle(0x100),
                                        1,
                                        Handle(0x101),
                                        winexinfo::ObservedStructureChangeType::ChildAdded,
                                    })
                                    .ok());
                    WXI_REQUIRE(PushCallback(
                                    &queue,
                                    {
                                        winexinfo::ObserverCallbackSource::BrowserNavigate,
                                        winexinfo::ObservedEventKind::NavigateComplete2,
                                        0,
                                        Handle(0x100),
                                        1,
                                        Handle(0x101),
                                        winexinfo::ObservedStructureChangeType::None,
                                    })
                                    .ok());
                    return WAIT_OBJECT_0;
                }
                return WAIT_OBJECT_0 + count - 1;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome outcome{};
    WXI_REQUIRE(winexinfo::RunObserverCoordinator(
                    1000,
                    5000,
                    &queue,
                    responseEvent,
                    operations,
                    &outcome)
                    .ok());
    const std::vector<int> expected{0, 11, 8, 21, 12, 9, 10};
    WXI_REQUIRE_EQ(order, expected);
    WXI_REQUIRE(outcome.completion.gate_passed);
}

WXI_TEST(
    observer_runtime_coordinator_prevalidates_duration_grace_and_handles,
    "observer_runtime.coordinator_input_transaction") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    const HANDLE wakeEvent = reinterpret_cast<HANDLE>(std::uintptr_t{7});
    winexinfo::ObserverCallbackQueue queue(
        wakeEvent,
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    std::size_t startupCalls = 0;
    const winexinfo::ObserverCoordinatorOperations operations{
        [&startupCalls]() {
            ++startupCalls;
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationSuccess(),
                false,
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                {
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                    std::nullopt,
                    false,
                },
            };
        },
        [](const winexinfo::ObserverCallbackEnvelope& envelope) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Ignored,
                envelope.sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [](std::size_t, std::uint64_t sequence) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Ignored,
                sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        []() {
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [](const winexinfo::EventObservationSnapshot&, bool* pass) {
            *pass = true;
            return RuntimeOperationSuccess();
        },
        {
            []() { return winexinfo::ObserverDeadline::Clock::now(); },
            [](DWORD, const HANDLE*, DWORD, DWORD, DWORD) {
                return WAIT_TIMEOUT;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome output{};
    output.snapshot.duration_ms = 77;
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            999,
            5000,
            &queue,
            reinterpret_cast<HANDLE>(std::uintptr_t{8}),
            operations,
            &output),
        E_INVALIDARG);
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            4999,
            &queue,
            reinterpret_cast<HANDLE>(std::uintptr_t{8}),
            operations,
            &output),
        E_INVALIDARG);
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            5000,
            &queue,
            nullptr,
            operations,
            &output),
        E_INVALIDARG);
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            5000,
            &queue,
            wakeEvent,
            operations,
            &output),
        E_INVALIDARG);
    WXI_REQUIRE_EQ(startupCalls, std::size_t{0});
    WXI_REQUIRE_EQ(output.snapshot.duration_ms, std::uint32_t{77});
}

WXI_TEST(
    observer_runtime_coordinator_pending_shutdown_timeout_still_cleans_up,
    "observer_runtime.coordinator_shutdown_timeout") {
    using namespace std::chrono_literals;
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    std::size_t nowCall = 0;
    std::size_t waitCall = 0;
    std::size_t cleanupCalls = 0;
    const winexinfo::ObserverCoordinatorOperations operations{
        []() {
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationSuccess(),
                false,
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                {
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                    std::nullopt,
                    false,
                },
            };
        },
        [](const winexinfo::ObserverCallbackEnvelope& envelope) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Pending,
                envelope.sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [](std::size_t, std::uint64_t sequence) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Completed,
                sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [&cleanupCalls]() {
            ++cleanupCalls;
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [](const winexinfo::EventObservationSnapshot&, bool* pass) {
            *pass = true;
            return RuntimeOperationSuccess();
        },
        {
            [&]() {
                ++nowCall;
                if (nowCall == 1) {
                    return ready;
                }
                if (nowCall == 2) {
                    return ready + 50ms;
                }
                if (nowCall <= 5) {
                    return ready + 1000ms;
                }
                return ready + 6000ms;
            },
            [&](DWORD, const HANDLE*, DWORD, DWORD, DWORD) {
                ++waitCall;
                WXI_REQUIRE_EQ(waitCall, std::size_t{1});
                WXI_REQUIRE(PushCallback(
                                &queue,
                                {
                                    winexinfo::ObserverCallbackSource::UiaStructure,
                                    winexinfo::ObservedEventKind::TabStructureChanged,
                                    0,
                                    Handle(0x100),
                                    1,
                                    Handle(0x101),
                                    winexinfo::ObservedStructureChangeType::ChildAdded,
                                })
                                .ok());
                return WAIT_OBJECT_0;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome outcome{};
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            5000,
            &queue,
            reinterpret_cast<HANDLE>(std::uintptr_t{8}),
            operations,
            &outcome),
        HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    WXI_REQUIRE_EQ(cleanupCalls, std::size_t{1});
    WXI_REQUIRE(outcome.failures.any_transport_failure());
    WXI_REQUIRE_EQ(
        outcome.failures.runtime_status().win32,
        DWORD{ERROR_TIMEOUT});
}

WXI_TEST(
    observer_runtime_coordinator_consumes_spurious_response_before_cleanup,
    "observer_runtime.coordinator_spurious_response") {
    using namespace std::chrono_literals;
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    std::size_t nowCall = 0;
    std::size_t responseCalls = 0;
    std::size_t cleanupCalls = 0;
    const winexinfo::ObserverCoordinatorOperations operations{
        []() {
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationSuccess(),
                false,
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                {
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                    std::nullopt,
                    false,
                },
            };
        },
        [](const winexinfo::ObserverCallbackEnvelope& envelope) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Ignored,
                envelope.sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [&responseCalls](const std::size_t index, const std::uint64_t sequence) {
            ++responseCalls;
            WXI_REQUIRE_EQ(index, std::size_t{1});
            WXI_REQUIRE_EQ(sequence, std::uint64_t{0});
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationFailure(
                    winexinfo::ObserverFailureOrigin::Transport,
                    RPC_E_DISCONNECTED),
                winexinfo::ObserverEventDisposition::Ignored,
                0,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [&cleanupCalls]() {
            ++cleanupCalls;
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [](const winexinfo::EventObservationSnapshot&, bool* pass) {
            *pass = true;
            return RuntimeOperationSuccess();
        },
        {
            [&]() {
                ++nowCall;
                return nowCall == 1 ? ready : ready + 50ms;
            },
            [](const DWORD count, const HANDLE*, DWORD, DWORD, DWORD) {
                return WAIT_OBJECT_0 + count - 1;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome outcome{};
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            5000,
            &queue,
            reinterpret_cast<HANDLE>(std::uintptr_t{8}),
            operations,
            &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(responseCalls, std::size_t{1});
    WXI_REQUIRE_EQ(cleanupCalls, std::size_t{1});
    WXI_REQUIRE_EQ(outcome.failures.runtime_status().hresult, S_FALSE);
    WXI_REQUIRE(outcome.failures.any_transport_failure());
}

WXI_TEST(
    observer_runtime_coordinator_rejects_completed_record_kind_mismatch,
    "observer_runtime.coordinator_sync_kind_correlation") {
    using namespace std::chrono_literals;
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    std::size_t waitCall = 0;
    const winexinfo::ObserverCoordinatorOperations operations{
        []() {
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationSuccess(),
                false,
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                {
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                    std::nullopt,
                    false,
                },
            };
        },
        [](const winexinfo::ObserverCallbackEnvelope& envelope) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Completed,
                envelope.sequence,
                winexinfo::ObservedEventRecord{
                    1,
                    envelope.payload.generation,
                    winexinfo::ObservedEventKind::TabSelected,
                    winexinfo::ObservedEventTransition::Remapped,
                    envelope.payload.top_level,
                    true,
                    envelope.payload.shell_tab,
                    true,
                    false,
                    0,
                    winexinfo::ObservedStructureChangeType::None,
                    Handle(0x300),
                    Handle(0x301),
                    1,
                    false,
                    {},
                    false,
                    {},
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                },
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [](std::size_t, std::uint64_t sequence) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Ignored,
                sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        []() {
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [](const winexinfo::EventObservationSnapshot&, bool* pass) {
            *pass = true;
            return RuntimeOperationSuccess();
        },
        {
            [ready]() { return ready; },
            [&](DWORD, const HANDLE*, DWORD, DWORD, DWORD) {
                ++waitCall;
                WXI_REQUIRE_EQ(waitCall, std::size_t{1});
                WXI_REQUIRE(PushCallback(
                                &queue,
                                {
                                    winexinfo::ObserverCallbackSource::BrowserNavigate,
                                    winexinfo::ObservedEventKind::NavigateComplete2,
                                    0,
                                    Handle(0x100),
                                    7,
                                    Handle(0x101),
                                    winexinfo::ObservedStructureChangeType::None,
                                })
                                .ok());
                return WAIT_OBJECT_0;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome outcome{};
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            5000,
            &queue,
            reinterpret_cast<HANDLE>(std::uintptr_t{8}),
            operations,
            &outcome),
        S_FALSE);
    WXI_REQUIRE(outcome.snapshot.events.empty());
    WXI_REQUIRE_EQ(
        outcome.snapshot.kind_counts.tab_selected,
        std::size_t{0});
}

WXI_TEST(
    observer_runtime_coordinator_rejects_pending_response_kind_mismatch,
    "observer_runtime.coordinator_pending_kind_correlation") {
    using namespace std::chrono_literals;
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    std::size_t waitCall = 0;
    const winexinfo::ObserverCoordinatorOperations operations{
        []() {
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationSuccess(),
                false,
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                {
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                    std::nullopt,
                    false,
                },
            };
        },
        [](const winexinfo::ObserverCallbackEnvelope& envelope) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Pending,
                envelope.sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [](const std::size_t index, const std::uint64_t sequence) {
            WXI_REQUIRE_EQ(index, std::size_t{1});
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Completed,
                sequence,
                winexinfo::ObservedEventRecord{
                    1,
                    11,
                    winexinfo::ObservedEventKind::NavigateComplete2,
                    winexinfo::ObservedEventTransition::Remapped,
                    Handle(0x100),
                    true,
                    Handle(0x101),
                    true,
                    false,
                    0,
                    winexinfo::ObservedStructureChangeType::None,
                    Handle(0x300),
                    Handle(0x301),
                    1,
                    false,
                    {},
                    false,
                    {},
                    {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                },
            };
        },
        []() { return RuntimeOperationSuccess(); },
        []() {
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [](const winexinfo::EventObservationSnapshot&, bool* pass) {
            *pass = true;
            return RuntimeOperationSuccess();
        },
        {
            [ready]() { return ready; },
            [&](const DWORD count, const HANDLE*, DWORD, DWORD, DWORD) {
                ++waitCall;
                if (waitCall == 1) {
                    WXI_REQUIRE(PushCallback(
                                    &queue,
                                    {
                                        winexinfo::ObserverCallbackSource::UiaStructure,
                                        winexinfo::ObservedEventKind::TabStructureChanged,
                                        0,
                                        Handle(0x100),
                                        11,
                                        Handle(0x101),
                                        winexinfo::ObservedStructureChangeType::ChildAdded,
                                    })
                                    .ok());
                    return WAIT_OBJECT_0;
                }
                WXI_REQUIRE_EQ(waitCall, std::size_t{2});
                return WAIT_OBJECT_0 + count - 1;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome outcome{};
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            5000,
            &queue,
            reinterpret_cast<HANDLE>(std::uintptr_t{8}),
            operations,
            &outcome),
        S_FALSE);
    WXI_REQUIRE(outcome.snapshot.events.empty());
    WXI_REQUIRE_EQ(
        outcome.snapshot.kind_counts.navigate_complete2,
        std::size_t{0});
}

WXI_TEST(
    observer_runtime_processing_result_rejects_malformed_disposition_shapes,
    "observer_runtime.coordinator_processing_shape") {
    const winexinfo::ObserverCallbackEnvelope lifecycleExpected{
        7,
        {
            winexinfo::ObserverCallbackSource::ShellLifecycle,
            winexinfo::ObservedEventKind::WindowRegistered,
            73,
            nullptr,
            0,
            nullptr,
            winexinfo::ObservedStructureChangeType::None,
        },
    };
    winexinfo::ObservedEventRecord record{};
    record.sequence = 1;
    record.kind = winexinfo::ObservedEventKind::WindowRegistered;
    record.shell_cookie_present = true;
    record.shell_cookie = 73;
    winexinfo::ObserverEventProcessingResult result{
        RuntimeOperationSuccess(),
        winexinfo::ObserverEventDisposition::Completed,
        7,
        record,
    };
    WXI_REQUIRE(winexinfo::ValidateObserverEventProcessingResult(
                    result, lifecycleExpected, 1, true)
                    .ok());
    result.record->structure_change_type =
        winexinfo::ObservedStructureChangeType::ChildAdded;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, lifecycleExpected, 1, true),
        S_FALSE);
    result.record->structure_change_type =
        winexinfo::ObservedStructureChangeType::None;
    result.record->shell_cookie = 74;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, lifecycleExpected, 1, true),
        S_FALSE);
    result.record->shell_cookie = 73;
    result.record->shell_cookie_present = false;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, lifecycleExpected, 1, true),
        S_FALSE);
    result.record->shell_cookie_present = true;
    const winexinfo::ObserverCallbackEnvelope metadataExpected{
        7,
        {
            winexinfo::ObserverCallbackSource::BrowserNavigate,
            winexinfo::ObservedEventKind::NavigateComplete2,
            0,
            Handle(0x100),
            9,
            Handle(0x101),
            winexinfo::ObservedStructureChangeType::None,
        },
    };
    result.record->kind = winexinfo::ObservedEventKind::NavigateComplete2;
    result.record->shell_cookie_present = false;
    result.record->shell_cookie = 0;
    result.record->source_top_level = Handle(0x100);
    result.record->generation = 9;
    result.record->source_shell_tab_present = true;
    result.record->source_shell_tab = Handle(0x101);
    WXI_REQUIRE(winexinfo::ValidateObserverEventProcessingResult(
                    result, metadataExpected, 1, true)
                    .ok());
    result.record->source_top_level = Handle(0x102);
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.record->source_top_level = Handle(0x100);
    result.record->generation = 10;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.record->generation = 9;
    result.record->source_shell_tab = Handle(0x102);
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.record->source_shell_tab = Handle(0x101);
    result.record->source_shell_tab_present = false;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.record->source_shell_tab_present = true;

    winexinfo::ObserverCallbackEnvelope malformed = metadataExpected;
    malformed.payload.kind = winexinfo::ObservedEventKind::TabSelected;
    result.record->kind = winexinfo::ObservedEventKind::TabSelected;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed = metadataExpected;
    result.record->kind = winexinfo::ObservedEventKind::NavigateComplete2;
    malformed.payload.top_level = nullptr;
    result.record->source_top_level = nullptr;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed = metadataExpected;
    result.record->source_top_level = Handle(0x100);
    malformed.payload.generation = 0;
    result.record->generation = 0;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed = metadataExpected;
    result.record->generation = 9;
    malformed.payload.shell_tab = nullptr;
    result.record->source_shell_tab_present = false;
    result.record->source_shell_tab = nullptr;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed = metadataExpected;
    result.record->source_shell_tab_present = true;
    result.record->source_shell_tab = Handle(0x101);
    malformed.payload.shell_cookie = 1;
    result.record->shell_cookie_present = true;
    result.record->shell_cookie = 1;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed = metadataExpected;
    result.record->shell_cookie_present = false;
    result.record->shell_cookie = 0;
    malformed.payload.structure_change_type =
        static_cast<winexinfo::ObservedStructureChangeType>(99);
    result.record->structure_change_type =
        static_cast<winexinfo::ObservedStructureChangeType>(99);
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed = metadataExpected;
    malformed.payload.source = winexinfo::ObserverCallbackSource::UiaStructure;
    malformed.payload.kind =
        winexinfo::ObservedEventKind::TabStructureChanged;
    result.record->kind = winexinfo::ObservedEventKind::TabStructureChanged;
    result.record->structure_change_type =
        winexinfo::ObservedStructureChangeType::None;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed.payload.structure_change_type =
        winexinfo::ObservedStructureChangeType::ChildAdded;
    result.record->structure_change_type =
        winexinfo::ObservedStructureChangeType::ChildAdded;
    WXI_REQUIRE(winexinfo::ValidateObserverEventProcessingResult(
                    result, malformed, 1, true)
                    .ok());
    malformed = metadataExpected;
    malformed.payload.source =
        static_cast<winexinfo::ObserverCallbackSource>(99);
    result.record->kind = winexinfo::ObservedEventKind::NavigateComplete2;
    result.record->structure_change_type =
        winexinfo::ObservedStructureChangeType::None;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);

    malformed = lifecycleExpected;
    malformed.payload.shell_cookie = 0;
    result.record->kind = winexinfo::ObservedEventKind::WindowRegistered;
    result.record->source_top_level = nullptr;
    result.record->generation = 0;
    result.record->source_shell_tab_present = false;
    result.record->source_shell_tab = nullptr;
    result.record->shell_cookie_present = true;
    result.record->shell_cookie = 0;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);
    malformed = lifecycleExpected;
    malformed.payload.top_level = Handle(0x100);
    result.record->shell_cookie = 73;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, malformed, 1, true),
        S_FALSE);

    result.record->kind = winexinfo::ObservedEventKind::NavigateComplete2;
    result.record->source_top_level = Handle(0x100);
    result.record->generation = 9;
    result.record->source_shell_tab_present = true;
    result.record->source_shell_tab = Handle(0x101);
    result.record->shell_cookie_present = false;
    result.record->shell_cookie = 0;
    result.record->structure_change_type =
        winexinfo::ObservedStructureChangeType::None;
    result.raw_sequence = 8;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.raw_sequence = 7;
    result.record->sequence = 2;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.disposition = winexinfo::ObserverEventDisposition::Ignored;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.record.reset();
    WXI_REQUIRE(winexinfo::ValidateObserverEventProcessingResult(
                    result, metadataExpected, 1, true)
                    .ok());
    result.disposition = winexinfo::ObserverEventDisposition::Pending;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, false),
        S_FALSE);
    result.disposition =
        static_cast<winexinfo::ObserverEventDisposition>(99);
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
    result.operation = RuntimeOperationFailure(
        winexinfo::ObserverFailureOrigin::Transport,
        RPC_E_DISCONNECTED);
    result.disposition = winexinfo::ObserverEventDisposition::Ignored;
    RequireActiveFailure(
        winexinfo::ValidateObserverEventProcessingResult(
            result, metadataExpected, 1, true),
        S_FALSE);
}

WXI_TEST(
    observer_runtime_coordinator_merges_startup_first_sticky_and_cleanup_retry,
    "observer_runtime.coordinator_startup_merge") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    std::size_t stopCalls = 0;
    std::size_t cleanupCalls = 0;
    const winexinfo::ObserverCoordinatorOperations operations{
        [&queue]() {
            WXI_REQUIRE(queue.EnsureStoppingState().ok());
            return winexinfo::ObserverStartupOutcome{
                RuntimeOperationFailure(
                    winexinfo::ObserverFailureOrigin::Contract,
                    S_FALSE),
                true,
                {
                    winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                    RPC_E_DISCONNECTED,
                    ERROR_SUCCESS,
                },
                {
                    {
                        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                        CONNECT_E_NOCONNECTION,
                        ERROR_SUCCESS,
                    },
                    winexinfo::ObserverFailureOrigin::Contract,
                    false,
                },
            };
        },
        [](const winexinfo::ObserverCallbackEnvelope& envelope) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Ignored,
                envelope.sequence,
                std::nullopt,
            };
        },
        []() { return RuntimeOperationSuccess(); },
        [](std::size_t, std::uint64_t sequence) {
            return winexinfo::ObserverEventProcessingResult{
                RuntimeOperationSuccess(),
                winexinfo::ObserverEventDisposition::Ignored,
                sequence,
                std::nullopt,
            };
        },
        [&stopCalls]() {
            ++stopCalls;
            return RuntimeOperationSuccess();
        },
        [&cleanupCalls]() {
            ++cleanupCalls;
            return winexinfo::ObserverCleanupOutcome{
                {
                    winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                    E_OUTOFMEMORY,
                    ERROR_SUCCESS,
                },
                winexinfo::ObserverFailureOrigin::Transport,
                true,
            };
        },
        [](const winexinfo::EventObservationSnapshot&, bool* pass) {
            *pass = true;
            return RuntimeOperationSuccess();
        },
        {
            []() {
                return winexinfo::ObserverDeadline::Clock::time_point{
                    std::chrono::milliseconds{10000}};
            },
            [](DWORD, const HANDLE*, DWORD, DWORD, DWORD) {
                return WAIT_TIMEOUT;
            },
            []() { return DWORD{ERROR_SUCCESS}; },
        },
    };
    winexinfo::ObserverRuntimeOutcome outcome{};
    RequireActiveFailure(
        winexinfo::RunObserverCoordinator(
            1000,
            5000,
            &queue,
            reinterpret_cast<HANDLE>(std::uintptr_t{8}),
            operations,
            &outcome),
        S_FALSE);
    WXI_REQUIRE_EQ(stopCalls, std::size_t{1});
    WXI_REQUIRE_EQ(cleanupCalls, std::size_t{1});
    WXI_REQUIRE_EQ(outcome.failures.runtime_status().hresult, S_FALSE);
    WXI_REQUIRE(outcome.failures.any_transport_failure());
    WXI_REQUIRE_EQ(
        outcome.completion.cleanup_status.hresult,
        CONNECT_E_NOCONNECTION);
    WXI_REQUIRE_EQ(
        outcome.completion.cleanup_failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
    WXI_REQUIRE(outcome.completion.any_cleanup_transport_failure);
}

WXI_TEST(
    observer_runtime_shell_sta_resource_graph_orders_start_pump_and_cleanup,
    "observer_runtime.shell_sta_resource_graph") {
    DWORD lastError = ERROR_SUCCESS;
    std::vector<int> wakeCalls;
    std::vector<int> fallbackCalls;
    winexinfo::ObserverCallbackQueue queue(
        reinterpret_cast<HANDLE>(std::uintptr_t{7}),
        QueueOperations(&lastError, &wakeCalls, &fallbackCalls));
    FakeShellWindows shellWindows;
    std::vector<int> queryLog;
    FakeDispatch lifecycleSink(&queryLog);
    FakeDispatch browserObject(&queryLog);
    FakeDispatch browserSink(&queryLog);
    FakeConnectionSource connectionPoint;
    const HWND topLevel = Handle(0x700);
    const HWND shellTab = Handle(0x701);
    const DWORD ownerThread = GetCurrentThreadId();
    std::vector<std::string> calls;
    std::uint64_t nextCookie = 40;
    std::size_t captureCount = 0;
    bool messageAvailable = true;
    bool failBrowserCleanupOnce = true;
    DWORD reportedThread = ownerThread;

    const winexinfo::ObserverShellStaOperations operations{
        [&calls]() {
            calls.push_back("prepare");
            return RuntimeOperationSuccess();
        },
        [&calls, topLevel, ownerThread](
            std::vector<winexinfo::ExplorerWindowRecord>* output) {
            calls.push_back("enumerate");
            output->push_back({topLevel, 100, ownerThread});
            return RuntimeOperationSuccess();
        },
        [&calls, &shellWindows](
            Microsoft::WRL::ComPtr<IShellWindows>& output) {
            calls.push_back("create_shell_windows");
            output = &shellWindows;
            return RuntimeOperationSuccess();
        },
        [&calls,
         &captureCount,
         &browserObject,
         topLevel,
         shellTab,
         ownerThread](
            IShellWindows*,
            std::span<const HWND> targets,
            winexinfo::ObserverShellStaCapture* output) {
            calls.push_back(captureCount++ == 0 ? "capture_a" : "capture_b");
            WXI_REQUIRE_EQ(targets.size(), std::size_t{1});
            WXI_REQUIRE_EQ(targets.front(), topLevel);
            output->browser_set.status = {
                winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
            output->browser_set.owner_thread_id = ownerThread;
            const std::uintptr_t identity = reinterpret_cast<std::uintptr_t>(
                static_cast<IUnknown*>(&browserObject));
            output->browsers.push_back({
                {identity, true, topLevel, shellTab},
                Microsoft::WRL::ComPtr<IUnknown>{
                    static_cast<IUnknown*>(&browserObject)},
                Microsoft::WRL::ComPtr<IUnknown>{
                    static_cast<IUnknown*>(&browserObject)},
            });
            return RuntimeOperationSuccess();
        },
        [&calls, &lifecycleSink](
            winexinfo::ObserverCallbackQueue*,
            Microsoft::WRL::ComPtr<IDispatch>& output) {
            calls.push_back("create_lifecycle_sink");
            output = &lifecycleSink;
            return RuntimeOperationSuccess();
        },
        [&calls, &browserSink, topLevel, shellTab](
            winexinfo::ObserverCallbackQueue*,
            IUnknown* source,
            HWND observedTopLevel,
            std::uint64_t generation,
            HWND observedShellTab,
            Microsoft::WRL::ComPtr<IDispatch>& output) {
            calls.push_back("create_browser_sink");
            WXI_REQUIRE(source != nullptr);
            WXI_REQUIRE_EQ(observedTopLevel, topLevel);
            WXI_REQUIRE_EQ(generation, std::uint64_t{1});
            WXI_REQUIRE_EQ(observedShellTab, shellTab);
            output = &browserSink;
            return RuntimeOperationSuccess();
        },
        [&calls, &connectionPoint, &nextCookie, ownerThread](
            IUnknown*,
            REFIID,
            IUnknown* sink,
            winexinfo::ObserverConnectionPointRegistration* output) {
            calls.push_back(calls.size() < 5 ? "advise_lifecycle" : "advise_browser");
            output->connection_point = static_cast<IConnectionPoint*>(&connectionPoint);
            output->sink = sink;
            output->subscription_cookie = static_cast<DWORD>(nextCookie++);
            output->owner_thread_id = ownerThread;
            return RuntimeOperationSuccess();
        },
        [&calls, &failBrowserCleanupOnce](
            winexinfo::ObserverConnectionPointRegistration* registration) {
            calls.push_back(
                registration->subscription_cookie == 41
                    ? "unadvise_browser"
                    : "unadvise_lifecycle");
            if (registration->subscription_cookie == 41 &&
                failBrowserCleanupOnce) {
                failBrowserCleanupOnce = false;
                return RuntimeOperationFailure(
                    winexinfo::ObserverFailureOrigin::Contract,
                    CONNECT_E_NOCONNECTION);
            }
            registration->connection_point.Reset();
            registration->sink.Reset();
            registration->subscription_cookie = 0;
            registration->owner_thread_id = 0;
            return RuntimeOperationSuccess();
        },
        [&messageAvailable](MSG* message, HWND, UINT, UINT, UINT remove) {
            WXI_REQUIRE_EQ(remove, UINT{PM_REMOVE});
            if (!messageAvailable) {
                return FALSE;
            }
            messageAvailable = false;
            *message = MSG{};
            message->message = WM_APP;
            return TRUE;
        },
        [&calls](const MSG*) {
            calls.push_back("translate");
            return TRUE;
        },
        [&calls](const MSG*) {
            calls.push_back("dispatch");
            return LRESULT{0};
        },
        [&reportedThread]() { return reportedThread; },
    };

    winexinfo::ObserverShellStaResourceGraph graph{};
    winexinfo::ObserverShellStartupOutcome startup{};
    WXI_REQUIRE(winexinfo::StartObserverShellStaResources(
                    &queue, operations, &graph, &startup)
                    .ok());
    WXI_REQUIRE(startup.setup.ok());
    WXI_REQUIRE_EQ(graph.owner_thread_id, ownerThread);
    WXI_REQUIRE_EQ(graph.browser_resources.size(), std::size_t{1});
    WXI_REQUIRE_EQ(graph.startup_state.baseline.size(), std::size_t{1});
    const std::vector<std::string> expectedStart{
        "prepare",
        "enumerate",
        "create_shell_windows",
        "create_lifecycle_sink",
        "advise_lifecycle",
        "capture_a",
        "create_browser_sink",
        "advise_browser",
        "capture_b",
    };
    WXI_REQUIRE_EQ(calls, expectedStart);

    WXI_REQUIRE(winexinfo::PumpObserverShellStaMessages(operations, graph).ok());
    WXI_REQUIRE_EQ(calls[calls.size() - 2], std::string{"translate"});
    WXI_REQUIRE_EQ(calls.back(), std::string{"dispatch"});
    reportedThread = ownerThread + 1;
    RequireActiveFailure(
        winexinfo::PumpObserverShellStaMessages(operations, graph).status,
        RPC_E_WRONG_THREAD);
    reportedThread = ownerThread;

    const winexinfo::ObserverShellCleanupOutcome partialCleanup =
        winexinfo::CleanupObserverShellStaResources(operations, &graph);
    RequireActiveFailure(partialCleanup.status, CONNECT_E_NOCONNECTION);
    WXI_REQUIRE_EQ(graph.browser_resources.size(), std::size_t{1});
    WXI_REQUIRE_EQ(graph.owner_thread_id, ownerThread);
    WXI_REQUIRE_EQ(calls[calls.size() - 2], std::string{"unadvise_browser"});
    WXI_REQUIRE_EQ(calls.back(), std::string{"unadvise_lifecycle"});

    const winexinfo::ObserverShellCleanupOutcome cleanup =
        winexinfo::CleanupObserverShellStaResources(operations, &graph);
    WXI_REQUIRE(cleanup.status.ok());
    WXI_REQUIRE_EQ(calls.back(), std::string{"unadvise_browser"});
    WXI_REQUIRE_EQ(graph.owner_thread_id, DWORD{0});
    WXI_REQUIRE(graph.shell_windows == nullptr);
    WXI_REQUIRE(graph.browser_resources.empty());
}

WXI_TEST(
    observer_runtime_shell_lifecycle_has_no_cookie_resolver,
    "observer_runtime.shell_lifecycle_no_cookie_resolver") {
    WXI_REQUIRE(
        !HasRegisteredShellResolver<
            winexinfo::ObserverShellStaOperations>);
}

WXI_TEST(
    observer_runtime_gate_requires_all_correlated_event_kinds,
    "observer_runtime.production_gate") {
    winexinfo::EventObservationSnapshot snapshot{};
    snapshot.runtime_status = {
        winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
    snapshot.cleanup_status = {
        winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
    bool passed = true;
    WXI_REQUIRE(winexinfo::EvaluateEventObservationGate(snapshot, &passed).ok());
    WXI_REQUIRE(!passed);
}

WXI_TEST(
    observer_runtime_gate_accepts_complete_synthetic_event_matrix,
    "observer_runtime.production_gate.complete_event_matrix") {
    winexinfo::EventObservationSnapshot snapshot =
        PassingProductionGateSnapshot();
    bool passed = false;
    WXI_REQUIRE(winexinfo::EvaluateEventObservationGate(snapshot, &passed).ok());
    WXI_REQUIRE(passed);
}

WXI_TEST(
    observer_runtime_gate_rejects_nonunique_active_view_remap,
    "observer_runtime.production_gate.active_view_cardinality") {
    winexinfo::EventObservationSnapshot snapshot =
        PassingProductionGateSnapshot();
    snapshot.events[3].active_view_count = 2;
    bool passed = true;
    WXI_REQUIRE(winexinfo::EvaluateEventObservationGate(snapshot, &passed).ok());
    WXI_REQUIRE(!passed);
}

WXI_TEST(
    observer_runtime_gate_rejects_nonterminal_revocation,
    "observer_runtime.production_gate.revoked_cardinality") {
    winexinfo::EventObservationSnapshot snapshot =
        PassingProductionGateSnapshot();
    snapshot.events[4].current_active_view = Handle(0x203);
    snapshot.events[4].active_view_count = 1;
    bool passed = true;
    WXI_REQUIRE(winexinfo::EvaluateEventObservationGate(snapshot, &passed).ok());
    WXI_REQUIRE(!passed);
}

WXI_TEST(
    observer_runtime_gate_rejects_visible_pending_registration,
    "observer_runtime.production_gate.pending_cardinality") {
    winexinfo::EventObservationSnapshot snapshot =
        PassingProductionGateSnapshot();
    snapshot.events[0].current_active_view = Handle(0x104);
    snapshot.events[0].active_view_count = 1;
    bool passed = true;
    WXI_REQUIRE(winexinfo::EvaluateEventObservationGate(snapshot, &passed).ok());
    WXI_REQUIRE(!passed);
}

WXI_TEST(
    observer_runtime_gate_rejects_runtime_failure,
    "observer_runtime.production_gate.runtime_failure") {
    winexinfo::EventObservationSnapshot snapshot =
        PassingProductionGateSnapshot();
    snapshot.runtime_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        RPC_E_DISCONNECTED,
        ERROR_SUCCESS,
    };
    bool passed = true;
    WXI_REQUIRE(winexinfo::EvaluateEventObservationGate(snapshot, &passed).ok());
    WXI_REQUIRE(!passed);
}
