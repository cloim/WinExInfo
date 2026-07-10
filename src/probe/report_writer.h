#pragma once

#include "probe/probe_types.h"
#include "probe/uia_probe.h"

#include <string>
#include <string_view>

namespace winexinfo {

[[nodiscard]] std::string WriteProbeReport(const ProbeReport& report);
void AppendUiaCardinalityReportFields(
    std::string_view prefix,
    const UiaSelectorCardinalities& cardinalities,
    ReportSection* output);

}  // namespace winexinfo
