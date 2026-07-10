#pragma once

#include "host/command_line.h"
#include "probe/probe_types.h"

namespace winexinfo {

struct ProbeRunResult final {
    ProbeReport report;
    HostExitCode exit_code;
};

[[nodiscard]] bool IsProbeTransportFailure(const Status& status);
[[nodiscard]] ProbeRunResult RunSnapshotProbe();

}  // namespace winexinfo
