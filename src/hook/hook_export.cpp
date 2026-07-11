#include "hook/hook_entry.h"
#include "hook/runtime.h"

#include "injection/thread_hook_injector.h"

#include <Windows.h>

extern "C" LRESULT CALLBACK WinExInfoCallWndProc(
    const int code,
    const WPARAM hookWparam,
    const LPARAM hookLparam) {
    const UINT attachMessage = RegisterWindowMessageW(L"WinExInfo.Attach.v1");
    const winexinfo::hook::HookEntryOperations operations{
        [](const HWND target, const std::uint64_t attachId) {
            return winexinfo::hook::BeginHookRuntimeAttach(target, attachId);
        },
        [](const int nextCode, const WPARAM nextWparam, const LPARAM nextLparam) {
            return CallNextHookEx(nullptr, nextCode, nextWparam, nextLparam);
        },
    };
    return winexinfo::hook::ProcessHookCall(
        code, hookWparam, hookLparam, attachMessage, operations);
}
