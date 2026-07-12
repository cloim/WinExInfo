#include "hook/hook_entry.h"

#include "injection/thread_hook_injector.h"

#include <Windows.h>

namespace winexinfo::hook {

LRESULT ProcessHookCall(
    const int code,
    const WPARAM hookWparam,
    const LPARAM hookLparam,
    const UINT attachMessage,
    const HookEntryOperations& operations) {
    if (!operations.begin_attach || !operations.observe_message ||
        !operations.call_next) {
        return 0;
    }
    if (code == HC_ACTION && hookLparam != 0) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(hookLparam);
        if (attachMessage != 0 && message->hwnd != nullptr &&
            message->message == attachMessage &&
            message->wParam == injection::kAttachMagic &&
            message->lParam > 0) {
            static_cast<void>(operations.begin_attach(
                message->hwnd,
                static_cast<std::uint64_t>(message->lParam)));
        }
        operations.observe_message(message->hwnd, message->message);
    }
    return operations.call_next(code, hookWparam, hookLparam);
}

}  // namespace winexinfo::hook
