#include "test_framework.h"

#include "common/contracts.h"
#include "common/status.h"
#include "common/utf8.h"
#include "common/win32_handle.h"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

WXI_TEST(contracts_target_versions, "contracts.target_versions") {
    WXI_REQUIRE_EQ(winexinfo::kTargetOsMajor, 10U);
    WXI_REQUIRE_EQ(winexinfo::kTargetOsMinor, 0U);
    WXI_REQUIRE_EQ(winexinfo::kTargetOsBuild, 26200U);
    WXI_REQUIRE_EQ(winexinfo::kTargetOsUbr, 8655U);
    WXI_REQUIRE_EQ(winexinfo::kTargetMachine, IMAGE_FILE_MACHINE_AMD64);
    WXI_REQUIRE_EQ(sizeof(void*), 8U);
    WXI_REQUIRE_EQ(winexinfo::kTargetExplorerVersion, std::wstring_view{L"10.0.26100.8655"});
    WXI_REQUIRE_EQ(
        winexinfo::kTargetExplorerFrameVersion,
        std::wstring_view{L"10.0.26100.8655"});
    WXI_REQUIRE_EQ(winexinfo::kTargetShell32Version, std::wstring_view{L"10.0.26100.8655"});
}

WXI_TEST(contracts_error_names, "contracts.error_names") {
    using winexinfo::ErrorCode;

    constexpr std::array expected{
        std::pair{ErrorCode::OK, std::string_view{"OK"}},
        std::pair{ErrorCode::INVALID_ARGUMENT, std::string_view{"INVALID_ARGUMENT"}},
        std::pair{ErrorCode::UNSUPPORTED_OS_BUILD, std::string_view{"UNSUPPORTED_OS_BUILD"}},
        std::pair{
            ErrorCode::UNSUPPORTED_EXPLORER_BUILD,
            std::string_view{"UNSUPPORTED_EXPLORER_BUILD"}},
        std::pair{
            ErrorCode::TARGET_VALIDATION_FAILED,
            std::string_view{"TARGET_VALIDATION_FAILED"}},
        std::pair{
            ErrorCode::TARGET_MITIGATION_BLOCKED,
            std::string_view{"TARGET_MITIGATION_BLOCKED"}},
        std::pair{ErrorCode::HOOK_INSTALL_FAILED, std::string_view{"HOOK_INSTALL_FAILED"}},
        std::pair{ErrorCode::HOOK_TRIGGER_FAILED, std::string_view{"HOOK_TRIGGER_FAILED"}},
        std::pair{ErrorCode::HOOK_RELEASE_FAILED, std::string_view{"HOOK_RELEASE_FAILED"}},
        std::pair{
            ErrorCode::DLL_INITIALIZATION_FAILED,
            std::string_view{"DLL_INITIALIZATION_FAILED"}},
        std::pair{ErrorCode::WINDOW_ATTACH_FAILED, std::string_view{"WINDOW_ATTACH_FAILED"}},
        std::pair{ErrorCode::DLL_UNLOAD_TIMEOUT, std::string_view{"DLL_UNLOAD_TIMEOUT"}},
        std::pair{
            ErrorCode::EXPLORER_UI_CONTRACT_MISMATCH,
            std::string_view{"EXPLORER_UI_CONTRACT_MISMATCH"}},
        std::pair{
            ErrorCode::ACTIVE_VIEW_CONTRACT_MISMATCH,
            std::string_view{"ACTIVE_VIEW_CONTRACT_MISMATCH"}},
        std::pair{ErrorCode::IPC_PROTOCOL_ERROR, std::string_view{"IPC_PROTOCOL_ERROR"}},
        std::pair{ErrorCode::PIPE_DISCONNECTED, std::string_view{"PIPE_DISCONNECTED"}},
    };

    for (const auto& [code, name] : expected) {
        WXI_REQUIRE_EQ(winexinfo::ToString(code), name);
    }

    const winexinfo::Status ok{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    const winexinfo::Status failed{ErrorCode::INVALID_ARGUMENT, E_INVALIDARG, ERROR_INVALID_PARAMETER};
    WXI_REQUIRE(ok.ok());
    WXI_REQUIRE(!failed.ok());
}

WXI_TEST(contracts_handle_move, "contracts.handle_move") {
    static_assert(!std::is_copy_constructible_v<winexinfo::UniqueHandle>);
    static_assert(!std::is_copy_assignable_v<winexinfo::UniqueHandle>);
    static_assert(std::is_move_constructible_v<winexinfo::UniqueHandle>);
    static_assert(std::is_move_assignable_v<winexinfo::UniqueHandle>);

    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WXI_REQUIRE(event != nullptr);

    winexinfo::UniqueHandle first{event};
    WXI_REQUIRE(static_cast<bool>(first));
    WXI_REQUIRE_EQ(first.get(), event);

    winexinfo::UniqueHandle second{std::move(first)};
    WXI_REQUIRE(!first);
    WXI_REQUIRE_EQ(second.get(), event);

    const HANDLE released = second.release();
    WXI_REQUIRE(!second);
    WXI_REQUIRE_EQ(released, event);

    first.reset(released);
    WXI_REQUIRE(static_cast<bool>(first));
    first.reset();
    WXI_REQUIRE(!first);

    DWORD flags = 0;
    WXI_REQUIRE(!GetHandleInformation(event, &flags));
    WXI_REQUIRE_EQ(GetLastError(), ERROR_INVALID_HANDLE);

    const HANDLE transferredEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WXI_REQUIRE(transferredEvent != nullptr);
    const HANDLE replacedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    WXI_REQUIRE(replacedEvent != nullptr);

    winexinfo::UniqueHandle source{transferredEvent};
    winexinfo::UniqueHandle destination{replacedEvent};
    destination = std::move(source);

    WXI_REQUIRE(!source);
    WXI_REQUIRE_EQ(destination.get(), transferredEvent);
    WXI_REQUIRE(GetHandleInformation(transferredEvent, &flags));
    WXI_REQUIRE(!GetHandleInformation(replacedEvent, &flags));
    WXI_REQUIRE_EQ(GetLastError(), ERROR_INVALID_HANDLE);

    destination.reset();
    WXI_REQUIRE(!destination);
    WXI_REQUIRE(!GetHandleInformation(transferredEvent, &flags));
    WXI_REQUIRE_EQ(GetLastError(), ERROR_INVALID_HANDLE);
}

WXI_TEST(contracts_utf8_roundtrip, "contracts.utf8_roundtrip") {
    const std::wstring original = L"WinExInfo Git 상태 \u2713";
    std::string utf8;
    WXI_REQUIRE(winexinfo::Utf8FromUtf16(original, &utf8).ok());
    WXI_REQUIRE(!utf8.empty());

    std::wstring restored;
    WXI_REQUIRE(winexinfo::Utf16FromUtf8(utf8, &restored).ok());
    WXI_REQUIRE_EQ(restored, original);
}

WXI_TEST(contracts_utf8_rejects_invalid, "contracts.utf8_rejects_invalid") {
    const std::wstring unchangedUtf16 = L"unchanged";
    std::wstring utf16Output = unchangedUtf16;
    const winexinfo::Status invalidUtf8 =
        winexinfo::Utf16FromUtf8(std::string_view{"\xC3\x28", 2}, &utf16Output);
    WXI_REQUIRE_EQ(invalidUtf8.code, winexinfo::ErrorCode::INVALID_ARGUMENT);
    WXI_REQUIRE_EQ(utf16Output, unchangedUtf16);
    WXI_REQUIRE_EQ(utf16Output.find(L'\uFFFD'), std::wstring::npos);

    const std::string unchangedUtf8 = "unchanged";
    std::string utf8Output = unchangedUtf8;
    const wchar_t loneHighSurrogate[] = {static_cast<wchar_t>(0xD800), L'\0'};
    const winexinfo::Status invalidUtf16 =
        winexinfo::Utf8FromUtf16(std::wstring_view{loneHighSurrogate, 1}, &utf8Output);
    WXI_REQUIRE_EQ(invalidUtf16.code, winexinfo::ErrorCode::INVALID_ARGUMENT);
    WXI_REQUIRE_EQ(utf8Output, unchangedUtf8);
    WXI_REQUIRE_EQ(utf8Output.find(std::string_view{"\xEF\xBF\xBD", 3}), std::string::npos);
}

}  // namespace
