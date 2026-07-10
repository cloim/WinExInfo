#include "test_framework.h"

#include <Windows.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

WXI_TEST(
    host_manifest_declares_windows_10_11_support,
    "host_manifest.declares_windows_10_11_support") {
    std::array<wchar_t, 32768> executablePath{};
    const DWORD pathLength = GetModuleFileNameW(
        nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    WXI_REQUIRE(pathLength > 0 && pathLength < executablePath.size());

    std::wstring hostPath{executablePath.data(), pathLength};
    const std::size_t separator = hostPath.find_last_of(L"\\/");
    WXI_REQUIRE(separator != std::wstring::npos);
    hostPath.replace(separator + 1, std::wstring::npos, L"WinExInfoHost.exe");

    HMODULE rawModule = LoadLibraryExW(
        hostPath.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    WXI_REQUIRE(rawModule != nullptr);
    const std::unique_ptr<std::remove_pointer_t<HMODULE>, decltype(&FreeLibrary)> module{
        rawModule, &FreeLibrary};

    const HRSRC resource = FindResourceW(module.get(), MAKEINTRESOURCEW(1), RT_MANIFEST);
    WXI_REQUIRE(resource != nullptr);
    const DWORD manifestSize = SizeofResource(module.get(), resource);
    WXI_REQUIRE(manifestSize > 0);
    const HGLOBAL loaded = LoadResource(module.get(), resource);
    WXI_REQUIRE(loaded != nullptr);
    const void* manifestBytes = LockResource(loaded);
    WXI_REQUIRE(manifestBytes != nullptr);

    const std::string_view manifest{
        static_cast<const char*>(manifestBytes), static_cast<std::size_t>(manifestSize)};
    const std::size_t firstMatch =
        manifest.find("supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"");
    WXI_REQUIRE(firstMatch != std::string_view::npos);
    WXI_REQUIRE(
        manifest.find(
            "supportedOS Id=\"{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}\"",
            firstMatch + 1) ==
        std::string_view::npos);
}
