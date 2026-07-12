#pragma once

#include "common/status.h"
#include "hook/status_pane.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace winexinfo::hook {

class TabSubclassSet;

enum class RuntimeState {
    Starting,
    Running,
    Stopping,
    Stopped,
};

struct StatusPanePlacementResult final {
    DWORD process_id = 0;
    DWORD thread_id = 0;
    HWND top_level = nullptr;
    HWND parent = nullptr;
    RECT rect{};
    bool visible = false;
};

class HookRuntimeRefreshIngress final {
public:
    void Enable() noexcept;
    void Disable() noexcept;
    [[nodiscard]] Status Signal(const std::function<Status()>& setEvent) noexcept;
    [[nodiscard]] Status SignalEvent(HANDLE event) noexcept;
    [[nodiscard]] bool Consume() noexcept;
    [[nodiscard]] bool enabled() const noexcept;

private:
    std::atomic<bool> enabled_{false};
    std::atomic<bool> pending_{false};
};

class StatusPaneRefreshCoordinator final {
public:
    explicit StatusPaneRefreshCoordinator(HWND pane = nullptr) noexcept;
    void Initialize(HWND pane) noexcept;
    [[nodiscard]] Status Signal(UINT message, const std::function<Status()>& wakeWorker);
    [[nodiscard]] Status Publish(
        const StatusPanePlacementResult& result,
        const StatusPanePostMessage& postMessage);
    [[nodiscard]] bool Consume(StatusPanePlacementResult* result);
    [[nodiscard]] Status CaptureFailed(const std::function<Status()>& wakeWorker);
    [[nodiscard]] Status ApplyCompleted(
        bool accepted,
        const std::function<Status()>& wakeWorker);
    void Stop() noexcept;
    [[nodiscard]] std::size_t pending_results() const noexcept;

private:
    mutable std::mutex mutex_;
    HWND pane_ = nullptr;
    bool stopped_ = false;
    bool worker_active_ = false;
    bool dirty_ = false;
    bool dispatch_pending_ = false;
    bool apply_pending_ = false;
    bool automatic_retry_used_ = false;
    std::optional<StatusPanePlacementResult> result_;
};

struct RuntimeSignalSourceState final {
    HWND parent = nullptr;
    bool lifecycle_failed = false;
    bool cleanup_blocked = false;
};

struct RuntimeParentDestroyContext final {
    bool active = false;
    DWORD process_id = 0;
    DWORD thread_id = 0;
    HWND target = nullptr;
    HWND pane = nullptr;
    HWND message_window = nullptr;
    RuntimeSignalSourceState* signal_source = nullptr;
};

struct RuntimeParentDestroyOperations final {
    std::function<DWORD(HWND, DWORD*)> get_window_thread_process_id;
    std::function<Status(HWND, std::wstring*)> get_class_name;
    std::function<HWND(HWND)> get_parent;
    std::function<HWND(HWND)> get_root;
    std::function<Status(HWND, HWND)> set_parent;
    std::function<Status(HWND, HWND, int, int, int, int, UINT)> set_window_pos;
};

[[nodiscard]] LRESULT ProcessRuntimeParentDestroy(
    const RuntimeParentDestroyContext& context,
    const RuntimeParentDestroyOperations& operations,
    const std::function<Status()>& signalRefresh,
    const std::function<LRESULT()>& callDefault);
[[nodiscard]] bool RuntimeSignalCleanupSafe(
    const RuntimeSignalSourceState& state) noexcept;

struct RuntimeSignalSubclassOperations final {
    std::function<Status(HWND)> remove;
    std::function<Status(HWND)> install;
};

struct HookRuntimeWindowMessageOperations final {
    std::function<DWORD(HWND, DWORD*)> get_window_thread_process_id;
    std::function<HWND(HWND)> get_root;
    std::function<Status(HWND, std::wstring*)> get_class_name;
    std::function<bool(HWND)> is_window_visible;
};

[[nodiscard]] bool ShouldNotifyHookRuntimeWindowMessage(
    HWND window,
    UINT message,
    HWND target,
    DWORD expectedProcess,
    DWORD expectedThread,
    const HookRuntimeWindowMessageOperations& operations);
void NotifyHookRuntimeWindowMessage(HWND window, UINT message) noexcept;
[[nodiscard]] Status SignalHookRuntimeRefresh() noexcept;
[[nodiscard]] Status SignalHookRuntimeRefresh(TabSubclassSet* owner) noexcept;

[[nodiscard]] Status UpdateRuntimeSignalParent(
    RuntimeSignalSourceState* state,
    HWND parent,
    const RuntimeSignalSubclassOperations& operations);

enum class RuntimeRollbackPath {
    CompareExchange,
    WorkerCreation,
};

[[nodiscard]] Status CleanupRuntimeRollback(
    RuntimeRollbackPath path,
    const StatusPaneOperations& operations,
    StatusPane* pane,
    const std::function<void()>& releaseModule);
[[nodiscard]] bool HandleStatusPaneRuntimeMessage(HWND pane, UINT message) noexcept;

[[nodiscard]] Status ApplyStatusPanePlacementResult(
    HWND pane,
    const StatusPanePlacementResult& result) noexcept;
[[nodiscard]] Status ApplyStatusPanePlacementResultWithOperations(
    HWND pane,
    const StatusPanePlacementResult& result,
    const StatusPanePlacementOperations& operations) noexcept;

class HookCallbackGate final {
public:
    [[nodiscard]] bool Enter() noexcept;
    void Leave() noexcept;
    void RejectNewWork() noexcept;
    void ResetForReuse() noexcept;
    [[nodiscard]] bool WaitForZero(DWORD timeoutMs) const noexcept;
    [[nodiscard]] std::uint32_t in_flight() const noexcept;

private:
    static constexpr std::uint64_t kClosed = std::uint64_t{1} << 63;
    static constexpr std::uint64_t kCountMask = ~kClosed;
    std::atomic<std::uint64_t> rundown_{0};
};

[[nodiscard]] HookCallbackGate& ProcessHookCallbackGate() noexcept;
[[nodiscard]] Status DrainHookCallbacksForUnload(
    HookCallbackGate& gate,
    DWORD timeoutMs) noexcept;

class HookRuntimeStateMachine final {
public:
    [[nodiscard]] Status BeginAttach() noexcept;
    [[nodiscard]] Status MarkRunning(bool attachValidated) noexcept;
    [[nodiscard]] Status BeginStop() noexcept;
    [[nodiscard]] Status MarkStopped() noexcept;
    [[nodiscard]] RuntimeState state() const noexcept;

private:
    RuntimeState state_ = RuntimeState::Stopped;
};

[[nodiscard]] Status BeginHookRuntimeAttach(
    HWND target,
    std::uint64_t attachId) noexcept;

}  // namespace winexinfo::hook
