#include "probe/event_observer.h"

#include <ExDispid.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <set>

namespace winexinfo {
namespace {

Status ContractFailure(const HRESULT hresult = S_FALSE) noexcept {
    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, ERROR_SUCCESS};
}

bool IsExactSuccessStatus(const Status& status) {
    return status.code == ErrorCode::OK && status.hresult == S_OK &&
        status.win32 == ERROR_SUCCESS;
}

bool IsCoherentStatus(const Status& status) {
    if (IsExactSuccessStatus(status)) {
        return true;
    }
    return status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        (status.hresult != S_OK || status.win32 != ERROR_SUCCESS);
}

bool IsLogicalStateValid(const LogicalActiveViewState& state) {
    if ((state.filesystem_path_available && state.filesystem_path.empty()) ||
        (!state.filesystem_path_available && !state.filesystem_path.empty())) {
        return false;
    }
    const bool success = IsExactSuccessStatus(state.status) &&
        state.active_view_count == 1 &&
        state.active_view != nullptr;
    const bool mismatch = !IsExactSuccessStatus(state.status) &&
        IsCoherentStatus(state.status) &&
        state.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        state.active_view_count != 1 && state.active_view == nullptr &&
        !state.filesystem_path_available && state.filesystem_path.empty();
    return success || mismatch;
}

bool IsTriggerMetadataValid(const ObservedEventTrigger& trigger) {
    if (trigger.source_top_level == nullptr || trigger.generation == 0 ||
        trigger.source_shell_tab_present != (trigger.source_shell_tab != nullptr)) {
        return false;
    }
    const bool cookieShape = trigger.shell_cookie_present || trigger.shell_cookie == 0;
    if (!cookieShape) {
        return false;
    }
    switch (trigger.kind) {
        case ObservedEventKind::WindowRegistered:
        case ObservedEventKind::WindowRevoked:
            return trigger.shell_cookie_present &&
                trigger.source_shell_tab_present &&
                trigger.structure_change_type == ObservedStructureChangeType::None;
        case ObservedEventKind::NavigateComplete2:
            return !trigger.shell_cookie_present &&
                trigger.source_shell_tab_present && trigger.source_was_active &&
                trigger.structure_change_type == ObservedStructureChangeType::None;
        case ObservedEventKind::TabSelected:
            return !trigger.shell_cookie_present &&
                trigger.structure_change_type == ObservedStructureChangeType::None;
        case ObservedEventKind::TabStructureChanged:
            if (trigger.shell_cookie_present) {
                return false;
            }
            switch (trigger.structure_change_type) {
                case ObservedStructureChangeType::ChildAdded:
                case ObservedStructureChangeType::ChildRemoved:
                case ObservedStructureChangeType::ChildrenInvalidated:
                case ObservedStructureChangeType::ChildrenBulkAdded:
                case ObservedStructureChangeType::ChildrenBulkRemoved:
                case ObservedStructureChangeType::ChildrenReordered:
                    return true;
                case ObservedStructureChangeType::None:
                    return false;
            }
            return false;
    }
    return false;
}

ObservedEventRecord MakeEventRecord(
    const std::uint64_t sequence,
    const ObservedEventTrigger& trigger,
    const ObservedEventTransition transition,
    const LogicalActiveViewState& previous,
    const LogicalActiveViewState& current,
    const std::size_t activeViewCount,
    const Status& status) {
    return {
        sequence,
        trigger.generation,
        trigger.kind,
        transition,
        trigger.source_top_level,
        trigger.source_shell_tab_present,
        trigger.source_shell_tab,
        trigger.source_was_active,
        trigger.shell_cookie_present,
        trigger.shell_cookie,
        trigger.structure_change_type,
        previous.active_view,
        current.active_view,
        activeViewCount,
        previous.filesystem_path_available,
        previous.filesystem_path,
        current.filesystem_path_available,
        current.filesystem_path,
        status,
    };
}

}  // namespace

Status ClassifyShellWindowsEvent(
    const DISPID dispatchId,
    REFIID invokeIid,
    const WORD flags,
    const DISPPARAMS* const parameters,
    bool* const accepted,
    ObservedEventKind* const kind,
    LONG* const shellCookie,
    UINT* const argumentError) {
    if (accepted == nullptr || kind == nullptr || shellCookie == nullptr) {
        return ContractFailure(E_INVALIDARG);
    }
    *accepted = false;
    if (dispatchId != DISPID_WINDOWREGISTERED &&
        dispatchId != DISPID_WINDOWREVOKED) {
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (invokeIid != IID_NULL) {
        return ContractFailure(DISP_E_UNKNOWNINTERFACE);
    }
    if (flags != DISPATCH_METHOD) {
        return ContractFailure(DISP_E_MEMBERNOTFOUND);
    }
    if (parameters == nullptr || parameters->cArgs != 1 ||
        parameters->rgvarg == nullptr) {
        return ContractFailure(DISP_E_BADPARAMCOUNT);
    }
    if (parameters->cNamedArgs != 0) {
        return ContractFailure(DISP_E_NONAMEDARGS);
    }
    if (parameters->rgvarg[0].vt != VT_I4) {
        if (argumentError != nullptr) {
            *argumentError = 0;
        }
        return ContractFailure(DISP_E_TYPEMISMATCH);
    }

    const ObservedEventKind classified =
        dispatchId == DISPID_WINDOWREGISTERED
        ? ObservedEventKind::WindowRegistered
        : ObservedEventKind::WindowRevoked;
    const LONG cookie = parameters->rgvarg[0].lVal;
    *kind = classified;
    *shellCookie = cookie;
    *accepted = true;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ClassifyBrowserEvent(
    const DISPID dispatchId,
    REFIID invokeIid,
    const WORD flags,
    const DISPPARAMS* const parameters,
    bool* const accepted,
    ObservedEventKind* const kind,
    UINT* const argumentError) {
    if (accepted == nullptr || kind == nullptr) {
        return ContractFailure(E_INVALIDARG);
    }
    *accepted = false;
    if (dispatchId != DISPID_NAVIGATECOMPLETE2) {
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (invokeIid != IID_NULL) {
        return ContractFailure(DISP_E_UNKNOWNINTERFACE);
    }
    if (flags != DISPATCH_METHOD) {
        return ContractFailure(DISP_E_MEMBERNOTFOUND);
    }
    if (parameters == nullptr || parameters->cArgs != 2 ||
        parameters->rgvarg == nullptr) {
        return ContractFailure(DISP_E_BADPARAMCOUNT);
    }
    if (parameters->cNamedArgs != 0) {
        return ContractFailure(DISP_E_NONAMEDARGS);
    }
    if (parameters->rgvarg[0].vt != (VT_BYREF | VT_VARIANT) ||
        parameters->rgvarg[0].pvarVal == nullptr) {
        if (argumentError != nullptr) {
            *argumentError = 0;
        }
        return ContractFailure(DISP_E_TYPEMISMATCH);
    }
    if (parameters->rgvarg[1].vt != VT_DISPATCH ||
        parameters->rgvarg[1].pdispVal == nullptr) {
        if (argumentError != nullptr) {
            *argumentError = 1;
        }
        return ContractFailure(DISP_E_TYPEMISMATCH);
    }

    *kind = ObservedEventKind::NavigateComplete2;
    *accepted = true;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ClassifyNavigateSource(
    const std::size_t canonicalBrowserIdentityCount,
    const HWND sourceShellTab,
    const HWND selectedShellTab,
    bool* const accepted) {
    if (accepted == nullptr || canonicalBrowserIdentityCount != 1 ||
        sourceShellTab == nullptr || selectedShellTab == nullptr) {
        return ContractFailure();
    }
    *accepted = sourceShellTab == selectedShellTab;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ClassifyUiaEvent(
    const UiaEventEvidence& evidence,
    bool* const accepted,
    ObservedEventKind* const kind) {
    if (accepted == nullptr || kind == nullptr) {
        return ContractFailure(E_INVALIDARG);
    }
    *accepted = false;
    if (evidence.event_id != UIA_SelectionItem_ElementSelectedEventId &&
        evidence.event_id != UIA_StructureChangedEventId) {
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (evidence.subscription_generation < evidence.current_generation) {
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (evidence.subscription_generation > evidence.current_generation) {
        return ContractFailure();
    }
    if (evidence.subscription_generation == 0 ||
        evidence.subscription_process_id == 0 ||
        evidence.sender_process_id != evidence.subscription_process_id) {
        return ContractFailure();
    }

    if (evidence.event_id == UIA_SelectionItem_ElementSelectedEventId) {
        if (evidence.registered_scope != TreeScope_Children ||
            evidence.sender_relation != UiaSenderRelation::DirectChild ||
            evidence.framework_id != L"XAML" ||
            evidence.control_type != UIA_TabItemControlTypeId ||
            evidence.class_name != L"ListViewItem" ||
            evidence.structure_change_type.has_value() ||
            evidence.runtime_id_present || evidence.runtime_id_dimensions != 0 ||
            evidence.runtime_id_vartype != VT_EMPTY ||
            evidence.structure_sender_role !=
                UiaStructureSenderRole::NotApplicable) {
            return ContractFailure();
        }
        *kind = ObservedEventKind::TabSelected;
        *accepted = true;
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    const bool senderInScope =
        evidence.sender_relation == UiaSenderRelation::RegistrationElement ||
        evidence.sender_relation == UiaSenderRelation::DirectChild ||
        evidence.sender_relation == UiaSenderRelation::Descendant ||
        evidence.sender_relation == UiaSenderRelation::RegisteredSubtree;
    if (evidence.registered_scope != TreeScope_Subtree || !senderInScope ||
        evidence.structure_sender_role == UiaStructureSenderRole::NotApplicable ||
        !evidence.structure_change_type.has_value()) {
        return ContractFailure();
    }
    const bool childAdded =
        *evidence.structure_change_type == StructureChangeType_ChildAdded;
    const bool childRemoved =
        *evidence.structure_change_type == StructureChangeType_ChildRemoved;
    const bool validChangeType = childAdded || childRemoved ||
        *evidence.structure_change_type == StructureChangeType_ChildrenInvalidated ||
        *evidence.structure_change_type == StructureChangeType_ChildrenBulkAdded ||
        *evidence.structure_change_type == StructureChangeType_ChildrenBulkRemoved ||
        *evidence.structure_change_type == StructureChangeType_ChildrenReordered;
    if (!validChangeType ||
        (childAdded &&
         (evidence.structure_sender_role != UiaStructureSenderRole::AddedChild ||
          evidence.sender_relation == UiaSenderRelation::RegistrationElement)) ||
        (!childAdded &&
         evidence.structure_sender_role != UiaStructureSenderRole::Container) ||
        (childRemoved &&
         (!evidence.runtime_id_present || evidence.runtime_id_dimensions != 1 ||
          evidence.runtime_id_vartype != VT_I4)) ||
        (!childRemoved &&
         (evidence.runtime_id_present || evidence.runtime_id_dimensions != 0 ||
          evidence.runtime_id_vartype != VT_EMPTY))) {
        return ContractFailure();
    }

    *kind = ObservedEventKind::TabStructureChanged;
    *accepted = true;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ValidateUiaHandlerContract(const UiaHandlerContractEvidence& evidence) {
    if (evidence.kind != UiaHandlerKind::Selection &&
        evidence.kind != UiaHandlerKind::Structure) {
        return ContractFailure();
    }
    const TreeScope requiredEventScope =
        evidence.kind == UiaHandlerKind::Selection ? TreeScope_Children
                                                  : TreeScope_Subtree;
    constexpr std::array<PROPERTYID, 6> requiredProperties = {
        UIA_FrameworkIdPropertyId,
        UIA_ControlTypePropertyId,
        UIA_ClassNamePropertyId,
        UIA_AutomationIdPropertyId,
        UIA_ProcessIdPropertyId,
        UIA_IsOffscreenPropertyId,
    };
    if (evidence.event_scope != requiredEventScope ||
        !evidence.cache_request_present ||
        evidence.cache_element_mode != AutomationElementMode_Full ||
        evidence.cache_scope != TreeScope_Element ||
        evidence.cached_properties.size() != requiredProperties.size()) {
        return ContractFailure();
    }
    std::vector<PROPERTYID> actual = evidence.cached_properties;
    std::ranges::sort(actual);
    std::array<PROPERTYID, requiredProperties.size()> expected = requiredProperties;
    std::ranges::sort(expected);
    if (!std::ranges::equal(actual, expected)) {
        return ContractFailure();
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::Start() {
    if (lifecycle_ != Lifecycle::Created || !shell_entries_.empty() ||
        !uia_handlers_.empty()) {
        return ContractFailure();
    }
    lifecycle_ = Lifecycle::Running;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::BeginStop() {
    if (lifecycle_ != Lifecycle::Running) {
        return ContractFailure();
    }
    lifecycle_ = Lifecycle::Stopping;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::FinishStop() {
    if (lifecycle_ != Lifecycle::Stopping || !shell_entries_.empty() ||
        !uia_handlers_.empty()) {
        return ContractFailure();
    }
    lifecycle_ = Lifecycle::Stopped;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::AcceptCallback(bool* const accepted) const {
    if (accepted == nullptr) {
        return ContractFailure(E_INVALIDARG);
    }
    *accepted = lifecycle_ == Lifecycle::Running;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::RegisterShellEntry(const LONG cookie) {
    if (lifecycle_ != Lifecycle::Running || shell_entries_.contains(cookie)) {
        return ContractFailure();
    }
    shell_entries_.emplace(cookie, std::nullopt);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::BindShellEntry(
    const LONG cookie,
    const HWND topLevel) {
    if (lifecycle_ != Lifecycle::Running || topLevel == nullptr) {
        return ContractFailure();
    }
    const auto entry = shell_entries_.find(cookie);
    if (entry == shell_entries_.end() || entry->second.has_value()) {
        return ContractFailure();
    }
    entry->second = topLevel;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::RevokeShellEntry(
    const LONG cookie,
    HWND* const previousTopLevel) {
    if ((lifecycle_ != Lifecycle::Running &&
         lifecycle_ != Lifecycle::Stopping) ||
        previousTopLevel == nullptr) {
        return ContractFailure();
    }
    const auto entry = shell_entries_.find(cookie);
    if (entry == shell_entries_.end() || !entry->second.has_value()) {
        return ContractFailure();
    }
    *previousTopLevel = entry->second.value_or(nullptr);
    shell_entries_.erase(entry);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::DiscardPendingShellEntryDuringStop(
    const LONG cookie) {
    if (lifecycle_ != Lifecycle::Stopping) {
        return ContractFailure();
    }
    const auto entry = shell_entries_.find(cookie);
    if (entry == shell_entries_.end() || entry->second.has_value()) {
        return ContractFailure();
    }
    shell_entries_.erase(entry);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::RegisterUiaHandler(
    const UiaHandlerIdentity& identity) {
    if (lifecycle_ != Lifecycle::Running ||
        (identity.kind != UiaHandlerKind::Selection &&
         identity.kind != UiaHandlerKind::Structure) ||
        identity.top_level == nullptr || identity.generation == 0 ||
        identity.element == 0 || identity.handler == 0 ||
        std::ranges::any_of(
            uia_handlers_, [&](const UiaHandlerIdentity& existing) {
                return existing.kind == identity.kind &&
                    existing.top_level == identity.top_level &&
                    existing.generation == identity.generation;
            })) {
        return ContractFailure();
    }
    uia_handlers_.push_back(identity);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverSubscriptionState::RemoveUiaHandler(
    const UiaHandlerIdentity& identity) {
    if (lifecycle_ != Lifecycle::Running && lifecycle_ != Lifecycle::Stopping) {
        return ContractFailure();
    }
    const auto registration = std::ranges::find_if(
        uia_handlers_, [&](const UiaHandlerIdentity& existing) {
            return existing.kind == identity.kind &&
                existing.top_level == identity.top_level &&
                existing.generation == identity.generation &&
                existing.element == identity.element &&
                existing.handler == identity.handler;
        });
    if (registration == uia_handlers_.end()) {
        return ContractFailure();
    }
    uia_handlers_.erase(registration);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::SeedWindow(
    const HWND topLevel,
    const std::uint64_t generation,
    const std::size_t topLevelEntryCount,
    const LogicalActiveViewState& state) {
    const auto latest = latest_generations_.find(topLevel);
    if (topLevel == nullptr || generation == 0 || topLevelEntryCount == 0 ||
        initial_baseline_finalized_ || windows_.contains(topLevel) ||
        (latest != latest_generations_.end() && generation <= latest->second) ||
        !IsExactSuccessStatus(state.status) || state.active_view_count != 1 ||
        !IsLogicalStateValid(state)) {
        return ContractFailure();
    }
    windows_.emplace(
        topLevel,
        WindowState{generation, topLevelEntryCount, state});
    latest_generations_[topLevel] = generation;
    initial_windows_.insert(
        {reinterpret_cast<std::uintptr_t>(topLevel), generation});
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::SeedInitialShellEntry(
    const CanonicalShellEntryIdentity& entry) {
    if (entry.entry_id == 0 || entry.top_level == nullptr ||
        entry.generation == 0 || entry.shell_tab == nullptr ||
        initial_baseline_finalized_ ||
        initial_shell_entries_.contains(entry.entry_id) ||
        finalized_initial_windows_.contains(
            {reinterpret_cast<std::uintptr_t>(entry.top_level),
             entry.generation}) ||
        std::ranges::any_of(
            initial_shell_entries_,
            [&](const auto& existing) {
                return existing.second.shell_tab == entry.shell_tab;
            })) {
        return ContractFailure();
    }
    const auto window = windows_.find(entry.top_level);
    if (window == windows_.end() ||
        window->second.generation != entry.generation) {
        return ContractFailure();
    }
    std::size_t seededForWindow = 0;
    for (const auto& [existingId, existingEntry] : initial_shell_entries_) {
        static_cast<void>(existingId);
        if (existingEntry.top_level == window->first &&
            existingEntry.generation == window->second.generation) {
            ++seededForWindow;
        }
    }
    if (seededForWindow >= window->second.last_observed_entry_count) {
        return ContractFailure();
    }
    initial_shell_entries_.emplace(
        entry.entry_id,
        InitialShellEntryState{
            entry.top_level,
            entry.generation,
            entry.shell_tab,
        });
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::FinalizeInitialShellEntries(
    const HWND topLevel,
    const std::uint64_t generation) {
    if (topLevel == nullptr || generation == 0 ||
        initial_baseline_finalized_) {
        return ContractFailure();
    }
    const auto key = std::pair{
        reinterpret_cast<std::uintptr_t>(topLevel), generation};
    const auto window = windows_.find(topLevel);
    if (window == windows_.end() || window->second.generation != generation ||
        finalized_initial_windows_.contains(key)) {
        return ContractFailure();
    }
    std::size_t seededForWindow = 0;
    for (const auto& [entryId, entry] : initial_shell_entries_) {
        static_cast<void>(entryId);
        if (entry.top_level == topLevel && entry.generation == generation) {
            ++seededForWindow;
        }
    }
    if (seededForWindow != window->second.last_observed_entry_count) {
        return ContractFailure();
    }
    finalized_initial_windows_.insert(key);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::FinalizeInitialBaseline() {
    if (initial_baseline_finalized_ ||
        initial_windows_ != finalized_initial_windows_) {
        return ContractFailure();
    }
    initial_baseline_finalized_ = true;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::ReconcileInitialEntryRevoke(
    const LONG cookie,
    const std::span<const CanonicalShellEntryIdentity> previousEntries,
    const std::span<const CanonicalShellEntryIdentity> currentEntries,
    const ObservedEventTrigger& trigger,
    const std::size_t remainingEntryCount,
    const LogicalActiveViewState& currentState,
    ObservedEventRecord* const output,
    std::uint64_t* const removedEntryId) {
    if (output == nullptr || removedEntryId == nullptr ||
        !initial_baseline_finalized_ ||
        pending_remap_.has_value() || shell_entries_.contains(cookie) ||
        trigger.kind != ObservedEventKind::WindowRevoked ||
        !IsTriggerMetadataValid(trigger) ||
        trigger.shell_cookie != cookie) {
        return ContractFailure();
    }

    std::map<std::uint64_t, CanonicalShellEntryIdentity> previous;
    std::set<HWND> previousShellTabs;
    for (const CanonicalShellEntryIdentity& entry : previousEntries) {
        if (entry.entry_id == 0 || entry.top_level == nullptr ||
            entry.generation == 0 || entry.shell_tab == nullptr ||
            !previous.emplace(entry.entry_id, entry).second ||
            !previousShellTabs.insert(entry.shell_tab).second) {
            return ContractFailure();
        }
    }

    std::map<std::uint64_t, CanonicalShellEntryIdentity> current;
    std::set<HWND> currentShellTabs;
    for (const CanonicalShellEntryIdentity& entry : currentEntries) {
        const auto previousEntry = previous.find(entry.entry_id);
        if (entry.entry_id == 0 || entry.top_level == nullptr ||
            entry.generation == 0 || entry.shell_tab == nullptr ||
            previousEntry == previous.end() ||
            previousEntry->second != entry ||
            !current.emplace(entry.entry_id, entry).second ||
            !currentShellTabs.insert(entry.shell_tab).second) {
            return ContractFailure();
        }
    }

    if (previous.size() != initial_shell_entries_.size()) {
        return ContractFailure();
    }
    for (const auto& [entryId, stored] : initial_shell_entries_) {
        const auto supplied = previous.find(entryId);
        if (supplied == previous.end() ||
            supplied->second.top_level != stored.top_level ||
            supplied->second.generation != stored.generation ||
            supplied->second.shell_tab != stored.shell_tab) {
            return ContractFailure();
        }
    }

    if (previous.size() != current.size() + 1) {
        return ContractFailure();
    }
    const auto removed = std::ranges::find_if(
        previous,
        [&](const auto& entry) {
            return !current.contains(entry.first);
        });
    if (removed == previous.end()) {
        return ContractFailure();
    }
    const auto entry = initial_shell_entries_.find(removed->first);
    if (entry == initial_shell_entries_.end()) {
        return ContractFailure();
    }
    const auto finalizedKey = std::pair{
        reinterpret_cast<std::uintptr_t>(entry->second.top_level),
        entry->second.generation};
    if (!finalized_initial_windows_.contains(finalizedKey) ||
        entry->second.top_level != trigger.source_top_level ||
        entry->second.generation != trigger.generation ||
        entry->second.shell_tab != trigger.source_shell_tab) {
        return ContractFailure();
    }

    EventCorrelationReducer candidate = *this;
    const auto candidateEntry = candidate.initial_shell_entries_.find(entry->first);
    if (candidateEntry == candidate.initial_shell_entries_.end() ||
        !candidate.shell_entries_
             .emplace(
        cookie,
        ShellEntryState{
            candidateEntry->second.top_level,
            candidateEntry->second.generation,
            candidateEntry->second.shell_tab,
        })
             .second) {
        return ContractFailure();
    }
    const std::uint64_t removedId = candidateEntry->first;
    candidate.initial_shell_entries_.erase(candidateEntry);
    ObservedEventRecord candidateRecord{};
    const Status revokeStatus = candidate.RecordRevoked(
        trigger,
        remainingEntryCount,
        currentState,
        &candidateRecord);
    if (!revokeStatus.ok()) {
        return revokeStatus;
    }

    *this = std::move(candidate);
    *output = std::move(candidateRecord);
    *removedEntryId = removedId;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::RecordRegistered(
    const ObservedEventTrigger& trigger,
    const std::size_t topLevelEntryCount,
    const LogicalActiveViewState& current,
    ObservedEventRecord* const output) {
    if (output == nullptr || pending_remap_.has_value() ||
        trigger.kind != ObservedEventKind::WindowRegistered ||
        !IsTriggerMetadataValid(trigger) ||
        topLevelEntryCount == 0 || shell_entries_.contains(trigger.shell_cookie)) {
        return ContractFailure();
    }
    const bool pendingResult =
        current.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
        current.status.hresult == S_FALSE &&
        current.status.win32 == ERROR_SUCCESS &&
        current.active_view_count == 0 && current.active_view == nullptr &&
        !current.filesystem_path_available && current.filesystem_path.empty();
    if (!pendingResult && !IsLogicalStateValid(current)) {
        return ContractFailure();
    }
    const LogicalActiveViewState hidden{
        nullptr,
        0,
        false,
        {},
        {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
    };
    LogicalActiveViewState previous = hidden;
    const auto existing = windows_.find(trigger.source_top_level);
    if (existing == windows_.end()) {
        if (topLevelEntryCount != 1) {
            return ContractFailure();
        }
        const auto latest = latest_generations_.find(trigger.source_top_level);
        if (latest != latest_generations_.end() &&
            trigger.generation <= latest->second) {
            return ContractFailure();
        }
        windows_.emplace(
            trigger.source_top_level,
            WindowState{trigger.generation, topLevelEntryCount, hidden});
        latest_generations_[trigger.source_top_level] = trigger.generation;
    } else {
        if (existing->second.generation != trigger.generation ||
            existing->second.last_observed_entry_count ==
                std::numeric_limits<std::size_t>::max() ||
            topLevelEntryCount !=
                existing->second.last_observed_entry_count + 1) {
            return ContractFailure();
        }
        previous = existing->second.active_view;
    }
    const bool remapped = IsExactSuccessStatus(current.status);
    const ObservedEventTransition transition = remapped
        ? ObservedEventTransition::Remapped
        : (pendingResult ? ObservedEventTransition::Pending
                         : ObservedEventTransition::Mismatch);
    const Status recordStatus = transition == ObservedEventTransition::Pending
        ? Status{ErrorCode::OK, S_OK, ERROR_SUCCESS}
        : current.status;
    windows_.find(trigger.source_top_level)->second.active_view =
        remapped ? current : hidden;
    windows_.find(trigger.source_top_level)->second.last_observed_entry_count =
        topLevelEntryCount;
    shell_entries_.emplace(
        trigger.shell_cookie,
        ShellEntryState{
            trigger.source_top_level,
            trigger.generation,
            trigger.source_shell_tab,
        });
    *output = MakeEventRecord(
        next_sequence_++,
        trigger,
        transition,
        previous,
        remapped ? current : hidden,
        current.active_view_count,
        recordStatus);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::BeginRemap(
    const ObservedEventTrigger& trigger,
    bool* const accepted) {
    if (accepted == nullptr) {
        return ContractFailure(E_INVALIDARG);
    }
    *accepted = false;
    if (pending_remap_.has_value() || !IsTriggerMetadataValid(trigger) ||
        (trigger.kind != ObservedEventKind::NavigateComplete2 &&
         trigger.kind != ObservedEventKind::TabSelected &&
         trigger.kind != ObservedEventKind::TabStructureChanged)) {
        return ContractFailure();
    }
    const auto window = windows_.find(trigger.source_top_level);
    if (window == windows_.end()) {
        const auto latest = latest_generations_.find(trigger.source_top_level);
        if (latest != latest_generations_.end() &&
            trigger.generation <= latest->second) {
            return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
        }
        return ContractFailure();
    }
    if (window->second.generation != trigger.generation) {
        return trigger.generation < window->second.generation
            ? Status{ErrorCode::OK, S_OK, ERROR_SUCCESS}
            : ContractFailure();
    }
    pending_remap_ = PendingRemap{trigger, window->second.active_view};
    window->second.active_view = {
        nullptr,
        0,
        false,
        {},
        {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
    };
    *accepted = true;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::CompleteRemap(
    const LogicalActiveViewState& current,
    ObservedEventRecord* const output) {
    if (output == nullptr || !pending_remap_.has_value() ||
        !IsLogicalStateValid(current)) {
        return ContractFailure();
    }
    const PendingRemap pending = *pending_remap_;
    const auto window = windows_.find(pending.trigger.source_top_level);
    if (window == windows_.end() ||
        window->second.generation != pending.trigger.generation) {
        return ContractFailure();
    }
    const bool remapped = IsExactSuccessStatus(current.status);
    const LogicalActiveViewState hidden{
        nullptr,
        0,
        false,
        {},
        {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
    };
    window->second.active_view = remapped ? current : hidden;
    *output = MakeEventRecord(
        next_sequence_++,
        pending.trigger,
        remapped ? ObservedEventTransition::Remapped
                 : ObservedEventTransition::Mismatch,
        pending.previous,
        remapped ? current : hidden,
        current.active_view_count,
        current.status);
    pending_remap_.reset();
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::RecordRevoked(
    const ObservedEventTrigger& trigger,
    const std::size_t remainingEntryCount,
    const LogicalActiveViewState& current,
    ObservedEventRecord* const output) {
    const bool terminalState = IsExactSuccessStatus(current.status) &&
        current.active_view_count == 0 && current.active_view == nullptr &&
        !current.filesystem_path_available && current.filesystem_path.empty();
    if (output == nullptr || pending_remap_.has_value() ||
        trigger.kind != ObservedEventKind::WindowRevoked ||
        !IsTriggerMetadataValid(trigger) ||
        !terminalState) {
        return ContractFailure();
    }
    const auto shellEntry = shell_entries_.find(trigger.shell_cookie);
    if (shellEntry == shell_entries_.end() ||
        shellEntry->second.top_level != trigger.source_top_level ||
        shellEntry->second.generation != trigger.generation ||
        shellEntry->second.shell_tab != trigger.source_shell_tab) {
        return ContractFailure();
    }
    const auto window = windows_.find(trigger.source_top_level);
    if (window == windows_.end() || window->second.generation != trigger.generation) {
        return ContractFailure();
    }
    if (window->second.last_observed_entry_count == 0 ||
        remainingEntryCount == std::numeric_limits<std::size_t>::max() ||
        remainingEntryCount + 1 != window->second.last_observed_entry_count) {
        return ContractFailure();
    }
    if (remainingEntryCount == 0 &&
        std::ranges::any_of(
            shell_entries_,
            [&](const auto& entry) {
                return entry.first != trigger.shell_cookie &&
                    entry.second.top_level == trigger.source_top_level &&
                    entry.second.generation == trigger.generation;
            })) {
        return ContractFailure();
    }
    const LogicalActiveViewState hidden{
        nullptr,
        0,
        false,
        {},
        {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
    };
    const LogicalActiveViewState previous = window->second.active_view;
    if (remainingEntryCount == 0) {
        windows_.erase(window);
    } else {
        window->second.active_view = hidden;
        window->second.last_observed_entry_count = remainingEntryCount;
    }
    shell_entries_.erase(shellEntry);
    *output = MakeEventRecord(
        next_sequence_++,
        trigger,
        ObservedEventTransition::Revoked,
        previous,
        hidden,
        current.active_view_count,
        current.status);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status EventCorrelationReducer::GetWindowState(
    const HWND topLevel,
    const std::uint64_t generation,
    LogicalActiveViewState* const output) const {
    if (output == nullptr || topLevel == nullptr || generation == 0) {
        return ContractFailure(E_INVALIDARG);
    }
    const auto window = windows_.find(topLevel);
    if (window == windows_.end() || window->second.generation != generation ||
        !IsLogicalStateValid(window->second.active_view)) {
        return ContractFailure();
    }
    *output = window->second.active_view;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverCleanupLedger::RecordSuccessfulRegistration(
    const ObserverCleanupRegistration& registration) {
    const bool connection =
        registration.kind == ObserverCleanupKind::BrowserConnection ||
        registration.kind == ObserverCleanupKind::ShellLifecycleConnection;
    const bool uia = registration.kind == ObserverCleanupKind::UiaSelection ||
        registration.kind == ObserverCleanupKind::UiaStructure;
    if (phase_ != Phase::Collecting || (!connection && !uia) ||
        registration.owner_identity == 0 ||
        (connection &&
         (registration.subscription_cookie == 0 ||
          registration.handler_identity != 0)) ||
        (uia &&
         (registration.subscription_cookie != 0 ||
          registration.handler_identity == 0)) ||
        (registration.kind == ObserverCleanupKind::ShellLifecycleConnection &&
         std::ranges::any_of(
             registrations_,
             [](const ObserverCleanupRegistration& existing) {
                 return existing.kind ==
                     ObserverCleanupKind::ShellLifecycleConnection;
             })) ||
        std::ranges::any_of(
            registrations_,
            [&](const ObserverCleanupRegistration& existing) {
                return existing.kind == registration.kind &&
                    existing.owner_identity == registration.owner_identity;
            })) {
        return ContractFailure();
    }
    registrations_.push_back(registration);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverCleanupLedger::ConsumeEarlyRemoval(
    const ObserverCleanupRegistration& registration,
    const Status& cleanupStatus) {
    if (phase_ != Phase::Collecting) {
        return ContractFailure();
    }
    const auto existing = std::ranges::find(registrations_, registration);
    if (existing == registrations_.end()) {
        return ContractFailure();
    }
    if (!IsCoherentStatus(cleanupStatus)) {
        return ContractFailure();
    }
    static_cast<void>(PreserveFirstFailure(cleanupStatus));
    if (IsExactSuccessStatus(cleanupStatus)) {
        registrations_.erase(existing);
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverCleanupLedger::PreserveFirstFailure(const Status& failure) {
    if (phase_ == Phase::Finished || !IsCoherentStatus(failure)) {
        return ContractFailure();
    }
    if (!IsExactSuccessStatus(failure) && IsExactSuccessStatus(first_failure_)) {
        first_failure_ = failure;
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverCleanupLedger::BeginCleanup(
    std::vector<ObserverCleanupRegistration>* const output) {
    if (phase_ != Phase::Collecting || output == nullptr) {
        return ContractFailure();
    }
    phase_ = Phase::Cleaning;
    const ObserverCleanupKind groups[] = {
        ObserverCleanupKind::BrowserConnection,
        ObserverCleanupKind::ShellLifecycleConnection,
        ObserverCleanupKind::UiaStructure,
        ObserverCleanupKind::UiaSelection,
    };
    for (const ObserverCleanupKind group : groups) {
        for (auto registration = registrations_.rbegin();
             registration != registrations_.rend();
             ++registration) {
            if (registration->kind == group) {
                cleanup_order_.push_back(*registration);
            }
        }
    }
    *output = cleanup_order_;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverCleanupLedger::MarkCleaned(
    const ObserverCleanupRegistration& registration,
    const Status& cleanupStatus) {
    if (phase_ != Phase::Cleaning || cleanup_index_ >= cleanup_order_.size() ||
        cleanup_order_[cleanup_index_] != registration) {
        return ContractFailure();
    }
    if (!IsCoherentStatus(cleanupStatus)) {
        return ContractFailure();
    }
    static_cast<void>(PreserveFirstFailure(cleanupStatus));
    ++cleanup_index_;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ObserverCleanupLedger::FinishCleanup() {
    if (phase_ != Phase::Cleaning || cleanup_index_ != cleanup_order_.size()) {
        return ContractFailure();
    }
    phase_ = Phase::Finished;
    return first_failure_;
}

}  // namespace winexinfo
