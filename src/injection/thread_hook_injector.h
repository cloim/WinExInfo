#pragma once

#include "common/status.h"
#include "injection/hook_platform.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace winexinfo::injection {

inline constexpr WPARAM kAttachMagic = 0x57495831;
inline constexpr std::wstring_view kAttachMessageName = L"WinExInfo.Attach.v1";
inline constexpr std::string_view kHookExportName = "WinExInfoCallWndProc";

struct HookTarget final {
    DWORD explorer_pid = 0;
    DWORD ui_thread_id = 0;
    HWND top_level_hwnd = nullptr;
};

struct HookAttachOutcome final {
    std::uint64_t attach_id = 0;
    std::wstring event_name;
    bool hook_released = false;
    bool release_event_signaled = false;
    bool unload_authorized = false;
    std::optional<Status> original_status;
};

class ThreadHookInjector final {
public:
    // Calls are externally serialized. ExplorerSession adds synchronization in C2.
    explicit ThreadHookInjector(
        HookPlatformOperations operations,
        std::uint64_t lastAttachId = 0,
        std::function<void()> beforeRetainedTargetReservation = {});
    ~ThreadHookInjector();

    ThreadHookInjector(const ThreadHookInjector&) = delete;
    ThreadHookInjector& operator=(const ThreadHookInjector&) = delete;

    [[nodiscard]] Status Attach(
        const HookTarget& target,
        HookAttachOutcome* output);
    [[nodiscard]] Status EnsureThreadHookLease(
        const HookTarget& target,
        const std::function<Status()>& finalValidate);
    [[nodiscard]] Status ReleaseHookForDetach(DWORD explorerPid) noexcept;
    [[nodiscard]] Status ConfirmTargetGone(DWORD explorerPid) noexcept;
    [[nodiscard]] std::size_t retained_target_count() const noexcept;

private:
    static constexpr std::size_t kMaximumThreadHookLeases = 64;

    struct ThreadHookLease final {
        DWORD ui_thread_id = 0;
        HHOOK hook = nullptr;
    };

    struct RetainedTarget final {
        std::array<ThreadHookLease, kMaximumThreadHookLeases> leases{};
        std::size_t lease_count = 0;
        HANDLE release_event = nullptr;
        bool release_event_signaled = false;
        bool release_started = false;
    };

    HookPlatformOperations operations_;
    std::uint64_t last_attach_id_ = 0;
    std::function<void()> before_retained_target_reservation_;
    std::map<DWORD, RetainedTarget> retained_targets_;
};

}  // namespace winexinfo::injection
