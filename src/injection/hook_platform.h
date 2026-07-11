#pragma once

#include "common/status.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace winexinfo::injection {

struct HookReleaseEvent final {
    HANDLE handle = nullptr;
    bool already_exists = false;
    bool manual_reset = false;
    bool initially_signaled = false;
    bool current_user_only = false;
    bool unexpected_principal_present = false;
};

struct HookAttachReceipt final {
    bool available = false;
    std::uint64_t request_id = 0;
    DWORD explorer_pid = 0;
    DWORD ui_thread_id = 0;
    HWND top_level_hwnd = nullptr;
    std::uint32_t result = 0;
    std::string error_code;
};

struct HookPlatformOperations final {
    std::function<Status(std::wstring_view, HookReleaseEvent*)>
        create_release_event;
    std::function<Status(std::wstring_view, UINT*)> register_message;
    std::function<Status(std::string_view, HMODULE*, HOOKPROC*)>
        resolve_hook_export;
    std::function<Status(int, HOOKPROC, HMODULE, DWORD, HHOOK*)> set_hook;
    std::function<Status(HWND, UINT, WPARAM, LPARAM, UINT, UINT)>
        send_message_timeout;
    std::function<Status(std::uint64_t, DWORD, HookAttachReceipt*)>
        wait_attach_result;
    std::function<Status(HHOOK, bool*)> unhook;
    std::function<Status(HANDLE, bool*)> set_event;
    std::function<void(HANDLE)> close_event;
};

using HookAttachWaitOperation =
    std::function<Status(std::uint64_t, DWORD, HookAttachReceipt*)>;

[[nodiscard]] Status InspectHookObjectSecurity(
    HANDLE object,
    bool* currentUserPresent,
    bool* unexpectedPrincipalPresent);

[[nodiscard]] HookPlatformOperations CreateProductionHookPlatformOperations(
    std::wstring hookDllPath,
    HookAttachWaitOperation waitAttachResult);

}  // namespace winexinfo::injection
