#include "probe/report_writer.h"

#include <algorithm>
#include <exception>
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

}  // namespace winexinfo
