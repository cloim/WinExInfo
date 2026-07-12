#pragma once

#include "common/status.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

namespace winexinfo {

struct ExplorerLayoutMetrics final {
    RECT parent_screen{};
    RECT status_screen{};
    RECT left_group_screen{};
    RECT right_group_screen{};
    UINT dpi = 96;
};

enum class ExplorerLayoutUiaScope {
    PaneSubtree,
    StatusBarChildren,
};

struct ExplorerLayoutUiaElementEvidence final {
    ExplorerLayoutUiaScope scope;
    std::wstring framework_id;
    LONG control_type;
    std::wstring automation_id;
    std::wstring class_name;
    void* native_window_handle;
    RECT bounds;
};

struct ExplorerLayoutCaptureEvidence final {
    Status capture_status;
    bool executed_in_mta;
    HWND pane_parent;
    RECT parent_screen;
    UINT dpi;
    std::vector<ExplorerLayoutUiaElementEvidence> elements;
};

using ExplorerLayoutMtaTask = std::function<void(Status)>;

struct ExplorerLayoutCaptureOperations final {
    std::function<Status(HWND, ExplorerLayoutCaptureEvidence*)> capture_current_thread;
    std::function<Status(ExplorerLayoutMtaTask, DWORD)> run_mta_worker;
};

enum class ExplorerLayoutWorkerModuleKind {
    Executable,
    DynamicLibrary,
};

enum class ExplorerLayoutWorkerReleasePath {
    FreeLibraryThenExitThread,
    FreeLibraryAndExitThread,
};

struct ExplorerLayoutWorkerModuleRetention final {
    HMODULE module;
    ExplorerLayoutWorkerModuleKind kind;
};

struct ExplorerLayoutWorkerModuleOperations final {
    void* context;
    Status (*retain)(void*, ExplorerLayoutWorkerModuleRetention*);
    void (*release)(void*, ExplorerLayoutWorkerModuleRetention);
    void (*release_and_exit)(void*, ExplorerLayoutWorkerModuleRetention);
};

inline constexpr DWORD kExplorerLayoutCaptureTimeoutMs = 5000;

[[nodiscard]] Status ComputeStatusPaneRect(
    const ExplorerLayoutMetrics& metrics,
    RECT* output) noexcept;
[[nodiscard]] Status CaptureExplorerLayout(
    HWND topLevel,
    ExplorerLayoutMetrics* metrics,
    HWND* paneParent);
[[nodiscard]] Status ValidateExplorerLayoutCapture(
    const ExplorerLayoutCaptureEvidence& evidence,
    ExplorerLayoutMetrics* metrics,
    HWND* paneParent);
[[nodiscard]] Status CaptureExplorerLayoutWithOperations(
    HWND topLevel,
    ExplorerLayoutMetrics* metrics,
    HWND* paneParent,
    const ExplorerLayoutCaptureOperations& operations);
[[nodiscard]] ExplorerLayoutCaptureOperations GetProductionExplorerLayoutCaptureOperations();
[[nodiscard]] ExplorerLayoutWorkerModuleKind ClassifyExplorerLayoutWorkerModule(
    HMODULE retainedModule,
    HMODULE executableModule) noexcept;
[[nodiscard]] ExplorerLayoutWorkerReleasePath GetExplorerLayoutWorkerReleasePath(
    ExplorerLayoutWorkerModuleKind kind) noexcept;
[[nodiscard]] Status RunExplorerLayoutMtaWorkerWithOperations(
    ExplorerLayoutMtaTask task,
    DWORD timeoutMs,
    const ExplorerLayoutWorkerModuleOperations& moduleOperations);

}  // namespace winexinfo
