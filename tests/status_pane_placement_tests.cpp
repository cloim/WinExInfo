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
                WXI_REQUIRE(window == pane || window == parent);
                *process = pid;
                return tid;
            },
            [&](const HWND window, std::wstring* const name) {
                *name = window == pane
                    ? std::wstring{winexinfo::hook::kStatusPaneClassName}
                    : std::wstring{winexinfo::hook::kStatusPaneParentClassName};
                return Success();
            },
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
        recorder.parent,
        RECT{0, 0, 200, 30},
        true,
    };
    WXI_REQUIRE(!winexinfo::hook::ApplyStatusPanePlacementResultWithOperations(
                     recorder.pane, stale, recorder.Operations())
                     .ok());
    WXI_REQUIRE(recorder.calls.empty());
}

WXI_TEST(status_pane_placement_reflow_posts_once_until_consumed, "status_pane.placement.reflow") {
    const HWND pane = reinterpret_cast<HWND>(std::uintptr_t{0x300});
    winexinfo::hook::StatusPaneReflowState state{};
    int posts = 0;
    const auto post = [&](const HWND window, const UINT message) {
        WXI_REQUIRE_EQ(window, pane);
        WXI_REQUIRE_EQ(message, winexinfo::hook::kStatusPaneReflowMessage);
        ++posts;
        return Success();
    };
    WXI_REQUIRE(winexinfo::hook::QueueStatusPaneReflow(pane, &state, post).ok());
    WXI_REQUIRE(winexinfo::hook::QueueStatusPaneReflow(pane, &state, post).ok());
    WXI_REQUIRE_EQ(posts, 1);
    WXI_REQUIRE(winexinfo::hook::ConsumeStatusPaneReflow(&state));
    WXI_REQUIRE(!winexinfo::hook::ConsumeStatusPaneReflow(&state));
    WXI_REQUIRE(winexinfo::hook::QueueStatusPaneReflow(pane, &state, post).ok());
    WXI_REQUIRE_EQ(posts, 2);
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
