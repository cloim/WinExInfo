#include "hook/runtime.h"

#include "common/win32_handle.h"
#include "hook/explorer_layout.h"
#include "hook/status_pane.h"
#include "hook/tab_subclass_set.h"
#include "ipc/named_pipe.h"
#include "ipc/protocol.h"

#include <Windows.h>
#include <CommCtrl.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

extern HMODULE g_hook_module;

namespace winexinfo::hook {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ContractFailure() noexcept {
    return {ErrorCode::DLL_INITIALIZATION_FAILED, S_FALSE, ERROR_INVALID_STATE};
}

Status PlacementFailure(const DWORD error) noexcept {
    return {
        ErrorCode::WINDOW_ATTACH_FAILED,
        HRESULT_FROM_WIN32(error),
        error,
    };
}

struct RuntimeContext final {
    HWND target = nullptr;
    std::uint64_t attach_id = 0;
    DWORD pid = 0;
    DWORD tid = 0;
    HMODULE module_reference = nullptr;
    UniqueHandle hook_released;
    UniqueHandle cleanup_ack;
    UniqueHandle refresh_event;
    UniqueHandle refresh_stop;
    UniqueHandle refresh_worker;
    UniqueHandle parent_cleanup_ack;
    UniqueHandle tab_update_ack;
    StatusPane pane{};
    StatusPaneRefreshCoordinator refresh;
    RuntimeSignalSourceState signal_source;
    HookRuntimeRefreshIngress ingress;
    HookRuntimeStateMachine state;
    std::mutex tab_update_mutex;
    std::optional<ipc::TabSetUpdate> pending_tab_update;
    ipc::TabSetResult tab_update_result;
};

std::atomic<RuntimeContext*> g_runtime{nullptr};
HookCallbackGate g_callback_gate;
inline constexpr UINT_PTR kRuntimeParentSubclassId = 0x57495832;

Status PostRuntimeMessage(const HWND pane, const UINT message) {
    return PostMessageW(pane, message, 0, 0) != FALSE
        ? Success()
        : PlacementFailure(GetLastError());
}

Status WakeRefreshWorker(RuntimeContext* const context) {
    return context != nullptr && SetEvent(context->refresh_event.get()) != FALSE
        ? Success()
        : PlacementFailure(GetLastError());
}

LRESULT CALLBACK RuntimeParentSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR id,
    const DWORD_PTR reference) {
    auto* const context = reinterpret_cast<RuntimeContext*>(reference);
    if (id == kRuntimeParentSubclassId && context != nullptr &&
        message == WM_DESTROY) {
        const StatusPanePlacementOperations placement =
            CreateProductionStatusPanePlacementOperations();
        return ProcessRuntimeParentDestroy(
            {
                context->ingress.enabled(),
                context->pid,
                context->tid,
                context->target,
                context->pane.hwnd,
                window,
                &context->signal_source,
            },
            {
                placement.get_window_thread_process_id,
                placement.get_class_name,
                placement.get_parent,
                [](const HWND candidate) { return GetAncestor(candidate, GA_ROOT); },
                placement.set_parent,
                placement.set_window_pos,
            },
            [context] {
                return context->ingress.SignalEvent(context->refresh_event.get());
            },
            [window, message, wparam, lparam] {
                return DefSubclassProc(window, message, wparam, lparam);
            });
    }
    if (id == kRuntimeParentSubclassId && context != nullptr &&
        window == context->signal_source.parent &&
        (message == WM_SIZE || message == WM_DPICHANGED ||
         message == WM_THEMECHANGED || message == WM_SHOWWINDOW ||
         message == WM_WINDOWPOSCHANGED)) {
        static_cast<void>(context->ingress.SignalEvent(context->refresh_event.get()));
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

Status UpdateRuntimeParentSubclass(
    RuntimeContext* const context,
    const HWND parent) {
    return UpdateRuntimeSignalParent(
        &context->signal_source,
        parent,
        {
            [](const HWND window) {
                return RemoveWindowSubclass(
                           window,
                           RuntimeParentSubclassProc,
                           kRuntimeParentSubclassId) != FALSE
                    ? Success()
                    : PlacementFailure(GetLastError());
            },
            [context](const HWND window) {
                return SetWindowSubclass(
                           window,
                           RuntimeParentSubclassProc,
                           kRuntimeParentSubclassId,
                           reinterpret_cast<DWORD_PTR>(context)) != FALSE
                    ? Success()
                    : PlacementFailure(GetLastError());
            },
        });
}

DWORD WINAPI RefreshWorker(void* const parameter) {
    auto* const context = static_cast<RuntimeContext*>(parameter);
    const HANDLE waits[]{context->refresh_stop.get(), context->refresh_event.get()};
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        bool captureRequested = true;
        if (context->ingress.Consume()) {
            captureRequested = false;
            static_cast<void>(context->refresh.Signal(
                WM_WINDOWPOSCHANGED,
                [&captureRequested] {
                    captureRequested = true;
                    return Success();
                }));
        }
        if (!captureRequested) {
            continue;
        }
        ExplorerLayoutMetrics metrics{};
        HWND parent = nullptr;
        RECT rect{};
        Status status = CaptureExplorerLayout(context->target, &metrics, &parent);
        if (status.ok()) {
            status = ComputeStatusPaneRect(metrics, &rect);
        }
        if (status.ok()) {
            const StatusPanePlacementResult result{
                context->pid,
                context->tid,
                context->target,
                parent,
                rect,
                rect.right > rect.left && rect.bottom > rect.top,
            };
            static_cast<void>(context->refresh.Publish(
                result,
                [](const HWND pane, const UINT message) {
                    return PostRuntimeMessage(pane, message);
                }));
        } else {
            static_cast<void>(context->refresh.CaptureFailed(
                [context] { return WakeRefreshWorker(context); }));
        }
    }
    return 0;
}

Status WriteAttachResult(
    const HANDLE pipe,
    const RuntimeContext& context,
    const std::uint32_t result,
    const std::string& error) {
    std::vector<std::uint8_t> frame;
    Status status = ipc::EncodeAttachResult(
        context.attach_id,
        {
            context.pid,
            context.tid,
            reinterpret_cast<std::uint64_t>(context.target),
            result,
            error,
        },
        &frame);
    return status.ok() ? ipc::WriteFrame(pipe, frame) : status;
}

Status DispatchTabSetUpdate(
    RuntimeContext* const context,
    const ipc::TabSetUpdate& update,
    ipc::TabSetResult* const result) {
    if (context == nullptr || result == nullptr) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    {
        const std::scoped_lock lock{context->tab_update_mutex};
        if (context->pending_tab_update) {
            return PlacementFailure(ERROR_BUSY);
        }
        context->pending_tab_update = update;
        context->tab_update_result = {};
    }
    if (ResetEvent(context->tab_update_ack.get()) == FALSE ||
        PostMessageW(context->pane.hwnd, kStatusPaneTabSetMessage, 0, 0) == FALSE) {
        const DWORD error = GetLastError();
        const std::scoped_lock lock{context->tab_update_mutex};
        context->pending_tab_update.reset();
        return PlacementFailure(error);
    }
    if (WaitForSingleObject(context->tab_update_ack.get(), 5000) != WAIT_OBJECT_0) {
        return PlacementFailure(ERROR_TIMEOUT);
    }
    const std::scoped_lock lock{context->tab_update_mutex};
    *result = context->tab_update_result;
    return Success();
}

DWORD WINAPI RuntimeWorker(void* const parameter) {
    std::unique_ptr<RuntimeContext> context{
        static_cast<RuntimeContext*>(parameter)};
    std::wstring pipeName;
    UniqueHandle pipe;
    Status status = ipc::BuildCurrentUserPipeName(&pipeName);
    if (status.ok()) {
        status = ipc::ConnectHookPipeClient(pipeName, &pipe);
    }
    if (status.ok()) {
        status = WriteAttachResult(pipe.get(), *context, 0, {});
    }
    if (!status.ok() || !context->state.MarkRunning(true).ok()) {
        context.release();
        return 1;
    }

    context->refresh_worker.reset(CreateThread(
        nullptr, 0, RefreshWorker, context.get(), 0, nullptr));
    if (!context->refresh_worker ||
        !context->refresh.Signal(
            WM_SIZE, [&context] { return WakeRefreshWorker(context.get()); }).ok()) {
        context.release();
        return 1;
    }

    ipc::DecodedFrame request{};
    bool detachRequested = false;
    while (status.ok()) {
        status = ipc::ReadFrame(&pipe, &request);
        if (!status.ok()) {
            break;
        }
        if (ipc::DecodeDetachRequest(request).ok()) {
            detachRequested = true;
            break;
        }
        ipc::TabSetUpdate update{};
        status = ipc::DecodeTabSetUpdate(request, &update);
        if (!status.ok()) {
            break;
        }
        ipc::TabSetResult result{};
        status = DispatchTabSetUpdate(context.get(), update, &result);
        if (!status.ok()) {
            break;
        }
        std::vector<std::uint8_t> response;
        status = ipc::EncodeTabSetResult(request.request_id, result, &response);
        if (status.ok()) {
            status = ipc::WriteFrame(pipe.get(), response);
        }
    }
    if (!context->state.BeginStop().ok()) {
        context.release();
        return 1;
    }
    g_callback_gate.RejectNewWork();
    context->ingress.Disable();
    if (!detachRequested) {
        pipe.reset();
    }

    context->refresh.Stop();
    if (SetEvent(context->refresh_stop.get()) == FALSE ||
        WaitForSingleObject(context->refresh_worker.get(), 5000) != WAIT_OBJECT_0 ||
        PostMessageW(
            context->pane.hwnd,
            kStatusPaneRuntimeCleanupMessage,
            0,
            0) == FALSE ||
        WaitForSingleObject(context->parent_cleanup_ack.get(), 5000) !=
            WAIT_OBJECT_0) {
        context.release();
        return 1;
    }

    status = RequestStatusPaneRemoval(context->pane.hwnd);
    if (!status.ok() ||
        WaitForSingleObject(context->cleanup_ack.get(), 5000) != WAIT_OBJECT_0) {
        context.release();
        return 1;
    }
    context->pane = {};
    if (!DrainHookCallbacksForUnload(g_callback_gate, 5000).ok()) {
        context.release();
        return 1;
    }
    if (WaitForSingleObject(context->hook_released.get(), 0) != WAIT_OBJECT_0) {
        context.release();
        return 1;
    }

    if (detachRequested) {
        std::vector<std::uint8_t> response;
        status = ipc::EncodeDetachResult(
            request.request_id,
            {context->pid, 0, {}},
            &response);
        if (status.ok()) {
            status = ipc::WriteFrame(pipe.get(), response);
        }
        if (!status.ok()) {
            context.release();
            return 1;
        }
    }
    pipe.reset();
    static_cast<void>(context->state.MarkStopped());
    const HMODULE module = context->module_reference;
    g_runtime.store(nullptr, std::memory_order_release);
    context.reset();
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

bool HandleStatusPaneRuntimeMessage(
    const HWND pane,
    const UINT message) noexcept {
    RuntimeContext* const context = g_runtime.load(std::memory_order_acquire);
    if (context == nullptr || context->pane.hwnd != pane) {
        return false;
    }
    if (message == kStatusPaneRuntimeCleanupMessage) {
        if (!RemoveAllTabSubclasses(CreateProductionTabSubclassOperations()).ok()) {
            return true;
        }
        if (!RuntimeSignalCleanupSafe(context->signal_source)) {
            return true;
        }
        if (context->signal_source.parent != nullptr) {
            if (RemoveWindowSubclass(
                    context->signal_source.parent,
                    RuntimeParentSubclassProc,
                    kRuntimeParentSubclassId) == FALSE) {
                return true;
            }
            context->signal_source.parent = nullptr;
        }
        static_cast<void>(SetEvent(context->parent_cleanup_ack.get()));
        return true;
    }
    if (message == kStatusPaneTabSetMessage) {
        ipc::TabSetUpdate update{};
        {
            const std::scoped_lock lock{context->tab_update_mutex};
            if (!context->pending_tab_update) {
                return true;
            }
            update = *context->pending_tab_update;
        }
        ipc::TabSetResult result{};
        static_cast<void>(ApplyTabSetUpdate(
            context->target,
            update,
            CreateProductionTabSubclassOperations(),
            &result));
        {
            const std::scoped_lock lock{context->tab_update_mutex};
            context->tab_update_result = std::move(result);
            context->pending_tab_update.reset();
        }
        static_cast<void>(SetEvent(context->tab_update_ack.get()));
        return true;
    }
    if (message != kStatusPaneReflowMessage) {
        return false;
    }
    StatusPanePlacementResult result{};
    if (!context->refresh.Consume(&result)) {
        return true;
    }
    const bool accepted = ApplyStatusPanePlacementResult(pane, result).ok() &&
        UpdateRuntimeParentSubclass(context, result.parent).ok();
    static_cast<void>(context->refresh.ApplyCompleted(
        accepted, [context] { return WakeRefreshWorker(context); }));
    return true;
}

Status ApplyStatusPanePlacementResultWithOperations(
    const HWND pane,
    const StatusPanePlacementResult& result,
    const StatusPanePlacementOperations& operations) noexcept {
    if (!operations.get_current_process_id || !operations.get_current_thread_id ||
        result.process_id == 0 || result.thread_id == 0) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    if (operations.get_current_process_id() != result.process_id ||
        operations.get_current_thread_id() != result.thread_id) {
        return PlacementFailure(ERROR_INVALID_WINDOW_HANDLE);
    }

    const auto exactClass = [&](const HWND window, const std::wstring_view expected) {
        DWORD process = 0;
        if (window == nullptr ||
            operations.get_window_thread_process_id(window, &process) != result.thread_id ||
            process != result.process_id) {
            return false;
        }
        std::wstring actual;
        return operations.get_class_name(window, &actual).ok() && actual == expected;
    };
    if (!exactClass(result.top_level, L"CabinetWClass") ||
        !operations.is_window_visible(result.top_level) ||
        !exactClass(result.parent, kStatusPaneParentClassName) ||
        !operations.is_window_visible(result.parent)) {
        return PlacementFailure(ERROR_INVALID_WINDOW_HANDLE);
    }

    HWND activeTab = nullptr;
    for (HWND child = operations.get_first_child(result.top_level);
         child != nullptr;
         child = operations.get_next_sibling(child)) {
        std::wstring childClass;
        if (operations.get_class_name(child, &childClass).ok() &&
            childClass == L"ShellTabWindowClass" &&
            operations.is_window_visible(child)) {
            activeTab = child;
            break;
        }
    }
    if (!exactClass(activeTab, L"ShellTabWindowClass")) {
        return PlacementFailure(ERROR_INVALID_WINDOW_HANDLE);
    }
    HWND ancestor = result.parent;
    while (ancestor != nullptr && operations.get_parent(ancestor) != result.top_level) {
        ancestor = operations.get_parent(ancestor);
    }
    if (ancestor != activeTab) {
        return PlacementFailure(ERROR_INVALID_WINDOW_HANDLE);
    }
    return ApplyStatusPanePlacementWithOperations(
        pane, result.parent, result.rect, result.visible, operations);
}

Status ApplyStatusPanePlacementResult(
    const HWND pane,
    const StatusPanePlacementResult& result) noexcept {
    return ApplyStatusPanePlacementResultWithOperations(
        pane, result, CreateProductionStatusPanePlacementOperations());
}

StatusPaneRefreshCoordinator::StatusPaneRefreshCoordinator(const HWND pane) noexcept
    : pane_(pane) {}

void StatusPaneRefreshCoordinator::Initialize(const HWND pane) noexcept {
    const std::scoped_lock lock{mutex_};
    pane_ = pane;
    stopped_ = false;
}

void HookRuntimeRefreshIngress::Enable() noexcept {
    pending_.store(false, std::memory_order_release);
    enabled_.store(true, std::memory_order_release);
}

void HookRuntimeRefreshIngress::Disable() noexcept {
    enabled_.store(false, std::memory_order_release);
    pending_.store(false, std::memory_order_release);
}

Status HookRuntimeRefreshIngress::Signal(
    const std::function<Status()>& setEvent) noexcept {
    if (!setEvent || !enabled_.load(std::memory_order_acquire)) {
        return PlacementFailure(ERROR_INVALID_STATE);
    }
    bool expected = false;
    if (!pending_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return Success();
    }
    Status status{};
    try {
        status = setEvent();
    } catch (...) {
        status = PlacementFailure(ERROR_INVALID_FUNCTION);
    }
    if (!status.ok()) {
        pending_.store(false, std::memory_order_release);
    }
    return status;
}

Status HookRuntimeRefreshIngress::SignalEvent(const HANDLE event) noexcept {
    if (event == nullptr || !enabled_.load(std::memory_order_acquire)) {
        return PlacementFailure(ERROR_INVALID_STATE);
    }
    bool expected = false;
    if (!pending_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return Success();
    }
    if (SetEvent(event) == FALSE) {
        const DWORD error = GetLastError();
        pending_.store(false, std::memory_order_release);
        return PlacementFailure(error);
    }
    return Success();
}

bool HookRuntimeRefreshIngress::Consume() noexcept {
    return pending_.exchange(false, std::memory_order_acq_rel);
}

bool HookRuntimeRefreshIngress::enabled() const noexcept {
    return enabled_.load(std::memory_order_acquire);
}

Status StatusPaneRefreshCoordinator::Signal(
    const UINT message,
    const std::function<Status()>& wakeWorker) {
    if ((message != WM_SIZE && message != WM_DPICHANGED &&
         message != WM_THEMECHANGED && message != WM_SHOWWINDOW &&
         message != WM_WINDOWPOSCHANGED) || !wakeWorker) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    {
        const std::scoped_lock lock{mutex_};
        if (stopped_ || pane_ == nullptr) {
            return PlacementFailure(ERROR_INVALID_STATE);
        }
        dirty_ = true;
        automatic_retry_used_ = false;
        if (worker_active_ || dispatch_pending_) {
            return Success();
        }
        worker_active_ = true;
        dirty_ = false;
    }
    const Status status = wakeWorker();
    if (!status.ok()) {
        const std::scoped_lock lock{mutex_};
        worker_active_ = false;
        dirty_ = true;
    }
    return status;
}

Status StatusPaneRefreshCoordinator::Publish(
    const StatusPanePlacementResult& result,
    const StatusPanePostMessage& postMessage) {
    if (!postMessage) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    HWND pane = nullptr;
    {
        const std::scoped_lock lock{mutex_};
        worker_active_ = false;
        if (stopped_ || pane_ == nullptr) {
            return PlacementFailure(ERROR_INVALID_STATE);
        }
        result_ = result;
        if (dispatch_pending_) {
            return Success();
        }
        dispatch_pending_ = true;
        pane = pane_;
    }
    const Status status = postMessage(pane, kStatusPaneReflowMessage);
    if (!status.ok()) {
        const std::scoped_lock lock{mutex_};
        dispatch_pending_ = false;
        result_.reset();
    }
    return status;
}

bool StatusPaneRefreshCoordinator::Consume(
    StatusPanePlacementResult* const result) {
    if (result == nullptr) {
        return false;
    }
    {
        const std::scoped_lock lock{mutex_};
        if (stopped_ || !dispatch_pending_ || !result_) {
            return false;
        }
        *result = *result_;
        result_.reset();
        dispatch_pending_ = false;
        apply_pending_ = true;
    }
    return true;
}

Status StatusPaneRefreshCoordinator::CaptureFailed(
    const std::function<Status()>& wakeWorker) {
    if (!wakeWorker) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    bool wake = false;
    {
        const std::scoped_lock lock{mutex_};
        worker_active_ = false;
        if (stopped_) {
            return PlacementFailure(ERROR_INVALID_STATE);
        }
        if (dirty_) {
            dirty_ = false;
            worker_active_ = true;
            wake = true;
        }
    }
    if (!wake) {
        return Success();
    }
    const Status status = wakeWorker();
    if (!status.ok()) {
        const std::scoped_lock lock{mutex_};
        worker_active_ = false;
        dirty_ = true;
    }
    return status;
}

Status StatusPaneRefreshCoordinator::ApplyCompleted(
    const bool accepted,
    const std::function<Status()>& wakeWorker) {
    if (!wakeWorker) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    bool wake = false;
    {
        const std::scoped_lock lock{mutex_};
        if (stopped_ || !apply_pending_) {
            return PlacementFailure(ERROR_INVALID_STATE);
        }
        apply_pending_ = false;
        if (accepted) {
            automatic_retry_used_ = false;
        }
        if (dirty_) {
            dirty_ = false;
            worker_active_ = true;
            automatic_retry_used_ = false;
            wake = true;
        } else if (!accepted && !automatic_retry_used_) {
            automatic_retry_used_ = true;
            worker_active_ = true;
            wake = true;
        }
    }
    if (!wake) {
        return Success();
    }
    const Status status = wakeWorker();
    if (!status.ok()) {
        const std::scoped_lock lock{mutex_};
        worker_active_ = false;
        dirty_ = true;
    }
    return status;
}

void StatusPaneRefreshCoordinator::Stop() noexcept {
    const std::scoped_lock lock{mutex_};
    stopped_ = true;
    worker_active_ = false;
    dirty_ = false;
    dispatch_pending_ = false;
    apply_pending_ = false;
    result_.reset();
}

Status UpdateRuntimeSignalParent(
    RuntimeSignalSourceState* const state,
    const HWND parent,
    const RuntimeSignalSubclassOperations& operations) {
    if (state == nullptr || parent == nullptr || !operations.remove ||
        !operations.install) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    if (state->parent == parent && !state->lifecycle_failed) {
        return Success();
    }
    if (state->parent != nullptr) {
        const Status removal = operations.remove(state->parent);
        if (!removal.ok()) {
            state->lifecycle_failed = true;
            return removal;
        }
        state->parent = nullptr;
    }
    const Status install = operations.install(parent);
    if (!install.ok()) {
        state->lifecycle_failed = true;
        return install;
    }
    state->parent = parent;
    state->lifecycle_failed = false;
    return Success();
}

LRESULT ProcessRuntimeParentDestroy(
    const RuntimeParentDestroyContext& context,
    const RuntimeParentDestroyOperations& operations,
    const std::function<Status()>& signalRefresh,
    const std::function<LRESULT()>& callDefault) {
    if (!callDefault) {
        return 0;
    }
    RuntimeSignalSourceState* const source = context.signal_source;
    if (!context.active || source == nullptr || context.target == nullptr ||
        context.pane == nullptr || context.message_window == nullptr ||
        context.message_window == context.target ||
        context.message_window != source->parent) {
        return callDefault();
    }
    const auto fail = [&] {
        source->lifecycle_failed = true;
        source->cleanup_blocked = true;
        return callDefault();
    };
    if (context.process_id == 0 || context.thread_id == 0 ||
        !operations.get_window_thread_process_id || !operations.get_class_name ||
        !operations.get_parent || !operations.get_root || !operations.set_parent ||
        !operations.set_window_pos || !signalRefresh) {
        return fail();
    }
    const auto exactWindow = [&](const HWND window, const std::wstring_view expected) {
        DWORD process = 0;
        if (operations.get_window_thread_process_id(window, &process) !=
                context.thread_id ||
            process != context.process_id) {
            return false;
        }
        std::wstring className;
        return operations.get_class_name(window, &className).ok() &&
            className == expected;
    };
    if (!exactWindow(context.pane, kStatusPaneClassName) ||
        !exactWindow(context.target, L"CabinetWClass") ||
        !exactWindow(context.message_window, kStatusPaneParentClassName) ||
        operations.get_root(context.target) != context.target ||
        operations.get_root(context.message_window) != context.target ||
        operations.get_parent(context.pane) != context.message_window) {
        return fail();
    }
    Status status = operations.set_parent(context.pane, context.target);
    if (!status.ok()) {
        return fail();
    }
    status = operations.set_window_pos(
        context.pane,
        HWND_TOP,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    source->parent = nullptr;
    if (!status.ok()) {
        return fail();
    }
    source->lifecycle_failed = false;
    status = signalRefresh();
    if (!status.ok()) {
        return fail();
    }
    return callDefault();
}

bool RuntimeSignalCleanupSafe(
    const RuntimeSignalSourceState& state) noexcept {
    return !state.lifecycle_failed && !state.cleanup_blocked;
}

bool ShouldNotifyHookRuntimeWindowMessage(
    const HWND window,
    const UINT message,
    const HWND target,
    const DWORD expectedProcess,
    const DWORD expectedThread,
    const HookRuntimeWindowMessageOperations& operations) {
    if (window == nullptr || target == nullptr || expectedProcess == 0 ||
        expectedThread == 0 ||
        (message != WM_SHOWWINDOW && message != WM_WINDOWPOSCHANGED) ||
        !operations.get_window_thread_process_id || !operations.get_root ||
        !operations.get_class_name || !operations.is_window_visible) {
        return false;
    }
    DWORD process = 0;
    if (operations.get_window_thread_process_id(window, &process) != expectedThread ||
        process != expectedProcess || operations.get_root(window) != target ||
        !operations.is_window_visible(window)) {
        return false;
    }
    std::wstring className;
    if (!operations.get_class_name(window, &className).ok()) {
        return false;
    }
    return className == L"ShellTabWindowClass" ||
        className == kStatusPaneParentClassName;
}

void NotifyHookRuntimeWindowMessage(
    const HWND window,
    const UINT message) noexcept {
    try {
        RuntimeContext* const context = g_runtime.load(std::memory_order_acquire);
        if (context == nullptr) {
            return;
        }
        if (window == nullptr ||
            (message != WM_SHOWWINDOW && message != WM_WINDOWPOSCHANGED)) {
            return;
        }
        DWORD process = 0;
        if (GetWindowThreadProcessId(window, &process) != context->tid ||
            process != context->pid || GetAncestor(window, GA_ROOT) != context->target ||
            IsWindowVisible(window) == FALSE) {
            return;
        }
        wchar_t className[64]{};
        const int length = GetClassNameW(window, className, 64);
        if (length <= 0 ||
            (std::wstring_view{className, static_cast<std::size_t>(length)} !=
                 L"ShellTabWindowClass" &&
             std::wstring_view{className, static_cast<std::size_t>(length)} !=
                 kStatusPaneParentClassName)) {
            return;
        }
        static_cast<void>(context->ingress.SignalEvent(context->refresh_event.get()));
    } catch (...) {
        // Hook callbacks must remain no-throw and continue to CallNextHookEx.
    }
}

Status SignalHookRuntimeRefresh() noexcept {
    RuntimeContext* const context = g_runtime.load(std::memory_order_acquire);
    return context != nullptr
        ? context->ingress.SignalEvent(context->refresh_event.get())
        : PlacementFailure(ERROR_INVALID_STATE);
}

std::size_t StatusPaneRefreshCoordinator::pending_results() const noexcept {
    const std::scoped_lock lock{mutex_};
    return result_ ? 1u : 0u;
}

Status CleanupRuntimeRollback(
    const RuntimeRollbackPath,
    const StatusPaneOperations& operations,
    StatusPane* const pane,
    const std::function<void()>& releaseModule) {
    if (!releaseModule) {
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    }
    const Status cleanup = RemoveStatusPane(operations, pane);
    if (!cleanup.ok()) {
        return cleanup;
    }
    releaseModule();
    return Success();
}

bool HookCallbackGate::Enter() noexcept {
    std::uint64_t state = rundown_.load(std::memory_order_acquire);
    do {
        if ((state & kClosed) != 0 || (state & kCountMask) == kCountMask) {
            return false;
        }
    } while (!rundown_.compare_exchange_weak(
        state, state + 1, std::memory_order_acq_rel));
    return true;
}

void HookCallbackGate::Leave() noexcept {
    rundown_.fetch_sub(1, std::memory_order_acq_rel);
}

void HookCallbackGate::RejectNewWork() noexcept {
    rundown_.fetch_or(kClosed, std::memory_order_acq_rel);
}

bool HookCallbackGate::WaitForZero(const DWORD timeoutMs) const noexcept {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        if ((rundown_.load(std::memory_order_acquire) & kCountMask) == 0) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            return false;
        }
        Sleep(1);
    } while (true);
}

std::uint32_t HookCallbackGate::in_flight() const noexcept {
    return static_cast<std::uint32_t>(
        rundown_.load(std::memory_order_acquire) & kCountMask);
}

HookCallbackGate& ProcessHookCallbackGate() noexcept {
    return g_callback_gate;
}

Status DrainHookCallbacksForUnload(
    HookCallbackGate& gate,
    const DWORD timeoutMs) noexcept {
    return gate.WaitForZero(timeoutMs)
        ? Success()
        : Status{
              ErrorCode::DLL_UNLOAD_TIMEOUT,
              HRESULT_FROM_WIN32(ERROR_TIMEOUT),
              ERROR_TIMEOUT};
}

Status HookRuntimeStateMachine::BeginAttach() noexcept {
    if (state_ != RuntimeState::Stopped) {
        return ContractFailure();
    }
    state_ = RuntimeState::Starting;
    return Success();
}

Status HookRuntimeStateMachine::MarkRunning(const bool attachValidated) noexcept {
    if (state_ != RuntimeState::Starting || !attachValidated) {
        return ContractFailure();
    }
    state_ = RuntimeState::Running;
    return Success();
}

Status HookRuntimeStateMachine::BeginStop() noexcept {
    if (state_ != RuntimeState::Running) {
        return ContractFailure();
    }
    state_ = RuntimeState::Stopping;
    return Success();
}

Status HookRuntimeStateMachine::MarkStopped() noexcept {
    if (state_ != RuntimeState::Stopping) {
        return ContractFailure();
    }
    state_ = RuntimeState::Stopped;
    return Success();
}

RuntimeState HookRuntimeStateMachine::state() const noexcept {
    return state_;
}

Status BeginHookRuntimeAttach(
    const HWND target,
    const std::uint64_t attachId) noexcept {
    if (target == nullptr || attachId == 0 || g_hook_module == nullptr ||
        g_runtime.load(std::memory_order_acquire) != nullptr) {
        return ContractFailure();
    }
    auto context = std::make_unique<RuntimeContext>();
    context->target = target;
    context->attach_id = attachId;
    context->pid = GetCurrentProcessId();
    context->tid = GetCurrentThreadId();
    if (!context->state.BeginAttach().ok()) {
        return ContractFailure();
    }
    const std::wstring eventName =
        L"Local\\WinExInfo.HookReleased." + std::to_wstring(context->pid) +
        L"." + std::to_wstring(context->tid) + L"." +
        std::to_wstring(attachId);
    context->hook_released.reset(OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str()));
    if (!context->hook_released) {
        return {ErrorCode::DLL_INITIALIZATION_FAILED,
                HRESULT_FROM_WIN32(GetLastError()), GetLastError()};
    }
    HMODULE moduleReference = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&BeginHookRuntimeAttach),
            &moduleReference) == FALSE) {
        return {ErrorCode::DLL_INITIALIZATION_FAILED,
                HRESULT_FROM_WIN32(GetLastError()), GetLastError()};
    }
    context->module_reference = moduleReference;
    context->cleanup_ack.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    context->refresh_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    context->refresh_stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    context->parent_cleanup_ack.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    context->tab_update_ack.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!context->cleanup_ack || !context->refresh_event ||
        !context->refresh_stop || !context->parent_cleanup_ack ||
        !context->tab_update_ack) {
        FreeLibrary(moduleReference);
        return {ErrorCode::DLL_INITIALIZATION_FAILED,
                HRESULT_FROM_WIN32(GetLastError()), GetLastError()};
    }
    Status status = InstallStatusPane(
        target,
        CreateProductionStatusPaneOperations(
            g_hook_module, context->cleanup_ack.get()),
        &context->pane);
    if (!status.ok()) {
        FreeLibrary(moduleReference);
        return status;
    }
    context->refresh.Initialize(context->pane.hwnd);
    RuntimeContext* expected = nullptr;
    if (!g_runtime.compare_exchange_strong(
            expected, context.get(), std::memory_order_acq_rel)) {
        status = CleanupRuntimeRollback(
            RuntimeRollbackPath::CompareExchange,
            CreateProductionStatusPaneOperations(
                g_hook_module, context->cleanup_ack.get()),
            &context->pane,
            [moduleReference] { FreeLibrary(moduleReference); });
        if (!status.ok()) {
            context.release();
        }
        return ContractFailure();
    }
    const HANDLE worker = CreateThread(
        nullptr, 0, RuntimeWorker, context.get(), 0, nullptr);
    if (worker == nullptr) {
        const DWORD error = GetLastError();
        status = CleanupRuntimeRollback(
            RuntimeRollbackPath::WorkerCreation,
            CreateProductionStatusPaneOperations(
                g_hook_module, context->cleanup_ack.get()),
            &context->pane,
            [moduleReference] { FreeLibrary(moduleReference); });
        if (!status.ok()) {
            context.release();
        } else {
            g_runtime.store(nullptr, std::memory_order_release);
        }
        return {ErrorCode::DLL_INITIALIZATION_FAILED,
                HRESULT_FROM_WIN32(error), error};
    }
    CloseHandle(worker);
    context->ingress.Enable();
    context.release();
    return Success();
}

}  // namespace winexinfo::hook
