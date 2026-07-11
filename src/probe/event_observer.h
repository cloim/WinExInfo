#pragma once

#include "common/status.h"
#include "probe/probe_types.h"

#include <Windows.h>
#include <oaidl.h>
#include <UIAutomation.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace winexinfo {

enum class UiaHandlerKind {
    Selection,
    Structure,
};

enum class UiaSenderRelation {
    RegistrationElement,
    DirectChild,
    Descendant,
    RegisteredSubtree,
    Outside,
};

enum class UiaStructureSenderRole {
    NotApplicable,
    AddedChild,
    Container,
};

struct UiaEventEvidence final {
    EVENTID event_id;
    TreeScope registered_scope;
    std::uint64_t subscription_generation;
    std::uint64_t current_generation;
    DWORD subscription_process_id;
    DWORD sender_process_id;
    UiaSenderRelation sender_relation;
    std::wstring framework_id;
    CONTROLTYPEID control_type;
    std::wstring class_name;
    std::wstring automation_id;
    bool sender_is_offscreen;
    std::optional<StructureChangeType> structure_change_type;
    bool runtime_id_present;
    UINT runtime_id_dimensions;
    VARTYPE runtime_id_vartype;
    UiaStructureSenderRole structure_sender_role;
};

struct UiaHandlerContractEvidence final {
    UiaHandlerKind kind;
    TreeScope event_scope;
    bool cache_request_present;
    AutomationElementMode cache_element_mode;
    TreeScope cache_scope;
    std::vector<PROPERTYID> cached_properties;
};

struct UiaHandlerIdentity final {
    UiaHandlerKind kind;
    HWND top_level;
    std::uint64_t generation;
    std::uintptr_t element;
    std::uintptr_t handler;
};

struct LogicalActiveViewState final {
    HWND active_view;
    std::size_t active_view_count;
    bool filesystem_path_available;
    std::wstring filesystem_path;
    Status status;
};

struct ObservedEventTrigger final {
    ObservedEventKind kind;
    HWND source_top_level;
    std::uint64_t generation;
    bool source_shell_tab_present;
    HWND source_shell_tab;
    bool source_was_active;
    bool shell_cookie_present;
    LONG shell_cookie;
    ObservedStructureChangeType structure_change_type;
};

struct CanonicalShellEntryIdentity final {
    std::uint64_t entry_id;
    HWND top_level;
    std::uint64_t generation;
    HWND shell_tab;

    bool operator==(const CanonicalShellEntryIdentity&) const = default;
};

class EventCorrelationReducer final {
public:
    [[nodiscard]] Status SeedWindow(
        HWND topLevel,
        std::uint64_t generation,
        std::size_t topLevelEntryCount,
        const LogicalActiveViewState& state);
    [[nodiscard]] Status SeedInitialShellEntry(
        const CanonicalShellEntryIdentity& entry);
    [[nodiscard]] Status FinalizeInitialShellEntries(
        HWND topLevel,
        std::uint64_t generation);
    [[nodiscard]] Status FinalizeInitialBaseline();
    [[nodiscard]] Status ReconcileInitialEntryRevoke(
        LONG cookie,
        std::span<const CanonicalShellEntryIdentity> previousEntries,
        std::span<const CanonicalShellEntryIdentity> currentEntries,
        const ObservedEventTrigger& trigger,
        std::size_t remainingEntryCount,
        const LogicalActiveViewState& current,
        ObservedEventRecord* output,
        std::uint64_t* removedEntryId);
    [[nodiscard]] Status RecordRegistered(
        const ObservedEventTrigger& trigger,
        std::size_t topLevelEntryCount,
        const LogicalActiveViewState& current,
        ObservedEventRecord* output);
    [[nodiscard]] Status BeginRemap(
        const ObservedEventTrigger& trigger,
        bool* accepted);
    [[nodiscard]] Status CompleteRemap(
        const LogicalActiveViewState& current,
        ObservedEventRecord* output);
    [[nodiscard]] Status RecordRevoked(
        const ObservedEventTrigger& trigger,
        std::size_t remainingEntryCount,
        const LogicalActiveViewState& current,
        ObservedEventRecord* output);
    [[nodiscard]] Status GetWindowState(
        HWND topLevel,
        std::uint64_t generation,
        LogicalActiveViewState* output) const;

private:
    struct WindowState final {
        std::uint64_t generation;
        std::size_t last_observed_entry_count;
        LogicalActiveViewState active_view;
    };
    struct ShellEntryState final {
        HWND top_level;
        std::uint64_t generation;
        HWND shell_tab;
    };
    struct InitialShellEntryState final {
        HWND top_level;
        std::uint64_t generation;
        HWND shell_tab;
    };
    struct PendingRemap final {
        ObservedEventTrigger trigger;
        LogicalActiveViewState previous;
    };

    std::uint64_t next_sequence_ = 1;
    std::map<HWND, WindowState> windows_;
    std::map<HWND, std::uint64_t> latest_generations_;
    std::map<LONG, ShellEntryState> shell_entries_;
    std::map<std::uint64_t, InitialShellEntryState> initial_shell_entries_;
    std::set<std::pair<std::uintptr_t, std::uint64_t>> initial_windows_;
    std::set<std::pair<std::uintptr_t, std::uint64_t>> finalized_initial_windows_;
    bool initial_baseline_finalized_ = false;
    std::optional<PendingRemap> pending_remap_;
};

enum class ObserverCleanupKind {
    BrowserConnection,
    ShellLifecycleConnection,
    UiaSelection,
    UiaStructure,
};

struct ObserverCleanupRegistration final {
    ObserverCleanupKind kind;
    std::uintptr_t owner_identity;
    DWORD subscription_cookie;
    std::uintptr_t handler_identity;

    bool operator==(const ObserverCleanupRegistration&) const = default;
};

class ObserverCleanupLedger final {
public:
    [[nodiscard]] Status RecordSuccessfulRegistration(
        const ObserverCleanupRegistration& registration);
    [[nodiscard]] Status ConsumeEarlyRemoval(
        const ObserverCleanupRegistration& registration,
        const Status& cleanupStatus);
    [[nodiscard]] Status PreserveFirstFailure(const Status& failure);
    [[nodiscard]] Status BeginCleanup(
        std::vector<ObserverCleanupRegistration>* output);
    [[nodiscard]] Status MarkCleaned(
        const ObserverCleanupRegistration& registration,
        const Status& cleanupStatus);
    [[nodiscard]] Status FinishCleanup();

private:
    enum class Phase {
        Collecting,
        Cleaning,
        Finished,
    };

    Phase phase_ = Phase::Collecting;
    std::vector<ObserverCleanupRegistration> registrations_;
    std::vector<ObserverCleanupRegistration> cleanup_order_;
    std::size_t cleanup_index_ = 0;
    Status first_failure_{ErrorCode::OK, S_OK, ERROR_SUCCESS};
};

[[nodiscard]] Status ClassifyShellWindowsEvent(
    DISPID dispatchId,
    REFIID invokeIid,
    WORD flags,
    const DISPPARAMS* parameters,
    bool* accepted,
    ObservedEventKind* kind,
    LONG* shellCookie,
    UINT* argumentError = nullptr);
[[nodiscard]] Status ClassifyBrowserEvent(
    DISPID dispatchId,
    REFIID invokeIid,
    WORD flags,
    const DISPPARAMS* parameters,
    bool* accepted,
    ObservedEventKind* kind,
    UINT* argumentError = nullptr);
[[nodiscard]] Status ClassifyNavigateSource(
    std::size_t canonicalBrowserIdentityCount,
    HWND sourceShellTab,
    HWND selectedShellTab,
    bool* accepted);
[[nodiscard]] Status ClassifyUiaEvent(
    const UiaEventEvidence& evidence,
    bool* accepted,
    ObservedEventKind* kind);
[[nodiscard]] Status ValidateUiaHandlerContract(
    const UiaHandlerContractEvidence& evidence);

class ObserverSubscriptionState final {
public:
    [[nodiscard]] Status Start();
    [[nodiscard]] Status BeginStop();
    [[nodiscard]] Status FinishStop();
    [[nodiscard]] Status AcceptCallback(bool* accepted) const;
    [[nodiscard]] Status RegisterShellEntry(LONG cookie);
    [[nodiscard]] Status BindShellEntry(LONG cookie, HWND topLevel);
    [[nodiscard]] Status RevokeShellEntry(LONG cookie, HWND* previousTopLevel);
    [[nodiscard]] Status DiscardPendingShellEntryDuringStop(LONG cookie);
    [[nodiscard]] Status RegisterUiaHandler(const UiaHandlerIdentity& identity);
    [[nodiscard]] Status RemoveUiaHandler(const UiaHandlerIdentity& identity);

private:
    enum class Lifecycle {
        Created,
        Running,
        Stopping,
        Stopped,
    };

    Lifecycle lifecycle_ = Lifecycle::Created;
    std::map<LONG, std::optional<HWND>> shell_entries_;
    std::vector<UiaHandlerIdentity> uia_handlers_;
};

}  // namespace winexinfo
