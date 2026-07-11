#include <Windows.h>

HMODULE g_hook_module = nullptr;

BOOL WINAPI DllMain(
    const HMODULE module,
    const DWORD reason,
    void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hook_module = module;
    }
    return TRUE;
}
