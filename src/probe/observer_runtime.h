#pragma once

#include "host/command_line.h"
#include "probe/event_observer.h"
#include "probe/observer_runtime_contract.h"
#include "probe/probe_types.h"
#include "probe/shell_probe.h"
#include "probe/win32_probe.h"

#include <Windows.h>
#include <oaidl.h>
#include <ocidl.h>
#include <wrl/client.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace winexinfo {

enum class ObserverCallbackSource {
    ShellLifecycle,
    BrowserNavigate,
    UiaSelection,
    UiaStructure,
};

struct ObserverCallbackPayload final {
    ObserverCallbackSource source;
    ObservedEventKind kind;
    LONG shell_cookie;
    HWND top_level;
    std::uint64_t generation;
    HWND shell_tab;
    ObservedStructureChangeType structure_change_type;
};

struct ObserverCallbackEnvelope final {
    std::uint64_t sequence;
    ObserverCallbackPayload payload;
};

struct ObserverCallbackTicket final {
    std::uint64_t sequence;
    bool admitted;
};

enum class ObserverCallbackSlotState {
    Pending,
    Payload,
    Failure,
    Ignored,
};

struct ObserverCallbackSlot final {
    std::uint64_t sequence;
    ObserverCallbackSlotState state;
    std::optional<ObserverCallbackPayload> payload;
    std::optional<ObserverFailure> failure;
};

struct ObserverCallbackQueueOperations final {
    std::function<void(
        std::deque<ObserverCallbackSlot>*,
        std::uint64_t)> reserve;
    std::function<BOOL(HANDLE)> set_event;
    std::function<BOOL()> post_emergency;
    std::function<DWORD()> get_last_error;
};

struct ObserverCallbackEmergency final {
    ObserverFailure first_failure;
    bool any_transport_failure;
    Status first_transport_status;
    std::uint64_t fatal_raw_sequence;
};

class ObserverCallbackQueue final {
public:
    ObserverCallbackQueue(
        HANDLE wakeEvent,
        ObserverCallbackQueueOperations operations);

    [[nodiscard]] Status BeginRunning(
        const std::function<ObserverDeadline::Clock::time_point()>& now,
        ObserverDeadline::Clock::time_point* readyTime);
    [[nodiscard]] Status ValidateStartingState() const noexcept;
    [[nodiscard]] Status BeginStopping();
    [[nodiscard]] Status EnsureStoppingState() noexcept;
    [[nodiscard]] Status Admit(ObserverCallbackTicket* output) noexcept;
    [[nodiscard]] Status Complete(
        const ObserverCallbackTicket& ticket,
        ObserverCallbackPayload payload) noexcept;
    [[nodiscard]] Status CompleteFailure(
        const ObserverCallbackTicket& ticket,
        const ObserverFailure& failure) noexcept;
    [[nodiscard]] Status CompleteIgnored(
        const ObserverCallbackTicket& ticket) noexcept;
    [[nodiscard]] Status RecordCoordinatorFailure(
        const ObserverFailure& failure,
        std::optional<std::uint64_t> rawCutoff = std::nullopt) noexcept;
    [[nodiscard]] Status Drain(std::vector<ObserverCallbackEnvelope>* output) noexcept;
    [[nodiscard]] Status IsQuiescent(bool* output) const noexcept;
    [[nodiscard]] std::optional<ObserverCallbackEmergency> emergency() const noexcept;
    [[nodiscard]] std::size_t late_event_count() const noexcept;
    [[nodiscard]] std::size_t ignored_event_count() const noexcept;
    [[nodiscard]] bool has_callback_failure() const noexcept;
    [[nodiscard]] HANDLE wake_event() const noexcept;

private:
    enum class Phase {
        Starting,
        Running,
        Failed,
        Stopping,
    };

    void RecordEmergencyLocked(
        const ObserverFailure& failure,
        std::uint64_t fatalRawSequence) noexcept;
    [[nodiscard]] Status SignalWakeLocked(
        std::uint64_t fatalRawSequence) noexcept;
    [[nodiscard]] ObserverCallbackSlot* FindPendingSlotLocked(
        const ObserverCallbackTicket& ticket) noexcept;

    HANDLE wake_event_;
    ObserverCallbackQueueOperations operations_;
    mutable std::mutex mutex_;
    Phase phase_ = Phase::Starting;
    std::deque<ObserverCallbackSlot> queue_;
    std::uint64_t next_sequence_ = 1;
    std::optional<ObserverFailure> first_failure_;
    std::optional<std::uint64_t> fatal_raw_sequence_;
    bool any_transport_failure_ = false;
    Status first_transport_status_{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    std::size_t in_flight_ = 0;
    std::size_t late_event_count_ = 0;
    std::size_t ignored_event_count_ = 0;
    bool has_callback_failure_ = false;
};

enum class ObserverWaitOutcome {
    HandleReady,
    WindowMessageReady,
    DeadlineReached,
};

struct ObserverWaitResult final {
    ObserverWaitOutcome outcome;
    std::size_t ready_handle_index;
};

struct ObserverWaitOperations final {
    std::function<ObserverDeadline::Clock::time_point()> now;
    std::function<DWORD(DWORD, const HANDLE*, DWORD, DWORD, DWORD)> wait;
    std::function<DWORD()> get_last_error;
};

struct ObserverOperationResult;

[[nodiscard]] ObserverOperationResult WaitForObserverActivity(
    const ObserverDeadline& deadline,
    std::span<const HANDLE> waitHandles,
    const ObserverWaitOperations& operations,
    ObserverWaitResult* output) noexcept;

struct ShellWindowResolverOperations final {
    std::function<HRESULT(
        VARIANT*,
        VARIANT*,
        int,
        long*,
        int,
        IDispatch**)> find_window;
};

struct ObserverShellEntryMetadata final {
    std::uintptr_t canonical_identity;
    bool target_matched;
    HWND top_level;
    HWND shell_tab;

    bool operator==(const ObserverShellEntryMetadata&) const = default;
};

enum class ObserverShellTransitionKind {
    Stable,
    Registered,
    Revoked,
};

struct ObserverShellSetTransition final {
    std::optional<ObserverShellEntryMetadata> added;
    std::optional<ObserverShellEntryMetadata> removed;

    bool operator==(const ObserverShellSetTransition&) const = default;
};

struct ObserverShellLifecycleCorrelation final {
    bool target_matched;
    bool new_top_level;
    ObserverShellEntryMetadata entry;
    std::uint64_t generation;
    std::size_t top_level_entry_count;
};

[[nodiscard]] Status ClassifyObserverShellSetTransition(
    ObserverShellTransitionKind kind,
    std::span<const ObserverShellEntryMetadata> previous,
    std::span<const ObserverShellEntryMetadata> current,
    std::uintptr_t resolvedAddedIdentity,
    ObserverShellSetTransition* output);
[[nodiscard]] Status CorrelateObserverShellLifecycle(
    ObservedEventKind kind,
    std::span<const ObserverShellEntryMetadata> previous,
    std::span<const ObserverShellEntryMetadata> current,
    std::uintptr_t resolvedAddedIdentity,
    const std::map<HWND, std::uint64_t>& activeGenerations,
    const std::map<HWND, std::uint64_t>& latestGenerations,
    ObserverShellLifecycleCorrelation* output);

struct ObserverOperationResult final {
    Status status;
    std::optional<ObserverFailureOrigin> failure_origin;

    [[nodiscard]] bool ok() const noexcept {
        return status.ok();
    }
};

struct ObserverShellBrowserRegistration final {
    std::uintptr_t canonical_identity;
    std::uint64_t registration_id;

    bool operator==(const ObserverShellBrowserRegistration&) const = default;
};

struct ObserverShellStartupState final {
    std::uint64_t lifecycle_registration_id;
    std::vector<ObserverShellBrowserRegistration> browser_registrations;
    std::vector<ObserverShellEntryMetadata> baseline;
};

struct ObserverShellCleanupOutcome final {
    Status status;
    std::optional<ObserverFailureOrigin> failure_origin;
    bool any_transport_failure;
};

struct ObserverShellStartupOutcome final {
    ObserverOperationResult setup;
    bool any_setup_transport_failure;
    Status first_setup_transport_status;
    ObserverShellCleanupOutcome rollback;
};

struct ObserverShellStartupOperations final {
    std::function<ObserverOperationResult(std::uint64_t*)> advise_lifecycle;
    std::function<ObserverOperationResult(
        std::vector<ObserverShellEntryMetadata>*)> capture;
    std::function<ObserverOperationResult(
        const ObserverShellEntryMetadata&,
        std::uint64_t*)> advise_browser;
    std::function<ObserverOperationResult(std::uint64_t)> unadvise;
};

[[nodiscard]] ObserverShellCleanupOutcome CleanupObserverShellSubscriptions(
    const ObserverShellStartupOperations& operations,
    ObserverShellStartupState* state) noexcept;
[[nodiscard]] Status StartObserverShellSubscriptions(
    ObserverCallbackQueue* queue,
    const ObserverShellStartupOperations& operations,
    ObserverShellStartupState* state,
    ObserverShellStartupOutcome* output) noexcept;

struct ObserverCleanupOutcome final {
    Status status;
    std::optional<ObserverFailureOrigin> failure_origin;
    bool any_transport_failure;
};

struct ObserverStartupOutcome final {
    ObserverOperationResult setup;
    bool any_transport_failure;
    Status first_transport_status;
    ObserverCleanupOutcome rollback;
};

enum class ObserverEventDisposition {
    Ignored,
    Completed,
    Pending,
};

struct ObserverEventProcessingResult final {
    ObserverOperationResult operation;
    ObserverEventDisposition disposition;
    std::uint64_t raw_sequence;
    std::optional<ObservedEventRecord> record;
};

[[nodiscard]] Status ValidateObserverEventProcessingResult(
    const ObserverEventProcessingResult& result,
    const ObserverCallbackEnvelope& expected,
    std::uint64_t nextRecordSequence,
    bool allowPending) noexcept;

struct ObserverCoordinatorOperations final {
    std::function<ObserverStartupOutcome()> startup;
    std::function<ObserverEventProcessingResult(
        const ObserverCallbackEnvelope&)> process_event;
    std::function<ObserverOperationResult()> pump_messages;
    std::function<ObserverEventProcessingResult(
        std::size_t,
        std::uint64_t)> process_response;
    std::function<ObserverOperationResult()> begin_stop;
    std::function<ObserverCleanupOutcome()> cleanup;
    std::function<ObserverOperationResult(
        const EventObservationSnapshot&,
        bool*)> evaluate_gate;
    ObserverWaitOperations wait;
};

struct ObserverRuntimeOutcome;

[[nodiscard]] Status RunObserverCoordinator(
    std::uint32_t durationMs,
    std::uint32_t shutdownGraceMs,
    ObserverCallbackQueue* queue,
    HANDLE responseEvent,
    const ObserverCoordinatorOperations& operations,
    ObserverRuntimeOutcome* output) noexcept;

[[nodiscard]] ObserverOperationResult ClassifyObserverWin32Failure(
    DWORD lastError) noexcept;

[[nodiscard]] ObserverOperationResult CreateShellLifecycleEventSink(
    ObserverCallbackQueue* queue,
    Microsoft::WRL::ComPtr<IDispatch>& output);
[[nodiscard]] ObserverOperationResult CreateBrowserNavigateEventSink(
    ObserverCallbackQueue* queue,
    IUnknown* canonicalSource,
    HWND topLevel,
    std::uint64_t generation,
    HWND shellTab,
    Microsoft::WRL::ComPtr<IDispatch>& output);

[[nodiscard]] ObserverOperationResult FindRegisteredShellDispatch(
    LONG lifecycleCookie,
    const ShellWindowResolverOperations& operations,
    Microsoft::WRL::ComPtr<IDispatch>& output);

[[nodiscard]] ObserverOperationResult ClassifyObserverConnectionPointResult(
    HRESULT hresult,
    bool requiredObjectPresent);
[[nodiscard]] ObserverOperationResult ClassifyObserverAdviseResult(
    HRESULT hresult,
    DWORD subscriptionCookie);
[[nodiscard]] ObserverOperationResult ClassifyObserverUnadviseResult(
    HRESULT hresult);

struct ObserverConnectionPointRegistration final {
    Microsoft::WRL::ComPtr<IConnectionPoint> connection_point;
    Microsoft::WRL::ComPtr<IUnknown> sink;
    DWORD subscription_cookie;
    DWORD owner_thread_id;
};

[[nodiscard]] ObserverOperationResult AdviseObserverConnectionPoint(
    IUnknown* source,
    REFIID eventInterface,
    IUnknown* sink,
    ObserverConnectionPointRegistration* output);
[[nodiscard]] ObserverOperationResult UnadviseObserverConnectionPoint(
    ObserverConnectionPointRegistration* registration);

struct ObserverShellStaCapturedBrowser final {
    ObserverShellEntryMetadata metadata;
    Microsoft::WRL::ComPtr<IUnknown> canonical_identity;
    Microsoft::WRL::ComPtr<IUnknown> browser;
};

struct ObserverShellStaCapture final {
    ShellBrowserSetCapture browser_set;
    std::vector<ObserverShellStaCapturedBrowser> browsers;
};

struct ObserverShellStaBrowserResource final {
    std::uint64_t registration_id;
    std::uintptr_t canonical_identity;
    Microsoft::WRL::ComPtr<IUnknown> browser;
    ObserverConnectionPointRegistration connection;
};

struct ObserverShellStaResourceGraph final {
    DWORD owner_thread_id = 0;
    Microsoft::WRL::ComPtr<IShellWindows> shell_windows;
    ShellBrowserSetCapture browser_set;
    std::vector<ObserverShellStaCapturedBrowser> captured_browsers;
    ObserverShellStartupState startup_state;
    ObserverConnectionPointRegistration lifecycle_connection;
    std::vector<ObserverShellStaBrowserResource> browser_resources;
    std::uint64_t next_registration_id = 1;
    std::vector<HWND> target_top_levels;
};

struct ObserverShellStaOperations final {
    std::function<ObserverOperationResult()> prepare_message_queue;
    std::function<ObserverOperationResult(
        std::vector<ExplorerWindowRecord>*)> enumerate_windows;
    std::function<ObserverOperationResult(
        Microsoft::WRL::ComPtr<IShellWindows>&)> create_shell_windows;
    std::function<ObserverOperationResult(
        IShellWindows*,
        std::span<const HWND>,
        ObserverShellStaCapture*)> capture;
    std::function<ObserverOperationResult(
        ObserverCallbackQueue*,
        Microsoft::WRL::ComPtr<IDispatch>&)> create_lifecycle_sink;
    std::function<ObserverOperationResult(
        ObserverCallbackQueue*,
        IUnknown*,
        HWND,
        std::uint64_t,
        HWND,
        Microsoft::WRL::ComPtr<IDispatch>&)> create_browser_sink;
    std::function<ObserverOperationResult(
        IUnknown*,
        REFIID,
        IUnknown*,
        ObserverConnectionPointRegistration*)> advise;
    std::function<ObserverOperationResult(
        ObserverConnectionPointRegistration*)> unadvise;
    std::function<BOOL(MSG*, HWND, UINT, UINT, UINT)> peek_message;
    std::function<BOOL(const MSG*)> translate_message;
    std::function<LRESULT(const MSG*)> dispatch_message;
    std::function<DWORD()> current_thread_id;
};

[[nodiscard]] ObserverShellStaOperations
CreateProductionObserverShellStaOperations();
[[nodiscard]] Status StartObserverShellStaResources(
    ObserverCallbackQueue* queue,
    const ObserverShellStaOperations& operations,
    ObserverShellStaResourceGraph* graph,
    ObserverShellStartupOutcome* output) noexcept;
[[nodiscard]] ObserverShellCleanupOutcome CleanupObserverShellStaResources(
    const ObserverShellStaOperations& operations,
    ObserverShellStaResourceGraph* graph) noexcept;
[[nodiscard]] ObserverOperationResult PumpObserverShellStaMessages(
    const ObserverShellStaOperations& operations,
    const ObserverShellStaResourceGraph& graph) noexcept;

struct ObserverRuntimeOutcome final {
    EventObservationSnapshot snapshot;
    ObserverFailureState failures;
    ObserverCompletion completion;
};

[[nodiscard]] ObserverOperationResult EvaluateEventObservationGate(
    const EventObservationSnapshot& snapshot,
    bool* output) noexcept;

[[nodiscard]] Status RunObserverRuntime(
    std::uint32_t durationMs,
    ObserverRuntimeOutcome* output);

}  // namespace winexinfo
