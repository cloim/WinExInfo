#pragma once

#include "common/status.h"

#include <Windows.h>

namespace winexinfo {

struct ExplorerLayoutMetrics final {
    RECT parent_screen{};
    RECT status_screen{};
    RECT left_group_screen{};
    RECT right_group_screen{};
    UINT dpi = 96;
};

[[nodiscard]] Status ComputeStatusPaneRect(
    const ExplorerLayoutMetrics& metrics,
    RECT* output) noexcept;
[[nodiscard]] Status CaptureExplorerLayout(
    HWND topLevel,
    ExplorerLayoutMetrics* metrics,
    HWND* paneParent);

}  // namespace winexinfo
