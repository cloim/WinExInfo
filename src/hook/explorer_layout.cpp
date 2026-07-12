#include "hook/explorer_layout.h"

#include <objbase.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace winexinfo {
namespace {

using Microsoft::WRL::ComPtr;

Status ContractMismatch(const HRESULT hresult, const DWORD win32 = ERROR_SUCCESS) {
    return {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, hresult, win32};
}

Status Win32Mismatch(const DWORD error) {
    return ContractMismatch(HRESULT_FROM_WIN32(error), error);
}

Status RequireExactSuccess(const HRESULT hresult, const bool present = true) {
    if (FAILED(hresult)) {
        const DWORD win32 = HRESULT_FACILITY(hresult) == FACILITY_WIN32
            ? static_cast<DWORD>(HRESULT_CODE(hresult))
            : ERROR_SUCCESS;
        return ContractMismatch(hresult, win32);
    }
    if (hresult != S_OK || !present) {
        return ContractMismatch(S_FALSE);
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

bool ExactClassName(const HWND window, const wchar_t* const expected, Status* const status) {
    std::array<wchar_t, 256> className{};
    SetLastError(ERROR_SUCCESS);
    const int length = GetClassNameW(window, className.data(), static_cast<int>(className.size()));
    if (length == 0) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_INVALID_WINDOW_HANDLE;
        }
        *status = Win32Mismatch(error);
        return false;
    }
    return std::wstring_view{className.data(), static_cast<std::size_t>(length)} == expected;
}

Status CaptureDirectChildZOrder(const HWND parent, std::vector<HWND>* const output) {
    output->clear();
    SetLastError(ERROR_SUCCESS);
    HWND child = GetTopWindow(parent);
    DWORD error = child == nullptr ? GetLastError() : ERROR_SUCCESS;
    if (error != ERROR_SUCCESS) {
        return Win32Mismatch(error);
    }
    while (child != nullptr) {
        if (std::ranges::find(*output, child) != output->end()) {
            return ContractMismatch(S_FALSE);
        }
        output->push_back(child);
        SetLastError(ERROR_SUCCESS);
        child = GetWindow(child, GW_HWNDNEXT);
        error = child == nullptr ? GetLastError() : ERROR_SUCCESS;
        if (error != ERROR_SUCCESS) {
            return Win32Mismatch(error);
        }
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status SelectPaneParent(const HWND topLevel, HWND* const paneParent) {
    Status status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    if (!ExactClassName(topLevel, L"CabinetWClass", &status)) {
        return status.ok() ? ContractMismatch(S_FALSE) : status;
    }

    std::vector<HWND> before;
    status = CaptureDirectChildZOrder(topLevel, &before);
    if (!status.ok()) {
        return status;
    }

    HWND shellTab = nullptr;
    for (const HWND child : before) {
        if (!ExactClassName(child, L"ShellTabWindowClass", &status)) {
            if (!status.ok()) {
                return status;
            }
            continue;
        }
        if (IsWindowVisible(child) != FALSE) {
            shellTab = child;
            break;
        }
    }
    if (shellTab == nullptr) {
        return ContractMismatch(S_FALSE);
    }

    struct EnumState final {
        std::vector<HWND> matches;
        Status status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    } state;
    SetLastError(ERROR_SUCCESS);
    const BOOL enumerated = EnumChildWindows(
        shellTab,
        [](const HWND child, const LPARAM parameter) -> BOOL {
            auto* const state = reinterpret_cast<EnumState*>(parameter);
            Status classStatus{ErrorCode::OK, S_OK, ERROR_SUCCESS};
            if (ExactClassName(child, L"DUIViewWndClassName", &classStatus)) {
                state->matches.push_back(child);
            } else if (!classStatus.ok()) {
                state->status = classStatus;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));
    if (!enumerated) {
        if (!state.status.ok()) {
            return state.status;
        }
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_INVALID_WINDOW_HANDLE;
        }
        return Win32Mismatch(error);
    }
    if (state.matches.size() != 1 || IsWindowVisible(state.matches[0]) == FALSE) {
        return ContractMismatch(S_FALSE);
    }

    std::vector<HWND> after;
    status = CaptureDirectChildZOrder(topLevel, &after);
    if (!status.ok()) {
        return status;
    }
    if (before != after) {
        return ContractMismatch(S_FALSE);
    }

    *paneParent = state.matches[0];
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

struct CachedElement final {
    ComPtr<IUIAutomationElement> element;
    std::wstring framework_id;
    CONTROLTYPEID control_type = 0;
    std::wstring automation_id;
    std::wstring class_name;
    UIA_HWND native_window_handle = nullptr;
    RECT bounds{};
};

Status ReadCachedText(
    IUIAutomationElement* const element,
    HRESULT (STDMETHODCALLTYPE IUIAutomationElement::*getter)(BSTR*),
    std::wstring* const output) {
    BSTR value = nullptr;
    const HRESULT hresult = (element->*getter)(&value);
    const Status status = RequireExactSuccess(hresult);
    if (!status.ok()) {
        SysFreeString(value);
        return status;
    }
    output->assign(value == nullptr ? L"" : value, value == nullptr ? 0 : SysStringLen(value));
    SysFreeString(value);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status ReadCachedElement(IUIAutomationElement* const element, CachedElement* const output) {
    Status status = ReadCachedText(
        element, &IUIAutomationElement::get_CachedFrameworkId, &output->framework_id);
    if (!status.ok()) {
        return status;
    }
    HRESULT hresult = element->get_CachedControlType(&output->control_type);
    status = RequireExactSuccess(hresult);
    if (!status.ok()) {
        return status;
    }
    status = ReadCachedText(
        element, &IUIAutomationElement::get_CachedAutomationId, &output->automation_id);
    if (!status.ok()) {
        return status;
    }
    status = ReadCachedText(
        element, &IUIAutomationElement::get_CachedClassName, &output->class_name);
    if (!status.ok()) {
        return status;
    }
    hresult = element->get_CachedNativeWindowHandle(&output->native_window_handle);
    status = RequireExactSuccess(hresult);
    if (!status.ok()) {
        return status;
    }
    hresult = element->get_CachedBoundingRectangle(&output->bounds);
    return RequireExactSuccess(hresult);
}

Status ReadElementArray(
    IUIAutomationElementArray* const array,
    std::vector<CachedElement>* const output) {
    int length = 0;
    Status status = RequireExactSuccess(array->get_Length(&length));
    if (!status.ok()) {
        return status;
    }
    if (length < 0) {
        return ContractMismatch(S_FALSE);
    }
    output->reserve(static_cast<std::size_t>(length));
    for (int index = 0; index < length; ++index) {
        CachedElement cached;
        HRESULT hresult = array->GetElement(index, &cached.element);
        status = RequireExactSuccess(hresult, cached.element != nullptr);
        if (!status.ok()) {
            return status;
        }
        status = ReadCachedElement(cached.element.Get(), &cached);
        if (!status.ok()) {
            return status;
        }
        output->push_back(std::move(cached));
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status CaptureUiaMetrics(
    IUIAutomation* const automation,
    const HWND paneParent,
    ExplorerLayoutCaptureEvidence* const evidence) {
    ComPtr<IUIAutomationCacheRequest> request;
    HRESULT hresult = automation->CreateCacheRequest(&request);
    Status status = RequireExactSuccess(hresult, request != nullptr);
    if (!status.ok()) {
        return status;
    }
    status = RequireExactSuccess(request->put_AutomationElementMode(AutomationElementMode_Full));
    if (!status.ok()) {
        return status;
    }
    status = RequireExactSuccess(request->put_TreeScope(TreeScope_Element));
    if (!status.ok()) {
        return status;
    }
    const PROPERTYID properties[] = {
        UIA_FrameworkIdPropertyId,
        UIA_ControlTypePropertyId,
        UIA_AutomationIdPropertyId,
        UIA_ClassNamePropertyId,
        UIA_NativeWindowHandlePropertyId,
        UIA_BoundingRectanglePropertyId,
    };
    for (const PROPERTYID property : properties) {
        status = RequireExactSuccess(request->AddProperty(property));
        if (!status.ok()) {
            return status;
        }
    }

    ComPtr<IUIAutomationCondition> trueCondition;
    hresult = automation->CreateTrueCondition(&trueCondition);
    status = RequireExactSuccess(hresult, trueCondition != nullptr);
    if (!status.ok()) {
        return status;
    }

    ComPtr<IUIAutomationElement> root;
    hresult = automation->ElementFromHandleBuildCache(paneParent, request.Get(), &root);
    status = RequireExactSuccess(hresult, root != nullptr);
    if (!status.ok()) {
        return status;
    }
    ComPtr<IUIAutomationElementArray> statusArray;
    hresult = root->FindAllBuildCache(
        TreeScope_Subtree, trueCondition.Get(), request.Get(), &statusArray);
    status = RequireExactSuccess(hresult, statusArray != nullptr);
    if (!status.ok()) {
        return status;
    }
    std::vector<CachedElement> statusElements;
    status = ReadElementArray(statusArray.Get(), &statusElements);
    if (!status.ok()) {
        return status;
    }

    std::vector<CachedElement*> statusMatches;
    for (CachedElement& element : statusElements) {
        evidence->elements.push_back({
            ExplorerLayoutUiaScope::PaneSubtree,
            element.framework_id,
            element.control_type,
            element.automation_id,
            element.class_name,
            element.native_window_handle,
            element.bounds,
        });
        if (element.framework_id == L"DirectUI" &&
            element.control_type == UIA_StatusBarControlTypeId &&
            element.automation_id == L"StatusBarModuleInner" &&
            element.class_name == L"StatusBarModuleInner" &&
            element.native_window_handle == nullptr) {
            statusMatches.push_back(&element);
        }
    }
    if (statusMatches.size() != 1) {
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    ComPtr<IUIAutomationElementArray> groupArray;
    hresult = statusMatches[0]->element->FindAllBuildCache(
        TreeScope_Children, trueCondition.Get(), request.Get(), &groupArray);
    status = RequireExactSuccess(hresult, groupArray != nullptr);
    if (!status.ok()) {
        return status;
    }
    std::vector<CachedElement> groups;
    status = ReadElementArray(groupArray.Get(), &groups);
    if (!status.ok()) {
        return status;
    }

    for (CachedElement& element : groups) {
        evidence->elements.push_back({
            ExplorerLayoutUiaScope::StatusBarChildren,
            element.framework_id,
            element.control_type,
            element.automation_id,
            element.class_name,
            element.native_window_handle,
            element.bounds,
        });
    }
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status CaptureExplorerLayoutCurrentThread(
    const HWND topLevel,
    ExplorerLayoutCaptureEvidence* const evidence) {
    APTTYPE apartmentType = APTTYPE_CURRENT;
    APTTYPEQUALIFIER qualifier = APTTYPEQUALIFIER_NONE;
    Status status = RequireExactSuccess(CoGetApartmentType(&apartmentType, &qualifier));
    if (!status.ok()) {
        return status;
    }
    if (apartmentType != APTTYPE_MTA) {
        return ContractMismatch(RPC_E_WRONG_THREAD);
    }
    evidence->executed_in_mta = true;

    ComPtr<IUIAutomation> automation;
    const HRESULT created = CoCreateInstance(
        CLSID_CUIAutomation,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    status = RequireExactSuccess(created, automation != nullptr);
    if (status.ok()) {
        status = SelectPaneParent(topLevel, &evidence->pane_parent);
    }
    if (status.ok() && !GetWindowRect(evidence->pane_parent, &evidence->parent_screen)) {
        DWORD error = GetLastError();
        if (error == ERROR_SUCCESS) {
            error = ERROR_INVALID_WINDOW_HANDLE;
        }
        status = Win32Mismatch(error);
    }
    if (status.ok()) {
        evidence->dpi = GetDpiForWindow(evidence->pane_parent);
        if (evidence->dpi == 0) {
            DWORD error = GetLastError();
            if (error == ERROR_SUCCESS) {
                error = ERROR_INVALID_WINDOW_HANDLE;
            }
            status = Win32Mismatch(error);
        }
    }
    if (status.ok()) {
        status = CaptureUiaMetrics(automation.Get(), evidence->pane_parent, evidence);
    }
    return status;
}

struct ProductionWorkerState final {
    HANDLE completion = nullptr;
    ExplorerLayoutWorkerModuleRetention retention{};
    ExplorerLayoutWorkerModuleOperations module_operations{};
    ExplorerLayoutMtaTask task;

    ~ProductionWorkerState() {
        if (completion != nullptr) {
            CloseHandle(completion);
        }
    }
};

DWORD WINAPI ProductionMtaThreadProc(void* const parameter) {
    std::unique_ptr<std::shared_ptr<ProductionWorkerState>> holder{
        static_cast<std::shared_ptr<ProductionWorkerState>*>(parameter)};
    std::shared_ptr<ProductionWorkerState> state = *holder;
    holder.reset();

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const Status apartmentStatus = RequireExactSuccess(initialized);
    try {
        state->task(apartmentStatus);
    } catch (...) {
    }
    if (apartmentStatus.ok()) {
        CoUninitialize();
    }
    static_cast<void>(SetEvent(state->completion));

    const ExplorerLayoutWorkerModuleRetention retention = state->retention;
    const ExplorerLayoutWorkerModuleOperations moduleOperations =
        state->module_operations;
    state.reset();
    moduleOperations.release_and_exit(moduleOperations.context, retention);
    ExitThread(ERROR_INVALID_FUNCTION);
}

Status RunMtaWorkerWithModuleOperations(
    ExplorerLayoutMtaTask task,
    const DWORD timeoutMs,
    const ExplorerLayoutWorkerModuleOperations& moduleOperations) {
    if (moduleOperations.retain == nullptr || moduleOperations.release == nullptr ||
        moduleOperations.release_and_exit == nullptr) {
        return ContractMismatch(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::shared_ptr<ProductionWorkerState> state;
    try {
        state = std::make_shared<ProductionWorkerState>();
        state->task = std::move(task);
        state->module_operations = moduleOperations;
    } catch (const std::bad_alloc&) {
        return ContractMismatch(E_OUTOFMEMORY, ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        return ContractMismatch(E_FAIL);
    }

    state->completion = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state->completion == nullptr) {
        return Win32Mismatch(GetLastError());
    }
    const Status retentionStatus = moduleOperations.retain(
        moduleOperations.context, &state->retention);
    if (!retentionStatus.ok()) {
        return retentionStatus;
    }

    auto* const parameter = new (std::nothrow) std::shared_ptr<ProductionWorkerState>{state};
    if (parameter == nullptr) {
        moduleOperations.release(moduleOperations.context, state->retention);
        return ContractMismatch(E_OUTOFMEMORY, ERROR_NOT_ENOUGH_MEMORY);
    }
    const HANDLE thread = CreateThread(
        nullptr, 0, &ProductionMtaThreadProc, parameter, 0, nullptr);
    if (thread == nullptr) {
        const DWORD error = GetLastError();
        delete parameter;
        moduleOperations.release(moduleOperations.context, state->retention);
        return Win32Mismatch(error);
    }
    CloseHandle(thread);

    const DWORD waitResult = WaitForSingleObject(state->completion, timeoutMs);
    if (waitResult == WAIT_OBJECT_0) {
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }
    if (waitResult == WAIT_TIMEOUT) {
        return ContractMismatch(HRESULT_FROM_WIN32(ERROR_TIMEOUT), ERROR_TIMEOUT);
    }
    const DWORD error = GetLastError();
    return Win32Mismatch(error == ERROR_SUCCESS ? ERROR_INVALID_FUNCTION : error);
}

Status RetainProductionWorkerModule(
    void*,
    ExplorerLayoutWorkerModuleRetention* const retention) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&ProductionMtaThreadProc),
            &module)) {
        return Win32Mismatch(GetLastError());
    }
    const HMODULE executable = GetModuleHandleW(nullptr);
    if (executable == nullptr) {
        const DWORD error = GetLastError();
        FreeLibrary(module);
        return Win32Mismatch(error == ERROR_SUCCESS ? ERROR_MOD_NOT_FOUND : error);
    }
    *retention = {
        module,
        ClassifyExplorerLayoutWorkerModule(module, executable),
    };
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

void ReleaseProductionWorkerModule(
    void*,
    const ExplorerLayoutWorkerModuleRetention retention) {
    static_cast<void>(FreeLibrary(retention.module));
}

void ReleaseAndExitProductionWorkerModule(
    void*,
    const ExplorerLayoutWorkerModuleRetention retention) {
    if (GetExplorerLayoutWorkerReleasePath(retention.kind) ==
        ExplorerLayoutWorkerReleasePath::FreeLibraryAndExitThread) {
        FreeLibraryAndExitThread(retention.module, 0);
    }
    static_cast<void>(FreeLibrary(retention.module));
    ExitThread(0);
}

ExplorerLayoutWorkerModuleOperations GetProductionWorkerModuleOperations() {
    return {
        nullptr,
        &RetainProductionWorkerModule,
        &ReleaseProductionWorkerModule,
        &ReleaseAndExitProductionWorkerModule,
    };
}

Status RunProductionMtaWorker(ExplorerLayoutMtaTask task, const DWORD timeoutMs) {
    return RunMtaWorkerWithModuleOperations(
        std::move(task), timeoutMs, GetProductionWorkerModuleOperations());
}

}  // namespace

ExplorerLayoutWorkerModuleKind ClassifyExplorerLayoutWorkerModule(
    const HMODULE retainedModule,
    const HMODULE executableModule) noexcept {
    return retainedModule == executableModule
        ? ExplorerLayoutWorkerModuleKind::Executable
        : ExplorerLayoutWorkerModuleKind::DynamicLibrary;
}

ExplorerLayoutWorkerReleasePath GetExplorerLayoutWorkerReleasePath(
    const ExplorerLayoutWorkerModuleKind kind) noexcept {
    return kind == ExplorerLayoutWorkerModuleKind::DynamicLibrary
        ? ExplorerLayoutWorkerReleasePath::FreeLibraryAndExitThread
        : ExplorerLayoutWorkerReleasePath::FreeLibraryThenExitThread;
}

Status RunExplorerLayoutMtaWorkerWithOperations(
    ExplorerLayoutMtaTask task,
    const DWORD timeoutMs,
    const ExplorerLayoutWorkerModuleOperations& moduleOperations) {
    return RunMtaWorkerWithModuleOperations(
        std::move(task), timeoutMs, moduleOperations);
}

Status ComputeStatusPaneRect(
    const ExplorerLayoutMetrics& metrics,
    RECT* const output) noexcept {
    if (output == nullptr || metrics.dpi == 0) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }
    const auto valid = [](const RECT& rectangle) {
        return rectangle.left <= rectangle.right && rectangle.top <= rectangle.bottom;
    };
    if (!valid(metrics.parent_screen) || !valid(metrics.status_screen) ||
        !valid(metrics.left_group_screen) || !valid(metrics.right_group_screen)) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    }

    const std::int64_t margin =
        (static_cast<std::int64_t>(8) * metrics.dpi + 48) / 96;
    const std::int64_t minimumWidth = static_cast<std::int64_t>(metrics.dpi);
    const std::int64_t screenLeft = std::max<std::int64_t>(
        static_cast<std::int64_t>(metrics.left_group_screen.right) + margin,
        std::max<std::int64_t>(metrics.parent_screen.left, metrics.status_screen.left));
    const std::int64_t screenRight = std::min<std::int64_t>(
        static_cast<std::int64_t>(metrics.right_group_screen.left) - margin,
        std::min<std::int64_t>(metrics.parent_screen.right, metrics.status_screen.right));
    const std::int64_t screenTop =
        std::max<std::int64_t>(metrics.parent_screen.top, metrics.status_screen.top);
    const std::int64_t screenBottom =
        std::min<std::int64_t>(metrics.parent_screen.bottom, metrics.status_screen.bottom);
    if (screenRight - screenLeft < minimumWidth || screenBottom <= screenTop) {
        *output = {};
        return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    const std::int64_t clientLeft = screenLeft - metrics.parent_screen.left;
    const std::int64_t clientTop = screenTop - metrics.parent_screen.top;
    const std::int64_t clientRight = screenRight - metrics.parent_screen.left;
    const std::int64_t clientBottom = screenBottom - metrics.parent_screen.top;
    constexpr std::int64_t minimumLong = (std::numeric_limits<LONG>::min)();
    constexpr std::int64_t maximumLong = (std::numeric_limits<LONG>::max)();
    if (clientLeft < minimumLong || clientTop < minimumLong || clientRight > maximumLong ||
        clientBottom > maximumLong) {
        return {ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_ARITHMETIC_OVERFLOW};
    }

    *output = {
        static_cast<LONG>(clientLeft),
        static_cast<LONG>(clientTop),
        static_cast<LONG>(clientRight),
        static_cast<LONG>(clientBottom),
    };
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status CaptureExplorerLayout(
    const HWND topLevel,
    ExplorerLayoutMetrics* const metrics,
    HWND* const paneParent) {
    if (topLevel == nullptr || metrics == nullptr || paneParent == nullptr) {
        return ContractMismatch(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }

    const ExplorerLayoutCaptureOperations operations =
        GetProductionExplorerLayoutCaptureOperations();
    return CaptureExplorerLayoutWithOperations(topLevel, metrics, paneParent, operations);
}

ExplorerLayoutCaptureOperations GetProductionExplorerLayoutCaptureOperations() {
    return {
        &CaptureExplorerLayoutCurrentThread,
        &RunProductionMtaWorker,
    };
}

Status ValidateExplorerLayoutCapture(
    const ExplorerLayoutCaptureEvidence& evidence,
    ExplorerLayoutMetrics* const metrics,
    HWND* const paneParent) {
    if (metrics == nullptr || paneParent == nullptr) {
        return ContractMismatch(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    if (!evidence.capture_status.ok()) {
        return evidence.capture_status;
    }
    if (!evidence.executed_in_mta || evidence.pane_parent == nullptr || evidence.dpi == 0) {
        return ContractMismatch(S_FALSE);
    }

    std::vector<const ExplorerLayoutUiaElementEvidence*> statusMatches;
    std::vector<const ExplorerLayoutUiaElementEvidence*> leftMatches;
    std::vector<const ExplorerLayoutUiaElementEvidence*> rightMatches;
    for (const ExplorerLayoutUiaElementEvidence& element : evidence.elements) {
        if (element.scope == ExplorerLayoutUiaScope::PaneSubtree &&
            element.framework_id == L"DirectUI" &&
            element.control_type == UIA_StatusBarControlTypeId &&
            element.automation_id == L"StatusBarModuleInner" &&
            element.class_name == L"StatusBarModuleInner" &&
            element.native_window_handle == nullptr) {
            statusMatches.push_back(&element);
        }
        if (element.scope == ExplorerLayoutUiaScope::StatusBarChildren &&
            element.control_type == UIA_GroupControlTypeId &&
            element.automation_id == L"System.StatusBarViewItemCount" &&
            element.native_window_handle == nullptr) {
            leftMatches.push_back(&element);
        }
        if (element.scope == ExplorerLayoutUiaScope::StatusBarChildren &&
            element.control_type == UIA_GroupControlTypeId &&
            element.automation_id == L"ViewButtonsGroup" &&
            element.native_window_handle == nullptr) {
            rightMatches.push_back(&element);
        }
    }
    if (statusMatches.size() != 1 || leftMatches.size() != 1 || rightMatches.size() != 1) {
        return ContractMismatch(S_FALSE);
    }

    ExplorerLayoutMetrics captured{
        evidence.parent_screen,
        statusMatches[0]->bounds,
        leftMatches[0]->bounds,
        rightMatches[0]->bounds,
        evidence.dpi,
    };
    *metrics = captured;
    *paneParent = evidence.pane_parent;
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status CaptureExplorerLayoutWithOperations(
    const HWND topLevel,
    ExplorerLayoutMetrics* const metrics,
    HWND* const paneParent,
    const ExplorerLayoutCaptureOperations& operations) {
    if (topLevel == nullptr || metrics == nullptr || paneParent == nullptr ||
        !operations.capture_current_thread || !operations.run_mta_worker) {
        return ContractMismatch(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }

    std::shared_ptr<ExplorerLayoutCaptureEvidence> evidence;
    try {
        evidence = std::make_shared<ExplorerLayoutCaptureEvidence>(
            ExplorerLayoutCaptureEvidence{
                {ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH, E_FAIL, ERROR_SUCCESS},
                false,
                nullptr,
                {},
                0,
                {},
            });
        const auto capture = operations.capture_current_thread;
        const ExplorerLayoutMtaTask task = [evidence, capture, topLevel](
                                               const Status apartmentStatus) {
            if (!apartmentStatus.ok()) {
                evidence->capture_status = apartmentStatus;
                return;
            }
            try {
                evidence->capture_status = capture(topLevel, evidence.get());
            } catch (const std::bad_alloc&) {
                evidence->capture_status =
                    ContractMismatch(E_OUTOFMEMORY, ERROR_NOT_ENOUGH_MEMORY);
            } catch (...) {
                evidence->capture_status = ContractMismatch(E_FAIL);
            }
        };
        const Status waitStatus = operations.run_mta_worker(
            task, kExplorerLayoutCaptureTimeoutMs);
        if (!waitStatus.ok()) {
            return waitStatus;
        }
    } catch (const std::bad_alloc&) {
        return ContractMismatch(E_OUTOFMEMORY, ERROR_NOT_ENOUGH_MEMORY);
    } catch (...) {
        return ContractMismatch(E_FAIL);
    }
    return ValidateExplorerLayoutCapture(*evidence, metrics, paneParent);
}

}  // namespace winexinfo
