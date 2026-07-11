#pragma once

#include "common/status.h"

#include <Windows.h>

#include <cstdint>
#include <functional>

namespace winexinfo::hook {

struct HookEntryOperations final {
    std::function<Status(HWND, std::uint64_t)> begin_attach;
    std::function<LRESULT(int, WPARAM, LPARAM)> call_next;
};

[[nodiscard]] LRESULT ProcessHookCall(
    int code,
    WPARAM hookWparam,
    LPARAM hookLparam,
    UINT attachMessage,
    const HookEntryOperations& operations);

}  // namespace winexinfo::hook
