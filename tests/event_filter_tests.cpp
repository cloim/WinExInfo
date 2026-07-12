#include "test_framework.h"

#include "probe/event_observer.h"
#include "probe/report_writer.h"

#include <ExDispid.h>
#include <UIAutomation.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

DISPPARAMS ShellParameters(VARIANT* const argument) {
    return {argument, nullptr, 1, 0};
}

struct BrowserParameters final {
    VARIANT url_value{};
    std::array<VARIANT, 2> arguments{};
    DISPPARAMS parameters{};

    BrowserParameters() {
        VariantInit(&url_value);
        url_value.vt = VT_BSTR;
        url_value.bstrVal = nullptr;
        VariantInit(&arguments[0]);
        arguments[0].vt = VT_BYREF | VT_VARIANT;
        arguments[0].pvarVal = &url_value;
        VariantInit(&arguments[1]);
        arguments[1].vt = VT_DISPATCH;
        arguments[1].pdispVal = reinterpret_cast<IDispatch*>(std::uintptr_t{1});
        parameters = {arguments.data(), nullptr, 2, 0};
    }
};

winexinfo::UiaEventEvidence SelectionEvidence() {
    return {
        UIA_SelectionItem_ElementSelectedEventId,
        TreeScope_Children,
        9,
        9,
        8312,
        8312,
        winexinfo::UiaSenderRelation::DirectChild,
        L"XAML",
        UIA_TabItemControlTypeId,
        L"ListViewItem",
        L"",
        false,
        std::nullopt,
        false,
        0,
        VT_EMPTY,
        winexinfo::UiaStructureSenderRole::NotApplicable,
    };
}

winexinfo::UiaEventEvidence StructureEvidence(
    const StructureChangeType changeType) {
    winexinfo::UiaEventEvidence evidence{
        UIA_StructureChangedEventId,
        TreeScope_Subtree,
        9,
        9,
        8312,
        8312,
        winexinfo::UiaSenderRelation::RegistrationElement,
        L"XAML",
        UIA_ListControlTypeId,
        L"ListView",
        L"TabListView",
        false,
        changeType,
        changeType == StructureChangeType_ChildRemoved,
        changeType == StructureChangeType_ChildRemoved ? 1U : 0U,
        static_cast<VARTYPE>(
            changeType == StructureChangeType_ChildRemoved ? VT_I4 : VT_EMPTY),
        winexinfo::UiaStructureSenderRole::Container,
    };
    if (changeType == StructureChangeType_ChildAdded) {
        evidence.sender_relation = winexinfo::UiaSenderRelation::DirectChild;
        evidence.structure_sender_role = winexinfo::UiaStructureSenderRole::AddedChild;
    }
    return evidence;
}

winexinfo::UiaHandlerContractEvidence ExactHandlerContract(
    const winexinfo::UiaHandlerKind kind) {
    return {
        kind,
        kind == winexinfo::UiaHandlerKind::Selection ? TreeScope_Children
                                                     : TreeScope_Subtree,
        true,
        AutomationElementMode_Full,
        TreeScope_Element,
        {
            UIA_FrameworkIdPropertyId,
            UIA_ControlTypePropertyId,
            UIA_ClassNamePropertyId,
            UIA_AutomationIdPropertyId,
            UIA_ProcessIdPropertyId,
            UIA_IsOffscreenPropertyId,
        },
    };
}

void RequireMismatch(const winexinfo::Status& status) {
    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
}

void RequireDropped(const winexinfo::Status& status, const bool accepted) {
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE(!accepted);
}

winexinfo::EventObservationSnapshot ExactObservation() {
    const winexinfo::ObservedEventRecord event{
        1,
        7,
        winexinfo::ObservedEventKind::NavigateComplete2,
        winexinfo::ObservedEventTransition::Remapped,
        Handle(0xABC),
        true,
        Handle(0xDEF),
        true,
        false,
        0,
        winexinfo::ObservedStructureChangeType::None,
        Handle(0xABCD),
        Handle(0xCDEF),
        1,
        true,
        L"C:\\work",
        true,
        L"C:\\work\\child",
        {winexinfo::ErrorCode::OK, S_OK, 0},
    };
    return {
        45000,
        1,
        2,
        3,
        {0, 0, 1, 0, 0},
        {winexinfo::ErrorCode::OK, S_OK, 0},
        {winexinfo::ErrorCode::OK, S_OK, 0},
        {event},
    };
}

winexinfo::EventObservationSnapshot PassingObservation() {
    winexinfo::EventObservationSnapshot snapshot = ExactObservation();
    snapshot.event_count = 5;
    snapshot.ignored_event_count = 0;
    snapshot.late_event_count = 0;
    snapshot.kind_counts = {1, 1, 1, 1, 1};
    snapshot.events.resize(5, snapshot.events[0]);

    auto& registered = snapshot.events[0];
    registered.sequence = 1;
    registered.kind = winexinfo::ObservedEventKind::WindowRegistered;
    registered.transition = winexinfo::ObservedEventTransition::Pending;
    registered.shell_cookie_present = true;
    registered.shell_cookie = 41;
    registered.current_active_view = nullptr;
    registered.active_view_count = 0;
    registered.current_filesystem_path_available = false;
    registered.current_filesystem_path.clear();

    auto& structure = snapshot.events[1];
    structure.sequence = 2;
    structure.kind = winexinfo::ObservedEventKind::TabStructureChanged;
    structure.transition = winexinfo::ObservedEventTransition::Remapped;
    structure.shell_cookie_present = false;
    structure.shell_cookie = 0;
    structure.structure_change_type =
        winexinfo::ObservedStructureChangeType::ChildAdded;

    auto& navigate = snapshot.events[2];
    navigate.sequence = 3;
    navigate.kind = winexinfo::ObservedEventKind::NavigateComplete2;
    navigate.transition = winexinfo::ObservedEventTransition::Remapped;
    navigate.shell_cookie_present = false;
    navigate.shell_cookie = 0;

    auto& selected = snapshot.events[3];
    selected.sequence = 4;
    selected.kind = winexinfo::ObservedEventKind::TabSelected;
    selected.transition = winexinfo::ObservedEventTransition::Remapped;
    selected.source_shell_tab_present = false;
    selected.source_shell_tab = nullptr;
    selected.source_was_active = false;
    selected.shell_cookie_present = false;
    selected.shell_cookie = 0;

    auto& revoked = snapshot.events[4];
    revoked.sequence = 5;
    revoked.kind = winexinfo::ObservedEventKind::WindowRevoked;
    revoked.transition = winexinfo::ObservedEventTransition::Revoked;
    revoked.shell_cookie_present = true;
    revoked.shell_cookie = 41;
    revoked.current_active_view = nullptr;
    revoked.active_view_count = 0;
    revoked.current_filesystem_path_available = false;
    revoked.current_filesystem_path.clear();
    return snapshot;
}

void RequireContractHresult(
    const winexinfo::Status& status,
    const HRESULT expectedHresult) {
    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, expectedHresult);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_SUCCESS});
}

}  // namespace

WXI_TEST(event_filter_accepts_exact_shell_window_invocations, "event_filter.shell_window_invocations") {
    VARIANT argument{};
    argument.vt = VT_I4;
    argument.lVal = 41;
    DISPPARAMS parameters = ShellParameters(&argument);
    bool accepted = false;
    LONG cookie = 0;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::NavigateComplete2;
    WXI_REQUIRE(winexinfo::ClassifyShellWindowsEvent(
                    DISPID_WINDOWREGISTERED,
                    IID_NULL,
                    DISPATCH_METHOD,
                    &parameters,
                    &accepted,
                    &kind,
                    &cookie)
                    .ok());
    WXI_REQUIRE(accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::WindowRegistered);
    WXI_REQUIRE_EQ(cookie, LONG{41});

    accepted = false;
    WXI_REQUIRE(winexinfo::ClassifyShellWindowsEvent(
                    DISPID_WINDOWREVOKED,
                    IID_NULL,
                    DISPATCH_METHOD,
                    &parameters,
                    &accepted,
                    &kind,
                    &cookie)
                    .ok());
    WXI_REQUIRE(accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::WindowRevoked);
}

WXI_TEST(event_filter_drops_unrelated_shell_dispids, "event_filter.shell_unrelated_drop") {
    bool accepted = true;
    LONG cookie = 88;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::TabSelected;
    WXI_REQUIRE(winexinfo::ClassifyShellWindowsEvent(
                    DISPID_NAVIGATECOMPLETE2,
                    IID_IUnknown,
                    0,
                    nullptr,
                    &accepted,
                    &kind,
                    &cookie)
                    .ok());
    WXI_REQUIRE(!accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::TabSelected);
    WXI_REQUIRE_EQ(cookie, LONG{88});
}

WXI_TEST(event_filter_rejects_malformed_shell_target_payload, "event_filter.shell_payload_shape") {
    VARIANT argument{};
    argument.vt = VT_UI4;
    argument.ulVal = 41;
    DISPPARAMS parameters = ShellParameters(&argument);
    bool accepted = false;
    LONG cookie = 0;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::NavigateComplete2;
    UINT argumentError = UINT_MAX;
    RequireContractHresult(winexinfo::ClassifyShellWindowsEvent(
        DISPID_WINDOWREGISTERED,
        IID_NULL,
        DISPATCH_METHOD,
        &parameters,
        &accepted,
        &kind,
        &cookie,
        &argumentError), DISP_E_TYPEMISMATCH);
    WXI_REQUIRE_EQ(argumentError, UINT{0});
    WXI_REQUIRE(!accepted);

    argument.vt = VT_I4;
    parameters = ShellParameters(&argument);
    parameters.cNamedArgs = 1;
    RequireContractHresult(winexinfo::ClassifyShellWindowsEvent(
        DISPID_WINDOWREGISTERED,
        IID_NULL,
        DISPATCH_METHOD,
        &parameters,
        &accepted,
        &kind,
        &cookie), DISP_E_NONAMEDARGS);
    parameters.cNamedArgs = 0;
    parameters.cArgs = 0;
    RequireContractHresult(winexinfo::ClassifyShellWindowsEvent(
        DISPID_WINDOWREGISTERED,
        IID_NULL,
        DISPATCH_METHOD,
        &parameters,
        &accepted,
        &kind,
        &cookie), DISP_E_BADPARAMCOUNT);
    parameters.cArgs = 1;
    RequireContractHresult(winexinfo::ClassifyShellWindowsEvent(
        DISPID_WINDOWREGISTERED,
        IID_IUnknown,
        DISPATCH_METHOD,
        &parameters,
        &accepted,
        &kind,
        &cookie), DISP_E_UNKNOWNINTERFACE);
    RequireContractHresult(winexinfo::ClassifyShellWindowsEvent(
        DISPID_WINDOWREGISTERED,
        IID_NULL,
        DISPATCH_PROPERTYGET,
        &parameters,
        &accepted,
        &kind,
        &cookie), DISP_E_MEMBERNOTFOUND);
}

WXI_TEST(event_filter_accepts_exact_navigate_invocation, "event_filter.navigate_invocation") {
    BrowserParameters invocation;
    invocation.url_value.vt = VT_ERROR;
    invocation.url_value.scode = E_UNEXPECTED;
    bool accepted = false;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRegistered;
    WXI_REQUIRE(winexinfo::ClassifyBrowserEvent(
                    DISPID_NAVIGATECOMPLETE2,
                    IID_NULL,
                    DISPATCH_METHOD,
                    &invocation.parameters,
                    &accepted,
                    &kind)
                    .ok());
    WXI_REQUIRE(accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::NavigateComplete2);
}

WXI_TEST(event_filter_drops_unrelated_browser_dispids, "event_filter.browser_unrelated_drop") {
    bool accepted = true;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRevoked;
    WXI_REQUIRE(winexinfo::ClassifyBrowserEvent(
                    DISPID_BEFORENAVIGATE2,
                    IID_IUnknown,
                    0,
                    nullptr,
                    &accepted,
                    &kind)
                    .ok());
    WXI_REQUIRE(!accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::WindowRevoked);
}

WXI_TEST(event_filter_rejects_malformed_navigate_payload_without_reading_url, "event_filter.navigate_payload_shape") {
    BrowserParameters invocation;
    bool accepted = false;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRegistered;
    invocation.arguments[0].pvarVal = nullptr;
    UINT argumentError = UINT_MAX;
    RequireContractHresult(winexinfo::ClassifyBrowserEvent(
        DISPID_NAVIGATECOMPLETE2,
        IID_NULL,
        DISPATCH_METHOD,
        &invocation.parameters,
        &accepted,
        &kind,
        &argumentError), DISP_E_TYPEMISMATCH);
    WXI_REQUIRE_EQ(argumentError, UINT{0});
    invocation.arguments[0].pvarVal = &invocation.url_value;
    invocation.arguments[1].pdispVal = nullptr;
    argumentError = UINT_MAX;
    RequireContractHresult(winexinfo::ClassifyBrowserEvent(
        DISPID_NAVIGATECOMPLETE2,
        IID_NULL,
        DISPATCH_METHOD,
        &invocation.parameters,
        &accepted,
        &kind,
        &argumentError), DISP_E_TYPEMISMATCH);
    WXI_REQUIRE_EQ(argumentError, UINT{1});
    invocation.arguments[1].pdispVal = reinterpret_cast<IDispatch*>(std::uintptr_t{1});
    invocation.parameters.cNamedArgs = 1;
    RequireContractHresult(winexinfo::ClassifyBrowserEvent(
        DISPID_NAVIGATECOMPLETE2,
        IID_NULL,
        DISPATCH_METHOD,
        &invocation.parameters,
        &accepted,
        &kind), DISP_E_NONAMEDARGS);
}

WXI_TEST(event_filter_drops_inactive_navigate_source, "event_filter.inactive_navigate_drop") {
    bool accepted = true;
    const winexinfo::Status inactive = winexinfo::ClassifyNavigateSource(
        1, Handle(0x100), Handle(0x200), &accepted);
    RequireDropped(inactive, accepted);
    WXI_REQUIRE(winexinfo::ClassifyNavigateSource(
                    1, Handle(0x100), Handle(0x100), &accepted)
                    .ok());
    WXI_REQUIRE(accepted);
    RequireMismatch(winexinfo::ClassifyNavigateSource(
        0, Handle(0x100), Handle(0x100), &accepted));
    RequireMismatch(winexinfo::ClassifyNavigateSource(
        2, Handle(0x100), Handle(0x100), &accepted));
}

WXI_TEST(event_filter_requires_exact_selection_sender_scope_pid, "event_filter.selection_sender_scope") {
    bool accepted = false;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRegistered;
    winexinfo::UiaEventEvidence evidence = SelectionEvidence();
    WXI_REQUIRE(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind).ok());
    WXI_REQUIRE(accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::TabSelected);

    evidence.registered_scope = TreeScope_Subtree;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = SelectionEvidence();
    evidence.sender_relation = winexinfo::UiaSenderRelation::Descendant;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = SelectionEvidence();
    evidence.sender_process_id = 999;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = SelectionEvidence();
    evidence.framework_id = L"Win32";
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = SelectionEvidence();
    evidence.control_type = UIA_ListItemControlTypeId;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = SelectionEvidence();
    evidence.class_name = L"AlternateItem";
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
}

WXI_TEST(event_filter_ignores_tabitem_automation_id_and_offscreen, "event_filter.no_name_title_lookup") {
    bool accepted = false;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRegistered;
    winexinfo::UiaEventEvidence evidence = SelectionEvidence();
    evidence.automation_id = L"arbitrary and non-empty";
    evidence.sender_is_offscreen = true;
    WXI_REQUIRE(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind).ok());
    WXI_REQUIRE(accepted);
}

WXI_TEST(event_filter_drops_unrelated_and_stale_uia_callbacks, "event_filter.uia_drop_semantics") {
    bool accepted = true;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRegistered;
    winexinfo::UiaEventEvidence evidence = SelectionEvidence();
    evidence.event_id = UIA_AutomationFocusChangedEventId;
    winexinfo::Status dropped =
        winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind);
    RequireDropped(dropped, accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::WindowRegistered);

    evidence = SelectionEvidence();
    evidence.subscription_generation = 8;
    dropped = winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind);
    RequireDropped(dropped, accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::WindowRegistered);
    evidence = SelectionEvidence();
    evidence.subscription_generation = 10;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
}

WXI_TEST(event_filter_validates_structure_change_matrix, "event_filter.structure_change_matrix") {
    constexpr std::array<StructureChangeType, 6> changeTypes = {
        StructureChangeType_ChildAdded,
        StructureChangeType_ChildRemoved,
        StructureChangeType_ChildrenInvalidated,
        StructureChangeType_ChildrenBulkAdded,
        StructureChangeType_ChildrenBulkRemoved,
        StructureChangeType_ChildrenReordered,
    };
    for (const StructureChangeType changeType : changeTypes) {
        bool accepted = false;
        winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRegistered;
        const winexinfo::UiaEventEvidence evidence = StructureEvidence(changeType);
        WXI_REQUIRE(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind).ok());
        WXI_REQUIRE(accepted);
        WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::TabStructureChanged);
    }

    bool accepted = false;
    winexinfo::ObservedEventKind kind = winexinfo::ObservedEventKind::WindowRegistered;
    winexinfo::UiaEventEvidence evidence = StructureEvidence(StructureChangeType_ChildAdded);
    evidence.sender_relation = winexinfo::UiaSenderRelation::Descendant;
    WXI_REQUIRE(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind).ok());
    WXI_REQUIRE(accepted);
    evidence = StructureEvidence(StructureChangeType_ChildrenInvalidated);
    evidence.sender_relation = winexinfo::UiaSenderRelation::Descendant;
    WXI_REQUIRE(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind).ok());
    WXI_REQUIRE(accepted);

    evidence = StructureEvidence(StructureChangeType_ChildAdded);
    evidence.structure_sender_role = winexinfo::UiaStructureSenderRole::Container;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = StructureEvidence(StructureChangeType_ChildAdded);
    evidence.sender_relation = winexinfo::UiaSenderRelation::RegistrationElement;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = StructureEvidence(StructureChangeType_ChildRemoved);
    evidence.runtime_id_present = false;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = StructureEvidence(StructureChangeType_ChildRemoved);
    evidence.runtime_id_dimensions = 2;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = StructureEvidence(StructureChangeType_ChildRemoved);
    evidence.runtime_id_vartype = VT_UI4;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = StructureEvidence(StructureChangeType_ChildrenInvalidated);
    evidence.runtime_id_present = true;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = StructureEvidence(StructureChangeType_ChildrenBulkAdded);
    evidence.structure_sender_role = winexinfo::UiaStructureSenderRole::AddedChild;
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
    evidence = StructureEvidence(StructureChangeType_ChildrenReordered);
    evidence.sender_relation = static_cast<winexinfo::UiaSenderRelation>(99);
    RequireMismatch(winexinfo::ClassifyUiaEvent(evidence, &accepted, &kind));
}

WXI_TEST(
    event_filter_accepts_exact_structure_subscription_scope_provenance,
    "event_filter.structure_subscription_scope_provenance") {
    bool accepted = false;
    winexinfo::ObservedEventKind kind =
        winexinfo::ObservedEventKind::WindowRegistered;
    winexinfo::UiaEventEvidence evidence =
        StructureEvidence(StructureChangeType_ChildAdded);
    evidence.sender_relation =
        winexinfo::UiaSenderRelation::RegisteredSubtree;
    WXI_REQUIRE(winexinfo::ClassifyUiaEvent(
                    evidence, &accepted, &kind)
                    .ok());
    WXI_REQUIRE(accepted);
    WXI_REQUIRE_EQ(kind, winexinfo::ObservedEventKind::TabStructureChanged);

    evidence = StructureEvidence(StructureChangeType_ChildRemoved);
    evidence.sender_relation =
        winexinfo::UiaSenderRelation::RegisteredSubtree;
    WXI_REQUIRE(winexinfo::ClassifyUiaEvent(
                    evidence, &accepted, &kind)
                    .ok());
    WXI_REQUIRE(accepted);

    evidence.registered_scope = TreeScope_Children;
    RequireMismatch(winexinfo::ClassifyUiaEvent(
        evidence, &accepted, &kind));
}

WXI_TEST(event_filter_requires_exact_uia_registration_contract, "event_filter.uia_registration_contract") {
    WXI_REQUIRE(winexinfo::ValidateUiaHandlerContract(
                    ExactHandlerContract(winexinfo::UiaHandlerKind::Selection))
                    .ok());
    WXI_REQUIRE(winexinfo::ValidateUiaHandlerContract(
                    ExactHandlerContract(winexinfo::UiaHandlerKind::Structure))
                    .ok());

    winexinfo::UiaHandlerContractEvidence evidence =
        ExactHandlerContract(winexinfo::UiaHandlerKind::Selection);
    evidence.cache_request_present = false;
    RequireMismatch(winexinfo::ValidateUiaHandlerContract(evidence));
    evidence = ExactHandlerContract(winexinfo::UiaHandlerKind::Selection);
    evidence.cache_element_mode = AutomationElementMode_None;
    RequireMismatch(winexinfo::ValidateUiaHandlerContract(evidence));
    evidence = ExactHandlerContract(winexinfo::UiaHandlerKind::Selection);
    evidence.cached_properties.push_back(UIA_NamePropertyId);
    RequireMismatch(winexinfo::ValidateUiaHandlerContract(evidence));
    evidence = ExactHandlerContract(winexinfo::UiaHandlerKind::Selection);
    evidence.kind = static_cast<winexinfo::UiaHandlerKind>(99);
    RequireMismatch(winexinfo::ValidateUiaHandlerContract(evidence));
}

WXI_TEST(event_filter_models_pending_bound_cookie_lifetime, "event_filter.shell_cookie_lifetime") {
    winexinfo::ObserverSubscriptionState state;
    WXI_REQUIRE(state.Start().ok());
    WXI_REQUIRE(state.RegisterShellEntry(41).ok());
    RequireMismatch(state.RegisterShellEntry(41));
    RequireMismatch(state.BindShellEntry(99, Handle(0x100)));
    RequireMismatch(state.BindShellEntry(41, nullptr));
    WXI_REQUIRE(state.BindShellEntry(41, Handle(0x100)).ok());
    RequireMismatch(state.BindShellEntry(41, Handle(0x100)));

    HWND revoked = Handle(0x999);
    RequireMismatch(state.RevokeShellEntry(99, &revoked));
    WXI_REQUIRE_EQ(revoked, Handle(0x999));
    WXI_REQUIRE(state.RevokeShellEntry(41, &revoked).ok());
    WXI_REQUIRE_EQ(revoked, Handle(0x100));
    RequireMismatch(state.RevokeShellEntry(41, &revoked));

    WXI_REQUIRE(state.RegisterShellEntry(42).ok());
    revoked = Handle(0x999);
    RequireMismatch(state.RevokeShellEntry(42, &revoked));
    WXI_REQUIRE_EQ(revoked, Handle(0x999));
    WXI_REQUIRE(state.BindShellEntry(42, Handle(0x200)).ok());
    WXI_REQUIRE(state.RevokeShellEntry(42, &revoked).ok());
    WXI_REQUIRE_EQ(revoked, Handle(0x200));
}

WXI_TEST(event_filter_enforces_logical_uia_registration_key, "event_filter.uia_logical_key") {
    winexinfo::ObserverSubscriptionState state;
    WXI_REQUIRE(state.Start().ok());
    const winexinfo::UiaHandlerIdentity exact{
        winexinfo::UiaHandlerKind::Selection, Handle(0x100), 7, 0x200, 0x300};
    WXI_REQUIRE(state.RegisterUiaHandler(exact).ok());
    RequireMismatch(state.RegisterUiaHandler({
        winexinfo::UiaHandlerKind::Selection, Handle(0x100), 7, 0x201, 0x301}));
    RequireMismatch(state.RemoveUiaHandler({
        winexinfo::UiaHandlerKind::Selection, Handle(0x100), 7, 0x200, 0x301}));
    WXI_REQUIRE(state.RemoveUiaHandler(exact).ok());
}

WXI_TEST(event_filter_runs_stopping_cleanup_before_stopped, "event_filter.lifecycle_reducer") {
    winexinfo::ObserverSubscriptionState state;
    bool accepted = true;
    WXI_REQUIRE(state.Start().ok());
    WXI_REQUIRE(state.RegisterShellEntry(41).ok());
    WXI_REQUIRE(state.BindShellEntry(41, Handle(0x100)).ok());
    WXI_REQUIRE(state.RegisterShellEntry(42).ok());
    RequireMismatch(state.DiscardPendingShellEntryDuringStop(42));
    const winexinfo::UiaHandlerIdentity handler{
        winexinfo::UiaHandlerKind::Structure, Handle(0x100), 7, 0x200, 0x300};
    WXI_REQUIRE(state.RegisterUiaHandler(handler).ok());
    WXI_REQUIRE(state.AcceptCallback(&accepted).ok());
    WXI_REQUIRE(accepted);

    WXI_REQUIRE(state.BeginStop().ok());
    accepted = true;
    winexinfo::Status dropped = state.AcceptCallback(&accepted);
    RequireDropped(dropped, accepted);
    RequireMismatch(state.RegisterShellEntry(42));
    RequireMismatch(state.RegisterUiaHandler({
        winexinfo::UiaHandlerKind::Selection, Handle(0x100), 7, 0x400, 0x500}));
    RequireMismatch(state.FinishStop());
    RequireMismatch(state.DiscardPendingShellEntryDuringStop(41));
    WXI_REQUIRE(state.DiscardPendingShellEntryDuringStop(42).ok());
    RequireMismatch(state.DiscardPendingShellEntryDuringStop(42));

    HWND revoked = nullptr;
    WXI_REQUIRE(state.RevokeShellEntry(41, &revoked).ok());
    WXI_REQUIRE_EQ(revoked, Handle(0x100));
    WXI_REQUIRE(state.RemoveUiaHandler(handler).ok());
    WXI_REQUIRE(state.FinishStop().ok());
    accepted = true;
    dropped = state.AcceptCallback(&accepted);
    RequireDropped(dropped, accepted);
    RequireMismatch(state.BeginStop());
}

WXI_TEST(event_filter_hides_then_remaps_active_view, "event_filter.active_view_reducer") {
    winexinfo::EventCorrelationReducer reducer;
    const winexinfo::LogicalActiveViewState initial{
        Handle(0x300), 1, true, L"C:\\work", {winexinfo::ErrorCode::OK, S_OK, 0}};
    WXI_REQUIRE(reducer.SeedWindow(Handle(0x100), 7, 1, initial).ok());

    const winexinfo::ObservedEventTrigger trigger{
        winexinfo::ObservedEventKind::TabSelected,
        Handle(0x100),
        7,
        true,
        Handle(0x200),
        true,
        false,
        0,
        winexinfo::ObservedStructureChangeType::None,
    };
    winexinfo::ObservedEventRecord noPending{};
    RequireMismatch(reducer.CompleteRemap(initial, &noPending));
    winexinfo::ObservedEventTrigger illegal = trigger;
    illegal.kind = winexinfo::ObservedEventKind::WindowRegistered;
    bool accepted = false;
    RequireMismatch(reducer.BeginRemap(illegal, &accepted));
    winexinfo::ObservedEventTrigger unknown = trigger;
    unknown.source_top_level = Handle(0x999);
    RequireMismatch(reducer.BeginRemap(unknown, &accepted));
    winexinfo::ObservedEventTrigger future = trigger;
    future.generation = 8;
    RequireMismatch(reducer.BeginRemap(future, &accepted));
    winexinfo::ObservedEventTrigger invalidNavigate = trigger;
    invalidNavigate.kind = winexinfo::ObservedEventKind::NavigateComplete2;
    invalidNavigate.source_was_active = false;
    RequireMismatch(reducer.BeginRemap(invalidNavigate, &accepted));
    invalidNavigate = trigger;
    invalidNavigate.kind = winexinfo::ObservedEventKind::NavigateComplete2;
    invalidNavigate.source_shell_tab_present = false;
    invalidNavigate.source_shell_tab = nullptr;
    RequireMismatch(reducer.BeginRemap(invalidNavigate, &accepted));
    winexinfo::ObservedEventTrigger absentCookieValue = trigger;
    absentCookieValue.shell_cookie = 7;
    RequireMismatch(reducer.BeginRemap(absentCookieValue, &accepted));
    winexinfo::ObservedEventTrigger tabWithCookie = trigger;
    tabWithCookie.shell_cookie_present = true;
    tabWithCookie.shell_cookie = 7;
    RequireMismatch(reducer.BeginRemap(tabWithCookie, &accepted));
    winexinfo::ObservedEventTrigger invalidStructure = trigger;
    invalidStructure.kind = winexinfo::ObservedEventKind::TabStructureChanged;
    invalidStructure.structure_change_type =
        static_cast<winexinfo::ObservedStructureChangeType>(99);
    RequireMismatch(reducer.BeginRemap(invalidStructure, &accepted));
    WXI_REQUIRE(reducer.BeginRemap(trigger, &accepted).ok());
    WXI_REQUIRE(accepted);
    RequireMismatch(reducer.BeginRemap(trigger, &accepted));
    winexinfo::LogicalActiveViewState hidden{};
    WXI_REQUIRE(reducer.GetWindowState(Handle(0x100), 7, &hidden).ok());
    WXI_REQUIRE_EQ(hidden.active_view, nullptr);
    WXI_REQUIRE_EQ(hidden.active_view_count, std::size_t{0});
    WXI_REQUIRE(!hidden.filesystem_path_available);
    WXI_REQUIRE(hidden.filesystem_path.empty());

    winexinfo::ObservedEventRecord mismatch{};
    const winexinfo::LogicalActiveViewState zero{
        nullptr,
        0,
        false,
        {},
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    const winexinfo::LogicalActiveViewState contradictoryZero{
        nullptr, 0, false, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    RequireMismatch(reducer.CompleteRemap(contradictoryZero, &mismatch));
    const winexinfo::LogicalActiveViewState contradictoryOne{
        Handle(0x399),
        1,
        false,
        {},
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    RequireMismatch(reducer.CompleteRemap(contradictoryOne, &mismatch));
    WXI_REQUIRE(reducer.CompleteRemap(zero, &mismatch).ok());
    WXI_REQUIRE_EQ(mismatch.sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(mismatch.transition, winexinfo::ObservedEventTransition::Mismatch);
    WXI_REQUIRE_EQ(mismatch.previous_active_view, Handle(0x300));
    WXI_REQUIRE_EQ(mismatch.current_active_view, nullptr);
    WXI_REQUIRE_EQ(mismatch.active_view_count, std::size_t{0});
    WXI_REQUIRE(mismatch.previous_filesystem_path_available);
    WXI_REQUIRE_EQ(mismatch.previous_filesystem_path, std::wstring{L"C:\\work"});

    WXI_REQUIRE(reducer.BeginRemap(trigger, &accepted).ok());
    const winexinfo::LogicalActiveViewState multiple{
        nullptr,
        2,
        false,
        {},
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    WXI_REQUIRE(reducer.CompleteRemap(multiple, &mismatch).ok());
    WXI_REQUIRE_EQ(mismatch.transition, winexinfo::ObservedEventTransition::Mismatch);
    WXI_REQUIRE_EQ(mismatch.active_view_count, std::size_t{2});

    WXI_REQUIRE(reducer.BeginRemap(trigger, &accepted).ok());
    const winexinfo::LogicalActiveViewState next{
        Handle(0x400), 1, true, L"C:\\work\\child", {winexinfo::ErrorCode::OK, S_OK, 0}};
    winexinfo::ObservedEventRecord remapped{};
    WXI_REQUIRE(reducer.CompleteRemap(next, &remapped).ok());
    WXI_REQUIRE_EQ(remapped.sequence, std::uint64_t{3});
    WXI_REQUIRE_EQ(remapped.transition, winexinfo::ObservedEventTransition::Remapped);
    WXI_REQUIRE_EQ(remapped.current_active_view, Handle(0x400));
    WXI_REQUIRE(remapped.current_filesystem_path_available);
    WXI_REQUIRE_EQ(remapped.current_filesystem_path, std::wstring{L"C:\\work\\child"});
}

WXI_TEST(event_filter_records_pending_revoke_and_drops_stale_generation, "event_filter.correlation_lifecycle") {
    winexinfo::EventCorrelationReducer reducer;
    const winexinfo::ObservedEventTrigger registered{
        winexinfo::ObservedEventKind::WindowRegistered,
        Handle(0x100),
        8,
        true,
        Handle(0x250),
        false,
        true,
        41,
        winexinfo::ObservedStructureChangeType::None,
    };
    winexinfo::ObservedEventRecord pending{};
    const winexinfo::LogicalActiveViewState pendingState{
        nullptr,
        0,
        false,
        {},
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    RequireMismatch(reducer.RecordRegistered(registered, 0, pendingState, &pending));
    WXI_REQUIRE(reducer.RecordRegistered(registered, 1, pendingState, &pending).ok());
    RequireMismatch(reducer.RecordRegistered(registered, 1, pendingState, &pending));
    WXI_REQUIRE_EQ(pending.sequence, std::uint64_t{1});
    WXI_REQUIRE_EQ(pending.generation, std::uint64_t{8});
    WXI_REQUIRE_EQ(pending.transition, winexinfo::ObservedEventTransition::Pending);
    WXI_REQUIRE_EQ(pending.active_view_count, std::size_t{0});
    WXI_REQUIRE(pending.status.ok());

    winexinfo::ObservedEventTrigger stale = registered;
    stale.kind = winexinfo::ObservedEventKind::TabSelected;
    stale.generation = 7;
    stale.shell_cookie_present = false;
    stale.shell_cookie = 0;
    bool accepted = true;
    const winexinfo::Status staleStatus = reducer.BeginRemap(stale, &accepted);
    RequireDropped(staleStatus, accepted);

    winexinfo::ObservedEventTrigger revoked = registered;
    revoked.kind = winexinfo::ObservedEventKind::WindowRevoked;
    const winexinfo::LogicalActiveViewState terminalState{
        nullptr,
        0,
        false,
        {},
        {winexinfo::ErrorCode::OK, S_OK, 0},
    };
    winexinfo::ObservedEventTrigger wrongGeneration = revoked;
    wrongGeneration.generation = 9;
    winexinfo::ObservedEventRecord terminal{};
    RequireMismatch(reducer.RecordRevoked(
        wrongGeneration, 0, terminalState, &terminal));
    winexinfo::ObservedEventTrigger missingCookie = revoked;
    missingCookie.shell_cookie_present = false;
    RequireMismatch(reducer.RecordRevoked(
        missingCookie, 0, terminalState, &terminal));
    winexinfo::ObservedEventTrigger wrongCookie = revoked;
    wrongCookie.shell_cookie = 99;
    RequireMismatch(reducer.RecordRevoked(wrongCookie, 0, terminalState, &terminal));
    WXI_REQUIRE(reducer.RecordRevoked(revoked, 0, terminalState, &terminal).ok());
    WXI_REQUIRE_EQ(terminal.sequence, std::uint64_t{2});
    WXI_REQUIRE_EQ(terminal.transition, winexinfo::ObservedEventTransition::Revoked);
    WXI_REQUIRE_EQ(terminal.current_active_view, nullptr);
    WXI_REQUIRE_EQ(terminal.active_view_count, std::size_t{0});
    winexinfo::LogicalActiveViewState removed{};
    RequireMismatch(reducer.GetWindowState(Handle(0x100), 8, &removed));
    accepted = true;
    const winexinfo::Status afterRevoke = reducer.BeginRemap(stale, &accepted);
    RequireDropped(afterRevoke, accepted);
}

WXI_TEST(
    event_filter_allows_empty_initial_baseline_before_first_registration,
    "event_filter.empty_initial_baseline") {
    winexinfo::EventCorrelationReducer reducer;
    WXI_REQUIRE(reducer.FinalizeInitialBaseline().ok());
    RequireMismatch(reducer.FinalizeInitialBaseline());
    const winexinfo::ObservedEventTrigger registered{
        winexinfo::ObservedEventKind::WindowRegistered,
        Handle(0x100),
        1,
        true,
        Handle(0x101),
        false,
        true,
        41,
        winexinfo::ObservedStructureChangeType::None,
    };
    const winexinfo::LogicalActiveViewState pending{
        nullptr,
        0,
        false,
        {},
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    winexinfo::ObservedEventRecord record{};
    WXI_REQUIRE(reducer.RecordRegistered(registered, 1, pending, &record).ok());
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Pending);
}

WXI_TEST(event_filter_preserves_top_level_across_tab_entry_revoke, "event_filter.multi_tab_entry_lifecycle") {
    winexinfo::EventCorrelationReducer reducer;
    const winexinfo::LogicalActiveViewState initial{
        Handle(0x300), 1, true, L"C:\\work", {winexinfo::ErrorCode::OK, S_OK, 0}};
    const winexinfo::ObservedEventTrigger registered{
        winexinfo::ObservedEventKind::WindowRegistered,
        Handle(0x100),
        4,
        true,
        Handle(0x250),
        false,
        true,
        50,
        winexinfo::ObservedStructureChangeType::None,
    };
    winexinfo::ObservedEventRecord pending{};
    WXI_REQUIRE(reducer.RecordRegistered(registered, 1, initial, &pending).ok());
    WXI_REQUIRE_EQ(pending.transition, winexinfo::ObservedEventTransition::Remapped);
    WXI_REQUIRE_EQ(pending.current_active_view, Handle(0x300));
    const winexinfo::LogicalActiveViewState next{
        Handle(0x400), 1, true, L"C:\\next", {winexinfo::ErrorCode::OK, S_OK, 0}};
    winexinfo::ObservedEventTrigger second = registered;
    second.shell_cookie = 51;
    RequireMismatch(reducer.RecordRegistered(second, 1, next, &pending));
    RequireMismatch(reducer.RecordRegistered(second, 3, next, &pending));
    WXI_REQUIRE(reducer.RecordRegistered(second, 2, next, &pending).ok());
    winexinfo::ObservedEventTrigger revoked = second;
    revoked.kind = winexinfo::ObservedEventKind::WindowRevoked;
    winexinfo::ObservedEventRecord record{};
    const winexinfo::LogicalActiveViewState terminalState{
        nullptr,
        0,
        false,
        {},
        {winexinfo::ErrorCode::OK, S_OK, 0},
    };
    RequireMismatch(reducer.RecordRevoked(revoked, 0, terminalState, &record));
    RequireMismatch(reducer.RecordRevoked(revoked, 2, terminalState, &record));
    RequireMismatch(reducer.RecordRevoked(revoked, 1, next, &record));
    WXI_REQUIRE(reducer.RecordRevoked(revoked, 1, terminalState, &record).ok());
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Revoked);
    WXI_REQUIRE_EQ(record.current_active_view, nullptr);
    WXI_REQUIRE_EQ(record.active_view_count, std::size_t{0});
    winexinfo::LogicalActiveViewState current{};
    WXI_REQUIRE(reducer.GetWindowState(Handle(0x100), 4, &current).ok());
    WXI_REQUIRE_EQ(current.active_view, nullptr);
    WXI_REQUIRE_EQ(current.active_view_count, std::size_t{0});

    winexinfo::ObservedEventTrigger closeWindow = registered;
    closeWindow.kind = winexinfo::ObservedEventKind::WindowRevoked;
    WXI_REQUIRE(reducer.RecordRevoked(closeWindow, 0, terminalState, &record).ok());
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Revoked);
    RequireMismatch(reducer.GetWindowState(Handle(0x100), 4, &current));

    const winexinfo::LogicalActiveViewState reused{
        Handle(0x500), 1, true, L"C:\\reused", {winexinfo::ErrorCode::OK, S_OK, 0}};
    RequireMismatch(reducer.SeedWindow(Handle(0x100), 4, 1, reused));
    WXI_REQUIRE(reducer.SeedWindow(Handle(0x100), 5, 1, reused).ok());
    winexinfo::ObservedEventTrigger reusedCookie = registered;
    reusedCookie.generation = 5;
    reusedCookie.source_shell_tab = Handle(0x260);
    winexinfo::ObservedEventRecord reusedRecord{};
    WXI_REQUIRE(reducer.RecordRegistered(
                    reusedCookie, 2, reused, &reusedRecord)
                    .ok());
    winexinfo::ObservedEventTrigger stale = registered;
    stale.kind = winexinfo::ObservedEventKind::TabSelected;
    stale.shell_cookie_present = false;
    stale.shell_cookie = 0;
    bool accepted = true;
    const winexinfo::Status staleStatus = reducer.BeginRemap(stale, &accepted);
    RequireDropped(staleStatus, accepted);
}

WXI_TEST(event_filter_binds_initial_entry_cookie_from_exact_difference, "event_filter.initial_entry_revoke.exact_difference") {
    winexinfo::EventCorrelationReducer reducer;
    const winexinfo::LogicalActiveViewState initial{
        Handle(0x300), 1, true, L"C:\\work", {winexinfo::ErrorCode::OK, S_OK, 0}};
    WXI_REQUIRE(reducer.SeedWindow(Handle(0x100), 4, 2, initial).ok());
    const winexinfo::CanonicalShellEntryIdentity first{
        1, Handle(0x100), 4, Handle(0x210)};
    const winexinfo::CanonicalShellEntryIdentity second{
        2, Handle(0x100), 4, Handle(0x220)};
    const winexinfo::CanonicalShellEntryIdentity duplicateTab{
        3, Handle(0x100), 4, Handle(0x210)};
    const winexinfo::CanonicalShellEntryIdentity added{
        3, Handle(0x100), 4, Handle(0x230)};
    WXI_REQUIRE(reducer.SeedInitialShellEntry(first).ok());
    RequireMismatch(reducer.FinalizeInitialShellEntries(Handle(0x100), 4));
    RequireMismatch(reducer.SeedInitialShellEntry(duplicateTab));
    WXI_REQUIRE(reducer.SeedInitialShellEntry(second).ok());
    RequireMismatch(reducer.SeedInitialShellEntry(first));
    RequireMismatch(reducer.SeedInitialShellEntry(
        {3, Handle(0x999), 4, Handle(0x230)}));
    WXI_REQUIRE(reducer.FinalizeInitialShellEntries(Handle(0x100), 4).ok());
    RequireMismatch(reducer.FinalizeInitialShellEntries(Handle(0x100), 4));
    RequireMismatch(reducer.SeedInitialShellEntry(added));
    WXI_REQUIRE(reducer.FinalizeInitialBaseline().ok());
    RequireMismatch(reducer.FinalizeInitialBaseline());

    const winexinfo::ObservedEventTrigger dynamicRegistered{
        winexinfo::ObservedEventKind::WindowRegistered,
        Handle(0x900),
        1,
        true,
        Handle(0x910),
        true,
        true,
        90,
        winexinfo::ObservedStructureChangeType::None,
    };
    winexinfo::ObservedEventRecord dynamicRecord{};
    WXI_REQUIRE(reducer.RecordRegistered(
                    dynamicRegistered, 1, initial, &dynamicRecord)
                    .ok());

    const winexinfo::CanonicalShellEntryIdentity both[] = {first, second};
    const winexinfo::CanonicalShellEntryIdentity onlySecond[] = {second};
    const winexinfo::CanonicalShellEntryIdentity simultaneous[] = {second, added};
    const winexinfo::CanonicalShellEntryIdentity duplicate[] = {second, second};
    winexinfo::ObservedEventTrigger firstRevoked{
        winexinfo::ObservedEventKind::WindowRevoked,
        Handle(0x100),
        4,
        true,
        Handle(0x210),
        true,
        true,
        70,
        winexinfo::ObservedStructureChangeType::None,
    };
    const winexinfo::LogicalActiveViewState remaining{
        Handle(0x400), 1, true, L"C:\\next", {winexinfo::ErrorCode::OK, S_OK, 0}};
    const winexinfo::LogicalActiveViewState terminal{
        nullptr, 0, false, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    winexinfo::ObservedEventRecord record{};
    record.sequence = 88;
    std::uint64_t removed = 99;

    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70, both, both, firstRevoked, 1, remaining, &record, &removed));
    WXI_REQUIRE_EQ(removed, std::uint64_t{99});
    WXI_REQUIRE_EQ(record.sequence, std::uint64_t{88});
    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70,
        both,
        std::span<const winexinfo::CanonicalShellEntryIdentity>{},
        firstRevoked,
        1,
        remaining,
        &record,
        &removed));
    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70, both, simultaneous, firstRevoked, 1, remaining, &record, &removed));
    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70, both, duplicate, firstRevoked, 1, remaining, &record, &removed));
    winexinfo::ObservedEventTrigger wrongTrigger = firstRevoked;
    wrongTrigger.source_shell_tab = Handle(0x999);
    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70, both, onlySecond, wrongTrigger, 1, remaining, &record, &removed));
    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70, both, onlySecond, firstRevoked, 0, remaining, &record, &removed));
    WXI_REQUIRE_EQ(removed, std::uint64_t{99});
    WXI_REQUIRE_EQ(record.sequence, std::uint64_t{88});

    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70,
        both,
        onlySecond,
        firstRevoked,
        1,
        remaining,
        &record,
        &removed));
    WXI_REQUIRE(reducer.ReconcileInitialEntryRevoke(
                    70,
                    both,
                    onlySecond,
                    firstRevoked,
                    1,
                    terminal,
                    &record,
                    &removed)
                    .ok());
    WXI_REQUIRE_EQ(removed, std::uint64_t{1});
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Revoked);

    winexinfo::ObservedEventTrigger secondRevoked = firstRevoked;
    secondRevoked.source_shell_tab = Handle(0x220);
    secondRevoked.shell_cookie = 71;
    WXI_REQUIRE(reducer.ReconcileInitialEntryRevoke(
                    71,
                    onlySecond,
                    std::span<const winexinfo::CanonicalShellEntryIdentity>{},
                    secondRevoked,
                    0,
                    terminal,
                    &record,
                    &removed)
                    .ok());
    WXI_REQUIRE_EQ(removed, std::uint64_t{2});
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Revoked);
}

WXI_TEST(event_filter_initial_entry_reconcile_is_atomic, "event_filter.initial_entry_revoke.atomic") {
    winexinfo::EventCorrelationReducer reducer;
    const winexinfo::LogicalActiveViewState initial{
        Handle(0x300), 1, true, L"C:\\work", {winexinfo::ErrorCode::OK, S_OK, 0}};
    const winexinfo::CanonicalShellEntryIdentity entry{
        1, Handle(0x100), 4, Handle(0x210)};
    WXI_REQUIRE(reducer.SeedWindow(Handle(0x100), 4, 1, initial).ok());
    WXI_REQUIRE(reducer.SeedInitialShellEntry(entry).ok());
    WXI_REQUIRE(reducer.FinalizeInitialShellEntries(Handle(0x100), 4).ok());
    WXI_REQUIRE(reducer.FinalizeInitialBaseline().ok());

    const winexinfo::ObservedEventTrigger selected{
        winexinfo::ObservedEventKind::TabSelected,
        Handle(0x100),
        4,
        true,
        Handle(0x210),
        true,
        false,
        0,
        winexinfo::ObservedStructureChangeType::None,
    };
    bool accepted = false;
    WXI_REQUIRE(reducer.BeginRemap(selected, &accepted).ok());
    WXI_REQUIRE(accepted);

    const winexinfo::ObservedEventTrigger revoked{
        winexinfo::ObservedEventKind::WindowRevoked,
        Handle(0x100),
        4,
        true,
        Handle(0x210),
        true,
        true,
        70,
        winexinfo::ObservedStructureChangeType::None,
    };
    const winexinfo::CanonicalShellEntryIdentity previous[] = {entry};
    const winexinfo::LogicalActiveViewState terminal{
        nullptr, 0, false, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    winexinfo::ObservedEventRecord record{};
    record.sequence = 77;
    std::uint64_t removed = 99;
    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70,
        previous,
        std::span<const winexinfo::CanonicalShellEntryIdentity>{},
        revoked,
        0,
        terminal,
        &record,
        &removed));
    WXI_REQUIRE_EQ(record.sequence, std::uint64_t{77});
    WXI_REQUIRE_EQ(removed, std::uint64_t{99});

    winexinfo::ObservedEventRecord remapRecord{};
    WXI_REQUIRE(reducer.CompleteRemap(initial, &remapRecord).ok());
    WXI_REQUIRE(reducer.ReconcileInitialEntryRevoke(
                    70,
                    previous,
                    std::span<const winexinfo::CanonicalShellEntryIdentity>{},
                    revoked,
                    0,
                    terminal,
                    &record,
                    &removed)
                    .ok());
    WXI_REQUIRE_EQ(removed, std::uint64_t{1});
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Revoked);
}

WXI_TEST(event_filter_requires_complete_global_initial_baseline, "event_filter.initial_entry_revoke.global_baseline") {
    winexinfo::EventCorrelationReducer reducer;
    const winexinfo::LogicalActiveViewState initial{
        Handle(0x300), 1, true, L"C:\\work", {winexinfo::ErrorCode::OK, S_OK, 0}};
    const winexinfo::CanonicalShellEntryIdentity first{
        1, Handle(0x100), 4, Handle(0x210)};
    const winexinfo::CanonicalShellEntryIdentity otherFirst{
        2, Handle(0x200), 7, Handle(0x220)};
    const winexinfo::CanonicalShellEntryIdentity otherSecond{
        3, Handle(0x200), 7, Handle(0x230)};
    WXI_REQUIRE(reducer.SeedWindow(Handle(0x100), 4, 1, initial).ok());
    WXI_REQUIRE(reducer.SeedInitialShellEntry(first).ok());
    WXI_REQUIRE(reducer.FinalizeInitialShellEntries(Handle(0x100), 4).ok());
    WXI_REQUIRE(reducer.SeedWindow(Handle(0x200), 7, 2, initial).ok());
    WXI_REQUIRE(reducer.SeedInitialShellEntry(otherFirst).ok());
    RequireMismatch(reducer.FinalizeInitialBaseline());

    const winexinfo::ObservedEventTrigger revoked{
        winexinfo::ObservedEventKind::WindowRevoked,
        Handle(0x100),
        4,
        true,
        Handle(0x210),
        true,
        true,
        70,
        winexinfo::ObservedStructureChangeType::None,
    };
    const winexinfo::CanonicalShellEntryIdentity incompletePrevious[] = {
        first, otherFirst};
    const winexinfo::CanonicalShellEntryIdentity incompleteCurrent[] = {
        otherFirst};
    const winexinfo::LogicalActiveViewState terminal{
        nullptr, 0, false, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    winexinfo::ObservedEventRecord record{};
    record.sequence = 66;
    std::uint64_t removed = 99;
    RequireMismatch(reducer.ReconcileInitialEntryRevoke(
        70,
        incompletePrevious,
        incompleteCurrent,
        revoked,
        0,
        terminal,
        &record,
        &removed));
    WXI_REQUIRE_EQ(record.sequence, std::uint64_t{66});
    WXI_REQUIRE_EQ(removed, std::uint64_t{99});

    WXI_REQUIRE(reducer.SeedInitialShellEntry(otherSecond).ok());
    WXI_REQUIRE(reducer.FinalizeInitialShellEntries(Handle(0x200), 7).ok());
    WXI_REQUIRE(reducer.FinalizeInitialBaseline().ok());
    const winexinfo::CanonicalShellEntryIdentity completePrevious[] = {
        first, otherFirst, otherSecond};
    const winexinfo::CanonicalShellEntryIdentity completeCurrent[] = {
        otherFirst, otherSecond};
    WXI_REQUIRE(reducer.ReconcileInitialEntryRevoke(
                    70,
                    completePrevious,
                    completeCurrent,
                    revoked,
                    0,
                    terminal,
                    &record,
                    &removed)
                    .ok());
    WXI_REQUIRE_EQ(removed, std::uint64_t{1});
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Revoked);
}

WXI_TEST(event_filter_classifies_registered_mapping_results, "event_filter.registered_result_cardinality") {
    const winexinfo::ObservedEventTrigger trigger{
        winexinfo::ObservedEventKind::WindowRegistered,
        Handle(0x100),
        1,
        true,
        Handle(0x250),
        true,
        true,
        61,
        winexinfo::ObservedStructureChangeType::None,
    };
    winexinfo::EventCorrelationReducer multipleReducer;
    const winexinfo::LogicalActiveViewState multiple{
        nullptr,
        2,
        false,
        {},
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_FALSE, 0},
    };
    winexinfo::ObservedEventRecord record{};
    RequireMismatch(multipleReducer.RecordRegistered(trigger, 99, multiple, &record));
    WXI_REQUIRE(multipleReducer.RecordRegistered(trigger, 1, multiple, &record).ok());
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Mismatch);
    WXI_REQUIRE_EQ(record.active_view_count, std::size_t{2});
    WXI_REQUIRE(!record.status.ok());

    winexinfo::EventCorrelationReducer transportReducer;
    const winexinfo::LogicalActiveViewState transport{
        nullptr,
        0,
        false,
        {},
        {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_ACCESSDENIED,
            ERROR_ACCESS_DENIED,
        },
    };
    WXI_REQUIRE(transportReducer.RecordRegistered(trigger, 1, transport, &record).ok());
    WXI_REQUIRE_EQ(record.transition, winexinfo::ObservedEventTransition::Mismatch);
    WXI_REQUIRE_EQ(record.status.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(record.status.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(event_filter_rejects_invalid_logical_view_shapes, "event_filter.active_view_invariants") {
    winexinfo::EventCorrelationReducer reducer;
    const winexinfo::LogicalActiveViewState invalidCount{
        Handle(0x300), 2, false, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    RequireMismatch(reducer.SeedWindow(Handle(0x100), 1, 1, invalidCount));
    const winexinfo::LogicalActiveViewState missingView{
        nullptr, 1, false, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    RequireMismatch(reducer.SeedWindow(Handle(0x100), 1, 1, missingView));
    const winexinfo::LogicalActiveViewState unexpectedView{
        Handle(0x300), 0, false, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    RequireMismatch(reducer.SeedWindow(Handle(0x100), 1, 1, unexpectedView));
    const winexinfo::LogicalActiveViewState invalidPath{
        Handle(0x300), 1, false, L"C:\\forbidden", {winexinfo::ErrorCode::OK, S_OK, 0}};
    RequireMismatch(reducer.SeedWindow(Handle(0x100), 1, 1, invalidPath));
    const winexinfo::LogicalActiveViewState missingPath{
        Handle(0x300), 1, true, {}, {winexinfo::ErrorCode::OK, S_OK, 0}};
    RequireMismatch(reducer.SeedWindow(Handle(0x100), 1, 1, missingPath));
    const winexinfo::LogicalActiveViewState contradictorySuccess{
        Handle(0x300),
        1,
        false,
        {},
        {winexinfo::ErrorCode::OK, E_ACCESSDENIED, ERROR_ACCESS_DENIED},
    };
    RequireMismatch(reducer.SeedWindow(
        Handle(0x100), 1, 1, contradictorySuccess));
    const winexinfo::LogicalActiveViewState contradictoryMismatch{
        nullptr,
        0,
        false,
        {},
        {winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, S_OK, 0},
    };
    RequireMismatch(reducer.SeedWindow(
        Handle(0x100), 1, 1, contradictoryMismatch));
}

WXI_TEST(event_filter_cleanup_uses_exact_reverse_group_order, "event_filter.cleanup_order") {
    winexinfo::ObserverCleanupLedger ledger;
    const winexinfo::ObserverCleanupRegistration browserOne{
        winexinfo::ObserverCleanupKind::BrowserConnection, 0x100, 11, 0};
    const winexinfo::ObserverCleanupRegistration shell{
        winexinfo::ObserverCleanupKind::ShellLifecycleConnection, 0x200, 12, 0};
    const winexinfo::ObserverCleanupRegistration selectionOne{
        winexinfo::ObserverCleanupKind::UiaSelection, 0x300, 0, 0x400};
    const winexinfo::ObserverCleanupRegistration structureOne{
        winexinfo::ObserverCleanupKind::UiaStructure, 0x300, 0, 0x500};
    const winexinfo::ObserverCleanupRegistration browserTwo{
        winexinfo::ObserverCleanupKind::BrowserConnection, 0x600, 13, 0};
    const winexinfo::ObserverCleanupRegistration selectionTwo{
        winexinfo::ObserverCleanupKind::UiaSelection, 0x700, 0, 0x800};
    const winexinfo::ObserverCleanupRegistration structureTwo{
        winexinfo::ObserverCleanupKind::UiaStructure, 0x700, 0, 0x900};
    for (const auto& registration : {
             browserOne,
             shell,
             selectionOne,
             structureOne,
             browserTwo,
             selectionTwo,
             structureTwo}) {
        WXI_REQUIRE(ledger.RecordSuccessfulRegistration(registration).ok());
    }
    RequireMismatch(ledger.RecordSuccessfulRegistration(browserOne));
    RequireMismatch(ledger.RecordSuccessfulRegistration({
        winexinfo::ObserverCleanupKind::BrowserConnection, 0x100, 99, 0}));
    RequireMismatch(ledger.RecordSuccessfulRegistration({
        winexinfo::ObserverCleanupKind::UiaSelection, 0x300, 0, 0x999}));
    RequireMismatch(ledger.RecordSuccessfulRegistration({
        winexinfo::ObserverCleanupKind::ShellLifecycleConnection, 0xB00, 55, 0}));
    RequireMismatch(ledger.RecordSuccessfulRegistration({
        static_cast<winexinfo::ObserverCleanupKind>(99), 0xA00, 1, 0}));
    std::vector<winexinfo::ObserverCleanupRegistration> order;
    WXI_REQUIRE(ledger.BeginCleanup(&order).ok());
    std::vector<winexinfo::ObserverCleanupRegistration> duplicateOrder;
    RequireMismatch(ledger.BeginCleanup(&duplicateOrder));
    RequireMismatch(ledger.FinishCleanup());
    WXI_REQUIRE_EQ(order.size(), std::size_t{7});
    WXI_REQUIRE_EQ(order[0], browserTwo);
    WXI_REQUIRE_EQ(order[1], browserOne);
    WXI_REQUIRE_EQ(order[2], shell);
    WXI_REQUIRE_EQ(order[3], structureTwo);
    WXI_REQUIRE_EQ(order[4], structureOne);
    WXI_REQUIRE_EQ(order[5], selectionTwo);
    WXI_REQUIRE_EQ(order[6], selectionOne);
}

WXI_TEST(event_filter_cleanup_preserves_first_failure_and_exact_identity, "event_filter.cleanup_first_failure") {
    winexinfo::ObserverCleanupLedger ledger;
    const winexinfo::ObserverCleanupRegistration browser{
        winexinfo::ObserverCleanupKind::BrowserConnection, 0x100, 11, 0};
    const winexinfo::ObserverCleanupRegistration shell{
        winexinfo::ObserverCleanupKind::ShellLifecycleConnection, 0x200, 12, 0};
    WXI_REQUIRE(ledger.RecordSuccessfulRegistration(browser).ok());
    WXI_REQUIRE(ledger.RecordSuccessfulRegistration(shell).ok());
    const winexinfo::Status first{
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_ACCESSDENIED,
        ERROR_ACCESS_DENIED,
    };
    const winexinfo::Status second{
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_OUTOFMEMORY,
        ERROR_OUTOFMEMORY,
    };
    RequireMismatch(ledger.PreserveFirstFailure({
        winexinfo::ErrorCode::INVALID_ARGUMENT,
        E_INVALIDARG,
        ERROR_INVALID_PARAMETER,
    }));
    WXI_REQUIRE(ledger.PreserveFirstFailure(first).ok());
    WXI_REQUIRE(ledger.PreserveFirstFailure(second).ok());
    std::vector<winexinfo::ObserverCleanupRegistration> order;
    WXI_REQUIRE(ledger.BeginCleanup(&order).ok());
    RequireMismatch(ledger.FinishCleanup());
    RequireMismatch(ledger.MarkCleaned(shell, {winexinfo::ErrorCode::OK, S_OK, 0}));
    WXI_REQUIRE(ledger.MarkCleaned(order[0], second).ok());
    WXI_REQUIRE(ledger.MarkCleaned(order[1], {winexinfo::ErrorCode::OK, S_OK, 0}).ok());
    const winexinfo::Status finalStatus = ledger.FinishCleanup();
    WXI_REQUIRE_EQ(finalStatus.code, first.code);
    WXI_REQUIRE_EQ(finalStatus.hresult, first.hresult);
    WXI_REQUIRE_EQ(finalStatus.win32, first.win32);
    RequireMismatch(ledger.PreserveFirstFailure(second));
}

WXI_TEST(event_filter_cleanup_consumes_dynamic_removal_once, "event_filter.cleanup_early_removal") {
    winexinfo::ObserverCleanupLedger ledger;
    const winexinfo::ObserverCleanupRegistration browser{
        winexinfo::ObserverCleanupKind::BrowserConnection, 0x100, 11, 0};
    const winexinfo::ObserverCleanupRegistration wrong{
        winexinfo::ObserverCleanupKind::BrowserConnection, 0x100, 12, 0};
    WXI_REQUIRE(ledger.RecordSuccessfulRegistration(browser).ok());
    RequireMismatch(ledger.ConsumeEarlyRemoval(
        wrong, {winexinfo::ErrorCode::OK, S_OK, 0}));
    const winexinfo::Status removalFailure{
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_ACCESSDENIED,
        ERROR_ACCESS_DENIED,
    };
    WXI_REQUIRE(ledger.ConsumeEarlyRemoval(browser, removalFailure).ok());
    std::vector<winexinfo::ObserverCleanupRegistration> order;
    WXI_REQUIRE(ledger.BeginCleanup(&order).ok());
    WXI_REQUIRE_EQ(order.size(), std::size_t{1});
    WXI_REQUIRE_EQ(order[0], browser);
    RequireMismatch(ledger.ConsumeEarlyRemoval(browser, removalFailure));
    WXI_REQUIRE(ledger.MarkCleaned(
                    browser,
                    {winexinfo::ErrorCode::OK, S_OK, 0})
                    .ok());
    const winexinfo::Status finalStatus = ledger.FinishCleanup();
    WXI_REQUIRE_EQ(finalStatus.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(finalStatus.win32, DWORD{ERROR_ACCESS_DENIED});

    winexinfo::ObserverCleanupLedger successLedger;
    WXI_REQUIRE(successLedger.RecordSuccessfulRegistration(browser).ok());
    WXI_REQUIRE(successLedger.ConsumeEarlyRemoval(
                    browser,
                    {winexinfo::ErrorCode::OK, S_OK, 0})
                    .ok());
    order.clear();
    WXI_REQUIRE(successLedger.BeginCleanup(&order).ok());
    WXI_REQUIRE(order.empty());
    WXI_REQUIRE(successLedger.FinishCleanup().ok());
}

WXI_TEST(event_filter_report_serializes_exact_schema, "event_filter.report_schema") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe,
        false,
        {},
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
    };
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(
                    ExactObservation(), &report)
                    .ok());
    std::string output = "sentinel";
    WXI_REQUIRE(winexinfo::WriteProbeReport(report, &output).ok());
    const std::string expectedLines[] = {
        "observe.duration_ms=45000\n",
        "observe.event_count=1\n",
        "observe.ignored_event_count=2\n",
        "observe.late_event_count=3\n",
        "observe.kind.window_registered_count=0\n",
        "observe.kind.window_revoked_count=0\n",
        "observe.kind.navigate_complete2_count=1\n",
        "observe.kind.tab_selected_count=0\n",
        "observe.kind.tab_structure_changed_count=0\n",
        "observe.runtime.error_code=OK\n",
        "observe.runtime.hresult=0\n",
        "observe.runtime.win32=0\n",
        "observe.cleanup.error_code=OK\n",
        "observe.cleanup.hresult=0\n",
        "observe.cleanup.win32=0\n",
        "event.00000000000000000001.sequence=1\n",
        "event.00000000000000000001.generation=7\n",
        "event.00000000000000000001.kind=navigate_complete2\n",
        "event.00000000000000000001.transition=remapped\n",
        "event.00000000000000000001.source_top_level_hwnd=0x0000000000000ABC\n",
        "event.00000000000000000001.source_shell_tab_present=true\n",
        "event.00000000000000000001.source_shell_tab_hwnd=0x0000000000000DEF\n",
        "event.00000000000000000001.source_was_active=true\n",
        "event.00000000000000000001.shell_window_cookie_present=false\n",
        "event.00000000000000000001.shell_window_cookie=0\n",
        "event.00000000000000000001.structure_change_type=none\n",
        "event.00000000000000000001.previous_active_view_hwnd=0x000000000000ABCD\n",
        "event.00000000000000000001.current_active_view_hwnd=0x000000000000CDEF\n",
        "event.00000000000000000001.active_view_count=1\n",
        "event.00000000000000000001.previous_filesystem_path_available=true\n",
        "event.00000000000000000001.previous_filesystem_path=C:\\work\n",
        "event.00000000000000000001.current_filesystem_path_available=true\n",
        "event.00000000000000000001.current_filesystem_path=C:\\work\\child\n",
        "event.00000000000000000001.status.error_code=OK\n",
        "event.00000000000000000001.status.hresult=0\n",
        "event.00000000000000000001.status.win32=0\n",
    };
    for (const std::string& line : expectedLines) {
        const std::size_t position = output.find(line);
        WXI_REQUIRE(position != std::string::npos);
        WXI_REQUIRE(output.find(line, position + 1) == std::string::npos);
    }
    const std::string globalLines[] = {
        "probe_version=1\n",
        "mode=observe\n",
        "result=fail\n",
        "error_code=ACTIVE_VIEW_CONTRACT_MISMATCH\n",
    };
    for (const std::string& line : globalLines) {
        WXI_REQUIRE(output.find(line) != std::string::npos);
    }
    WXI_REQUIRE_EQ(
        static_cast<std::size_t>(std::ranges::count(output, '\n')),
        std::size(expectedLines) + std::size(globalLines));
    WXI_REQUIRE(output.find("LocationURL") == std::string::npos);
    WXI_REQUIRE(output.find("Name=") == std::string::npos);
    WXI_REQUIRE(output.find("title=") == std::string::npos);
    WXI_REQUIRE(output.find("timestamp=") == std::string::npos);
    WXI_REQUIRE(output.starts_with(
        "probe_version=1\nmode=observe\nresult=fail\n"));
    WXI_REQUIRE(output.ends_with(
        "error_code=ACTIVE_VIEW_CONTRACT_MISMATCH\n"));
    WXI_REQUIRE(
        output.find("observe.duration_ms=") <
        output.find("event.00000000000000000001."));
}

WXI_TEST(event_filter_report_rejects_duplicate_and_reserved_keys, "event_filter.report_duplicate_keys") {
    winexinfo::ProbeReport duplicate{
        winexinfo::ProbeMode::Observe,
        false,
        {
            {{{"duplicate", "one"}}},
            {{{"duplicate", "two"}}},
        },
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
    };
    std::string output = "unchanged";
    RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
    WXI_REQUIRE_EQ(output, std::string{"unchanged"});
    duplicate.sections = {{{{"mode", "observe"}}}};
    RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
    WXI_REQUIRE_EQ(output, std::string{"unchanged"});
    const std::string reserved[] = {"probe_version", "mode", "result", "error_code"};
    for (const std::string& key : reserved) {
        duplicate.sections = {{{{key, "value"}}}};
        RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
        WXI_REQUIRE_EQ(output, std::string{"unchanged"});
    }
    const std::string invalidKeys[] = {
        "",
        "bad\nkey",
        "bad\rkey",
        "bad=key",
        "bad%key",
        "bad key",
        "bad\tkey",
        std::string{"bad\0key", 7},
    };
    for (const std::string& key : invalidKeys) {
        duplicate.sections = {{{{key, "value"}}}};
        RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
        WXI_REQUIRE_EQ(output, std::string{"unchanged"});
    }
    duplicate.sections = {{{{"valid", std::string{"bad\0value", 9}}}}};
    RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
    WXI_REQUIRE_EQ(output, std::string{"unchanged"});
    const winexinfo::Status nullOutput =
        winexinfo::WriteProbeReport(duplicate, nullptr);
    WXI_REQUIRE_EQ(nullOutput.hresult, E_INVALIDARG);
    WXI_REQUIRE_EQ(nullOutput.win32, DWORD{ERROR_INVALID_PARAMETER});
    duplicate.sections.clear();
    duplicate.mode = static_cast<winexinfo::ProbeMode>(99);
    RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
    duplicate.mode = winexinfo::ProbeMode::Observe;
    duplicate.passed = true;
    duplicate.error_code = winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH;
    RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
    duplicate.passed = false;
    duplicate.error_code = winexinfo::ErrorCode::OK;
    RequireMismatch(winexinfo::WriteProbeReport(duplicate, &output));
    WXI_REQUIRE_EQ(output, std::string{"unchanged"});
}

WXI_TEST(event_filter_report_accepts_complete_gate_pass, "event_filter.report_complete_pass") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe, true, {}, winexinfo::ErrorCode::OK};
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(
                    PassingObservation(), &report)
                    .ok());
    std::string output;
    WXI_REQUIRE(winexinfo::WriteProbeReport(report, &output).ok());
    WXI_REQUIRE(output.find("result=pass\n") != std::string::npos);
    WXI_REQUIRE(output.ends_with("error_code=OK\n"));
    std::size_t previous = 0;
    for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
        std::ostringstream prefix;
        prefix << "event." << std::setw(20) << std::setfill('0') << sequence << ".";
        const std::size_t position = output.find(prefix.str());
        WXI_REQUIRE(position != std::string::npos);
        WXI_REQUIRE(position >= previous);
        previous = position;
    }
}

WXI_TEST(event_filter_report_rejects_unresolved_pending_and_missing_terminal_revoke, "event_filter.report_temporal_pass_invariants") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe, true, {}, winexinfo::ErrorCode::OK};
    winexinfo::EventObservationSnapshot unresolved = PassingObservation();
    unresolved.events[0].source_top_level = Handle(0xAAAA);
    unresolved.events[0].generation = 8;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(unresolved, &report));

    winexinfo::EventObservationSnapshot noTerminal = PassingObservation();
    auto& revoked = noTerminal.events[4];
    revoked.transition = winexinfo::ObservedEventTransition::Remapped;
    revoked.current_active_view = Handle(0xCDEF);
    revoked.active_view_count = 1;
    revoked.current_filesystem_path_available = true;
    revoked.current_filesystem_path = L"C:\\work\\child";
    RequireMismatch(winexinfo::AppendEventObservationReportFields(noTerminal, &report));
}

WXI_TEST(event_filter_report_rejects_summary_sequence_and_kind_mismatch, "event_filter.report_invariants") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe,
        true,
        {},
        winexinfo::ErrorCode::OK,
    };
    winexinfo::EventObservationSnapshot snapshot = ExactObservation();
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot.event_count = 2;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    WXI_REQUIRE(report.sections.empty());
    snapshot = ExactObservation();
    snapshot.kind_counts.navigate_complete2 = 0;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    WXI_REQUIRE(report.sections.empty());
    snapshot = ExactObservation();
    snapshot.events[0].sequence = 2;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].generation = 0;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.duration_ms = 999;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.duration_ms = 60001;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    report.mode = winexinfo::ProbeMode::Snapshot;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    report.mode = winexinfo::ProbeMode::Observe;
    report.sections = {{{{"already", "present"}}}};
    report.passed = false;
    report.error_code = winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH;
    snapshot = ExactObservation();
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(snapshot, &report).ok());
    WXI_REQUIRE_EQ(report.sections[0].fields[0].key, std::string{"already"});
    WXI_REQUIRE_EQ(report.sections[0].fields[0].value, std::string{"present"});
    WXI_REQUIRE_EQ(report.sections.size(), std::size_t{4});
    report.sections.clear();
    snapshot.cleanup_status = {winexinfo::ErrorCode::OK, E_FAIL, 0};
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot.cleanup_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        S_OK,
        0,
    };
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    const winexinfo::Status nullReport =
        winexinfo::AppendEventObservationReportFields(snapshot, nullptr);
    WXI_REQUIRE_EQ(nullReport.hresult, E_INVALIDARG);

    winexinfo::ProbeReport sentinel{
        winexinfo::ProbeMode::Observe,
        false,
        {{{{"sentinel", "unchanged"}}}},
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
    };
    snapshot = ExactObservation();
    snapshot.duration_ms = 999;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &sentinel));
    WXI_REQUIRE_EQ(sentinel.sections.size(), std::size_t{1});
    WXI_REQUIRE_EQ(sentinel.sections[0].fields.size(), std::size_t{1});
    WXI_REQUIRE_EQ(sentinel.sections[0].fields[0].key, std::string{"sentinel"});
    WXI_REQUIRE_EQ(sentinel.sections[0].fields[0].value, std::string{"unchanged"});
}

WXI_TEST(event_filter_report_preserves_runtime_and_cleanup_status, "event_filter.report_runtime_status") {
    winexinfo::EventObservationSnapshot runtimeFailure = PassingObservation();
    runtimeFailure.runtime_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        RPC_E_DISCONNECTED,
        ERROR_BROKEN_PIPE,
    };

    winexinfo::ProbeReport failedReport{
        winexinfo::ProbeMode::Observe,
        false,
        {},
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
    };
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(
                    runtimeFailure, &failedReport)
                    .ok());
    std::string serialized;
    WXI_REQUIRE(winexinfo::WriteProbeReport(failedReport, &serialized).ok());
    WXI_REQUIRE(serialized.find(
                    "observe.runtime.error_code=ACTIVE_VIEW_CONTRACT_MISMATCH\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find(
                    "observe.runtime.hresult=" +
                    std::to_string(RPC_E_DISCONNECTED) + "\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find(
                    "observe.runtime.win32=" +
                    std::to_string(ERROR_BROKEN_PIPE) + "\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find("observe.cleanup.error_code=OK\n") !=
                std::string::npos);

    winexinfo::ProbeReport passReport{
        winexinfo::ProbeMode::Observe,
        true,
        {},
        winexinfo::ErrorCode::OK,
    };
    RequireMismatch(winexinfo::AppendEventObservationReportFields(
        runtimeFailure, &passReport));
    WXI_REQUIRE(passReport.sections.empty());

    winexinfo::EventObservationSnapshot doubleFailure = runtimeFailure;
    doubleFailure.cleanup_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_ACCESSDENIED,
        ERROR_ACCESS_DENIED,
    };
    failedReport.sections.clear();
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(
                    doubleFailure, &failedReport)
                    .ok());
    serialized.clear();
    WXI_REQUIRE(winexinfo::WriteProbeReport(failedReport, &serialized).ok());
    WXI_REQUIRE(serialized.find(
                    "observe.runtime.error_code=ACTIVE_VIEW_CONTRACT_MISMATCH\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find(
                    "observe.runtime.hresult=" +
                    std::to_string(RPC_E_DISCONNECTED) + "\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find(
                    "observe.runtime.win32=" +
                    std::to_string(ERROR_BROKEN_PIPE) + "\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find(
                    "observe.cleanup.error_code=ACTIVE_VIEW_CONTRACT_MISMATCH\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find(
                    "observe.cleanup.hresult=" +
                    std::to_string(E_ACCESSDENIED) + "\n") !=
                std::string::npos);
    WXI_REQUIRE(serialized.find(
                    "observe.cleanup.win32=" +
                    std::to_string(ERROR_ACCESS_DENIED) + "\n") !=
                std::string::npos);

    winexinfo::ProbeReport sentinel{
        winexinfo::ProbeMode::Observe,
        false,
        {{{{"sentinel", "unchanged"}}}},
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
    };
    winexinfo::EventObservationSnapshot incoherent = PassingObservation();
    incoherent.runtime_status = {winexinfo::ErrorCode::OK, E_FAIL, 0};
    RequireMismatch(winexinfo::AppendEventObservationReportFields(
        incoherent, &sentinel));
    WXI_REQUIRE_EQ(sentinel.sections.size(), std::size_t{1});
    WXI_REQUIRE_EQ(sentinel.sections[0].fields[0].key, std::string{"sentinel"});
    incoherent.runtime_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        S_OK,
        0,
    };
    RequireMismatch(winexinfo::AppendEventObservationReportFields(
        incoherent, &sentinel));
    WXI_REQUIRE_EQ(sentinel.sections.size(), std::size_t{1});
    incoherent.runtime_status = {
        winexinfo::ErrorCode::INVALID_ARGUMENT,
        E_INVALIDARG,
        ERROR_INVALID_PARAMETER,
    };
    RequireMismatch(winexinfo::AppendEventObservationReportFields(
        incoherent, &sentinel));
    WXI_REQUIRE_EQ(sentinel.sections.size(), std::size_t{1});

    sentinel.sections = {{{{"observe.runtime.error_code", "forged"}}}};
    RequireMismatch(winexinfo::AppendEventObservationReportFields(
        runtimeFailure, &sentinel));
    WXI_REQUIRE_EQ(sentinel.sections.size(), std::size_t{1});
    WXI_REQUIRE_EQ(
        sentinel.sections[0].fields[0].value,
        std::string{"forged"});
}

WXI_TEST(event_filter_report_rejects_record_shape_and_invalid_utf16, "event_filter.report_record_shape") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe,
        true,
        {},
        winexinfo::ErrorCode::OK,
    };
    winexinfo::EventObservationSnapshot snapshot = ExactObservation();
    snapshot.events[0].active_view_count = 2;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].current_filesystem_path_available = false;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].current_filesystem_path =
        std::wstring{static_cast<wchar_t>(0xD800)};
    const winexinfo::Status invalidUtf16 =
        winexinfo::AppendEventObservationReportFields(snapshot, &report);
    WXI_REQUIRE_EQ(
        invalidUtf16.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(invalidUtf16.win32, DWORD{ERROR_NO_UNICODE_TRANSLATION});
    WXI_REQUIRE(report.sections.empty());
    snapshot = ExactObservation();
    snapshot.events[0].previous_filesystem_path =
        std::wstring{static_cast<wchar_t>(0xD800)};
    const winexinfo::Status invalidPreviousUtf16 =
        winexinfo::AppendEventObservationReportFields(snapshot, &report);
    WXI_REQUIRE_EQ(
        invalidPreviousUtf16.win32,
        DWORD{ERROR_NO_UNICODE_TRANSLATION});
    WXI_REQUIRE(report.sections.empty());
}

WXI_TEST(event_filter_report_validates_transition_matrix, "event_filter.report_transition_matrix") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe,
        false,
        {},
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH};
    winexinfo::EventObservationSnapshot snapshot = ExactObservation();
    auto& event = snapshot.events[0];
    event.kind = winexinfo::ObservedEventKind::WindowRegistered;
    event.transition = winexinfo::ObservedEventTransition::Pending;
    event.shell_cookie_present = true;
    event.shell_cookie = 41;
    event.current_active_view = nullptr;
    event.active_view_count = 0;
    event.current_filesystem_path_available = false;
    event.current_filesystem_path.clear();
    snapshot.kind_counts = {1, 0, 0, 0, 0};
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(snapshot, &report).ok());

    report.sections.clear();
    event.kind = winexinfo::ObservedEventKind::WindowRevoked;
    event.transition = winexinfo::ObservedEventTransition::Revoked;
    snapshot.kind_counts = {0, 1, 0, 0, 0};
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(snapshot, &report).ok());

    report.sections.clear();
    snapshot = ExactObservation();
    snapshot.events[0].kind = winexinfo::ObservedEventKind::TabSelected;
    snapshot.events[0].transition = winexinfo::ObservedEventTransition::Mismatch;
    snapshot.events[0].source_shell_tab_present = false;
    snapshot.events[0].source_shell_tab = nullptr;
    snapshot.events[0].source_was_active = false;
    snapshot.events[0].current_active_view = nullptr;
    snapshot.events[0].active_view_count = 2;
    snapshot.events[0].current_filesystem_path_available = false;
    snapshot.events[0].current_filesystem_path.clear();
    snapshot.events[0].status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        S_FALSE,
        0,
    };
    snapshot.kind_counts = {0, 0, 0, 1, 0};
    WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(snapshot, &report).ok());

    auto requireAccepted = [&](winexinfo::EventObservationSnapshot value) {
        report.sections.clear();
        WXI_REQUIRE(winexinfo::AppendEventObservationReportFields(value, &report).ok());
    };
    auto makeMismatch = [](winexinfo::EventObservationSnapshot value) {
        auto& valueEvent = value.events[0];
        valueEvent.transition = winexinfo::ObservedEventTransition::Mismatch;
        valueEvent.current_active_view = nullptr;
        valueEvent.active_view_count = 2;
        valueEvent.current_filesystem_path_available = false;
        valueEvent.current_filesystem_path.clear();
        valueEvent.status = {
            winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            S_FALSE,
            0,
        };
        return value;
    };

    snapshot = ExactObservation();
    snapshot.events[0].kind = winexinfo::ObservedEventKind::WindowRegistered;
    snapshot.events[0].shell_cookie_present = true;
    snapshot.events[0].shell_cookie = 41;
    snapshot.kind_counts = {1, 0, 0, 0, 0};
    requireAccepted(snapshot);
    requireAccepted(makeMismatch(snapshot));

    snapshot = ExactObservation();
    snapshot.events[0].kind = winexinfo::ObservedEventKind::WindowRevoked;
    snapshot.events[0].shell_cookie_present = true;
    snapshot.events[0].shell_cookie = 41;
    snapshot.kind_counts = {0, 1, 0, 0, 0};
    report.sections.clear();
    RequireMismatch(winexinfo::AppendEventObservationReportFields(
        snapshot, &report));
    requireAccepted(makeMismatch(snapshot));

    snapshot = ExactObservation();
    snapshot.kind_counts = {0, 0, 1, 0, 0};
    requireAccepted(makeMismatch(snapshot));

    snapshot = ExactObservation();
    snapshot.events[0].kind = winexinfo::ObservedEventKind::TabSelected;
    snapshot.events[0].source_shell_tab_present = false;
    snapshot.events[0].source_shell_tab = nullptr;
    snapshot.events[0].source_was_active = false;
    snapshot.kind_counts = {0, 0, 0, 1, 0};
    requireAccepted(snapshot);

    const winexinfo::ObservedStructureChangeType structureTypes[] = {
        winexinfo::ObservedStructureChangeType::ChildAdded,
        winexinfo::ObservedStructureChangeType::ChildRemoved,
        winexinfo::ObservedStructureChangeType::ChildrenInvalidated,
        winexinfo::ObservedStructureChangeType::ChildrenBulkAdded,
        winexinfo::ObservedStructureChangeType::ChildrenBulkRemoved,
        winexinfo::ObservedStructureChangeType::ChildrenReordered,
    };
    for (const auto structureType : structureTypes) {
        snapshot = ExactObservation();
        snapshot.events[0].kind =
            winexinfo::ObservedEventKind::TabStructureChanged;
        snapshot.events[0].structure_change_type = structureType;
        snapshot.kind_counts = {0, 0, 0, 0, 1};
        requireAccepted(snapshot);
        std::string serialized;
        WXI_REQUIRE(winexinfo::WriteProbeReport(report, &serialized).ok());
        WXI_REQUIRE(serialized.find("structure_change_type=") != std::string::npos);
    }
}

WXI_TEST(event_filter_report_rejects_source_and_event_metadata_mismatch, "event_filter.report_metadata_invariants") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe, true, {}, winexinfo::ErrorCode::OK};
    winexinfo::EventObservationSnapshot snapshot = ExactObservation();
    snapshot.events[0].source_top_level = nullptr;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].source_shell_tab_present = false;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].shell_cookie = 1;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].shell_cookie_present = true;
    snapshot.events[0].shell_cookie = 1;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].kind = winexinfo::ObservedEventKind::WindowRegistered;
    snapshot.events[0].transition = winexinfo::ObservedEventTransition::Pending;
    snapshot.events[0].current_active_view = nullptr;
    snapshot.events[0].active_view_count = 0;
    snapshot.events[0].current_filesystem_path_available = false;
    snapshot.events[0].current_filesystem_path.clear();
    snapshot.kind_counts = {1, 0, 0, 0, 0};
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].kind = winexinfo::ObservedEventKind::TabStructureChanged;
    snapshot.events[0].structure_change_type =
        winexinfo::ObservedStructureChangeType::None;
    snapshot.kind_counts = {0, 0, 0, 0, 1};
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].structure_change_type =
        winexinfo::ObservedStructureChangeType::ChildAdded;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].previous_filesystem_path_available = false;
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
}

WXI_TEST(event_filter_report_rejects_invalid_enums_and_status, "event_filter.report_enum_status_invariants") {
    winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe, true, {}, winexinfo::ErrorCode::OK};
    winexinfo::EventObservationSnapshot snapshot = ExactObservation();
    snapshot.events[0].kind = static_cast<winexinfo::ObservedEventKind>(99);
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].transition =
        static_cast<winexinfo::ObservedEventTransition>(99);
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].structure_change_type =
        static_cast<winexinfo::ObservedStructureChangeType>(99);
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].status = {winexinfo::ErrorCode::OK, E_FAIL, 0};
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
    snapshot = ExactObservation();
    snapshot.events[0].status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        S_OK,
        0,
    };
    RequireMismatch(winexinfo::AppendEventObservationReportFields(snapshot, &report));
}
