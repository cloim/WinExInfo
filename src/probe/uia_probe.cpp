#include "probe/uia_probe.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

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

Status AppendElementArray(
    IUIAutomationElementArray* const array,
    const UiaQueryScope scope,
    UiaContractEvidence* const evidence,
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>>* const rawElements) {
    int length = 0;
    HRESULT hresult = array->get_Length(&length);
    if (FAILED(hresult)) {
        return UiaTransportStatus(hresult);
    }

    for (int index = 0; index < length; ++index) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        hresult = array->GetElement(index, &element);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }

        BSTR framework = nullptr;
        hresult = element->get_CachedFrameworkId(&framework);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }
        std::wstring frameworkText{framework == nullptr ? L"" : framework};
        SysFreeString(framework);

        CONTROLTYPEID controlType = 0;
        hresult = element->get_CachedControlType(&controlType);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }

        BSTR automationId = nullptr;
        hresult = element->get_CachedAutomationId(&automationId);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }
        std::wstring automationIdText{automationId == nullptr ? L"" : automationId};
        SysFreeString(automationId);

        BSTR className = nullptr;
        hresult = element->get_CachedClassName(&className);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }
        std::wstring classNameText{className == nullptr ? L"" : className};
        SysFreeString(className);

        int processId = 0;
        hresult = element->get_CachedProcessId(&processId);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }
        UIA_HWND nativeHwnd = nullptr;
        hresult = element->get_CachedNativeWindowHandle(&nativeHwnd);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }
        RECT bounds{};
        hresult = element->get_CachedBoundingRectangle(&bounds);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
        }
        BOOL offscreen = FALSE;
        hresult = element->get_CachedIsOffscreen(&offscreen);
        if (FAILED(hresult)) {
            return UiaTransportStatus(hresult);
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

    UiaContractEvidence evidence{{ErrorCode::OK, S_OK, ERROR_SUCCESS}, {}};
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    HRESULT hresult = CoCreateInstance(
        CLSID_CUIAutomation8,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(hresult)) {
        evidence.transport_status = UiaTransportStatus(hresult);
        *output = std::move(evidence);
        return output->transport_status;
    }

    Microsoft::WRL::ComPtr<IUIAutomationCacheRequest> cacheRequest;
    hresult = automation->CreateCacheRequest(&cacheRequest);
    if (SUCCEEDED(hresult)) {
        hresult = cacheRequest->put_AutomationElementMode(AutomationElementMode_Full);
    }
    if (SUCCEEDED(hresult)) {
        hresult = cacheRequest->put_TreeScope(TreeScope_Element);
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
        if (SUCCEEDED(hresult)) {
            hresult = cacheRequest->AddProperty(property);
        }
    }
    if (FAILED(hresult)) {
        evidence.transport_status = UiaTransportStatus(hresult);
        *output = std::move(evidence);
        return output->transport_status;
    }

    Microsoft::WRL::ComPtr<IUIAutomationCondition> trueCondition;
    hresult = automation->CreateTrueCondition(&trueCondition);
    if (FAILED(hresult)) {
        evidence.transport_status = UiaTransportStatus(hresult);
        *output = std::move(evidence);
        return output->transport_status;
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> activeViewRoot;
    hresult = automation->ElementFromHandleBuildCache(activeView, cacheRequest.Get(), &activeViewRoot);
    if (FAILED(hresult)) {
        evidence.transport_status = UiaTransportStatus(hresult);
        *output = std::move(evidence);
        return output->transport_status;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> statusScope;
    hresult = activeViewRoot->FindAllBuildCache(
        TreeScope_Subtree,
        trueCondition.Get(),
        cacheRequest.Get(),
        &statusScope);
    if (FAILED(hresult)) {
        evidence.transport_status = UiaTransportStatus(hresult);
        *output = std::move(evidence);
        return output->transport_status;
    }
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> statusRaw;
    Status status = AppendElementArray(
        statusScope.Get(), UiaQueryScope::ActiveViewSubtree, &evidence, &statusRaw);
    if (!status.ok()) {
        evidence.transport_status = status;
        *output = std::move(evidence);
        return output->transport_status;
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
        if (FAILED(hresult)) {
            evidence.transport_status = UiaTransportStatus(hresult);
            *output = std::move(evidence);
            return output->transport_status;
        }
        std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> groupRaw;
        status = AppendElementArray(
            groupScope.Get(), UiaQueryScope::StatusBarChildren, &evidence, &groupRaw);
        if (!status.ok()) {
            evidence.transport_status = status;
            *output = std::move(evidence);
            return output->transport_status;
        }
    }

    Microsoft::WRL::ComPtr<IUIAutomationElement> explorerRoot;
    hresult = automation->ElementFromHandleBuildCache(topLevel, cacheRequest.Get(), &explorerRoot);
    if (FAILED(hresult)) {
        evidence.transport_status = UiaTransportStatus(hresult);
        *output = std::move(evidence);
        return output->transport_status;
    }
    Microsoft::WRL::ComPtr<IUIAutomationElementArray> tabScope;
    hresult = explorerRoot->FindAllBuildCache(
        TreeScope_Subtree,
        trueCondition.Get(),
        cacheRequest.Get(),
        &tabScope);
    if (FAILED(hresult)) {
        evidence.transport_status = UiaTransportStatus(hresult);
        *output = std::move(evidence);
        return output->transport_status;
    }
    const std::size_t tabEvidenceStart = evidence.elements.size();
    std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> tabRaw;
    status = AppendElementArray(
        tabScope.Get(), UiaQueryScope::ExplorerSubtree, &evidence, &tabRaw);
    if (!status.ok()) {
        evidence.transport_status = status;
        *output = std::move(evidence);
        return output->transport_status;
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
    if (tabMatches == 1) {
        Microsoft::WRL::ComPtr<IUIAutomationElementArray> tabListScope;
        hresult = selectedTab->FindAllBuildCache(
            TreeScope_Children,
            trueCondition.Get(),
            cacheRequest.Get(),
            &tabListScope);
        if (FAILED(hresult)) {
            evidence.transport_status = UiaTransportStatus(hresult);
            *output = std::move(evidence);
            return output->transport_status;
        }
        std::vector<Microsoft::WRL::ComPtr<IUIAutomationElement>> tabListRaw;
        status = AppendElementArray(
            tabListScope.Get(), UiaQueryScope::TabViewChildren, &evidence, &tabListRaw);
        if (!status.ok()) {
            evidence.transport_status = status;
            *output = std::move(evidence);
            return output->transport_status;
        }
    }

    *output = std::move(evidence);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace winexinfo
