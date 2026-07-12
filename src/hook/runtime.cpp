#include "hook/runtime.h"

#include "common/win32_handle.h"
#include "hook/explorer_layout.h"
#include "hook/process_runtime.h"
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

struct RuntimeWindowResources final {
    WindowRuntimeKey key{};
    UniqueHandle cleanup_ack;
    UniqueHandle refresh_event;
    UniqueHandle refresh_stop;
    UniqueHandle refresh_worker;
    UniqueHandle parent_cleanup_ack;
    StatusPane pane{};
    StatusPaneRefreshCoordinator refresh;
    RuntimeSignalSourceState signal_source;
    HookRuntimeRefreshIngress ingress;
    bool worker_started = false;
    bool ui_cleaned = false;
};

struct RuntimeContext final {
    HWND initial_target = nullptr;
    std::uint64_t attach_id = 0;
    DWORD pid = 0;
    DWORD initial_tid = 0;
    HMODULE module_reference = nullptr;
    UniqueHandle hook_released;
    HookRuntimeStateMachine state;
    ProcessRuntime process;
};

std::atomic<RuntimeContext*> g_runtime{nullptr};
HookCallbackGate g_callback_gate;
inline constexpr UINT_PTR kRuntimeParentSubclassId = 0x57495832;

RuntimeWindowResources* Resources(WindowRuntime* window) noexcept {
    return window ? static_cast<RuntimeWindowResources*>(window->resources.get()) : nullptr;
}

Status PostRuntimeMessage(const HWND pane, const UINT message) {
    return PostMessageW(pane, message, 0, 0) != FALSE
        ? Success()
        : PlacementFailure(GetLastError());
}

Status WakeRefreshWorker(RuntimeWindowResources* const resources) {
    return resources && SetEvent(resources->refresh_event.get()) != FALSE
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
    auto* const resources = reinterpret_cast<RuntimeWindowResources*>(reference);
    if (id == kRuntimeParentSubclassId && resources && resources->key.top_level &&
        message == WM_DESTROY) {
        const StatusPanePlacementOperations placement =
            CreateProductionStatusPanePlacementOperations();
        return ProcessRuntimeParentDestroy(
            {
                resources->ingress.enabled(),
                resources->key.process_id,
                resources->key.ui_thread_id,
                resources->key.top_level,
                resources->pane.hwnd,
                window,
                &resources->signal_source,
            },
            {
                placement.get_window_thread_process_id,
                placement.get_class_name,
                placement.get_parent,
                [](const HWND candidate) { return GetAncestor(candidate, GA_ROOT); },
                placement.set_parent,
                placement.set_window_pos,
            },
            [resources] {
                return resources->ingress.SignalEvent(resources->refresh_event.get());
            },
            [window, message, wparam, lparam] {
                return DefSubclassProc(window, message, wparam, lparam);
            });
    }
    if (id == kRuntimeParentSubclassId && resources &&
        window == resources->signal_source.parent &&
        (message == WM_SIZE || message == WM_DPICHANGED ||
         message == WM_THEMECHANGED || message == WM_SHOWWINDOW ||
         message == WM_WINDOWPOSCHANGED)) {
        static_cast<void>(resources->ingress.SignalEvent(resources->refresh_event.get()));
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

Status UpdateRuntimeParentSubclass(
    RuntimeWindowResources* const resources,
    const HWND parent) {
    return UpdateRuntimeSignalParent(
        &resources->signal_source,
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
            [resources](const HWND window) {
                return SetWindowSubclass(
                           window,
                           RuntimeParentSubclassProc,
                           kRuntimeParentSubclassId,
                           reinterpret_cast<DWORD_PTR>(resources)) != FALSE
                    ? Success()
                    : PlacementFailure(GetLastError());
            },
        });
}

DWORD WINAPI RefreshWorker(void* const parameter) {
    auto* const resources = static_cast<RuntimeWindowResources*>(parameter);
    const HANDLE waits[]{resources->refresh_stop.get(), resources->refresh_event.get()};
    for (;;) {
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 5000);
        if (wait == WAIT_OBJECT_0) return 0;
        if (wait == WAIT_TIMEOUT) continue;
        if (wait != WAIT_OBJECT_0 + 1) return 1;
        bool captureRequested = true;
        if (resources->ingress.Consume()) {
            captureRequested = false;
            static_cast<void>(resources->refresh.Signal(
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
        Status status = CaptureExplorerLayout(resources->key.top_level, &metrics, &parent);
        if (status.ok()) {
            status = ComputeStatusPaneRect(metrics, &rect);
        }
        if (status.ok()) {
            const StatusPanePlacementResult result{
                resources->key.process_id,
                resources->key.ui_thread_id,
                resources->key.top_level,
                parent,
                rect,
                rect.right > rect.left && rect.bottom > rect.top,
            };
            static_cast<void>(resources->refresh.Publish(
                result,
                [](const HWND pane, const UINT message) {
                    return PostRuntimeMessage(pane, message);
                }));
        } else {
            static_cast<void>(resources->refresh.CaptureFailed(
                [resources] { return WakeRefreshWorker(resources); }));
        }
    }
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
            context.initial_tid,
            reinterpret_cast<std::uint64_t>(context.initial_target),
            result,
            error,
        },
        &frame);
    return status.ok() ? ipc::WriteFrame(pipe, frame) : status;
}

Status StopWindowWorker(RuntimeWindowResources* resources) {
    if (!resources) return PlacementFailure(ERROR_INVALID_PARAMETER);
    resources->ingress.Disable();
    resources->refresh.Stop();
    if (!resources->worker_started) return Success();
    if (SetEvent(resources->refresh_stop.get()) == FALSE ||
        WaitForSingleObject(resources->refresh_worker.get(), 5000) != WAIT_OBJECT_0)
        return PlacementFailure(ERROR_TIMEOUT);
    resources->worker_started = false;
    resources->refresh_worker.reset();
    return Success();
}

Status CreateRuntimeWindow(const WindowRuntimeKey& key,
                           std::unique_ptr<WindowRuntime>* output) {
    if (!output || !key.top_level || !key.process_id || !key.ui_thread_id ||
        GetCurrentProcessId() != key.process_id || GetCurrentThreadId() != key.ui_thread_id)
        return PlacementFailure(ERROR_INVALID_PARAMETER);
    DWORD pid = 0;
    wchar_t name[64]{};
    const int length = GetClassNameW(key.top_level, name, 64);
    const std::wstring_view actual{name, length > 0 ? static_cast<std::size_t>(length) : 0};
    if (GetWindowThreadProcessId(key.top_level, &pid) != key.ui_thread_id ||
        pid != key.process_id ||
        (actual != L"CabinetWClass" &&
         !(key.generation == 0 && actual == L"WinExInfo.GateBTarget.v1")))
        return PlacementFailure(ERROR_INVALID_WINDOW_HANDLE);
    auto window = std::make_unique<WindowRuntime>();
    window->key = key;
    auto resources = std::make_shared<RuntimeWindowResources>();
    resources->key = key;
    window->resources = resources;
    *output = std::move(window);
    resources->cleanup_ack.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    resources->refresh_event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    resources->refresh_stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    resources->parent_cleanup_ack.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!resources->cleanup_ack || !resources->refresh_event ||
        !resources->refresh_stop || !resources->parent_cleanup_ack)
        return PlacementFailure(GetLastError());
    Status status = InstallStatusPane(key.top_level,
        CreateProductionStatusPaneOperations(g_hook_module, resources->cleanup_ack.get()),
        &resources->pane);
    if (status.ok()) resources->refresh.Initialize(resources->pane.hwnd);
    return status;
}

Status ActivateRuntimeWindow(WindowRuntime& window) {
    RuntimeWindowResources* resources = Resources(&window);
    if (!resources || resources->worker_started || resources->ui_cleaned)
        return PlacementFailure(ERROR_INVALID_STATE);
    resources->refresh_worker.reset(CreateThread(nullptr, 0, RefreshWorker, resources, 0, nullptr));
    if (!resources->refresh_worker) return PlacementFailure(GetLastError());
    resources->worker_started = true;
    resources->ingress.Enable();
    return resources->refresh.Signal(WM_SIZE, [resources] { return WakeRefreshWorker(resources); });
}

Status ApplyRuntimeWindowUpdate(WindowRuntime& window, const ipc::TabSetUpdate& update,
                                ipc::TabSetResult* result) {
    return window.tab_subclasses->Apply(window.key.top_level, update,
        CreateProductionTabSubclassOperations(*window.tab_subclasses), result);
}

Status CleanupRuntimeWindowOnUi(WindowRuntime& window) {
    RuntimeWindowResources* resources = Resources(&window);
    if (!resources) return PlacementFailure(ERROR_INVALID_STATE);
    if (resources->ui_cleaned) return Success();
    Status status = StopWindowWorker(resources);
    if (!status.ok()) return status;
    status = window.tab_subclasses->RemoveAll(
        CreateProductionTabSubclassOperations(*window.tab_subclasses));
    if (!status.ok() || !window.tab_subclasses->cleanup_safe())
        return status.ok() ? PlacementFailure(ERROR_INVALID_STATE) : status;
    if (!RuntimeSignalCleanupSafe(resources->signal_source))
        return PlacementFailure(ERROR_INVALID_STATE);
    if (resources->signal_source.parent) {
        if (RemoveWindowSubclass(resources->signal_source.parent,
                RuntimeParentSubclassProc, kRuntimeParentSubclassId) == FALSE)
            return PlacementFailure(GetLastError());
        resources->signal_source.parent = nullptr;
    }
    status = RemoveStatusPane(CreateProductionStatusPaneOperations(
        g_hook_module, resources->cleanup_ack.get()), &resources->pane);
    if (status.ok()) resources->ui_cleaned = true;
    return status;
}

Status CleanupRuntimeWindowFromWorker(WindowRuntime& window) {
    RuntimeWindowResources* resources = Resources(&window);
    if (!resources) return PlacementFailure(ERROR_INVALID_STATE);
    if (resources->ui_cleaned) return Success();
    Status status = StopWindowWorker(resources);
    if (!status.ok()) return status;
    if (PostMessageW(resources->pane.hwnd, kStatusPaneRuntimeCleanupMessage, 0, 0) == FALSE ||
        WaitForSingleObject(resources->parent_cleanup_ack.get(), 5000) != WAIT_OBJECT_0)
        return PlacementFailure(ERROR_TIMEOUT);
    status = RequestStatusPaneRemoval(resources->pane.hwnd);
    if (!status.ok() || WaitForSingleObject(resources->cleanup_ack.get(), 5000) != WAIT_OBJECT_0)
        return status.ok() ? PlacementFailure(ERROR_TIMEOUT) : status;
    resources->pane = {};
    resources->ui_cleaned = true;
    return Success();
}

Status ShelterRuntimeWindowOnDestroy(WindowRuntime& window) {
    RuntimeWindowResources* resources = Resources(&window);
    if (!resources || resources->ui_cleaned || !resources->pane.hwnd ||
        GetCurrentProcessId() != window.key.process_id ||
        GetCurrentThreadId() != window.key.ui_thread_id)
        return PlacementFailure(ERROR_INVALID_STATE);
    if (resources->signal_source.parent) {
        if (RemoveWindowSubclass(resources->signal_source.parent,
                RuntimeParentSubclassProc, kRuntimeParentSubclassId) == FALSE)
            return PlacementFailure(GetLastError());
        resources->signal_source.parent = nullptr;
    }
    SetLastError(ERROR_SUCCESS);
    if (SetParent(resources->pane.hwnd, HWND_MESSAGE) == nullptr &&
        GetLastError() != ERROR_SUCCESS)
        return PlacementFailure(GetLastError());
    if (SetWindowPos(resources->pane.hwnd, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_HIDEWINDOW) == FALSE)
        return PlacementFailure(GetLastError());
    resources->signal_source.lifecycle_failed = false;
    return Success();
}

DWORD WINAPI RuntimeWorker(void* const parameter) {
    std::unique_ptr<RuntimeContext> context{
        static_cast<RuntimeContext*>(parameter)};
    std::wstring pipeName;
    UniqueHandle pipe;
    Status status = ipc::BuildCurrentUserPipeNameForProcess(
        context->pid, &pipeName);
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
        if (request.message_type == ipc::MessageType::WindowRemoveRequest) {
            ipc::WindowRemoveRequest removal{};
            status = ipc::DecodeWindowRemoveRequest(request, &removal);
            ipc::WindowRemoveResult result{};
            if (status.ok()) {
                status = DispatchProcessWindowRemovalByIdentity(
                    context->process, removal, &result);
            }
            std::vector<std::uint8_t> response;
            if (status.ok()) {
                status = ipc::EncodeWindowRemoveResult(
                    request.request_id, result, &response);
            }
            if (status.ok()) status = ipc::WriteFrame(pipe.get(), response);
            if (status.ok()) continue;
            break;
        }
        ipc::TabSetUpdate update{};
        status = ipc::DecodeTabSetUpdate(request, &update);
        if (!status.ok()) {
            break;
        }
        ipc::TabSetResult result{};
        status = DispatchProcessTabSetUpdate(context->process, update, &result);
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
    if (!detachRequested) {
        pipe.reset();
    }
    status = RemoveAllProcessWindows(context->process);
    if (!status.ok()) {
        context.release();
        return 1;
    }
    g_callback_gate.RejectNewWork();
    if (!DrainHookCallbacksForUnload(g_callback_gate, 5000).ok()) {
        context.release();
        return 1;
    }
    if (!ReapRemovedProcessWindows(context->process, 5000).ok()) {
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
    if (!FinalizeProcessWindowsAfterDrain(context->process, 5000).ok()) {
        context.release(); return 1;
    }
    SetProcessRuntimeForCallbacks(nullptr);
    g_runtime.store(nullptr, std::memory_order_release);
    context.reset();
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

bool HandleStatusPaneRuntimeMessage(
    const HWND pane,
    const UINT message) noexcept {
    RuntimeContext* const context = g_runtime.load(std::memory_order_acquire);
    if (!context) {
        return false;
    }
    WindowRuntime* window = nullptr;
    RuntimeWindowResources* resources = nullptr;
    WindowRuntimeStorageLease storageLease;
    for (std::size_t index = 0; index < kMaximumRuntimeWindows; ++index) {
        auto candidateLease = AcquireProcessWindowStorageAt(context->process, index);
        WindowRuntime* candidate = candidateLease.get();
        RuntimeWindowResources* candidateResources = Resources(candidate);
        if (candidateResources && candidateResources->pane.hwnd == pane) {
            window = candidate; resources = candidateResources;
            storageLease = std::move(candidateLease); break;
        }
    }
    if (!window || !resources) return false;
    if (message == kStatusPaneRuntimeCleanupMessage) {
        if (!window->tab_subclasses->RemoveAll(
                 CreateProductionTabSubclassOperations(
                     *window->tab_subclasses)).ok() ||
            !window->tab_subclasses->cleanup_safe()) {
            return true;
        }
        if (!RuntimeSignalCleanupSafe(resources->signal_source)) {
            return true;
        }
        if (resources->signal_source.parent != nullptr) {
            if (RemoveWindowSubclass(
                    resources->signal_source.parent,
                    RuntimeParentSubclassProc,
                    kRuntimeParentSubclassId) == FALSE) {
                return true;
            }
            resources->signal_source.parent = nullptr;
        }
        static_cast<void>(SetEvent(resources->parent_cleanup_ack.get()));
        return true;
    }
    if (message != kStatusPaneReflowMessage) {
        return false;
    }
    StatusPanePlacementResult result{};
    if (!resources->refresh.Consume(&result)) {
        return true;
    }
    const bool accepted = ApplyStatusPanePlacementResult(pane, result).ok() &&
        UpdateRuntimeParentSubclass(resources, result.parent).ok();
    static_cast<void>(resources->refresh.ApplyCompleted(
        accepted, [resources] { return WakeRefreshWorker(resources); }));
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
        if (!window) {
            return;
        }
        for (std::size_t index = 0; index < kMaximumRuntimeWindows; ++index) {
            auto lease = AcquireProcessWindowCallbackAt(context->process, index);
            WindowRuntime* candidate = lease.get();
            if (!candidate) continue;
            if (window == candidate->key.top_level &&
                (message == WM_DESTROY || message == WM_NCDESTROY)) {
                lease = {};
                static_cast<void>(HandleProcessRuntimeTopLevelDestroy(context->process, window));
                return;
            }
            if (message != WM_SHOWWINDOW && message != WM_WINDOWPOSCHANGED) continue;
            DWORD process = 0;
            if (GetWindowThreadProcessId(window, &process) != candidate->key.ui_thread_id ||
                process != candidate->key.process_id ||
                GetAncestor(window, GA_ROOT) != candidate->key.top_level ||
                IsWindowVisible(window) == FALSE) continue;
            wchar_t className[64]{};
            const int length = GetClassNameW(window, className, 64);
            if (length <= 0 ||
                (std::wstring_view{className, static_cast<std::size_t>(length)} != L"ShellTabWindowClass" &&
                 std::wstring_view{className, static_cast<std::size_t>(length)} != kStatusPaneParentClassName)) continue;
            RuntimeWindowResources* resources = Resources(candidate);
            if (resources) static_cast<void>(resources->ingress.SignalEvent(resources->refresh_event.get()));
            return;
        }
    } catch (...) {
        // Hook callbacks must remain no-throw and continue to CallNextHookEx.
    }
}

Status SignalHookRuntimeRefresh() noexcept {
    RuntimeContext* const context = g_runtime.load(std::memory_order_acquire);
    if (!context || context->process.active_window_count() != 1) return PlacementFailure(ERROR_INVALID_STATE);
    for (std::size_t i = 0; i < kMaximumRuntimeWindows; ++i) {
        auto lease = AcquireProcessWindowCallbackAt(context->process, i);
        WindowRuntime* window = lease.get();
        RuntimeWindowResources* resources = Resources(window);
        if (lease && resources)
            return resources->ingress.SignalEvent(resources->refresh_event.get());
    }
    return PlacementFailure(ERROR_INVALID_STATE);
}

Status SignalHookRuntimeRefresh(TabSubclassSet* owner) noexcept {
    RuntimeContext* const context = g_runtime.load(std::memory_order_acquire);
    if (!context || !owner) return PlacementFailure(ERROR_INVALID_STATE);
    for (std::size_t i = 0; i < kMaximumRuntimeWindows; ++i) {
        auto lease = AcquireProcessWindowCallbackAt(context->process, i);
        WindowRuntime* window = lease.get();
        RuntimeWindowResources* resources = Resources(window);
        if (window && resources && window->tab_subclasses.get() == owner)
            return resources->ingress.SignalEvent(resources->refresh_event.get());
    }
    return PlacementFailure(ERROR_INVALID_STATE);
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

void HookCallbackGate::ResetForReuse() noexcept {
    rundown_.store(0, std::memory_order_release);
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
    context->initial_target = target;
    context->attach_id = attachId;
    context->pid = GetCurrentProcessId();
    context->initial_tid = GetCurrentThreadId();
    if (!context->state.BeginAttach().ok()) {
        return ContractFailure();
    }
    const std::wstring eventName =
        L"Local\\WinExInfo.HookReleased." + std::to_wstring(context->pid) +
        L"." + std::to_wstring(context->initial_tid) + L"." +
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
    RuntimeContext* expected = nullptr;
    if (!g_runtime.compare_exchange_strong(
            expected, context.get(), std::memory_order_acq_rel)) {
        FreeLibrary(moduleReference);
        return ContractFailure();
    }
    context->process.process_id = context->pid;
    context->process.control_message = RegisterWindowMessageW(kProcessRuntimeControlMessageName.data());
    context->process.operations.get_current_process_id = &GetCurrentProcessId;
    context->process.operations.get_current_thread_id = &GetCurrentThreadId;
    context->process.operations.get_window_thread_process_id = &GetWindowThreadProcessId;
    context->process.operations.send_control = [](HWND window, UINT message, WPARAM wp, LPARAM lp,
                                                  UINT flags, UINT timeout, DWORD_PTR* result) {
        if (SendMessageTimeoutW(window, message, wp, lp, flags, timeout, result) == 0) {
            DWORD error = GetLastError();
            return PlacementFailure(error ? error : ERROR_TIMEOUT);
        }
        return Success();
    };
    context->process.operations.create_window = &CreateRuntimeWindow;
    context->process.operations.apply_update = &ApplyRuntimeWindowUpdate;
    context->process.operations.activate_window = &ActivateRuntimeWindow;
    context->process.operations.cleanup_window_on_ui = &CleanupRuntimeWindowOnUi;
    context->process.operations.cleanup_window_from_worker = &CleanupRuntimeWindowFromWorker;
    context->process.operations.shelter_window_on_destroy = &ShelterRuntimeWindowOnDestroy;
    SetProcessRuntimeForCallbacks(&context->process);
    std::unique_ptr<WindowRuntime> initial;
    Status status = context->process.control_message
        ? CreateRuntimeWindow({target, context->pid, context->initial_tid, 0}, &initial)
        : PlacementFailure(GetLastError());
    if (status.ok()) status = RegisterInitialProvisionalProcessWindow(context->process, std::move(initial));
    if (status.ok()) status = ActivateAndPublishInitialProcessWindow(context->process, target);
    if (!status.ok()) {
        Status cleanup = Success();
        if (initial) cleanup = CleanupRuntimeWindowOnUi(*initial);
        else cleanup = RetireProcessWindowOnCurrentUi(context->process, target);
        if (!cleanup.ok()) {
            if (initial) {
                static_cast<void>(RegisterInitialProvisionalProcessWindow(
                    context->process, std::move(initial)));
            }
            context.release(); return status;
        }
        Status drain = ReapRemovedProcessWindows(context->process, 5000);
        if (drain.ok()) drain = FinalizeProcessWindowsAfterDrain(context->process, 5000);
        if (!drain.ok()) { context.release(); return status; }
        SetProcessRuntimeForCallbacks(nullptr);
        g_runtime.store(nullptr, std::memory_order_release);
        FreeLibrary(moduleReference);
        return status;
    }
    const HANDLE worker = CreateThread(
        nullptr, 0, RuntimeWorker, context.get(), 0, nullptr);
    if (worker == nullptr) {
        const DWORD error = GetLastError();
        status = RetireProcessWindowOnCurrentUi(context->process, target);
        if (!status.ok()) { context.release(); return PlacementFailure(error); }
        Status drain = ReapRemovedProcessWindows(context->process, 5000);
        if (drain.ok()) drain = FinalizeProcessWindowsAfterDrain(context->process, 5000);
        if (!drain.ok()) { context.release(); return PlacementFailure(error); }
        SetProcessRuntimeForCallbacks(nullptr);
        g_runtime.store(nullptr, std::memory_order_release);
        FreeLibrary(moduleReference);
        return {ErrorCode::DLL_INITIALIZATION_FAILED,
                HRESULT_FROM_WIN32(error), error};
    }
    CloseHandle(worker);
    context.release();
    return Success();
}

}  // namespace winexinfo::hook
