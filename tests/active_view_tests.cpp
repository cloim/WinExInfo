#include "test_framework.h"

#include "probe/probe_runner.h"
#include "probe/report_writer.h"
#include "probe/shell_probe.h"

#include <Windows.h>
#include <ExDisp.h>
#include <ShObjIdl_core.h>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

class ScopedSta final {
public:
    ScopedSta() : status_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}

    ~ScopedSta() {
        if (SUCCEEDED(status_)) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT status() const noexcept {
        return status_;
    }

private:
    HRESULT status_;
};

class TestShellWindows final : public IShellWindows {
public:
    TestShellWindows(
        std::vector<Microsoft::WRL::ComPtr<IDispatch>> items,
        const long failureIndex,
        const HRESULT failureResult)
        : items_(std::move(items)),
          failure_index_(failureIndex),
          failure_result_(failureResult) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(
        const IID& interfaceId,
        void** const object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (interfaceId == IID_IUnknown || interfaceId == IID_IDispatch ||
            interfaceId == IID_IShellWindows) {
            *object = static_cast<IShellWindows*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++reference_count_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        if (reference_count_ > 1) {
            --reference_count_;
        }
        return reference_count_;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* const count) override {
        if (count == nullptr) {
            return E_POINTER;
        }
        *count = 0;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE GetIDsOfNames(
        const IID&,
        LPOLESTR*,
        UINT,
        LCID,
        DISPID*) override {
        return DISP_E_UNKNOWNNAME;
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        DISPID,
        const IID&,
        LCID,
        WORD,
        DISPPARAMS*,
        VARIANT*,
        EXCEPINFO*,
        UINT*) override {
        return DISP_E_MEMBERNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE get_Count(long* const count) override {
        if (count == nullptr) {
            return E_POINTER;
        }
        *count = static_cast<long>(items_.size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Item(
        const VARIANT index,
        IDispatch** const folder) override {
        if (folder == nullptr) {
            return E_POINTER;
        }
        *folder = nullptr;
        if (index.vt != VT_I4) {
            return DISP_E_TYPEMISMATCH;
        }
        if (index.lVal == failure_index_) {
            return failure_result_;
        }
        if (index.lVal < 0 ||
            static_cast<std::size_t>(index.lVal) >= items_.size()) {
            return S_FALSE;
        }
        *folder = items_[static_cast<std::size_t>(index.lVal)].Get();
        (*folder)->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE _NewEnum(IUnknown**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Register(IDispatch*, long, int, long*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE RegisterPending(
        long,
        VARIANT*,
        VARIANT*,
        int,
        long*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Revoke(long) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OnNavigate(long, VARIANT*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OnActivated(long, VARIANT_BOOL) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE FindWindowSW(
        VARIANT*,
        VARIANT*,
        int,
        long*,
        int,
        IDispatch**) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE OnCreated(long, IUnknown*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE ProcessAttachDetach(VARIANT_BOOL) override {
        return E_NOTIMPL;
    }

private:
    ULONG reference_count_{1};
    std::vector<Microsoft::WRL::ComPtr<IDispatch>> items_;
    long failure_index_;
    HRESULT failure_result_;
};

Microsoft::WRL::ComPtr<IDispatch> CaptureLiveBrowserDispatch(HWND* const topLevel) {
    WXI_REQUIRE(topLevel != nullptr);
    *topLevel = nullptr;
    Microsoft::WRL::ComPtr<IShellWindows> shellWindows;
    WXI_REQUIRE_EQ(
        CoCreateInstance(
            CLSID_ShellWindows,
            nullptr,
            CLSCTX_LOCAL_SERVER,
            IID_PPV_ARGS(&shellWindows)),
        S_OK);
    long count = 0;
    WXI_REQUIRE_EQ(shellWindows->get_Count(&count), S_OK);
    WXI_REQUIRE(count > 0);
    for (long indexValue = 0; indexValue < count; ++indexValue) {
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = indexValue;
        Microsoft::WRL::ComPtr<IDispatch> dispatch;
        if (shellWindows->Item(index, &dispatch) != S_OK || dispatch == nullptr) {
            continue;
        }
        Microsoft::WRL::ComPtr<IWebBrowser2> browser;
        if (dispatch.As(&browser) != S_OK || browser == nullptr) {
            continue;
        }
        SHANDLE_PTR rawTopLevel = 0;
        if (browser->get_HWND(&rawTopLevel) != S_OK || rawTopLevel == 0) {
            continue;
        }
        *topLevel = reinterpret_cast<HWND>(rawTopLevel);
        return dispatch;
    }
    WXI_REQUIRE(false);
    return {};
}

winexinfo::ShellViewEntryEvidence ExactEntry() {
    return {
        Handle(1),
        Handle(2),
        Handle(3),
        true,
        true,
        true,
        L"C:\\work\\repo",
    };
}

winexinfo::ActiveShellViewEvidence ExactEvidence() {
    return {
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
        winexinfo::ShellProbeTerminalStage::NotStarted,
        {ExactEntry()},
    };
}

winexinfo::ActiveShellViewSnapshot Validate(
    const winexinfo::ActiveShellViewEvidence& evidence) {
    winexinfo::ActiveShellViewSnapshot snapshot{};
    snapshot.status = winexinfo::ValidateActiveShellViewEvidence(
        Handle(1), Handle(2), evidence, &snapshot);
    return snapshot;
}

void RequireMismatch(const winexinfo::ActiveShellViewEvidence& evidence) {
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE_EQ(
        snapshot.status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(snapshot.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(snapshot.status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(active_view_selects_one_exact_entry, "active_view.selects_one_exact_entry") {
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(ExactEvidence());

    WXI_REQUIRE(snapshot.status.ok());
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view, Handle(3));
    WXI_REQUIRE(snapshot.filesystem_path_available);
    WXI_REQUIRE_EQ(snapshot.filesystem_path, std::wstring{L"C:\\work\\repo"});
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::Complete);
}

WXI_TEST(active_view_preserves_rpc_terminal_stage, "active_view.preserves_rpc_terminal_stage") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.capture_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        RPC_E_CANTCALLOUT_ININPUTSYNCCALL,
        ERROR_SUCCESS,
    };
    evidence.terminal_stage =
        winexinfo::ShellProbeTerminalStage::IShellBrowserGetWindow;

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE_EQ(
        snapshot.status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(snapshot.status.hresult, RPC_E_CANTCALLOUT_ININPUTSYNCCALL);
    WXI_REQUIRE_EQ(snapshot.status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::IShellBrowserGetWindow);
    WXI_REQUIRE(winexinfo::IsProbeTransportFailure(snapshot.status));
}

WXI_TEST(active_view_normalizes_nonfailed_capture_stage, "active_view.normalizes_nonfailed_capture_stage") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.capture_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        S_FALSE,
        ERROR_SUCCESS,
    };
    evidence.terminal_stage =
        winexinfo::ShellProbeTerminalStage::IShellItemGetDisplayName;

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE_EQ(
        snapshot.status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(snapshot.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(snapshot.status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::ValidateActiveView);
    WXI_REQUIRE(!winexinfo::IsProbeTransportFailure(snapshot.status));
}

WXI_TEST(active_view_rejects_zero_top_level_entries, "active_view.rejects_zero_top_level_entries") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].browser_top_level = Handle(9);

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{0});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{0});
}

WXI_TEST(active_view_rejects_two_qualifying_entries, "active_view.rejects_two_qualifying_entries") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    winexinfo::ShellViewEntryEvidence second = ExactEntry();
    second.active_view = Handle(4);
    second.filesystem_path = L"C:\\work\\other";
    evidence.entries.push_back(second);

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view, nullptr);
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_rejects_two_shell_tabs_with_one_hidden_view, "active_view.rejects_two_shell_tabs_with_one_hidden_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    winexinfo::ShellViewEntryEvidence hidden = ExactEntry();
    hidden.active_view = Handle(4);
    hidden.active_view_visible = false;
    hidden.filesystem_path_available = false;
    hidden.filesystem_path.clear();
    evidence.entries.push_back(hidden);

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view, nullptr);
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_accepts_one_selected_tab_among_multiple_entries, "active_view.accepts_one_selected_tab_among_multiple_entries") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    for (const std::uintptr_t shellTab : {std::uintptr_t{7}, std::uintptr_t{8}}) {
        winexinfo::ShellViewEntryEvidence nonselected = ExactEntry();
        nonselected.shell_browser = Handle(shellTab);
        nonselected.active_view = nullptr;
        nonselected.active_view_visible = false;
        nonselected.active_view_descendant = false;
        nonselected.filesystem_path_available = false;
        nonselected.filesystem_path.clear();
        evidence.entries.push_back(nonselected);
    }

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE(snapshot.status.ok());
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{3});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view, Handle(3));
}

WXI_TEST(active_view_rejects_hidden_view, "active_view.rejects_hidden_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].active_view_visible = false;
    RequireMismatch(evidence);
}

WXI_TEST(active_view_rejects_wrong_shell_tab, "active_view.rejects_wrong_shell_tab") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].shell_browser = Handle(8);
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{0});
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::ValidateActiveView);
}

WXI_TEST(active_view_rejects_view_outside_shell_tab, "active_view.rejects_view_outside_shell_tab") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].active_view_descendant = false;
    RequireMismatch(evidence);
}

WXI_TEST(active_view_accepts_non_filesystem_view, "active_view.accepts_non_filesystem_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path_available = false;
    evidence.entries[0].filesystem_path.clear();

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE(snapshot.status.ok());
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_rejects_available_empty_path, "active_view.rejects_available_empty_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path.clear();
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.active_view, nullptr);
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_rejects_unavailable_nonempty_path, "active_view.rejects_unavailable_nonempty_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path_available = false;
    RequireMismatch(evidence);
}

WXI_TEST(active_view_preserves_pidl_conversion_failure, "active_view.preserves_pidl_conversion_failure") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.capture_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_ACCESSDENIED,
        ERROR_SUCCESS,
    };

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE_EQ(
        snapshot.status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(snapshot.status.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(snapshot.status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(active_view_classifies_filesystem_attribute, "active_view.classifies_filesystem_attribute") {
    bool available = false;
    const winexinfo::Status status = winexinfo::ClassifyFilesystemAttributes(
        S_OK, SFGAO_FILESYSTEM, &available);
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE(available);
}

WXI_TEST(active_view_classifies_non_filesystem_attribute, "active_view.classifies_non_filesystem_attribute") {
    bool available = true;
    const winexinfo::Status status =
        winexinfo::ClassifyFilesystemAttributes(S_FALSE, 0, &available);
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE(!available);
}

WXI_TEST(active_view_rejects_attribute_result_mismatch, "active_view.rejects_attribute_result_mismatch") {
    bool available = true;
    const winexinfo::Status missingBit =
        winexinfo::ClassifyFilesystemAttributes(S_OK, 0, &available);
    WXI_REQUIRE_EQ(
        missingBit.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(missingBit.hresult, S_FALSE);
    WXI_REQUIRE_EQ(missingBit.win32, DWORD{ERROR_SUCCESS});

    const winexinfo::Status unexpectedBit = winexinfo::ClassifyFilesystemAttributes(
        S_FALSE, SFGAO_FILESYSTEM, &available);
    WXI_REQUIRE_EQ(
        unexpectedBit.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(unexpectedBit.hresult, S_FALSE);
    WXI_REQUIRE_EQ(unexpectedBit.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(active_view_preserves_attribute_transport_failure, "active_view.preserves_attribute_transport_failure") {
    bool available = false;
    const winexinfo::Status status = winexinfo::ClassifyFilesystemAttributes(
        E_ACCESSDENIED, 0, &available);
    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(active_view_classifies_shell_item_results, "active_view.classifies_shell_item_results") {
    bool useEntry = false;
    WXI_REQUIRE(
        winexinfo::ClassifyShellWindowsItemResult(S_OK, true, &useEntry).ok());
    WXI_REQUIRE(useEntry);

    useEntry = true;
    WXI_REQUIRE(
        winexinfo::ClassifyShellWindowsItemResult(S_FALSE, false, &useEntry).ok());
    WXI_REQUIRE(!useEntry);
}

WXI_TEST(active_view_rejects_shell_item_output_mismatch, "active_view.rejects_shell_item_output_mismatch") {
    bool useEntry = false;
    const winexinfo::Status missingDispatch =
        winexinfo::ClassifyShellWindowsItemResult(S_OK, false, &useEntry);
    WXI_REQUIRE_EQ(missingDispatch.hresult, S_FALSE);
    WXI_REQUIRE_EQ(
        missingDispatch.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);

    const winexinfo::Status unexpectedDispatch =
        winexinfo::ClassifyShellWindowsItemResult(S_FALSE, true, &useEntry);
    WXI_REQUIRE_EQ(unexpectedDispatch.hresult, S_FALSE);
    WXI_REQUIRE_EQ(
        unexpectedDispatch.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
}

WXI_TEST(active_view_classifies_browser_interface_results, "active_view.classifies_browser_interface_results") {
    bool useEntry = false;
    WXI_REQUIRE(
        winexinfo::ClassifyBrowserInterfaceResult(S_OK, true, &useEntry).ok());
    WXI_REQUIRE(useEntry);

    useEntry = true;
    WXI_REQUIRE(winexinfo::ClassifyBrowserInterfaceResult(
                    E_NOINTERFACE, false, &useEntry)
                    .ok());
    WXI_REQUIRE(!useEntry);
}

WXI_TEST(active_view_preserves_shell_item_transport_failure, "active_view.preserves_shell_item_transport_failure") {
    bool useEntry = false;
    const winexinfo::Status itemStatus = winexinfo::ClassifyShellWindowsItemResult(
        E_ACCESSDENIED, false, &useEntry);
    WXI_REQUIRE_EQ(itemStatus.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(itemStatus.win32, DWORD{ERROR_ACCESS_DENIED});

    const winexinfo::Status browserStatus = winexinfo::ClassifyBrowserInterfaceResult(
        E_ACCESSDENIED, false, &useEntry);
    WXI_REQUIRE_EQ(browserStatus.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(browserStatus.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(active_view_classifies_required_com_outputs, "active_view.classifies_required_com_outputs") {
    WXI_REQUIRE(winexinfo::ClassifyRequiredComObjectResult(S_OK, true).ok());

    const winexinfo::Status missing =
        winexinfo::ClassifyRequiredComObjectResult(S_OK, false);
    WXI_REQUIRE_EQ(
        missing.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(missing.hresult, S_FALSE);
    WXI_REQUIRE_EQ(missing.win32, DWORD{ERROR_SUCCESS});

    const winexinfo::Status failure =
        winexinfo::ClassifyRequiredComObjectResult(E_ACCESSDENIED, false);
    WXI_REQUIRE_EQ(failure.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(failure.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(active_view_compares_full_width_hwnds, "active_view.compares_full_width_hwnds") {
    constexpr std::uintptr_t topValue = 0xFEDCBA9876543210ull;
    constexpr std::uintptr_t tabValue = 0xABCDEF0123456789ull;
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].browser_top_level = Handle(topValue);
    evidence.entries[0].shell_browser = Handle(tabValue);
    winexinfo::ActiveShellViewSnapshot snapshot{};
    const winexinfo::Status status = winexinfo::ValidateActiveShellViewEvidence(
        Handle(topValue), Handle(tabValue), evidence, &snapshot);
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{1});

    const winexinfo::Status truncated = winexinfo::ValidateActiveShellViewEvidence(
        Handle(static_cast<std::uint32_t>(topValue)),
        Handle(tabValue),
        evidence,
        &snapshot);
    WXI_REQUIRE_EQ(
        truncated.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
}

WXI_TEST(active_view_rejects_duplicate_entries_for_same_view, "active_view.rejects_duplicate_entries_for_same_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries.push_back(ExactEntry());

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{2});
}

WXI_TEST(shell_browser_set_rejects_empty_target_list, "active_view.shell_browser_set_rejects_empty_target_list") {
    winexinfo::ShellBrowserSetCapture capture{};
    const winexinfo::Status status = winexinfo::CaptureShellBrowserSet(
        reinterpret_cast<IShellWindows*>(std::uintptr_t{1}),
        std::span<const HWND>{},
        &capture);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_INVALID_PARAMETER});
    WXI_REQUIRE_EQ(capture.status.code, status.code);
    WXI_REQUIRE_EQ(capture.status.hresult, status.hresult);
    WXI_REQUIRE_EQ(capture.status.win32, status.win32);
    WXI_REQUIRE_EQ(
        capture.terminal_stage,
        winexinfo::ShellProbeTerminalStage::NotStarted);
    WXI_REQUIRE_EQ(capture.owner_thread_id, DWORD{0});
    WXI_REQUIRE(capture.entries.empty());
}

WXI_TEST(shell_browser_set_rejects_null_target, "active_view.shell_browser_set_rejects_null_target") {
    const std::array<HWND, 1> targets{nullptr};
    winexinfo::ShellBrowserSetCapture capture{};
    const winexinfo::Status status = winexinfo::CaptureShellBrowserSet(
        reinterpret_cast<IShellWindows*>(std::uintptr_t{1}), targets, &capture);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_INVALID_PARAMETER});
    WXI_REQUIRE(capture.entries.empty());
}

WXI_TEST(shell_browser_set_rejects_duplicate_target, "active_view.shell_browser_set_rejects_duplicate_target") {
    const std::array<HWND, 2> targets{Handle(1), Handle(1)};
    winexinfo::ShellBrowserSetCapture capture{};
    const winexinfo::Status status = winexinfo::CaptureShellBrowserSet(
        reinterpret_cast<IShellWindows*>(std::uintptr_t{1}), targets, &capture);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_INVALID_PARAMETER});
    WXI_REQUIRE(capture.entries.empty());
}

WXI_TEST(shell_browser_set_retains_non_target_identity, "active_view.shell_browser_set_retains_non_target_identity") {
    ScopedSta apartment;
    WXI_REQUIRE(SUCCEEDED(apartment.status()));
    HWND liveTopLevel = nullptr;
    const Microsoft::WRL::ComPtr<IDispatch> dispatch =
        CaptureLiveBrowserDispatch(&liveTopLevel);
    const HWND targetTopLevel = Handle(
        reinterpret_cast<std::uintptr_t>(liveTopLevel) ^ std::uintptr_t{1});
    const std::array<HWND, 1> targets{targetTopLevel};
    TestShellWindows shellWindows({dispatch}, -1, S_OK);
    winexinfo::ShellBrowserSetCapture capture{};

    const winexinfo::Status status = winexinfo::CaptureShellBrowserSet(
        &shellWindows, targets, &capture);

    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(status.hresult, S_OK);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(capture.entries.size(), std::size_t{1});
    const winexinfo::ShellBrowserEntryCapture& entry = capture.entries[0];
    WXI_REQUIRE(entry.canonical_identity != nullptr);
    WXI_REQUIRE(entry.browser != nullptr);
    WXI_REQUIRE(entry.shell_browser == nullptr);
    WXI_REQUIRE(!entry.target_matched);
    WXI_REQUIRE_EQ(entry.top_level, liveTopLevel);
    WXI_REQUIRE_EQ(entry.shell_tab, nullptr);

    winexinfo::ActiveShellViewSnapshot snapshot{};
    const winexinfo::Status mapping =
        winexinfo::CaptureActiveShellViewFromBrowserSet(
            capture, targetTopLevel, Handle(2), &snapshot);
    WXI_REQUIRE_EQ(
        mapping.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(mapping.hresult, S_FALSE);
    WXI_REQUIRE_EQ(mapping.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{0});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{0});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{0});
}

WXI_TEST(shell_browser_set_clears_partial_capture_on_item_failure, "active_view.shell_browser_set_clears_partial_capture_on_item_failure") {
    ScopedSta apartment;
    WXI_REQUIRE(SUCCEEDED(apartment.status()));
    HWND liveTopLevel = nullptr;
    const Microsoft::WRL::ComPtr<IDispatch> dispatch =
        CaptureLiveBrowserDispatch(&liveTopLevel);
    const HWND targetTopLevel = Handle(
        reinterpret_cast<std::uintptr_t>(liveTopLevel) ^ std::uintptr_t{1});
    const std::array<HWND, 1> targets{targetTopLevel};
    TestShellWindows shellWindows(
        {dispatch, dispatch}, 1, E_ACCESSDENIED);
    winexinfo::ShellBrowserSetCapture capture{};

    const winexinfo::Status status = winexinfo::CaptureShellBrowserSet(
        &shellWindows, targets, &capture);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_ACCESS_DENIED});
    WXI_REQUIRE_EQ(
        capture.terminal_stage,
        winexinfo::ShellProbeTerminalStage::IShellWindowsItem);
    WXI_REQUIRE(capture.entries.empty());
}

WXI_TEST(shell_browser_set_rejects_duplicate_canonical_identity, "active_view.shell_browser_set_rejects_duplicate_canonical_identity") {
    ScopedSta apartment;
    WXI_REQUIRE(SUCCEEDED(apartment.status()));
    HWND liveTopLevel = nullptr;
    const Microsoft::WRL::ComPtr<IDispatch> dispatch =
        CaptureLiveBrowserDispatch(&liveTopLevel);
    const HWND targetTopLevel = Handle(
        reinterpret_cast<std::uintptr_t>(liveTopLevel) ^ std::uintptr_t{1});
    const std::array<HWND, 1> targets{targetTopLevel};
    TestShellWindows shellWindows({dispatch, dispatch}, -1, S_OK);
    winexinfo::ShellBrowserSetCapture capture{};

    const winexinfo::Status status = winexinfo::CaptureShellBrowserSet(
        &shellWindows, targets, &capture);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(
        capture.terminal_stage,
        winexinfo::ShellProbeTerminalStage::ValidateActiveView);
    WXI_REQUIRE(capture.entries.empty());
}

WXI_TEST(shell_browser_set_propagates_capture_failure, "active_view.shell_browser_set_propagates_capture_failure") {
    winexinfo::ShellBrowserSetCapture capture{
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_ACCESSDENIED,
            ERROR_ACCESS_DENIED,
        },
        winexinfo::ShellProbeTerminalStage::IShellWindowsItem,
        GetCurrentThreadId(),
        {},
    };
    winexinfo::ActiveShellViewSnapshot snapshot{};
    const winexinfo::Status status =
        winexinfo::CaptureActiveShellViewFromBrowserSet(
            capture, Handle(1), Handle(2), &snapshot);

    WXI_REQUIRE_EQ(status.code, capture.status.code);
    WXI_REQUIRE_EQ(status.hresult, capture.status.hresult);
    WXI_REQUIRE_EQ(status.win32, capture.status.win32);
    WXI_REQUIRE_EQ(snapshot.status.code, capture.status.code);
    WXI_REQUIRE_EQ(snapshot.status.hresult, capture.status.hresult);
    WXI_REQUIRE_EQ(snapshot.status.win32, capture.status.win32);
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::IShellWindowsItem);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{0});
    WXI_REQUIRE_EQ(snapshot.active_view, nullptr);
}

WXI_TEST(shell_browser_set_rejects_empty_capture, "active_view.shell_browser_set_rejects_empty_capture") {
    winexinfo::ShellBrowserSetCapture capture{
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
        winexinfo::ShellProbeTerminalStage::Complete,
        GetCurrentThreadId(),
        {},
    };
    winexinfo::ActiveShellViewSnapshot snapshot{};
    const winexinfo::Status status =
        winexinfo::CaptureActiveShellViewFromBrowserSet(
            capture, Handle(1), Handle(2), &snapshot);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::ValidateActiveView);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{0});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{0});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{0});
}

WXI_TEST(shell_browser_set_rejects_incoherent_success_status, "active_view.shell_browser_set_rejects_incoherent_success_status") {
    winexinfo::ShellBrowserSetCapture capture{
        {winexinfo::ErrorCode::OK, E_FAIL, ERROR_SUCCESS},
        winexinfo::ShellProbeTerminalStage::Complete,
        GetCurrentThreadId(),
        {},
    };
    winexinfo::ActiveShellViewSnapshot snapshot{};
    const winexinfo::Status status =
        winexinfo::CaptureActiveShellViewFromBrowserSet(
            capture, Handle(1), Handle(2), &snapshot);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(snapshot.status.code, status.code);
    WXI_REQUIRE_EQ(snapshot.status.hresult, status.hresult);
    WXI_REQUIRE_EQ(snapshot.status.win32, status.win32);
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::ValidateActiveView);
}

WXI_TEST(shell_browser_set_rejects_foreign_thread, "active_view.shell_browser_set_rejects_foreign_thread") {
    winexinfo::ShellBrowserSetCapture capture{
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
        winexinfo::ShellProbeTerminalStage::Complete,
        GetCurrentThreadId(),
        {},
    };
    winexinfo::ActiveShellViewSnapshot snapshot{};
    winexinfo::Status status{winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
    std::thread worker([&]() {
        status = winexinfo::CaptureActiveShellViewFromBrowserSet(
            capture, Handle(1), Handle(2), &snapshot);
    });
    worker.join();

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, RPC_E_WRONG_THREAD);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
    WXI_REQUIRE_EQ(snapshot.status.code, status.code);
    WXI_REQUIRE_EQ(snapshot.status.hresult, status.hresult);
    WXI_REQUIRE_EQ(snapshot.status.win32, status.win32);
    WXI_REQUIRE_EQ(
        snapshot.terminal_stage,
        winexinfo::ShellProbeTerminalStage::ValidateActiveView);
}

WXI_TEST(active_view_report_includes_exact_mapping, "active_view.report_includes_exact_mapping") {
    winexinfo::ReportSection section{};
    winexinfo::AppendActiveShellViewReportFields("window.5", Validate(ExactEvidence()), &section);
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    }, &report).ok());

    WXI_REQUIRE(report.find("window.5.active_view_count=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.shell_tab_match_count=1\n") != std::string::npos);
    WXI_REQUIRE(
        report.find("window.5.shell_view=0x0000000000000003\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.filesystem_path_available=true\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.filesystem_path=C:\\work\\repo\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.shell_hresult=0\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.shell_win32=0\n") != std::string::npos);
    WXI_REQUIRE(report.find("LocationURL") == std::string::npos);
    WXI_REQUIRE(report.find("window_title") == std::string::npos);
    const std::string keys[] = {
        "window.5.top_level_entry_count=",
        "window.5.shell_tab_match_count=",
        "window.5.active_view_count=",
        "window.5.shell_view=",
        "window.5.filesystem_path_available=",
        "window.5.filesystem_path=",
        "window.5.shell_hresult=",
        "window.5.shell_win32=",
        "window.5.shell_terminal_stage=",
    };
    for (const std::string& key : keys) {
        const std::size_t position = report.find(key);
        WXI_REQUIRE(position != std::string::npos);
        WXI_REQUIRE(report.find(key, position + 1) == std::string::npos);
    }
}

WXI_TEST(active_view_report_preserves_rpc_stage_once, "active_view.report_preserves_rpc_stage_once") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.capture_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        RPC_E_CANTCALLOUT_ININPUTSYNCCALL,
        ERROR_SUCCESS,
    };
    evidence.terminal_stage =
        winexinfo::ShellProbeTerminalStage::IShellBrowserGetWindow;
    winexinfo::ReportSection section{};
    WXI_REQUIRE(winexinfo::AppendActiveShellViewReportFields(
                    "window.2", Validate(evidence), &section)
                    .ok());
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        false,
        {section},
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
    }, &report).ok());

    const std::string stage =
        "window.2.shell_terminal_stage=ishellbrowser_get_window\n";
    const std::size_t stagePosition = report.find(stage);
    WXI_REQUIRE(stagePosition != std::string::npos);
    WXI_REQUIRE(report.find(stage, stagePosition + 1) == std::string::npos);
    const std::string hresult = "window.2.shell_hresult=" +
        std::to_string(RPC_E_CANTCALLOUT_ININPUTSYNCCALL) + "\n";
    const std::size_t hresultPosition = report.find(hresult);
    WXI_REQUIRE(hresultPosition != std::string::npos);
    WXI_REQUIRE(report.find(hresult, hresultPosition + 1) == std::string::npos);
    WXI_REQUIRE(report.find("unknown") == std::string::npos);
    WXI_REQUIRE(report.find("alternate") == std::string::npos);
}

WXI_TEST(active_view_report_seeds_early_failure_stage_once, "active_view.report_seeds_early_failure_stage_once") {
    winexinfo::ReportSection section{};
    WXI_REQUIRE(winexinfo::InitializeShellTerminalStageReportField(
                    "window.4", &section)
                    .ok());
    const winexinfo::Status duplicate =
        winexinfo::InitializeShellTerminalStageReportField("window.4", &section);
    WXI_REQUIRE_EQ(
        duplicate.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(duplicate.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(duplicate.win32, DWORD{ERROR_INVALID_PARAMETER});
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        false,
        {section},
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
    }, &report).ok());
    const std::string stage = "window.4.shell_terminal_stage=not_started\n";
    const std::size_t position = report.find(stage);
    WXI_REQUIRE(position != std::string::npos);
    WXI_REQUIRE(report.find(stage, position + 1) == std::string::npos);
}

WXI_TEST(active_view_report_replaces_not_started_stage, "active_view.report_replaces_not_started_stage") {
    winexinfo::ReportSection section{};
    WXI_REQUIRE(winexinfo::InitializeShellTerminalStageReportField(
                    "window.1", &section)
                    .ok());
    WXI_REQUIRE(winexinfo::AppendActiveShellViewReportFields(
                    "window.1", Validate(ExactEvidence()), &section)
                    .ok());
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    }, &report).ok());
    const std::string complete = "window.1.shell_terminal_stage=complete\n";
    const std::size_t completePosition = report.find(complete);
    WXI_REQUIRE(completePosition != std::string::npos);
    WXI_REQUIRE(report.find(complete, completePosition + 1) == std::string::npos);
    WXI_REQUIRE(report.find("shell_terminal_stage=not_started") == std::string::npos);
}

WXI_TEST(active_view_report_serializes_every_terminal_stage, "active_view.report_serializes_every_terminal_stage") {
    const std::pair<winexinfo::ShellProbeTerminalStage, std::string_view> cases[] = {
        {winexinfo::ShellProbeTerminalStage::NotStarted, "not_started"},
        {winexinfo::ShellProbeTerminalStage::CoCreateShellWindows, "co_create_shell_windows"},
        {winexinfo::ShellProbeTerminalStage::IShellWindowsGetCount, "ishellwindows_get_count"},
        {winexinfo::ShellProbeTerminalStage::IShellWindowsItem, "ishellwindows_item"},
        {winexinfo::ShellProbeTerminalStage::IDispatchQueryIWebBrowser2, "idispatch_query_iwebbrowser2"},
        {winexinfo::ShellProbeTerminalStage::IWebBrowser2GetHwnd, "iwebbrowser2_get_hwnd"},
        {winexinfo::ShellProbeTerminalStage::IWebBrowser2QueryIServiceProvider, "iwebbrowser2_query_iserviceprovider"},
        {winexinfo::ShellProbeTerminalStage::IServiceProviderQueryTopLevelBrowser, "iserviceprovider_query_top_level_browser"},
        {winexinfo::ShellProbeTerminalStage::IShellBrowserGetWindow, "ishellbrowser_get_window"},
        {winexinfo::ShellProbeTerminalStage::IShellBrowserQueryActiveShellView, "ishellbrowser_query_active_shell_view"},
        {winexinfo::ShellProbeTerminalStage::IShellViewGetWindow, "ishellview_get_window"},
        {winexinfo::ShellProbeTerminalStage::ValidateActiveView, "validate_active_view"},
        {winexinfo::ShellProbeTerminalStage::IShellViewQueryIFolderView, "ishellview_query_ifolderview"},
        {winexinfo::ShellProbeTerminalStage::IFolderViewGetFolder, "ifolderview_get_folder"},
        {winexinfo::ShellProbeTerminalStage::ShGetIdListFromObject, "sh_get_id_list_from_object"},
        {winexinfo::ShellProbeTerminalStage::ShCreateItemFromIdList, "sh_create_item_from_id_list"},
        {winexinfo::ShellProbeTerminalStage::IShellItemGetAttributes, "ishellitem_get_attributes"},
        {winexinfo::ShellProbeTerminalStage::IShellItemGetDisplayName, "ishellitem_get_display_name"},
        {winexinfo::ShellProbeTerminalStage::Complete, "complete"},
    };
    for (const auto& [stage, name] : cases) {
        winexinfo::ActiveShellViewSnapshot snapshot = Validate(ExactEvidence());
        snapshot.terminal_stage = stage;
        winexinfo::ReportSection section{};
        WXI_REQUIRE(winexinfo::AppendActiveShellViewReportFields(
                        "window.0", snapshot, &section)
                        .ok());
        std::string report;
        WXI_REQUIRE(winexinfo::WriteProbeReport({
            winexinfo::ProbeMode::Snapshot,
            true,
            {section},
            winexinfo::ErrorCode::OK,
        }, &report).ok());
        const std::string expected =
            "window.0.shell_terminal_stage=" + std::string{name} + "\n";
        const std::size_t position = report.find(expected);
        WXI_REQUIRE(position != std::string::npos);
        WXI_REQUIRE(report.find(expected, position + 1) == std::string::npos);
    }
}

WXI_TEST(active_view_report_rejects_invalid_terminal_stage, "active_view.report_rejects_invalid_terminal_stage") {
    winexinfo::ActiveShellViewSnapshot snapshot = Validate(ExactEvidence());
    snapshot.terminal_stage =
        static_cast<winexinfo::ShellProbeTerminalStage>(999);
    winexinfo::ReportSection section{};
    const winexinfo::Status status = winexinfo::AppendActiveShellViewReportFields(
        "window.0", snapshot, &section);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_INVALID_PARAMETER});
    WXI_REQUIRE(section.fields.empty());
}

WXI_TEST(active_view_report_includes_empty_non_filesystem_path, "active_view.report_includes_empty_non_filesystem_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path_available = false;
    evidence.entries[0].filesystem_path.clear();
    winexinfo::ReportSection section{};
    WXI_REQUIRE(winexinfo::AppendActiveShellViewReportFields(
                    "window.0", Validate(evidence), &section)
                    .ok());
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    }, &report).ok());
    WXI_REQUIRE(report.find("window.0.filesystem_path=\n") != std::string::npos);
    const std::size_t position = report.find("window.0.filesystem_path=");
    WXI_REQUIRE(report.find("window.0.filesystem_path=", position + 1) == std::string::npos);
}

WXI_TEST(active_view_report_encodes_korean_path, "active_view.report_encodes_korean_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path = L"C:\\작업\\저장소";
    winexinfo::ReportSection section{};
    const winexinfo::Status status = winexinfo::AppendActiveShellViewReportFields(
        "window.0", Validate(evidence), &section);
    WXI_REQUIRE(status.ok());
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    }, &report).ok());
    WXI_REQUIRE(
        report.find("window.0.filesystem_path=C:\\작업\\저장소\n") != std::string::npos);
}

WXI_TEST(active_view_report_rejects_invalid_utf16_path, "active_view.report_rejects_invalid_utf16_path") {
    winexinfo::ActiveShellViewSnapshot snapshot = Validate(ExactEvidence());
    snapshot.filesystem_path = std::wstring{static_cast<wchar_t>(0xD800)};
    winexinfo::ReportSection section{};
    const winexinfo::Status status = winexinfo::AppendActiveShellViewReportFields(
        "window.0", snapshot, &section);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_NO_UNICODE_TRANSLATION});
    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION));
}

}  // namespace
