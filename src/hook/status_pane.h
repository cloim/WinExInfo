#pragma once

#include "common/status.h"

#include <Windows.h>

#include <functional>
#include <string_view>

namespace winexinfo::hook {

inline constexpr std::wstring_view kStatusPaneClassName =
    L"WinExInfo.StatusPane";
inline constexpr std::wstring_view kStatusPaneText = L"WinExInfo Gate B";
inline constexpr UINT_PTR kStatusPaneSubclassId = 0x57495831;
inline constexpr UINT kStatusPaneRemoveMessage = WM_APP + 0x571;

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

}  // namespace winexinfo::hook
