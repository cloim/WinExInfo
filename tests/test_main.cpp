#include "test_framework.h"

#include "host/command_line.h"
#include "hook_test_modes.h"

#include <iostream>
#include <span>
#include <string_view>
#include <vector>

int main(const int argc, const char* const argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--unit") {
        const int result = winexinfo::tests::RunUnitTests({});
        return result == 0 ? static_cast<int>(winexinfo::HostExitCode::Pass)
                           : static_cast<int>(winexinfo::HostExitCode::ContractFailure);
    }

    if (argc == 4 && std::string_view{argv[1]} == "--unit" &&
        std::string_view{argv[2]} == "--filter") {
        const int result = winexinfo::tests::RunUnitTests(argv[3]);
        return result == 0 ? static_cast<int>(winexinfo::HostExitCode::Pass)
                           : static_cast<int>(winexinfo::HostExitCode::ContractFailure);
    }

    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    winexinfo::tests::HookTestCommand command{};
    if (winexinfo::tests::ParseHookTestCommandLine(arguments, &command).ok()) {
        if (command.mode == winexinfo::tests::HookTestMode::Target) {
            return winexinfo::tests::RunHookTargetMode();
        }
        return winexinfo::tests::RunHookControllerMode(command);
    }

    std::cerr << "INVALID_ARGUMENT: WinExInfoTests.exe --unit [--filter <prefix>]\n";
    return static_cast<int>(winexinfo::HostExitCode::InvalidCli);
}
