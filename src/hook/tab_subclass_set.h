#pragma once

#include "common/status.h"
#include "ipc/protocol.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace winexinfo::hook {

inline constexpr UINT_PTR kTabSubclassId = 0x57495832;
inline constexpr std::size_t kMaximumTabDirectChildren = 4096;

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

class TabSubclassSet final {
public:
    TabSubclassSet();
    ~TabSubclassSet();
    TabSubclassSet(const TabSubclassSet&) = delete;
    TabSubclassSet& operator=(const TabSubclassSet&) = delete;

    [[nodiscard]] Status Apply(
        HWND topLevel,
        const ipc::TabSetUpdate& update,
        const TabSubclassOperations& operations,
        ipc::TabSetResult* result);
    [[nodiscard]] Status RemoveAll(const TabSubclassOperations& operations);
    [[nodiscard]] bool cleanup_safe() const noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept;
    [[nodiscard]] bool Matches(
        HWND topLevel, std::uint64_t topGeneration,
        HWND tab, std::uint64_t tabGeneration) const noexcept;
    void Notify(HWND window, UINT message) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] TabSubclassOperations CreateProductionTabSubclassOperations(
    TabSubclassSet& owner);

}  // namespace winexinfo::hook
