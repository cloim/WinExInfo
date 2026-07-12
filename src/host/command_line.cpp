#include "host/command_line.h"

#include <Windows.h>

#include <charconv>
#include <cstdint>
#include <string_view>

namespace winexinfo {
namespace {

Status InvalidCommandStatus() noexcept {
    return Status{ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
}

bool IsDecimalText(const std::string_view text) noexcept {
    if (text.empty()) {
        return false;
    }
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
    }
    return true;
}

bool ParseDuration(
    const std::string_view text,
    const std::uint32_t minimum,
    const std::uint32_t maximum,
    std::uint32_t* const output) noexcept {
    if (output == nullptr || !IsDecimalText(text)) {
        return false;
    }

    std::uint32_t duration = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), duration);
    if (error != std::errc{} || end != text.data() + text.size() || duration < minimum ||
        duration > maximum) {
        return false;
    }

    *output = duration;
    return true;
}

}  // namespace

Status ParseCommandLine(
    const std::span<const std::string_view> arguments,
    ParsedCommand* const output) {
    if (output == nullptr) {
        return InvalidCommandStatus();
    }

    if (arguments.size() == 2 && arguments[0] == "--probe" && arguments[1] == "snapshot") {
        *output = ParsedCommand{HostCommand::ProbeSnapshot, 0, 0};
        return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    if (arguments.size() == 1 && arguments[0] == "--background") {
        *output = ParsedCommand{HostCommand::Background, 0, 0};
        return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    if (arguments.size() == 5 && arguments[0] == "--gate-c-place" &&
        arguments[1] == "--hwnd" && arguments[3] == "--duration-ms") {
        const std::string_view hwndText = arguments[2];
        if (hwndText.size() != 18 || !hwndText.starts_with("0x")) {
            return InvalidCommandStatus();
        }

        const std::string_view hexadecimal = hwndText.substr(2);
        for (const char character : hexadecimal) {
            const bool decimalDigit = character >= '0' && character <= '9';
            const bool uppercaseHexDigit = character >= 'A' && character <= 'F';
            if (!decimalDigit && !uppercaseHexDigit) {
                return InvalidCommandStatus();
            }
        }

        std::uint64_t targetHwnd = 0;
        const auto [hwndEnd, hwndError] = std::from_chars(
            hexadecimal.data(),
            hexadecimal.data() + hexadecimal.size(),
            targetHwnd,
            16);
        if (hwndError != std::errc{} ||
            hwndEnd != hexadecimal.data() + hexadecimal.size() || targetHwnd == 0) {
            return InvalidCommandStatus();
        }

        std::uint32_t duration = 0;
        if (!ParseDuration(arguments[4], 5000, 30000, &duration)) {
            return InvalidCommandStatus();
        }

        *output = ParsedCommand{HostCommand::GateCPlace, duration, targetHwnd};
        return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    if (arguments.size() != 4 || arguments[0] != "--probe" ||
        arguments[1] != "observe" || arguments[2] != "--duration-ms") {
        return InvalidCommandStatus();
    }

    std::uint32_t duration = 0;
    if (!ParseDuration(arguments[3], 1000, 60000, &duration)) {
        return InvalidCommandStatus();
    }

    *output = ParsedCommand{HostCommand::ProbeObserve, duration, 0};
    return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace winexinfo
