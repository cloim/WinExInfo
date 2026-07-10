#pragma once

#include "common/status.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomation.h>

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

struct UiaContractSnapshot final {
    UiaElementEvidence status_bar;
    UiaElementEvidence left_group;
    UiaElementEvidence right_group;
    UiaElementEvidence tab_view;
    UiaElementEvidence tab_list;
};

[[nodiscard]] Status ValidateUiaContract(
    const UiaContractEvidence& evidence,
    UiaContractSnapshot* output);
[[nodiscard]] Status CaptureUiaContractEvidence(
    HWND topLevel,
    HWND activeView,
    UiaContractEvidence* output);

}  // namespace winexinfo
