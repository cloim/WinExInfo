#pragma once

#include "common/status.h"
#include "probe/probe_types.h"

#include <Windows.h>
#include <ShObjIdl_core.h>
#include <ExDisp.h>
#include <wrl/client.h>

#include <span>
#include <string>
#include <vector>

namespace winexinfo {

struct ShellViewEntryEvidence final {
    HWND browser_top_level;
    HWND shell_browser;
    HWND active_view;
    bool active_view_visible;
    bool active_view_descendant;
    bool filesystem_path_available;
    std::wstring filesystem_path;
};

struct ActiveShellViewEvidence final {
    Status capture_status;
    ShellProbeTerminalStage terminal_stage;
    std::vector<ShellViewEntryEvidence> entries;
};

struct ShellBrowserEntryCapture final {
    Microsoft::WRL::ComPtr<IUnknown> canonical_identity;
    Microsoft::WRL::ComPtr<IWebBrowser2> browser;
    Microsoft::WRL::ComPtr<IShellBrowser> shell_browser;
    bool target_matched;
    HWND top_level;
    HWND shell_tab;
};

struct ShellBrowserSetCapture final {
    Status status;
    ShellProbeTerminalStage terminal_stage;
    DWORD owner_thread_id;
    std::vector<ShellBrowserEntryCapture> entries;
};

[[nodiscard]] Status ClassifyFilesystemAttributes(
    HRESULT hresult,
    ULONG attributes,
    bool* available);
[[nodiscard]] Status ClassifyShellWindowsItemResult(
    HRESULT hresult,
    bool hasDispatch,
    bool* useEntry);
[[nodiscard]] Status ClassifyBrowserInterfaceResult(
    HRESULT hresult,
    bool hasBrowser,
    bool* useEntry);
[[nodiscard]] Status ClassifyRequiredComObjectResult(
    HRESULT hresult,
    bool hasObject);
[[nodiscard]] Status ValidateActiveShellViewEvidence(
    HWND topLevel,
    HWND selectedShellTab,
    const ActiveShellViewEvidence& evidence,
    ActiveShellViewSnapshot* output);
[[nodiscard]] Status CaptureShellBrowserSet(
    IShellWindows* shellWindows,
    std::span<const HWND> targetTopLevels,
    ShellBrowserSetCapture* output);
[[nodiscard]] Status CaptureActiveShellViewFromBrowserSet(
    const ShellBrowserSetCapture& browserSet,
    HWND topLevel,
    HWND selectedShellTab,
    ActiveShellViewSnapshot* output);
[[nodiscard]] Status CaptureActiveShellView(
    HWND topLevel,
    HWND selectedShellTab,
    ActiveShellViewSnapshot* output);

}  // namespace winexinfo
