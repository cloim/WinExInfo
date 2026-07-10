#include "test_framework.h"

#include "probe/uia_probe.h"
#include "probe/win32_probe.h"

#include <Windows.h>
#include <UIAutomation.h>

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
    uia_contract_preserves_partial_tree_callback_error,
    "uia_contract.preserves_partial_tree_callback_error") {
    winexinfo::Win32ClassTree tree = ExactClassTree();
    tree.capture_status = {
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
        HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE),
        ERROR_INVALID_WINDOW_HANDLE,
    };

    const winexinfo::Win32ContractResult result = winexinfo::ValidateWin32Contract(tree);
    WXI_REQUIRE_EQ(
        result.status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(result.status.hresult, HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
    WXI_REQUIRE_EQ(result.status.win32, DWORD{ERROR_INVALID_WINDOW_HANDLE});
}

}  // namespace
