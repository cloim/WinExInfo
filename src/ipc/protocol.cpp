#include "ipc/protocol.h"

#include "common/utf8.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace winexinfo::ipc {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status InvalidArgument() noexcept {
    return {
        ErrorCode::INVALID_ARGUMENT,
        E_INVALIDARG,
        ERROR_INVALID_PARAMETER,
    };
}

Status ProtocolError() noexcept {
    return {
        ErrorCode::IPC_PROTOCOL_ERROR,
        S_FALSE,
        ERROR_INVALID_DATA,
    };
}

void AppendU16(std::vector<std::uint8_t>* const output, const std::uint16_t value) {
    output->push_back(static_cast<std::uint8_t>(value));
    output->push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendU32(std::vector<std::uint8_t>* const output, const std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        output->push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

void AppendU64(std::vector<std::uint8_t>* const output, const std::uint64_t value) {
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        output->push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

bool ReadU16(
    const std::span<const std::uint8_t> bytes,
    std::size_t* const offset,
    std::uint16_t* const output) noexcept {
    if (*offset > bytes.size() || bytes.size() - *offset < 2) {
        return false;
    }
    *output = static_cast<std::uint16_t>(bytes[*offset]) |
        static_cast<std::uint16_t>(bytes[*offset + 1]) << 8;
    *offset += 2;
    return true;
}

bool ReadU32(
    const std::span<const std::uint8_t> bytes,
    std::size_t* const offset,
    std::uint32_t* const output) noexcept {
    if (*offset > bytes.size() || bytes.size() - *offset < 4) {
        return false;
    }
    std::uint32_t value = 0;
    for (unsigned int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[*offset + index]) <<
            (index * 8);
    }
    *offset += 4;
    *output = value;
    return true;
}

bool ReadU64(
    const std::span<const std::uint8_t> bytes,
    std::size_t* const offset,
    std::uint64_t* const output) noexcept {
    if (*offset > bytes.size() || bytes.size() - *offset < 8) {
        return false;
    }
    std::uint64_t value = 0;
    for (unsigned int index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[*offset + index]) <<
            (index * 8);
    }
    *offset += 8;
    *output = value;
    return true;
}

Status ValidateString(const std::string& value) {
    if (value.size() >= kMaximumStringSize ||
        value.find('\0') != std::string::npos) {
        return ProtocolError();
    }
    std::wstring converted;
    return Utf16FromUtf8(value, &converted).ok() ? Success() : ProtocolError();
}

Status AppendString(
    std::vector<std::uint8_t>* const output,
    const std::string& value) {
    const Status valid = ValidateString(value);
    if (!valid.ok()) {
        return valid;
    }
    AppendU32(output, static_cast<std::uint32_t>(value.size()));
    output->insert(output->end(), value.begin(), value.end());
    return Success();
}

Status ReadString(
    const std::span<const std::uint8_t> bytes,
    std::size_t* const offset,
    std::string* const output) {
    std::uint32_t length = 0;
    if (!ReadU32(bytes, offset, &length) ||
        length >= kMaximumStringSize || *offset > bytes.size() ||
        bytes.size() - *offset < length) {
        return ProtocolError();
    }
    std::string candidate(
        reinterpret_cast<const char*>(bytes.data() + *offset), length);
    *offset += length;
    const Status valid = ValidateString(candidate);
    if (!valid.ok()) {
        return valid;
    }
    *output = std::move(candidate);
    return Success();
}

Status ValidateResultRule(const std::uint32_t result, const std::string& error) {
    const Status validString = ValidateString(error);
    if (!validString.ok() || (result == 0) != error.empty()) {
        return ProtocolError();
    }
    return Success();
}

Status EncodeFrame(
    const MessageType type,
    const std::uint64_t requestId,
    std::vector<std::uint8_t> payload,
    std::vector<std::uint8_t>* const output) {
    if (output == nullptr) {
        return InvalidArgument();
    }
    if (requestId == 0 || payload.size() > kMaximumFrameSize - kFrameHeaderSize ||
        payload.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return ProtocolError();
    }
    try {
        std::vector<std::uint8_t> candidate;
        candidate.reserve(kFrameHeaderSize + payload.size());
        candidate.insert(candidate.end(), {'W', 'X', 'I', '1'});
        AppendU16(&candidate, kProtocolVersion);
        AppendU16(&candidate, static_cast<std::uint16_t>(type));
        AppendU32(&candidate, static_cast<std::uint32_t>(payload.size()));
        AppendU64(&candidate, requestId);
        candidate.insert(candidate.end(), payload.begin(), payload.end());
        *output = std::move(candidate);
        return Success();
    } catch (const std::bad_alloc&) {
        return {ErrorCode::IPC_PROTOCOL_ERROR, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

}  // namespace

Status EncodeDetachRequest(
    const std::uint64_t requestId,
    std::vector<std::uint8_t>* const output) {
    return EncodeFrame(MessageType::DetachRequest, requestId, {}, output);
}

Status EncodeAttachResult(
    const std::uint64_t requestId,
    const AttachResult& result,
    std::vector<std::uint8_t>* const output) {
    const Status valid = ValidateResultRule(result.result, result.error_code);
    if (!valid.ok()) {
        return valid;
    }
    try {
        std::vector<std::uint8_t> payload;
        payload.reserve(24 + result.error_code.size());
        AppendU32(&payload, result.explorer_pid);
        AppendU32(&payload, result.ui_thread_id);
        AppendU64(&payload, result.top_level_hwnd);
        AppendU32(&payload, result.result);
        const Status appended = AppendString(&payload, result.error_code);
        return appended.ok()
            ? EncodeFrame(MessageType::AttachResult, requestId, std::move(payload), output)
            : appended;
    } catch (const std::bad_alloc&) {
        return {ErrorCode::IPC_PROTOCOL_ERROR, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

Status EncodeDetachResult(
    const std::uint64_t requestId,
    const DetachResult& result,
    std::vector<std::uint8_t>* const output) {
    const Status valid = ValidateResultRule(result.result, result.error_code);
    if (!valid.ok()) {
        return valid;
    }
    try {
        std::vector<std::uint8_t> payload;
        payload.reserve(12 + result.error_code.size());
        AppendU32(&payload, result.explorer_pid);
        AppendU32(&payload, result.result);
        const Status appended = AppendString(&payload, result.error_code);
        return appended.ok()
            ? EncodeFrame(MessageType::DetachResult, requestId, std::move(payload), output)
            : appended;
    } catch (const std::bad_alloc&) {
        return {ErrorCode::IPC_PROTOCOL_ERROR, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

Status DecodeFrame(
    const std::span<const std::uint8_t> bytes,
    DecodedFrame* const output) {
    if (output == nullptr) {
        return InvalidArgument();
    }
    if (bytes.size() < kFrameHeaderSize || bytes.size() > kMaximumFrameSize ||
        !std::equal(bytes.begin(), bytes.begin() + 4, std::array{'W', 'X', 'I', '1'}.begin())) {
        return ProtocolError();
    }
    std::size_t offset = 4;
    std::uint16_t version = 0;
    std::uint16_t rawType = 0;
    std::uint32_t payloadLength = 0;
    std::uint64_t requestId = 0;
    if (!ReadU16(bytes, &offset, &version) ||
        !ReadU16(bytes, &offset, &rawType) ||
        !ReadU32(bytes, &offset, &payloadLength) ||
        !ReadU64(bytes, &offset, &requestId) ||
        version != kProtocolVersion || requestId == 0 ||
        payloadLength > kMaximumFrameSize - kFrameHeaderSize ||
        bytes.size() != kFrameHeaderSize + payloadLength ||
        (rawType != static_cast<std::uint16_t>(MessageType::DetachRequest) &&
         rawType != static_cast<std::uint16_t>(MessageType::AttachResult) &&
         rawType != static_cast<std::uint16_t>(MessageType::DetachResult))) {
        return ProtocolError();
    }
    try {
        DecodedFrame candidate{
            static_cast<MessageType>(rawType),
            requestId,
            std::vector<std::uint8_t>(bytes.begin() + kFrameHeaderSize, bytes.end()),
        };
        *output = std::move(candidate);
        return Success();
    } catch (const std::bad_alloc&) {
        return {ErrorCode::IPC_PROTOCOL_ERROR, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

Status DecodeDetachRequest(const DecodedFrame& frame) noexcept {
    return frame.message_type == MessageType::DetachRequest && frame.payload.empty()
        ? Success()
        : ProtocolError();
}

Status DecodeAttachResult(const DecodedFrame& frame, AttachResult* const output) {
    if (output == nullptr) {
        return InvalidArgument();
    }
    if (frame.message_type != MessageType::AttachResult) {
        return ProtocolError();
    }
    std::size_t offset = 0;
    AttachResult candidate{};
    if (!ReadU32(frame.payload, &offset, &candidate.explorer_pid) ||
        !ReadU32(frame.payload, &offset, &candidate.ui_thread_id) ||
        !ReadU64(frame.payload, &offset, &candidate.top_level_hwnd) ||
        !ReadU32(frame.payload, &offset, &candidate.result)) {
        return ProtocolError();
    }
    const Status read = ReadString(frame.payload, &offset, &candidate.error_code);
    const Status valid = ValidateResultRule(candidate.result, candidate.error_code);
    if (!read.ok() || !valid.ok() || offset != frame.payload.size()) {
        return ProtocolError();
    }
    *output = std::move(candidate);
    return Success();
}

Status DecodeDetachResult(
    const DecodedFrame& frame,
    const std::uint64_t expectedRequestId,
    DetachResult* const output) {
    if (output == nullptr) {
        return InvalidArgument();
    }
    if (frame.message_type != MessageType::DetachResult || expectedRequestId == 0 ||
        frame.request_id != expectedRequestId) {
        return ProtocolError();
    }
    std::size_t offset = 0;
    DetachResult candidate{};
    if (!ReadU32(frame.payload, &offset, &candidate.explorer_pid) ||
        !ReadU32(frame.payload, &offset, &candidate.result)) {
        return ProtocolError();
    }
    const Status read = ReadString(frame.payload, &offset, &candidate.error_code);
    const Status valid = ValidateResultRule(candidate.result, candidate.error_code);
    if (!read.ok() || !valid.ok() || offset != frame.payload.size()) {
        return ProtocolError();
    }
    *output = std::move(candidate);
    return Success();
}

Status AcceptDetachId(
    const std::uint64_t requestId,
    std::uint64_t* const lastAcceptedId) noexcept {
    if (lastAcceptedId == nullptr) {
        return InvalidArgument();
    }
    if (requestId == 0 || requestId <= *lastAcceptedId) {
        return ProtocolError();
    }
    *lastAcceptedId = requestId;
    return Success();
}

}  // namespace winexinfo::ipc
