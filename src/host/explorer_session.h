#pragma once

#include "common/status.h"
#include "injection/thread_hook_injector.h"
#include "ipc/protocol.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <mutex>
#include <vector>

namespace winexinfo {

struct SessionWindowSnapshot final {
    HWND top_level = nullptr;
    std::uint64_t top_level_generation = 0;
    std::vector<ipc::TabDescriptor> tabs;

    bool operator==(const SessionWindowSnapshot&) const = default;
};

struct ExplorerSessionOperations final {
    std::function<Status(
        const SessionWindowSnapshot&, injection::HookTarget*)> validate_window;
    std::function<Status(
        const injection::HookTarget&,
        const std::function<Status()>&,
        injection::HookAttachOutcome*)> attach_initial;
    std::function<Status(
        const injection::HookTarget&,
        const std::function<Status()>&)> ensure_thread_hook_lease;
    std::function<Status(
        const std::vector<std::uint8_t>&, ipc::DecodedFrame*)> exchange;
    std::function<Status(DWORD)> release_hooks;
    std::function<Status(DWORD, DWORD, bool*)> wait_exact_module_absent;
    std::function<Status(DWORD)> confirm_target_gone;
};

class ExplorerSession final {
public:
    ExplorerSession(DWORD processId, ExplorerSessionOperations operations);
    ExplorerSession(const ExplorerSession&) = delete;
    ExplorerSession& operator=(const ExplorerSession&) = delete;

    [[nodiscard]] Status Reconcile(std::span<const SessionWindowSnapshot> windows);
    [[nodiscard]] Status Stop();

private:
    struct WindowState final {
        SessionWindowSnapshot snapshot;
        DWORD ui_thread_id = 0;
    };

    [[nodiscard]] Status NextRequestId(std::uint64_t* output) noexcept;
    [[nodiscard]] Status SendUpdate(const SessionWindowSnapshot& snapshot);
    [[nodiscard]] Status SendRemoval(const WindowState& window);

    DWORD process_id_ = 0;
    std::mutex mutex_;
    ExplorerSessionOperations operations_;
    std::vector<WindowState> windows_;
    std::vector<DWORD> installed_thread_ids_;
    std::uint64_t last_request_id_ = 0;
    bool attached_ = false;
    bool uncertain_ = false;
    bool release_complete_ = false;
    bool detach_complete_ = false;
    bool stopped_ = false;
};

[[nodiscard]] ExplorerSessionOperations CreateProductionExplorerSessionOperations(
    DWORD processId,
    std::wstring hookDllPath);

}  // namespace winexinfo
