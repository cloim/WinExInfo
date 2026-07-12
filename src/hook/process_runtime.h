#pragma once

#include "hook/runtime.h"
#include "hook/tab_subclass_set.h"
#include "ipc/protocol.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace winexinfo::hook {

inline constexpr std::size_t kMaximumRuntimeWindows = 64;
inline constexpr WPARAM kProcessRuntimeControlMagic = 0x57495833;
inline constexpr std::wstring_view kProcessRuntimeControlMessageName =
    L"WinExInfo.ProcessControl.v1";

class ProcessRuntime;

struct WindowRuntimeKey final {
    HWND top_level = nullptr;
    DWORD process_id = 0;
    DWORD ui_thread_id = 0;
    std::uint64_t generation = 0;
    bool operator==(const WindowRuntimeKey&) const = default;
};

struct WindowRuntime final {
    WindowRuntimeKey key{};
    std::uint64_t creation_sequence = 0;
    HookCallbackGate callback_gate;
    HookCallbackGate storage_gate;
    std::atomic<unsigned> lifecycle{0}; // active, cleaning, cleaned, failed
    std::unique_ptr<TabSubclassSet> tab_subclasses = std::make_unique<TabSubclassSet>();
    std::shared_ptr<void> resources;
};

class WindowRuntimeCallbackLease final {
public:
    WindowRuntimeCallbackLease() noexcept = default;
    ~WindowRuntimeCallbackLease();
    WindowRuntimeCallbackLease(WindowRuntimeCallbackLease&&) noexcept;
    WindowRuntimeCallbackLease& operator=(WindowRuntimeCallbackLease&&) noexcept;
    WindowRuntimeCallbackLease(const WindowRuntimeCallbackLease&) = delete;
    WindowRuntimeCallbackLease& operator=(const WindowRuntimeCallbackLease&) = delete;
    [[nodiscard]] WindowRuntime* get() const noexcept { return window_; }
    explicit operator bool() const noexcept { return window_ != nullptr; }
private:
    friend WindowRuntimeCallbackLease AcquireProcessWindowCallback(ProcessRuntime&, HWND) noexcept;
    friend WindowRuntimeCallbackLease AcquireProcessWindowCallbackAt(ProcessRuntime&, std::size_t) noexcept;
    explicit WindowRuntimeCallbackLease(WindowRuntime* window) noexcept : window_(window) {}
    WindowRuntime* window_ = nullptr;
};

class WindowRuntimeStorageLease final {
public:
    WindowRuntimeStorageLease() noexcept = default;
    ~WindowRuntimeStorageLease();
    WindowRuntimeStorageLease(WindowRuntimeStorageLease&&) noexcept;
    WindowRuntimeStorageLease& operator=(WindowRuntimeStorageLease&&) noexcept;
    WindowRuntimeStorageLease(const WindowRuntimeStorageLease&) = delete;
    WindowRuntimeStorageLease& operator=(const WindowRuntimeStorageLease&) = delete;
    [[nodiscard]] WindowRuntime* get() const noexcept { return window_; }
    explicit operator bool() const noexcept { return window_ != nullptr; }
private:
    friend WindowRuntimeStorageLease AcquireProcessWindowStorageAt(ProcessRuntime&, std::size_t) noexcept;
    explicit WindowRuntimeStorageLease(WindowRuntime* window) noexcept : window_(window) {}
    WindowRuntime* window_ = nullptr;
};

struct ProcessRuntimeOperations final {
    std::function<DWORD()> get_current_process_id;
    std::function<DWORD()> get_current_thread_id;
    std::function<DWORD(HWND, DWORD*)> get_window_thread_process_id;
    std::function<Status(HWND, UINT, WPARAM, LPARAM, UINT, UINT, DWORD_PTR*)>
        send_control;
    std::function<Status(const WindowRuntimeKey&, std::unique_ptr<WindowRuntime>*)>
        create_window;
    std::function<Status(WindowRuntime&, const ipc::TabSetUpdate&, ipc::TabSetResult*)>
        apply_update;
    std::function<Status(WindowRuntime&)> activate_window;
    std::function<Status(WindowRuntime&)> cleanup_window_on_ui;
    std::function<Status(WindowRuntime&)> cleanup_window_from_worker;
    std::function<Status(const WindowRuntime&, ipc::DetachCleanupProof*)>
        capture_cleanup_proof;
    std::function<Status(WindowRuntime&)> shelter_window_on_destroy;
};

class ProcessRuntime final {
public:
    ProcessRuntime() noexcept;
    ProcessRuntime(const ProcessRuntime&) = delete;
    ProcessRuntime& operator=(const ProcessRuntime&) = delete;

    DWORD process_id = 0;
    UINT control_message = 0;
    ProcessRuntimeOperations operations;

    [[nodiscard]] std::size_t active_window_count() const noexcept;
    [[nodiscard]] std::size_t retained_window_count() const noexcept;
    [[nodiscard]] bool retention_required() const noexcept;

private:
    friend Status RegisterInitialProvisionalProcessWindow(ProcessRuntime&, std::unique_ptr<WindowRuntime>);
    friend Status ActivateAndPublishInitialProcessWindow(ProcessRuntime&, HWND);
    friend WindowRuntimeCallbackLease AcquireProcessWindowCallback(ProcessRuntime&, HWND) noexcept;
    friend WindowRuntimeCallbackLease AcquireProcessWindowCallbackAt(ProcessRuntime&, std::size_t) noexcept;
    friend WindowRuntimeStorageLease AcquireProcessWindowStorageAt(ProcessRuntime&, std::size_t) noexcept;
    friend Status PrepareProcessRuntimeControl(ProcessRuntime&, const ipc::TabSetUpdate&, std::uint64_t);
    friend bool HandleProcessRuntimeControlMessageFor(ProcessRuntime&, HWND, UINT, WPARAM, LPARAM) noexcept;
    friend Status DispatchProcessTabSetUpdate(ProcessRuntime&, const ipc::TabSetUpdate&, ipc::TabSetResult*);
    friend Status DispatchProcessWindowRemoval(ProcessRuntime&, const WindowRuntimeKey&);
    friend Status DispatchProcessWindowRemovalByIdentity(
        ProcessRuntime&, const ipc::WindowRemoveRequest&, ipc::WindowRemoveResult*);
    friend Status ReapRemovedProcessWindows(ProcessRuntime&, DWORD) noexcept;
    friend Status RemoveAllProcessWindows(ProcessRuntime&);
    friend Status FinalizeProcessWindowsAfterDrain(ProcessRuntime&, DWORD) noexcept;
    friend Status CaptureProcessDetachCleanupProof(
        ProcessRuntime&, ipc::DetachCleanupProof*) noexcept;
    friend bool HandleProcessRuntimeTopLevelDestroy(ProcessRuntime&, HWND) noexcept;
    friend Status ProcessRequestedWindowRemovals(ProcessRuntime&);
    friend Status RetireProcessWindowOnCurrentUi(ProcessRuntime&, HWND);

    enum class ControlKind { Update, Remove };
    struct PendingControl final {
        ControlKind kind = ControlKind::Update;
        ipc::TabSetUpdate update;
        ipc::TabSetResult result;
        WindowRuntimeKey key{};
        std::atomic<std::uint64_t> token{0};
        std::atomic<bool> completed{false};
        Status status{};
    };

    std::array<std::unique_ptr<WindowRuntime>, kMaximumRuntimeWindows> slots_{};
    std::array<bool, kMaximumRuntimeWindows> occupied_{};
    std::array<bool, kMaximumRuntimeWindows> removed_{};
    std::array<std::atomic<std::uint64_t>, kMaximumRuntimeWindows> admission_epoch_{};
    std::array<std::atomic<std::uint64_t>, kMaximumRuntimeWindows> storage_epoch_{};
    std::mutex dispatch_mutex_;
    PendingControl pending_;
    std::uint64_t next_token_ = 0;
    std::uint64_t next_creation_sequence_ = 0;
    std::uint64_t next_admission_epoch_ = 0;
    std::atomic<std::size_t> active_count_{0};
    std::atomic<bool> retention_required_{false};
};

[[nodiscard]] Status RegisterInitialProvisionalProcessWindow(ProcessRuntime&, std::unique_ptr<WindowRuntime>);
[[nodiscard]] Status ActivateAndPublishInitialProcessWindow(ProcessRuntime&, HWND);
[[nodiscard]] WindowRuntimeCallbackLease AcquireProcessWindowCallback(ProcessRuntime&, HWND) noexcept;
[[nodiscard]] WindowRuntimeCallbackLease AcquireProcessWindowCallbackAt(ProcessRuntime&, std::size_t) noexcept;
[[nodiscard]] WindowRuntimeStorageLease AcquireProcessWindowStorageAt(ProcessRuntime&, std::size_t) noexcept;
[[nodiscard]] Status PrepareProcessRuntimeControl(ProcessRuntime&, const ipc::TabSetUpdate&, std::uint64_t);
[[nodiscard]] bool HandleProcessRuntimeControlMessageFor(ProcessRuntime&, HWND, UINT, WPARAM, LPARAM) noexcept;
[[nodiscard]] Status DispatchProcessTabSetUpdate(ProcessRuntime&, const ipc::TabSetUpdate&, ipc::TabSetResult*);
[[nodiscard]] Status DispatchProcessWindowRemoval(ProcessRuntime&, const WindowRuntimeKey&);
[[nodiscard]] Status DispatchProcessWindowRemovalByIdentity(
    ProcessRuntime&,
    const ipc::WindowRemoveRequest&,
    ipc::WindowRemoveResult*);
[[nodiscard]] Status ReapRemovedProcessWindows(ProcessRuntime&, DWORD) noexcept;
[[nodiscard]] Status RemoveAllProcessWindows(ProcessRuntime&);
[[nodiscard]] Status FinalizeProcessWindowsAfterDrain(ProcessRuntime&, DWORD) noexcept;
[[nodiscard]] Status CaptureProcessDetachCleanupProof(
    ProcessRuntime&, ipc::DetachCleanupProof*) noexcept;
[[nodiscard]] bool HandleProcessRuntimeTopLevelDestroy(ProcessRuntime&, HWND) noexcept;
[[nodiscard]] Status ProcessRequestedWindowRemovals(ProcessRuntime&);
[[nodiscard]] Status RetireProcessWindowOnCurrentUi(ProcessRuntime&, HWND);
[[nodiscard]] bool HandleProcessRuntimeControlMessage(HWND, UINT, WPARAM, LPARAM) noexcept;
void SetProcessRuntimeForCallbacks(ProcessRuntime*) noexcept;

}  // namespace winexinfo::hook
