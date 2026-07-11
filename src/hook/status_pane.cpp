#include "hook/status_pane.h"

#include <CommCtrl.h>

#include <string>

namespace winexinfo::hook {
namespace {

Status Success() noexcept {
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status Failure(const DWORD error = ERROR_INVALID_STATE) noexcept {
    return {ErrorCode::WINDOW_ATTACH_FAILED, HRESULT_FROM_WIN32(error), error};
}

LRESULT CALLBACK StatusPaneWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK StatusPaneSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR id,
    const DWORD_PTR reference) {
    if (message == kStatusPaneRemoveMessage && id == kStatusPaneSubclassId) {
        const HANDLE ack = reinterpret_cast<HANDLE>(reference);
        const BOOL removed = RemoveWindowSubclass(
            window, StatusPaneSubclassProc, kStatusPaneSubclassId);
        const BOOL destroyed = removed != FALSE ? DestroyWindow(window) : FALSE;
        if (destroyed != FALSE && ack != nullptr) {
            SetEvent(ack);
        }
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

}  // namespace

Status InstallStatusPane(
    const HWND parent,
    const StatusPaneOperations& operations,
    StatusPane* const output) {
    if (parent == nullptr || output == nullptr || output->hwnd != nullptr ||
        !operations.register_class || !operations.create_child ||
        !operations.install_subclass || !operations.remove_subclass ||
        !operations.destroy_child) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    Status status = operations.register_class(kStatusPaneClassName);
    if (!status.ok()) {
        return status;
    }
    HWND child = nullptr;
    status = operations.create_child(
        parent, kStatusPaneClassName, kStatusPaneText, &child);
    if (!status.ok() || child == nullptr) {
        return status.ok() ? Failure() : status;
    }
    status = operations.install_subclass(child, kStatusPaneSubclassId);
    if (!status.ok()) {
        static_cast<void>(operations.destroy_child(child));
        return status;
    }
    *output = {child, true};
    return Success();
}

Status RemoveStatusPane(
    const StatusPaneOperations& operations,
    StatusPane* const pane) {
    if (pane == nullptr || pane->hwnd == nullptr || !pane->subclass_installed ||
        !operations.remove_subclass || !operations.destroy_child) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    const HWND child = pane->hwnd;
    Status status = operations.remove_subclass(child, kStatusPaneSubclassId);
    if (!status.ok()) {
        return status;
    }
    pane->subclass_installed = false;
    status = operations.destroy_child(child);
    if (!status.ok()) {
        return status;
    }
    pane->hwnd = nullptr;
    return Success();
}

StatusPaneOperations CreateProductionStatusPaneOperations(
    const HMODULE module,
    const HANDLE cleanupAck) {
    return {
        [module](const std::wstring_view className) {
            const std::wstring name{className};
            WNDCLASSEXW windowClass{
                sizeof(WNDCLASSEXW),
                0,
                StatusPaneWindowProc,
                0,
                0,
                module,
                nullptr,
                nullptr,
                reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1),
                nullptr,
                name.c_str(),
                nullptr,
            };
            const ATOM atom = RegisterClassExW(&windowClass);
            if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return Failure(GetLastError());
            }
            return Success();
        },
        [module](
            const HWND parent,
            const std::wstring_view className,
            const std::wstring_view text,
            HWND* const output) {
            if (output == nullptr) {
                return Failure(ERROR_INVALID_PARAMETER);
            }
            const std::wstring classValue{className};
            const std::wstring textValue{text};
            const HWND child = CreateWindowExW(
                0,
                classValue.c_str(),
                textValue.c_str(),
                WS_CHILD | WS_VISIBLE,
                0,
                0,
                180,
                24,
                parent,
                nullptr,
                module,
                nullptr);
            if (child == nullptr) {
                return Failure(GetLastError());
            }
            *output = child;
            return Success();
        },
        [cleanupAck](const HWND child, const UINT_PTR id) {
            return SetWindowSubclass(
                       child,
                       StatusPaneSubclassProc,
                       id,
                       reinterpret_cast<DWORD_PTR>(cleanupAck)) != FALSE
                ? Success()
                : Failure(GetLastError());
        },
        [](const HWND child, const UINT_PTR id) {
            return RemoveWindowSubclass(child, StatusPaneSubclassProc, id) != FALSE
                ? Success()
                : Failure(GetLastError());
        },
        [](const HWND child) {
            return DestroyWindow(child) != FALSE
                ? Success()
                : Failure(GetLastError());
        },
    };
}

Status RequestStatusPaneRemoval(const HWND pane) noexcept {
    if (pane == nullptr) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    return PostMessageW(pane, kStatusPaneRemoveMessage, 0, 0) != FALSE
        ? Success()
        : Failure(GetLastError());
}

}  // namespace winexinfo::hook
