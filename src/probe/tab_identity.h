#pragma once

#include "common/status.h"

#include <Windows.h>

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace winexinfo {

struct ObserverShellEntryMetadata final {
    std::uintptr_t canonical_identity;
    bool target_matched;
    HWND top_level;
    HWND shell_tab;

    bool operator==(const ObserverShellEntryMetadata&) const = default;
};

struct ObserverOrderedTab final {
    HWND shell_tab;
    bool visible;

    bool operator==(const ObserverOrderedTab&) const = default;
};

struct ObserverTopLevelTabOrder final {
    HWND top_level;
    std::vector<ObserverOrderedTab> tabs;

    bool operator==(const ObserverTopLevelTabOrder&) const = default;
};

struct ObserverTabIdentity final {
    std::uintptr_t canonical_identity;
    HWND top_level;
    HWND shell_tab;
    std::uint64_t top_level_generation;
    std::uint64_t tab_generation;

    bool operator==(const ObserverTabIdentity&) const = default;
};

struct ObserverTabGenerationState final {
    std::map<HWND, std::uint64_t> latest_top_levels;
    std::map<HWND, std::uint64_t> latest_shell_tabs;

    bool operator==(const ObserverTabGenerationState&) const = default;
};

struct ObserverTabSetReconciliation final {
    std::vector<ObserverTabIdentity> current;
    std::vector<ObserverTabIdentity> retained;
    std::vector<ObserverTabIdentity> added;
    std::vector<ObserverTabIdentity> removed;
    std::map<HWND, HWND> active_shell_tabs;
    ObserverTabGenerationState generations;

    bool operator==(const ObserverTabSetReconciliation&) const = default;
};

[[nodiscard]] Status ValidateObserverShellEntrySet(
    std::span<const ObserverShellEntryMetadata> entries) noexcept;

[[nodiscard]] Status ReconcileObserverTabSet(
    std::span<const ObserverTabIdentity> previous,
    std::span<const ObserverShellEntryMetadata> currentShellEntries,
    std::span<const ObserverTopLevelTabOrder> currentOrders,
    const ObserverTabGenerationState& generations,
    ObserverTabSetReconciliation* output) noexcept;

}  // namespace winexinfo
