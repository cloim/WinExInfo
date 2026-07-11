#include "probe/uia_probe.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace winexinfo {
namespace {

std::vector<const UiaElementEvidence*> FindMatches(
    const UiaContractEvidence& evidence,
    const std::function<bool(const UiaElementEvidence&)>& matches) {
    std::vector<const UiaElementEvidence*> found;
    for (const UiaElementEvidence& element : evidence.elements) {
        if (matches(element)) {
            found.push_back(&element);
        }
    }
    return found;
}

Status UiaTransportStatus(const HRESULT hresult) {
    const DWORD win32 = HRESULT_FACILITY(hresult) == FACILITY_WIN32
        ? static_cast<DWORD>(HRESULT_CODE(hresult))
        : ERROR_SUCCESS;
    return {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, hresult, win32};
}

Status ActiveUiaStatus(const HRESULT hresult) {
    const DWORD win32 = HRESULT_FACILITY(hresult) == FACILITY_WIN32
        ? static_cast<DWORD>(HRESULT_CODE(hresult))
        : ERROR_SUCCESS;
    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, win32};
}

Status ClassifyCapturedUiaCallResult(
    const HRESULT hresult,
    const bool requiredObjectPresent,
    const bool exactHresults) {
    if (exactHresults) {
        return ClassifyExactUiaCallResult(hresult, requiredObjectPresent);
    }
    return ClassifyLegacyUiaCaptureCallResult(
        hresult, requiredObjectPresent);
}

Status AppendElementArray(
    IUIAutomationElementArray* const array,
    const UiaQueryScope scope,
    UiaContractEvidence* const evidence,
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>>* const rawElements,
    const bool exactHresults) {
    int length = 0;
    HRESULT hresult = array->get_Length(&length);
    Status status =
        ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
    if (!status.ok()) {
        return status;
    }
    if (exactHresults && length < 0) {
        return ActiveUiaStatus(S_FALSE);
    }

    for (int index = 0; index < length; ++index) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        hresult = array->GetElement(index, &element);
        status = ClassifyCapturedUiaCallResult(
            hresult, element != nullptr, exactHresults);
        if (!status.ok()) {
            return status;
        }

        BSTR framework = nullptr;
        hresult = element->get_CachedFrameworkId(&framework);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            SysFreeString(framework);
            return status;
        }
        std::wstring frameworkText;
        if (framework != nullptr) {
            frameworkText.assign(framework, SysStringLen(framework));
        }
        SysFreeString(framework);

        CONTROLTYPEID controlType = 0;
        hresult = element->get_CachedControlType(&controlType);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            return status;
        }

        BSTR automationId = nullptr;
        hresult = element->get_CachedAutomationId(&automationId);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            SysFreeString(automationId);
            return status;
        }
        std::wstring automationIdText;
        if (automationId != nullptr) {
            automationIdText.assign(automationId, SysStringLen(automationId));
        }
        SysFreeString(automationId);

        BSTR className = nullptr;
        hresult = element->get_CachedClassName(&className);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            SysFreeString(className);
            return status;
        }
        std::wstring classNameText;
        if (className != nullptr) {
            classNameText.assign(className, SysStringLen(className));
        }
        SysFreeString(className);

        int processId = 0;
        hresult = element->get_CachedProcessId(&processId);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            return status;
        }
        UIA_HWND nativeHwnd = nullptr;
        hresult = element->get_CachedNativeWindowHandle(&nativeHwnd);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            return status;
        }
        RECT bounds{};
        hresult = element->get_CachedBoundingRectangle(&bounds);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            return status;
        }
        BOOL offscreen = FALSE;
        hresult = element->get_CachedIsOffscreen(&offscreen);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            return status;
        }

        evidence->elements.push_back({
            scope,
            std::move(frameworkText),
            controlType,
            std::move(automationIdText),
            std::move(classNameText),
            L"",
            static_cast<DWORD>(processId),
            nativeHwnd,
            bounds,
            offscreen != FALSE,
        });
        rawElements->push_back(std::move(element));
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace

Status ValidateUiaContract(
    const UiaContractEvidence& evidence,
    UiaContractSnapshot* const output) {
    if (!evidence.transport_status.ok()) {
        return evidence.transport_status;
    }
    if (output == nullptr) {
        return {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, E_POINTER, ERROR_INVALID_PARAMETER};
    }

    const auto statusBars = FindMatches(evidence, [](const UiaElementEvidence& element) {
        return element.scope == UiaQueryScope::ActiveViewSubtree &&
            element.framework_id == L"DirectUI" &&
            element.control_type == UIA_StatusBarControlTypeId &&
            element.automation_id == L"StatusBarModuleInner" &&
            element.class_name == L"StatusBarModuleInner" && element.native_window_handle == 0;
    });
    const auto leftGroups = FindMatches(evidence, [](const UiaElementEvidence& element) {
        return element.scope == UiaQueryScope::StatusBarChildren &&
            element.control_type == UIA_GroupControlTypeId &&
            element.automation_id == L"System.StatusBarViewItemCount" &&
            element.native_window_handle == 0;
    });
    const auto rightGroups = FindMatches(evidence, [](const UiaElementEvidence& element) {
        return element.scope == UiaQueryScope::StatusBarChildren &&
            element.control_type == UIA_GroupControlTypeId &&
            element.automation_id == L"ViewButtonsGroup" && element.native_window_handle == 0;
    });
    const auto tabViews = FindMatches(evidence, [](const UiaElementEvidence& element) {
        return element.scope == UiaQueryScope::ExplorerSubtree &&
            element.framework_id == L"XAML" && element.control_type == UIA_TabControlTypeId &&
            element.automation_id == L"TabView" &&
            element.class_name == L"Microsoft.UI.Xaml.Controls.TabView";
    });
    const auto tabLists = FindMatches(evidence, [](const UiaElementEvidence& element) {
        return element.scope == UiaQueryScope::TabViewChildren && element.framework_id == L"XAML" &&
            element.control_type == UIA_ListControlTypeId &&
            element.automation_id == L"TabListView" && element.class_name == L"ListView";
    });

    output->cardinalities = {
        statusBars.size(),
        leftGroups.size(),
        rightGroups.size(),
        tabViews.size(),
        tabLists.size(),
    };

    if (statusBars.size() != 1 || leftGroups.size() != 1 || rightGroups.size() != 1 ||
        tabViews.size() != 1 || tabLists.size() != 1) {
        return {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, S_FALSE, ERROR_SUCCESS};
    }

    output->status_bar = *statusBars[0];
    output->left_group = *leftGroups[0];
    output->right_group = *rightGroups[0];
    output->tab_view = *tabViews[0];
    output->tab_list = *tabLists[0];
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

UiaEventCacheContract GetExactUiaEventCacheContract() {
    return {
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

Status ClassifyExactUiaCallResult(
    const HRESULT hresult,
    const bool requiredObjectPresent) {
    if (FAILED(hresult)) {
        return ActiveUiaStatus(hresult);
    }
    if (hresult != S_OK || !requiredObjectPresent) {
        return ActiveUiaStatus(S_FALSE);
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ClassifyLegacyUiaCaptureCallResult(
    const HRESULT hresult,
    const bool requiredObjectPresent) {
    if (FAILED(hresult)) {
        return UiaTransportStatus(hresult);
    }
    if (!requiredObjectPresent) {
        return UiaTransportStatus(S_FALSE);
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status CreateUiaEventCacheRequest(
    IUIAutomation* const automation,
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest>* const output) {
    if (automation == nullptr || output == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> request;
    HRESULT hresult = automation->CreateCacheRequest(&request);
    Status status = ClassifyExactUiaCallResult(hresult, request != nullptr);
    if (!status.ok()) {
        return status;
    }
    const UiaEventCacheContract contract = GetExactUiaEventCacheContract();
    hresult = request->put_AutomationElementMode(contract.element_mode);
    status = ClassifyExactUiaCallResult(hresult, true);
    if (!status.ok()) {
        return status;
    }
    hresult = request->put_TreeScope(contract.tree_scope);
    status = ClassifyExactUiaCallResult(hresult, true);
    if (!status.ok()) {
        return status;
    }
    for (const PROPERTYID property : contract.properties) {
        hresult = request->AddProperty(property);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return status;
        }
    }

    *output = std::move(request);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ValidateRetainedUiaAccess(
    const RetainedUiaAccessEvidence& evidence) {
    const UiaElementEvidence& tabList = evidence.tab_list;
    if (evidence.owner_thread_id != 0 && evidence.current_thread_id != 0 &&
        evidence.owner_thread_id != evidence.current_thread_id) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            RPC_E_WRONG_THREAD,
            ERROR_SUCCESS,
        };
    }
    if (evidence.owner_thread_id == 0 || evidence.current_thread_id == 0 ||
        !evidence.automation_present || !evidence.tab_list_element_present ||
        tabList.scope != UiaQueryScope::TabViewChildren ||
        tabList.framework_id != L"XAML" ||
        tabList.control_type != UIA_ListControlTypeId ||
        tabList.automation_id != L"TabListView" ||
        tabList.class_name != L"ListView" || tabList.process_id == 0) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            S_FALSE,
            ERROR_SUCCESS,
        };
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ValidateTabListDirectChildren(
    const DWORD expectedProcessId,
    const std::span<const UiaTabChildEvidence> children,
    std::size_t* const outputCount) {
    if (expectedProcessId == 0 || outputCount == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }
    if (std::ranges::any_of(
            children,
            [&](const UiaTabChildEvidence& child) {
                return child.framework_id != L"XAML" ||
                    child.control_type != UIA_TabItemControlTypeId ||
                    child.class_name != L"ListViewItem" ||
                    child.process_id != expectedProcessId;
            })) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            S_FALSE,
            ERROR_SUCCESS,
        };
    }

    *outputCount = children.size();
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

namespace {

Status CaptureUiaContractEvidenceWithAutomation(
    IUIAutomation* const automation,
    const HWND topLevel,
    const HWND activeView,
    UiaContractEvidence* const output,
    Microsoft::WRL::ComPtr<IUIAutomationElement>* const exactTabList,
    const bool exactHresults) {
    if (automation == nullptr || topLevel == nullptr || activeView == nullptr ||
        output == nullptr || exactTabList == nullptr) {
        return exactHresults
            ? ActiveUiaStatus(E_INVALIDARG)
            : UiaTransportStatus(E_INVALIDARG);
    }

    UiaContractEvidence evidence{{ErrorCode::OK, S_OK, ERROR_SUCCESS}, {}};
    const auto preserveFailure = [&](const Status& failure) {
        evidence.transport_status = failure;
        *output = std::move(evidence);
        return output->transport_status;
    };
    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cacheRequest;
    HRESULT hresult = automation->CreateCacheRequest(&cacheRequest);
    Status status = ClassifyCapturedUiaCallResult(
        hresult, cacheRequest != nullptr, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }
    hresult = cacheRequest->put_AutomationElementMode(AutomationElementMode_Full);
    status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }
    hresult = cacheRequest->put_TreeScope(TreeScope_Element);
    status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }
    const PROPERTYID properties[] = {
        UIA_FrameworkIdPropertyId,
        UIA_ControlTypePropertyId,
        UIA_ClassNamePropertyId,
        UIA_AutomationIdPropertyId,
        UIA_ProcessIdPropertyId,
        UIA_NativeWindowHandlePropertyId,
        UIA_BoundingRectanglePropertyId,
        UIA_IsOffscreenPropertyId,
    };
    for (const PROPERTYID property : properties) {
        hresult = cacheRequest->AddProperty(property);
        status = ClassifyCapturedUiaCallResult(hresult, true, exactHresults);
        if (!status.ok()) {
            return preserveFailure(status);
        }
    }

    Microsoft::WRL::ComPtr<IUIAutomationCondition> trueCondition;
    hresult = automation->CreateTrueCondition(&trueCondition);
    status = ClassifyCapturedUiaCallResult(
        hresult, trueCondition != nullptr, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> activeViewRoot;
    hresult = automation->ElementFromHandleBuildCache(activeView, cacheRequest.Get(), &activeViewRoot);
    status = ClassifyCapturedUiaCallResult(
        hresult, activeViewRoot != nullptr, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> statusScope;
    hresult = activeViewRoot->FindAllBuildCache(
        TreeScope_Subtree,
        trueCondition.Get(),
        cacheRequest.Get(),
        &statusScope);
    status = ClassifyCapturedUiaCallResult(
        hresult, statusScope != nullptr, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> statusRaw;
    status = AppendElementArray(
        statusScope.Get(),
        UiaQueryScope::ActiveViewSubtree,
        &evidence,
        &statusRaw,
        exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> selectedStatus;
    std::size_t statusMatches = 0;
    for (std::size_t index = 0; index < statusRaw.size(); ++index) {
        const UiaElementEvidence& element = evidence.elements[index];
        if (element.framework_id == L"DirectUI" &&
            element.control_type == UIA_StatusBarControlTypeId &&
            element.automation_id == L"StatusBarModuleInner" &&
            element.class_name == L"StatusBarModuleInner" && element.native_window_handle == 0) {
            ++statusMatches;
            selectedStatus = statusRaw[index];
        }
    }
    if (statusMatches == 1) {
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> groupScope;
        hresult = selectedStatus->FindAllBuildCache(
            TreeScope_Children,
            trueCondition.Get(),
            cacheRequest.Get(),
            &groupScope);
        status = ClassifyCapturedUiaCallResult(
            hresult, groupScope != nullptr, exactHresults);
        if (!status.ok()) {
            return preserveFailure(status);
        }
        std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> groupRaw;
        status = AppendElementArray(
            groupScope.Get(),
            UiaQueryScope::StatusBarChildren,
            &evidence,
            &groupRaw,
            exactHresults);
        if (!status.ok()) {
            return preserveFailure(status);
        }
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> explorerRoot;
    hresult = automation->ElementFromHandleBuildCache(topLevel, cacheRequest.Get(), &explorerRoot);
    status = ClassifyCapturedUiaCallResult(
        hresult, explorerRoot != nullptr, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> tabScope;
    hresult = explorerRoot->FindAllBuildCache(
        TreeScope_Subtree,
        trueCondition.Get(),
        cacheRequest.Get(),
        &tabScope);
    status = ClassifyCapturedUiaCallResult(
        hresult, tabScope != nullptr, exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }
    const std::size_t tabEvidenceStart = evidence.elements.size();
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> tabRaw;
    status = AppendElementArray(
        tabScope.Get(),
        UiaQueryScope::ExplorerSubtree,
        &evidence,
        &tabRaw,
        exactHresults);
    if (!status.ok()) {
        return preserveFailure(status);
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> selectedTab;
    std::size_t tabMatches = 0;
    for (std::size_t index = 0; index < tabRaw.size(); ++index) {
        const UiaElementEvidence& element = evidence.elements[tabEvidenceStart + index];
        if (element.framework_id == L"XAML" && element.control_type == UIA_TabControlTypeId &&
            element.automation_id == L"TabView" &&
            element.class_name == L"Microsoft.UI.Xaml.Controls.TabView") {
            ++tabMatches;
            selectedTab = tabRaw[index];
        }
    }
    Microsoft::WRL::ComPtr<IUIAutomationElement> selectedExactTabList;
    if (tabMatches == 1) {
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> tabListScope;
        hresult = selectedTab->FindAllBuildCache(
            TreeScope_Children,
            trueCondition.Get(),
            cacheRequest.Get(),
            &tabListScope);
        status = ClassifyCapturedUiaCallResult(
            hresult, tabListScope != nullptr, exactHresults);
        if (!status.ok()) {
            return preserveFailure(status);
        }
        const std::size_t tabListEvidenceStart = evidence.elements.size();
        std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> tabListRaw;
        status = AppendElementArray(
            tabListScope.Get(),
            UiaQueryScope::TabViewChildren,
            &evidence,
            &tabListRaw,
            exactHresults);
        if (!status.ok()) {
            return preserveFailure(status);
        }
        std::size_t tabListMatches = 0;
        for (std::size_t index = 0; index < tabListRaw.size(); ++index) {
            const UiaElementEvidence& element =
                evidence.elements[tabListEvidenceStart + index];
            if (element.framework_id == L"XAML" &&
                element.control_type == UIA_ListControlTypeId &&
                element.automation_id == L"TabListView" &&
                element.class_name == L"ListView") {
                ++tabListMatches;
                selectedExactTabList = tabListRaw[index];
            }
        }
        if (tabListMatches != 1) {
            selectedExactTabList.Reset();
        }
    }

    *output = std::move(evidence);
    *exactTabList = std::move(selectedExactTabList);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace

Status CaptureUiaContractEvidence(
    const HWND topLevel,
    const HWND activeView,
    UiaContractEvidence* const output) {
    if (topLevel == nullptr || activeView == nullptr || output == nullptr) {
        return {
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    const HRESULT hresult = CoCreateInstance(
        CLSID_CUIAutomation8,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(hresult)) {
        UiaContractEvidence evidence{UiaTransportStatus(hresult), {}};
        *output = std::move(evidence);
        return output->transport_status;
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> exactTabList;
    return CaptureUiaContractEvidenceWithAutomation(
        automation.Get(), topLevel, activeView, output, &exactTabList, false);
}

Status CaptureRetainedUiaContract(
    IUIAutomation* const automation,
    const HWND topLevel,
    const HWND activeView,
    RetainedUiaContractCapture* const output) {
    if (automation == nullptr || topLevel == nullptr || activeView == nullptr ||
        output == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    RetainedUiaContractCapture candidate{};
    Microsoft::WRL::ComPtr<IUIAutomationElement> exactTabList;
    const Status captureStatus = CaptureUiaContractEvidenceWithAutomation(
        automation,
        topLevel,
        activeView,
        &candidate.evidence,
        &exactTabList,
        true);
    if (!captureStatus.ok()) {
        return captureStatus;
    }
    const Status validationStatus =
        ValidateUiaContract(candidate.evidence, &candidate.snapshot);
    if (!validationStatus.ok()) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            validationStatus.hresult,
            validationStatus.win32,
        };
    }
    candidate.automation = automation;
    candidate.tab_list_element = std::move(exactTabList);
    candidate.owner_thread_id = GetCurrentThreadId();
    const Status accessStatus = ValidateRetainedUiaAccess({
        candidate.owner_thread_id,
        candidate.owner_thread_id,
        candidate.automation != nullptr,
        candidate.tab_list_element != nullptr,
        candidate.snapshot.tab_list,
    });
    if (!accessStatus.ok()) {
        return accessStatus;
    }

    *output = std::move(candidate);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

namespace {

enum class CachedTabChildString {
    FrameworkId,
    AutomationId,
    ClassName,
};

Status ReadCachedTabChildString(
    IUIAutomationElement* const element,
    const CachedTabChildString property,
    std::wstring* const output) {
    BSTR value = nullptr;
    HRESULT hresult = E_INVALIDARG;
    switch (property) {
        case CachedTabChildString::FrameworkId:
            hresult = element->get_CachedFrameworkId(&value);
            break;
        case CachedTabChildString::AutomationId:
            hresult = element->get_CachedAutomationId(&value);
            break;
        case CachedTabChildString::ClassName:
            hresult = element->get_CachedClassName(&value);
            break;
    }
    const Status status = ClassifyExactUiaCallResult(hresult, true);
    if (!status.ok()) {
        SysFreeString(value);
        return status;
    }
    std::wstring text;
    if (value != nullptr) {
        text.assign(value, SysStringLen(value));
    }
    SysFreeString(value);
    *output = std::move(text);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace

Status ReenumerateRetainedTabListDirectChildren(
    const RetainedUiaContractCapture& capture,
    std::size_t* const outputCount) {
    if (outputCount == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    const Status accessStatus = ValidateRetainedUiaAccess({
        capture.owner_thread_id,
        GetCurrentThreadId(),
        capture.automation != nullptr,
        capture.tab_list_element != nullptr,
        capture.snapshot.tab_list,
    });
    if (!accessStatus.ok()) {
        return accessStatus;
    }

    UiaContractSnapshot validated{};
    const Status validationStatus = ValidateUiaContract(capture.evidence, &validated);
    if (!validationStatus.ok()) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            validationStatus.hresult,
            validationStatus.win32,
        };
    }
    const UiaElementEvidence& expected = validated.tab_list;
    const UiaElementEvidence& retained = capture.snapshot.tab_list;
    if (retained.scope != expected.scope ||
        retained.framework_id != expected.framework_id ||
        retained.control_type != expected.control_type ||
        retained.automation_id != expected.automation_id ||
        retained.class_name != expected.class_name ||
        retained.process_id != expected.process_id ||
        retained.native_window_handle != expected.native_window_handle ||
        retained.bounds.left != expected.bounds.left ||
        retained.bounds.top != expected.bounds.top ||
        retained.bounds.right != expected.bounds.right ||
        retained.bounds.bottom != expected.bounds.bottom ||
        retained.offscreen != expected.offscreen) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            S_FALSE,
            ERROR_SUCCESS,
        };
    }

    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cacheRequest;
    Status status =
        CreateUiaEventCacheRequest(capture.automation.Get(), &cacheRequest);
    if (!status.ok()) {
        return status;
    }
    Microsoft::WRL::ComPtr<IUIAutomationCondition> trueCondition;
    HRESULT hresult =
        capture.automation->CreateTrueCondition(&trueCondition);
    status = ClassifyExactUiaCallResult(
        hresult, trueCondition != nullptr);
    if (!status.ok()) {
        return status;
    }

    Microsoft::WRL::ComPtr<IUIAutomationElementArray> childArray;
    hresult = capture.tab_list_element->FindAllBuildCache(
        TreeScope_Children,
        trueCondition.Get(),
        cacheRequest.Get(),
        &childArray);
    status = ClassifyExactUiaCallResult(hresult, childArray != nullptr);
    if (!status.ok()) {
        return status;
    }

    int length = 0;
    hresult = childArray->get_Length(&length);
    status = ClassifyExactUiaCallResult(hresult, true);
    if (!status.ok()) {
        return status;
    }
    if (length < 0) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            S_FALSE,
            ERROR_SUCCESS,
        };
    }

    std::vector<UiaTabChildEvidence> children;
    children.reserve(static_cast<std::size_t>(length));
    for (int index = 0; index < length; ++index) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        hresult = childArray->GetElement(index, &element);
        status = ClassifyExactUiaCallResult(hresult, element != nullptr);
        if (!status.ok()) {
            return status;
        }

        UiaTabChildEvidence child{};
        status = ReadCachedTabChildString(
            element.Get(), CachedTabChildString::FrameworkId, &child.framework_id);
        if (!status.ok()) {
            return status;
        }
        hresult = element->get_CachedControlType(&child.control_type);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return status;
        }
        status = ReadCachedTabChildString(
            element.Get(), CachedTabChildString::ClassName, &child.class_name);
        if (!status.ok()) {
            return status;
        }
        status = ReadCachedTabChildString(
            element.Get(),
            CachedTabChildString::AutomationId,
            &child.automation_id);
        if (!status.ok()) {
            return status;
        }
        int processId = 0;
        hresult = element->get_CachedProcessId(&processId);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return status;
        }
        child.process_id = static_cast<DWORD>(processId);
        BOOL offscreen = FALSE;
        hresult = element->get_CachedIsOffscreen(&offscreen);
        status = ClassifyExactUiaCallResult(hresult, true);
        if (!status.ok()) {
            return status;
        }
        child.offscreen = offscreen != FALSE;
        children.push_back(std::move(child));
    }

    return ValidateTabListDirectChildren(
        retained.process_id, children, outputCount);
}

}  // namespace winexinfo
