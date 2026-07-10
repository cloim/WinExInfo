#pragma once

#include "common/status.h"

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

}  // namespace winexinfo
