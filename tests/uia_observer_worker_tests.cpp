#include "test_framework.h"

#include "probe/uia_observer_worker.h"

#include <UIAutomationCoreApi.h>
#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <new>
#include <string>
#include <vector>

namespace {

winexinfo::ObserverOperationResult SuccessOperation() {
    return {
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
        std::nullopt,
    };
}

}  // namespace

WXI_TEST(
    uia_observer_worker_ignores_only_stale_callback_statuses,
    "uia_observer_worker.stale_callback_status") {
    WXI_REQUIRE(winexinfo::IsIgnorableObserverUiaCallbackStatus({
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
        ERROR_SUCCESS,
    }));
    WXI_REQUIRE(winexinfo::IsIgnorableObserverUiaCallbackStatus({
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        S_FALSE,
        ERROR_SUCCESS,
    }));
    WXI_REQUIRE(!winexinfo::IsIgnorableObserverUiaCallbackStatus({
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        RPC_E_DISCONNECTED,
        ERROR_SUCCESS,
    }));
    WXI_REQUIRE(!winexinfo::IsIgnorableObserverUiaCallbackStatus({
        winexinfo::ErrorCode::OK,
        S_OK,
        ERROR_SUCCESS,
    }));
}

WXI_TEST(
    uia_observer_worker_preserves_fifo_signal_and_cleanup,
    "uia_observer_worker.fifo_cleanup") {
    HANDLE responseEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WXI_REQUIRE(responseEvent != nullptr);
    std::vector<std::string> calls;
    const winexinfo::ObserverUiaWorkerOperations operations{
        [&calls]() {
            calls.push_back("initialize");
            return SuccessOperation();
        },
        [&calls](const winexinfo::ObserverUiaCommand& command) {
            calls.push_back(
                command.kind == winexinfo::ObserverUiaCommandKind::AddTarget
                    ? "add"
                    : "reenumerate");
            return winexinfo::ObserverUiaResponse{
                command.id,
                command.kind,
                command.target,
                SuccessOperation(),
                command.kind == winexinfo::ObserverUiaCommandKind::Reenumerate
                    ? std::size_t{2}
                    : std::size_t{0},
            };
        },
        [&calls]() {
            calls.push_back("cleanup");
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        [&calls]() { calls.push_back("uninitialize"); },
        [](HANDLE event) { return SetEvent(event); },
        [](HANDLE event) { return ResetEvent(event); },
        []() { return GetLastError(); },
        [](std::deque<winexinfo::ObserverUiaResponse>* responses,
           winexinfo::ObserverUiaResponse&& response) {
            responses->push_back(std::move(response));
        },
    };
    winexinfo::ObserverUiaMtaWorker worker(operations);
    WXI_REQUIRE(worker.Start(responseEvent).ok());
    const winexinfo::ObserverUiaTarget target{
        reinterpret_cast<HWND>(std::uintptr_t{0x100}),
        1,
        reinterpret_cast<HWND>(std::uintptr_t{0x101}),
        reinterpret_cast<HWND>(std::uintptr_t{0x102}),
    };
    std::uint64_t addId = 0;
    std::uint64_t reenumerateId = 0;
    WXI_REQUIRE(worker.Submit(
                    winexinfo::ObserverUiaCommandKind::AddTarget,
                    target,
                    &addId)
                    .ok());
    WXI_REQUIRE(worker.Submit(
                    winexinfo::ObserverUiaCommandKind::Reenumerate,
                    target,
                    &reenumerateId)
                    .ok());
    WXI_REQUIRE_EQ(addId, std::uint64_t{1});
    WXI_REQUIRE_EQ(reenumerateId, std::uint64_t{2});
    WXI_REQUIRE_EQ(WaitForSingleObject(responseEvent, 5000), DWORD{WAIT_OBJECT_0});

    winexinfo::ObserverUiaResponse response{};
    WXI_REQUIRE(worker.Consume(addId, &response).ok());
    WXI_REQUIRE_EQ(response.id, addId);
    WXI_REQUIRE_EQ(
        response.kind, winexinfo::ObserverUiaCommandKind::AddTarget);
    WXI_REQUIRE_EQ(WaitForSingleObject(responseEvent, 0), DWORD{WAIT_OBJECT_0});
    WXI_REQUIRE(worker.Consume(reenumerateId, &response).ok());
    WXI_REQUIRE_EQ(response.direct_child_count, std::size_t{2});
    WXI_REQUIRE_EQ(WaitForSingleObject(responseEvent, 0), DWORD{WAIT_TIMEOUT});

    WXI_REQUIRE(worker.BeginStopping().ok());
    const winexinfo::ObserverCleanupOutcome cleanup = worker.Join();
    WXI_REQUIRE(cleanup.status.ok());
    const std::vector<std::string> expected{
        "initialize", "add", "reenumerate", "cleanup", "uninitialize"};
    WXI_REQUIRE_EQ(calls, expected);
    CloseHandle(responseEvent);
}

WXI_TEST(
    uia_observer_worker_reports_response_allocation_failure_without_timeout,
    "uia_observer_worker.response_allocation_failure") {
    HANDLE responseEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WXI_REQUIRE(responseEvent != nullptr);
    winexinfo::ObserverUiaWorkerOperations operations{
        []() { return SuccessOperation(); },
        [](const winexinfo::ObserverUiaCommand& command) {
            return winexinfo::ObserverUiaResponse{
                command.id,
                command.kind,
                command.target,
                SuccessOperation(),
                0,
            };
        },
        []() {
            return winexinfo::ObserverCleanupOutcome{
                {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
                std::nullopt,
                false,
            };
        },
        []() {},
        [](HANDLE event) { return SetEvent(event); },
        [](HANDLE event) { return ResetEvent(event); },
        []() { return GetLastError(); },
    };
    operations.append_response = [](
        std::deque<winexinfo::ObserverUiaResponse>*,
        winexinfo::ObserverUiaResponse&&) {
        throw std::bad_alloc{};
    };
    winexinfo::ObserverUiaMtaWorker worker(std::move(operations));
    WXI_REQUIRE(worker.Start(responseEvent).ok());
    const winexinfo::ObserverUiaTarget target{
        reinterpret_cast<HWND>(std::uintptr_t{0x100}),
        1,
        reinterpret_cast<HWND>(std::uintptr_t{0x101}),
        reinterpret_cast<HWND>(std::uintptr_t{0x102}),
    };
    std::uint64_t commandId = 0;
    WXI_REQUIRE(worker.Submit(
                    winexinfo::ObserverUiaCommandKind::AddTarget,
                    target,
                    &commandId)
                    .ok());
    WXI_REQUIRE_EQ(
        WaitForSingleObject(responseEvent, 5000), DWORD{WAIT_OBJECT_0});
    winexinfo::ObserverUiaResponse response{};
    WXI_REQUIRE(worker.Consume(commandId, &response).ok());
    WXI_REQUIRE_EQ(
        response.operation.status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(response.operation.status.hresult, E_OUTOFMEMORY);
    WXI_REQUIRE_EQ(
        response.operation.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Transport});
    const winexinfo::ObserverCleanupOutcome cleanup = worker.Join();
    WXI_REQUIRE(cleanup.status.ok());
    CloseHandle(responseEvent);
}

WXI_TEST(
    uia_observer_worker_partial_registration_retains_removal_identity,
    "uia_observer_worker.partial_registration_removal_identity") {
    const std::uintptr_t tabListIdentity = 0x100;
    const std::uintptr_t selectionIdentity = 0x200;
    winexinfo::ObserverUiaHandlerRemovalState state{
        tabListIdentity,
        selectionIdentity,
        0,
        true,
        false,
    };
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> removals;
    bool failSelection = true;
    const winexinfo::ObserverUiaHandlerRemovalOperations operations{
        [&removals, &failSelection](
            const std::uintptr_t owner,
            const std::uintptr_t handler) {
            removals.emplace_back(owner, handler);
            if (failSelection) {
                return winexinfo::ObserverOperationResult{
                    {
                        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                        RPC_E_DISCONNECTED,
                        ERROR_SUCCESS,
                    },
                    winexinfo::ObserverFailureOrigin::Transport,
                };
            }
            return SuccessOperation();
        },
        {},
    };

    winexinfo::ObserverCleanupOutcome cleanup =
        winexinfo::RemoveObserverUiaHandlerRegistrations(
            operations, &state);
    WXI_REQUIRE_EQ(cleanup.status.hresult, RPC_E_DISCONNECTED);
    WXI_REQUIRE_EQ(
        cleanup.failure_origin,
        std::optional{winexinfo::ObserverFailureOrigin::Transport});
    WXI_REQUIRE(cleanup.any_transport_failure);
    WXI_REQUIRE(state.selection_registered);
    WXI_REQUIRE_EQ(
        removals,
        (std::vector<std::pair<std::uintptr_t, std::uintptr_t>>{
            {tabListIdentity, selectionIdentity}}));

    failSelection = false;
    cleanup = winexinfo::RemoveObserverUiaHandlerRegistrations(
        operations, &state);
    WXI_REQUIRE(cleanup.status.ok());
    WXI_REQUIRE(!state.selection_registered);
    WXI_REQUIRE_EQ(removals.back().first, tabListIdentity);
    WXI_REQUIRE_EQ(removals.back().second, selectionIdentity);
}
