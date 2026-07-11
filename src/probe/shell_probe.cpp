#include "probe/shell_probe.h"

#include <Windows.h>
#include <ShObjIdl_core.h>
#include <ExDisp.h>
#include <ShlGuid.h>
#include <servprov.h>
#include <wrl/client.h>

#include <array>
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

Status InvalidShellArgument() {
    return {
        ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_INVALIDARG,
        ERROR_INVALID_PARAMETER,
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

Status CaptureShellBrowserSet(
    IShellWindows* const shellWindows,
    const std::span<const HWND> targetTopLevels,
    ShellBrowserSetCapture* const output) {
    if (output == nullptr) {
        return InvalidShellArgument();
    }

    ShellBrowserSetCapture capture{
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        ShellProbeTerminalStage::NotStarted,
        0,
        {},
    };
    const auto fail = [&](const Status status) {
        capture.status = status;
        capture.entries.clear();
        *output = std::move(capture);
        return output->status;
    };
    if (shellWindows == nullptr || targetTopLevels.empty()) {
        return fail(InvalidShellArgument());
    }
    for (std::size_t index = 0; index < targetTopLevels.size(); ++index) {
        if (targetTopLevels[index] == nullptr) {
            return fail(InvalidShellArgument());
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (targetTopLevels[previous] == targetTopLevels[index]) {
                return fail(InvalidShellArgument());
            }
        }
    }

    capture.owner_thread_id = GetCurrentThreadId();
    long count = 0;
    capture.terminal_stage = ShellProbeTerminalStage::IShellWindowsGetCount;
    HRESULT hresult = shellWindows->get_Count(&count);
    if (hresult != S_OK) {
        return fail(ShellFailure(hresult));
    }
    if (count < 0) {
        return fail(ShellFailure(S_FALSE));
    }

    for (long indexValue = 0; indexValue < count; ++indexValue) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = indexValue;
        Microsoft::WRL::ComPtr<IDispatch> dispatch;
        capture.terminal_stage = ShellProbeTerminalStage::IShellWindowsItem;
        hresult = shellWindows->Item(index, &dispatch);
        bool useEntry = false;
        const Status itemStatus = ClassifyShellWindowsItemResult(
            hresult, dispatch != nullptr, &useEntry);
        if (!itemStatus.ok()) {
            return fail(itemStatus);
        }
        if (!useEntry) {
            continue;
        }

        Microsoft::WRL::ComPtr<IWebBrowser2> browser;
        capture.terminal_stage =
            ShellProbeTerminalStage::IDispatchQueryIWebBrowser2;
        hresult = dispatch.As(&browser);
        const Status browserStatus = ClassifyBrowserInterfaceResult(
            hresult, browser != nullptr, &useEntry);
        if (!browserStatus.ok()) {
            return fail(browserStatus);
        }
        if (!useEntry) {
            continue;
        }

        SHANDLE_PTR browserHandle = 0;
        capture.terminal_stage = ShellProbeTerminalStage::IWebBrowser2GetHwnd;
        hresult = browser->get_HWND(&browserHandle);
        if (hresult != S_OK) {
            return fail(ShellFailure(hresult));
        }
        const HWND topLevel = reinterpret_cast<HWND>(browserHandle);
        if (topLevel == nullptr) {
            return fail(ShellFailure(S_FALSE));
        }
        bool target = false;
        for (const HWND candidate : targetTopLevels) {
            if (candidate == topLevel) {
                target = true;
                break;
            }
        }
        Microsoft::WRL::ComPtr<IUnknown> canonicalIdentity;
        capture.terminal_stage =
            ShellProbeTerminalStage::IDispatchQueryIWebBrowser2;
        hresult = browser.As(&canonicalIdentity);
        Status requiredStatus = ClassifyRequiredComObjectResult(
            hresult, canonicalIdentity != nullptr);
        if (!requiredStatus.ok()) {
            return fail(requiredStatus);
        }

        for (const ShellBrowserEntryCapture& existing : capture.entries) {
            if (existing.canonical_identity.Get() == canonicalIdentity.Get()) {
                capture.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
                return fail(ShellFailure(S_FALSE));
            }
        }
        if (!target) {
            capture.entries.push_back({
                std::move(canonicalIdentity),
                std::move(browser),
                nullptr,
                false,
                topLevel,
                nullptr,
            });
            continue;
        }

        Microsoft::WRL::ComPtr<IServiceProvider> serviceProvider;
        capture.terminal_stage =
            ShellProbeTerminalStage::IWebBrowser2QueryIServiceProvider;
        hresult = browser.As(&serviceProvider);
        requiredStatus =
            ClassifyRequiredComObjectResult(hresult, serviceProvider != nullptr);
        if (!requiredStatus.ok()) {
            return fail(requiredStatus);
        }

        Microsoft::WRL::ComPtr<IShellBrowser> shellBrowser;
        capture.terminal_stage =
            ShellProbeTerminalStage::IServiceProviderQueryTopLevelBrowser;
        hresult = serviceProvider->QueryService(
            SID_STopLevelBrowser,
            IID_PPV_ARGS(&shellBrowser));
        requiredStatus =
            ClassifyRequiredComObjectResult(hresult, shellBrowser != nullptr);
        if (!requiredStatus.ok()) {
            return fail(requiredStatus);
        }

        HWND shellTab = nullptr;
        capture.terminal_stage = ShellProbeTerminalStage::IShellBrowserGetWindow;
        hresult = shellBrowser->GetWindow(&shellTab);
        requiredStatus =
            ClassifyRequiredComObjectResult(hresult, shellTab != nullptr);
        if (!requiredStatus.ok()) {
            return fail(requiredStatus);
        }

        for (const ShellBrowserEntryCapture& existing : capture.entries) {
            if (existing.target_matched && existing.top_level == topLevel &&
                existing.shell_tab == shellTab) {
                capture.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
                return fail(ShellFailure(S_FALSE));
            }
        }
        capture.entries.push_back({
            std::move(canonicalIdentity),
            std::move(browser),
            std::move(shellBrowser),
            true,
            topLevel,
            shellTab,
        });
    }

    capture.status = {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    capture.terminal_stage = ShellProbeTerminalStage::Complete;
    *output = std::move(capture);
    return output->status;
}

Status CaptureActiveShellViewFromBrowserSet(
    const ShellBrowserSetCapture& browserSet,
    const HWND topLevel,
    const HWND selectedShellTab,
    ActiveShellViewSnapshot* const output) {
    if (output == nullptr || topLevel == nullptr || selectedShellTab == nullptr) {
        return InvalidShellArgument();
    }

    ActiveShellViewEvidence evidence{
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        ShellProbeTerminalStage::NotStarted,
        {},
    };
    const bool exactSuccess =
        browserSet.status.code == ErrorCode::OK &&
        browserSet.status.hresult == S_OK &&
        browserSet.status.win32 == ERROR_SUCCESS;
    if (!exactSuccess) {
        const bool exactStructuralFailure =
            browserSet.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
            browserSet.status.hresult == S_FALSE &&
            browserSet.status.win32 == ERROR_SUCCESS;
        const bool exactTransportFailure =
            browserSet.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
            FAILED(browserSet.status.hresult) &&
            ((HRESULT_FACILITY(browserSet.status.hresult) == FACILITY_WIN32 &&
              browserSet.status.win32 ==
                  static_cast<DWORD>(HRESULT_CODE(browserSet.status.hresult))) ||
             (HRESULT_FACILITY(browserSet.status.hresult) != FACILITY_WIN32 &&
              browserSet.status.win32 == ERROR_SUCCESS));
        if (!exactStructuralFailure && !exactTransportFailure) {
            evidence.capture_status = ShellFailure(S_FALSE);
            evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
            return ValidateActiveShellViewEvidence(
                topLevel, selectedShellTab, evidence, output);
        }
        evidence.capture_status = browserSet.status;
        evidence.terminal_stage = browserSet.terminal_stage;
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
    }
    if (browserSet.owner_thread_id == 0 ||
        browserSet.owner_thread_id != GetCurrentThreadId()) {
        evidence.capture_status = ShellFailure(RPC_E_WRONG_THREAD);
        evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
    }
    if (browserSet.terminal_stage != ShellProbeTerminalStage::Complete) {
        evidence.capture_status = ShellFailure(S_FALSE);
        evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
    }

    Microsoft::WRL::ComPtr<IShellView> selectedShellView;
    std::size_t selectedEntryIndex = 0;
    for (std::size_t index = 0; index < browserSet.entries.size(); ++index) {
        const ShellBrowserEntryCapture& retained = browserSet.entries[index];
        const bool validCommonShape =
            retained.canonical_identity != nullptr && retained.browser != nullptr &&
            retained.top_level != nullptr;
        const bool validTargetShape =
            retained.target_matched && retained.shell_browser != nullptr &&
            retained.shell_tab != nullptr;
        const bool validNonTargetShape =
            !retained.target_matched && retained.shell_browser == nullptr &&
            retained.shell_tab == nullptr;
        if (!validCommonShape || (!validTargetShape && !validNonTargetShape)) {
            evidence.capture_status = ShellFailure(S_FALSE);
            evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
            return ValidateActiveShellViewEvidence(
                topLevel, selectedShellTab, evidence, output);
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            const ShellBrowserEntryCapture& existing = browserSet.entries[previous];
            if (existing.canonical_identity.Get() == retained.canonical_identity.Get() ||
                (existing.target_matched && retained.target_matched &&
                 existing.top_level == retained.top_level &&
                 existing.shell_tab == retained.shell_tab)) {
                evidence.capture_status = ShellFailure(S_FALSE);
                evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
                return ValidateActiveShellViewEvidence(
                topLevel, selectedShellTab, evidence, output);
            }
        }
        if (!retained.target_matched) {
            continue;
        }
        if (retained.top_level != topLevel) {
            continue;
        }

        ShellViewEntryEvidence entry{
            retained.top_level,
            retained.shell_tab,
            nullptr,
            false,
            false,
            false,
            {},
        };
        if (retained.shell_tab != selectedShellTab) {
            evidence.entries.push_back(std::move(entry));
            continue;
        }

        Microsoft::WRL::ComPtr<IShellView> shellView;
        evidence.terminal_stage =
            ShellProbeTerminalStage::IShellBrowserQueryActiveShellView;
        HRESULT hresult = retained.shell_browser->QueryActiveShellView(&shellView);
        Status requiredStatus =
            ClassifyRequiredComObjectResult(hresult, shellView != nullptr);
        if (!requiredStatus.ok()) {
            evidence.capture_status = requiredStatus;
            return ValidateActiveShellViewEvidence(
                topLevel, selectedShellTab, evidence, output);
        }

        evidence.terminal_stage = ShellProbeTerminalStage::IShellViewGetWindow;
        hresult = shellView->GetWindow(&entry.active_view);
        if (hresult != S_OK) {
            evidence.capture_status = ShellFailure(hresult);
            return ValidateActiveShellViewEvidence(
                topLevel, selectedShellTab, evidence, output);
        }
        entry.active_view_visible =
            entry.active_view != nullptr && IsWindowVisible(entry.active_view) != FALSE;
        entry.active_view_descendant =
            entry.active_view != nullptr && IsChild(selectedShellTab, entry.active_view) != FALSE;
        if (entry.active_view_visible && entry.active_view_descendant &&
            selectedShellView == nullptr) {
            selectedShellView = shellView;
            selectedEntryIndex = evidence.entries.size();
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
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
    }

    ShellViewEntryEvidence& selectedEntry = evidence.entries[selectedEntryIndex];
    Microsoft::WRL::ComPtr<IFolderView> folderView;
    evidence.terminal_stage = ShellProbeTerminalStage::IShellViewQueryIFolderView;
    HRESULT hresult = selectedShellView.As(&folderView);
    Status requiredStatus =
        ClassifyRequiredComObjectResult(hresult, folderView != nullptr);
    if (!requiredStatus.ok()) {
        evidence.capture_status = requiredStatus;
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
    }

    Microsoft::WRL::ComPtr<IShellFolder> folder;
    evidence.terminal_stage = ShellProbeTerminalStage::IFolderViewGetFolder;
    hresult = folderView->GetFolder(IID_PPV_ARGS(&folder));
    requiredStatus = ClassifyRequiredComObjectResult(hresult, folder != nullptr);
    if (!requiredStatus.ok()) {
        evidence.capture_status = requiredStatus;
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
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
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
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
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
    }

    SFGAOF attributes = 0;
    evidence.terminal_stage = ShellProbeTerminalStage::IShellItemGetAttributes;
    hresult = item->GetAttributes(SFGAO_FILESYSTEM, &attributes);
    bool filesystem = false;
    const Status attributeStatus =
        ClassifyFilesystemAttributes(hresult, attributes, &filesystem);
    if (!attributeStatus.ok()) {
        evidence.capture_status = attributeStatus;
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
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
            return ValidateActiveShellViewEvidence(
                topLevel, selectedShellTab, evidence, output);
        }
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(rawPath, &CoTaskMemFree);
        selectedEntry.filesystem_path_available = true;
        selectedEntry.filesystem_path = path.get();
    }

    evidence.terminal_stage = ShellProbeTerminalStage::ValidateActiveView;
    return ValidateActiveShellViewEvidence(
        topLevel, selectedShellTab, evidence, output);
}

Status CaptureActiveShellView(
    const HWND topLevel,
    const HWND selectedShellTab,
    ActiveShellViewSnapshot* const output) {
    if (output == nullptr || topLevel == nullptr || selectedShellTab == nullptr) {
        return InvalidShellArgument();
    }

    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    HRESULT hresult = CoCreateInstance(
        CLSID_ShellWindows,
        nullptr,
        CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&shellWindows));
    const Status requiredStatus =
        ClassifyRequiredComObjectResult(hresult, shellWindows != nullptr);
    if (!requiredStatus.ok()) {
        const ActiveShellViewEvidence evidence{
            requiredStatus,
            ShellProbeTerminalStage::CoCreateShellWindows,
            {},
        };
        return ValidateActiveShellViewEvidence(
            topLevel, selectedShellTab, evidence, output);
    }

    const std::array<HWND, 1> targets{topLevel};
    ShellBrowserSetCapture browserSet{};
    static_cast<void>(
        CaptureShellBrowserSet(shellWindows.Get(), targets, &browserSet));
    return CaptureActiveShellViewFromBrowserSet(
        browserSet, topLevel, selectedShellTab, output);
}

}  // namespace winexinfo
