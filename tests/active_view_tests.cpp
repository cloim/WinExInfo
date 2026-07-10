#include "test_framework.h"

#include "probe/report_writer.h"
#include "probe/shell_probe.h"

#include <Windows.h>
#include <ShObjIdl_core.h>

#include <cstdint>
#include <string>

namespace {

HWND Handle(const std::uintptr_t value) {
    return reinterpret_cast<HWND>(value);
}

winexinfo::ShellViewEntryEvidence ExactEntry() {
    return {
        Handle(1),
        Handle(2),
        Handle(3),
        true,
        true,
        true,
        L"C:\\work\\repo",
    };
}

winexinfo::ActiveShellViewEvidence ExactEvidence() {
    return {
        {winexinfo::ErrorCode::OK, S_OK, ERROR_SUCCESS},
        {ExactEntry()},
    };
}

winexinfo::ActiveShellViewSnapshot Validate(
    const winexinfo::ActiveShellViewEvidence& evidence) {
    winexinfo::ActiveShellViewSnapshot snapshot{};
    snapshot.status = winexinfo::ValidateActiveShellViewEvidence(
        Handle(1), Handle(2), evidence, &snapshot);
    return snapshot;
}

void RequireMismatch(const winexinfo::ActiveShellViewEvidence& evidence) {
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE_EQ(
        snapshot.status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(snapshot.status.hresult, S_FALSE);
    WXI_REQUIRE_EQ(snapshot.status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(active_view_selects_one_exact_entry, "active_view.selects_one_exact_entry") {
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(ExactEvidence());

    WXI_REQUIRE(snapshot.status.ok());
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view, Handle(3));
    WXI_REQUIRE(snapshot.filesystem_path_available);
    WXI_REQUIRE_EQ(snapshot.filesystem_path, std::wstring{L"C:\\work\\repo"});
}

WXI_TEST(active_view_rejects_zero_top_level_entries, "active_view.rejects_zero_top_level_entries") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].browser_top_level = Handle(9);

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{0});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{0});
}

WXI_TEST(active_view_rejects_two_qualifying_entries, "active_view.rejects_two_qualifying_entries") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    winexinfo::ShellViewEntryEvidence second = ExactEntry();
    second.active_view = Handle(4);
    second.filesystem_path = L"C:\\work\\other";
    evidence.entries.push_back(second);

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view, nullptr);
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_rejects_two_shell_tabs_with_one_hidden_view, "active_view.rejects_two_shell_tabs_with_one_hidden_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    winexinfo::ShellViewEntryEvidence hidden = ExactEntry();
    hidden.active_view = Handle(4);
    hidden.active_view_visible = false;
    hidden.filesystem_path_available = false;
    hidden.filesystem_path.clear();
    evidence.entries.push_back(hidden);

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view, nullptr);
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_accepts_one_selected_tab_among_multiple_entries, "active_view.accepts_one_selected_tab_among_multiple_entries") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    for (const std::uintptr_t shellTab : {std::uintptr_t{7}, std::uintptr_t{8}}) {
        winexinfo::ShellViewEntryEvidence nonselected = ExactEntry();
        nonselected.shell_browser = Handle(shellTab);
        nonselected.active_view = nullptr;
        nonselected.active_view_visible = false;
        nonselected.active_view_descendant = false;
        nonselected.filesystem_path_available = false;
        nonselected.filesystem_path.clear();
        evidence.entries.push_back(nonselected);
    }

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE(snapshot.status.ok());
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{3});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.active_view, Handle(3));
}

WXI_TEST(active_view_rejects_hidden_view, "active_view.rejects_hidden_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].active_view_visible = false;
    RequireMismatch(evidence);
}

WXI_TEST(active_view_rejects_wrong_shell_tab, "active_view.rejects_wrong_shell_tab") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].shell_browser = Handle(8);
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{0});
}

WXI_TEST(active_view_rejects_view_outside_shell_tab, "active_view.rejects_view_outside_shell_tab") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].active_view_descendant = false;
    RequireMismatch(evidence);
}

WXI_TEST(active_view_accepts_non_filesystem_view, "active_view.accepts_non_filesystem_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path_available = false;
    evidence.entries[0].filesystem_path.clear();

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE(snapshot.status.ok());
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{1});
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_rejects_available_empty_path, "active_view.rejects_available_empty_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path.clear();
    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.active_view, nullptr);
    WXI_REQUIRE(!snapshot.filesystem_path_available);
    WXI_REQUIRE(snapshot.filesystem_path.empty());
}

WXI_TEST(active_view_rejects_unavailable_nonempty_path, "active_view.rejects_unavailable_nonempty_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path_available = false;
    RequireMismatch(evidence);
}

WXI_TEST(active_view_preserves_pidl_conversion_failure, "active_view.preserves_pidl_conversion_failure") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.capture_status = {
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        E_ACCESSDENIED,
        ERROR_SUCCESS,
    };

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    WXI_REQUIRE_EQ(
        snapshot.status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(snapshot.status.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(snapshot.status.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(active_view_classifies_filesystem_attribute, "active_view.classifies_filesystem_attribute") {
    bool available = false;
    const winexinfo::Status status = winexinfo::ClassifyFilesystemAttributes(
        S_OK, SFGAO_FILESYSTEM, &available);
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE(available);
}

WXI_TEST(active_view_classifies_non_filesystem_attribute, "active_view.classifies_non_filesystem_attribute") {
    bool available = true;
    const winexinfo::Status status =
        winexinfo::ClassifyFilesystemAttributes(S_FALSE, 0, &available);
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE(!available);
}

WXI_TEST(active_view_rejects_attribute_result_mismatch, "active_view.rejects_attribute_result_mismatch") {
    bool available = true;
    const winexinfo::Status missingBit =
        winexinfo::ClassifyFilesystemAttributes(S_OK, 0, &available);
    WXI_REQUIRE_EQ(
        missingBit.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(missingBit.hresult, S_FALSE);
    WXI_REQUIRE_EQ(missingBit.win32, DWORD{ERROR_SUCCESS});

    const winexinfo::Status unexpectedBit = winexinfo::ClassifyFilesystemAttributes(
        S_FALSE, SFGAO_FILESYSTEM, &available);
    WXI_REQUIRE_EQ(
        unexpectedBit.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(unexpectedBit.hresult, S_FALSE);
    WXI_REQUIRE_EQ(unexpectedBit.win32, DWORD{ERROR_SUCCESS});
}

WXI_TEST(active_view_preserves_attribute_transport_failure, "active_view.preserves_attribute_transport_failure") {
    bool available = false;
    const winexinfo::Status status = winexinfo::ClassifyFilesystemAttributes(
        E_ACCESSDENIED, 0, &available);
    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(active_view_classifies_shell_item_results, "active_view.classifies_shell_item_results") {
    bool useEntry = false;
    WXI_REQUIRE(
        winexinfo::ClassifyShellWindowsItemResult(S_OK, true, &useEntry).ok());
    WXI_REQUIRE(useEntry);

    useEntry = true;
    WXI_REQUIRE(
        winexinfo::ClassifyShellWindowsItemResult(S_FALSE, false, &useEntry).ok());
    WXI_REQUIRE(!useEntry);
}

WXI_TEST(active_view_rejects_shell_item_output_mismatch, "active_view.rejects_shell_item_output_mismatch") {
    bool useEntry = false;
    const winexinfo::Status missingDispatch =
        winexinfo::ClassifyShellWindowsItemResult(S_OK, false, &useEntry);
    WXI_REQUIRE_EQ(missingDispatch.hresult, S_FALSE);
    WXI_REQUIRE_EQ(
        missingDispatch.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);

    const winexinfo::Status unexpectedDispatch =
        winexinfo::ClassifyShellWindowsItemResult(S_FALSE, true, &useEntry);
    WXI_REQUIRE_EQ(unexpectedDispatch.hresult, S_FALSE);
    WXI_REQUIRE_EQ(
        unexpectedDispatch.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
}

WXI_TEST(active_view_classifies_browser_interface_results, "active_view.classifies_browser_interface_results") {
    bool useEntry = false;
    WXI_REQUIRE(
        winexinfo::ClassifyBrowserInterfaceResult(S_OK, true, &useEntry).ok());
    WXI_REQUIRE(useEntry);

    useEntry = true;
    WXI_REQUIRE(winexinfo::ClassifyBrowserInterfaceResult(
                    E_NOINTERFACE, false, &useEntry)
                    .ok());
    WXI_REQUIRE(!useEntry);
}

WXI_TEST(active_view_preserves_shell_item_transport_failure, "active_view.preserves_shell_item_transport_failure") {
    bool useEntry = false;
    const winexinfo::Status itemStatus = winexinfo::ClassifyShellWindowsItemResult(
        E_ACCESSDENIED, false, &useEntry);
    WXI_REQUIRE_EQ(itemStatus.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(itemStatus.win32, DWORD{ERROR_ACCESS_DENIED});

    const winexinfo::Status browserStatus = winexinfo::ClassifyBrowserInterfaceResult(
        E_ACCESSDENIED, false, &useEntry);
    WXI_REQUIRE_EQ(browserStatus.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(browserStatus.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(active_view_classifies_required_com_outputs, "active_view.classifies_required_com_outputs") {
    WXI_REQUIRE(winexinfo::ClassifyRequiredComObjectResult(S_OK, true).ok());

    const winexinfo::Status missing =
        winexinfo::ClassifyRequiredComObjectResult(S_OK, false);
    WXI_REQUIRE_EQ(
        missing.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(missing.hresult, S_FALSE);
    WXI_REQUIRE_EQ(missing.win32, DWORD{ERROR_SUCCESS});

    const winexinfo::Status failure =
        winexinfo::ClassifyRequiredComObjectResult(E_ACCESSDENIED, false);
    WXI_REQUIRE_EQ(failure.hresult, E_ACCESSDENIED);
    WXI_REQUIRE_EQ(failure.win32, DWORD{ERROR_ACCESS_DENIED});
}

WXI_TEST(active_view_compares_full_width_hwnds, "active_view.compares_full_width_hwnds") {
    constexpr std::uintptr_t topValue = 0xFEDCBA9876543210ull;
    constexpr std::uintptr_t tabValue = 0xABCDEF0123456789ull;
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].browser_top_level = Handle(topValue);
    evidence.entries[0].shell_browser = Handle(tabValue);
    winexinfo::ActiveShellViewSnapshot snapshot{};
    const winexinfo::Status status = winexinfo::ValidateActiveShellViewEvidence(
        Handle(topValue), Handle(tabValue), evidence, &snapshot);
    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{1});
    WXI_REQUIRE_EQ(snapshot.shell_tab_match_count, std::size_t{1});

    const winexinfo::Status truncated = winexinfo::ValidateActiveShellViewEvidence(
        Handle(static_cast<std::uint32_t>(topValue)),
        Handle(tabValue),
        evidence,
        &snapshot);
    WXI_REQUIRE_EQ(
        truncated.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
}

WXI_TEST(active_view_rejects_duplicate_entries_for_same_view, "active_view.rejects_duplicate_entries_for_same_view") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries.push_back(ExactEntry());

    const winexinfo::ActiveShellViewSnapshot snapshot = Validate(evidence);
    RequireMismatch(evidence);
    WXI_REQUIRE_EQ(snapshot.top_level_entry_count, std::size_t{2});
    WXI_REQUIRE_EQ(snapshot.active_view_count, std::size_t{2});
}

WXI_TEST(active_view_report_includes_exact_mapping, "active_view.report_includes_exact_mapping") {
    winexinfo::ReportSection section{};
    winexinfo::AppendActiveShellViewReportFields("window.5", Validate(ExactEvidence()), &section);
    const std::string report = winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    });

    WXI_REQUIRE(report.find("window.5.active_view_count=1\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.shell_tab_match_count=1\n") != std::string::npos);
    WXI_REQUIRE(
        report.find("window.5.shell_view=0x0000000000000003\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.filesystem_path_available=true\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.filesystem_path=C:\\work\\repo\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.shell_hresult=0\n") != std::string::npos);
    WXI_REQUIRE(report.find("window.5.shell_win32=0\n") != std::string::npos);
    WXI_REQUIRE(report.find("LocationURL") == std::string::npos);
    WXI_REQUIRE(report.find("window_title") == std::string::npos);
    const std::string keys[] = {
        "window.5.top_level_entry_count=",
        "window.5.shell_tab_match_count=",
        "window.5.active_view_count=",
        "window.5.shell_view=",
        "window.5.filesystem_path_available=",
        "window.5.filesystem_path=",
        "window.5.shell_hresult=",
        "window.5.shell_win32=",
    };
    for (const std::string& key : keys) {
        const std::size_t position = report.find(key);
        WXI_REQUIRE(position != std::string::npos);
        WXI_REQUIRE(report.find(key, position + 1) == std::string::npos);
    }
}

WXI_TEST(active_view_report_includes_empty_non_filesystem_path, "active_view.report_includes_empty_non_filesystem_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path_available = false;
    evidence.entries[0].filesystem_path.clear();
    winexinfo::ReportSection section{};
    WXI_REQUIRE(winexinfo::AppendActiveShellViewReportFields(
                    "window.0", Validate(evidence), &section)
                    .ok());
    const std::string report = winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    });
    WXI_REQUIRE(report.find("window.0.filesystem_path=\n") != std::string::npos);
    const std::size_t position = report.find("window.0.filesystem_path=");
    WXI_REQUIRE(report.find("window.0.filesystem_path=", position + 1) == std::string::npos);
}

WXI_TEST(active_view_report_encodes_korean_path, "active_view.report_encodes_korean_path") {
    winexinfo::ActiveShellViewEvidence evidence = ExactEvidence();
    evidence.entries[0].filesystem_path = L"C:\\작업\\저장소";
    winexinfo::ReportSection section{};
    const winexinfo::Status status = winexinfo::AppendActiveShellViewReportFields(
        "window.0", Validate(evidence), &section);
    WXI_REQUIRE(status.ok());
    const std::string report = winexinfo::WriteProbeReport({
        winexinfo::ProbeMode::Snapshot,
        true,
        {section},
        winexinfo::ErrorCode::OK,
    });
    WXI_REQUIRE(
        report.find("window.0.filesystem_path=C:\\작업\\저장소\n") != std::string::npos);
}

WXI_TEST(active_view_report_rejects_invalid_utf16_path, "active_view.report_rejects_invalid_utf16_path") {
    winexinfo::ActiveShellViewSnapshot snapshot = Validate(ExactEvidence());
    snapshot.filesystem_path = std::wstring{static_cast<wchar_t>(0xD800)};
    winexinfo::ReportSection section{};
    const winexinfo::Status status = winexinfo::AppendActiveShellViewReportFields(
        "window.0", snapshot, &section);

    WXI_REQUIRE_EQ(
        status.code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    WXI_REQUIRE_EQ(status.win32, DWORD{ERROR_NO_UNICODE_TRANSLATION});
    WXI_REQUIRE_EQ(status.hresult, HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION));
}

}  // namespace
