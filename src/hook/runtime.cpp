#include "hook/runtime.h"

#include "common/win32_handle.h"
#include "hook/status_pane.h"
#include "ipc/named_pipe.h"
#include "ipc/protocol.h"

#include <Windows.h>

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

struct RuntimeContext final {
    HWND target = nullptr;
    std::uint64_t attach_id = 0;
    DWORD pid = 0;
    DWORD tid = 0;
    HMODULE module_reference = nullptr;
    UniqueHandle hook_released;
    UniqueHandle cleanup_ack;
    StatusPane pane{};
    HookRuntimeStateMachine state;
};

std::atomic<RuntimeContext*> g_runtime{nullptr};

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
    if (!status.ok() ||
        WaitForSingleObject(context->hook_released.get(), 5000) != WAIT_OBJECT_0 ||
        !context->state.MarkRunning(true).ok()) {
        context.release();
        return 1;
    }

    ipc::DecodedFrame request{};
    status = ipc::ReadFrame(&pipe, &request);
    const bool detachRequested = status.ok() &&
        ipc::DecodeDetachRequest(request).ok();
    if ((status.ok() && !detachRequested) ||
        !context->state.BeginStop().ok()) {
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

Status HookRuntimeStateMachine::BeginAttach() noexcept {
    if (state_ != RuntimeState::Stopped) {
        return ContractFailure();
    }
    state_ = RuntimeState::Starting;
    return Success();
}

Status HookRuntimeStateMachine::MarkRunning(const bool hookReleased) noexcept {
    if (state_ != RuntimeState::Starting || !hookReleased) {
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
    if (!context->cleanup_ack) {
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
    RuntimeContext* expected = nullptr;
    if (!g_runtime.compare_exchange_strong(
            expected, context.get(), std::memory_order_acq_rel)) {
        static_cast<void>(RemoveStatusPane(
            CreateProductionStatusPaneOperations(
                g_hook_module, context->cleanup_ack.get()),
            &context->pane));
        FreeLibrary(moduleReference);
        return ContractFailure();
    }
    const HANDLE worker = CreateThread(
        nullptr, 0, RuntimeWorker, context.get(), 0, nullptr);
    if (worker == nullptr) {
        g_runtime.store(nullptr, std::memory_order_release);
        static_cast<void>(RemoveStatusPane(
            CreateProductionStatusPaneOperations(
                g_hook_module, context->cleanup_ack.get()),
            &context->pane));
        FreeLibrary(moduleReference);
        return {ErrorCode::DLL_INITIALIZATION_FAILED,
                HRESULT_FROM_WIN32(GetLastError()), GetLastError()};
    }
    CloseHandle(worker);
    context.release();
    return Success();
}

}  // namespace winexinfo::hook
