#pragma once

#include "host/command_line.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string_view>

namespace winexinfo {

struct ExplorerControllerTarget final {
    DWORD process_id = 0;
    DWORD thread_id = 0;
    HWND top_level = nullptr;
};

struct ExplorerControllerOperations final {
    std::function<HostExitCode(HWND, ExplorerControllerTarget*)> inspect_target;
    std::function<void(const ExplorerControllerTarget&)> emit_target_accepted;
    std::function<bool(const ExplorerControllerTarget&)> target_still_matches;
    std::function<HostExitCode(const ExplorerControllerTarget&, std::wstring_view)>
        attach;
    std::function<HostExitCode(const ExplorerControllerTarget&, DWORD*)>
        query_pane_owner;
    std::function<void(std::uint32_t)> wait_duration;
    std::function<HostExitCode(const ExplorerControllerTarget&)> detach;
    std::function<bool(const ExplorerControllerTarget&)> pane_absent;
    std::function<bool(DWORD, std::wstring_view)> exact_module_absent;
    std::function<void(DWORD)> release_retained_target;
};

[[nodiscard]] HostExitCode RunGateCPlacementWithOperations(
    HWND target,
    std::uint32_t durationMs,
    std::wstring_view hookDllPath,
    const ExplorerControllerOperations& operations);

[[nodiscard]] HostExitCode RunGateCPlacement(
    HWND target,
    std::uint32_t durationMs,
    std::wstring_view hookDllPath);

}  // namespace winexinfo
