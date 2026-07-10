#include "common/utf8.h"
#include "host/command_line.h"
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
    if (!parsed.ok() || command.command != winexinfo::HostCommand::ProbeSnapshot) {
        std::cerr << "INVALID_ARGUMENT: WinExInfoHost.exe --probe snapshot\n";
        return static_cast<int>(winexinfo::HostExitCode::InvalidCli);
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        const winexinfo::ProbeReport report{
            winexinfo::ProbeMode::Snapshot,
            false,
            {{
                {
                    {"com_initialize_hresult", std::to_string(initialized)},
                },
            }},
            winexinfo::ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
        };
        std::cout << winexinfo::WriteProbeReport(report);
        return static_cast<int>(winexinfo::HostExitCode::Win32ComFailure);
    }

    winexinfo::ProbeRunResult result = winexinfo::RunSnapshotProbe();
    std::cout << winexinfo::WriteProbeReport(result.report);
    CoUninitialize();
    return static_cast<int>(result.exit_code);
}
