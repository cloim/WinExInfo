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
    std::vector<HWND> shellTabs;
    for (const Win32ClassNode& node : classTree.nodes) {
        if (node.class_name != L"ShellTabWindowClass" || !node.visible) {
            continue;
        }

        HWND parent = node.parent;
        while (parent != nullptr && parent != classTree.top_level) {
            const Win32ClassNode* parentNode = nullptr;
            for (const Win32ClassNode& candidate : classTree.nodes) {
                if (candidate.hwnd == parent) {
                    parentNode = &candidate;
                    break;
                }
            }
            parent = parentNode == nullptr ? nullptr : parentNode->parent;
        }
        if (parent == classTree.top_level) {
            shellTabs.push_back(node.hwnd);
        }
    }

    if (shellTabs.size() != 1) {
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
        while (parent != nullptr && parent != shellTabs[0]) {
            const Win32ClassNode* parentNode = nullptr;
            for (const Win32ClassNode& candidate : classTree.nodes) {
                if (candidate.hwnd == parent) {
                    parentNode = &candidate;
                    break;
                }
            }
            parent = parentNode == nullptr ? nullptr : parentNode->parent;
        }
        if (parent == shellTabs[0]) {
            views.push_back(node.hwnd);
            selectedViewVisible = node.visible;
        }
    }

    if (views.size() != 1 || !selectedViewVisible) {
        return {
            {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
            classTree,
            shellTabs[0],
            nullptr,
        };
    }

    return {
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        classTree,
        shellTabs[0],
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

Status CaptureWin32ClassTree(const HWND topLevel, Win32ClassTree* const output) {
    if (output == nullptr || topLevel == nullptr) {
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    Win32ClassTree tree{topLevel, {}};
    std::array<wchar_t, 256> topClass{};
    const int topClassLength =
        GetClassNameW(topLevel, topClass.data(), static_cast<int>(topClass.size()));
    RECT topBounds{};
    if (topClassLength == 0 || !GetWindowRect(topLevel, &topBounds)) {
        const DWORD error = GetLastError();
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
        IsWindowVisible(topLevel) != FALSE,
        topBounds,
    });

    struct TreeContext final {
        Win32ClassTree* tree;
        Status status;
    };
    TreeContext context{&tree, {ErrorCode::OK, S_OK, ERROR_SUCCESS}};
    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated = EnumChildWindows(
        topLevel,
        [](const HWND hwnd, const LPARAM parameter) -> BOOL {
            auto* const state = reinterpret_cast<TreeContext*>(parameter);
            std::array<wchar_t, 256> className{};
            const int length = GetClassNameW(hwnd, className.data(), static_cast<int>(className.size()));
            RECT bounds{};
            if (length == 0 || !GetWindowRect(hwnd, &bounds)) {
                const DWORD error = GetLastError();
                state->status = {
                    ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                    HRESULT_FROM_WIN32(error),
                    error,
                };
                return FALSE;
            }
            state->tree->nodes.push_back({
                hwnd,
                GetParent(hwnd),
                std::wstring{className.data(), static_cast<std::size_t>(length)},
                IsWindowVisible(hwnd) != FALSE,
                bounds,
            });
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

    std::ranges::sort(tree.nodes.begin() + 1, tree.nodes.end(), [](const auto& left, const auto& right) {
        return reinterpret_cast<std::uintptr_t>(left.hwnd) <
            reinterpret_cast<std::uintptr_t>(right.hwnd);
    });
    *output = std::move(tree);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace winexinfo
