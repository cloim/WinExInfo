#include "test_framework.h"

#include "host/com_apartment.h"

#include <Windows.h>
#include <objbase.h>

WXI_TEST(
    com_apartment_shell_sta_flags_exact,
    "com_apartment.shell_sta_flags_exact") {
    const DWORD exactFlags = static_cast<DWORD>(
        COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WXI_REQUIRE_EQ(winexinfo::kShellComApartmentFlags, exactFlags);
    WXI_REQUIRE(
        (winexinfo::kShellComApartmentFlags &
         static_cast<DWORD>(COINIT_APARTMENTTHREADED)) != 0);
    WXI_REQUIRE(
        (winexinfo::kShellComApartmentFlags &
         static_cast<DWORD>(COINIT_DISABLE_OLE1DDE)) != 0);
    WXI_REQUIRE(
        winexinfo::kShellComApartmentFlags !=
        static_cast<DWORD>(COINIT_MULTITHREADED));
}
