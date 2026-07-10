#include "test_framework.h"

#include <iostream>
#include <string_view>

int main(const int argc, const char* const argv[]) {
    if (argc == 2 && std::string_view{argv[1]} == "--unit") {
        return winexinfo::tests::RunUnitTests({});
    }

    if (argc == 4 && std::string_view{argv[1]} == "--unit" &&
        std::string_view{argv[2]} == "--filter") {
        return winexinfo::tests::RunUnitTests(argv[3]);
    }

    std::cerr << "INVALID_ARGUMENT: WinExInfoTests.exe --unit [--filter <prefix>]\n";
    return 2;
}
