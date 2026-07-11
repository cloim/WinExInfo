#pragma once

#include "common/status.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace winexinfo::ipc {

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::size_t kFrameHeaderSize = 20;
inline constexpr std::size_t kMaximumFrameSize = 256 * 1024;
inline constexpr std::size_t kMaximumStringSize = 256 * 1024;

enum class MessageType : std::uint16_t {
    DetachRequest = 3,
    AttachResult = 4,
    DetachResult = 5,
};

struct DecodedFrame final {
    MessageType message_type{};
    std::uint64_t request_id = 0;
    std::vector<std::uint8_t> payload;
};

struct AttachResult final {
    std::uint32_t explorer_pid = 0;
    std::uint32_t ui_thread_id = 0;
    std::uint64_t top_level_hwnd = 0;
    std::uint32_t result = 0;
    std::string error_code;

    bool operator==(const AttachResult&) const = default;
};

struct DetachResult final {
    std::uint32_t explorer_pid = 0;
    std::uint32_t result = 0;
    std::string error_code;

    bool operator==(const DetachResult&) const = default;
};

[[nodiscard]] Status EncodeDetachRequest(
    std::uint64_t requestId,
    std::vector<std::uint8_t>* output);
[[nodiscard]] Status EncodeAttachResult(
    std::uint64_t requestId,
    const AttachResult& result,
    std::vector<std::uint8_t>* output);
[[nodiscard]] Status EncodeDetachResult(
    std::uint64_t requestId,
    const DetachResult& result,
    std::vector<std::uint8_t>* output);

[[nodiscard]] Status DecodeFrame(
    std::span<const std::uint8_t> bytes,
    DecodedFrame* output);
[[nodiscard]] Status DecodeDetachRequest(const DecodedFrame& frame) noexcept;
[[nodiscard]] Status DecodeAttachResult(
    const DecodedFrame& frame,
    AttachResult* output);
[[nodiscard]] Status DecodeDetachResult(
    const DecodedFrame& frame,
    std::uint64_t expectedRequestId,
    DetachResult* output);

[[nodiscard]] Status AcceptDetachId(
    std::uint64_t requestId,
    std::uint64_t* lastAcceptedId) noexcept;

}  // namespace winexinfo::ipc
