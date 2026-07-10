#pragma once

#include "common/status.h"

#include <Windows.h>

#include <functional>
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
    std::vector<HWND> top_level_child_z_order;
};

struct Win32ContractResult final {
    Status status;
    Win32ClassTree class_tree;
    HWND active_shell_tab;
    HWND active_view;
};

struct Win32ProbeOperations final {
    std::function<int(HWND, wchar_t*, int)> get_class_name;
    std::function<BOOL(HWND, RECT*)> get_window_rect;
    std::function<BOOL(HWND)> is_window_visible;
    std::function<BOOL(HWND, WNDENUMPROC, LPARAM)> enum_child_windows;
    std::function<HWND(HWND)> get_parent;
    std::function<void(DWORD)> set_last_error;
    std::function<DWORD()> get_last_error;
    std::function<HWND(HWND)> get_top_window;
    std::function<HWND(HWND)> get_next_window;
};

[[nodiscard]] Win32ContractResult ValidateWin32Contract(const Win32ClassTree& classTree);
[[nodiscard]] Status EnumerateExplorerWindows(std::vector<ExplorerWindowRecord>* output);
[[nodiscard]] Status CaptureWin32ClassTree(HWND topLevel, Win32ClassTree* output);
[[nodiscard]] Status CaptureWin32ClassTreeWithOperations(
    HWND topLevel,
    const Win32ProbeOperations& operations,
    Win32ClassTree* output);

}  // namespace winexinfo
