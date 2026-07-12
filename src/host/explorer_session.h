#pragma once

#include "common/status.h"
#include "injection/thread_hook_injector.h"
#include "ipc/protocol.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <mutex>
#include <vector>

namespace winexinfo {

struct SessionWindowSnapshot final {
    HWND top_level = nullptr;
    std::uint64_t top_level_generation = 0;
    std::vector<ipc::TabDescriptor> tabs;
    HWND active_tab = nullptr;
    std::uint64_t active_tab_generation = 0;
    std::string display_text;
    std::string tooltip_text;

    bool operator==(const SessionWindowSnapshot&) const = default;
};

class LifecycleDiagnosticJournal final {
public:
    explicit LifecycleDiagnosticJournal(std::size_t capacity = 4096);
    ~LifecycleDiagnosticJournal();
    LifecycleDiagnosticJournal(const LifecycleDiagnosticJournal&) = delete;
    LifecycleDiagnosticJournal& operator=(const LifecycleDiagnosticJournal&) = delete;
    [[nodiscard]] bool TryAppend(
        std::string dedupKey, std::string line) noexcept;
    [[nodiscard]] bool incomplete() const noexcept;
    [[nodiscard]] std::vector<std::string> Snapshot() const;
    void Flush(std::ostream& output) const;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct ExplorerSessionOperations final {
    std::function<Status(
        const SessionWindowSnapshot&, injection::HookTarget*)> validate_window;
    std::function<Status(
        const injection::HookTarget&,
        const std::function<Status()>&,
        injection::HookAttachOutcome*)> attach_initial;
    std::function<Status(
        const injection::HookTarget&,
        const std::function<Status()>&)> ensure_thread_hook_lease;
    std::function<Status(
        const std::vector<std::uint8_t>&, ipc::DecodedFrame*)> exchange;
    std::function<Status(DWORD)> release_hooks;
    std::function<Status(DWORD, DWORD, bool*)> wait_exact_module_absent;
    std::function<Status(DWORD)> confirm_target_gone;
    std::shared_ptr<LifecycleDiagnosticJournal> diagnostic_journal;
};

class ExplorerSession final {
public:
    ExplorerSession(DWORD processId, ExplorerSessionOperations operations);
    ExplorerSession(const ExplorerSession&) = delete;
    ExplorerSession& operator=(const ExplorerSession&) = delete;

    [[nodiscard]] Status Reconcile(std::span<const SessionWindowSnapshot> windows);
    [[nodiscard]] Status Stop();

private:
    struct WindowState final {
        SessionWindowSnapshot snapshot;
        DWORD ui_thread_id = 0;
    };

    [[nodiscard]] Status NextRequestId(std::uint64_t* output) noexcept;
    [[nodiscard]] Status SendUpdate(const SessionWindowSnapshot& snapshot);
    [[nodiscard]] Status SendPaneText(const SessionWindowSnapshot& snapshot);
    [[nodiscard]] Status SendRemoval(const WindowState& window);
    void RecordDiagnostic(std::string key, std::string line) noexcept;

    DWORD process_id_ = 0;
    std::mutex mutex_;
    ExplorerSessionOperations operations_;
    std::vector<WindowState> windows_;
    std::vector<DWORD> installed_thread_ids_;
    std::uint64_t last_request_id_ = 0;
    bool attached_ = false;
    bool uncertain_ = false;
    bool release_complete_ = false;
    bool detach_complete_ = false;
    bool stopped_ = false;
    ipc::DetachResult last_detach_result_{};
    std::uint64_t last_detach_request_id_ = 0;
    bool has_detach_result_ = false;
};

[[nodiscard]] std::string FormatLifecycleAttachDiagnostic(
    DWORD processId, DWORD threadId, HWND topLevel, std::uint32_t result);
[[nodiscard]] std::string FormatLifecycleUpdateDiagnostic(
    DWORD processId, std::uint64_t requestId,
    const ipc::TabSetUpdate&, const ipc::TabSetResult&);
[[nodiscard]] std::string FormatLifecycleRemoveDiagnostic(
    DWORD processId, std::uint64_t requestId,
    const ipc::WindowRemoveRequest&, const ipc::WindowRemoveResult&);
[[nodiscard]] std::string FormatLifecycleDetachDiagnostic(
    DWORD processId, std::uint64_t requestId, const ipc::DetachResult&,
    std::uint32_t finalResult);

[[nodiscard]] ExplorerSessionOperations CreateProductionExplorerSessionOperations(
    DWORD processId,
    std::wstring hookDllPath,
    std::shared_ptr<LifecycleDiagnosticJournal> diagnosticJournal = {});

}  // namespace winexinfo
