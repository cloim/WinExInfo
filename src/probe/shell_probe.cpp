#include "probe/shell_probe.h"

#include <Windows.h>
#include <ShObjIdl_core.h>
#include <ExDisp.h>
#include <ShlGuid.h>
#include <servprov.h>
#include <wrl/client.h>

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace winexinfo {
namespace {

Status ShellFailure(const HRESULT hresult) {
    return {
        ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        hresult,
        HRESULT_FACILITY(hresult) == FACILITY_WIN32
            ? static_cast<DWORD>(HRESULT_CODE(hresult))
            : ERROR_SUCCESS,
    };
}

}  // namespace

Status ClassifyFilesystemAttributes(
    const HRESULT hresult,
    const ULONG attributes,
    bool* const available) {
    if (available == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }
    if (hresult == S_OK && (attributes & SFGAO_FILESYSTEM) != 0) {
        *available = true;
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (hresult == S_FALSE && (attributes & SFGAO_FILESYSTEM) == 0) {
        *available = false;
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (FAILED(hresult)) {
        return ShellFailure(hresult);
    }
    return ShellFailure(S_FALSE);
}

Status ClassifyShellWindowsItemResult(
    const HRESULT hresult,
    const bool hasDispatch,
    bool* const useEntry) {
    if (useEntry == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }
    if (hresult == S_OK && hasDispatch) {
        *useEntry = true;
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (hresult == S_FALSE && !hasDispatch) {
        *useEntry = false;
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (FAILED(hresult)) {
        return ShellFailure(hresult);
    }
    return ShellFailure(S_FALSE);
}

Status ClassifyBrowserInterfaceResult(
    const HRESULT hresult,
    const bool hasBrowser,
    bool* const useEntry) {
    if (useEntry == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }
    if (hresult == S_OK && hasBrowser) {
        *useEntry = true;
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (hresult == E_NOINTERFACE && !hasBrowser) {
        *useEntry = false;
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (FAILED(hresult)) {
        return ShellFailure(hresult);
    }
    return ShellFailure(S_FALSE);
}

Status ClassifyRequiredComObjectResult(
    const HRESULT hresult,
    const bool hasObject) {
    if (hresult == S_OK && hasObject) {
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (FAILED(hresult)) {
        return ShellFailure(hresult);
    }
    return ShellFailure(S_FALSE);
}

Status ValidateActiveShellViewEvidence(
    const HWND topLevel,
    const HWND selectedShellTab,
    const ActiveShellViewEvidence& evidence,
    ActiveShellViewSnapshot* const output) {
    if (output == nullptr || topLevel == nullptr || selectedShellTab == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    ActiveShellViewSnapshot snapshot{
        {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS},
        ShellProbeTerminalStage::ValidateActiveView,
        0,
        0,
        0,
        nullptr,
        false,
        {},
    };
    bool invalidPathState = false;
    for (const ShellViewEntryEvidence& entry : evidence.entries) {
        if (entry.browser_top_level != topLevel) {
            continue;
        }
        ++snapshot.top_level_entry_count;
        if (entry.shell_browser != selectedShellTab) {
            continue;
        }
        ++snapshot.shell_tab_match_count;
        if (entry.active_view == nullptr || !entry.active_view_visible ||
            !entry.active_view_descendant) {
            continue;
        }
        ++snapshot.active_view_count;
        if ((entry.filesystem_path_available && entry.filesystem_path.empty()) ||
            (!entry.filesystem_path_available && !entry.filesystem_path.empty())) {
            invalidPathState = true;
        }
        if (snapshot.active_view_count == 1) {
            snapshot.active_view = entry.active_view;
            snapshot.filesystem_path_available = entry.filesystem_path_available;
            snapshot.filesystem_path = entry.filesystem_path;
        }
    }

    if (!evidence.capture_status.ok()) {
        snapshot.status = evidence.capture_status;
        snapshot.terminal_stage = FAILED(evidence.capture_status.hresult)
            ? evidence.terminal_stage
            : ShellProbeTerminalStage::ValidateActiveView;
    } else if (snapshot.shell_tab_match_count == 1 &&
               snapshot.active_view_count == 1 && !invalidPathState) {
        snapshot.status = {ErrorCode::OK, S_OK, ERROR_SUCCESS};
        snapshot.terminal_stage = ShellProbeTerminalStage::Complete;
    }
    if (!snapshot.status.ok()) {
        snapshot.active_view = nullptr;
        snapshot.filesystem_path_available = false;
        snapshot.filesystem_path.clear();
    }
    *output = std::move(snapshot);
    return output->status;
}

Status CaptureActiveShellView(
    const HWND topLevel,
    const HWND selectedShellTab,
    ActiveShellViewSnapshot* const output) {
    if (output == nullptr || topLevel == nullptr || selectedShellTab == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    ActiveShellViewEvidence evidence{
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        ShellProbeTerminalStage::NotStarted,
        {},
    };
    Microsoft::WRL::ComPtr<IShellView> selectedShellView;
    std::size_t selectedEntryIndex = 0;
    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    evidence.terminal_stage = ShellProbeTerminalStage::CoCreateShellWindows;
    HRESULT hresult = CoCreateInstance(
        CLSID_ShellWindows,
        nullptr,
        CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&shellWindows));
    Status requiredStatus =
        ClassifyRequiredComObjectResult(hresult, shellWindows != nullptr);
    if (!requiredStatus.ok()) {
        evidence.capture_status = requiredStatus;
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }

    long count = 0;
    evidence.terminal_stage = ShellProbeTerminalStage::IShellWindowsGetCount;
    hresult = shellWindows->get_Count(&count);
    if (hresult != S_OK) {
        evidence.capture_status = ShellFailure(hresult);
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }
    if (count < 0) {
        evidence.capture_status = ShellFailure(S_FALSE);
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }

    for (long indexValue = 0; indexValue < count; ++indexValue) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = indexValue;
        Microsoft::WRL::ComPtr<IDispatch> dispatch;
        evidence.terminal_stage = ShellProbeTerminalStage::IShellWindowsItem;
        hresult = shellWindows->Item(index, &dispatch);
        bool useEntry = false;
        const Status itemStatus = ClassifyShellWindowsItemResult(
            hresult, dispatch != nullptr, &useEntry);
        if (!itemStatus.ok()) {
            evidence.capture_status = itemStatus;
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }
        if (!useEntry) {
            continue;
        }

        Microsoft::WRL::ComPtr<IWebBrowser2> browser;
        evidence.terminal_stage =
            ShellProbeTerminalStage::IDispatchQueryIWebBrowser2;
        hresult = dispatch.As(&browser);
        const Status browserStatus = ClassifyBrowserInterfaceResult(
            hresult, browser != nullptr, &useEntry);
        if (!browserStatus.ok()) {
            evidence.capture_status = browserStatus;
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }
        if (!useEntry) {
            continue;
        }

        SHANDLE_PTR browserHandle = 0;
        evidence.terminal_stage = ShellProbeTerminalStage::IWebBrowser2GetHwnd;
        hresult = browser->get_HWND(&browserHandle);
        if (hresult != S_OK) {
            evidence.capture_status = ShellFailure(hresult);
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }
        const HWND browserTopLevel = reinterpret_cast<HWND>(browserHandle);
        if (browserTopLevel != topLevel) {
            continue;
        }

        ShellViewEntryEvidence entry{
            browserTopLevel,
            nullptr,
            nullptr,
            false,
            false,
            false,
            {},
        };
        Microsoft::WRL::ComPtr<IServiceProvider> serviceProvider;
        evidence.terminal_stage =
            ShellProbeTerminalStage::IWebBrowser2QueryIServiceProvider;
        hresult = browser.As(&serviceProvider);
        requiredStatus =
            ClassifyRequiredComObjectResult(hresult, serviceProvider != nullptr);
        if (!requiredStatus.ok()) {
            evidence.capture_status = requiredStatus;
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }

        Microsoft::WRL::ComPtr<IShellBrowser> shellBrowser;
        evidence.terminal_stage =
            ShellProbeTerminalStage::IServiceProviderQueryTopLevelBrowser;
        hresult = serviceProvider->QueryService(
            SID_STopLevelBrowser,
            IID_PPV_ARGS(&shellBrowser));
        requiredStatus =
            ClassifyRequiredComObjectResult(hresult, shellBrowser != nullptr);
        if (!requiredStatus.ok()) {
            evidence.capture_status = requiredStatus;
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }

        evidence.terminal_stage = ShellProbeTerminalStage::IShellBrowserGetWindow;
        hresult = shellBrowser->GetWindow(&entry.shell_browser);
        if (hresult != S_OK) {
            evidence.capture_status = ShellFailure(hresult);
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }
        if (entry.shell_browser != selectedShellTab) {
            evidence.entries.push_back(std::move(entry));
            continue;
        }

        Microsoft::WRL::ComPtr<IShellView> shellView;
        evidence.terminal_stage =
            ShellProbeTerminalStage::IShellBrowserQueryActiveShellView;
        hresult = shellBrowser->QueryActiveShellView(&shellView);
        requiredStatus =
            ClassifyRequiredComObjectResult(hresult, shellView != nullptr);
        if (!requiredStatus.ok()) {
            evidence.capture_status = requiredStatus;
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }

        evidence.terminal_stage = ShellProbeTerminalStage::IShellViewGetWindow;
        hresult = shellView->GetWindow(&entry.active_view);
        if (hresult != S_OK) {
            evidence.capture_status = ShellFailure(hresult);
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }
        entry.active_view_visible =
            entry.active_view != nullptr && IsWindowVisible(entry.active_view) != FALSE;
        entry.active_view_descendant =
            entry.active_view != nullptr && IsChild(selectedShellTab, entry.active_view) != FALSE;
        if (entry.active_view_visible && entry.active_view_descendant) {
            if (selectedShellView == nullptr) {
                selectedShellView = shellView;
                selectedEntryIndex = evidence.entries.size();
            }
        }
        evidence.entries.push_back(std::move(entry));
    }

    evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
    const Status mappingStatus =
        ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    if (!mappingStatus.ok()) {
        return mappingStatus;
    }
    if (selectedShellView == nullptr || selectedEntryIndex >= evidence.entries.size()) {
        evidence.capture_status = ShellFailure(S_FALSE);
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }

    ShellViewEntryEvidence& selectedEntry = evidence.entries[selectedEntryIndex];
    Microsoft::WRL::ComPtr<IFolderView> folderView;
    evidence.terminal_stage = ShellProbeTerminalStage::IShellViewQueryIFolderView;
    hresult = selectedShellView.As(&folderView);
    requiredStatus = ClassifyRequiredComObjectResult(hresult, folderView != nullptr);
    if (!requiredStatus.ok()) {
        evidence.capture_status = requiredStatus;
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }

    Microsoft::WRL::ComPtr<IShellFolder> folder;
    evidence.terminal_stage = ShellProbeTerminalStage::IFolderViewGetFolder;
    hresult = folderView->GetFolder(IID_PPV_ARGS(&folder));
    requiredStatus = ClassifyRequiredComObjectResult(hresult, folder != nullptr);
    if (!requiredStatus.ok()) {
        evidence.capture_status = requiredStatus;
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }

    PIDLIST_ABSOLUTE rawPidl = nullptr;
    evidence.terminal_stage = ShellProbeTerminalStage::ShGetIdListFromObject;
    hresult = SHGetIDListFromObject(folder.Get(), &rawPidl);
    if (hresult != S_OK || rawPidl == nullptr) {
        if (rawPidl != nullptr) {
            CoTaskMemFree(rawPidl);
        }
        evidence.capture_status =
            hresult == S_OK ? ShellFailure(S_FALSE) : ShellFailure(hresult);
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }
    struct PidlDeleter final {
        using pointer = PIDLIST_ABSOLUTE;

        void operator()(const PIDLIST_ABSOLUTE value) const noexcept {
            CoTaskMemFree(value);
        }
    };
    std::unique_ptr<ITEMIDLIST, PidlDeleter> pidl(rawPidl);

    Microsoft::WRL::ComPtr<IShellItem> item;
    evidence.terminal_stage = ShellProbeTerminalStage::ShCreateItemFromIdList;
    hresult = SHCreateItemFromIDList(pidl.get(), IID_PPV_ARGS(&item));
    requiredStatus = ClassifyRequiredComObjectResult(hresult, item != nullptr);
    if (!requiredStatus.ok()) {
        evidence.capture_status = requiredStatus;
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }

    SFGAOF attributes = 0;
    evidence.terminal_stage = ShellProbeTerminalStage::IShellItemGetAttributes;
    hresult = item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
    bool filesystem = false;
    const Status attributeStatus =
        ClassifyFilesystemAttributes(hresult, attributes, &filesystem);
    if (!attributeStatus.ok()) {
        evidence.capture_status = attributeStatus;
        return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
    }
    if (filesystem) {
        PWSTR rawPath = nullptr;
        evidence.terminal_stage = ShellProbeTerminalStage::IShellItemGetDisplayName;
        hresult = item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath);
        if (hresult != S_OK || rawPath == nullptr || rawPath[0] == L'\0') {
            if (rawPath != nullptr) {
                CoTaskMemFree(rawPath);
            }
            evidence.capture_status =
                hresult == S_OK ? ShellFailure(S_FALSE) : ShellFailure(hresult);
            return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
        }
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, &CoTaskMemFree);
        selectedEntry.filesystem_path_available = true;
        selectedEntry.filesystem_path = path.get();
    }

    evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
    return ValidateActiveShellViewEvidence(topLevel, selectedShellTab, evidence, output);
}

}  // namespace winexinfo
