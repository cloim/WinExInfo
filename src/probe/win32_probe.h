#pragma once

#include "common/status.h"

#include <Windows.h>

#include <string>
#include <vector>

namespace winexinfo {

struct ExplorerWindowRecord final {
    HWND hwnd;
    DWORD process_id;
    DWORD thread_id;
};

struct Win32ClassNode final {
    HWND hwnd;
    HWND parent;
    std::wstring class_name;
    bool visible;
    RECT bounds;
};

struct Win32ClassTree final {
    HWND top_level;
    std::vector<Win32ClassNode> nodes;
    Status capture_status;
};

struct Win32ContractResult final {
    Status status;
    Win32ClassTree class_tree;
    HWND active_shell_tab;
    HWND active_view;
};

[[nodiscard]] Win32ContractResult ValidateWin32Contract(const Win32ClassTree& classTree);
[[nodiscard]] Status EnumerateExplorerWindows(std::vector<ExplorerWindowRecord>* output);
[[nodiscard]] Status CaptureWin32ClassTree(HWND topLevel, Win32ClassTree* output);

}  // namespace winexinfo
