#include "test_framework.h"

#include "hook/process_runtime.h"

#include <Windows.h>

#include <cstdint>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace {
HWND H(std::uintptr_t value) { return reinterpret_cast<HWND>(value); }
winexinfo::Status Ok() { return {winexinfo::ErrorCode::OK, S_OK, 0}; }
winexinfo::Status Fail(DWORD e = ERROR_INVALID_STATE) {
    return {winexinfo::ErrorCode::WINDOW_ATTACH_FAILED, HRESULT_FROM_WIN32(e), e};
}
winexinfo::ipc::TabSetUpdate U(std::uintptr_t top, std::uint64_t generation, DWORD tid) {
    return {top, generation, {{top + 1, generation, tid}}};
}

struct Fixture {
    winexinfo::hook::ProcessRuntime runtime;
    DWORD current_tid = 11;
    bool apply_fail = false;
    bool cleanup_fail = false;
    int creates = 0;
    int applies = 0;
    bool activate_fail = false;
    std::vector<HWND> cleaned;
    std::vector<std::string> lifecycle_events;

    Fixture() {
        runtime.process_id = 10;
        runtime.control_message = 0xC410;
        runtime.operations.get_current_process_id = [] { return DWORD{10}; };
        runtime.operations.get_current_thread_id = [&] { return current_tid; };
        runtime.operations.get_window_thread_process_id = [](HWND window, DWORD* pid) {
            *pid = 10;
            return window == H(0x300) ? DWORD{12} : DWORD{11};
        };
        runtime.operations.create_window = [&](const winexinfo::hook::WindowRuntimeKey& key,
                                                std::unique_ptr<winexinfo::hook::WindowRuntime>* out) {
            ++creates;
            auto value = std::make_unique<winexinfo::hook::WindowRuntime>();
            value->key = key;
            *out = std::move(value);
            return Ok();
        };
        runtime.operations.apply_update = [&](winexinfo::hook::WindowRuntime&,
                                               const winexinfo::ipc::TabSetUpdate& update,
                                               winexinfo::ipc::TabSetResult* result) {
            ++applies;
            *result = {update.top_level_generation,
                       apply_fail ? DWORD{ERROR_INVALID_STATE} : DWORD{0},
                       apply_fail ? "failed" : ""};
            return apply_fail ? Fail() : Ok();
        };
        runtime.operations.cleanup_window_on_ui = [&](winexinfo::hook::WindowRuntime& window) {
            cleaned.push_back(window.key.top_level);
            return cleanup_fail ? Fail() : Ok();
        };
        runtime.operations.cleanup_window_from_worker =
            runtime.operations.cleanup_window_on_ui;
        runtime.operations.shelter_window_on_destroy = [&](winexinfo::hook::WindowRuntime& window) {
            lifecycle_events.push_back("shelter:" + std::to_string(
                reinterpret_cast<std::uintptr_t>(window.key.top_level)));
            return Ok();
        };
        runtime.operations.activate_window = [&](winexinfo::hook::WindowRuntime&) {
            return activate_fail ? Fail(ERROR_NOT_READY) : Ok();
        };
        runtime.operations.send_control = [&](HWND top, UINT message, WPARAM magic,
                                               LPARAM token, UINT flags, UINT timeout,
                                               DWORD_PTR*) {
            WXI_REQUIRE_EQ(flags, UINT{SMTO_ABORTIFHUNG | SMTO_BLOCK});
            WXI_REQUIRE_EQ(timeout, UINT{5000});
            current_tid = top == H(0x300) ? 12 : 11;
            return winexinfo::hook::HandleProcessRuntimeControlMessageFor(
                       runtime, top, message, magic, token)
                ? Ok() : Fail(ERROR_TIMEOUT);
        };
    }
};
}

WXI_TEST(process_runtime_retired_slot_is_reserved_before_create,
         "process_runtime.physical_capacity") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    for (std::size_t i = 0; i < 63; ++i) {
        WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                        f.runtime, U(0x700 + i, 1, 11), &result).ok());
    }
    f.cleanup_fail = true;
    WXI_REQUIRE(!winexinfo::hook::DispatchProcessWindowRemoval(
                     f.runtime, {H(0x700), 10, 11, 1}).ok());
    const int creates = f.creates;
    WXI_REQUIRE(!winexinfo::hook::DispatchProcessTabSetUpdate(
                     f.runtime, U(0x900, 1, 11), &result).ok());
    WXI_REQUIRE_EQ(f.creates, creates);
}

WXI_TEST(process_runtime_removal_drains_callback_lease_before_ack,
         "process_runtime.callback_lease_drain") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    auto lease = winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x200));
    WXI_REQUIRE(lease);
    std::thread release([&lease] { Sleep(10); lease = {}; });
    WXI_REQUIRE(winexinfo::hook::DispatchProcessWindowRemoval(
                    f.runtime, {H(0x200), 10, 11, 1}).ok());
    release.join();
    WXI_REQUIRE_EQ(f.cleaned.size(), std::size_t{1});
}

WXI_TEST(process_runtime_top_destroy_only_requests_worker_cleanup,
         "process_runtime.destroy_request") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE(winexinfo::hook::HandleProcessRuntimeTopLevelDestroy(f.runtime, H(0x200)));
    WXI_REQUIRE_EQ(f.lifecycle_events.size(), std::size_t{1});
    WXI_REQUIRE(f.cleaned.empty());
    WXI_REQUIRE(winexinfo::hook::ProcessRequestedWindowRemovals(f.runtime).ok());
    WXI_REQUIRE_EQ(f.cleaned, (std::vector<HWND>{H(0x200)}));
}

WXI_TEST(process_runtime_mixed_destroy_and_stop_is_globally_reverse_created,
         "process_runtime.global_reverse_cleanup") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    for (const auto top : {0x200u, 0x201u, 0x202u}) {
        WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                        f.runtime, U(top, 1, 11), &result).ok());
    }
    WXI_REQUIRE(winexinfo::hook::HandleProcessRuntimeTopLevelDestroy(f.runtime, H(0x200)));
    WXI_REQUIRE(winexinfo::hook::RemoveAllProcessWindows(f.runtime).ok());
    WXI_REQUIRE_EQ(f.cleaned, (std::vector<HWND>{H(0x202), H(0x201), H(0x200)}));
}

WXI_TEST(process_runtime_activation_failure_overrides_success_and_cleans,
         "process_runtime.activation_failure") {
    Fixture f;
    f.activate_fail = true;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE(result.result != 0);
    WXI_REQUIRE_EQ(f.runtime.active_window_count(), std::size_t{0});
    WXI_REQUIRE_EQ(f.cleaned.size(), std::size_t{1});
}

WXI_TEST(process_runtime_removal_rejects_zero_message,
         "process_runtime.removal_token_contract") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    f.runtime.control_message = 0;
    WXI_REQUIRE(!winexinfo::hook::DispatchProcessWindowRemoval(
                     f.runtime, {H(0x200), 10, 11, 1}).ok());
}

WXI_TEST(process_runtime_destroy_request_then_shutdown_is_single_retirement,
         "process_runtime.destroy_then_stop") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE(winexinfo::hook::HandleProcessRuntimeTopLevelDestroy(f.runtime, H(0x200)));
    WXI_REQUIRE(winexinfo::hook::RemoveAllProcessWindows(f.runtime).ok());
    WXI_REQUIRE_EQ(f.runtime.active_window_count(), std::size_t{0});
    WXI_REQUIRE_EQ(f.cleaned.size(), std::size_t{1});
}

WXI_TEST(process_runtime_missing_activation_fails_and_cleans,
         "process_runtime.activation_required") {
    Fixture f;
    f.runtime.operations.activate_window = {};
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE(result.result != 0);
    WXI_REQUIRE_EQ(f.runtime.active_window_count(), std::size_t{0});
    WXI_REQUIRE_EQ(f.cleaned.size(), std::size_t{1});
}

WXI_TEST(process_runtime_stable_slot_survives_acquire_reap_interleaving,
         "process_runtime.acquire_reap") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    std::atomic<bool> run{true};
    std::thread acquirer([&] {
        while (run.load(std::memory_order_acquire)) {
            auto lease = winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x200));
            if (lease) WXI_REQUIRE_EQ(lease.get()->key.top_level, H(0x200));
        }
    });
    WXI_REQUIRE(winexinfo::hook::DispatchProcessWindowRemoval(
                    f.runtime, {H(0x200), 10, 11, 1}).ok());
    WXI_REQUIRE(winexinfo::hook::ReapRemovedProcessWindows(f.runtime, 5000).ok());
    run.store(false, std::memory_order_release);
    acquirer.join();
}

WXI_TEST(process_runtime_new_window_is_unpublished_during_apply_and_activation,
         "process_runtime.publish_after_activation") {
    Fixture f;
    bool applyObserved = false;
    bool activateObserved = false;
    f.runtime.operations.apply_update = [&](winexinfo::hook::WindowRuntime&,
                                            const winexinfo::ipc::TabSetUpdate& update,
                                            winexinfo::ipc::TabSetResult* result) {
        applyObserved = true;
        WXI_REQUIRE(!winexinfo::hook::AcquireProcessWindowCallback(
            f.runtime, H(0x200)));
        *result = {update.top_level_generation, 0, {}};
        return Ok();
    };
    f.runtime.operations.activate_window = [&](winexinfo::hook::WindowRuntime&) {
        activateObserved = true;
        WXI_REQUIRE(!winexinfo::hook::AcquireProcessWindowCallback(
            f.runtime, H(0x200)));
        return Ok();
    };
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE(applyObserved && activateObserved);
    WXI_REQUIRE(winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x200)));
}

WXI_TEST(process_runtime_initial_provisional_is_unpublished_until_activation,
         "process_runtime.initial_publish") {
    Fixture f;
    auto initial = std::make_unique<winexinfo::hook::WindowRuntime>();
    initial->key = {H(0x100), 10, 11, 0};
    WXI_REQUIRE(winexinfo::hook::RegisterInitialProvisionalProcessWindow(
                    f.runtime, std::move(initial)).ok());
    WXI_REQUIRE(!winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x100)));
    WXI_REQUIRE(winexinfo::hook::ActivateAndPublishInitialProcessWindow(
                    f.runtime, H(0x100)).ok());
    WXI_REQUIRE(winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x100)));
}

WXI_TEST(process_runtime_freezes_only_one_exact_provisional,
         "process_runtime.provisional") {
    Fixture f;
    auto initial = std::make_unique<winexinfo::hook::WindowRuntime>();
    initial->key = {H(0x100), 10, 11, 0};
    WXI_REQUIRE(winexinfo::hook::RegisterInitialProvisionalProcessWindow(
                    f.runtime, std::move(initial)).ok());
    WXI_REQUIRE(winexinfo::hook::ActivateAndPublishInitialProcessWindow(
                    f.runtime, H(0x100)).ok());
    auto second = std::make_unique<winexinfo::hook::WindowRuntime>();
    second->key = {H(0x101), 10, 11, 0};
    WXI_REQUIRE(!winexinfo::hook::RegisterInitialProvisionalProcessWindow(
                     f.runtime, std::move(second)).ok());
    winexinfo::ipc::TabSetResult result{};
    f.apply_fail = true;
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x100, 7, 11), &result).ok());
    { auto lease = winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x100));
      WXI_REQUIRE(lease); WXI_REQUIRE_EQ(lease.get()->key.generation, std::uint64_t{0}); }
    f.apply_fail = false;
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x100, 7, 11), &result).ok());
    { auto lease = winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x100));
      WXI_REQUIRE(lease); WXI_REQUIRE_EQ(lease.get()->key.generation, std::uint64_t{7}); }
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x100, 6, 11), &result).ok());
    WXI_REQUIRE(result.result != 0);
}

WXI_TEST(process_runtime_dispatches_same_and_different_thread_windows_once,
         "process_runtime.multi_tid") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x201, 1, 11), &result).ok());
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x300, 1, 12), &result).ok());
    WXI_REQUIRE_EQ(f.runtime.active_window_count(), std::size_t{3});
    WXI_REQUIRE_EQ(f.creates, 3);
    WXI_REQUIRE_EQ(f.applies, 3);
}

WXI_TEST(process_runtime_exact_control_is_single_consumer,
         "process_runtime.control_correlation") {
    Fixture f;
    WXI_REQUIRE(winexinfo::hook::PrepareProcessRuntimeControl(
                    f.runtime, U(0x200, 1, 11), 9).ok());
    WXI_REQUIRE(!winexinfo::hook::HandleProcessRuntimeControlMessageFor(
        f.runtime, H(0x200), f.runtime.control_message, 0, 9));
    WXI_REQUIRE(!winexinfo::hook::HandleProcessRuntimeControlMessageFor(
        f.runtime, H(0x201), f.runtime.control_message,
        winexinfo::hook::kProcessRuntimeControlMagic, 9));
    WXI_REQUIRE(winexinfo::hook::HandleProcessRuntimeControlMessageFor(
        f.runtime, H(0x200), f.runtime.control_message,
        winexinfo::hook::kProcessRuntimeControlMagic, 9));
    WXI_REQUIRE(!winexinfo::hook::HandleProcessRuntimeControlMessageFor(
        f.runtime, H(0x200), f.runtime.control_message,
        winexinfo::hook::kProcessRuntimeControlMagic, 9));
}

WXI_TEST(process_runtime_partial_create_rolls_back_or_poison_retains,
         "process_runtime.partial_create") {
    Fixture f;
    f.apply_fail = true;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE_EQ(f.runtime.active_window_count(), std::size_t{0});
    WXI_REQUIRE(!f.runtime.retention_required());
    f.cleanup_fail = true;
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x201, 1, 11), &result).ok());
    WXI_REQUIRE(f.runtime.retention_required());
    WXI_REQUIRE(f.runtime.retained_window_count() != 0);
}

WXI_TEST(process_runtime_removes_and_reuses_hwnd_after_callback_drain,
         "process_runtime.remove_reuse") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 1, 11), &result).ok());
    WXI_REQUIRE(winexinfo::hook::DispatchProcessWindowRemoval(
                    f.runtime, {H(0x200), 10, 11, 1}).ok());
    WXI_REQUIRE_EQ(f.runtime.active_window_count(), std::size_t{0});
    WXI_REQUIRE(winexinfo::hook::ReapRemovedProcessWindows(f.runtime, 0).ok());
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x200, 2, 11), &result).ok());
    { auto lease = winexinfo::hook::AcquireProcessWindowCallback(f.runtime, H(0x200));
      WXI_REQUIRE(lease); WXI_REQUIRE_EQ(lease.get()->key.generation, std::uint64_t{2}); }
}

WXI_TEST(process_runtime_maximum_and_reverse_cleanup_retention,
         "process_runtime.bound_cleanup") {
    Fixture f;
    winexinfo::ipc::TabSetResult result{};
    for (std::size_t i = 0; i < winexinfo::hook::kMaximumRuntimeWindows; ++i) {
        WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                        f.runtime, U(0x400 + i, 1, 11), &result).ok());
    }
    WXI_REQUIRE(winexinfo::hook::DispatchProcessTabSetUpdate(
                    f.runtime, U(0x999, 1, 11), &result).ok());
    WXI_REQUIRE(result.result != 0);
    f.cleanup_fail = true;
    WXI_REQUIRE(!winexinfo::hook::RemoveAllProcessWindows(f.runtime).ok());
    WXI_REQUIRE_EQ(f.cleaned.front(), H(0x400 + 63));
    WXI_REQUIRE_EQ(f.cleaned.back(), H(0x400));
    WXI_REQUIRE(f.runtime.retention_required());
    WXI_REQUIRE_EQ(f.runtime.retained_window_count(), std::size_t{64});
}
