#pragma once

#include "common/status.h"
#include "hook/status_pane.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>

namespace winexinfo::hook {

enum class RuntimeState {
    Starting,
    Running,
    Stopping,
    Stopped,
};

struct StatusPanePlacementResult final {
    DWORD process_id = 0;
    DWORD thread_id = 0;
    HWND parent = nullptr;
    RECT rect{};
    bool visible = false;
};

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
    [[nodiscard]] Status MarkRunning(bool hookReleased) noexcept;
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
