#pragma once

#include "common/status.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace winexinfo {

enum class UiaQueryScope {
    ActiveViewSubtree,
    StatusBarChildren,
    ExplorerSubtree,
    TabViewChildren,
};

struct UiaElementEvidence final {
    UiaQueryScope scope;
    std::wstring framework_id;
    CONTROLTYPEID control_type;
    std::wstring automation_id;
    std::wstring class_name;
    std::wstring name;
    DWORD process_id;
    UIA_HWND native_window_handle;
    RECT bounds;
    bool offscreen;
};

struct UiaContractEvidence final {
    Status transport_status;
    std::vector<UiaElementEvidence> elements;
};

struct UiaSelectorCardinalities final {
    std::size_t status_bar;
    std::size_t left_group;
    std::size_t right_group;
    std::size_t tab_view;
    std::size_t tab_list;
};

struct UiaContractSnapshot final {
    UiaElementEvidence status_bar;
    UiaElementEvidence left_group;
    UiaElementEvidence right_group;
    UiaElementEvidence tab_view;
    UiaElementEvidence tab_list;
    UiaSelectorCardinalities cardinalities;
};

struct UiaEventCacheContract final {
    AutomationElementMode element_mode;
    TreeScope tree_scope;
    std::array<PROPERTYID, 6> properties;
};

struct RetainedUiaContractCapture final {
    UiaContractEvidence evidence;
    UiaContractSnapshot snapshot;
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    Microsoft::WRL::ComPtr<IUIAutomationElement> tab_list_element;
    DWORD owner_thread_id;
};

struct RetainedUiaAccessEvidence final {
    DWORD owner_thread_id;
    DWORD current_thread_id;
    bool automation_present;
    bool tab_list_element_present;
    UiaElementEvidence tab_list;
};

struct UiaTabChildEvidence final {
    std::wstring framework_id;
    CONTROLTYPEID control_type;
    std::wstring automation_id;
    std::wstring class_name;
    DWORD process_id;
    bool offscreen;
};

[[nodiscard]] Status ValidateUiaContract(
    const UiaContractEvidence& evidence,
    UiaContractSnapshot* output);
[[nodiscard]] UiaEventCacheContract GetExactUiaEventCacheContract();
[[nodiscard]] Status ClassifyExactUiaCallResult(
    HRESULT hresult,
    bool requiredObjectPresent);
[[nodiscard]] Status ClassifyLegacyUiaCaptureCallResult(
    HRESULT hresult,
    bool requiredObjectPresent);
[[nodiscard]] Status CreateUiaEventCacheRequest(
    IUIAutomation* automation,
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest>* output);
[[nodiscard]] Status CaptureRetainedUiaContract(
    IUIAutomation* automation,
    HWND topLevel,
    HWND activeView,
    RetainedUiaContractCapture* output);
[[nodiscard]] Status ValidateRetainedUiaAccess(
    const RetainedUiaAccessEvidence& evidence);
[[nodiscard]] Status ValidateTabListDirectChildren(
    DWORD expectedProcessId,
    std::span<const UiaTabChildEvidence> children,
    std::size_t* outputCount);
[[nodiscard]] Status ReenumerateRetainedTabListDirectChildren(
    const RetainedUiaContractCapture& capture,
    std::size_t* outputCount);
[[nodiscard]] Status CaptureUiaContractEvidence(
    HWND topLevel,
    HWND activeView,
    UiaContractEvidence* output);

}  // namespace winexinfo
