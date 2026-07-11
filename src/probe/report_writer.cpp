#include "probe/report_writer.h"

#include "common/utf8.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace winexinfo {
namespace {

Status ReportFailure(
    const HRESULT hresult = S_FALSE,
    const DWORD win32 = ERROR_SUCCESS) {
    return {ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH, hresult, win32};
}

bool TryErrorCodeName(const ErrorCode code, std::string_view* const output) {
    if (output == nullptr) {
        return false;
    }
    switch (code) {
        case ErrorCode::OK:
        case ErrorCode::INVALID_ARGUMENT:
        case ErrorCode::UNSUPPORTED_OS_BUILD:
        case ErrorCode::UNSUPPORTED_EXPLORER_BUILD:
        case ErrorCode::TARGET_VALIDATION_FAILED:
        case ErrorCode::TARGET_MITIGATION_BLOCKED:
        case ErrorCode::HOOK_INSTALL_FAILED:
        case ErrorCode::HOOK_TRIGGER_FAILED:
        case ErrorCode::HOOK_RELEASE_FAILED:
        case ErrorCode::DLL_INITIALIZATION_FAILED:
        case ErrorCode::WINDOW_ATTACH_FAILED:
        case ErrorCode::DLL_UNLOAD_TIMEOUT:
        case ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH:
        case ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH:
        case ErrorCode::IPC_PROTOCOL_ERROR:
        case ErrorCode::PIPE_DISCONNECTED:
            *output = ToString(code);
            return true;
    }
    return false;
}

std::string FormatReportHwnd(const HWND hwnd) {
    std::ostringstream stream;
    stream << "0x" << std::uppercase << std::hex << std::setw(sizeof(void*) * 2)
           << std::setfill('0') << reinterpret_cast<std::uintptr_t>(hwnd);
    return stream.str();
}

bool IsValidReportField(const ReportField& field) {
    if (field.key.empty() || field.value.find('\0') != std::string::npos) {
        return false;
    }
    for (const unsigned char character : field.key) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '_' ||
            character == '.') {
            continue;
        }
        return false;
    }
    return true;
}

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

Status WriteProbeReport(
    const ProbeReport& report,
    std::string* const output) {
    if (output == nullptr) {
        return ReportFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::string mode;
    switch (report.mode) {
        case ProbeMode::Snapshot:
            mode = "snapshot";
            break;
        case ProbeMode::Observe:
            mode = "observe";
            break;
        default:
            return ReportFailure();
    }

    std::string_view reportErrorName;
    if (!TryErrorCodeName(report.error_code, &reportErrorName) ||
        report.passed != (report.error_code == ErrorCode::OK)) {
        return ReportFailure();
    }

    std::set<std::string> keys{
        "probe_version",
        "mode",
        "result",
        "error_code",
    };
    for (const ReportSection& section : report.sections) {
        for (const ReportField& field : section.fields) {
            if (!IsValidReportField(field) || !keys.insert(field.key).second) {
                return ReportFailure();
            }
        }
    }

    std::string serialized;
    AppendLine(&serialized, "probe_version", "1");
    AppendLine(&serialized, "mode", mode);
    AppendLine(&serialized, "result", report.passed ? "pass" : "fail");

    for (const ReportSection& section : report.sections) {
        std::vector<ReportField> fields = section.fields;
        std::ranges::stable_sort(fields, {}, &ReportField::key);
        for (const ReportField& field : fields) {
            AppendLine(&serialized, field.key, field.value);
        }
    }

    AppendLine(&serialized, "error_code", reportErrorName);
    *output = std::move(serialized);
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
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
    output->fields.push_back({
        keyPrefix + ".shell_view",
        FormatReportHwnd(snapshot.active_view),
    });
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

Status AppendEventObservationReportFields(
    const EventObservationSnapshot& snapshot,
    ProbeReport* const output) {
    if (output == nullptr) {
        return ReportFailure(E_INVALIDARG, ERROR_INVALID_PARAMETER);
    }
    std::string_view outputErrorName;
    if (output->mode != ProbeMode::Observe ||
        !TryErrorCodeName(output->error_code, &outputErrorName) ||
        output->passed != (output->error_code == ErrorCode::OK) ||
        snapshot.duration_ms < 1000 || snapshot.duration_ms > 60000 ||
        snapshot.event_count != snapshot.events.size()) {
        return ReportFailure();
    }
    static_cast<void>(outputErrorName);

    const bool runtimeSuccess = snapshot.runtime_status.code == ErrorCode::OK &&
        snapshot.runtime_status.hresult == S_OK &&
        snapshot.runtime_status.win32 == ERROR_SUCCESS;
    std::string_view runtimeErrorName;
    const bool runtimeStatusCoherent =
        TryErrorCodeName(snapshot.runtime_status.code, &runtimeErrorName) &&
        (runtimeSuccess ||
         (snapshot.runtime_status.code ==
              ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
          (snapshot.runtime_status.hresult != S_OK ||
           snapshot.runtime_status.win32 != ERROR_SUCCESS)));
    if (!runtimeStatusCoherent) {
        return ReportFailure();
    }

    const bool cleanupSuccess = snapshot.cleanup_status.code == ErrorCode::OK &&
        snapshot.cleanup_status.hresult == S_OK &&
        snapshot.cleanup_status.win32 == ERROR_SUCCESS;
    std::string_view cleanupErrorName;
    const bool cleanupStatusCoherent =
        TryErrorCodeName(snapshot.cleanup_status.code, &cleanupErrorName) &&
        (cleanupSuccess ||
         (snapshot.cleanup_status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
          (snapshot.cleanup_status.hresult != S_OK ||
           snapshot.cleanup_status.win32 != ERROR_SUCCESS)));
    if (!cleanupStatusCoherent) {
        return ReportFailure();
    }

    ObservedEventKindCounts actualCounts{};
    bool hasMismatch = false;
    bool hasNonSuccessEvent = false;
    bool hasTabAddition = false;
    bool hasTabRemoval = false;
    bool hasSelectionRemap = false;
    bool hasLifecycleRemoval = false;
    bool hasNavigatePathTransition = false;
    std::set<std::tuple<std::uintptr_t, std::uint64_t, LONG>> unresolvedPending;
    std::vector<ReportSection> additions;
    additions.reserve(snapshot.events.size() + 2);

    ReportSection summary{};
    summary.fields = {
        {"observe.duration_ms", std::to_string(snapshot.duration_ms)},
        {"observe.event_count", std::to_string(snapshot.event_count)},
        {"observe.ignored_event_count", std::to_string(snapshot.ignored_event_count)},
        {"observe.late_event_count", std::to_string(snapshot.late_event_count)},
        {"observe.kind.window_registered_count", std::to_string(snapshot.kind_counts.window_registered)},
        {"observe.kind.window_revoked_count", std::to_string(snapshot.kind_counts.window_revoked)},
        {"observe.kind.navigate_complete2_count", std::to_string(snapshot.kind_counts.navigate_complete2)},
        {"observe.kind.tab_selected_count", std::to_string(snapshot.kind_counts.tab_selected)},
        {"observe.kind.tab_structure_changed_count", std::to_string(snapshot.kind_counts.tab_structure_changed)},
        {"observe.cleanup.error_code", std::string{cleanupErrorName}},
        {"observe.cleanup.hresult", std::to_string(snapshot.cleanup_status.hresult)},
        {"observe.cleanup.win32", std::to_string(snapshot.cleanup_status.win32)},
    };
    additions.push_back(std::move(summary));

    ReportSection runtime{};
    runtime.fields = {
        {"observe.runtime.error_code", std::string{runtimeErrorName}},
        {"observe.runtime.hresult", std::to_string(snapshot.runtime_status.hresult)},
        {"observe.runtime.win32", std::to_string(snapshot.runtime_status.win32)},
    };
    if (!snapshot.runtime_stage.empty()) {
        runtime.fields.push_back({
            "observe.runtime.stage",
            snapshot.runtime_stage,
        });
    }
    additions.push_back(std::move(runtime));

    for (std::size_t index = 0; index < snapshot.events.size(); ++index) {
        const ObservedEventRecord& event = snapshot.events[index];
        if (event.sequence != index + 1 || event.generation == 0 ||
            event.source_top_level == nullptr ||
            event.source_shell_tab_present != (event.source_shell_tab != nullptr) ||
            (!event.shell_cookie_present && event.shell_cookie != 0) ||
            event.previous_filesystem_path_available !=
                !event.previous_filesystem_path.empty() ||
            event.current_filesystem_path_available !=
                !event.current_filesystem_path.empty() ||
            (event.previous_filesystem_path_available &&
             event.previous_active_view == nullptr) ||
            (event.current_filesystem_path_available &&
             event.current_active_view == nullptr)) {
            return ReportFailure();
        }
        if (event.removed_tab_count > event.previous_tab_count ||
            event.added_tab_count > event.current_tab_count ||
            event.retained_tab_count !=
                event.previous_tab_count - event.removed_tab_count ||
            event.retained_tab_count !=
                event.current_tab_count - event.added_tab_count ||
            (event.current_tab_count == 0) !=
                (event.reconciled_active_shell_tab == nullptr)) {
            return ReportFailure();
        }

        std::string kindName;
        bool lifecycleKind = false;
        bool structureKind = false;
        switch (event.kind) {
            case ObservedEventKind::WindowRegistered:
                kindName = "window_registered";
                lifecycleKind = true;
                ++actualCounts.window_registered;
                break;
            case ObservedEventKind::WindowRevoked:
                kindName = "window_revoked";
                lifecycleKind = true;
                ++actualCounts.window_revoked;
                break;
            case ObservedEventKind::NavigateComplete2:
                kindName = "navigate_complete2";
                ++actualCounts.navigate_complete2;
                break;
            case ObservedEventKind::TabSelected:
                kindName = "tab_selected";
                ++actualCounts.tab_selected;
                break;
            case ObservedEventKind::TabStructureChanged:
                kindName = "tab_structure_changed";
                structureKind = true;
                ++actualCounts.tab_structure_changed;
                break;
            default:
                return ReportFailure();
        }
        if (lifecycleKind != event.shell_cookie_present ||
            ((event.kind == ObservedEventKind::NavigateComplete2 || lifecycleKind) &&
             !event.source_shell_tab_present) ||
            (event.kind == ObservedEventKind::NavigateComplete2 &&
             !event.source_was_active)) {
            return ReportFailure();
        }

        std::string structureName;
        switch (event.structure_change_type) {
            case ObservedStructureChangeType::None:
                structureName = "none";
                break;
            case ObservedStructureChangeType::ChildAdded:
                structureName = "child_added";
                break;
            case ObservedStructureChangeType::ChildRemoved:
                structureName = "child_removed";
                break;
            case ObservedStructureChangeType::ChildrenInvalidated:
                structureName = "children_invalidated";
                break;
            case ObservedStructureChangeType::ChildrenBulkAdded:
                structureName = "children_bulk_added";
                break;
            case ObservedStructureChangeType::ChildrenBulkRemoved:
                structureName = "children_bulk_removed";
                break;
            case ObservedStructureChangeType::ChildrenReordered:
                structureName = "children_reordered";
                break;
            default:
                return ReportFailure();
        }
        if (structureKind !=
            (event.structure_change_type != ObservedStructureChangeType::None)) {
            return ReportFailure();
        }

        std::string_view eventErrorName;
        const bool eventSuccess = event.status.code == ErrorCode::OK &&
            event.status.hresult == S_OK && event.status.win32 == ERROR_SUCCESS;
        const bool eventStatusCoherent =
            TryErrorCodeName(event.status.code, &eventErrorName) &&
            (eventSuccess ||
             (event.status.code == ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH &&
              (event.status.hresult != S_OK || event.status.win32 != ERROR_SUCCESS)));
        if (!eventStatusCoherent) {
            return ReportFailure();
        }

        std::string transitionName;
        bool transitionValid = false;
        switch (event.transition) {
            case ObservedEventTransition::Pending:
                transitionName = "pending";
                transitionValid =
                    event.kind == ObservedEventKind::WindowRegistered &&
                    event.current_active_view == nullptr &&
                    event.active_view_count == 0 &&
                    !event.current_filesystem_path_available && eventSuccess;
                break;
            case ObservedEventTransition::Revoked:
                transitionName = "revoked";
                transitionValid = event.kind == ObservedEventKind::WindowRevoked &&
                    event.current_active_view == nullptr &&
                    event.active_view_count == 0 &&
                    !event.current_filesystem_path_available && eventSuccess;
                break;
            case ObservedEventTransition::Remapped:
                transitionName = "remapped";
                transitionValid = event.current_active_view != nullptr &&
                    event.active_view_count == 1 && eventSuccess;
                break;
            case ObservedEventTransition::Mismatch:
                transitionName = "mismatch";
                transitionValid = event.current_active_view == nullptr &&
                    event.active_view_count != 1 &&
                    !event.current_filesystem_path_available && !eventSuccess;
                hasMismatch = true;
                break;
            case ObservedEventTransition::Reconciled:
                transitionName = "reconciled";
                transitionValid = lifecycleKind &&
                    event.current_active_view == nullptr &&
                    event.active_view_count == 0 &&
                    !event.current_filesystem_path_available && eventSuccess;
                break;
            default:
                return ReportFailure();
        }
        const bool kindTransitionAllowed =
            (event.kind == ObservedEventKind::WindowRegistered &&
             (event.transition == ObservedEventTransition::Pending ||
              event.transition == ObservedEventTransition::Remapped ||
              event.transition == ObservedEventTransition::Mismatch ||
              event.transition == ObservedEventTransition::Reconciled)) ||
            (event.kind == ObservedEventKind::WindowRevoked &&
              (event.transition == ObservedEventTransition::Revoked ||
               event.transition == ObservedEventTransition::Mismatch ||
               event.transition == ObservedEventTransition::Reconciled)) ||
            ((event.kind == ObservedEventKind::NavigateComplete2 ||
              event.kind == ObservedEventKind::TabSelected ||
              event.kind == ObservedEventKind::TabStructureChanged) &&
             (event.transition == ObservedEventTransition::Remapped ||
              event.transition == ObservedEventTransition::Mismatch));
        if (!transitionValid || !kindTransitionAllowed) {
            return ReportFailure();
        }
        hasNonSuccessEvent = hasNonSuccessEvent || !eventSuccess;
        hasTabAddition = hasTabAddition || event.added_tab_count > 0;
        hasTabRemoval = hasTabRemoval || event.removed_tab_count > 0;
        hasSelectionRemap = hasSelectionRemap ||
            (event.kind == ObservedEventKind::TabSelected &&
             event.transition == ObservedEventTransition::Remapped &&
             event.active_view_count == 1);
        hasLifecycleRemoval = hasLifecycleRemoval ||
            (event.kind == ObservedEventKind::WindowRevoked &&
             event.removed_tab_count > 0);
        hasNavigatePathTransition = hasNavigatePathTransition ||
            (event.kind == ObservedEventKind::NavigateComplete2 &&
             event.transition == ObservedEventTransition::Remapped &&
             event.previous_filesystem_path_available &&
             event.current_filesystem_path_available &&
             event.previous_filesystem_path != event.current_filesystem_path);

        const auto pendingIdentity =
            std::tuple{
                reinterpret_cast<std::uintptr_t>(event.source_top_level),
                event.generation,
                event.shell_cookie,
            };
        if (event.transition == ObservedEventTransition::Pending) {
            if (!unresolvedPending.insert(pendingIdentity).second) {
                return ReportFailure();
            }
        } else if (event.kind == ObservedEventKind::WindowRevoked) {
            unresolvedPending.erase(pendingIdentity);
        } else if (event.transition == ObservedEventTransition::Remapped) {
            for (auto pending = unresolvedPending.begin();
                 pending != unresolvedPending.end();) {
                if (std::get<0>(*pending) ==
                        reinterpret_cast<std::uintptr_t>(event.source_top_level) &&
                    std::get<1>(*pending) == event.generation) {
                    pending = unresolvedPending.erase(pending);
                } else {
                    ++pending;
                }
            }
        }

        std::string previousPath;
        if (event.previous_filesystem_path_available) {
            const Status conversion =
                Utf8FromUtf16(event.previous_filesystem_path, &previousPath);
            if (!conversion.ok()) {
                return ReportFailure(conversion.hresult, conversion.win32);
            }
        }
        std::string currentPath;
        if (event.current_filesystem_path_available) {
            const Status conversion =
                Utf8FromUtf16(event.current_filesystem_path, &currentPath);
            if (!conversion.ok()) {
                return ReportFailure(conversion.hresult, conversion.win32);
            }
        }

        std::ostringstream sequenceText;
        sequenceText << std::setw(20) << std::setfill('0') << event.sequence;
        const std::string prefix = "event." + sequenceText.str() + ".";
        ReportSection section{};
        section.fields = {
            {prefix + "sequence", std::to_string(event.sequence)},
            {prefix + "generation", std::to_string(event.generation)},
            {prefix + "kind", std::move(kindName)},
            {prefix + "transition", std::move(transitionName)},
            {prefix + "source_top_level_hwnd", FormatReportHwnd(event.source_top_level)},
            {prefix + "source_shell_tab_present", event.source_shell_tab_present ? "true" : "false"},
            {prefix + "source_shell_tab_hwnd", FormatReportHwnd(event.source_shell_tab)},
            {prefix + "source_was_active", event.source_was_active ? "true" : "false"},
            {prefix + "structure_change_type", std::move(structureName)},
            {prefix + "reconcile.previous_tab_count", std::to_string(event.previous_tab_count)},
            {prefix + "reconcile.current_tab_count", std::to_string(event.current_tab_count)},
            {prefix + "reconcile.added_tab_count", std::to_string(event.added_tab_count)},
            {prefix + "reconcile.removed_tab_count", std::to_string(event.removed_tab_count)},
            {prefix + "reconcile.retained_tab_count", std::to_string(event.retained_tab_count)},
            {prefix + "reconcile.active_shell_tab", FormatReportHwnd(event.reconciled_active_shell_tab)},
            {prefix + "previous_active_view_hwnd", FormatReportHwnd(event.previous_active_view)},
            {prefix + "current_active_view_hwnd", FormatReportHwnd(event.current_active_view)},
            {prefix + "active_view_count", std::to_string(event.active_view_count)},
            {prefix + "previous_filesystem_path_available", event.previous_filesystem_path_available ? "true" : "false"},
            {prefix + "previous_filesystem_path", std::move(previousPath)},
            {prefix + "current_filesystem_path_available", event.current_filesystem_path_available ? "true" : "false"},
            {prefix + "current_filesystem_path", std::move(currentPath)},
            {prefix + "status.error_code", std::string{eventErrorName}},
            {prefix + "status.hresult", std::to_string(event.status.hresult)},
            {prefix + "status.win32", std::to_string(event.status.win32)},
        };
        if (lifecycleKind) {
            section.fields.push_back({
                prefix + "lifecycle.cookie",
                std::to_string(event.shell_cookie),
            });
        }
        additions.push_back(std::move(section));
    }

    if (actualCounts.window_registered != snapshot.kind_counts.window_registered ||
        actualCounts.window_revoked != snapshot.kind_counts.window_revoked ||
        actualCounts.navigate_complete2 != snapshot.kind_counts.navigate_complete2 ||
        actualCounts.tab_selected != snapshot.kind_counts.tab_selected ||
        actualCounts.tab_structure_changed != snapshot.kind_counts.tab_structure_changed) {
        return ReportFailure();
    }
    const bool hasAllKinds = actualCounts.window_registered > 0 &&
        actualCounts.window_revoked > 0 && actualCounts.navigate_complete2 > 0 &&
        actualCounts.tab_selected > 0 && actualCounts.tab_structure_changed > 0;
    const bool passShape = hasAllKinds && runtimeSuccess && cleanupSuccess &&
        !hasMismatch && !hasNonSuccessEvent && unresolvedPending.empty() &&
        hasTabAddition && hasTabRemoval && hasSelectionRemap &&
        hasLifecycleRemoval && hasNavigatePathTransition;
    const bool failureEvidence = !hasAllKinds || !runtimeSuccess ||
        !cleanupSuccess || hasMismatch || hasNonSuccessEvent ||
        !unresolvedPending.empty() || !hasTabAddition || !hasTabRemoval ||
        !hasSelectionRemap || !hasLifecycleRemoval ||
        !hasNavigatePathTransition;
    if ((output->passed && !passShape) || (!output->passed && !failureEvidence)) {
        return ReportFailure();
    }

    std::set<std::string> existingKeys{
        "probe_version",
        "mode",
        "result",
        "error_code",
    };
    for (const ReportSection& section : output->sections) {
        for (const ReportField& field : section.fields) {
            if (!IsValidReportField(field) ||
                !existingKeys.insert(field.key).second) {
                return ReportFailure();
            }
        }
    }
    for (const ReportSection& section : additions) {
        for (const ReportField& field : section.fields) {
            if (!IsValidReportField(field) ||
                !existingKeys.insert(field.key).second) {
                return ReportFailure();
            }
        }
    }
    output->sections.insert(
        output->sections.end(),
        std::make_move_iterator(additions.begin()),
        std::make_move_iterator(additions.end()));
    return {ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace winexinfo
