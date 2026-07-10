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

    const std::string keyPrefix{prefix};
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
