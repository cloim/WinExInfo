#include "probe/win32_probe.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace winexinfo {

Win32ContractResult ValidateWin32Contract(const Win32ClassTree& classTree) {
    if (!classTree.capture_status.ok()) {
        return {classTree.capture_status, classTree, nullptr, nullptr};
    }

    const Win32ClassNode* selectedShellTab = nullptr;
    for (const HWND zOrderChild : classTree.top_level_child_z_order) {
        const Win32ClassNode* node = nullptr;
        for (const Win32ClassNode& candidate : classTree.nodes) {
            if (candidate.hwnd == zOrderChild) {
                node = &candidate;
                break;
            }
        }
        if (node == nullptr || node->parent != classTree.top_level) {
            return {
                {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
                classTree,
                nullptr,
                nullptr,
            };
        }
        if (node->class_name != L"ShellTabWindowClass") {
            continue;
        }
        if (!node->visible) {
            return {
                {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
                classTree,
                node->hwnd,
                nullptr,
            };
        }
        selectedShellTab = node;
        break;
    }

    if (selectedShellTab == nullptr) {
        return {
            {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
            classTree,
            nullptr,
            nullptr,
        };
    }

    std::vector<HWND> views;
    bool selectedViewVisible = false;
    for (const Win32ClassNode& node : classTree.nodes) {
        if (node.class_name != L"DUIViewWndClassName") {
            continue;
        }

        HWND parent = node.parent;
        while (parent != nullptr && parent != selectedShellTab->hwnd) {
            const Win32ClassNode* parentNode = nullptr;
            for (const Win32ClassNode& candidate : classTree.nodes) {
                if (candidate.hwnd == parent) {
                    parentNode = &candidate;
                    break;
                }
            }
            parent = parentNode == nullptr ? nullptr : parentNode->parent;
        }
        if (parent == selectedShellTab->hwnd) {
            views.push_back(node.hwnd);
            selectedViewVisible = node.visible;
        }
    }

    if (views.size() != 1 || !selectedViewVisible) {
        return {
            {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
            classTree,
            selectedShellTab->hwnd,
            nullptr,
        };
    }

    return {
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        classTree,
        selectedShellTab->hwnd,
        views[0],
    };
}

Status EnumerateExplorerWindows(std::vector<ExplorerWindowRecord>* const output) {
    if (output == nullptr) {
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    struct EnumerationContext final {
        std::vector<ExplorerWindowRecord>* windows;
        Status status;
    };
    EnumerationContext context{
        output,
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
    };
    output->clear();

    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated = EnumWindows(
        [](const HWND hwnd, const LPARAM parameter) -> BOOL {
            auto* const state = reinterpret_cast<EnumerationContext*>(parameter);
            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }

            std::array<wchar_t, 256> className{};
            const int length = GetClassNameW(hwnd, className.data(), static_cast<int>(className.size()));
            if (length == 0) {
                const DWORD error = GetLastError();
                state->status = {
                    ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                    HRESULT_FROM_WIN32(error),
                    error,
                };
                return FALSE;
            }
            if (std::wstring_view{className.data(), static_cast<std::size_t>(length)} !=
                L"CabinetWClass") {
                return TRUE;
            }

            DWORD processId = 0;
            const DWORD threadId = GetWindowThreadProcessId(hwnd, &processId);
            if (threadId == 0 || processId == 0) {
                const DWORD error = GetLastError();
                state->status = {
                    ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                    HRESULT_FROM_WIN32(error),
                    error,
                };
                return FALSE;
            }
            state->windows->push_back({hwnd, processId, threadId});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    if (!enumerated) {
        if (!context.status.ok()) {
            return context.status;
        }
        const DWORD error = GetLastError();
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            HRESULT_FROM_WIN32(error),
            error,
        };
    }

    std::ranges::sort(*output, [](const ExplorerWindowRecord& left, const ExplorerWindowRecord& right) {
        return reinterpret_cast<std::uintptr_t>(left.hwnd) <
            reinterpret_cast<std::uintptr_t>(right.hwnd);
    });
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status CaptureWin32ClassTreeWithOperations(
    const HWND topLevel,
    const Win32ProbeOperations& operations,
    Win32ClassTree* const output) {
    if (output == nullptr || topLevel == nullptr || !operations.get_class_name ||
        !operations.get_window_rect || !operations.is_window_visible ||
        !operations.enum_child_windows || !operations.get_parent ||
        !operations.set_last_error || !operations.get_last_error ||
        !operations.get_top_window || !operations.get_next_window) {
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    Win32ClassTree tree{topLevel, {}, {ErrorCode::OK, S_OK, ERROR_SUCCESS}, {}};
    std::array<wchar_t, 256> topClass{};
    const int topClassLength = operations.get_class_name(
        topLevel, topClass.data(), static_cast<int>(topClass.size()));
    if (topClassLength == 0) {
        const DWORD error = operations.get_last_error();
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            HRESULT_FROM_WIN32(error),
            error,
        };
    }
    RECT topBounds{};
    if (!operations.get_window_rect(topLevel, &topBounds)) {
        const DWORD error = operations.get_last_error();
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            HRESULT_FROM_WIN32(error),
            error,
        };
    }
    tree.nodes.push_back({
        topLevel,
        nullptr,
        std::wstring{topClass.data(), static_cast<std::size_t>(topClassLength)},
        operations.is_window_visible(topLevel) != FALSE,
        topBounds,
    });

    operations.set_last_error(ERROR_SUCCESS);
    HWND zOrderChild = operations.get_top_window(topLevel);
    DWORD zOrderError = zOrderChild == nullptr ? operations.get_last_error() : ERROR_SUCCESS;
    if (zOrderError != ERROR_SUCCESS) {
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            HRESULT_FROM_WIN32(zOrderError),
            zOrderError,
        };
    }
    while (zOrderChild != nullptr) {
        if (std::ranges::find(tree.top_level_child_z_order, zOrderChild) !=
            tree.top_level_child_z_order.end()) {
            return {
                ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                S_FALSE,
                ERROR_SUCCESS,
            };
        }
        tree.top_level_child_z_order.push_back(zOrderChild);
        operations.set_last_error(ERROR_SUCCESS);
        zOrderChild = operations.get_next_window(zOrderChild);
        zOrderError = zOrderChild == nullptr ? operations.get_last_error() : ERROR_SUCCESS;
        if (zOrderError != ERROR_SUCCESS) {
            return {
                ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                HRESULT_FROM_WIN32(zOrderError),
                zOrderError,
            };
        }
    }

    struct TreeContext final {
        Win32ClassTree* tree;
        Status status;
        const Win32ProbeOperations* operations;
    };
    TreeContext context{
        &tree,
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        &operations,
    };
    static_cast<void>(operations.enum_child_windows(
        topLevel,
        [](const HWND hwnd, const LPARAM parameter) -> BOOL {
            auto* const state = reinterpret_cast<TreeContext*>(parameter);
            std::array<wchar_t, 256> className{};
            const int length = state->operations->get_class_name(
                hwnd, className.data(), static_cast<int>(className.size()));
            if (length == 0) {
                const DWORD error = state->operations->get_last_error();
                state->status = {
                    ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                    HRESULT_FROM_WIN32(error),
                    error,
                };
                return FALSE;
            }
            RECT bounds{};
            if (!state->operations->get_window_rect(hwnd, &bounds)) {
                const DWORD error = state->operations->get_last_error();
                state->status = {
                    ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                    HRESULT_FROM_WIN32(error),
                    error,
                };
                return FALSE;
            }
            state->operations->set_last_error(ERROR_SUCCESS);
            const HWND parent = state->operations->get_parent(hwnd);
            if (parent == nullptr) {
                const DWORD error = state->operations->get_last_error();
                if (error != ERROR_SUCCESS) {
                    state->status = {
                        ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                        HRESULT_FROM_WIN32(error),
                        error,
                    };
                    return FALSE;
                }
            }
            state->tree->nodes.push_back({
                hwnd,
                parent,
                std::wstring{className.data(), static_cast<std::size_t>(length)},
                state->operations->is_window_visible(hwnd) != FALSE,
                bounds,
            });
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context)));
    tree.capture_status = context.status;
    if (!context.status.ok()) {
        *output = std::move(tree);
        return context.status;
    }

    std::vector<HWND> confirmedZOrder;
    operations.set_last_error(ERROR_SUCCESS);
    zOrderChild = operations.get_top_window(topLevel);
    zOrderError = zOrderChild == nullptr ? operations.get_last_error() : ERROR_SUCCESS;
    if (zOrderError != ERROR_SUCCESS) {
        tree.capture_status = {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            HRESULT_FROM_WIN32(zOrderError),
            zOrderError,
        };
        *output = std::move(tree);
        return output->capture_status;
    }
    while (zOrderChild != nullptr) {
        if (std::ranges::find(confirmedZOrder, zOrderChild) != confirmedZOrder.end()) {
            tree.capture_status = {
                ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                S_FALSE,
                ERROR_SUCCESS,
            };
            *output = std::move(tree);
            return output->capture_status;
        }
        confirmedZOrder.push_back(zOrderChild);
        operations.set_last_error(ERROR_SUCCESS);
        zOrderChild = operations.get_next_window(zOrderChild);
        zOrderError = zOrderChild == nullptr ? operations.get_last_error() : ERROR_SUCCESS;
        if (zOrderError != ERROR_SUCCESS) {
            tree.capture_status = {
                ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                HRESULT_FROM_WIN32(zOrderError),
                zOrderError,
            };
            *output = std::move(tree);
            return output->capture_status;
        }
    }
    if (confirmedZOrder != tree.top_level_child_z_order) {
        tree.capture_status = {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            S_FALSE,
            ERROR_SUCCESS,
        };
        *output = std::move(tree);
        return output->capture_status;
    }

    std::ranges::sort(tree.nodes.begin() + 1, tree.nodes.end(), [](const auto& left, const auto& right) {
        return reinterpret_cast<std::uintptr_t>(left.hwnd) <
            reinterpret_cast<std::uintptr_t>(right.hwnd);
    });
    *output = std::move(tree);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status CaptureWin32ClassTree(const HWND topLevel, Win32ClassTree* const output) {
    const Win32ProbeOperations operations{
        [](const HWND hwnd, wchar_t* const buffer, const int capacity) {
            return GetClassNameW(hwnd, buffer, capacity);
        },
        [](const HWND hwnd, RECT* const bounds) { return GetWindowRect(hwnd, bounds); },
        [](const HWND hwnd) { return IsWindowVisible(hwnd); },
        [](const HWND hwnd, const WNDENUMPROC callback, const LPARAM parameter) {
            return EnumChildWindows(hwnd, callback, parameter);
        },
        [](const HWND hwnd) { return GetParent(hwnd); },
        [](const DWORD error) { SetLastError(error); },
        []() { return GetLastError(); },
        [](const HWND hwnd) { return GetTopWindow(hwnd); },
        [](const HWND hwnd) { return GetWindow(hwnd, GW_HWNDNEXT); },
    };
    return CaptureWin32ClassTreeWithOperations(topLevel, operations, output);
}

}  // namespace winexinfo
