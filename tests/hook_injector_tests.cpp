#include "test_framework.h"

#include "injection/thread_hook_injector.h"

#include <Windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

winexinfo::Status Success() {
    return {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

winexinfo::Status Failure(const winexinfo::ErrorCode code) {
    return {code, S_FALSE, ERROR_SUCCESS};
}

struct FakeState final {
    bool event_already_exists = false;
    bool event_manual_reset = true;
    bool event_initially_signaled = false;
    bool event_current_user_only = true;
    bool event_unexpected_principal = false;
    bool set_hook_succeeds = true;
    bool trigger_succeeds = true;
    bool wait_succeeds = true;
    bool receipt_available = true;
    bool unhook_succeeds = true;
    bool set_event_succeeds = true;
    winexinfo::injection::HookAttachReceipt receipt{
        true,
        1,
        100,
        200,
        reinterpret_cast<HWND>(std::uintptr_t{0x300}),
        0,
        "",
    };
    std::vector<std::wstring> event_names;
    std::vector<std::wstring> message_names;
    std::vector<std::string> export_names;
    std::vector<int> hook_types;
    std::vector<DWORD> hook_threads;
    std::vector<UINT> trigger_messages;
    std::vector<WPARAM> trigger_wparams;
    std::vector<LPARAM> trigger_lparams;
    std::vector<UINT> trigger_flags;
    std::vector<UINT> trigger_timeouts;
    std::vector<std::uint64_t> waited_ids;
    std::vector<DWORD> wait_timeouts;
    int unhook_calls = 0;
    int set_event_calls = 0;
    int close_event_calls = 0;
    std::vector<std::string> install_order;
};

winexinfo::injection::HookPlatformOperations Operations(FakeState* const state) {
    return {
        [state](
            const std::wstring_view name,
            winexinfo::injection::HookReleaseEvent* const output) {
            state->event_names.emplace_back(name);
            *output = {
                reinterpret_cast<HANDLE>(std::uintptr_t{0x100}),
                state->event_already_exists,
                state->event_manual_reset,
                state->event_initially_signaled,
                state->event_current_user_only,
                state->event_unexpected_principal,
            };
            return Success();
        },
        [state](const std::wstring_view name, UINT* const output) {
            state->message_names.emplace_back(name);
            *output = 0xC123;
            return Success();
        },
        [state](
            const std::string_view name,
            HMODULE* const module,
            HOOKPROC* const procedure) {
            state->export_names.emplace_back(name);
            *module = reinterpret_cast<HMODULE>(std::uintptr_t{0x200});
            *procedure = reinterpret_cast<HOOKPROC>(std::uintptr_t{0x300});
            return Success();
        },
        [state](
            const int hookType,
            const HOOKPROC,
            const HMODULE,
            const DWORD threadId,
            HHOOK* const output) {
            state->hook_types.push_back(hookType);
            state->hook_threads.push_back(threadId);
            state->install_order.push_back("set_hook");
            if (!state->set_hook_succeeds) {
                return Failure(winexinfo::ErrorCode::HOOK_INSTALL_FAILED);
            }
            *output = reinterpret_cast<HHOOK>(std::uintptr_t{0x400});
            return Success();
        },
        [state](
            const HWND,
            const UINT message,
            const WPARAM wparam,
            const LPARAM lparam,
            const UINT flags,
            const UINT timeout) {
            state->trigger_messages.push_back(message);
            state->trigger_wparams.push_back(wparam);
            state->trigger_lparams.push_back(lparam);
            state->trigger_flags.push_back(flags);
            state->trigger_timeouts.push_back(timeout);
            return state->trigger_succeeds
                ? Success()
                : Failure(winexinfo::ErrorCode::HOOK_TRIGGER_FAILED);
        },
        [state](
            const std::uint64_t attachId,
            const DWORD timeout,
            winexinfo::injection::HookAttachReceipt* const output) {
            state->waited_ids.push_back(attachId);
            state->wait_timeouts.push_back(timeout);
            if (!state->wait_succeeds) {
                return Failure(winexinfo::ErrorCode::DLL_INITIALIZATION_FAILED);
            }
            *output = state->receipt;
            output->available = state->receipt_available;
            return Success();
        },
        [state](const HHOOK, bool* const output) {
            ++state->unhook_calls;
            state->install_order.push_back("unhook");
            *output = state->unhook_succeeds;
            return Success();
        },
        [state](const HANDLE, bool* const output) {
            ++state->set_event_calls;
            state->install_order.push_back("signal");
            *output = state->set_event_succeeds;
            return Success();
        },
        [state](const HANDLE) { ++state->close_event_calls; },
    };
}

winexinfo::injection::HookTarget Target();

WXI_TEST(hook_injector_runs_final_validation_adjacent_to_hook_install,
         "hook_injector.final_validation_adjacency") {
    FakeState state;
    auto operations = Operations(&state);
    operations.before_set_hook = [&state] {
        state.install_order.push_back("final_validate");
        return Success();
    };
    winexinfo::injection::ThreadHookInjector injector{std::move(operations)};
    winexinfo::injection::HookAttachOutcome outcome{};
    WXI_REQUIRE(injector.Attach(Target(), &outcome).ok());
    WXI_REQUIRE_EQ(
        state.install_order,
        (std::vector<std::string>{"final_validate", "set_hook"}));
}

winexinfo::injection::HookTarget Target() {
    return {
        100,
        200,
        reinterpret_cast<HWND>(std::uintptr_t{0x300}),
    };
}

void RequireStatus(
    const winexinfo::Status& status,
    const winexinfo::ErrorCode code) {
    WXI_REQUIRE_EQ(status.code, code);
}

}  // namespace

WXI_TEST(hook_injector_uses_exact_event_hook_and_trigger, "hook_injector.exact_contract") {
    FakeState state;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome outcome{};
    WXI_REQUIRE(injector.Attach(Target(), &outcome).ok());
    WXI_REQUIRE_EQ(outcome.attach_id, std::uint64_t{1});
    WXI_REQUIRE_EQ(
        outcome.event_name,
        std::wstring{L"Local\\WinExInfo.HookReleased.100.200.1"});
    WXI_REQUIRE_EQ(state.event_names, std::vector{outcome.event_name});
    WXI_REQUIRE_EQ(
        state.message_names,
        std::vector<std::wstring>{L"WinExInfo.Attach.v1"});
    WXI_REQUIRE_EQ(
        state.export_names,
        std::vector<std::string>{"WinExInfoCallWndProc"});
    WXI_REQUIRE_EQ(state.hook_types, std::vector<int>{WH_CALLWNDPROC});
    WXI_REQUIRE_EQ(state.hook_threads, std::vector<DWORD>{200});
    WXI_REQUIRE_EQ(state.trigger_wparams, std::vector<WPARAM>{0x57495831});
    WXI_REQUIRE_EQ(state.trigger_lparams, std::vector<LPARAM>{1});
    WXI_REQUIRE_EQ(
        state.trigger_flags,
        std::vector<UINT>{SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT});
    WXI_REQUIRE_EQ(state.trigger_timeouts, std::vector<UINT>{1000});
    WXI_REQUIRE_EQ(state.waited_ids, std::vector<std::uint64_t>{1});
    WXI_REQUIRE_EQ(state.wait_timeouts, std::vector<DWORD>{5000});
    WXI_REQUIRE_EQ(state.unhook_calls, 0);
    WXI_REQUIRE_EQ(state.set_event_calls, 0);
    WXI_REQUIRE(!outcome.hook_released);
    WXI_REQUIRE(!outcome.release_event_signaled);
    WXI_REQUIRE(outcome.unload_authorized);
    WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{1});
    WXI_REQUIRE(injector.ReleaseHookForDetach(100).ok());
    WXI_REQUIRE_EQ(state.unhook_calls, 1);
    WXI_REQUIRE_EQ(state.set_event_calls, 1);
    WXI_REQUIRE(state.install_order.size() >= 2);
    WXI_REQUIRE_EQ(state.install_order[state.install_order.size() - 2], "unhook");
    WXI_REQUIRE_EQ(state.install_order.back(), "signal");
}

WXI_TEST(hook_injector_retains_active_hook_until_detach_release, "hook_injector.active_retention") {
    FakeState state;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome outcome{};
    WXI_REQUIRE(injector.Attach(Target(), &outcome).ok());
    WXI_REQUIRE_EQ(state.unhook_calls, 0);
    WXI_REQUIRE_EQ(state.set_event_calls, 0);
    WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{1});
    WXI_REQUIRE(injector.ReleaseHookForDetach(100).ok());
    WXI_REQUIRE_EQ(state.unhook_calls, 1);
    WXI_REQUIRE_EQ(state.set_event_calls, 1);
}

WXI_TEST(hook_injector_detach_release_failure_retains_hook_state, "hook_injector.detach_release_failure") {
    FakeState unhookFailure;
    winexinfo::injection::ThreadHookInjector first{Operations(&unhookFailure)};
    winexinfo::injection::HookAttachOutcome outcome{};
    WXI_REQUIRE(first.Attach(Target(), &outcome).ok());
    unhookFailure.unhook_succeeds = false;
    WXI_REQUIRE(!first.ReleaseHookForDetach(100).ok());
    WXI_REQUIRE_EQ(unhookFailure.set_event_calls, 0);
    WXI_REQUIRE_EQ(first.retained_target_count(), std::size_t{1});

    FakeState signalFailure;
    winexinfo::injection::ThreadHookInjector second{Operations(&signalFailure)};
    WXI_REQUIRE(second.Attach(Target(), &outcome).ok());
    signalFailure.set_event_succeeds = false;
    WXI_REQUIRE(!second.ReleaseHookForDetach(100).ok());
    WXI_REQUIRE_EQ(signalFailure.unhook_calls, 1);
    WXI_REQUIRE_EQ(signalFailure.set_event_calls, 1);
    WXI_REQUIRE_EQ(second.retained_target_count(), std::size_t{1});
}

WXI_TEST(hook_injector_sethook_failure_never_unhooks_or_signals, "hook_injector.sethook_failure") {
    FakeState state;
    state.set_hook_succeeds = false;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome outcome{};
    RequireStatus(
        injector.Attach(Target(), &outcome),
        winexinfo::ErrorCode::HOOK_INSTALL_FAILED);
    WXI_REQUIRE_EQ(state.unhook_calls, 0);
    WXI_REQUIRE_EQ(state.set_event_calls, 0);
    WXI_REQUIRE_EQ(state.close_event_calls, 1);
    WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{0});
}

WXI_TEST(hook_injector_trigger_failure_still_releases_hook, "hook_injector.trigger_failure") {
    FakeState state;
    state.trigger_succeeds = false;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome outcome{};
    RequireStatus(
        injector.Attach(Target(), &outcome),
        winexinfo::ErrorCode::HOOK_TRIGGER_FAILED);
    WXI_REQUIRE_EQ(state.unhook_calls, 1);
    WXI_REQUIRE_EQ(state.set_event_calls, 1);
    WXI_REQUIRE_EQ(state.close_event_calls, 1);
    WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{0});
}

WXI_TEST(hook_injector_unhook_failure_blocks_signal_and_unload, "hook_injector.unhook_failure") {
    FakeState state;
    state.unhook_succeeds = false;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome outcome{};
    WXI_REQUIRE(injector.Attach(Target(), &outcome).ok());
    RequireStatus(
        injector.ReleaseHookForDetach(100),
        winexinfo::ErrorCode::HOOK_RELEASE_FAILED);
    WXI_REQUIRE_EQ(state.unhook_calls, 1);
    WXI_REQUIRE_EQ(state.set_event_calls, 0);
    WXI_REQUIRE(!outcome.hook_released);
    WXI_REQUIRE(outcome.unload_authorized);
    WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{1});
}

WXI_TEST(hook_injector_rejects_invalid_event_before_hook, "hook_injector.event_contract") {
    const auto run = [](FakeState state) {
        winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
        winexinfo::injection::HookAttachOutcome outcome{};
        RequireStatus(
            injector.Attach(Target(), &outcome),
            winexinfo::ErrorCode::HOOK_INSTALL_FAILED);
        WXI_REQUIRE(state.hook_types.empty());
        WXI_REQUIRE_EQ(state.unhook_calls, 0);
        WXI_REQUIRE_EQ(state.set_event_calls, 0);
        WXI_REQUIRE_EQ(state.close_event_calls, 1);
    };
    FakeState alreadyExists;
    alreadyExists.event_already_exists = true;
    run(alreadyExists);
    FakeState autoReset;
    autoReset.event_manual_reset = false;
    run(autoReset);
    FakeState signaled;
    signaled.event_initially_signaled = true;
    run(signaled);
    FakeState missingUser;
    missingUser.event_current_user_only = false;
    run(missingUser);
    FakeState broad;
    broad.event_unexpected_principal = true;
    run(broad);
}

WXI_TEST(hook_injector_validates_attach_receipt_exactly, "hook_injector.receipt_contract") {
    const auto run = [](FakeState state) {
        winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
        winexinfo::injection::HookAttachOutcome outcome{};
        RequireStatus(
            injector.Attach(Target(), &outcome),
            winexinfo::ErrorCode::WINDOW_ATTACH_FAILED);
        WXI_REQUIRE_EQ(state.unhook_calls, 1);
        WXI_REQUIRE_EQ(state.set_event_calls, 1);
        WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{1});
    };
    FakeState unavailable;
    unavailable.receipt_available = false;
    run(unavailable);
    FakeState wrongId;
    wrongId.receipt.request_id = 2;
    run(wrongId);
    FakeState wrongPid;
    wrongPid.receipt.explorer_pid = 101;
    run(wrongPid);
    FakeState wrongTid;
    wrongTid.receipt.ui_thread_id = 201;
    run(wrongTid);
    FakeState wrongHwnd;
    wrongHwnd.receipt.top_level_hwnd =
        reinterpret_cast<HWND>(std::uintptr_t{0x301});
    run(wrongHwnd);
    FakeState failedResult;
    failedResult.receipt.result = 1;
    failedResult.receipt.error_code = "WINDOW_ATTACH_FAILED";
    run(failedResult);
}

WXI_TEST(hook_injector_timeout_retains_event_without_retry, "hook_injector.timeout_retention") {
    FakeState state;
    state.wait_succeeds = false;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome outcome{};
    RequireStatus(
        injector.Attach(Target(), &outcome),
        winexinfo::ErrorCode::DLL_INITIALIZATION_FAILED);
    WXI_REQUIRE_EQ(state.unhook_calls, 1);
    WXI_REQUIRE_EQ(state.set_event_calls, 1);
    WXI_REQUIRE_EQ(state.waited_ids.size(), std::size_t{1});
    WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{1});
    WXI_REQUIRE(injector.ConfirmTargetGone(100).ok());
    WXI_REQUIRE_EQ(state.close_event_calls, 1);
    WXI_REQUIRE_EQ(injector.retained_target_count(), std::size_t{0});
}

WXI_TEST(hook_injector_rejects_duplicate_and_attach_id_exhaustion, "hook_injector.identity_lifetime") {
    FakeState state;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome first{};
    WXI_REQUIRE(injector.Attach(Target(), &first).ok());
    winexinfo::injection::HookAttachOutcome duplicate{};
    RequireStatus(
        injector.Attach(Target(), &duplicate),
        winexinfo::ErrorCode::HOOK_INSTALL_FAILED);
    WXI_REQUIRE_EQ(state.hook_types.size(), std::size_t{1});

    FakeState exhaustedState;
    winexinfo::injection::ThreadHookInjector exhausted{
        Operations(&exhaustedState),
        static_cast<std::uint64_t>(INT64_MAX)};
    winexinfo::injection::HookAttachOutcome outcome{};
    RequireStatus(
        exhausted.Attach(Target(), &outcome),
        winexinfo::ErrorCode::HOOK_INSTALL_FAILED);
    WXI_REQUIRE(exhaustedState.event_names.empty());
}

WXI_TEST(hook_injector_production_event_is_current_user_only, "hook_injector.production_event_security") {
    const std::wstring name =
        L"Local\\WinExInfo.HookReleased.999.888." +
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    auto operations = winexinfo::injection::CreateProductionHookPlatformOperations(
        L"unused.dll",
        [](std::uint64_t, DWORD, winexinfo::injection::HookAttachReceipt*) {
            return Failure(winexinfo::ErrorCode::DLL_INITIALIZATION_FAILED);
        });
    winexinfo::injection::HookReleaseEvent first{};
    WXI_REQUIRE(operations.create_release_event(name, &first).ok());
    WXI_REQUIRE(!first.already_exists);
    WXI_REQUIRE(first.manual_reset);
    WXI_REQUIRE(!first.initially_signaled);
    bool currentUserPresent = false;
    bool unexpectedPrincipalPresent = true;
    WXI_REQUIRE(winexinfo::injection::InspectHookObjectSecurity(
                    first.handle,
                    &currentUserPresent,
                    &unexpectedPrincipalPresent)
                    .ok());
    WXI_REQUIRE(currentUserPresent);
    WXI_REQUIRE(!unexpectedPrincipalPresent);

    winexinfo::injection::HookReleaseEvent duplicate{};
    WXI_REQUIRE(operations.create_release_event(name, &duplicate).ok());
    WXI_REQUIRE(duplicate.already_exists);
    operations.close_event(duplicate.handle);
    operations.close_event(first.handle);
}

WXI_TEST(hook_injector_release_failure_preserves_original_diagnostic, "hook_injector.release_overrides") {
    FakeState state;
    state.trigger_succeeds = false;
    state.unhook_succeeds = false;
    winexinfo::injection::ThreadHookInjector injector{Operations(&state)};
    winexinfo::injection::HookAttachOutcome outcome{};
    RequireStatus(
        injector.Attach(Target(), &outcome),
        winexinfo::ErrorCode::HOOK_RELEASE_FAILED);
    WXI_REQUIRE(outcome.original_status.has_value());
    WXI_REQUIRE_EQ(
        outcome.original_status->code,
        winexinfo::ErrorCode::HOOK_TRIGGER_FAILED);
    WXI_REQUIRE_EQ(state.unhook_calls, 1);
    WXI_REQUIRE_EQ(state.set_event_calls, 0);
}
