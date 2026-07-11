#pragma once

#include "probe/probe_types.h"
#include "probe/uia_probe.h"

#include <string>
#include <string_view>

namespace winexinfo {

[[nodiscard]] Status WriteProbeReport(
    const ProbeReport& report,
    std::string* output);
[[nodiscard]] Status AppendEventObservationReportFields(
    const EventObservationSnapshot& snapshot,
    ProbeReport* output);
void AppendUiaCardinalityReportFields(
    std::string_view prefix,
    const UiaSelectorCardinalities& cardinalities,
    ReportSection* output);
Status InitializeShellTerminalStageReportField(
    std::string_view prefix,
    ReportSection* output);
Status AppendActiveShellViewReportFields(
    std::string_view prefix,
    const ActiveShellViewSnapshot& snapshot,
    ReportSection* output);

}  // namespace winexinfo
