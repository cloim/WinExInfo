#include "common/utf8.h"
#include "host/command_line.h"
#include "host/com_apartment.h"
#include "probe/probe_runner.h"
#include "probe/report_writer.h"

#include <Windows.h>
#include <objbase.h>

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

int wmain(const int argc, const wchar_t* const argv[]) {
    std::vector<std::string> utf8Arguments;
    std::vector<std::string_view> argumentViews;
    utf8Arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
    for (int index = 1; index < argc; ++index) {
        std::string argument;
        const winexinfo::Status conversion = winexinfo::Utf8FromUtf16(argv[index], &argument);
        if (!conversion.ok()) {
            std::cerr << "INVALID_ARGUMENT: invalid UTF-16 command line\n";
            return static_cast<int>(winexinfo::HostExitCode::InvalidCli);
        }
        utf8Arguments.push_back(std::move(argument));
    }
    argumentViews.reserve(utf8Arguments.size());
    for (const std::string& argument : utf8Arguments) {
        argumentViews.push_back(argument);
    }

    winexinfo::ParsedCommand command{};
    const winexinfo::Status parsed = winexinfo::ParseCommandLine(argumentViews, &command);
    if (!parsed.ok()) {
        std::cerr << "INVALID_ARGUMENT: WinExInfoHost.exe --probe snapshot | "
                     "--probe observe --duration-ms <1000..60000>\n";
        return static_cast<int>(winexinfo::HostExitCode::InvalidCli);
    }

    const HRESULT initialized =
        CoInitializeEx(nullptr, winexinfo::kShellComApartmentFlags);
    if (FAILED(initialized)) {
        const winexinfo::ProbeRunResult failureResult =
            command.command == winexinfo::HostCommand::ProbeObserve
            ? winexinfo::CreateObserveInfrastructureFailure(
                  command.duration_ms,
                  {
                      winexinfo::ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
                      initialized,
                      ERROR_SUCCESS,
                  },
                  "host.com_initialize")
            : winexinfo::ProbeRunResult{
                  {
                      winexinfo::ProbeMode::Snapshot,
                      false,
                      {{
                          {
                              {"com_initialize_hresult",
                               std::to_string(initialized)},
                          },
                      }},
                      winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
                  },
                  winexinfo::HostExitCode::Win32ComFailure,
              };
        std::string serialized;
        const winexinfo::Status written =
            winexinfo::WriteProbeReport(failureResult.report, &serialized);
        if (!written.ok()) {
            std::cerr << "ACTIVE_VIEW_CONTRACT_MISMATCH: report serialization failed\n";
            return static_cast<int>(winexinfo::HostExitCode::Win32ComFailure);
        }
        std::cout << serialized;
        return static_cast<int>(failureResult.exit_code);
    }

    winexinfo::ProbeRunResult result =
        command.command == winexinfo::HostCommand::ProbeObserve
        ? winexinfo::RunObserveProbe(command.duration_ms)
        : winexinfo::RunSnapshotProbe();
    std::string serialized;
    const winexinfo::Status written =
        winexinfo::WriteProbeReport(result.report, &serialized);
    if (!written.ok()) {
        CoUninitialize();
        std::cerr << "ACTIVE_VIEW_CONTRACT_MISMATCH: report serialization failed\n";
        return static_cast<int>(
            winexinfo::IsProbeTransportFailure(written)
                ? winexinfo::HostExitCode::Win32ComFailure
                : winexinfo::HostExitCode::ContractFailure);
    }
    std::cout << serialized;
    CoUninitialize();
    return static_cast<int>(result.exit_code);
}
