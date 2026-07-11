#include "test_framework.h"

#include "probe/tab_identity.h"

#include <array>
#include <cstdint>
#include <map>
#include <vector>

namespace {

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

winexinfo::ObserverShellEntryMetadata Target(
    const std::uintptr_t identity,
    const std::uintptr_t topLevel,
    const std::uintptr_t shellTab) {
    return {identity, true, Handle(topLevel), Handle(shellTab)};
}

winexinfo::ObserverTopLevelTabOrder Order(
    const std::uintptr_t topLevel,
    const std::initializer_list<winexinfo::ObserverOrderedTab> tabs) {
    return {Handle(topLevel), std::vector<winexinfo::ObserverOrderedTab>{tabs}};
}

winexinfo::ObserverTabIdentity Identity(
    const std::uintptr_t canonicalIdentity,
    const std::uintptr_t topLevel,
    const std::uintptr_t shellTab,
    const std::uint64_t topLevelGeneration,
    const std::uint64_t tabGeneration) {
    return {
        canonicalIdentity,
        Handle(topLevel),
        Handle(shellTab),
        topLevelGeneration,
        tabGeneration,
    };
}

winexinfo::ObserverTabGenerationState Generations(
    const std::initializer_list<std::pair<const HWND, std::uint64_t>> topLevels,
    const std::initializer_list<std::pair<const HWND, std::uint64_t>> tabs) {
    return {
        std::map<HWND, std::uint64_t>{topLevels},
        std::map<HWND, std::uint64_t>{tabs},
    };
}

void RequireContractFailure(const winexinfo::Status& status) {
    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE(!status.ok());
}

}  // namespace

WXI_TEST(
    tab_identity_reconciles_complete_set_and_preserves_generations,
    "tab_identity.complete_set") {
    const std::array previous{
        Identity(11, 0x100, 0x101, 4, 7),
        Identity(22, 0x100, 0x102, 4, 3),
    };
    const std::array current{
        Target(22, 0x100, 0x102),
        Target(33, 0x100, 0x103),
    };
    const std::array orders{
        Order(0x100, {{Handle(0x102), true}, {Handle(0x103), false}}),
    };
    const auto generations = Generations(
        {{Handle(0x100), 4}},
        {{Handle(0x101), 7}, {Handle(0x102), 3}});

    winexinfo::ObserverTabSetReconciliation output{};
    WXI_REQUIRE(winexinfo::ReconcileObserverTabSet(
                    previous, current, orders, generations, &output)
                    .ok());
    WXI_REQUIRE_EQ(output.current.size(), std::size_t{2});
    WXI_REQUIRE_EQ(output.retained.size(), std::size_t{1});
    WXI_REQUIRE_EQ(output.added.size(), std::size_t{1});
    WXI_REQUIRE_EQ(output.removed.size(), std::size_t{1});
    WXI_REQUIRE_EQ(output.current[0], Identity(22, 0x100, 0x102, 4, 3));
    WXI_REQUIRE_EQ(output.current[1], Identity(33, 0x100, 0x103, 4, 1));
    WXI_REQUIRE_EQ(output.active_shell_tabs.at(Handle(0x100)), Handle(0x102));
    WXI_REQUIRE_EQ(
        output.generations.latest_shell_tabs.at(Handle(0x101)),
        std::uint64_t{7});
    WXI_REQUIRE_EQ(
        output.generations.latest_shell_tabs.at(Handle(0x103)),
        std::uint64_t{1});
}

WXI_TEST(
    tab_identity_treats_reused_hwnd_as_remove_and_add,
    "tab_identity.handle_reuse") {
    const std::array previous{Identity(11, 0x100, 0x101, 2, 9)};
    const std::array current{Target(12, 0x100, 0x101)};
    const std::array orders{Order(0x100, {{Handle(0x101), true}})};
    const auto generations = Generations(
        {{Handle(0x100), 2}},
        {{Handle(0x101), 9}});

    winexinfo::ObserverTabSetReconciliation output{};
    WXI_REQUIRE(winexinfo::ReconcileObserverTabSet(
                    previous, current, orders, generations, &output)
                    .ok());
    WXI_REQUIRE_EQ(output.retained.size(), std::size_t{0});
    WXI_REQUIRE_EQ(output.added, std::vector{Identity(12, 0x100, 0x101, 2, 10)});
    WXI_REQUIRE_EQ(output.removed, std::vector{previous[0]});
}

WXI_TEST(
    tab_identity_assigns_next_generation_to_reused_top_level,
    "tab_identity.top_level_reuse") {
    const std::array<winexinfo::ObserverTabIdentity, 0> previous{};
    const std::array current{Target(11, 0x100, 0x101)};
    const std::array orders{Order(0x100, {{Handle(0x101), true}})};
    const auto generations = Generations(
        {{Handle(0x100), 5}},
        {{Handle(0x101), 8}});

    winexinfo::ObserverTabSetReconciliation output{};
    WXI_REQUIRE(winexinfo::ReconcileObserverTabSet(
                    previous, current, orders, generations, &output)
                    .ok());
    WXI_REQUIRE_EQ(output.current, std::vector{Identity(11, 0x100, 0x101, 6, 9)});
}

WXI_TEST(
    tab_identity_accepts_non_target_entries_without_joining_them,
    "tab_identity.non_target") {
    const std::array<winexinfo::ObserverTabIdentity, 0> previous{};
    const std::array current{
        Target(11, 0x100, 0x101),
        winexinfo::ObserverShellEntryMetadata{22, false, Handle(0x900), nullptr},
    };
    const std::array orders{Order(0x100, {{Handle(0x101), true}})};

    winexinfo::ObserverTabSetReconciliation output{};
    WXI_REQUIRE(winexinfo::ReconcileObserverTabSet(
                    previous, current, orders, {}, &output)
                    .ok());
    WXI_REQUIRE_EQ(output.current.size(), std::size_t{1});
    WXI_REQUIRE_EQ(output.current[0], Identity(11, 0x100, 0x101, 1, 1));
}

WXI_TEST(
    tab_identity_ignores_stale_shell_metadata_after_hwnd_removal,
    "tab_identity.stale_removed_shell_metadata") {
    const std::array previous{
        Identity(11, 0x100, 0x101, 1, 1),
        Identity(22, 0x100, 0x102, 1, 1),
    };
    const std::array current{
        Target(11, 0x100, 0x101),
        Target(22, 0x100, 0x102),
    };
    const std::array orders{
        Order(0x100, {{Handle(0x101), true}}),
    };
    const auto generations = Generations(
        {{Handle(0x100), 1}},
        {{Handle(0x101), 1}, {Handle(0x102), 1}});

    winexinfo::ObserverTabSetReconciliation output{};
    WXI_REQUIRE(winexinfo::ReconcileObserverTabSet(
                    previous, current, orders, generations, &output)
                    .ok());
    WXI_REQUIRE_EQ(
        output.current,
        std::vector{Identity(11, 0x100, 0x101, 1, 1)});
    WXI_REQUIRE_EQ(
        output.removed,
        std::vector{Identity(22, 0x100, 0x102, 1, 1)});
}

WXI_TEST(
    tab_identity_rejects_incomplete_or_duplicate_bijections,
    "tab_identity.bijection_failures") {
    const std::array<winexinfo::ObserverTabIdentity, 0> previous{};
    const std::array current{
        Target(11, 0x100, 0x101),
        Target(22, 0x100, 0x102),
    };
    const std::array mismatchedTopLevel{
        Order(0x200, {{Handle(0x101), true}}),
    };
    const std::array extraOrder{Order(
        0x100,
        {{Handle(0x101), true}, {Handle(0x102), false}, {Handle(0x103), false}})};
    const std::array duplicateOrder{Order(
        0x100,
        {{Handle(0x101), true}, {Handle(0x101), false}})};
    winexinfo::ObserverTabSetReconciliation sentinel{};
    sentinel.current.push_back(Identity(99, 0x900, 0x901, 1, 1));
    auto output = sentinel;

    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, current, mismatchedTopLevel, {}, &output));
    WXI_REQUIRE_EQ(output, sentinel);
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, current, extraOrder, {}, &output));
    WXI_REQUIRE_EQ(output, sentinel);
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, current, duplicateOrder, {}, &output));
    WXI_REQUIRE_EQ(output, sentinel);
}

WXI_TEST(
    tab_identity_rejects_invalid_identity_and_visibility_shapes,
    "tab_identity.shape_failures") {
    const std::array<winexinfo::ObserverTabIdentity, 0> previous{};
    const std::array valid{Target(11, 0x100, 0x101)};
    const std::array validOrder{Order(0x100, {{Handle(0x101), true}})};
    winexinfo::ObserverTabSetReconciliation output{};

    auto zeroIdentity = valid;
    zeroIdentity[0].canonical_identity = 0;
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, zeroIdentity, validOrder, {}, &output));

    const std::array duplicateIdentity{
        Target(11, 0x100, 0x101),
        Target(11, 0x100, 0x102),
    };
    const std::array twoTabs{Order(
        0x100,
        {{Handle(0x101), true}, {Handle(0x102), false}})};
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, duplicateIdentity, twoTabs, {}, &output));

    const std::array duplicateHwnd{
        Target(11, 0x100, 0x101),
        Target(22, 0x100, 0x101),
    };
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, duplicateHwnd, twoTabs, {}, &output));

    const std::array noVisible{Order(0x100, {{Handle(0x101), false}})};
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, valid, noVisible, {}, &output));

}

WXI_TEST(
    tab_identity_selects_first_visible_tab_when_multiple_hwnds_are_visible,
    "tab_identity.first_visible") {
    const std::array<winexinfo::ObserverTabIdentity, 0> previous{};
    const std::array current{
        Target(11, 0x100, 0x101),
        Target(22, 0x100, 0x102),
    };
    const std::array orders{Order(
        0x100,
        {{Handle(0x101), true}, {Handle(0x102), true}})};
    winexinfo::ObserverTabSetReconciliation output{};

    WXI_REQUIRE(winexinfo::ReconcileObserverTabSet(
                    previous, current, orders, {}, &output)
                    .ok());
    WXI_REQUIRE_EQ(output.active_shell_tabs.at(Handle(0x100)), Handle(0x101));
}

WXI_TEST(
    tab_identity_rejects_generation_inconsistency_and_overflow,
    "tab_identity.generation_failures") {
    const std::array previous{Identity(11, 0x100, 0x101, 4, 7)};
    const std::array current{Target(11, 0x100, 0x101)};
    const std::array orders{Order(0x100, {{Handle(0x101), true}})};
    winexinfo::ObserverTabSetReconciliation output{};

    const auto missingLatest = Generations({}, {{Handle(0x101), 7}});
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, current, orders, missingLatest, &output));

    const auto wrongLatest = Generations(
        {{Handle(0x100), 4}},
        {{Handle(0x101), 6}});
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        previous, current, orders, wrongLatest, &output));

    const std::array<winexinfo::ObserverTabIdentity, 0> none{};
    const auto overflow = Generations(
        {},
        {{Handle(0x101), UINT64_MAX}});
    RequireContractFailure(winexinfo::ReconcileObserverTabSet(
        none, current, orders, overflow, &output));
}
