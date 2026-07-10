#pragma once

#include "common/status.h"

#include <string>
#include <string_view>

namespace winexinfo {

[[nodiscard]] Status Utf8FromUtf16(std::wstring_view input, std::string* output);
[[nodiscard]] Status Utf16FromUtf8(std::string_view input, std::wstring* output);

}  // namespace winexinfo
