#include "probe/tab_identity.h"

#include <algorithm>
#include <limits>
#include <set>

namespace winexinfo {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ContractFailure(
    const HRESULT hresult = S_FALSE,
    const DWORD win32 = ERROR_INVALID_DATA) noexcept {
    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, win32};
}

bool IsExactIdentity(
    const ObserverTabIdentity& left,
    const ObserverTabIdentity& right) noexcept {
    return left.canonical_identity == right.canonical_identity &&
        left.top_level == right.top_level && left.shell_tab == right.shell_tab;
}

bool IsExactIdentity(
    const ObserverTabIdentity& left,
    const ObserverShellEntryMetadata& right) noexcept {
    return left.canonical_identity == right.canonical_identity &&
        left.top_level == right.top_level && left.shell_tab == right.shell_tab;
}

Status NextGeneration(
    const std::map<HWND, std::uint64_t>& latest,
    const HWND handle,
    std::uint64_t* const output) noexcept {
    const auto found = latest.find(handle);
    if (found == latest.end()) {
        *output = 1;
        return Success();
    }
    if (found->second == std::numeric_limits<std::uint64_t>::max()) {
        return ContractFailure();
    }
    *output = found->second + 1;
    return Success();
}

}  // namespace

Status ValidateObserverShellEntrySet(
    const std::span<const ObserverShellEntryMetadata> entries) noexcept {
    std::set<std::uintptr_t> canonicalIdentities;
    std::set<HWND> targetTabs;
    for (const ObserverShellEntryMetadata& entry : entries) {
        if (entry.canonical_identity == 0 || entry.top_level == nullptr ||
            entry.target_matched != (entry.shell_tab != nullptr) ||
            !canonicalIdentities.insert(entry.canonical_identity).second ||
            (entry.target_matched &&
             !targetTabs.insert(entry.shell_tab).second)) {
            return ContractFailure();
        }
    }
    return Success();
}

Status ReconcileObserverTabSet(
    const std::span<const ObserverTabIdentity> previous,
    const std::span<const ObserverShellEntryMetadata> currentShellEntries,
    const std::span<const ObserverTopLevelTabOrder> currentOrders,
    const ObserverTabGenerationState& generations,
    ObserverTabSetReconciliation* const output) noexcept {
    if (output == nullptr) {
        return ContractFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }

    for (const auto& [handle, generation] : generations.latest_top_levels) {
        if (handle == nullptr || generation == 0) {
            return ContractFailure();
        }
    }
    for (const auto& [handle, generation] : generations.latest_shell_tabs) {
        if (handle == nullptr || generation == 0) {
            return ContractFailure();
        }
    }

    std::set<std::uintptr_t> previousCanonical;
    std::set<HWND> previousTabs;
    std::map<HWND, std::uint64_t> activeTopGenerations;
    for (const ObserverTabIdentity& entry : previous) {
        if (entry.canonical_identity == 0 || entry.top_level == nullptr ||
            entry.shell_tab == nullptr || entry.top_level_generation == 0 ||
            entry.tab_generation == 0 ||
            !previousCanonical.insert(entry.canonical_identity).second ||
            !previousTabs.insert(entry.shell_tab).second) {
            return ContractFailure();
        }
        const auto [top, inserted] = activeTopGenerations.emplace(
            entry.top_level, entry.top_level_generation);
        if (!inserted && top->second != entry.top_level_generation) {
            return ContractFailure();
        }
        const auto latestTop = generations.latest_top_levels.find(entry.top_level);
        const auto latestTab = generations.latest_shell_tabs.find(entry.shell_tab);
        if (latestTop == generations.latest_top_levels.end() ||
            latestTop->second != entry.top_level_generation ||
            latestTab == generations.latest_shell_tabs.end() ||
            latestTab->second != entry.tab_generation) {
            return ContractFailure();
        }
    }

    const Status currentShape =
        ValidateObserverShellEntrySet(currentShellEntries);
    if (!currentShape.ok()) {
        return currentShape;
    }
    std::set<HWND> currentTargetTabs;
    for (const ObserverShellEntryMetadata& entry : currentShellEntries) {
        if (entry.target_matched) {
            currentTargetTabs.insert(entry.shell_tab);
        }
    }

    std::set<HWND> orderedTopLevels;
    std::set<HWND> orderedTabs;
    std::map<HWND, HWND> activeShellTabs;
    for (const ObserverTopLevelTabOrder& order : currentOrders) {
        if (order.top_level == nullptr || order.tabs.empty() ||
            !orderedTopLevels.insert(order.top_level).second) {
            return ContractFailure();
        }
        HWND active = nullptr;
        for (const ObserverOrderedTab& tab : order.tabs) {
            if (tab.shell_tab == nullptr ||
                !orderedTabs.insert(tab.shell_tab).second) {
                return ContractFailure();
            }
            if (tab.visible) {
                if (active == nullptr) {
                    active = tab.shell_tab;
                }
            }
        }
        if (active == nullptr) {
            return ContractFailure();
        }
        activeShellTabs.emplace(order.top_level, active);
    }
    if (!std::includes(
            currentTargetTabs.begin(),
            currentTargetTabs.end(),
            orderedTabs.begin(),
            orderedTabs.end())) {
        return ContractFailure();
    }

    ObserverTabSetReconciliation candidate{};
    candidate.generations = generations;
    candidate.active_shell_tabs = std::move(activeShellTabs);

    for (const ObserverTopLevelTabOrder& order : currentOrders) {
        std::uint64_t topGeneration = 0;
        const auto activeTop = activeTopGenerations.find(order.top_level);
        if (activeTop != activeTopGenerations.end()) {
            topGeneration = activeTop->second;
        } else {
            const Status next = NextGeneration(
                candidate.generations.latest_top_levels,
                order.top_level,
                &topGeneration);
            if (!next.ok()) {
                return next;
            }
        }
        candidate.generations.latest_top_levels[order.top_level] = topGeneration;

        for (const ObserverOrderedTab& orderedTab : order.tabs) {
            const auto metadata = std::find_if(
                currentShellEntries.begin(),
                currentShellEntries.end(),
                [&](const ObserverShellEntryMetadata& entry) {
                    return entry.target_matched &&
                        entry.top_level == order.top_level &&
                        entry.shell_tab == orderedTab.shell_tab;
                });
            if (metadata == currentShellEntries.end()) {
                return ContractFailure();
            }

            const auto retained = std::find_if(
                previous.begin(),
                previous.end(),
                [&](const ObserverTabIdentity& entry) {
                    return IsExactIdentity(entry, *metadata);
                });
            std::uint64_t tabGeneration = 0;
            if (retained != previous.end()) {
                tabGeneration = retained->tab_generation;
            } else {
                const Status next = NextGeneration(
                    candidate.generations.latest_shell_tabs,
                    orderedTab.shell_tab,
                    &tabGeneration);
                if (!next.ok()) {
                    return next;
                }
            }
            candidate.generations.latest_shell_tabs[orderedTab.shell_tab] =
                tabGeneration;
            ObserverTabIdentity identity{
                metadata->canonical_identity,
                metadata->top_level,
                metadata->shell_tab,
                topGeneration,
                tabGeneration,
            };
            candidate.current.push_back(identity);
            if (retained == previous.end()) {
                candidate.added.push_back(identity);
            } else {
                candidate.retained.push_back(identity);
            }
        }
    }

    for (const ObserverTabIdentity& oldEntry : previous) {
        const auto retained = std::find_if(
            candidate.current.begin(),
            candidate.current.end(),
            [&](const ObserverTabIdentity& entry) {
                return IsExactIdentity(entry, oldEntry);
            });
        if (retained == candidate.current.end()) {
            candidate.removed.push_back(oldEntry);
        }
    }

    *output = std::move(candidate);
    return Success();
}

}  // namespace winexinfo
