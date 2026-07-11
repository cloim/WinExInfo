#include "hook_test_modes.h"

#include <Windows.h>

#include <charconv>
#include <limits>

namespace winexinfo::tests {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status Invalid() noexcept {
    return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
}

bool ParseDecimal(const std::string_view text, std::uint32_t* const output) {
    if (text.empty() || output == nullptr) {
        return false;
    }
    std::uint32_t candidate = 0;
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), candidate, 10);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return false;
    }
    *output = candidate;
    return true;
}

}  // namespace

Status ParseHookTestCommandLine(
    const std::span<const std::string_view> arguments,
    HookTestCommand* const output) noexcept {
    if (output == nullptr) {
        return Invalid();
    }
    if (arguments.size() == 2 && arguments[1] == "--hook-target") {
        *output = {HookTestMode::Target, 0, 0};
        return Success();
    }
    if (arguments.size() != 5 || arguments[1] != "--hook-controller") {
        return Invalid();
    }
    std::uint32_t pid = 0;
    if (!ParseDecimal(arguments[2], &pid) || pid == 0) {
        return Invalid();
    }
    if (arguments[3] == "--iterations") {
        std::uint32_t iterations = 0;
        if (!ParseDecimal(arguments[4], &iterations) ||
            iterations == 0 || iterations > 100) {
            return Invalid();
        }
        *output = {HookTestMode::ControllerNormal, pid, iterations};
        return Success();
    }
    if (arguments[3] == "--fault" &&
        arguments[4] == "unhook-failure") {
        *output = {HookTestMode::ControllerFault, pid, 0};
        return Success();
    }
    return Invalid();
}

}  // namespace winexinfo::tests
