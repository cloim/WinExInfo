#pragma once

#include "probe/probe_types.h"

#include <string>

namespace winexinfo {

[[nodiscard]] std::string WriteProbeReport(const ProbeReport& report);

}  // namespace winexinfo
