#include "hook/explorer_layout.h"

#include "test_framework.h"

#include <Windows.h>
#include <limits>

namespace {

bool IsEmpty(const RECT& rectangle) {
    return rectangle.left == 0 && rectangle.top == 0 &&
        rectangle.right == 0 && rectangle.bottom == 0;
}

bool RectEquals(const RECT& left, const RECT& right) {
    return left.left == right.left && left.top == right.top &&
        left.right == right.right && left.bottom == right.bottom;
}

}  // namespace

WXI_TEST(explorer_layout_gap, "explorer_layout.gap") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{100, 100, 1100, 800},
         {100, 760, 1100, 800},
         {110, 760, 250, 800},
         {900, 760, 1090, 800},
         96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{158, 660, 792, 700}));
}

WXI_TEST(explorer_layout_scales_dips, "explorer_layout.scales_dips") {
    RECT at144{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 1200, 800}, {0, 740, 1200, 800}, {0, 740, 300, 800},
         {900, 740, 1200, 800}, 144},
        &at144).ok());
    WXI_REQUIRE(RectEquals(at144, RECT{312, 740, 888, 800}));

    RECT at192{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 1200, 800}, {0, 740, 1200, 800}, {0, 740, 300, 800},
         {900, 740, 1200, 800}, 192},
        &at192).ok());
    WXI_REQUIRE(RectEquals(at192, RECT{316, 740, 884, 800}));
}

WXI_TEST(explorer_layout_clips_to_parent_and_status, "explorer_layout.clipping") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{100, 100, 500, 400}, {-100, 350, 700, 450}, {-200, 350, 120, 450},
         {450, 350, 700, 450}, 96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{28, 250, 342, 300}));
}

WXI_TEST(explorer_layout_converts_negative_screen_origin, "explorer_layout.negative_origin") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{-500, -300, 500, 400}, {-700, 360, 700, 450}, {-700, 360, -400, 450},
         {400, 360, 700, 450}, 96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{108, 660, 892, 700}));
}

WXI_TEST(explorer_layout_accepts_exact_minimum, "explorer_layout.minimum_width") {
    RECT output{};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 400, 100}, {0, 60, 400, 100}, {0, 60, 100, 100},
         {212, 60, 400, 100}, 96},
        &output).ok());
    WXI_REQUIRE(RectEquals(output, RECT{108, 60, 204, 100}));
}

WXI_TEST(explorer_layout_hides_narrow_gap, "explorer_layout.hidden_narrow") {
    RECT output{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 400, 100}, {0, 60, 400, 100}, {0, 60, 100, 100},
         {211, 60, 400, 100}, 96},
        &output).ok());
    WXI_REQUIRE(IsEmpty(output));
}

WXI_TEST(explorer_layout_hides_overlap_and_reversed_groups, "explorer_layout.group_order") {
    RECT overlap{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 500, 100}, {0, 60, 500, 100}, {0, 60, 300, 100},
         {250, 60, 500, 100}, 96},
        &overlap).ok());
    WXI_REQUIRE(IsEmpty(overlap));

    RECT reversed{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{0, 0, 500, 100}, {0, 60, 500, 100}, {350, 60, 450, 100},
         {50, 60, 150, 100}, 96},
        &reversed).ok());
    WXI_REQUIRE(IsEmpty(reversed));
}

WXI_TEST(explorer_layout_extreme_coordinates_do_not_wrap, "explorer_layout.no_overflow") {
    RECT output{1, 2, 3, 4};
    WXI_REQUIRE(winexinfo::ComputeStatusPaneRect(
        {{-100, -100, 100, 100}, {-100, 50, 100, 100},
         {-100, 50, (std::numeric_limits<LONG>::max)(), 100},
         {(std::numeric_limits<LONG>::min)(), 50, 100, 100}, 192},
        &output).ok());
    WXI_REQUIRE(IsEmpty(output));
}

WXI_TEST(explorer_layout_rejects_invalid_arguments, "explorer_layout.invalid_arguments") {
    RECT output{};
    WXI_REQUIRE_EQ(
        winexinfo::ComputeStatusPaneRect({}, nullptr).code,
        winexinfo::ErrorCode::INVALID_ARGUMENT);

    winexinfo::ExplorerLayoutMetrics zeroDpi{};
    zeroDpi.parent_screen = {0, 0, 100, 100};
    zeroDpi.status_screen = {0, 60, 100, 100};
    zeroDpi.left_group_screen = {0, 60, 10, 100};
    zeroDpi.right_group_screen = {90, 60, 100, 100};
    zeroDpi.dpi = 0;
    WXI_REQUIRE_EQ(
        winexinfo::ComputeStatusPaneRect(zeroDpi, &output).code,
        winexinfo::ErrorCode::INVALID_ARGUMENT);
}
