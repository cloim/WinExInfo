#pragma once

#include <Windows.h>

#include <exception>
#include <string_view>

namespace winexinfo {

enum class ErrorCode {
    OK,
    INVALID_ARGUMENT,
    UNSUPPORTED_OS_BUILD,
    UNSUPPORTED_EXPLORER_BUILD,
    TARGET_VALIDATION_FAILED,
    TARGET_MITIGATION_BLOCKED,
    HOOK_INSTALL_FAILED,
    HOOK_TRIGGER_FAILED,
    HOOK_RELEASE_FAILED,
    DLL_INITIALIZATION_FAILED,
    WINDOW_ATTACH_FAILED,
    DLL_UNLOAD_TIMEOUT,
    EXPLORER_UI_CONTRACT_MISMATCH,
    ACTIVE_VIEW_CONTRACT_MISMATCH,
    IPC_PROTOCOL_ERROR,
    PIPE_DISCONNECTED,
};

[[nodiscard]] inline std::string_view ToString(const ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::OK:
            return "OK";
        case ErrorCode::INVALID_ARGUMENT:
            return "INVALID_ARGUMENT";
        case ErrorCode::UNSUPPORTED_OS_BUILD:
            return "UNSUPPORTED_OS_BUILD";
        case ErrorCode::UNSUPPORTED_EXPLORER_BUILD:
            return "UNSUPPORTED_EXPLORER_BUILD";
        case ErrorCode::TARGET_VALIDATION_FAILED:
            return "TARGET_VALIDATION_FAILED";
        case ErrorCode::TARGET_MITIGATION_BLOCKED:
            return "TARGET_MITIGATION_BLOCKED";
        case ErrorCode::HOOK_INSTALL_FAILED:
            return "HOOK_INSTALL_FAILED";
        case ErrorCode::HOOK_TRIGGER_FAILED:
            return "HOOK_TRIGGER_FAILED";
        case ErrorCode::HOOK_RELEASE_FAILED:
            return "HOOK_RELEASE_FAILED";
        case ErrorCode::DLL_INITIALIZATION_FAILED:
            return "DLL_INITIALIZATION_FAILED";
        case ErrorCode::WINDOW_ATTACH_FAILED:
            return "WINDOW_ATTACH_FAILED";
        case ErrorCode::DLL_UNLOAD_TIMEOUT:
            return "DLL_UNLOAD_TIMEOUT";
        case ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH:
            return "EXPLORER_UI_CONTRACT_MISMATCH";
        case ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH:
            return "ACTIVE_VIEW_CONTRACT_MISMATCH";
        case ErrorCode::IPC_PROTOCOL_ERROR:
            return "IPC_PROTOCOL_ERROR";
        case ErrorCode::PIPE_DISCONNECTED:
            return "PIPE_DISCONNECTED";
    }

    std::terminate();
}

struct Status final {
    ErrorCode code;
    HRESULT hresult;
    DWORD win32;

    [[nodiscard]] bool ok() const noexcept {
        return ToString(code) == std::string_view{"OK"};
    }
};

}  // namespace winexinfo
