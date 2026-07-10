#pragma once

#include <Windows.h>

#include <cstdint>
#include <string_view>

namespace winexinfo {

inline constexpr std::uint32_t kTargetOsMajor = 10;
inline constexpr std::uint32_t kTargetOsMinor = 0;
inline constexpr std::uint32_t kTargetOsBuild = 26200;
inline constexpr std::uint32_t kTargetOsUbr = 8655;
inline constexpr std::uint16_t kTargetMachine = IMAGE_FILE_MACHINE_AMD64;

inline constexpr std::wstring_view kTargetExplorerVersion = L"10.0.26100.8457";
inline constexpr std::wstring_view kTargetExplorerFrameVersion = L"10.0.26100.8457";
inline constexpr std::wstring_view kTargetShell32Version = L"10.0.26100.8521";

}  // namespace winexinfo
