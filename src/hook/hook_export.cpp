#include "hook/hook_entry.h"
#include "hook/runtime.h"

#include "injection/thread_hook_injector.h"

#include <Windows.h>

extern "C" LRESULT CALLBACK WinExInfoCallWndProc(
    const int code,
    const WPARAM hookWparam,
    const LPARAM hookLparam) {
    HMODULE callbackReference = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&WinExInfoCallWndProc),
            &callbackReference) == FALSE) {
        return CallNextHookEx(nullptr, code, hookWparam, hookLparam);
    }
    auto& callbackGate = winexinfo::hook::ProcessHookCallbackGate();
    const bool accepting = callbackGate.Enter();
    if (accepting) {
        FreeLibrary(callbackReference);
    }
    struct CallbackExit final {
        winexinfo::hook::HookCallbackGate* gate;
        bool entered;
        ~CallbackExit() {
            if (entered) {
                gate->Leave();
            }
        }
    } callbackExit{&callbackGate, accepting};
    if (!accepting) {
        // A post-rundown callback retains its reference so unload fails safe.
        return CallNextHookEx(nullptr, code, hookWparam, hookLparam);
    }
    const UINT attachMessage = RegisterWindowMessageW(L"WinExInfo.Attach.v1");
    const winexinfo::hook::HookEntryOperations operations{
        [accepting](const HWND target, const std::uint64_t attachId) {
            return accepting
                ? winexinfo::hook::BeginHookRuntimeAttach(target, attachId)
                : winexinfo::Status{
                      winexinfo::ErrorCode::DLL_UNLOAD_TIMEOUT,
                      S_FALSE,
                      ERROR_INVALID_STATE};
        },
        [](const int nextCode, const WPARAM nextWparam, const LPARAM nextLparam) {
            return CallNextHookEx(nullptr, nextCode, nextWparam, nextLparam);
        },
    };
    return winexinfo::hook::ProcessHookCall(
        code, hookWparam, hookLparam, attachMessage, operations);
}
