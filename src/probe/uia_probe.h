#pragma once

#include "common/status.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomation.h>

#include <cstddef>
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

[[nodiscard]] Status ValidateUiaContract(
    const UiaContractEvidence& evidence,
    UiaContractSnapshot* output);
[[nodiscard]] Status CaptureUiaContractEvidence(
    HWND topLevel,
    HWND activeView,
    UiaContractEvidence* output);

}  // namespace winexinfo
