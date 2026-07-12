#pragma once

#include "host/command_line.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace winexinfo {

struct ExplorerControllerTarget final {
    DWORD process_id = 0;
    DWORD thread_id = 0;
    HWND top_level = nullptr;
};

struct ExplorerControllerFileIdentity final {
    std::uint64_t volume_serial = 0;
    std::array<std::uint8_t, 16> file_id{};

    bool operator==(const ExplorerControllerFileIdentity&) const = default;
};

struct ExplorerControllerOperations final {
    std::function<HostExitCode(HWND, ExplorerControllerTarget*)> inspect_target;
    std::function<void(const ExplorerControllerTarget&)> emit_target_accepted;
    std::function<HostExitCode(
        const ExplorerControllerTarget&,
        std::wstring_view,
        const std::function<void()>&,
        bool*)> attach;
    std::function<HostExitCode(const ExplorerControllerTarget&, DWORD*)>
        query_pane_owner;
    std::function<void(std::uint32_t)> wait_duration;
    std::function<HostExitCode(const ExplorerControllerTarget&)> detach;
    std::function<HostExitCode(const ExplorerControllerTarget&, bool*)> pane_absent;
    std::function<HostExitCode(DWORD, bool*)> exact_module_absent;
    std::function<HostExitCode(DWORD)> release_retained_target;
    std::shared_ptr<void> retention_context;
};

[[nodiscard]] HostExitCode CaptureExplorerControllerFileIdentity(
    std::wstring_view path,
    ExplorerControllerFileIdentity* output);
[[nodiscard]] HostExitCode CheckExplorerControllerFileIdentity(
    std::wstring_view path,
    const ExplorerControllerFileIdentity& expected,
    bool* matches);

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
