#include "probe/probe_runner.h"

#include "common/utf8.h"
#include "probe/report_writer.h"
#include "probe/observer_runtime.h"
#include "probe/shell_probe.h"
#include "probe/target_validator.h"
#include "probe/uia_probe.h"
#include "probe/win32_probe.h"

#include <Windows.h>
#include <objbase.h>
#include <UIAutomation.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace winexinfo {
namespace {

std::string FormatHwnd(const HWND hwnd) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(sizeof(void*) * 2)
           << std::setfill('0') << reinterpret_cast<std::uintptr_t>(hwnd);
    return stream.str();
}

std::string FormatRect(const RECT& bounds) {
    return std::to_string(bounds.left) + "," + std::to_string(bounds.top) + "," +
        std::to_string(bounds.right) + "," + std::to_string(bounds.bottom);
}

void AppendUiaElement(
    ReportSection* const section,
    const std::string_view prefix,
    const UiaElementEvidence& element,
    Status* const conversionStatus) {
    std::string framework;
    Status status = Utf8FromUtf16(element.framework_id, &framework);
    if (!status.ok() && conversionStatus->ok()) {
        *conversionStatus = status;
    }
    std::string automationId;
    status = Utf8FromUtf16(element.automation_id, &automationId);
    if (!status.ok() && conversionStatus->ok()) {
        *conversionStatus = status;
    }
    std::string className;
    status = Utf8FromUtf16(element.class_name, &className);
    if (!status.ok() && conversionStatus->ok()) {
        *conversionStatus = status;
    }

    const std::string keyPrefix{prefix};
    section->fields.push_back({keyPrefix + ".framework_id", std::move(framework)});
    section->fields.push_back(
        {keyPrefix + ".control_type", std::to_string(element.control_type)});
    section->fields.push_back({keyPrefix + ".automation_id", std::move(automationId)});
    section->fields.push_back({keyPrefix + ".class_name", std::move(className)});
    section->fields.push_back(
        {keyPrefix + ".process_id", std::to_string(element.process_id)});
    section->fields.push_back(
        {keyPrefix + ".native_hwnd", FormatHwnd(reinterpret_cast<HWND>(element.native_window_handle))});
    section->fields.push_back({keyPrefix + ".screen_rect", FormatRect(element.bounds)});
    section->fields.push_back({keyPrefix + ".offscreen", element.offscreen ? "true" : "false"});
}

}  // namespace

bool IsProbeTransportFailure(const Status& status) {
    return status.win32 != ERROR_SUCCESS || FAILED(status.hresult);
}

ProbeRunResult RunSnapshotProbe() {
    std::vector<ExplorerWindowRecord> windows;
    const Status enumerationStatus = EnumerateExplorerWindows(&windows);
    ProbeReport report{ProbeMode::Snapshot, false, {}, ErrorCode::OK};
    bool transportFailure = false;
    ErrorCode firstError = ErrorCode::OK;

    ReportSection summary{};
    summary.fields.push_back({"window_count", std::to_string(windows.size())});
    summary.fields.push_back({"enumeration_hresult", std::to_string(enumerationStatus.hresult)});
    summary.fields.push_back({"enumeration_win32", std::to_string(enumerationStatus.win32)});
    report.sections.push_back(std::move(summary));

    if (!enumerationStatus.ok()) {
        firstError = enumerationStatus.code;
        transportFailure = true;
    } else if (windows.empty()) {
        firstError = ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH;
    }

    std::size_t exactUiaWindows = 0;
    for (std::size_t index = 0; index < windows.size(); ++index) {
        const ExplorerWindowRecord& window = windows[index];
        const std::string prefix = "window." + std::to_string(index);
        ReportSection section{};
        const Status terminalStageInitialization =
            InitializeShellTerminalStageReportField(prefix, &section);
        if (!terminalStageInitialization.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = terminalStageInitialization.code;
            }
            transportFailure =
                transportFailure || IsProbeTransportFailure(terminalStageInitialization);
            section.fields.push_back({
                prefix + ".error_code",
                std::string{ToString(terminalStageInitialization.code)},
            });
            section.fields.push_back({
                prefix + ".error_hresult",
                std::to_string(terminalStageInitialization.hresult),
            });
            section.fields.push_back({
                prefix + ".error_win32",
                std::to_string(terminalStageInitialization.win32),
            });
            report.sections.push_back(std::move(section));
            continue;
        }
        section.fields.push_back({prefix + ".hwnd", FormatHwnd(window.hwnd)});
        section.fields.push_back({prefix + ".pid", std::to_string(window.process_id)});
        section.fields.push_back({prefix + ".thread_id", std::to_string(window.thread_id)});

        TargetValidationEvidence targetEvidence{};
        static_cast<void>(CaptureTargetValidationEvidence(window.process_id, &targetEvidence));
        const TargetValidationResult targetValidation = ValidateTargetEvidence(targetEvidence);
        for (const TargetValidationRow& row : targetValidation.rows) {
            section.fields.push_back(
                {prefix + ".preflight." + row.name, row.passed ? "pass" : "fail"});
        }
        section.fields.push_back(
            {prefix + ".preflight.error_code", std::string{ToString(targetValidation.status.code)}});
        section.fields.push_back(
            {prefix + ".preflight.hresult", std::to_string(targetValidation.status.hresult)});
        section.fields.push_back(
            {prefix + ".preflight.win32", std::to_string(targetValidation.status.win32)});
        if (!targetValidation.status.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = targetValidation.status.code;
            }
            if (targetValidation.status.win32 != ERROR_SUCCESS) {
                transportFailure = true;
            }
            section.fields.push_back(
                {prefix + ".error_code", std::string{ToString(targetValidation.status.code)}});
            report.sections.push_back(std::move(section));
            continue;
        }

        Win32ClassTree classTree{};
        const Status treeCapture = CaptureWin32ClassTree(window.hwnd, &classTree);
        if (!treeCapture.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = treeCapture.code;
            }
            transportFailure = transportFailure || IsProbeTransportFailure(treeCapture);
            section.fields.push_back(
                {prefix + ".error_code", std::string{ToString(treeCapture.code)}});
            section.fields.push_back(
                {prefix + ".error_hresult", std::to_string(treeCapture.hresult)});
            section.fields.push_back(
                {prefix + ".error_win32", std::to_string(treeCapture.win32)});
            report.sections.push_back(std::move(section));
            continue;
        }

        Status conversionStatus{ErrorCode::OK, S_OK, ERROR_SUCCESS};
        for (std::size_t nodeIndex = 0; nodeIndex < classTree.nodes.size(); ++nodeIndex) {
            const Win32ClassNode& node = classTree.nodes[nodeIndex];
            const std::string nodePrefix =
                prefix + ".class." + std::to_string(nodeIndex);
            std::string className;
            const Status status = Utf8FromUtf16(node.class_name, &className);
            if (!status.ok() && conversionStatus.ok()) {
                conversionStatus = status;
            }
            section.fields.push_back({nodePrefix + ".hwnd", FormatHwnd(node.hwnd)});
            section.fields.push_back({nodePrefix + ".parent", FormatHwnd(node.parent)});
            section.fields.push_back({nodePrefix + ".name", std::move(className)});
            section.fields.push_back({nodePrefix + ".visible", node.visible ? "true" : "false"});
            section.fields.push_back({nodePrefix + ".rect", FormatRect(node.bounds)});
        }
        if (!conversionStatus.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = conversionStatus.code;
            }
            transportFailure = transportFailure || IsProbeTransportFailure(conversionStatus);
            section.fields.push_back(
                {prefix + ".error_code", std::string{ToString(conversionStatus.code)}});
            section.fields.push_back(
                {prefix + ".error_win32", std::to_string(conversionStatus.win32)});
            report.sections.push_back(std::move(section));
            continue;
        }

        const Win32ContractResult win32Validation = ValidateWin32Contract(classTree);
        section.fields.push_back(
            {prefix + ".active_shell_tab", FormatHwnd(win32Validation.active_shell_tab)});
        section.fields.push_back(
            {prefix + ".active_view", FormatHwnd(win32Validation.active_view)});
        if (!win32Validation.status.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = win32Validation.status.code;
            }
            section.fields.push_back(
                {prefix + ".error_code", std::string{ToString(win32Validation.status.code)}});
            report.sections.push_back(std::move(section));
            continue;
        }

        ActiveShellViewSnapshot shellSnapshot{};
        const Status shellStatus = CaptureActiveShellView(
            window.hwnd, win32Validation.active_shell_tab, &shellSnapshot);
        const Status shellReportStatus =
            AppendActiveShellViewReportFields(prefix, shellSnapshot, &section);
        const Status effectiveShellStatus =
            shellStatus.ok() ? shellReportStatus : shellStatus;
        if (!effectiveShellStatus.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = effectiveShellStatus.code;
            }
            transportFailure =
                transportFailure || IsProbeTransportFailure(effectiveShellStatus);
            section.fields.push_back({
                prefix + ".error_code",
                std::string{ToString(effectiveShellStatus.code)},
            });
            section.fields.push_back({
                prefix + ".error_hresult",
                std::to_string(effectiveShellStatus.hresult),
            });
            section.fields.push_back({
                prefix + ".error_win32",
                std::to_string(effectiveShellStatus.win32),
            });
            report.sections.push_back(std::move(section));
            continue;
        }

        UiaContractEvidence uiaEvidence{};
        Status workerStatus{ErrorCode::OK, S_OK, ERROR_SUCCESS};
        std::thread uiaWorker([&]() {
            const HRESULT initialize = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(initialize)) {
                workerStatus = {
                    ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                    initialize,
                    HRESULT_FACILITY(initialize) == FACILITY_WIN32
                        ? static_cast<DWORD>(HRESULT_CODE(initialize))
                        : ERROR_SUCCESS,
                };
                return;
            }
            workerStatus = CaptureUiaContractEvidence(
                window.hwnd, win32Validation.active_view, &uiaEvidence);
            CoUninitialize();
        });
        uiaWorker.join();

        if (!workerStatus.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = workerStatus.code;
            }
            transportFailure = transportFailure || IsProbeTransportFailure(workerStatus);
            section.fields.push_back(
                {prefix + ".error_code", std::string{ToString(workerStatus.code)}});
            section.fields.push_back(
                {prefix + ".error_hresult", std::to_string(workerStatus.hresult)});
            section.fields.push_back(
                {prefix + ".error_win32", std::to_string(workerStatus.win32)});
            report.sections.push_back(std::move(section));
            continue;
        }

        UiaContractSnapshot uiaSnapshot{};
        const Status uiaValidation = ValidateUiaContract(uiaEvidence, &uiaSnapshot);
        AppendUiaCardinalityReportFields(prefix, uiaSnapshot.cardinalities, &section);
        if (!uiaValidation.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = uiaValidation.code;
            }
            section.fields.push_back(
                {prefix + ".error_code", std::string{ToString(uiaValidation.code)}});
            section.fields.push_back(
                {prefix + ".error_hresult", std::to_string(uiaValidation.hresult)});
            section.fields.push_back(
                {prefix + ".error_win32", std::to_string(uiaValidation.win32)});
            report.sections.push_back(std::move(section));
            continue;
        }

        AppendUiaElement(
            &section, prefix + ".status_bar", uiaSnapshot.status_bar, &conversionStatus);
        AppendUiaElement(
            &section, prefix + ".left_group", uiaSnapshot.left_group, &conversionStatus);
        AppendUiaElement(
            &section, prefix + ".right_group", uiaSnapshot.right_group, &conversionStatus);
        AppendUiaElement(
            &section, prefix + ".tab_view", uiaSnapshot.tab_view, &conversionStatus);
        AppendUiaElement(
            &section, prefix + ".tab_list", uiaSnapshot.tab_list, &conversionStatus);

        const UiaElementEvidence* convertedElements[] = {
            &uiaSnapshot.status_bar,
            &uiaSnapshot.left_group,
            &uiaSnapshot.right_group,
        };
        for (std::size_t rectangleIndex = 0; rectangleIndex < std::size(convertedElements);
             ++rectangleIndex) {
            POINT topLeft{
                convertedElements[rectangleIndex]->bounds.left,
                convertedElements[rectangleIndex]->bounds.top,
            };
            POINT bottomRight{
                convertedElements[rectangleIndex]->bounds.right,
                convertedElements[rectangleIndex]->bounds.bottom,
            };
            if (!ScreenToClient(win32Validation.active_view, &topLeft) ||
                !ScreenToClient(win32Validation.active_view, &bottomRight)) {
                const DWORD error = GetLastError();
                conversionStatus = {
                    ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                    HRESULT_FROM_WIN32(error),
                    error,
                };
                break;
            }
            const std::string names[] = {"status_bar", "left_group", "right_group"};
            section.fields.push_back({
                prefix + "." + names[rectangleIndex] + ".client_rect",
                FormatRect(RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y}),
            });
        }

        if (!conversionStatus.ok()) {
            if (firstError == ErrorCode::OK) {
                firstError = conversionStatus.code;
            }
            transportFailure = transportFailure || IsProbeTransportFailure(conversionStatus);
            section.fields.push_back(
                {prefix + ".error_code", std::string{ToString(conversionStatus.code)}});
            section.fields.push_back(
                {prefix + ".error_hresult", std::to_string(conversionStatus.hresult)});
            section.fields.push_back(
                {prefix + ".error_win32", std::to_string(conversionStatus.win32)});
            report.sections.push_back(std::move(section));
            continue;
        }

        ++exactUiaWindows;
        section.fields.push_back({prefix + ".error_code", "OK"});
        report.sections.push_back(std::move(section));
    }

    if (firstError == ErrorCode::OK && exactUiaWindows == 0) {
        firstError = ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH;
    }
    report.passed = firstError == ErrorCode::OK && exactUiaWindows > 0;
    report.error_code = firstError;
    return {
        std::move(report),
        transportFailure ? HostExitCode::Win32ComFailure
                         : (firstError == ErrorCode::OK ? HostExitCode::Pass
                                                       : HostExitCode::ContractFailure),
    };
}

ProbeRunResult CreateObserveInfrastructureFailure(
    const std::uint32_t durationMs,
    const Status& failure,
    const std::string_view runtimeStage) {
    EventObservationSnapshot snapshot{
        durationMs,
        0,
        0,
        0,
        {0, 0, 0, 0, 0},
        failure,
        {ErrorCode::OK, S_OK, ERROR_SUCCESS},
        {},
        std::string{runtimeStage},
    };
    ProbeReport report{
        ProbeMode::Observe,
        false,
        {},
        ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
    };
    const Status appended = AppendEventObservationReportFields(
        snapshot, &report);
    if (!appended.ok()) {
        report = {
            ProbeMode::Observe,
            false,
            {},
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        };
        return {
            std::move(report),
            IsProbeTransportFailure(appended)
                ? HostExitCode::Win32ComFailure
                : HostExitCode::ContractFailure,
        };
    }
    return {
        std::move(report),
        IsProbeTransportFailure(failure)
            ? HostExitCode::Win32ComFailure
            : HostExitCode::ContractFailure,
    };
}

ProbeRunResult RunObserveProbe(const std::uint32_t durationMs) {
    ObserverRuntimeOutcome outcome{};
    const Status runtime = RunObserverRuntime(durationMs, &outcome);
    HostExitCode exitCode = HostExitCode::ContractFailure;
    const Status exitStatus = outcome.failures.ResolveHostExitCode(
        outcome.completion, &exitCode);
    if (!exitStatus.ok()) {
        exitCode = HostExitCode::ContractFailure;
    }

    ErrorCode reportError = ErrorCode::OK;
    if (!outcome.snapshot.runtime_status.ok()) {
        reportError = outcome.snapshot.runtime_status.code;
    } else if (!outcome.snapshot.cleanup_status.ok()) {
        reportError = outcome.snapshot.cleanup_status.code;
    } else if (!runtime.ok() || !outcome.completion.gate_passed) {
        reportError = ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH;
    }
    ProbeReport report{
        ProbeMode::Observe,
        outcome.completion.gate_passed,
        {},
        reportError,
    };
    const Status appended = AppendEventObservationReportFields(
        outcome.snapshot, &report);
    if (!appended.ok()) {
        return CreateObserveInfrastructureFailure(
            durationMs, appended, "report.append");
    }
    return {std::move(report), exitCode};
}

}  // namespace winexinfo
