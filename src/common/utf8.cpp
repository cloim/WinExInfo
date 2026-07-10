#include "common/utf8.h"

#include <Windows.h>

#include <limits>
#include <string>
#include <utility>

namespace winexinfo {
namespace {

Status InvalidArgumentStatus(const DWORD win32) noexcept {
    return Status{ErrorCode::INVALID_ARGUMENT, HRESULT_FROM_WIN32(win32), win32};
}

}  // namespace

Status Utf8FromUtf16(const std::wstring_view input, std::string* const output) {
    if (output == nullptr || input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return InvalidArgumentStatus(ERROR_INVALID_PARAMETER);
    }

    if (input.empty()) {
        output->clear();
        return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    const int inputLength = static_cast<int>(input.size());
    const int outputLength = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        inputLength,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (outputLength == 0) {
        return InvalidArgumentStatus(GetLastError());
    }

    std::string converted(static_cast<std::size_t>(outputLength), '\0');
    const int convertedLength = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        inputLength,
        converted.data(),
        outputLength,
        nullptr,
        nullptr);
    if (convertedLength == 0) {
        return InvalidArgumentStatus(GetLastError());
    }

    *output = std::move(converted);
    return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

Status Utf16FromUtf8(const std::string_view input, std::wstring* const output) {
    if (output == nullptr || input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return InvalidArgumentStatus(ERROR_INVALID_PARAMETER);
    }

    if (input.empty()) {
        output->clear();
        return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
    }

    const int inputLength = static_cast<int>(input.size());
    const int outputLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        inputLength,
        nullptr,
        0);
    if (outputLength == 0) {
        return InvalidArgumentStatus(GetLastError());
    }

    std::wstring converted(static_cast<std::size_t>(outputLength), L'\0');
    const int convertedLength = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        inputLength,
        converted.data(),
        outputLength);
    if (convertedLength == 0) {
        return InvalidArgumentStatus(GetLastError());
    }

    *output = std::move(converted);
    return Status{ErrorCode::OK, S_OK, ERROR_SUCCESS};
}

}  // namespace winexinfo
