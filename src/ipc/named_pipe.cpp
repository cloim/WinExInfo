#include "ipc/named_pipe.h"

#include <Aclapi.h>
#include <Sddl.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <vector>

namespace winexinfo::ipc {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status InvalidArgument() noexcept {
    return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
}

Status ProtocolError() noexcept {
    return {ErrorCode::IPC_PROTOCOL_ERROR, S_FALSE, ERROR_INVALID_DATA};
}

Status Win32Failure(const ErrorCode code, const DWORD error) noexcept {
    return {code, HRESULT_FROM_WIN32(error), error};
}

Status CurrentUserSid(std::vector<std::uint8_t>* const output) {
    if (output == nullptr) {
        return InvalidArgument();
    }
    UniqueHandle token;
    HANDLE rawToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken) == FALSE) {
        return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, GetLastError());
    }
    token.reset(rawToken);
    DWORD size = 0;
    if (GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size) != FALSE ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, GetLastError());
    }
    try {
        std::vector<std::uint8_t> buffer(size);
        if (GetTokenInformation(
                token.get(), TokenUser, buffer.data(), size, &size) == FALSE) {
            return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, GetLastError());
        }
        const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        const DWORD sidLength = GetLengthSid(tokenUser->User.Sid);
        if (sidLength == 0) {
            return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, GetLastError());
        }
        std::vector<std::uint8_t> sid(sidLength);
        if (CopySid(sidLength, sid.data(), tokenUser->User.Sid) == FALSE) {
            return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, GetLastError());
        }
        *output = std::move(sid);
        return Success();
    } catch (const std::bad_alloc&) {
        return {ErrorCode::IPC_PROTOCOL_ERROR, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

Status ReadExact(
    const HANDLE pipe,
    std::span<std::uint8_t> bytes) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD request = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (ReadFile(pipe, bytes.data() + offset, request, &read, nullptr) == FALSE) {
            return Win32Failure(ErrorCode::PIPE_DISCONNECTED, GetLastError());
        }
        if (read == 0) {
            return Win32Failure(ErrorCode::PIPE_DISCONNECTED, ERROR_BROKEN_PIPE);
        }
        offset += read;
    }
    return Success();
}

std::uint32_t HeaderPayloadLength(
    const std::array<std::uint8_t, kFrameHeaderSize>& header) noexcept {
    return static_cast<std::uint32_t>(header[8]) |
        static_cast<std::uint32_t>(header[9]) << 8 |
        static_cast<std::uint32_t>(header[10]) << 16 |
        static_cast<std::uint32_t>(header[11]) << 24;
}

}  // namespace

Status BuildCurrentUserPipeName(std::wstring* const output) {
    if (output == nullptr) {
        return InvalidArgument();
    }
    std::vector<std::uint8_t> sid;
    const Status captured = CurrentUserSid(&sid);
    if (!captured.ok()) {
        return captured;
    }
    LPWSTR sidText = nullptr;
    if (ConvertSidToStringSidW(sid.data(), &sidText) == FALSE || sidText == nullptr) {
        return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, GetLastError());
    }
    try {
        *output = L"\\\\.\\pipe\\WinExInfo.v1." + std::wstring{sidText};
    } catch (const std::bad_alloc&) {
        LocalFree(sidText);
        return {ErrorCode::IPC_PROTOCOL_ERROR, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
    LocalFree(sidText);
    return Success();
}

Status CreateControllerPipeServer(
    const std::wstring_view pipeName,
    UniqueHandle* const output) {
    if (output == nullptr || *output || pipeName.empty() ||
        pipeName.find(L'\0') != std::wstring_view::npos) {
        return InvalidArgument();
    }
    std::vector<std::uint8_t> sid;
    const Status captured = CurrentUserSid(&sid);
    if (!captured.ok()) {
        return captured;
    }
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
    PACL acl = nullptr;
    const DWORD aclResult = SetEntriesInAclW(1, &access, nullptr, &acl);
    if (aclResult != ERROR_SUCCESS || acl == nullptr) {
        return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, aclResult);
    }
    SECURITY_DESCRIPTOR descriptor{};
    if (InitializeSecurityDescriptor(
            &descriptor, SECURITY_DESCRIPTOR_REVISION) == FALSE ||
        SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) == FALSE) {
        const DWORD error = GetLastError();
        LocalFree(acl);
        return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, error);
    }
    SECURITY_ATTRIBUTES attributes{
        sizeof(SECURITY_ATTRIBUTES),
        &descriptor,
        FALSE,
    };
    std::wstring name{pipeName};
    const HANDLE pipe = CreateNamedPipeW(
        name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        kControllerPipeMode,
        1,
        static_cast<DWORD>(kMaximumFrameSize),
        static_cast<DWORD>(kMaximumFrameSize),
        0,
        &attributes);
    const DWORD error = pipe == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    LocalFree(acl);
    if (pipe == INVALID_HANDLE_VALUE) {
        return Win32Failure(ErrorCode::PIPE_DISCONNECTED, error);
    }
    output->reset(pipe);
    return Success();
}

Status ConnectHookPipeClient(
    const std::wstring_view pipeName,
    UniqueHandle* const output) {
    if (output == nullptr || *output || pipeName.empty() ||
        pipeName.find(L'\0') != std::wstring_view::npos) {
        return InvalidArgument();
    }
    const std::wstring name{pipeName};
    const HANDLE pipe = CreateFileW(
        name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return Win32Failure(ErrorCode::PIPE_DISCONNECTED, GetLastError());
    }
    DWORD mode = PIPE_READMODE_BYTE;
    if (SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) == FALSE) {
        const DWORD error = GetLastError();
        CloseHandle(pipe);
        return Win32Failure(ErrorCode::PIPE_DISCONNECTED, error);
    }
    output->reset(pipe);
    return Success();
}

Status CreateLocalPipePair(
    const std::wstring_view pipeName,
    UniqueHandle* const server,
    UniqueHandle* const client) {
    if (server == nullptr || client == nullptr || *server || *client) {
        return InvalidArgument();
    }
    UniqueHandle candidateServer;
    Status status = CreateControllerPipeServer(pipeName, &candidateServer);
    if (!status.ok()) {
        return status;
    }
    UniqueHandle candidateClient;
    status = ConnectHookPipeClient(pipeName, &candidateClient);
    if (!status.ok()) {
        return status;
    }
    if (ConnectNamedPipe(candidateServer.get(), nullptr) == FALSE &&
        GetLastError() != ERROR_PIPE_CONNECTED) {
        return Win32Failure(ErrorCode::PIPE_DISCONNECTED, GetLastError());
    }
    *server = std::move(candidateServer);
    *client = std::move(candidateClient);
    return Success();
}

Status InspectPipeSecurity(
    const HANDLE pipe,
    PipeSecurityInspection* const output) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || output == nullptr) {
        return InvalidArgument();
    }
    std::vector<std::uint8_t> currentSid;
    Status status = CurrentUserSid(&currentSid);
    if (!status.ok()) {
        return status;
    }
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD securityResult = GetSecurityInfo(
        pipe,
        SE_KERNEL_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &dacl,
        nullptr,
        &descriptor);
    if (securityResult != ERROR_SUCCESS || dacl == nullptr || descriptor == nullptr) {
        LocalFree(descriptor);
        return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, securityResult);
    }
    PipeSecurityInspection candidate{};
    ACL_SIZE_INFORMATION information{};
    if (GetAclInformation(
            dacl,
            &information,
            sizeof(information),
            AclSizeInformation) == FALSE) {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, error);
    }
    for (DWORD index = 0; index < information.AceCount; ++index) {
        void* rawAce = nullptr;
        if (GetAce(dacl, index, &rawAce) == FALSE || rawAce == nullptr) {
            const DWORD error = GetLastError();
            LocalFree(descriptor);
            return Win32Failure(ErrorCode::IPC_PROTOCOL_ERROR, error);
        }
        const auto* header = static_cast<const ACE_HEADER*>(rawAce);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            candidate.unexpected_principal_present = true;
            continue;
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(rawAce);
        PSID aceSid = const_cast<DWORD*>(&ace->SidStart);
        if (EqualSid(aceSid, currentSid.data()) != FALSE) {
            candidate.current_user_present = true;
        } else {
            candidate.unexpected_principal_present = true;
        }
    }
    DWORD flags = 0;
    if (GetNamedPipeInfo(pipe, &flags, nullptr, nullptr, nullptr) == FALSE) {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        return Win32Failure(ErrorCode::PIPE_DISCONNECTED, error);
    }
    DWORD state = 0;
    if (GetNamedPipeHandleStateW(
            pipe, &state, nullptr, nullptr, nullptr, nullptr, 0) == FALSE) {
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        return Win32Failure(ErrorCode::PIPE_DISCONNECTED, error);
    }
    candidate.byte_type = (flags & PIPE_TYPE_MESSAGE) == 0;
    candidate.byte_read_mode = (state & PIPE_READMODE_MESSAGE) == 0;
    LocalFree(descriptor);
    *output = candidate;
    return Success();
}

Status ValidatePipeSecurityInspection(
    const PipeSecurityInspection& inspection) noexcept {
    return inspection.current_user_present &&
            !inspection.unexpected_principal_present &&
            inspection.byte_type && inspection.byte_read_mode
        ? Success()
        : ProtocolError();
}

Status WriteFrame(
    const HANDLE pipe,
    const std::span<const std::uint8_t> frame) noexcept {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE || frame.empty() ||
        frame.size() > kMaximumFrameSize) {
        return InvalidArgument();
    }
    std::size_t offset = 0;
    while (offset < frame.size()) {
        DWORD written = 0;
        const DWORD request = static_cast<DWORD>(frame.size() - offset);
        if (WriteFile(pipe, frame.data() + offset, request, &written, nullptr) == FALSE) {
            return Win32Failure(ErrorCode::PIPE_DISCONNECTED, GetLastError());
        }
        if (written == 0) {
            return Win32Failure(ErrorCode::PIPE_DISCONNECTED, ERROR_BROKEN_PIPE);
        }
        offset += written;
    }
    return Success();
}

Status ReadFrame(UniqueHandle* const pipe, DecodedFrame* const output) {
    if (pipe == nullptr || !*pipe || output == nullptr) {
        return InvalidArgument();
    }
    std::array<std::uint8_t, kFrameHeaderSize> header{};
    const Status headerRead = ReadExact(pipe->get(), header);
    if (!headerRead.ok()) {
        pipe->reset();
        return headerRead;
    }
    const std::uint32_t payloadLength = HeaderPayloadLength(header);
    const bool validHeader =
        std::equal(header.begin(), header.begin() + 4, std::array{'W', 'X', 'I', '1'}.begin()) &&
        header[4] == 1 && header[5] == 0 && header[7] == 0 &&
        (header[6] == 3 || header[6] == 4 || header[6] == 5) &&
        payloadLength <= kMaximumFrameSize - kFrameHeaderSize;
    if (!validHeader) {
        pipe->reset();
        return ProtocolError();
    }
    try {
        std::vector<std::uint8_t> frame;
        frame.reserve(kFrameHeaderSize + payloadLength);
        frame.insert(frame.end(), header.begin(), header.end());
        if (payloadLength != 0) {
            std::vector<std::uint8_t> payload(payloadLength);
            const Status payloadRead = ReadExact(pipe->get(), payload);
            if (!payloadRead.ok()) {
                pipe->reset();
                return payloadRead;
            }
            frame.insert(frame.end(), payload.begin(), payload.end());
        }
        const Status decoded = DecodeFrame(frame, output);
        if (!decoded.ok()) {
            pipe->reset();
        }
        return decoded;
    } catch (const std::bad_alloc&) {
        pipe->reset();
        return {ErrorCode::IPC_PROTOCOL_ERROR, E_OUTOFMEMORY, ERROR_SUCCESS};
    }
}

}  // namespace winexinfo::ipc
