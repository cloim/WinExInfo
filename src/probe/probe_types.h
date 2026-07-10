#pragma once

#include "common/status.h"

#include <cstddef>
#include <string>
#include <vector>

namespace winexinfo {

enum class ProbeMode {
    Snapshot,
    Observe,
};

struct ReportField final {
    std::string key;
    std::string value;
};

struct ReportSection final {
    std::vector<ReportField> fields;
};

struct ProbeReport final {
    ProbeMode mode;
    bool passed;
    std::vector<ReportSection> sections;
    ErrorCode error_code;
};

struct ActiveShellViewSnapshot final {
    Status status;
    std::size_t top_level_entry_count;
    std::size_t shell_tab_match_count;
    std::size_t active_view_count;
    HWND active_view;
    bool filesystem_path_available;
    std::wstring filesystem_path;
};

}  // namespace winexinfo
