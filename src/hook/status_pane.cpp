#include "hook/status_pane.h"

#include "hook/runtime.h"

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

Status GetClassNameStatus(const HWND window, std::wstring* const output) {
    if (output == nullptr) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    wchar_t name[256]{};
    const int length = GetClassNameW(window, name, 256);
    if (length <= 0) {
        return Failure(GetLastError());
    }
    output->assign(name, static_cast<std::size_t>(length));
    return Success();
}

Status GetWindowLongPtrStatus(
    const HWND window,
    const int index,
    LONG_PTR* const output) noexcept {
    if (output == nullptr) {
        return Failure(ERROR_INVALID_PARAMETER);
    }
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR value = GetWindowLongPtrW(window, index);
    const DWORD error = GetLastError();
    if (value == 0 && error != ERROR_SUCCESS) {
        return Failure(error);
    }
    *output = value;
    return Success();
}

Status SetWindowLongPtrStatus(
    const HWND window,
    const int index,
    const LONG_PTR value) noexcept {
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR prior = SetWindowLongPtrW(window, index, value);
    const DWORD error = GetLastError();
    return prior != 0 || error == ERROR_SUCCESS ? Success() : Failure(error);
}

Status SetParentStatus(const HWND window, const HWND parent) noexcept {
    SetLastError(ERROR_SUCCESS);
    const HWND prior = SetParent(window, parent);
    const DWORD error = GetLastError();
    return prior != nullptr || error == ERROR_SUCCESS ? Success() : Failure(error);
}

Status SetWindowPosStatus(
    const HWND window,
    const HWND after,
    const int x,
    const int y,
    const int width,
    const int height,
    const UINT flags) noexcept {
    return SetWindowPos(window, after, x, y, width, height, flags) != FALSE
        ? Success()
        : Failure(GetLastError());
}

StatusPanePlacementOperations ProductionPlacementOperations() {
    return {
        &GetCurrentProcessId,
        &GetCurrentThreadId,
        &GetWindowThreadProcessId,
        &GetClassNameStatus,
        &GetParent,
        [](const HWND window) { return GetWindow(window, GW_CHILD); },
        [](const HWND window) { return GetWindow(window, GW_HWNDNEXT); },
        [](const HWND window) { return IsWindowVisible(window) != FALSE; },
        &GetWindowLongPtrStatus,
        &SetWindowLongPtrStatus,
        &SetParentStatus,
        &GetDpiForWindow,
        &SetWindowPosStatus,
    };
}

LRESULT CALLBACK StatusPaneWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK StatusPaneSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR id,
    const DWORD_PTR reference) {
    if ((message == kStatusPaneReflowMessage ||
         message == kStatusPaneRuntimeCleanupMessage ||
         message == kStatusPaneTabSetMessage) &&
        id == kStatusPaneSubclassId &&
        HandleStatusPaneRuntimeMessage(window, message)) {
        return 0;
    }
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
                WS_CHILD,
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

Status ApplyStatusPanePlacementWithOperations(
    const HWND pane,
    const HWND expectedParent,
    const RECT& rect,
    const bool visible,
    const StatusPanePlacementOperations& operations) noexcept {
    if (pane == nullptr || expectedParent == nullptr || rect.right < rect.left ||
        rect.bottom < rect.top || !operations.get_current_process_id ||
        !operations.get_current_thread_id ||
        !operations.get_window_thread_process_id || !operations.get_class_name ||
        !operations.get_parent || !operations.get_first_child ||
        !operations.get_next_sibling || !operations.is_window_visible ||
        !operations.get_window_long_ptr || !operations.set_window_long_ptr ||
        !operations.set_parent || !operations.get_dpi_for_window ||
        !operations.set_window_pos) {
        return Failure(ERROR_INVALID_PARAMETER);
    }

    const DWORD currentProcess = operations.get_current_process_id();
    const DWORD currentThread = operations.get_current_thread_id();
    DWORD paneProcess = 0;
    DWORD parentProcess = 0;
    const DWORD paneThread =
        operations.get_window_thread_process_id(pane, &paneProcess);
    const DWORD parentThread =
        operations.get_window_thread_process_id(expectedParent, &parentProcess);
    if (paneThread == 0 || parentThread == 0 || paneProcess != currentProcess ||
        parentProcess != currentProcess || paneThread != currentThread ||
        parentThread != currentThread) {
        return Failure(ERROR_INVALID_WINDOW_HANDLE);
    }

    std::wstring paneClass;
    std::wstring parentClass;
    Status status = operations.get_class_name(pane, &paneClass);
    if (!status.ok()) {
        return status;
    }
    status = operations.get_class_name(expectedParent, &parentClass);
    if (!status.ok()) {
        return status;
    }
    if (paneClass != kStatusPaneClassName ||
        parentClass != kStatusPaneParentClassName) {
        return Failure(ERROR_INVALID_WINDOW_HANDLE);
    }

    LONG_PTR style = 0;
    LONG_PTR exStyle = 0;
    status = operations.get_window_long_ptr(pane, GWL_STYLE, &style);
    if (!status.ok()) {
        return status;
    }
    status = operations.get_window_long_ptr(pane, GWL_EXSTYLE, &exStyle);
    if (!status.ok()) {
        return status;
    }
    status = operations.set_window_long_ptr(pane, GWL_STYLE, style | WS_CHILD);
    if (!status.ok()) {
        return status;
    }
    status = operations.set_window_long_ptr(
        pane, GWL_EXSTYLE, exStyle | WS_EX_NOACTIVATE);
    if (!status.ok()) {
        return status;
    }
    status = operations.set_parent(pane, expectedParent);
    if (!status.ok()) {
        return status;
    }

    const LONG width = rect.right - rect.left;
    const LONG height = rect.bottom - rect.top;
    const UINT dpi = operations.get_dpi_for_window(pane);
    if (dpi == 0) {
        return Failure(ERROR_INVALID_DATA);
    }
    const bool show = visible && width >= static_cast<LONG>(dpi) && height > 0;
    return operations.set_window_pos(
        pane,
        HWND_TOP,
        rect.left,
        rect.top,
        width,
        height,
        SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
}

Status ApplyStatusPanePlacement(
    const HWND pane,
    const HWND expectedParent,
    const RECT& rect,
    const bool visible) noexcept {
    return ApplyStatusPanePlacementWithOperations(
        pane, expectedParent, rect, visible, ProductionPlacementOperations());
}

StatusPanePlacementOperations CreateProductionStatusPanePlacementOperations() {
    return ProductionPlacementOperations();
}

}  // namespace winexinfo::hook
