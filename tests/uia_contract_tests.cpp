#include "test_framework.h"

#include "probe/uia_probe.h"
#include "probe/report_writer.h"
#include "probe/win32_probe.h"

#include <Windows.h>
#include <UIAutomation.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

winexinfo::Win32ClassTree ExactClassTree() {
    const HWND top = Handle(1);
    const HWND shellTab = Handle(2);
    const HWND activeView = Handle(3);
    return {
        top,
        {
            {top, nullptr, L"CabinetWClass", true, RECT{0, 0, 1000, 700}},
            {shellTab, top, L"ShellTabWindowClass", true, RECT{0, 0, 1000, 700}},
            {activeView, shellTab, L"DUIViewWndClassName", true, RECT{0, 0, 1000, 700}},
            {Handle(4), activeView, L"DirectUIHWND", true, RECT{0, 0, 1000, 700}},
            {Handle(5), top, L"msctls_statusbar32", true, RECT{0, 700, 0, 700}},
        },
    };
}

struct FakeWin32CaptureState final {
    DWORD last_error = ERROR_SUCCESS;
    DWORD child_class_error = ERROR_SUCCESS;
    DWORD parent_error = ERROR_SUCCESS;
    bool child_has_top_parent = true;
};

winexinfo::Win32ProbeOperations FakeWin32CaptureOperations(
    FakeWin32CaptureState* const state) {
    return {
        [state](const HWND hwnd, wchar_t* const buffer, const int capacity) {
            if (hwnd == Handle(2) && state->child_class_error != ERROR_SUCCESS) {
                state->last_error = state->child_class_error;
                return 0;
            }
            const std::wstring_view name =
                hwnd == Handle(1) ? L"CabinetWClass" : L"ShellTabWindowClass";
            if (capacity <= static_cast<int>(name.size())) {
                state->last_error = ERROR_INSUFFICIENT_BUFFER;
                return 0;
            }
            std::ranges::copy(name, buffer);
            buffer[name.size()] = L'\0';
            return static_cast<int>(name.size());
        },
        [](const HWND, RECT* const bounds) {
            *bounds = RECT{0, 0, 100, 100};
            return TRUE;
        },
        [](const HWND) { return TRUE; },
        [](const HWND, const WNDENUMPROC callback, const LPARAM parameter) {
            static_cast<void>(callback(Handle(2), parameter));
            return TRUE;
        },
        [state](const HWND) -> HWND {
            if (state->parent_error != ERROR_SUCCESS) {
                state->last_error = state->parent_error;
                return nullptr;
            }
            return state->child_has_top_parent ? Handle(1) : nullptr;
        },
        [state](const DWORD error) { state->last_error = error; },
        [state]() { return state->last_error; },
    };
}

winexinfo::UiaElementEvidence Element(
    const winexinfo::UiaQueryScope scope,
    const std::wstring& framework,
    const CONTROLTYPEID controlType,
    const std::wstring& automationId,
    const std::wstring& className,
    const UIA_HWND nativeHwnd = 0) {
    return {
        scope,
        framework,
        controlType,
        automationId,
        className,
        L"localized display name",
        1234,
        nativeHwnd,
        RECT{10, 20, 30, 40},
        false,
    };
}

winexinfo::UiaContractEvidence ExactUia() {
    winexinfo::UiaContractEvidence evidence{};
    evidence.transport_status = {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS};
    evidence.elements = {
        Element(
            winexinfo::UiaQueryScope::ActiveViewSubtree,
            L"DirectUI",
            UIA_StatusBarControlTypeId,
            L"StatusBarModuleInner",
            L"StatusBarModuleInner"),
        Element(
            winexinfo::UiaQueryScope::StatusBarChildren,
            L"DirectUI",
            UIA_GroupControlTypeId,
            L"System.StatusBarViewItemCount",
            L""),
        Element(
            winexinfo::UiaQueryScope::StatusBarChildren,
            L"DirectUI",
            UIA_GroupControlTypeId,
            L"ViewButtonsGroup",
            L""),
        Element(
            winexinfo::UiaQueryScope::ExplorerSubtree,
            L"XAML",
            UIA_TabControlTypeId,
            L"TabView",
            L"Microsoft.UI.Xaml.Controls.TabView"),
        Element(
            winexinfo::UiaQueryScope::TabViewChildren,
            L"XAML",
            UIA_ListControlTypeId,
            L"TabListView",
            L"ListView"),
    };
    return evidence;
}

void RequireUiaMismatch(const winexinfo::UiaContractEvidence& evidence) {
    winexinfo::UiaContractSnapshot snapshot{};
    const winexinfo::Status status = winexinfo::ValidateUiaContract(evidence, &snapshot);
    WXI_REQUIRE_EQ(status.code, winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
}

WXI_TEST(uia_contract_exact_tree_passes, "uia_contract.exact_tree_passes") {
    const winexinfo::Win32ContractResult win32 =
        winexinfo::ValidateWin32Contract(ExactClassTree());
    WXI_REQUIRE(win32.status.ok());
    WXI_REQUIRE_EQ(win32.active_shell_tab, Handle(2));
    WXI_REQUIRE_EQ(win32.active_view, Handle(3));

    winexinfo::UiaContractSnapshot snapshot{};
    const winexinfo::Status status = winexinfo::ValidateUiaContract(ExactUia(), &snapshot);
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(snapshot.status_bar.automation_id, std::wstring{L"StatusBarModuleInner"});
    WXI_REQUIRE_EQ(snapshot.left_group.automation_id, std::wstring{L"System.StatusBarViewItemCount"});
    WXI_REQUIRE_EQ(snapshot.right_group.automation_id, std::wstring{L"ViewButtonsGroup"});
    WXI_REQUIRE_EQ(snapshot.tab_view.automation_id, std::wstring{L"TabView"});
    WXI_REQUIRE_EQ(snapshot.tab_list.automation_id, std::wstring{L"TabListView"});
}

WXI_TEST(uia_contract_shell_tab_cardinality, "uia_contract.shell_tab_cardinality") {
    winexinfo::Win32ClassTree missing = ExactClassTree();
    missing.nodes[1].visible = false;
    WXI_REQUIRE_EQ(
        winexinfo::ValidateWin32Contract(missing).status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);

    winexinfo::Win32ClassTree duplicate = ExactClassTree();
    duplicate.nodes.push_back(
        {Handle(6), duplicate.top_level, L"ShellTabWindowClass", true, RECT{0, 0, 1, 1}});
    WXI_REQUIRE_EQ(
        winexinfo::ValidateWin32Contract(duplicate).status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
}

WXI_TEST(uia_contract_diview_counts_hidden_duplicates, "uia_contract.diview_counts_hidden_duplicates") {
    winexinfo::Win32ClassTree missing = ExactClassTree();
    missing.nodes.erase(missing.nodes.begin() + 2);
    WXI_REQUIRE_EQ(
        winexinfo::ValidateWin32Contract(missing).status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);

    winexinfo::Win32ClassTree hidden = ExactClassTree();
    hidden.nodes[2].visible = false;
    WXI_REQUIRE_EQ(
        winexinfo::ValidateWin32Contract(hidden).status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);

    winexinfo::Win32ClassTree duplicate = ExactClassTree();
    duplicate.nodes.push_back(
        {Handle(6), Handle(2), L"DUIViewWndClassName", false, RECT{0, 0, 1, 1}});
    WXI_REQUIRE_EQ(
        winexinfo::ValidateWin32Contract(duplicate).status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
}

WXI_TEST(uia_contract_legacy_statusbar_is_evidence_only, "uia_contract.legacy_statusbar_is_evidence_only") {
    const winexinfo::Win32ContractResult result =
        winexinfo::ValidateWin32Contract(ExactClassTree());
    WXI_REQUIRE(result.status.ok());
    WXI_REQUIRE_EQ(result.class_tree.nodes.size(), std::size_t{5});
    WXI_REQUIRE_EQ(result.class_tree.nodes[4].class_name, std::wstring{L"msctls_statusbar32"});
    WXI_REQUIRE_EQ(result.class_tree.nodes[4].bounds.left, result.class_tree.nodes[4].bounds.right);
    WXI_REQUIRE(result.active_view != result.class_tree.nodes[4].hwnd);
}

WXI_TEST(uia_contract_required_selector_cardinality, "uia_contract.required_selector_cardinality") {
    for (std::size_t index = 0; index < ExactUia().elements.size(); ++index) {
        winexinfo::UiaContractEvidence missing = ExactUia();
        missing.elements.erase(missing.elements.begin() + static_cast<std::ptrdiff_t>(index));
        RequireUiaMismatch(missing);

        winexinfo::UiaContractEvidence duplicate = ExactUia();
        duplicate.elements.push_back(duplicate.elements[index]);
        RequireUiaMismatch(duplicate);
    }
}

WXI_TEST(
    uia_contract_reports_exact_selector_match_cardinalities,
    "uia_contract.reports_exact_selector_match_cardinalities") {
    winexinfo::UiaContractSnapshot exactSnapshot{};
    WXI_REQUIRE(winexinfo::ValidateUiaContract(ExactUia(), &exactSnapshot).ok());
    WXI_REQUIRE_EQ(exactSnapshot.cardinalities.status_bar, std::size_t{1});
    WXI_REQUIRE_EQ(exactSnapshot.cardinalities.left_group, std::size_t{1});
    WXI_REQUIRE_EQ(exactSnapshot.cardinalities.right_group, std::size_t{1});
    WXI_REQUIRE_EQ(exactSnapshot.cardinalities.tab_view, std::size_t{1});
    WXI_REQUIRE_EQ(exactSnapshot.cardinalities.tab_list, std::size_t{1});

    winexinfo::UiaContractEvidence missing = ExactUia();
    missing.elements.erase(missing.elements.begin() + 1);
    winexinfo::UiaContractSnapshot missingSnapshot{};
    RequireUiaMismatch(missing);
    static_cast<void>(winexinfo::ValidateUiaContract(missing, &missingSnapshot));
    WXI_REQUIRE_EQ(missingSnapshot.cardinalities.status_bar, std::size_t{1});
    WXI_REQUIRE_EQ(missingSnapshot.cardinalities.left_group, std::size_t{0});
    WXI_REQUIRE_EQ(missingSnapshot.cardinalities.right_group, std::size_t{1});
    WXI_REQUIRE_EQ(missingSnapshot.cardinalities.tab_view, std::size_t{1});
    WXI_REQUIRE_EQ(missingSnapshot.cardinalities.tab_list, std::size_t{1});

    winexinfo::UiaContractEvidence duplicate = ExactUia();
    duplicate.elements.push_back(duplicate.elements[0]);
    duplicate.elements.push_back(duplicate.elements[4]);
    winexinfo::UiaContractSnapshot duplicateSnapshot{};
    RequireUiaMismatch(duplicate);
    static_cast<void>(winexinfo::ValidateUiaContract(duplicate, &duplicateSnapshot));
    WXI_REQUIRE_EQ(duplicateSnapshot.cardinalities.status_bar, std::size_t{2});
    WXI_REQUIRE_EQ(duplicateSnapshot.cardinalities.left_group, std::size_t{1});
    WXI_REQUIRE_EQ(duplicateSnapshot.cardinalities.right_group, std::size_t{1});
    WXI_REQUIRE_EQ(duplicateSnapshot.cardinalities.tab_view, std::size_t{1});
    WXI_REQUIRE_EQ(duplicateSnapshot.cardinalities.tab_list, std::size_t{2});
}

WXI_TEST(uia_contract_rejects_alternate_properties, "uia_contract.rejects_alternate_properties") {
    winexinfo::UiaContractEvidence automationId = ExactUia();
    automationId.elements[0].automation_id = L"StatusBar";
    RequireUiaMismatch(automationId);

    winexinfo::UiaContractEvidence framework = ExactUia();
    framework.elements[3].framework_id = L"Win32";
    RequireUiaMismatch(framework);

    winexinfo::UiaContractEvidence statusNative = ExactUia();
    statusNative.elements[0].native_window_handle = reinterpret_cast<UIA_HWND>(77);
    RequireUiaMismatch(statusNative);

    winexinfo::UiaContractEvidence groupNative = ExactUia();
    groupNative.elements[1].native_window_handle = reinterpret_cast<UIA_HWND>(77);
    RequireUiaMismatch(groupNative);
}

WXI_TEST(uia_contract_rejects_wrong_parent_scope, "uia_contract.rejects_wrong_parent_scope") {
    winexinfo::UiaContractEvidence evidence = ExactUia();
    evidence.elements[1].scope = winexinfo::UiaQueryScope::ExplorerSubtree;
    RequireUiaMismatch(evidence);
}

WXI_TEST(uia_contract_name_is_never_a_selector, "uia_contract.name_is_never_a_selector") {
    winexinfo::UiaContractEvidence evidence = ExactUia();
    evidence.elements[0].automation_id = L"alternate";
    evidence.elements[0].class_name = L"alternate";
    evidence.elements[0].name = L"StatusBarModuleInner";
    RequireUiaMismatch(evidence);
}

WXI_TEST(uia_contract_retains_transport_hresult, "uia_contract.retains_transport_hresult") {
    winexinfo::UiaContractEvidence evidence = ExactUia();
    evidence.transport_status = {
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
        static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
        ERROR_SUCCESS,
    };
    winexinfo::UiaContractSnapshot snapshot{};
    const winexinfo::Status status = winexinfo::ValidateUiaContract(evidence, &snapshot);
    WXI_REQUIRE_EQ(status.code, winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, UIA_E_ELEMENTNOTAVAILABLE);
}

WXI_TEST(
    uia_contract_capture_tree_preserves_getparent_error,
    "uia_contract.capture_tree_preserves_getparent_error") {
    FakeWin32CaptureState state{};
    state.parent_error = ERROR_INVALID_WINDOW_HANDLE;
    const winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_INVALID_WINDOW_HANDLE});
    WXI_REQUIRE_EQ(tree.capture_status.win32, DWORD{ERROR_INVALID_WINDOW_HANDLE});
    WXI_REQUIRE_EQ(tree.nodes.size(), std::size_t{1});
}

WXI_TEST(
    uia_contract_capture_tree_preserves_callback_class_error,
    "uia_contract.capture_tree_preserves_callback_class_error") {
    FakeWin32CaptureState state{};
    state.child_class_error = ERROR_ACCESS_DENIED;
    const winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_ACCESS_DENIED});
    WXI_REQUIRE_EQ(tree.capture_status.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(
    uia_contract_capture_tree_distinguishes_structural_null_parent,
    "uia_contract.capture_tree_distinguishes_structural_null_parent") {
    FakeWin32CaptureState state{};
    state.child_has_top_parent = false;
    const winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(tree.nodes.size(), std::size_t{2});
    WXI_REQUIRE_EQ(tree.nodes[1].parent, nullptr);
    const winexinfo::Win32ContractResult result = winexinfo::ValidateWin32Contract(tree);
    WXI_REQUIRE_EQ(result.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(result.status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(
    uia_contract_host_report_wires_mismatch_cardinalities,
    "uia_contract.host_report_wires_mismatch_cardinalities") {
    winexinfo::ReportSection section{};
    winexinfo::AppendUiaCardinalityReportFields(
        "window.7",
        winexinfo::UiaSelectorCardinalities{2, 0, 1, 3, 4},
        &section);
    const std::string report = winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        false,
        {section},
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
    });

    WXI_REQUIRE(report.find("window.7.cardinality.status_bar=2\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.7.cardinality.left_group=0\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.7.cardinality.right_group=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.7.cardinality.tab_view=3\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.7.cardinality.tab_list=4\n") != std::string::npos);
    WXI_REQUIRE(report.find("active_view_subtree") == std::string::npos);
    WXI_REQUIRE(report.find("status_bar_children") == std::string::npos);
}

WXI_TEST(
    uia_contract_host_report_wires_passing_cardinalities,
    "uia_contract.host_report_wires_passing_cardinalities") {
    winexinfo::ReportSection section{};
    winexinfo::AppendUiaCardinalityReportFields(
        "window.0",
        winexinfo::UiaSelectorCardinalities{1, 1, 1, 1, 1},
        &section);
    const std::string report = winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    });

    WXI_REQUIRE(report.find("window.0.cardinality.status_bar=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.left_group=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.right_group=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.tab_view=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.tab_list=1\n") != std::string::npos);
}

}  // namespace
