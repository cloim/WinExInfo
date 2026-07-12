#pragma once

#include "common/status.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace winexinfo {

enum class HostCommand {
    ProbeSnapshot,
    ProbeObserve,
    GateCPlace,
    Background,
};

enum class HostExitCode : int {
    Pass = 0,
    ContractFailure = 1,
    InvalidCli = 2,
    Win32ComFailure = 3,
};

struct ParsedCommand final {
    HostCommand command{};
    std::uint32_t duration_ms = 0;
    std::uint64_t target_hwnd = 0;
};

[[nodiscard]] Status ParseCommandLine(
    std::span<const std::string_view> arguments,
    ParsedCommand* output);

}  // namespace winexinfo
