#include "probe/observer_runtime_contract.h"

#include <chrono>
#include <limits>

namespace winexinfo {
namespace {

Status ContractFailure() noexcept {
    return {
        ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_INVALIDARG,
        ERROR_INVALID_PARAMETER,
    };
}

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace

Status ObserverFailureState::RecordRuntimeFailure(const ObserverFailure& failure) noexcept {
    const bool validOrigin =
        failure.origin == ObserverFailureOrigin::Contract ||
        failure.origin == ObserverFailureOrigin::Transport;
    const bool exactActiveFailure =
        failure.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (failure.status.hresult != S_OK || failure.status.win32 != ERROR_SUCCESS);
    if (!validOrigin || !exactActiveFailure) {
        return ContractFailure();
    }

    if (!has_runtime_failure_) {
        runtime_status_ = failure.status;
        has_runtime_failure_ = true;
    }
    if (failure.origin == ObserverFailureOrigin::Transport) {
        any_transport_failure_ = true;
    }

    return Success();
}

const Status& ObserverFailureState::runtime_status() const noexcept {
    return runtime_status_;
}

bool ObserverFailureState::has_runtime_failure() const noexcept {
    return has_runtime_failure_;
}

bool ObserverFailureState::any_transport_failure() const noexcept {
    return any_transport_failure_;
}

Status ObserverFailureState::ResolveHostExitCode(
    const ObserverCompletion& completion,
    HostExitCode* const output) const noexcept {
    if (output == nullptr) {
        return ContractFailure();
    }

    const bool exactCleanupSuccess =
        completion.cleanup_status.code == ErrorCode::OK &&
        completion.cleanup_status.hresult == S_OK &&
        completion.cleanup_status.win32 == ERROR_SUCCESS;
    const bool exactCleanupFailure =
        completion.cleanup_status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (completion.cleanup_status.hresult != S_OK ||
         completion.cleanup_status.win32 != ERROR_SUCCESS);
    const bool validCleanupOrigin =
        completion.cleanup_failure_origin.has_value() &&
        (*completion.cleanup_failure_origin == ObserverFailureOrigin::Contract ||
         *completion.cleanup_failure_origin == ObserverFailureOrigin::Transport);
    const bool coherentCleanup =
        (exactCleanupSuccess && !completion.cleanup_failure_origin.has_value() &&
         !completion.any_cleanup_transport_failure) ||
        (exactCleanupFailure && validCleanupOrigin &&
         (*completion.cleanup_failure_origin != ObserverFailureOrigin::Transport ||
          completion.any_cleanup_transport_failure));
    if (!coherentCleanup) {
        return ContractFailure();
    }

    HostExitCode result = HostExitCode::Pass;
    if (any_transport_failure_ || completion.any_cleanup_transport_failure) {
        result = HostExitCode::Win32ComFailure;
    } else if (
        has_runtime_failure_ || exactCleanupFailure || !completion.gate_passed) {
        result = HostExitCode::ContractFailure;
    }

    *output = result;
    return Success();
}

Status ObserverDeadline::Start(
    const Clock::time_point ready,
    const std::uint32_t durationMs) noexcept {
    if (started_ || durationMs < 1000 || durationMs > 60000) {
        return ContractFailure();
    }

    const Clock::duration duration =
        std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds{durationMs});
    if (duration <= Clock::duration::zero() ||
        ready.time_since_epoch() >
            Clock::time_point::max().time_since_epoch() - duration) {
        return ContractFailure();
    }

    const Clock::time_point deadline = ready + duration;
    ready_ = ready;
    deadline_ = deadline;
    started_ = true;
    return Success();
}

Status ObserverDeadline::RemainingWaitMilliseconds(
    const Clock::time_point now,
    std::uint32_t* const output) const noexcept {
    if (output == nullptr || !started_ || now < ready_) {
        return ContractFailure();
    }

    std::uint32_t result = 0;
    if (now < deadline_) {
        const auto rounded =
            std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now).count();
        if (rounded <= 0 ||
            rounded > static_cast<decltype(rounded)>(std::numeric_limits<std::uint32_t>::max())) {
            return ContractFailure();
        }
        result = static_cast<std::uint32_t>(rounded);
    }

    *output = result;
    return Success();
}

bool ObserverDeadline::started() const noexcept {
    return started_;
}

ObserverDeadline::Clock::time_point ObserverDeadline::ready_time() const noexcept {
    return ready_;
}

ObserverDeadline::Clock::time_point ObserverDeadline::deadline() const noexcept {
    return deadline_;
}

}  // namespace winexinfo
