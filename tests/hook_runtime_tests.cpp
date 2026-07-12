#include "test_framework.h"

#include "hook/hook_entry.h"
#include "hook/runtime.h"
#include "hook/status_pane.h"
#include "hook_test_modes.h"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

winexinfo::Status Success() {
    return {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

winexinfo::Status ParseHookCommand(
    const std::initializer_list<std::string_view> arguments,
    winexinfo::tests::HookTestCommand* const output) {
    return winexinfo::tests::ParseHookTestCommandLine(
        std::span<const std::string_view>{arguments.begin(), arguments.size()},
        output);
}

}  // namespace

WXI_TEST(hook_runtime_negative_code_never_dereferences_lparam, "hook_runtime.negative_code") {
    int beginCalls = 0;
    int observeCalls = 0;
    int nextCalls = 0;
    const winexinfo::hook::HookEntryOperations operations{
        [&](HWND, std::uint64_t) {
            ++beginCalls;
            return Success();
        },
        [&](HWND, UINT) { ++observeCalls; },
        [&](const int code, const WPARAM wparam, const LPARAM lparam) {
            ++nextCalls;
            WXI_REQUIRE_EQ(code, -1);
            WXI_REQUIRE_EQ(wparam, WPARAM{0x1234});
            WXI_REQUIRE_EQ(lparam, LPARAM{1});
            return LRESULT{77};
        },
    };
    WXI_REQUIRE_EQ(
        winexinfo::hook::ProcessHookCall(
            -1, 0x1234, 1, 0xC123, operations),
        LRESULT{77});
    WXI_REQUIRE_EQ(beginCalls, 0);
    WXI_REQUIRE_EQ(observeCalls, 0);
    WXI_REQUIRE_EQ(nextCalls, 1);
}

WXI_TEST(hook_runtime_validates_inner_attach_message_and_forwards_originals, "hook_runtime.callback_contract") {
    const HWND target = reinterpret_cast<HWND>(std::uintptr_t{0x100});
    CWPSTRUCT message{};
    message.hwnd = target;
    message.message = 0xC123;
    message.wParam = 0x57495831;
    message.lParam = 9;
    int beginCalls = 0;
    int observeCalls = 0;
    int nextCalls = 0;
    int nextCode = 0;
    WPARAM nextWparam = 0;
    LPARAM nextLparam = 0;
    const winexinfo::hook::HookEntryOperations operations{
        [&](const HWND hwnd, const std::uint64_t attachId) {
            ++beginCalls;
            WXI_REQUIRE_EQ(hwnd, target);
            WXI_REQUIRE_EQ(attachId, std::uint64_t{9});
            return Success();
        },
        [&](HWND, UINT) { ++observeCalls; },
        [&](const int code, const WPARAM wparam, const LPARAM lparam) {
            ++nextCalls;
            nextCode = code;
            nextWparam = wparam;
            nextLparam = lparam;
            return LRESULT{88};
        },
    };
    WXI_REQUIRE_EQ(
        winexinfo::hook::ProcessHookCall(
            HC_ACTION,
            0xDEADBEEF,
            reinterpret_cast<LPARAM>(&message),
            0xC123,
            operations),
        LRESULT{88});
    WXI_REQUIRE_EQ(beginCalls, 1);
    WXI_REQUIRE_EQ(observeCalls, 1);
    WXI_REQUIRE_EQ(nextCalls, 1);
    WXI_REQUIRE_EQ(nextCode, HC_ACTION);
    WXI_REQUIRE_EQ(nextWparam, WPARAM{0xDEADBEEF});
    WXI_REQUIRE_EQ(nextLparam, reinterpret_cast<LPARAM>(&message));

    const auto reject = [&](const CWPSTRUCT value, const WPARAM outer) {
        CWPSTRUCT candidate = value;
        const int before = beginCalls;
        static_cast<void>(winexinfo::hook::ProcessHookCall(
            HC_ACTION,
            outer,
            reinterpret_cast<LPARAM>(&candidate),
            0xC123,
            operations));
        WXI_REQUIRE_EQ(beginCalls, before);
    };
    auto wrongHwnd = message;
    wrongHwnd.hwnd = nullptr;
    reject(wrongHwnd, 0);
    auto wrongMessage = message;
    wrongMessage.message = 0xC124;
    reject(wrongMessage, 0);
    auto wrongMagic = message;
    wrongMagic.wParam = 0;
    reject(wrongMagic, 0x57495831);
    auto zeroId = message;
    zeroId.lParam = 0;
    reject(zeroId, 0);
}

WXI_TEST(hook_runtime_state_machine_is_forward_only, "hook_runtime.state_machine") {
    winexinfo::hook::HookRuntimeStateMachine state;
    WXI_REQUIRE_EQ(state.state(), winexinfo::hook::RuntimeState::Stopped);
    WXI_REQUIRE(state.BeginAttach().ok());
    WXI_REQUIRE_EQ(state.state(), winexinfo::hook::RuntimeState::Starting);
    WXI_REQUIRE(!state.BeginAttach().ok());
    WXI_REQUIRE(state.MarkRunning(true).ok());
    WXI_REQUIRE_EQ(state.state(), winexinfo::hook::RuntimeState::Running);
    WXI_REQUIRE(state.BeginStop().ok());
    WXI_REQUIRE_EQ(state.state(), winexinfo::hook::RuntimeState::Stopping);
    WXI_REQUIRE(!state.BeginStop().ok());
    WXI_REQUIRE(state.MarkStopped().ok());
    WXI_REQUIRE_EQ(state.state(), winexinfo::hook::RuntimeState::Stopped);
    WXI_REQUIRE(!state.MarkRunning(true).ok());
}

WXI_TEST(hook_runtime_requires_validated_attach_before_running, "hook_runtime.validated_attach_before_running") {
    winexinfo::hook::HookRuntimeStateMachine state;
    WXI_REQUIRE(state.BeginAttach().ok());
    WXI_REQUIRE(!state.MarkRunning(false).ok());
    WXI_REQUIRE_EQ(state.state(), winexinfo::hook::RuntimeState::Starting);
    WXI_REQUIRE(state.MarkRunning(true).ok());
}

WXI_TEST(hook_runtime_callback_gate_drains_before_unload, "hook_runtime.callback_drain") {
    winexinfo::hook::HookCallbackGate gate;
    WXI_REQUIRE(gate.Enter());
    WXI_REQUIRE_EQ(gate.in_flight(), std::uint32_t{1});
    gate.RejectNewWork();
    WXI_REQUIRE(!gate.Enter());
    WXI_REQUIRE_EQ(gate.in_flight(), std::uint32_t{1});
    WXI_REQUIRE(!gate.WaitForZero(0));
    const winexinfo::Status timeout =
        winexinfo::hook::DrainHookCallbacksForUnload(gate, 0);
    WXI_REQUIRE_EQ(timeout.code, winexinfo::ErrorCode::DLL_UNLOAD_TIMEOUT);
    WXI_REQUIRE_EQ(timeout.win32, DWORD{ERROR_TIMEOUT});
    gate.Leave();
    WXI_REQUIRE(winexinfo::hook::DrainHookCallbacksForUnload(gate, 0).ok());
    WXI_REQUIRE_EQ(gate.in_flight(), std::uint32_t{0});
}

WXI_TEST(hook_runtime_status_pane_uses_exact_subclass_identity, "hook_runtime.status_pane") {
    const HWND parent = reinterpret_cast<HWND>(std::uintptr_t{0x100});
    const HWND child = reinterpret_cast<HWND>(std::uintptr_t{0x200});
    std::vector<std::wstring> classes;
    std::vector<std::wstring> texts;
    std::vector<std::pair<HWND, UINT_PTR>> installs;
    std::vector<std::pair<HWND, UINT_PTR>> removals;
    int destroys = 0;
    const winexinfo::hook::StatusPaneOperations operations{
        [&](const std::wstring_view className) {
            classes.emplace_back(className);
            return Success();
        },
        [&](const HWND actualParent,
            const std::wstring_view className,
            const std::wstring_view text,
            HWND* const output) {
            WXI_REQUIRE_EQ(actualParent, parent);
            classes.emplace_back(className);
            texts.emplace_back(text);
            *output = child;
            return Success();
        },
        [&](const HWND hwnd, const UINT_PTR id) {
            installs.emplace_back(hwnd, id);
            return Success();
        },
        [&](const HWND hwnd, const UINT_PTR id) {
            removals.emplace_back(hwnd, id);
            return Success();
        },
        [&](const HWND hwnd) {
            WXI_REQUIRE_EQ(hwnd, child);
            ++destroys;
            return Success();
        },
    };
    winexinfo::hook::StatusPane pane{};
    WXI_REQUIRE(winexinfo::hook::InstallStatusPane(parent, operations, &pane).ok());
    WXI_REQUIRE_EQ(pane.hwnd, child);
    WXI_REQUIRE_EQ(
        classes,
        (std::vector<std::wstring>{
            L"WinExInfo.StatusPane", L"WinExInfo.StatusPane"}));
    WXI_REQUIRE_EQ(texts, std::vector<std::wstring>{L"WinExInfo Gate B"});
    WXI_REQUIRE_EQ(
        installs,
        (std::vector<std::pair<HWND, UINT_PTR>>{
            {child, winexinfo::hook::kStatusPaneSubclassId}}));
    WXI_REQUIRE(winexinfo::hook::RemoveStatusPane(operations, &pane).ok());
    WXI_REQUIRE_EQ(removals, installs);
    WXI_REQUIRE_EQ(destroys, 1);
    WXI_REQUIRE_EQ(pane.hwnd, nullptr);
}

WXI_TEST(hook_runtime_observes_before_call_next, "hook_runtime.observation_order") {
    CWPSTRUCT message{};
    message.hwnd = reinterpret_cast<HWND>(std::uintptr_t{0x701});
    message.message = WM_WINDOWPOSCHANGED;
    std::vector<std::string> calls;
    const winexinfo::hook::HookEntryOperations operations{
        [&](HWND, std::uint64_t) {
            calls.emplace_back("attach");
            return Success();
        },
        [&](const HWND hwnd, const UINT observed) {
            WXI_REQUIRE_EQ(hwnd, message.hwnd);
            WXI_REQUIRE_EQ(observed, message.message);
            calls.emplace_back("observe");
        },
        [&](int, WPARAM, LPARAM) {
            calls.emplace_back("next");
            return LRESULT{91};
        },
    };
    WXI_REQUIRE_EQ(
        winexinfo::hook::ProcessHookCall(
            HC_ACTION, 0, reinterpret_cast<LPARAM>(&message), 0xC123, operations),
        LRESULT{91});
    WXI_REQUIRE_EQ(calls, (std::vector<std::string>{"observe", "next"}));
}

WXI_TEST(hook_runtime_filters_exact_thread_hook_tab_view_messages, "hook_runtime.window_message_filter") {
    const HWND target = reinterpret_cast<HWND>(std::uintptr_t{0x801});
    const HWND tab = reinterpret_cast<HWND>(std::uintptr_t{0x802});
    const HWND view = reinterpret_cast<HWND>(std::uintptr_t{0x803});
    const DWORD pid = 51;
    const DWORD tid = 52;
    winexinfo::hook::HookRuntimeWindowMessageOperations operations{
        [&](const HWND window, DWORD* const process) {
            *process = pid;
            return window == tab || window == view ? tid : DWORD{0};
        },
        [&](const HWND) { return target; },
        [&](const HWND window, std::wstring* const name) {
            *name = window == tab ? L"ShellTabWindowClass" : L"DUIViewWndClassName";
            return Success();
        },
        [](HWND) { return true; },
    };
    for (const auto [window, message] : {
             std::pair{tab, UINT{WM_SHOWWINDOW}},
             std::pair{view, UINT{WM_WINDOWPOSCHANGED}}}) {
        WXI_REQUIRE(winexinfo::hook::ShouldNotifyHookRuntimeWindowMessage(
            window, message, target, pid, tid, operations));
    }
    WXI_REQUIRE(!winexinfo::hook::ShouldNotifyHookRuntimeWindowMessage(
        tab, WM_SIZE, target, pid, tid, operations));

    auto wrongRoot = operations;
    wrongRoot.get_root = [](HWND) {
        return reinterpret_cast<HWND>(std::uintptr_t{0x999});
    };
    WXI_REQUIRE(!winexinfo::hook::ShouldNotifyHookRuntimeWindowMessage(
        tab, WM_SHOWWINDOW, target, pid, tid, wrongRoot));
    auto wrongClass = operations;
    wrongClass.get_class_name = [](HWND, std::wstring* name) {
        *name = L"Other";
        return Success();
    };
    WXI_REQUIRE(!winexinfo::hook::ShouldNotifyHookRuntimeWindowMessage(
        tab, WM_SHOWWINDOW, target, pid, tid, wrongClass));
    auto wrongThread = operations;
    wrongThread.get_window_thread_process_id = [&](HWND, DWORD* process) {
        *process = pid;
        return tid + 1;
    };
    WXI_REQUIRE(!winexinfo::hook::ShouldNotifyHookRuntimeWindowMessage(
        tab, WM_SHOWWINDOW, target, pid, tid, wrongThread));
    auto wrongProcess = operations;
    wrongProcess.get_window_thread_process_id = [&](HWND, DWORD* process) {
        *process = pid + 1;
        return tid;
    };
    WXI_REQUIRE(!winexinfo::hook::ShouldNotifyHookRuntimeWindowMessage(
        tab, WM_SHOWWINDOW, target, pid, tid, wrongProcess));
    auto oldHidden = operations;
    oldHidden.is_window_visible = [](HWND) { return false; };
    WXI_REQUIRE(!winexinfo::hook::ShouldNotifyHookRuntimeWindowMessage(
        tab, WM_SHOWWINDOW, target, pid, tid, oldHidden));
}

WXI_TEST(hook_runtime_window_message_signal_coalesces_and_rejects_after_stop, "hook_runtime.window_message_signal") {
    const HWND pane = reinterpret_cast<HWND>(std::uintptr_t{0x901});
    winexinfo::hook::StatusPaneRefreshCoordinator coordinator{pane};
    int wakes = 0;
    const auto wake = [&] {
        ++wakes;
        return Success();
    };
    WXI_REQUIRE(winexinfo::hook::SignalHookRuntimeWindowMessage(
                    WM_SHOWWINDOW, &coordinator, wake)
                    .ok());
    WXI_REQUIRE(winexinfo::hook::SignalHookRuntimeWindowMessage(
                    WM_WINDOWPOSCHANGED, &coordinator, wake)
                    .ok());
    WXI_REQUIRE_EQ(wakes, 1);
    coordinator.Stop();
    WXI_REQUIRE(!winexinfo::hook::SignalHookRuntimeWindowMessage(
                     WM_SHOWWINDOW, &coordinator, wake)
                     .ok());
    WXI_REQUIRE_EQ(wakes, 1);
}

WXI_TEST(hook_runtime_rollback_retains_module_when_pane_cleanup_fails, "hook_runtime.rollback_retention") {
    const HWND child = reinterpret_cast<HWND>(std::uintptr_t{0x610});
    for (const auto path : {
             winexinfo::hook::RuntimeRollbackPath::CompareExchange,
             winexinfo::hook::RuntimeRollbackPath::WorkerCreation}) {
        winexinfo::hook::StatusPane pane{child, true};
        int releases = 0;
        const winexinfo::hook::StatusPaneOperations operations{
            [](std::wstring_view) { return Success(); },
            [](HWND, std::wstring_view, std::wstring_view, HWND*) {
                return Success();
            },
            [](HWND, UINT_PTR) { return Success(); },
            [](HWND, UINT_PTR) {
                return winexinfo::Status{
                    winexinfo::ErrorCode::WINDOW_ATTACH_FAILED,
                    E_FAIL,
                    ERROR_INVALID_STATE};
            },
            [](HWND) { return Success(); },
        };
        WXI_REQUIRE(!winexinfo::hook::CleanupRuntimeRollback(
                         path, operations, &pane, [&] { ++releases; })
                         .ok());
        WXI_REQUIRE_EQ(releases, 0);
        WXI_REQUIRE_EQ(pane.hwnd, child);
        WXI_REQUIRE(pane.subclass_installed);
    }
}

WXI_TEST(hook_runtime_rollback_releases_module_after_exact_cleanup, "hook_runtime.rollback_cleanup") {
    winexinfo::hook::StatusPane pane{
        reinterpret_cast<HWND>(std::uintptr_t{0x620}), true};
    int releases = 0;
    const winexinfo::hook::StatusPaneOperations operations{
        [](std::wstring_view) { return Success(); },
        [](HWND, std::wstring_view, std::wstring_view, HWND*) { return Success(); },
        [](HWND, UINT_PTR) { return Success(); },
        [](HWND, UINT_PTR) { return Success(); },
        [](HWND) { return Success(); },
    };
    WXI_REQUIRE(winexinfo::hook::CleanupRuntimeRollback(
                    winexinfo::hook::RuntimeRollbackPath::WorkerCreation,
                    operations,
                    &pane,
                    [&] { ++releases; })
                    .ok());
    WXI_REQUIRE_EQ(releases, 1);
    WXI_REQUIRE_EQ(pane.hwnd, nullptr);
}

WXI_TEST(hook_runtime_cli_is_exact, "hook_runtime.cli") {
    winexinfo::tests::HookTestCommand command{};
    WXI_REQUIRE(ParseHookCommand(
                    {"WinExInfoTests.exe", "--hook-target"}, &command)
                    .ok());
    WXI_REQUIRE_EQ(command.mode, winexinfo::tests::HookTestMode::Target);
    WXI_REQUIRE(ParseHookCommand(
                    {"WinExInfoTests.exe", "--hook-controller", "123", "--iterations", "100"},
                    &command)
                    .ok());
    WXI_REQUIRE_EQ(command.mode, winexinfo::tests::HookTestMode::ControllerNormal);
    WXI_REQUIRE_EQ(command.target_pid, DWORD{123});
    WXI_REQUIRE_EQ(command.iterations, std::uint32_t{100});
    WXI_REQUIRE(ParseHookCommand(
                    {"WinExInfoTests.exe", "--hook-controller", "123", "--fault", "unhook-failure"},
                    &command)
                    .ok());
    WXI_REQUIRE_EQ(command.mode, winexinfo::tests::HookTestMode::ControllerFault);

    WXI_REQUIRE(!ParseHookCommand(
                     {"WinExInfoTests.exe", "--hook-target", "extra"}, &command)
                     .ok());
    WXI_REQUIRE(!ParseHookCommand(
                     {"WinExInfoTests.exe", "--hook-controller", "+123", "--iterations", "1"},
                     &command)
                     .ok());
    WXI_REQUIRE(!ParseHookCommand(
                     {"WinExInfoTests.exe", "--hook-controller", "123", "--iterations", "101"},
                     &command)
                     .ok());
    WXI_REQUIRE(!ParseHookCommand(
                     {"WinExInfoTests.exe", "--hook-controller", "123", "--fault", "other"},
                     &command)
                     .ok());
}

WXI_TEST(hook_runtime_dllmain_contains_only_module_capture, "hook_runtime.dllmain_audit") {
    const std::filesystem::path source =
        std::filesystem::path{__FILE__}.parent_path().parent_path() /
        "src" / "hook" / "dll_main.cpp";
    std::ifstream stream{source, std::ios::binary};
    WXI_REQUIRE(stream.good());
    const std::string text{
        std::istreambuf_iterator<char>{stream},
        std::istreambuf_iterator<char>{}};
    WXI_REQUIRE(text.find("g_hook_module = module") != std::string::npos);
    WXI_REQUIRE(text.find("DisableThreadLibraryCalls") == std::string::npos);
    WXI_REQUIRE(text.find("CoInitialize") == std::string::npos);
    WXI_REQUIRE(text.find("CreateThread") == std::string::npos);
    WXI_REQUIRE(text.find("CreateWindow") == std::string::npos);
    WXI_REQUIRE(text.find("CreateNamedPipe") == std::string::npos);
}

WXI_TEST(hook_runtime_export_name_is_exact, "hook_runtime.export_name") {
    wchar_t executable[32768]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, 32768);
    WXI_REQUIRE(size > 0 && size < 32768);
    const std::filesystem::path dll =
        std::filesystem::path{std::wstring{executable, size}}.parent_path() /
        L"WinExInfoHook.dll";
    const HMODULE module = LoadLibraryW(dll.c_str());
    WXI_REQUIRE(module != nullptr);
    WXI_REQUIRE(GetProcAddress(module, "WinExInfoCallWndProc") != nullptr);
    WXI_REQUIRE(GetProcAddress(module, "_WinExInfoCallWndProc@12") == nullptr);
    WXI_REQUIRE(GetProcAddress(module, "WinExInfoCallWndProcW") == nullptr);
    WXI_REQUIRE(FreeLibrary(module) != FALSE);
}
