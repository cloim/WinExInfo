#pragma once

#include "common/status.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace winexinfo {

enum class HostCommand {
    ProbeSnapshot,
    ProbeObserve,
};

enum class HostExitCode : int {
    Pass = 0,
    ContractFailure = 1,
    InvalidCli = 2,
    Win32ComFailure = 3,
};

struct ParsedCommand final {
    HostCommand command;
    std::uint32_t duration_ms;
};

[[nodiscard]] Status ParseCommandLine(
    std::span<const std::string_view> arguments,
    ParsedCommand* output);

}  // namespace winexinfo
