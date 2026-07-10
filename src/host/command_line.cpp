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

}  // namespace

Status ParseCommandLine(
    const std::span<const std::string_view> arguments,
    ParsedCommand* const output) {
    if (output == nullptr) {
        return InvalidCommandStatus();
    }

    if (arguments.size() == 2 && arguments[0] == "--probe" && arguments[1] == "snapshot") {
        *output = ParsedCommand{HostCommand::ProbeSnapshot, 0};
        return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    if (arguments.size() != 4 || arguments[0] != "--probe" ||
        arguments[1] != "observe" || arguments[2] != "--duration-ms") {
        return InvalidCommandStatus();
    }

    const std::string_view durationText = arguments[3];
    if (durationText.empty()) {
        return InvalidCommandStatus();
    }

    for (const char character : durationText) {
        if (character < '0' || character > '9') {
            return InvalidCommandStatus();
        }
    }

    std::uint32_t duration = 0;
    const auto [end, error] =
        std::from_chars(durationText.data(), durationText.data() + durationText.size(), duration);
    if (error != std::errc{} || end != durationText.data() + durationText.size() || duration < 1000 ||
        duration > 60000) {
        return InvalidCommandStatus();
    }

    *output = ParsedCommand{HostCommand::ProbeObserve, duration};
    return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace winexinfo
