#pragma once

#include "common/status.h"

#include <Windows.h>

#include <cstdint>

namespace winexinfo::hook {

enum class RuntimeState {
    Starting,
    Running,
    Stopping,
    Stopped,
};

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
