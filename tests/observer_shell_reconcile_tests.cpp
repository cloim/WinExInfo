#include "test_framework.h"

#include "probe/observer_runtime.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

winexinfo::ObserverTabIdentity Identity(
    const std::uintptr_t canonicalIdentity,
    const std::uintptr_t shellTab,
    const std::uint64_t tabGeneration = 1) {
    return {
        canonicalIdentity,
        Handle(0x100),
        Handle(shellTab),
        1,
        tabGeneration,
    };
}

winexinfo::ObserverOperationResult Success() {
    return {{winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS}, std::nullopt};
}

winexinfo::ObserverOperationResult ContractFailure(
    const HRESULT hresult = S_FALSE) {
    return {
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            hresult,
            ERROR_SUCCESS,
        },
        winexinfo::ObserverFailureOrigin::Contract,
    };
}

winexinfo::ObserverOperationResult TransportFailure(
    const HRESULT hresult = RPC_E_DISCONNECTED) {
    return {
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            hresult,
            ERROR_SUCCESS,
        },
        winexinfo::ObserverFailureOrigin::Transport,
    };
}

winexinfo::ObserverShellSubscriptionState State(
    std::initializer_list<std::pair<winexinfo::ObserverTabIdentity, std::uint64_t>>
        entries,
    const std::uint64_t nextId) {
    winexinfo::ObserverShellSubscriptionState state{};
    for (const auto& [identity, id] : entries) {
        state.subscriptions.push_back({identity, id});
    }
    state.next_registration_id = nextId;
    return state;
}

}  // namespace

WXI_TEST(
    observer_shell_reconcile_applies_batch_add_remove_atomically,
    "observer_shell_reconcile.batch_success") {
    auto state = State({{Identity(11, 0x101), 10}, {Identity(22, 0x102), 11}}, 12);
    const std::vector desired{
        Identity(22, 0x102),
        Identity(33, 0x103),
        Identity(44, 0x104),
    };
    std::vector<std::string> calls;
    const winexinfo::ObserverShellReconcileOperations operations{
        [&](const winexinfo::ObserverTabIdentity& identity, const std::uint64_t id) {
            calls.push_back(
                "add:" + std::to_string(identity.canonical_identity) + ":" +
                std::to_string(id));
            return Success();
        },
        [&](const std::uint64_t id) {
            calls.push_back("remove:" + std::to_string(id));
            return Success();
        },
    };
    winexinfo::ObserverShellReconcileOutcome outcome{};

    WXI_REQUIRE(winexinfo::ReconcileObserverShellSubscriptions(
                    desired, operations, &state, &outcome)
                    .ok());
    WXI_REQUIRE_EQ(
        calls,
        (std::vector<std::string>{
            "add:33:12", "add:44:13", "remove:10"}));
    WXI_REQUIRE_EQ(state.next_registration_id, std::uint64_t{14});
    WXI_REQUIRE_EQ(state.subscriptions.size(), std::size_t{3});
    WXI_REQUIRE_EQ(state.subscriptions[0].registration_id, std::uint64_t{11});
    WXI_REQUIRE_EQ(state.subscriptions[1].registration_id, std::uint64_t{12});
    WXI_REQUIRE_EQ(state.subscriptions[2].registration_id, std::uint64_t{13});
    WXI_REQUIRE(outcome.operation.ok());
    WXI_REQUIRE(outcome.rollback.status.ok());
}

WXI_TEST(
    observer_shell_reconcile_rolls_back_completed_additions,
    "observer_shell_reconcile.add_failure") {
    auto state = State({{Identity(11, 0x101), 10}}, 11);
    const auto original = state;
    const std::vector desired{
        Identity(11, 0x101),
        Identity(22, 0x102),
        Identity(33, 0x103),
    };
    std::vector<std::string> calls;
    const winexinfo::ObserverShellReconcileOperations operations{
        [&](const winexinfo::ObserverTabIdentity& identity, const std::uint64_t id) {
            static_cast<void>(id);
            calls.push_back("add:" + std::to_string(identity.canonical_identity));
            return identity.canonical_identity == 33 ? ContractFailure() : Success();
        },
        [&](const std::uint64_t id) {
            calls.push_back("remove:" + std::to_string(id));
            return Success();
        },
    };
    winexinfo::ObserverShellReconcileOutcome outcome{};

    WXI_REQUIRE(!winexinfo::ReconcileObserverShellSubscriptions(
                     desired, operations, &state, &outcome)
                     .ok());
    WXI_REQUIRE_EQ(
        calls,
        (std::vector<std::string>{"add:22", "add:33", "remove:11"}));
    WXI_REQUIRE_EQ(state.subscriptions, original.subscriptions);
    WXI_REQUIRE_EQ(state.next_registration_id, std::uint64_t{13});
    WXI_REQUIRE_EQ(outcome.operation.failure_origin, winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE(outcome.rollback.status.ok());
}

WXI_TEST(
    observer_shell_reconcile_restores_completed_removals,
    "observer_shell_reconcile.remove_failure") {
    auto state = State(
        {{Identity(11, 0x101), 10},
         {Identity(22, 0x102), 11},
         {Identity(33, 0x103), 12}},
        13);
    const auto original = state;
    const std::vector<winexinfo::ObserverTabIdentity> desired{};
    std::vector<std::string> calls;
    const winexinfo::ObserverShellReconcileOperations operations{
        [&](const winexinfo::ObserverTabIdentity& identity, const std::uint64_t id) {
            calls.push_back(
                "restore:" + std::to_string(identity.canonical_identity) + ":" +
                std::to_string(id));
            return Success();
        },
        [&](const std::uint64_t id) {
            calls.push_back("remove:" + std::to_string(id));
            return id == 11 ? TransportFailure() : Success();
        },
    };
    winexinfo::ObserverShellReconcileOutcome outcome{};

    WXI_REQUIRE(!winexinfo::ReconcileObserverShellSubscriptions(
                     desired, operations, &state, &outcome)
                     .ok());
    WXI_REQUIRE_EQ(
        calls,
        (std::vector<std::string>{"remove:10", "remove:11", "restore:11:10"}));
    WXI_REQUIRE_EQ(state, original);
    WXI_REQUIRE_EQ(outcome.operation.failure_origin, winexinfo::ObserverFailureOrigin::Transport);
    WXI_REQUIRE(outcome.rollback.status.ok());
}

WXI_TEST(
    observer_shell_reconcile_records_known_live_state_when_rollback_fails,
    "observer_shell_reconcile.rollback_failure") {
    auto state = State({{Identity(11, 0x101), 10}}, 11);
    const std::vector desired{Identity(22, 0x102), Identity(33, 0x103)};
    const winexinfo::ObserverShellReconcileOperations operations{
        [](const winexinfo::ObserverTabIdentity& identity, std::uint64_t) {
            if (identity.canonical_identity == 33) {
                return ContractFailure();
            }
            return Success();
        },
        [](const std::uint64_t id) {
            return id == 11 ? TransportFailure(E_OUTOFMEMORY) : Success();
        },
    };
    winexinfo::ObserverShellReconcileOutcome outcome{};

    WXI_REQUIRE(!winexinfo::ReconcileObserverShellSubscriptions(
                     desired, operations, &state, &outcome)
                     .ok());
    WXI_REQUIRE_EQ(state.subscriptions.size(), std::size_t{2});
    WXI_REQUIRE_EQ(state.subscriptions[0].registration_id, std::uint64_t{10});
    WXI_REQUIRE_EQ(state.subscriptions[1].registration_id, std::uint64_t{11});
    WXI_REQUIRE_EQ(outcome.operation.failure_origin, winexinfo::ObserverFailureOrigin::Contract);
    WXI_REQUIRE_EQ(outcome.rollback.failure_origin, winexinfo::ObserverFailureOrigin::Transport);
    WXI_REQUIRE(outcome.rollback.any_transport_failure);
    WXI_REQUIRE_EQ(outcome.rollback.status.hresult, E_OUTOFMEMORY);
}
