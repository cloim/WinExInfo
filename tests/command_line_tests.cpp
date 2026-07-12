#include "test_framework.h"

#include "host/command_line.h"
#include "probe/probe_runner.h"
#include "probe/report_writer.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>

namespace {

void RequireInvalid(const std::initializer_list<std::string_view> arguments) {
    winexinfo::ParsedCommand parsed{};
    const winexinfo::Status status = winexinfo::ParseCommandLine(
        std::span<const std::string_view>{arguments.begin(), arguments.size()},
        &parsed);
    WXI_REQUIRE_EQ(status.code, winexinfo::ErrorCode::INVALID_ARGUMENT);
}

WXI_TEST(command_line_snapshot_exact, "command_line.snapshot_exact") {
    constexpr std::string_view arguments[] = {"--probe", "snapshot"};
    winexinfo::ParsedCommand parsed{};

    const winexinfo::Status status = winexinfo::ParseCommandLine(arguments, &parsed);

    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(parsed.command, winexinfo::HostCommand::ProbeSnapshot);
    WXI_REQUIRE_EQ(parsed.duration_ms, std::uint32_t{0});
    static_assert(static_cast<int>(winexinfo::HostExitCode::Pass) == 0);
    static_assert(static_cast<int>(winexinfo::HostExitCode::ContractFailure) == 1);
    static_assert(static_cast<int>(winexinfo::HostExitCode::InvalidCli) == 2);
    static_assert(static_cast<int>(winexinfo::HostExitCode::Win32ComFailure) == 3);
}

WXI_TEST(command_line_observe_exact, "command_line.observe_exact") {
    constexpr std::string_view arguments[] = {
        "--probe",
        "observe",
        "--duration-ms",
        "12345",
    };
    winexinfo::ParsedCommand parsed{};

    const winexinfo::Status status = winexinfo::ParseCommandLine(arguments, &parsed);

    WXI_REQUIRE(status.ok());
    WXI_REQUIRE_EQ(parsed.command, winexinfo::HostCommand::ProbeObserve);
    WXI_REQUIRE_EQ(parsed.duration_ms, std::uint32_t{12345});
}

WXI_TEST(command_line_gate_c_exact, "command_line.gate_c") {
    constexpr std::string_view arguments[] = {
        "--gate-c-place",
        "--hwnd",
        "0x000000001234ABCD",
        "--duration-ms",
        "15000",
    };
    winexinfo::ParsedCommand command{};

    WXI_REQUIRE(winexinfo::ParseCommandLine(arguments, &command).ok());
    WXI_REQUIRE_EQ(command.command, winexinfo::HostCommand::GateCPlace);
    WXI_REQUIRE_EQ(command.target_hwnd, std::uint64_t{0x1234ABCD});
    WXI_REQUIRE_EQ(command.duration_ms, std::uint32_t{15000});
}

WXI_TEST(command_line_gate_c_rejects_non_exact, "command_line.gate_c_invalid") {
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x000000001234abcd",
        "--duration-ms",
        "15000",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "000000001234ABCD",
        "--duration-ms",
        "15000",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x00000001234ABCD",
        "--duration-ms",
        "15000",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x0000000001234ABCD",
        "--duration-ms",
        "15000",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x0000000000000000",
        "--duration-ms",
        "15000",
    });
    RequireInvalid({
        "--gate-c-place",
        "--duration-ms",
        "15000",
        "--hwnd",
        "0x000000001234ABCD",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x000000001234ABCD",
        "--duration-ms",
        "15000",
        "extra",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x000000001234ABCD",
        "--duration-ms",
        "4999",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x000000001234ABCD",
        "--duration-ms",
        "30001",
    });
    RequireInvalid({
        "--gate-c-place",
        "--hwnd",
        "0x000000001234ABCD",
        "--duration-ms",
        "15000",
        "--probe",
        "snapshot",
    });
    RequireInvalid({
        "--probe",
        "snapshot",
        "--gate-c-place",
        "--hwnd",
        "0x000000001234ABCD",
        "--duration-ms",
        "15000",
    });
}

WXI_TEST(command_line_rejects_unknown, "command_line.rejects_unknown") {
    RequireInvalid({});
    RequireInvalid({"--probe"});
    RequireInvalid({"/probe", "snapshot"});
    RequireInvalid({"--snapshot"});
    RequireInvalid({"--probe", "Snapshot"});
    RequireInvalid({"--probe", "unknown"});
    RequireInvalid({"--probe", "snapshot", "extra"});
    RequireInvalid({"--probe", "observe", "--duration", "1000"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "1000", "extra"});
}

WXI_TEST(command_line_rejects_duplicate, "command_line.rejects_duplicate") {
    RequireInvalid({"--probe", "snapshot", "--probe", "snapshot"});
    RequireInvalid({"--probe", "--probe"});
    RequireInvalid({
        "--probe",
        "observe",
        "--duration-ms",
        "1000",
        "--duration-ms",
        "2000",
    });
}

WXI_TEST(command_line_rejects_duration_bounds, "command_line.rejects_duration_bounds") {
    RequireInvalid({"--probe", "observe"});
    RequireInvalid({"--probe", "observe", "--duration-ms"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "999"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "60001"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "1.000"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "+1000"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "-1000"});
    RequireInvalid({"--probe", "observe", "--duration-ms", " 1000"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "1000 "});
    RequireInvalid({"--probe", "observe", "--duration-ms", "1 000"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "4294967296"});
    RequireInvalid({"--probe", "observe", "--duration-ms", "18446744073709551616"});
}

WXI_TEST(report_escapes_control_characters, "report.escapes_control_characters") {
    const winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Snapshot,
        true,
        {
            winexinfo::ReportSection{{
                winexinfo::ReportField{"detail", "left\rright\nkey=value%done\tutf8-\xE2\x9C\x93"},
            }},
        },
        winexinfo::ErrorCode::OK,
    };

    std::string output;
    WXI_REQUIRE(winexinfo::WriteProbeReport(report, &output).ok());

    WXI_REQUIRE_EQ(
        output,
        std::string{
            "probe_version=1\n"
            "mode=snapshot\n"
            "result=pass\n"
            "detail=left%0Dright%0Akey%3Dvalue%25done\tutf8-\xE2\x9C\x93\n"
            "error_code=OK\n"});
    WXI_REQUIRE(!output.starts_with(std::string_view{"\xEF\xBB\xBF", 3}));
    WXI_REQUIRE_EQ(output.find('\r'), std::string::npos);
}

WXI_TEST(report_orders_keys, "report.orders_keys") {
    const winexinfo::ProbeReport report{
        winexinfo::ProbeMode::Observe,
        false,
        {
            winexinfo::ReportSection{{
                winexinfo::ReportField{"zeta", "1"},
                winexinfo::ReportField{"alpha", "2"},
            }},
            winexinfo::ReportSection{{
                winexinfo::ReportField{"theta", "3"},
                winexinfo::ReportField{"beta", "4"},
            }},
        },
        winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
    };

    std::string output;
    WXI_REQUIRE(winexinfo::WriteProbeReport(report, &output).ok());
    WXI_REQUIRE_EQ(
        output,
        std::string{
            "probe_version=1\n"
            "mode=observe\n"
            "result=fail\n"
            "alpha=2\n"
            "zeta=1\n"
            "beta=4\n"
            "theta=3\n"
            "error_code=EXPLORER_UI_CONTRACT_MISMATCH\n"});
}

WXI_TEST(
    report_observe_infrastructure_failure_uses_fixed_schema,
    "report.observe_infrastructure_failure_schema") {
    const winexinfo::Status failure{
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
        RPC_E_CHANGED_MODE,
        ERROR_SUCCESS,
    };
    const winexinfo::ProbeRunResult result =
        winexinfo::CreateObserveInfrastructureFailure(
            45000,
            failure,
            "host.com_initialize");
    WXI_REQUIRE_EQ(
        result.exit_code,
        winexinfo::HostExitCode::Win32ComFailure);
    WXI_REQUIRE_EQ(
        result.report.error_code,
        winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH);
    std::string output;
    WXI_REQUIRE(winexinfo::WriteProbeReport(result.report, &output).ok());
    WXI_REQUIRE(output.find("mode=observe\n") != std::string::npos);
    WXI_REQUIRE(output.find("result=fail\n") != std::string::npos);
    WXI_REQUIRE(
        output.find("observe.runtime.stage=host.com_initialize\n") !=
        std::string::npos);
    WXI_REQUIRE(
        output.find(
            "observe.runtime.hresult=" +
            std::to_string(static_cast<long>(RPC_E_CHANGED_MODE)) + "\n") !=
        std::string::npos);
    WXI_REQUIRE(
        output.find("error_code=ACTIVE_VIEW_CONTRACT_MISMATCH\n") !=
        std::string::npos);
    WXI_REQUIRE_EQ(output.find("observe_report_"), std::string::npos);
}

}  // namespace
