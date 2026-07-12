#pragma once

#include "common/status.h"

#include <Windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace winexinfo::hook {

inline constexpr std::wstring_view kStatusPaneClassName =
    L"WinExInfo.StatusPane";
inline constexpr std::wstring_view kStatusPaneText = L"WinExInfo Gate B";
inline constexpr UINT_PTR kStatusPaneSubclassId = 0x57495831;
inline constexpr UINT kStatusPaneRemoveMessage = WM_APP + 0x571;
inline constexpr UINT kStatusPaneReflowMessage = WM_APP + 0x572;
inline constexpr std::wstring_view kStatusPaneParentClassName =
    L"DUIViewWndClassName";

struct StatusPane final {
    HWND hwnd = nullptr;
    bool subclass_installed = false;
};

struct StatusPaneOperations final {
    std::function<Status(std::wstring_view)> register_class;
    std::function<Status(HWND, std::wstring_view, std::wstring_view, HWND*)>
        create_child;
    std::function<Status(HWND, UINT_PTR)> install_subclass;
    std::function<Status(HWND, UINT_PTR)> remove_subclass;
    std::function<Status(HWND)> destroy_child;
};

struct StatusPanePlacementOperations final {
    std::function<DWORD()> get_current_process_id;
    std::function<DWORD()> get_current_thread_id;
    std::function<DWORD(HWND, DWORD*)> get_window_thread_process_id;
    std::function<Status(HWND, std::wstring*)> get_class_name;
    std::function<Status(HWND, int, LONG_PTR*)> get_window_long_ptr;
    std::function<Status(HWND, int, LONG_PTR)> set_window_long_ptr;
    std::function<Status(HWND, HWND)> set_parent;
    std::function<UINT(HWND)> get_dpi_for_window;
    std::function<Status(HWND, HWND, int, int, int, int, UINT)> set_window_pos;
};

struct StatusPaneReflowState final {
    bool pending = false;
};

using StatusPanePostMessage = std::function<Status(HWND, UINT)>;

[[nodiscard]] Status InstallStatusPane(
    HWND parent,
    const StatusPaneOperations& operations,
    StatusPane* output);
[[nodiscard]] Status RemoveStatusPane(
    const StatusPaneOperations& operations,
    StatusPane* pane);
[[nodiscard]] StatusPaneOperations CreateProductionStatusPaneOperations(
    HMODULE module,
    HANDLE cleanupAck);
[[nodiscard]] Status RequestStatusPaneRemoval(HWND pane) noexcept;
[[nodiscard]] Status ApplyStatusPanePlacement(
    HWND pane,
    HWND expectedParent,
    const RECT& rect,
    bool visible) noexcept;
[[nodiscard]] Status ApplyStatusPanePlacementWithOperations(
    HWND pane,
    HWND expectedParent,
    const RECT& rect,
    bool visible,
    const StatusPanePlacementOperations& operations) noexcept;
[[nodiscard]] Status QueueStatusPaneReflow(
    HWND pane,
    StatusPaneReflowState* state,
    const StatusPanePostMessage& postMessage);
[[nodiscard]] bool ConsumeStatusPaneReflow(StatusPaneReflowState* state) noexcept;

}  // namespace winexinfo::hook
