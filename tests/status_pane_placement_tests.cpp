#include "test_framework.h"

#include "hook/status_pane.h"
#include "hook/runtime.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

winexinfo::Status Success() {
    return {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

struct PlacementRecorder final {
    HWND pane = reinterpret_cast<HWND>(std::uintptr_t{0x100});
    HWND parent = reinterpret_cast<HWND>(std::uintptr_t{0x200});
    HWND tab = reinterpret_cast<HWND>(std::uintptr_t{0x300});
    HWND top = reinterpret_cast<HWND>(std::uintptr_t{0x400});
    DWORD pid = 41;
    DWORD tid = 73;
    UINT dpi = 96;
    LONG_PTR style = WS_VISIBLE;
    LONG_PTR ex_style = 0;
    std::vector<std::string> calls;
    HWND insert_after = nullptr;
    RECT positioned{};
    UINT position_flags = 0;

    winexinfo::hook::StatusPanePlacementOperations Operations() {
        return {
            [&] { return pid; },
            [&] { return tid; },
            [&](const HWND window, DWORD* const process) {
                WXI_REQUIRE(window == pane || window == parent || window == tab ||
                            window == top);
                *process = pid;
                return tid;
            },
            [&](const HWND window, std::wstring* const name) {
                *name = window == pane
                    ? std::wstring{winexinfo::hook::kStatusPaneClassName}
                    : window == parent
                    ? std::wstring{winexinfo::hook::kStatusPaneParentClassName}
                    : window == tab ? L"ShellTabWindowClass" : L"CabinetWClass";
                return Success();
            },
            [&](const HWND window) {
                if (window == parent) {
                    return tab;
                }
                if (window == tab) {
                    return top;
                }
                return HWND{nullptr};
            },
            [&](const HWND window) {
                return window == top ? tab : HWND{nullptr};
            },
            [&](const HWND) { return HWND{nullptr}; },
            [&](const HWND) { return true; },
            [&](const HWND window, const int index, LONG_PTR* const value) {
                WXI_REQUIRE_EQ(window, pane);
                *value = index == GWL_STYLE ? style : ex_style;
                return Success();
            },
            [&](const HWND window, const int index, const LONG_PTR value) {
                WXI_REQUIRE_EQ(window, pane);
                calls.emplace_back(index == GWL_STYLE ? "style" : "ex_style");
                if (index == GWL_STYLE) {
                    style = value;
                } else {
                    ex_style = value;
                }
                return Success();
            },
            [&](const HWND window, const HWND newParent) {
                WXI_REQUIRE_EQ(window, pane);
                WXI_REQUIRE_EQ(newParent, parent);
                calls.emplace_back("parent");
                return Success();
            },
            [&](const HWND window) {
                WXI_REQUIRE_EQ(window, pane);
                return dpi;
            },
            [&](const HWND window,
                const HWND after,
                const int x,
                const int y,
                const int width,
                const int height,
                const UINT flags) {
                WXI_REQUIRE_EQ(window, pane);
                calls.emplace_back("position");
                insert_after = after;
                positioned = {x, y, x + width, y + height};
                position_flags = flags;
                return Success();
            },
        };
    }
};

}  // namespace

WXI_TEST(status_pane_placement_is_exact_and_nonactivating, "status_pane.placement.exact") {
    PlacementRecorder recorder;
    const RECT rect{10, 20, 210, 50};
    WXI_REQUIRE(winexinfo::hook::ApplyStatusPanePlacementWithOperations(
                    recorder.pane,
                    recorder.parent,
                    rect,
                    true,
                    recorder.Operations())
                    .ok());
    WXI_REQUIRE((recorder.style & WS_CHILD) != 0);
    WXI_REQUIRE((recorder.ex_style & WS_EX_NOACTIVATE) != 0);
    WXI_REQUIRE_EQ(
        recorder.calls,
        (std::vector<std::string>{"style", "ex_style", "parent", "position"}));
    WXI_REQUIRE_EQ(recorder.insert_after, HWND_TOP);
    WXI_REQUIRE_EQ(recorder.positioned.left, rect.left);
    WXI_REQUIRE_EQ(recorder.positioned.top, rect.top);
    WXI_REQUIRE_EQ(recorder.positioned.right, rect.right);
    WXI_REQUIRE_EQ(recorder.positioned.bottom, rect.bottom);
    WXI_REQUIRE_EQ(
        recorder.position_flags,
        UINT{SWP_NOACTIVATE | SWP_SHOWWINDOW});
}

WXI_TEST(status_pane_placement_hides_geometry_under_96_dip, "status_pane.placement.minimum_width") {
    PlacementRecorder recorder;
    recorder.dpi = 144;
    const RECT rect{0, 0, 143, 30};
    WXI_REQUIRE(winexinfo::hook::ApplyStatusPanePlacementWithOperations(
                    recorder.pane,
                    recorder.parent,
                    rect,
                    true,
                    recorder.Operations())
                    .ok());
    WXI_REQUIRE_EQ(
        recorder.position_flags,
        UINT{SWP_NOACTIVATE | SWP_HIDEWINDOW});
}

WXI_TEST(status_pane_placement_revalidates_identity_before_mutation, "status_pane.placement.revalidate") {
    PlacementRecorder recorder;
    auto operations = recorder.Operations();
    operations.get_window_thread_process_id =
        [&](const HWND, DWORD* const process) {
            *process = recorder.pid;
            return recorder.tid + 1;
        };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementWithOperations(
                     recorder.pane,
                     recorder.parent,
                     RECT{0, 0, 200, 30},
                     true,
                     operations)
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());

    operations = recorder.Operations();
    operations.get_class_name = [&](const HWND, std::wstring* const name) {
        *name = L"unexpected";
        return Success();
    };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementWithOperations(
                     recorder.pane,
                     recorder.parent,
                     RECT{0, 0, 200, 30},
                     true,
                     operations)
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());
}

WXI_TEST(status_pane_placement_rejects_stale_worker_result, "status_pane.placement.immutable_result") {
    PlacementRecorder recorder;
    const winexinfo::hook::StatusPanePlacementResult result{
        recorder.pid,
        recorder.tid,
        recorder.top,
        recorder.parent,
        RECT{0, 0, 200, 30},
        true,
    };
    WXI_REQUIRE(winexinfo::hook::ApplyStatusPanePlacementResultWithOperations(
                    recorder.pane, result, recorder.Operations())
                    .ok());
    WXI_REQUIRE(!recorder.calls.empty());

    recorder.calls.clear();
    const auto stale = winexinfo::hook::StatusPanePlacementResult{
        recorder.pid,
        recorder.tid + 1,
        recorder.top,
        recorder.parent,
        RECT{0, 0, 200, 30},
        true,
    };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementResultWithOperations(
                     recorder.pane, stale, recorder.Operations())
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());
}

WXI_TEST(status_pane_placement_rejects_inactive_or_stale_parent, "status_pane.placement.active_parent") {
    PlacementRecorder recorder;
    const winexinfo::hook::StatusPanePlacementResult result{
        recorder.pid,
        recorder.tid,
        recorder.top,
        recorder.parent,
        RECT{0, 0, 200, 30},
        true,
    };

    auto hidden = recorder.Operations();
    hidden.is_window_visible = [&](const HWND window) {
        return window != recorder.parent;
    };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementResultWithOperations(
                     recorder.pane, result, hidden)
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());

    auto hiddenTab = recorder.Operations();
    hiddenTab.is_window_visible = [&](const HWND window) {
        return window != recorder.tab;
    };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementResultWithOperations(
                     recorder.pane, result, hiddenTab)
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());

    auto reparented = recorder.Operations();
    reparented.get_parent = [&](const HWND window) {
        return window == recorder.parent ? recorder.top : HWND{nullptr};
    };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementResultWithOperations(
                     recorder.pane, result, reparented)
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());

    const HWND otherTab = reinterpret_cast<HWND>(std::uintptr_t{0x500});
    auto staleTab = recorder.Operations();
    staleTab.get_first_child = [&](const HWND window) {
        return window == recorder.top ? otherTab : HWND{nullptr};
    };
    staleTab.get_window_thread_process_id = [&](const HWND, DWORD* const process) {
        *process = recorder.pid;
        return recorder.tid;
    };
    staleTab.get_class_name = [&](const HWND window, std::wstring* const name) {
        *name = window == recorder.pane
            ? std::wstring{winexinfo::hook::kStatusPaneClassName}
            : window == recorder.parent
            ? std::wstring{winexinfo::hook::kStatusPaneParentClassName}
            : window == recorder.top ? L"CabinetWClass" : L"ShellTabWindowClass";
        return Success();
    };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementResultWithOperations(
                     recorder.pane, result, staleTab)
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());
}

WXI_TEST(status_pane_production_reflow_coalesces_and_owns_payload, "status_pane.placement.production_reflow") {
    PlacementRecorder recorder;
    winexinfo::hook::StatusPaneRefreshCoordinator coordinator{recorder.pane};
    int wakes = 0;
    int posts = 0;
    const auto wake = [&] {
        ++wakes;
        return Success();
    };
    for (const UINT message : {WM_SIZE, WM_DPICHANGED, WM_THEMECHANGED, WM_SHOWWINDOW}) {
        WXI_REQUIRE(coordinator.Signal(message, wake).ok());
    }
    WXI_REQUIRE_EQ(wakes, 1);

    const winexinfo::hook::StatusPanePlacementResult result{
        recorder.pid, recorder.tid, recorder.top, recorder.parent,
        RECT{0, 0, 200, 30}, true};
    WXI_REQUIRE(coordinator.Publish(result, [&](const HWND pane, const UINT message) {
        WXI_REQUIRE_EQ(pane, recorder.pane);
        WXI_REQUIRE_EQ(message, winexinfo::hook::kStatusPaneReflowMessage);
        ++posts;
        return Success();
    }).ok());
    WXI_REQUIRE_EQ(posts, 1);
    WXI_REQUIRE_EQ(coordinator.pending_results(), std::size_t{1});

    winexinfo::hook::StatusPanePlacementResult consumed{};
    WXI_REQUIRE(coordinator.Consume(&consumed));
    WXI_REQUIRE(coordinator.ApplyCompleted(true, wake).ok());
    WXI_REQUIRE_EQ(consumed.parent, recorder.parent);
    WXI_REQUIRE_EQ(coordinator.pending_results(), std::size_t{0});
    WXI_REQUIRE_EQ(wakes, 2);
}

WXI_TEST(status_pane_production_reflow_cleanup_reclaims_undispatched_result, "status_pane.placement.remove_before_dispatch") {
    PlacementRecorder recorder;
    winexinfo::hook::StatusPaneRefreshCoordinator coordinator{recorder.pane};
    WXI_REQUIRE(coordinator.Signal(WM_SIZE, [] { return Success(); }).ok());
    WXI_REQUIRE(coordinator.Publish(
                    {recorder.pid, recorder.tid, recorder.top, recorder.parent,
                     RECT{0, 0, 200, 30}, true},
                    [](HWND, UINT) { return Success(); })
                    .ok());
    WXI_REQUIRE_EQ(coordinator.pending_results(), std::size_t{1});
    coordinator.Stop();
    WXI_REQUIRE_EQ(coordinator.pending_results(), std::size_t{0});
    winexinfo::hook::StatusPanePlacementResult ignored{};
    WXI_REQUIRE(!coordinator.Consume(&ignored));
}

WXI_TEST(status_pane_production_reflow_post_failure_reclaims_result, "status_pane.placement.post_failure") {
    PlacementRecorder recorder;
    winexinfo::hook::StatusPaneRefreshCoordinator coordinator{recorder.pane};
    WXI_REQUIRE(coordinator.Signal(WM_SIZE, [] { return Success(); }).ok());
    WXI_REQUIRE(!coordinator.Publish(
                     {recorder.pid, recorder.tid, recorder.top, recorder.parent,
                      RECT{0, 0, 200, 30}, true},
                     [](HWND, UINT) {
                         return winexinfo::Status{
                             winexinfo::ErrorCode::WINDOW_ATTACH_FAILED,
                             E_FAIL,
                             ERROR_INVALID_WINDOW_HANDLE};
                     })
                     .ok());
    WXI_REQUIRE_EQ(coordinator.pending_results(), std::size_t{0});
}

WXI_TEST(status_pane_failed_capture_completion_rearms_without_busy_loop, "status_pane.placement.failed_capture_completion") {
    PlacementRecorder recorder;
    winexinfo::hook::StatusPaneRefreshCoordinator coordinator{recorder.pane};
    int wakes = 0;
    const auto wake = [&] {
        ++wakes;
        return Success();
    };
    WXI_REQUIRE(coordinator.Signal(WM_SIZE, wake).ok());
    WXI_REQUIRE_EQ(wakes, 1);
    WXI_REQUIRE(coordinator.CaptureFailed(wake).ok());
    WXI_REQUIRE_EQ(wakes, 1);

    WXI_REQUIRE(coordinator.Signal(WM_DPICHANGED, wake).ok());
    WXI_REQUIRE_EQ(wakes, 2);
    WXI_REQUIRE(coordinator.CaptureFailed(wake).ok());
    WXI_REQUIRE_EQ(wakes, 2);

    WXI_REQUIRE(coordinator.Signal(WM_THEMECHANGED, wake).ok());
    WXI_REQUIRE_EQ(wakes, 3);
    WXI_REQUIRE(coordinator.Publish(
                    {recorder.pid, recorder.tid, recorder.top, recorder.parent,
                     RECT{0, 0, 200, 30}, true},
                    [](HWND, UINT) { return Success(); })
                    .ok());
    winexinfo::hook::StatusPanePlacementResult result{};
    WXI_REQUIRE(coordinator.Consume(&result));
    WXI_REQUIRE(coordinator.ApplyCompleted(true, wake).ok());
    WXI_REQUIRE_EQ(result.parent, recorder.parent);
    WXI_REQUIRE_EQ(wakes, 3);
}

WXI_TEST(status_pane_rejected_apply_recovery_is_bounded, "status_pane.placement.rejected_apply_recovery") {
    PlacementRecorder recorder;
    winexinfo::hook::StatusPaneRefreshCoordinator coordinator{recorder.pane};
    int wakes = 0;
    const auto wake = [&] {
        ++wakes;
        return Success();
    };
    const auto publish = [&] {
        return coordinator.Publish(
            {recorder.pid, recorder.tid, recorder.top, recorder.parent,
             RECT{0, 0, 200, 30}, true},
            [](HWND, UINT) { return Success(); });
    };
    winexinfo::hook::StatusPanePlacementResult result{};

    WXI_REQUIRE(coordinator.Signal(WM_SIZE, wake).ok());
    WXI_REQUIRE(publish().ok());
    WXI_REQUIRE(coordinator.Consume(&result));
    WXI_REQUIRE(coordinator.ApplyCompleted(false, wake).ok());
    WXI_REQUIRE_EQ(wakes, 2);

    WXI_REQUIRE(publish().ok());
    WXI_REQUIRE(coordinator.Consume(&result));
    WXI_REQUIRE(coordinator.ApplyCompleted(false, wake).ok());
    WXI_REQUIRE_EQ(wakes, 2);

    WXI_REQUIRE(coordinator.Signal(WM_SHOWWINDOW, wake).ok());
    WXI_REQUIRE_EQ(wakes, 3);
}

WXI_TEST(status_pane_parent_subclass_install_failure_recovers, "status_pane.placement.parent_subclass_recovery") {
    const HWND oldParent = reinterpret_cast<HWND>(std::uintptr_t{0x710});
    const HWND newParent = reinterpret_cast<HWND>(std::uintptr_t{0x720});
    winexinfo::hook::RuntimeSignalSourceState state{oldParent, false};
    int removes = 0;
    int installs = 0;
    bool failInstall = true;
    const winexinfo::hook::RuntimeSignalSubclassOperations operations{
        [&](const HWND window) {
            WXI_REQUIRE_EQ(window, oldParent);
            ++removes;
            return Success();
        },
        [&](const HWND window) {
            WXI_REQUIRE_EQ(window, newParent);
            ++installs;
            return failInstall
                ? winexinfo::Status{winexinfo::ErrorCode::WINDOW_ATTACH_FAILED,
                      E_FAIL, ERROR_INVALID_STATE}
                : Success();
        },
    };
    WXI_REQUIRE(!winexinfo::hook::UpdateRuntimeSignalParent(
                     &state, newParent, operations)
                     .ok());
    WXI_REQUIRE_EQ(state.parent, nullptr);
    WXI_REQUIRE(state.lifecycle_failed);
    WXI_REQUIRE_EQ(removes, 1);
    WXI_REQUIRE_EQ(installs, 1);

    failInstall = false;
    WXI_REQUIRE(winexinfo::hook::UpdateRuntimeSignalParent(
                    &state, newParent, operations)
                    .ok());
    WXI_REQUIRE_EQ(state.parent, newParent);
    WXI_REQUIRE(!state.lifecycle_failed);
    WXI_REQUIRE_EQ(removes, 1);
    WXI_REQUIRE_EQ(installs, 2);
}

WXI_TEST(status_pane_placement_cleanup_removes_subclass_first, "status_pane.placement.lifetime") {
    const HWND paneWindow = reinterpret_cast<HWND>(std::uintptr_t{0x400});
    std::vector<std::string> calls;
    winexinfo::hook::StatusPane pane{paneWindow, true};
    const winexinfo::hook::StatusPaneOperations operations{
        [](std::wstring_view) { return Success(); },
        [](HWND, std::wstring_view, std::wstring_view, HWND*) { return Success(); },
        [](HWND, UINT_PTR) { return Success(); },
        [&](const HWND window, const UINT_PTR id) {
            WXI_REQUIRE_EQ(window, paneWindow);
            WXI_REQUIRE_EQ(id, winexinfo::hook::kStatusPaneSubclassId);
            calls.emplace_back("remove");
            return Success();
        },
        [&](const HWND window) {
            WXI_REQUIRE_EQ(window, paneWindow);
            calls.emplace_back("destroy");
            return Success();
        },
    };
    WXI_REQUIRE(winexinfo::hook::RemoveStatusPane(operations, &pane).ok());
    WXI_REQUIRE_EQ(calls, (std::vector<std::string>{"remove", "destroy"}));
}
