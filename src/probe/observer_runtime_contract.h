#pragma once

#include "common/status.h"
#include "host/command_line.h"

#include <chrono>
#include <cstdint>
#include <optional>

namespace winexinfo {

enum class ObserverFailureOrigin {
    Contract,
    Transport,
};

struct ObserverFailure final {
    ObserverFailureOrigin origin;
    Status status;
};

struct ObserverCompletion final {
    Status cleanup_status;
    std::optional<ObserverFailureOrigin> cleanup_failure_origin;
    bool any_cleanup_transport_failure;
    bool gate_passed;
};

class ObserverFailureState final {
public:
    [[nodiscard]] Status RecordRuntimeFailure(const ObserverFailure& failure) noexcept;
    [[nodiscard]] const Status& runtime_status() const noexcept;
    [[nodiscard]] bool has_runtime_failure() const noexcept;
    [[nodiscard]] bool any_transport_failure() const noexcept;
    [[nodiscard]] Status ResolveHostExitCode(
        const ObserverCompletion& completion,
        HostExitCode* output) const noexcept;

private:
    Status runtime_status_{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    bool has_runtime_failure_ = false;
    bool any_transport_failure_ = false;
};

class ObserverDeadline final {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] Status Start(Clock::time_point ready, std::uint32_t duration_ms) noexcept;
    [[nodiscard]] Status RemainingWaitMilliseconds(
        Clock::time_point now,
        std::uint32_t* output) const noexcept;
    [[nodiscard]] bool started() const noexcept;
    [[nodiscard]] Clock::time_point ready_time() const noexcept;
    [[nodiscard]] Clock::time_point deadline() const noexcept;

private:
    bool started_ = false;
    Clock::time_point ready_{};
    Clock::time_point deadline_{};
};

}  // namespace winexinfo
