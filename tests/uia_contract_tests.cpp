#include "test_framework.h"

#include "probe/uia_probe.h"
#include "probe/probe_runner.h"
#include "probe/report_writer.h"
#include "probe/win32_probe.h"

#include <Windows.h>
#include <UIAutomation.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
        {},
        {shellTab, Handle(5)},
    };
}

struct FakeWin32CaptureState final {
    DWORD last_error = ERROR_SUCCESS;
    DWORD child_class_error = ERROR_SUCCESS;
    DWORD parent_error = ERROR_SUCCESS;
    bool child_has_top_parent = true;
    std::array<std::vector<HWND>, 2> top_level_z_orders{
        std::vector<HWND>{Handle(2)},
        std::vector<HWND>{Handle(2)},
    };
    std::size_t z_order_capture_index = 0;
    std::size_t z_order_child_index = 0;
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
        [state](const HWND) -> HWND {
            if (state->z_order_capture_index >= state->top_level_z_orders.size()) {
                state->last_error = ERROR_INVALID_STATE;
                return nullptr;
            }
            state->z_order_child_index = 0;
            const std::vector<HWND>& children =
                state->top_level_z_orders[state->z_order_capture_index];
            if (children.empty()) {
                ++state->z_order_capture_index;
                state->last_error = ERROR_SUCCESS;
                return nullptr;
            }
            state->last_error = ERROR_SUCCESS;
            return children[0];
        },
        [state](const HWND) -> HWND {
            const std::vector<HWND>& children =
                state->top_level_z_orders[state->z_order_capture_index];
            ++state->z_order_child_index;
            if (state->z_order_child_index >= children.size()) {
                ++state->z_order_capture_index;
                state->last_error = ERROR_SUCCESS;
                return nullptr;
            }
            state->last_error = ERROR_SUCCESS;
            return children[state->z_order_child_index];
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

void RequireActiveViewMismatch(
    const winexinfo::Status& status,
    const HRESULT expectedHresult = S_FALSE) {
    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, expectedHresult);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(uia_contract_exact_tree_passes, "uia_contract.exact_tree_passes") {
    const winexinfo::Win32ContractResult win32 =
        winexinfo::ValidateWin32Contract(ExactClassTree());
    WXI_REQUIRE(win32.status.ok());
    WXI_REQUIRE_EQ(win32.active_shell_tab, Handle(2));
    WXI_REQUIRE_EQ(win32.active_view, Handle(3));
    WXI_REQUIRE_EQ(win32.ordered_shell_tabs, std::vector<HWND>{Handle(2)});

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
    winexinfo::Win32ClassTree missingZOrder = ExactClassTree();
    missingZOrder.top_level_child_z_order.clear();
    WXI_REQUIRE_EQ(
        winexinfo::ValidateWin32Contract(missingZOrder).status.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);

    winexinfo::Win32ClassTree hiddenFirst = ExactClassTree();
    hiddenFirst.nodes.push_back(
        {Handle(6), hiddenFirst.top_level, L"ShellTabWindowClass", false, RECT{0, 0, 1, 1}});
    hiddenFirst.nodes.push_back(
        {Handle(7), Handle(6), L"DUIViewWndClassName", true, RECT{0, 0, 1, 1}});
    hiddenFirst.top_level_child_z_order = {Handle(6), Handle(2), Handle(5)};
    const winexinfo::Win32ContractResult hiddenSelected =
        winexinfo::ValidateWin32Contract(hiddenFirst);
    WXI_REQUIRE(hiddenSelected.status.ok());
    WXI_REQUIRE_EQ(hiddenSelected.active_shell_tab, Handle(2));
    WXI_REQUIRE_EQ(hiddenSelected.active_view, Handle(3));
    WXI_REQUIRE_EQ(
        hiddenSelected.ordered_shell_tabs,
        (std::vector<HWND>{Handle(6), Handle(2)}));

    winexinfo::Win32ClassTree multipleVisible = ExactClassTree();
    multipleVisible.nodes.push_back(
        {Handle(6), multipleVisible.top_level, L"ShellTabWindowClass", true, RECT{0, 0, 1, 1}});
    multipleVisible.nodes.push_back(
        {Handle(7), Handle(6), L"DUIViewWndClassName", true, RECT{0, 0, 1, 1}});
    multipleVisible.top_level_child_z_order = {Handle(6), Handle(2), Handle(5)};
    WXI_REQUIRE_EQ(
        winexinfo::ValidateWin32Contract(multipleVisible).status.code,
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

    winexinfo::UiaContractEvidence embeddedFramework = ExactUia();
    embeddedFramework.elements[4].framework_id =
        std::wstring{L"XAML\0suffix", 11};
    RequireUiaMismatch(embeddedFramework);
    winexinfo::UiaContractEvidence embeddedAutomationId = ExactUia();
    embeddedAutomationId.elements[4].automation_id =
        std::wstring{L"TabListView\0suffix", 18};
    RequireUiaMismatch(embeddedAutomationId);
    winexinfo::UiaContractEvidence embeddedClass = ExactUia();
    embeddedClass.elements[4].class_name =
        std::wstring{L"ListView\0suffix", 15};
    RequireUiaMismatch(embeddedClass);
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
    uia_contract_capture_tree_preserves_top_level_z_order,
    "uia_contract.capture_tree_preserves_top_level_z_order") {
    FakeWin32CaptureState state{};
    const winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(tree.top_level_child_z_order.size(), std::size_t{1});
    WXI_REQUIRE_EQ(tree.top_level_child_z_order[0], Handle(2));
}

WXI_TEST(
    uia_contract_capture_tree_rejects_z_order_change,
    "uia_contract.capture_tree_rejects_z_order_change") {
    FakeWin32CaptureState state{};
    state.top_level_z_orders[1] = {Handle(8), Handle(2)};
    const winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(status.code, winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(
    uia_contract_capture_tree_preserves_z_order_error,
    "uia_contract.capture_tree_preserves_z_order_error") {
    FakeWin32CaptureState state{};
    winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    operations.get_top_window = [&state](const HWND) -> HWND {
        state.last_error = ERROR_ACCESS_DENIED;
        return nullptr;
    };
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(
    uia_contract_capture_tree_preserves_getnextwindow_error,
    "uia_contract.capture_tree_preserves_getnextwindow_error") {
    FakeWin32CaptureState state{};
    winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    operations.get_next_window = [&state](const HWND) -> HWND {
        state.last_error = ERROR_INVALID_WINDOW_HANDLE;
        return nullptr;
    };
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_INVALID_WINDOW_HANDLE});
}

WXI_TEST(
    uia_contract_capture_tree_preserves_confirmation_gettopwindow_error,
    "uia_contract.capture_tree_preserves_confirmation_gettopwindow_error") {
    FakeWin32CaptureState state{};
    winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    const std::function<HWND(HWND)> firstPass = operations.get_top_window;
    int calls = 0;
    operations.get_top_window = [&state, &calls, firstPass](const HWND hwnd) -> HWND {
        ++calls;
        if (calls == 2) {
            state.last_error = ERROR_RETRY;
            return nullptr;
        }
        return firstPass(hwnd);
    };
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_RETRY));
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_RETRY});
}

WXI_TEST(
    uia_contract_capture_tree_preserves_confirmation_getnextwindow_error,
    "uia_contract.capture_tree_preserves_confirmation_getnextwindow_error") {
    FakeWin32CaptureState state{};
    winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    const std::function<HWND(HWND)> firstPass = operations.get_next_window;
    int calls = 0;
    operations.get_next_window = [&state, &calls, firstPass](const HWND hwnd) -> HWND {
        ++calls;
        if (calls == 2) {
            state.last_error = ERROR_RETRY;
            return nullptr;
        }
        return firstPass(hwnd);
    };
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_RETRY));
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_RETRY});
}

WXI_TEST(
    uia_contract_capture_tree_rejects_z_order_cycle,
    "uia_contract.capture_tree_rejects_z_order_cycle") {
    FakeWin32CaptureState state{};
    state.top_level_z_orders[0] = {Handle(2), Handle(2)};
    const winexinfo::Win32ProbeOperations operations = FakeWin32CaptureOperations(&state);
    winexinfo::Win32ClassTree tree{};
    const winexinfo::Status status = winexinfo::CaptureWin32ClassTreeWithOperations(
        Handle(1), operations, &tree);

    WXI_REQUIRE_EQ(status.code, winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(
    uia_contract_probe_failure_exit_classification,
    "uia_contract.probe_failure_exit_classification") {
    WXI_REQUIRE(!winexinfo::IsProbeTransportFailure({
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
        S_FALSE,
        ERROR_SUCCESS,
    }));
    WXI_REQUIRE(winexinfo::IsProbeTransportFailure({
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
        HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
        ERROR_ACCESS_DENIED,
    }));
    WXI_REQUIRE(winexinfo::IsProbeTransportFailure({
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
        static_cast<HRESULT>(UIA_E_ELEMENTNOTAVAILABLE),
        ERROR_SUCCESS,
    }));
}

WXI_TEST(
    uia_contract_host_report_wires_mismatch_cardinalities,
    "uia_contract.host_report_wires_mismatch_cardinalities") {
    winexinfo::ReportSection section{};
    winexinfo::AppendUiaCardinalityReportFields(
        "window.7",
        winexinfo::UiaSelectorCardinalities{2, 0, 1, 3, 4},
        &section);
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        false,
        {section},
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
    }, &report).ok());

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
    std::string report;
    WXI_REQUIRE(winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    }, &report).ok());

    WXI_REQUIRE(report.find("window.0.cardinality.status_bar=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.left_group=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.right_group=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.tab_view=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.0.cardinality.tab_list=1\n") != std::string::npos);
}

WXI_TEST(
    uia_contract_event_cache_is_exact_and_name_free,
    "uia_contract.retained.event_cache") {
    const winexinfo::UiaEventCacheContract contract =
        winexinfo::GetExactUiaEventCacheContract();
    WXI_REQUIRE_EQ(contract.element_mode, AutomationElementMode_Full);
    WXI_REQUIRE_EQ(contract.tree_scope, TreeScope_Element);
    const std::array<PROPERTYID, 6> expected{
        UIA_FrameworkIdPropertyId,
        UIA_ControlTypePropertyId,
        UIA_ClassNamePropertyId,
        UIA_AutomationIdPropertyId,
        UIA_ProcessIdPropertyId,
        UIA_IsOffscreenPropertyId,
    };
    WXI_REQUIRE_EQ(contract.properties, expected);
    WXI_REQUIRE(
        std::ranges::find(contract.properties, UIA_NamePropertyId) ==
        contract.properties.end());

    WXI_REQUIRE(winexinfo::ClassifyExactUiaCallResult(S_OK, true).ok());
    const winexinfo::Status nonExact =
        winexinfo::ClassifyExactUiaCallResult(S_FALSE, true);
    WXI_REQUIRE_EQ(
        nonExact.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(nonExact.hresult, S_FALSE);
    WXI_REQUIRE_EQ(nonExact.win32, DWORD{ERROR_SUCCESS});
    const winexinfo::Status missingObject =
        winexinfo::ClassifyExactUiaCallResult(S_OK, false);
    WXI_REQUIRE_EQ(missingObject.hresult, S_FALSE);
    const HRESULT accessDenied = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    const winexinfo::Status transport =
        winexinfo::ClassifyExactUiaCallResult(accessDenied, true);
    WXI_REQUIRE_EQ(transport.hresult, accessDenied);
    WXI_REQUIRE_EQ(transport.win32, DWORD{ERROR_ACCESS_DENIED});

    WXI_REQUIRE(
        winexinfo::ClassifyLegacyUiaCaptureCallResult(S_FALSE, true).ok());
    const winexinfo::Status legacyMissing =
        winexinfo::ClassifyLegacyUiaCaptureCallResult(S_OK, false);
    WXI_REQUIRE_EQ(
        legacyMissing.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(legacyMissing.hresult, S_FALSE);
    const winexinfo::Status legacyNonExactMissing =
        winexinfo::ClassifyLegacyUiaCaptureCallResult(S_FALSE, false);
    WXI_REQUIRE_EQ(
        legacyNonExactMissing.code,
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(legacyNonExactMissing.hresult, S_FALSE);

    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> request;
    const winexinfo::Status nullAutomation =
        winexinfo::CreateUiaEventCacheRequest(nullptr, &request);
    WXI_REQUIRE_EQ(
        nullAutomation.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(nullAutomation.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(nullAutomation.win32, DWORD{ERROR_INVALID_PARAMETER});
    WXI_REQUIRE(request == nullptr);
    const winexinfo::Status nullOutput =
        winexinfo::CreateUiaEventCacheRequest(
            reinterpret_cast<IUIAutomation*>(std::uintptr_t{1}), nullptr);
    WXI_REQUIRE_EQ(nullOutput.hresult, E_INVALIDARG);
}

WXI_TEST(
    uia_contract_retained_access_requires_exact_owner_and_tab_list,
    "uia_contract.retained.access_guard") {
    winexinfo::RetainedUiaAccessEvidence access{
        71,
        71,
        true,
        true,
        Element(
            winexinfo::UiaQueryScope::TabViewChildren,
            L"XAML",
            UIA_ListControlTypeId,
            L"TabListView",
            L"ListView"),
    };
    WXI_REQUIRE(winexinfo::ValidateRetainedUiaAccess(access).ok());

    access.current_thread_id = 72;
    const winexinfo::Status wrongThread =
        winexinfo::ValidateRetainedUiaAccess(access);
    WXI_REQUIRE_EQ(
        wrongThread.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(wrongThread.hresult, RPC_E_WRONG_THREAD);
    WXI_REQUIRE_EQ(wrongThread.win32, DWORD{ERROR_SUCCESS});
    access.current_thread_id = 71;
    access.automation_present = false;
    RequireActiveViewMismatch(winexinfo::ValidateRetainedUiaAccess(access));
    access.automation_present = true;
    access.tab_list_element_present = false;
    RequireActiveViewMismatch(winexinfo::ValidateRetainedUiaAccess(access));
    access.tab_list_element_present = true;
    access.tab_list.process_id = 0;
    RequireActiveViewMismatch(winexinfo::ValidateRetainedUiaAccess(access));
    access.tab_list.process_id = 1234;
    access.tab_list.control_type = UIA_TabControlTypeId;
    RequireActiveViewMismatch(winexinfo::ValidateRetainedUiaAccess(access));
    access.tab_list.control_type = UIA_ListControlTypeId;
    WXI_REQUIRE(winexinfo::ValidateRetainedUiaAccess(access).ok());
}

WXI_TEST(
    uia_contract_tab_children_require_exact_shape_and_pid,
    "uia_contract.retained.direct_children") {
    std::vector<winexinfo::UiaTabChildEvidence> children{
        {L"XAML", UIA_TabItemControlTypeId, L"Tab-1", L"ListViewItem", 1234, false},
        {L"XAML", UIA_TabItemControlTypeId, L"Tab-2", L"ListViewItem", 1234, true},
    };
    std::size_t count = 99;
    WXI_REQUIRE(winexinfo::ValidateTabListDirectChildren(
                    1234, children, &count)
                    .ok());
    WXI_REQUIRE_EQ(count, std::size_t{2});

    children[0].automation_id = L"a completely different id";
    count = 99;
    WXI_REQUIRE(winexinfo::ValidateTabListDirectChildren(
                    1234, children, &count)
                    .ok());
    WXI_REQUIRE_EQ(count, std::size_t{2});

    const auto requireTransactionalMismatch = [&](const auto mutate) {
        std::vector<winexinfo::UiaTabChildEvidence> malformed = children;
        mutate(malformed[0]);
        std::size_t unchanged = 99;
        const winexinfo::Status status =
            winexinfo::ValidateTabListDirectChildren(
                1234, malformed, &unchanged);
        WXI_REQUIRE_EQ(
            status.code,
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
        WXI_REQUIRE_EQ(status.hresult, S_FALSE);
        WXI_REQUIRE_EQ(unchanged, std::size_t{99});
    };
    requireTransactionalMismatch([](auto& child) { child.framework_id = L"Win32"; });
    requireTransactionalMismatch([](auto& child) {
        child.control_type = UIA_ButtonControlTypeId;
    });
    requireTransactionalMismatch([](auto& child) { child.class_name = L"ListBoxItem"; });
    requireTransactionalMismatch([](auto& child) { child.process_id = 4321; });

    count = 99;
    const winexinfo::Status empty = winexinfo::ValidateTabListDirectChildren(
        1234, std::span<const winexinfo::UiaTabChildEvidence>{}, &count);
    WXI_REQUIRE(empty.ok());
    WXI_REQUIRE_EQ(count, std::size_t{0});
    const winexinfo::Status nullCount =
        winexinfo::ValidateTabListDirectChildren(1234, children, nullptr);
    WXI_REQUIRE_EQ(nullCount.hresult, E_INVALIDARG);
}

WXI_TEST(
    uia_contract_retained_capture_guards_are_transactional,
    "uia_contract.retained.transactional_guards") {
    winexinfo::RetainedUiaContractCapture capture{};
    capture.owner_thread_id = 777;
    capture.evidence.transport_status = {
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
        E_ACCESSDENIED,
        ERROR_ACCESS_DENIED,
    };
    const winexinfo::Status nullAutomation =
        winexinfo::CaptureRetainedUiaContract(
            nullptr, Handle(1), Handle(2), &capture);
    WXI_REQUIRE_EQ(
        nullAutomation.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(nullAutomation.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(capture.owner_thread_id, DWORD{777});
    WXI_REQUIRE_EQ(capture.evidence.transport_status.hresult, E_ACCESSDENIED);

    const winexinfo::Status nullHandle =
        winexinfo::CaptureRetainedUiaContract(
            reinterpret_cast<IUIAutomation*>(std::uintptr_t{1}),
            nullptr,
            Handle(2),
            &capture);
    WXI_REQUIRE_EQ(nullHandle.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(capture.owner_thread_id, DWORD{777});

    capture.owner_thread_id = GetCurrentThreadId();
    std::size_t count = 99;
    const winexinfo::Status malformed =
        winexinfo::ReenumerateRetainedTabListDirectChildren(capture, &count);
    WXI_REQUIRE_EQ(
        malformed.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(malformed.hresult, S_FALSE);
    WXI_REQUIRE_EQ(count, std::size_t{99});
    const winexinfo::Status nullCount =
        winexinfo::ReenumerateRetainedTabListDirectChildren(capture, nullptr);
    WXI_REQUIRE_EQ(nullCount.hresult, E_INVALIDARG);
}

}  // namespace
