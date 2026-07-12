#pragma once

#include "common/status.h"
#include "ipc/protocol.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace winexinfo::hook {

inline constexpr UINT_PTR kTabSubclassId = 0x57495832;

struct TabSubclassOperations final {
    std::function<DWORD()> get_current_thread_id;
    std::function<DWORD(HWND, DWORD*)> get_window_thread_process_id;
    std::function<Status(HWND, std::wstring*)> get_class_name;
    std::function<HWND(HWND)> get_parent;
    std::function<HWND(HWND)> get_first_child;
    std::function<HWND(HWND)> get_next_sibling;
    std::function<Status(HWND, UINT_PTR, std::uint64_t)> install_subclass;
    std::function<Status(HWND, UINT_PTR)> remove_subclass;
    std::function<Status()> post_refresh;
};

[[nodiscard]] Status ApplyTabSetUpdate(
    HWND topLevel,
    const ipc::TabSetUpdate& update,
    const TabSubclassOperations& operations,
    ipc::TabSetResult* result);
[[nodiscard]] Status RemoveAllTabSubclasses(
    const TabSubclassOperations& operations);
[[nodiscard]] TabSubclassOperations CreateProductionTabSubclassOperations();
void NotifyTabSubclassMessage(HWND window, UINT message) noexcept;

}  // namespace winexinfo::hook
