#pragma once

#include "common/status.h"
#include "common/win32_handle.h"
#include "ipc/protocol.h"

#include <Windows.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace winexinfo::ipc {

inline constexpr DWORD kControllerPipeMode =
    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;

struct PipeSecurityInspection final {
    bool current_user_present = false;
    bool unexpected_principal_present = false;
    bool byte_type = false;
    bool byte_read_mode = false;
};

[[nodiscard]] Status BuildCurrentUserPipeName(std::wstring* output);
[[nodiscard]] Status CreateControllerPipeServer(
    std::wstring_view pipeName,
    UniqueHandle* output);
[[nodiscard]] Status ConnectHookPipeClient(
    std::wstring_view pipeName,
    UniqueHandle* output);
[[nodiscard]] Status CreateLocalPipePair(
    std::wstring_view pipeName,
    UniqueHandle* server,
    UniqueHandle* client);
[[nodiscard]] Status InspectPipeSecurity(
    HANDLE pipe,
    PipeSecurityInspection* output);
[[nodiscard]] Status ValidatePipeSecurityInspection(
    const PipeSecurityInspection& inspection) noexcept;
[[nodiscard]] Status WriteFrame(
    HANDLE pipe,
    std::span<const std::uint8_t> frame) noexcept;
[[nodiscard]] Status ReadFrame(
    UniqueHandle* pipe,
    DecodedFrame* output);

}  // namespace winexinfo::ipc
