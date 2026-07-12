#include "test_framework.h"

#include "hook/tab_subclass_set.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using winexinfo::ErrorCode;
using winexinfo::Status;
using winexinfo::hook::TabSubclassOperations;
using winexinfo::ipc::TabSetResult;
using winexinfo::ipc::TabSetUpdate;

Status Success() { return {ErrorCode::OK, S_OK, ERROR_SUCCESS}; }
Status Failure() {
    return {ErrorCode::WINDOW_ATTACH_FAILED, E_FAIL, ERROR_INVALID_STATE};
}
HWND Window(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

struct Recorder final {
    HWND top = Window(0x100);
    DWORD process = 71;
    DWORD thread = 73;
    std::vector<HWND> order{Window(0x201), Window(0x202)};
    std::unordered_map<HWND, std::wstring> classes;
    std::unordered_map<HWND, HWND> parents;
    std::vector<std::string> calls;
    HWND fail_install = nullptr;
    HWND fail_remove = nullptr;

    Recorder() {
        classes[top] = L"CabinetWClass";
        for (const HWND tab : order) {
            classes[tab] = L"ShellTabWindowClass";
            parents[tab] = top;
        }
    }

    TabSetUpdate Update(
        const std::uint64_t topGeneration,
        std::vector<std::pair<HWND, std::uint64_t>> tabs) const {
        TabSetUpdate update{reinterpret_cast<std::uint64_t>(top), topGeneration, {}};
        for (const auto& [tab, generation] : tabs) {
            update.tabs.push_back(
                {reinterpret_cast<std::uint64_t>(tab), generation, thread});
        }
        return update;
    }

    TabSubclassOperations Operations() {
        return {
            [&] { return thread; },
            [&](const HWND window, DWORD* const outputProcess) {
                *outputProcess = process;
                return classes.contains(window) ? thread : DWORD{0};
            },
            [&](const HWND window, std::wstring* const output) {
                const auto found = classes.find(window);
                if (found == classes.end()) return Failure();
                *output = found->second;
                return Success();
            },
            [&](const HWND window) {
                const auto found = parents.find(window);
                return found == parents.end() ? HWND{nullptr} : found->second;
            },
            [&](const HWND window) {
                return window == top && !order.empty() ? order.front() : HWND{nullptr};
            },
            [&](const HWND window) {
                auto found = std::find(order.begin(), order.end(), window);
                if (found == order.end()) return HWND{nullptr};
                ++found;
                return found != order.end() ? *found : HWND{nullptr};
            },
            [&](const HWND window, const UINT_PTR id, const std::uint64_t generation) {
                WXI_REQUIRE_EQ(id, winexinfo::hook::kTabSubclassId);
                calls.push_back(
                    "add:" + std::to_string(reinterpret_cast<std::uintptr_t>(window)) +
                    ":" + std::to_string(generation));
                return window == fail_install ? Failure() : Success();
            },
            [&](const HWND window, const UINT_PTR id) {
                WXI_REQUIRE_EQ(id, winexinfo::hook::kTabSubclassId);
                calls.push_back(
                    "remove:" + std::to_string(reinterpret_cast<std::uintptr_t>(window)));
                return window == fail_remove ? Failure() : Success();
            },
            [&] {
                calls.emplace_back("refresh");
                return Success();
            },
        };
    }
};

void Reset(Recorder& recorder) {
    recorder.fail_remove = nullptr;
    WXI_REQUIRE(winexinfo::hook::RemoveAllTabSubclasses(recorder.Operations()).ok());
    recorder.calls.clear();
}

void RequireFailureAck(const Status status, const TabSetResult& result,
                       const std::uint64_t generation) {
    WXI_REQUIRE(!status.ok());
    WXI_REQUIRE_EQ(result.top_level_generation, generation);
    WXI_REQUIRE(result.result != 0);
    WXI_REQUIRE(!result.error_code.empty());
}

}  // namespace

WXI_TEST(tab_subclass_applies_exact_order_and_duplicate_is_idempotent,
         "tab_subclass.order_duplicate") {
    Recorder recorder;
    Reset(recorder);
    const auto update = recorder.Update(
        10, {{recorder.order[0], 20}, {recorder.order[1], 21}});
    TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top, update, recorder.Operations(), &result).ok());
    WXI_REQUIRE_EQ(
        recorder.calls,
        (std::vector<std::string>{"add:513:20", "add:514:21", "refresh"}));
    WXI_REQUIRE_EQ(result, (TabSetResult{10, 0, {}}));
    recorder.calls.clear();
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top, update, recorder.Operations(), &result).ok());
    WXI_REQUIRE(recorder.calls.empty());
}

WXI_TEST(tab_subclass_rejects_wrong_thread_class_parent_and_zorder,
         "tab_subclass.identity") {
    Recorder recorder;
    Reset(recorder);
    const auto update = recorder.Update(
        10, {{recorder.order[0], 20}, {recorder.order[1], 21}});
    const auto reject = [&](TabSubclassOperations operations) {
        TabSetResult result{};
        RequireFailureAck(
            winexinfo::hook::ApplyTabSetUpdate(
                recorder.top, update, operations, &result), result, 10);
        WXI_REQUIRE(recorder.calls.empty());
    };
    auto operations = recorder.Operations();
    operations.get_current_thread_id = [&] { return recorder.thread + 1; };
    reject(operations);
    operations = recorder.Operations();
    operations.get_class_name = [&](HWND, std::wstring* output) {
        *output = L"wrong";
        return Success();
    };
    reject(operations);
    operations = recorder.Operations();
    operations.get_parent = [&](HWND) { return Window(0x999); };
    reject(operations);
    operations = recorder.Operations();
    operations.get_first_child = [&](HWND) { return recorder.order[1]; };
    reject(operations);
}

WXI_TEST(tab_subclass_rejects_stale_generations_and_wrong_same_generation_ack,
         "tab_subclass.generation") {
    Recorder recorder;
    Reset(recorder);
    recorder.order.resize(1);
    TabSetResult result{};
    const auto initial = recorder.Update(10, {{recorder.order[0], 20}});
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top, initial, recorder.Operations(), &result).ok());
    recorder.calls.clear();
    for (const auto& stale : {
             recorder.Update(9, {{recorder.order[0], 21}}),
             recorder.Update(11, {{recorder.order[0], 19}}),
             recorder.Update(10, {{recorder.order[0], 21}})}) {
        RequireFailureAck(
            winexinfo::hook::ApplyTabSetUpdate(
                recorder.top, stale, recorder.Operations(), &result),
            result, stale.top_level_generation);
        WXI_REQUIRE(recorder.calls.empty());
    }
}

WXI_TEST(tab_subclass_hwnd_reuse_removes_before_reinstall,
         "tab_subclass.hwnd_reuse") {
    Recorder recorder;
    Reset(recorder);
    recorder.order.resize(1);
    TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top,
                    recorder.Update(10, {{recorder.order[0], 20}}),
                    recorder.Operations(), &result).ok());
    recorder.calls.clear();
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top,
                    recorder.Update(11, {{recorder.order[0], 21}}),
                    recorder.Operations(), &result).ok());
    WXI_REQUIRE_EQ(
        recorder.calls,
        (std::vector<std::string>{"remove:513", "add:513:21", "refresh"}));
}

WXI_TEST(tab_subclass_rejects_stale_generation_when_removed_hwnd_reappears,
         "tab_subclass.reappearing_hwnd") {
    Recorder recorder;
    Reset(recorder);
    TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top,
                    recorder.Update(10, {{recorder.order[0], 20},
                                         {recorder.order[1], 21}}),
                    recorder.Operations(), &result).ok());
    recorder.order.resize(1);
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top,
                    recorder.Update(11, {{recorder.order[0], 20}}),
                    recorder.Operations(), &result).ok());
    recorder.calls.clear();
    recorder.order.push_back(Window(0x202));
    const auto stale = recorder.Update(
        12, {{recorder.order[0], 20}, {recorder.order[1], 19}});
    RequireFailureAck(
        winexinfo::hook::ApplyTabSetUpdate(
            recorder.top, stale, recorder.Operations(), &result),
        result, 12);
    WXI_REQUIRE(recorder.calls.empty());
}

WXI_TEST(tab_subclass_install_failure_rolls_back_without_new_generation,
         "tab_subclass.rollback") {
    Recorder recorder;
    Reset(recorder);
    recorder.order.resize(1);
    TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top,
                    recorder.Update(10, {{recorder.order[0], 20}}),
                    recorder.Operations(), &result).ok());
    recorder.calls.clear();
    recorder.order.push_back(Window(0x202));
    recorder.fail_install = recorder.order[1];
    RequireFailureAck(
        winexinfo::hook::ApplyTabSetUpdate(
            recorder.top,
            recorder.Update(11, {{recorder.order[0], 20}, {recorder.order[1], 21}}),
            recorder.Operations(), &result), result, 11);
    WXI_REQUIRE_EQ(recorder.calls, (std::vector<std::string>{"add:514:21"}));
    recorder.fail_install = nullptr;
    recorder.calls.clear();
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top,
                    recorder.Update(11, {{recorder.order[0], 20}, {recorder.order[1], 21}}),
                    recorder.Operations(), &result).ok());
    WXI_REQUIRE_EQ(
        recorder.calls, (std::vector<std::string>{"add:514:21", "refresh"}));
}

WXI_TEST(tab_subclass_remove_all_is_reverse_and_preserves_failed_lifetime,
         "tab_subclass.reverse_cleanup") {
    Recorder recorder;
    Reset(recorder);
    TabSetResult result{};
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top,
                    recorder.Update(10, {{recorder.order[0], 20}, {recorder.order[1], 21}}),
                    recorder.Operations(), &result).ok());
    recorder.calls.clear();
    WXI_REQUIRE(winexinfo::hook::RemoveAllTabSubclasses(recorder.Operations()).ok());
    WXI_REQUIRE_EQ(
        recorder.calls,
        (std::vector<std::string>{"remove:514", "remove:513"}));
    recorder.calls.clear();
    WXI_REQUIRE(winexinfo::hook::RemoveAllTabSubclasses(recorder.Operations()).ok());
    WXI_REQUIRE(recorder.calls.empty());
}

WXI_TEST(tab_subclass_destroy_notification_forgets_lifetime_and_coalesces_refresh,
         "tab_subclass.destroy_signal") {
    Recorder recorder;
    Reset(recorder);
    recorder.order.resize(1);
    TabSetResult result{};
    const auto update = recorder.Update(10, {{recorder.order[0], 20}});
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top, update, recorder.Operations(), &result).ok());
    recorder.calls.clear();
    winexinfo::hook::NotifyTabSubclassMessage(recorder.order[0], WM_NCDESTROY);
    WXI_REQUIRE(winexinfo::hook::ApplyTabSetUpdate(
                    recorder.top, update, recorder.Operations(), &result).ok());
    WXI_REQUIRE_EQ(
        recorder.calls, (std::vector<std::string>{"add:513:20", "refresh"}));
}

WXI_TEST(tab_subclass_rejects_update_that_omits_a_live_tab,
         "tab_subclass.exact_set") {
    Recorder recorder;
    Reset(recorder);
    TabSetResult result{};
    const auto incomplete = recorder.Update(10, {{recorder.order[0], 20}});
    RequireFailureAck(
        winexinfo::hook::ApplyTabSetUpdate(
            recorder.top, incomplete, recorder.Operations(), &result),
        result, 10);
    WXI_REQUIRE(recorder.calls.empty());
}
