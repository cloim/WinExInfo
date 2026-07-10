#include "test_framework.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace winexinfo::tests {
namespace {

struct TestCase final {
    std::string name;
    TestFunction function;
};

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

}  // namespace

TestRegistrar::TestRegistrar(const std::string_view name, const TestFunction function) {
    Registry().push_back(TestCase{std::string{name}, function});
}

void Require(
    const bool condition,
    const std::string_view expression,
    const std::string_view file,
    const int line) {
    if (condition) {
        return;
    }

    throw std::runtime_error{
        std::string{file} + ":" + std::to_string(line) + ": " + std::string{expression}};
}

int RunUnitTests(const std::string_view prefix) {
    std::vector<TestCase> selected;
    for (const TestCase& test : Registry()) {
        if (test.name.starts_with(prefix)) {
            selected.push_back(test);
        }
    }

    std::ranges::sort(selected, {}, &TestCase::name);

    int passed = 0;
    int failed = 0;
    for (const TestCase& test : selected) {
        try {
            test.function();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cout << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << "[SUMMARY] passed=" << passed << " failed=" << failed << '\n';
    return failed == 0 ? 0 : 1;
}

}  // namespace winexinfo::tests
