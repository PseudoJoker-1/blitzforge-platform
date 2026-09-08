#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include "../include/wotb_mod_api.h"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA(
            "WOTBMOD_TEST_CRASH_IN_DLLMAIN",
            value,
            static_cast<DWORD>(sizeof(value)));
        if (length == 1u && value[0] == '1') {
            TerminateProcess(GetCurrentProcess(), 73u);
        }
    }
    return TRUE;
}

WOTBMOD_ENTRY {
    if (!host || !mod || !out_info) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.safe-mode-crash";
    out_info->name = "Safe Mode Crash Fixture";
    out_info->version = "1.0.0";
    out_info->author = "Runtime tests";
    out_info->description =
        "Terminates only when the safe-mode crash test env flag is set.";
    return WOTBMOD_OK;
}
