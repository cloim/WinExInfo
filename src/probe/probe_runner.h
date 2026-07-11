#pragma once

#include "host/command_line.h"
#include "probe/probe_types.h"

#include <string_view>

namespace winexinfo {

struct ProbeRunResult final {
    ProbeReport report;
    HostExitCode exit_code;
};

[[nodiscard]] bool IsProbeTransportFailure(const Status& status);
[[nodiscard]] ProbeRunResult CreateObserveInfrastructureFailure(
    std::uint32_t durationMs,
    const Status& failure,
    std::string_view runtimeStage);
[[nodiscard]] ProbeRunResult RunSnapshotProbe();
[[nodiscard]] ProbeRunResult RunObserveProbe(std::uint32_t durationMs);

}  // namespace winexinfo
