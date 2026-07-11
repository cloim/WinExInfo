#include "hook_test_modes.h"

#include "injection/hook_platform.h"

#include <Windows.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace winexinfo::tests {
namespace {

constexpr wchar_t kTargetClass[] = L"WinExInfo.GateBTarget.v1";

LRESULT CALLBACK TargetWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    if (message == WM_CLOSE) {
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int RunHookTargetMode() {
    const DWORD pid = GetCurrentProcessId();
    const std::wstring readyName =
        L"Local\\WinExInfo.GateBTarget.Ready.v1." + std::to_wstring(pid);
    auto eventOperations = injection::CreateProductionHookPlatformOperations(
        {}, {});
    injection::HookReleaseEvent ready{};
    if (!eventOperations.create_release_event(readyName, &ready).ok() ||
        ready.handle == nullptr || ready.already_exists || !ready.manual_reset ||
        ready.initially_signaled || !ready.current_user_only ||
        ready.unexpected_principal_present) {
        if (ready.handle != nullptr) {
            eventOperations.close_event(ready.handle);
        }
        return 3;
    }

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const WNDCLASSEXW windowClass{
        sizeof(WNDCLASSEXW), 0, TargetWindowProc, 0, 0, instance,
        nullptr, nullptr, nullptr, nullptr, kTargetClass, nullptr};
    if (RegisterClassExW(&windowClass) == 0) {
        eventOperations.close_event(ready.handle);
        return 3;
    }
    const HWND window = CreateWindowExW(
        0, kTargetClass, L"WinExInfo Gate B Target", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 360, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        eventOperations.close_event(ready.handle);
        return 3;
    }
    ShowWindow(window, SW_SHOW);
    const DWORD tid = GetWindowThreadProcessId(window, nullptr);
    bool signaled = false;
    if (!eventOperations.set_event(ready.handle, &signaled).ok() || !signaled) {
        DestroyWindow(window);
        eventOperations.close_event(ready.handle);
        return 3;
    }
    std::ostringstream line;
    line << "READY protocol=1 pid=" << pid << " tid=" << tid
         << " hwnd=0x" << std::uppercase << std::hex << std::setw(16)
         << std::setfill('0') << reinterpret_cast<std::uintptr_t>(window);
    std::cout << line.str() << '\n' << std::flush;

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    eventOperations.close_event(ready.handle);
    return 0;
}

}  // namespace winexinfo::tests
