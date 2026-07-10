#pragma once

#include <Windows.h>
#include <objbase.h>

namespace winexinfo {

inline constexpr DWORD kShellComApartmentFlags = static_cast<DWORD>(
    COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

}  // namespace winexinfo
