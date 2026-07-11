#pragma once

#include "common/status.h"

#include <Windows.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace winexinfo::tests {

enum class HookTestMode {
    Target,
    ControllerNormal,
    ControllerFault,
};

struct HookTestCommand final {
    HookTestMode mode{};
    DWORD target_pid = 0;
    std::uint32_t iterations = 0;
};

[[nodiscard]] Status ParseHookTestCommandLine(
    std::span<const std::string_view> arguments,
    HookTestCommand* output) noexcept;

[[nodiscard]] int RunHookTargetMode();
[[nodiscard]] int RunHookControllerMode(const HookTestCommand& command);

}  // namespace winexinfo::tests
