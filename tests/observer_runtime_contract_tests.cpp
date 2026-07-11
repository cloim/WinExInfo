#include "test_framework.h"

#include "probe/observer_runtime_contract.h"

#include <chrono>
#include <cstdint>

namespace {

constexpr winexinfo::Status OkStatus() noexcept {
    return {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

constexpr winexinfo::Status ActiveFailure(
    const HRESULT hresult,
    const DWORD win32 = ERROR_SUCCESS) noexcept {
    return {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, win32};
}

winexinfo::ObserverCompletion SuccessfulCompletion(const bool gatePassed = true) {
    return {OkStatus(), std::nullopt, false, gatePassed};
}

}  // namespace

WXI_TEST(
    observer_runtime_contract_contract_failure_selects_exit_one,
    "observer_runtime.contract_failure_selects_exit_one") {
    winexinfo::ObserverFailureState state;
    const winexinfo::Status first = ActiveFailure(DISP_E_TYPEMISMATCH);

    WXI_REQUIRE(state.RecordRuntimeFailure({
        winexinfo::ObserverFailureOrigin::Contract,
        first,
    }).ok());

    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::Win32ComFailure;
    WXI_REQUIRE(state.ResolveHostExitCode(SuccessfulCompletion(), &exitCode).ok());
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::ContractFailure);
    WXI_REQUIRE(state.has_runtime_failure());
    WXI_REQUIRE(!state.any_transport_failure());
    WXI_REQUIRE_EQ(state.runtime_status().code, first.code);
    WXI_REQUIRE_EQ(state.runtime_status().hresult, first.hresult);
    WXI_REQUIRE_EQ(state.runtime_status().win32, first.win32);
}

WXI_TEST(
    observer_runtime_contract_later_transport_is_sticky_without_replacing_first_tuple,
    "observer_runtime.later_transport_is_sticky_without_replacing_first_tuple") {
    winexinfo::ObserverFailureState state;
    const winexinfo::Status first = ActiveFailure(DISP_E_TYPEMISMATCH);

    WXI_REQUIRE(state.RecordRuntimeFailure({
        winexinfo::ObserverFailureOrigin::Contract,
        first,
    }).ok());
    WXI_REQUIRE(state.RecordRuntimeFailure({
        winexinfo::ObserverFailureOrigin::Transport,
        ActiveFailure(E_OUTOFMEMORY),
    }).ok());

    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::Pass;
    WXI_REQUIRE(state.ResolveHostExitCode(SuccessfulCompletion(), &exitCode).ok());
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::Win32ComFailure);
    WXI_REQUIRE(state.any_transport_failure());
    WXI_REQUIRE_EQ(state.runtime_status().code, first.code);
    WXI_REQUIRE_EQ(state.runtime_status().hresult, first.hresult);
    WXI_REQUIRE_EQ(state.runtime_status().win32, first.win32);
}

WXI_TEST(
    observer_runtime_contract_cleanup_transport_selects_exit_three,
    "observer_runtime.cleanup_transport_selects_exit_three") {
    const winexinfo::ObserverFailureState state;
    const winexinfo::ObserverCompletion completion{
        ActiveFailure(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), ERROR_ACCESS_DENIED),
        winexinfo::ObserverFailureOrigin::Transport,
        true,
        true,
    };

    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::Pass;
    WXI_REQUIRE(state.ResolveHostExitCode(completion, &exitCode).ok());
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::Win32ComFailure);
}

WXI_TEST(
    observer_runtime_contract_cleanup_contract_selects_exit_one,
    "observer_runtime.cleanup_contract_selects_exit_one") {
    const winexinfo::ObserverFailureState state;
    const winexinfo::ObserverCompletion completion{
        ActiveFailure(DISP_E_TYPEMISMATCH),
        winexinfo::ObserverFailureOrigin::Contract,
        false,
        false,
    };

    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::Pass;
    WXI_REQUIRE(state.ResolveHostExitCode(completion, &exitCode).ok());
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::ContractFailure);
}

WXI_TEST(
    observer_runtime_contract_later_cleanup_transport_is_sticky,
    "observer_runtime.later_cleanup_transport_is_sticky") {
    const winexinfo::ObserverFailureState state;
    const winexinfo::Status firstCleanup = ActiveFailure(DISP_E_TYPEMISMATCH);
    const winexinfo::ObserverCompletion completion{
        firstCleanup,
        winexinfo::ObserverFailureOrigin::Contract,
        true,
        false,
    };

    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::Pass;
    WXI_REQUIRE(state.ResolveHostExitCode(completion, &exitCode).ok());
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::Win32ComFailure);
    WXI_REQUIRE_EQ(completion.cleanup_status.code, firstCleanup.code);
    WXI_REQUIRE_EQ(completion.cleanup_status.hresult, firstCleanup.hresult);
    WXI_REQUIRE_EQ(completion.cleanup_status.win32, firstCleanup.win32);
    WXI_REQUIRE_EQ(
        completion.cleanup_failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Contract});
}

WXI_TEST(
    observer_runtime_contract_success_selects_exit_zero,
    "observer_runtime.success_selects_exit_zero") {
    const winexinfo::ObserverFailureState state;
    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::ContractFailure;

    WXI_REQUIRE(state.ResolveHostExitCode(SuccessfulCompletion(), &exitCode).ok());
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::Pass);
}

WXI_TEST(
    observer_runtime_contract_gate_failure_selects_exit_one,
    "observer_runtime.gate_failure_selects_exit_one") {
    const winexinfo::ObserverFailureState state;
    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::Pass;

    WXI_REQUIRE(state.ResolveHostExitCode(SuccessfulCompletion(false), &exitCode).ok());
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::ContractFailure);
}

WXI_TEST(
    observer_runtime_contract_origin_is_not_inferred_from_hresult,
    "observer_runtime.origin_is_not_inferred_from_hresult") {
    winexinfo::ObserverFailureState contractState;
    WXI_REQUIRE(contractState.RecordRuntimeFailure({
        winexinfo::ObserverFailureOrigin::Contract,
        ActiveFailure(E_OUTOFMEMORY),
    }).ok());

    winexinfo::HostExitCode contractExit = winexinfo::HostExitCode::Pass;
    WXI_REQUIRE(
        contractState.ResolveHostExitCode(SuccessfulCompletion(), &contractExit).ok());
    WXI_REQUIRE_EQ(contractExit, winexinfo::HostExitCode::ContractFailure);

    winexinfo::ObserverFailureState transportState;
    WXI_REQUIRE(transportState.RecordRuntimeFailure({
        winexinfo::ObserverFailureOrigin::Transport,
        ActiveFailure(DISP_E_TYPEMISMATCH),
    }).ok());

    winexinfo::HostExitCode transportExit = winexinfo::HostExitCode::Pass;
    WXI_REQUIRE(
        transportState.ResolveHostExitCode(SuccessfulCompletion(), &transportExit).ok());
    WXI_REQUIRE_EQ(transportExit, winexinfo::HostExitCode::Win32ComFailure);
}

WXI_TEST(
    observer_runtime_contract_rejects_non_active_runtime_status_transactionally,
    "observer_runtime.rejects_non_active_runtime_status_transactionally") {
    winexinfo::ObserverFailureState state;
    const winexinfo::Status result = state.RecordRuntimeFailure({
        winexinfo::ObserverFailureOrigin::Transport,
        {winexinfo::ErrorCode::PIPE_DISCONNECTED, E_FAIL, ERROR_BROKEN_PIPE},
    });

    WXI_REQUIRE_EQ(
        result.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE(!state.has_runtime_failure());
    WXI_REQUIRE(!state.any_transport_failure());
    WXI_REQUIRE(state.runtime_status().ok());
}

WXI_TEST(
    observer_runtime_contract_rejects_incoherent_completion_transactionally,
    "observer_runtime.rejects_incoherent_completion_transactionally") {
    const winexinfo::ObserverFailureState state;
    const winexinfo::ObserverCompletion completion{
        OkStatus(),
        winexinfo::ObserverFailureOrigin::Transport,
        true,
        true,
    };
    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::InvalidCli;

    const winexinfo::Status result = state.ResolveHostExitCode(completion, &exitCode);
    WXI_REQUIRE_EQ(
        result.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::InvalidCli);
}

WXI_TEST(
    observer_runtime_contract_rejects_success_with_cleanup_transport_sticky,
    "observer_runtime.rejects_success_with_cleanup_transport_sticky") {
    const winexinfo::ObserverFailureState state;
    const winexinfo::ObserverCompletion completion{
        OkStatus(),
        std::nullopt,
        true,
        true,
    };
    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::InvalidCli;

    const winexinfo::Status result = state.ResolveHostExitCode(completion, &exitCode);
    WXI_REQUIRE_EQ(
        result.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::InvalidCli);
}

WXI_TEST(
    observer_runtime_contract_rejects_non_sticky_first_cleanup_transport,
    "observer_runtime.rejects_non_sticky_first_cleanup_transport") {
    const winexinfo::ObserverFailureState state;
    const winexinfo::ObserverCompletion completion{
        ActiveFailure(E_OUTOFMEMORY),
        winexinfo::ObserverFailureOrigin::Transport,
        false,
        false,
    };
    winexinfo::HostExitCode exitCode = winexinfo::HostExitCode::InvalidCli;

    const winexinfo::Status result = state.ResolveHostExitCode(completion, &exitCode);
    WXI_REQUIRE_EQ(
        result.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(exitCode, winexinfo::HostExitCode::InvalidCli);
}

WXI_TEST(
    observer_runtime_deadline_accepts_inclusive_duration_bounds,
    "observer_runtime.deadline_accepts_inclusive_duration_bounds") {
    using namespace std::chrono_literals;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{123456ms};
    winexinfo::ObserverDeadline minimum;
    winexinfo::ObserverDeadline maximum;

    WXI_REQUIRE(minimum.Start(ready, 1000).ok());
    WXI_REQUIRE(maximum.Start(ready, 60000).ok());
    WXI_REQUIRE_EQ(minimum.ready_time(), ready);
    WXI_REQUIRE_EQ(minimum.deadline(), ready + 1000ms);
    WXI_REQUIRE_EQ(maximum.deadline(), ready + 60000ms);
}

WXI_TEST(
    observer_runtime_deadline_is_absolute_and_ceil_rounds_remaining_wait,
    "observer_runtime.deadline_is_absolute_and_ceil_rounds_remaining_wait") {
    using namespace std::chrono_literals;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    winexinfo::ObserverDeadline deadline;
    WXI_REQUIRE(deadline.Start(ready, 1000).ok());
    const auto absolute = deadline.deadline();

    std::uint32_t remaining = 77;
    WXI_REQUIRE(deadline.RemainingWaitMilliseconds(ready + 1us, &remaining).ok());
    WXI_REQUIRE_EQ(remaining, std::uint32_t{1000});
    WXI_REQUIRE_EQ(deadline.deadline(), absolute);

    WXI_REQUIRE(deadline.RemainingWaitMilliseconds(ready + 999001us, &remaining).ok());
    WXI_REQUIRE_EQ(remaining, std::uint32_t{1});
    WXI_REQUIRE_EQ(deadline.deadline(), absolute);

    WXI_REQUIRE(deadline.RemainingWaitMilliseconds(ready + 1000ms, &remaining).ok());
    WXI_REQUIRE_EQ(remaining, std::uint32_t{0});
    WXI_REQUIRE_EQ(deadline.deadline(), absolute);
}

WXI_TEST(
    observer_runtime_deadline_is_one_shot_transactionally,
    "observer_runtime.deadline_is_one_shot_transactionally") {
    using namespace std::chrono_literals;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{20000ms};
    winexinfo::ObserverDeadline deadline;
    WXI_REQUIRE(deadline.Start(ready, 1000).ok());
    const auto absolute = deadline.deadline();

    const winexinfo::Status result = deadline.Start(ready + 500ms, 2000);
    WXI_REQUIRE_EQ(
        result.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(deadline.ready_time(), ready);
    WXI_REQUIRE_EQ(deadline.deadline(), absolute);
}

WXI_TEST(
    observer_runtime_deadline_rejects_invalid_duration_transactionally,
    "observer_runtime.deadline_rejects_invalid_duration_transactionally") {
    winexinfo::ObserverDeadline deadline;

    WXI_REQUIRE_EQ(
        deadline.Start({}, 999).code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE(!deadline.started());
    WXI_REQUIRE_EQ(
        deadline.Start({}, 60001).code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE(!deadline.started());
}

WXI_TEST(
    observer_runtime_deadline_rejects_overflow_transactionally,
    "observer_runtime.deadline_rejects_overflow_transactionally") {
    using namespace std::chrono_literals;
    winexinfo::ObserverDeadline deadline;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point::max() - 500ms;

    const winexinfo::Status result = deadline.Start(ready, 1000);
    WXI_REQUIRE_EQ(
        result.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE(!deadline.started());
}

WXI_TEST(
    observer_runtime_deadline_rejects_clock_regression_without_touching_output,
    "observer_runtime.deadline_rejects_clock_regression_without_touching_output") {
    using namespace std::chrono_literals;
    const auto ready = winexinfo::ObserverDeadline::Clock::time_point{10000ms};
    winexinfo::ObserverDeadline deadline;
    WXI_REQUIRE(deadline.Start(ready, 1000).ok());
    std::uint32_t remaining = 54321;

    const winexinfo::Status result =
        deadline.RemainingWaitMilliseconds(ready - 1ms, &remaining);
    WXI_REQUIRE_EQ(
        result.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(remaining, std::uint32_t{54321});
}
