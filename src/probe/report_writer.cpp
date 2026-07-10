#include "probe/report_writer.h"

#include "common/utf8.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace winexinfo {
namespace {

void AppendLine(
    std::string* const output,
    const std::string_view key,
    const std::string_view value) {
    output->append(key);
    output->push_back('=');

    for (const char character : value) {
        switch (character) {
            case '\r':
                output->append("%0D");
                break;
            case '\n':
                output->append("%0A");
                break;
            case '=':
                output->append("%3D");
                break;
            case '%':
                output->append("%25");
                break;
            default:
                output->push_back(character);
                break;
        }
    }

    output->push_back('\n');
}

}  // namespace

std::string WriteProbeReport(const ProbeReport& report) {
    std::string mode;
    switch (report.mode) {
        case ProbeMode::Snapshot:
            mode = "snapshot";
            break;
        case ProbeMode::Observe:
            mode = "observe";
            break;
        default:
            std::terminate();
    }

    std::string output;
    AppendLine(&output, "probe_version", "1");
    AppendLine(&output, "mode", mode);
    AppendLine(&output, "result", report.passed ? "pass" : "fail");

    for (const ReportSection& section : report.sections) {
        std::vector<ReportField> fields = section.fields;
        std::ranges::stable_sort(fields, {}, &ReportField::key);
        for (const ReportField& field : fields) {
            AppendLine(&output, field.key, field.value);
        }
    }

    AppendLine(&output, "error_code", ToString(report.error_code));
    return output;
}

void AppendUiaCardinalityReportFields(
    const std::string_view prefix,
    const UiaSelectorCardinalities& cardinalities,
    ReportSection* const output) {
    if (output == nullptr) {
        std::terminate();
    }

    const std::string keyPrefix{prefix};
    output->fields.push_back({
        keyPrefix + ".cardinality.status_bar",
        std::to_string(cardinalities.status_bar),
    });
    output->fields.push_back({
        keyPrefix + ".cardinality.left_group",
        std::to_string(cardinalities.left_group),
    });
    output->fields.push_back({
        keyPrefix + ".cardinality.right_group",
        std::to_string(cardinalities.right_group),
    });
    output->fields.push_back({
        keyPrefix + ".cardinality.tab_view",
        std::to_string(cardinalities.tab_view),
    });
    output->fields.push_back({
        keyPrefix + ".cardinality.tab_list",
        std::to_string(cardinalities.tab_list),
    });
}

Status InitializeShellTerminalStageReportField(
    const std::string_view prefix,
    ReportSection* const output) {
    if (output == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    const std::string key = std::string{prefix} + ".shell_terminal_stage";
    for (const ReportField& field : output->fields) {
        if (field.key == key) {
            return {
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER,
            };
        }
    }
    output->fields.push_back({key, "not_started"});
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status AppendActiveShellViewReportFields(
    const std::string_view prefix,
    const ActiveShellViewSnapshot& snapshot,
    ReportSection* const output) {
    if (output == nullptr) {
        return {
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            E_INVALIDARG,
            ERROR_INVALID_PARAMETER,
        };
    }

    std::string terminalStage;
    switch (snapshot.terminal_stage) {
        case ShellProbeTerminalStage::NotStarted:
            terminalStage = "not_started";
            break;
        case ShellProbeTerminalStage::CoCreateShellWindows:
            terminalStage = "co_create_shell_windows";
            break;
        case ShellProbeTerminalStage::IShellWindowsGetCount:
            terminalStage = "ishellwindows_get_count";
            break;
        case ShellProbeTerminalStage::IShellWindowsItem:
            terminalStage = "ishellwindows_item";
            break;
        case ShellProbeTerminalStage::IDispatchQueryIWebBrowser2:
            terminalStage = "idispatch_query_iwebbrowser2";
            break;
        case ShellProbeTerminalStage::IWebBrowser2GetHwnd:
            terminalStage = "iwebbrowser2_get_hwnd";
            break;
        case ShellProbeTerminalStage::IWebBrowser2QueryIServiceProvider:
            terminalStage = "iwebbrowser2_query_iserviceprovider";
            break;
        case ShellProbeTerminalStage::IServiceProviderQueryTopLevelBrowser:
            terminalStage = "iserviceprovider_query_top_level_browser";
            break;
        case ShellProbeTerminalStage::IShellBrowserGetWindow:
            terminalStage = "ishellbrowser_get_window";
            break;
        case ShellProbeTerminalStage::IShellBrowserQueryActiveShellView:
            terminalStage = "ishellbrowser_query_active_shell_view";
            break;
        case ShellProbeTerminalStage::IShellViewGetWindow:
            terminalStage = "ishellview_get_window";
            break;
        case ShellProbeTerminalStage::ValidateActiveView:
            terminalStage = "validate_active_view";
            break;
        case ShellProbeTerminalStage::IShellViewQueryIFolderView:
            terminalStage = "ishellview_query_ifolderview";
            break;
        case ShellProbeTerminalStage::IFolderViewGetFolder:
            terminalStage = "ifolderview_get_folder";
            break;
        case ShellProbeTerminalStage::ShGetIdListFromObject:
            terminalStage = "sh_get_id_list_from_object";
            break;
        case ShellProbeTerminalStage::ShCreateItemFromIdList:
            terminalStage = "sh_create_item_from_id_list";
            break;
        case ShellProbeTerminalStage::IShellItemGetAttributes:
            terminalStage = "ishellitem_get_attributes";
            break;
        case ShellProbeTerminalStage::IShellItemGetDisplayName:
            terminalStage = "ishellitem_get_display_name";
            break;
        case ShellProbeTerminalStage::Complete:
            terminalStage = "complete";
            break;
        default:
            return {
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER,
            };
    }

    const std::string keyPrefix{prefix};
    const std::string terminalStageKey = keyPrefix + ".shell_terminal_stage";
    ReportField* existingTerminalStage = nullptr;
    for (ReportField& field : output->fields) {
        if (field.key != terminalStageKey) {
            continue;
        }
        if (existingTerminalStage != nullptr) {
            return {
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                E_INVALIDARG,
                ERROR_INVALID_PARAMETER,
            };
        }
        existingTerminalStage = &field;
    }
    if (existingTerminalStage == nullptr) {
        output->fields.push_back({terminalStageKey, std::move(terminalStage)});
    } else {
        existingTerminalStage->value = std::move(terminalStage);
    }
    output->fields.push_back({
        keyPrefix + ".top_level_entry_count",
        std::to_string(snapshot.top_level_entry_count),
    });
    output->fields.push_back({
        keyPrefix + ".active_view_count",
        std::to_string(snapshot.active_view_count),
    });
    output->fields.push_back({
        keyPrefix + ".shell_tab_match_count",
        std::to_string(snapshot.shell_tab_match_count),
    });
    std::ostringstream handle;
    handle << "0x" << std::uppercase << std::hex << std::setw(sizeof(void*) * 2)
           << std::setfill('0') << reinterpret_cast<std::uintptr_t>(snapshot.active_view);
    output->fields.push_back({keyPrefix + ".shell_view", handle.str()});
    output->fields.push_back({
        keyPrefix + ".filesystem_path_available",
        snapshot.filesystem_path_available ? "true" : "false",
    });
    output->fields.push_back({
        keyPrefix + ".shell_hresult",
        std::to_string(snapshot.status.hresult),
    });
    output->fields.push_back({
        keyPrefix + ".shell_win32",
        std::to_string(snapshot.status.win32),
    });

    std::string filesystemPath;
    if (snapshot.filesystem_path_available) {
        const Status conversion = Utf8FromUtf16(snapshot.filesystem_path, &filesystemPath);
        if (!conversion.ok()) {
            return {
                ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                conversion.hresult,
                conversion.win32,
            };
        }
    }
    output->fields.push_back({keyPrefix + ".filesystem_path", std::move(filesystemPath)});
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace winexinfo
