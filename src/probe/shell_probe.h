#pragma once

#include "common/status.h"
#include "probe/probe_types.h"

#include <Windows.h>

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
    std::vector<ShellViewEntryEvidence> entries;
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
[[nodiscard]] Status CaptureActiveShellView(
    HWND topLevel,
    HWND selectedShellTab,
    ActiveShellViewSnapshot* output);

}  // namespace winexinfo
