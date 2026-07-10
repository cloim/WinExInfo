#include "test_framework.h"

#include "host/command_line.h"

#include <iostream>
#include <string_view>

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

    std::cerr << "INVALID_ARGUMENT: WinExInfoTests.exe --unit [--filter <prefix>]\n";
    return static_cast<int>(winexinfo::HostExitCode::InvalidCli);
}
