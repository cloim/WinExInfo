#pragma once

#include <string_view>

namespace winexinfo::tests {

using TestFunction = void (*)();

class TestRegistrar final {
public:
    TestRegistrar(std::string_view name, TestFunction function);
};

void Require(bool condition, std::string_view expression, std::string_view file, int line);
int RunUnitTests(std::string_view prefix);

}  // namespace winexinfo::tests

#define WXI_TEST(symbol, name)                                                        \
    static void symbol();                                                             \
    static const ::winexinfo::tests::TestRegistrar symbol##_registrar{name, &symbol}; \
    static void symbol()

#define WXI_REQUIRE(expression)                                                        \
    ::winexinfo::tests::Require(                                                       \
        static_cast<bool>(expression), #expression, __FILE__, __LINE__)

#define WXI_REQUIRE_EQ(actual, expected) WXI_REQUIRE((actual) == (expected))
