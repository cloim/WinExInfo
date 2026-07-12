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
inline constexpr std::size_t kMaximumTabDescriptors = 4096;

enum class MessageType : std::uint16_t {
    DetachRequest = 3,
    AttachResult = 4,
    DetachResult = 5,
    TabSetUpdate = 6,
    TabSetResult = 7,
    PaneTextUpdate = 8,
    PaneTextResult = 9,
    WindowRemoveRequest = 10,
    WindowRemoveResult = 11,
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

struct TabDescriptor final {
    std::uint64_t tab_hwnd = 0;
    std::uint64_t tab_generation = 0;
    std::uint32_t ui_thread_id = 0;

    bool operator==(const TabDescriptor&) const = default;
};

struct TabSetUpdate final {
    std::uint64_t top_level_hwnd = 0;
    std::uint64_t top_level_generation = 0;
    std::vector<TabDescriptor> tabs;

    bool operator==(const TabSetUpdate&) const = default;
};

struct TabSetResult final {
    std::uint64_t top_level_generation = 0;
    std::uint32_t result = 0;
    std::string error_code;

    bool operator==(const TabSetResult&) const = default;
};

struct PaneTextUpdate final {
    std::uint64_t top_level_hwnd = 0;
    std::uint64_t top_level_generation = 0;
    std::uint64_t tab_hwnd = 0;
    std::uint64_t tab_generation = 0;
    std::string display_text;
    std::string tooltip_text;

    bool operator==(const PaneTextUpdate&) const = default;
};

struct PaneTextResult final {
    std::uint64_t top_level_generation = 0;
    std::uint64_t tab_generation = 0;
    std::uint32_t result = 0;
    std::string error_code;

    bool operator==(const PaneTextResult&) const = default;
};

struct WindowRemoveRequest final {
    std::uint64_t top_level_hwnd = 0;
    std::uint64_t top_level_generation = 0;

    bool operator==(const WindowRemoveRequest&) const = default;
};

struct WindowRemoveResult final {
    std::uint64_t top_level_generation = 0;
    std::uint32_t result = 0;
    std::string error_code;

    bool operator==(const WindowRemoveResult&) const = default;
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
[[nodiscard]] Status EncodeTabSetUpdate(
    std::uint64_t requestId,
    const TabSetUpdate& update,
    std::vector<std::uint8_t>* output);
[[nodiscard]] Status EncodeTabSetResult(
    std::uint64_t requestId,
    const TabSetResult& result,
    std::vector<std::uint8_t>* output);
[[nodiscard]] Status EncodePaneTextUpdate(
    std::uint64_t requestId,
    const PaneTextUpdate& update,
    std::vector<std::uint8_t>* output);
[[nodiscard]] Status EncodePaneTextResult(
    std::uint64_t requestId,
    const PaneTextResult& result,
    std::vector<std::uint8_t>* output);
[[nodiscard]] Status EncodeWindowRemoveRequest(
    std::uint64_t requestId,
    const WindowRemoveRequest& request,
    std::vector<std::uint8_t>* output);
[[nodiscard]] Status EncodeWindowRemoveResult(
    std::uint64_t requestId,
    const WindowRemoveResult& result,
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
[[nodiscard]] Status DecodeTabSetUpdate(
    const DecodedFrame& frame,
    TabSetUpdate* output);
[[nodiscard]] Status DecodeTabSetResult(
    const DecodedFrame& frame,
    std::uint64_t expectedRequestId,
    std::uint64_t expectedTopLevelGeneration,
    TabSetResult* output);
[[nodiscard]] Status DecodePaneTextUpdate(
    const DecodedFrame& frame,
    PaneTextUpdate* output);
[[nodiscard]] Status DecodePaneTextResult(
    const DecodedFrame& frame,
    std::uint64_t expectedRequestId,
    std::uint64_t expectedTopLevelGeneration,
    std::uint64_t expectedTabGeneration,
    PaneTextResult* output);
[[nodiscard]] Status DecodeWindowRemoveRequest(
    const DecodedFrame& frame,
    WindowRemoveRequest* output);
[[nodiscard]] Status DecodeWindowRemoveResult(
    const DecodedFrame& frame,
    std::uint64_t expectedRequestId,
    std::uint64_t expectedTopLevelGeneration,
    WindowRemoveResult* output);

[[nodiscard]] Status AcceptDetachId(
    std::uint64_t requestId,
    std::uint64_t* lastAcceptedId) noexcept;
[[nodiscard]] Status AcceptTopLevelGeneration(
    std::uint64_t candidate,
    std::uint64_t* lastAcceptedGeneration) noexcept;

}  // namespace winexinfo::ipc
