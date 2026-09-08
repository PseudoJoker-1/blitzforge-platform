#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string.h>

#include "../include/wotb_mod_api.h"

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModFrameInfo*) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
}

WOTBMOD_ENTRY {
    (void)mod;
    if (!host || !out_info) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.fault";
    out_info->name = "Fault Isolation Test";
    out_info->version = "1.2.0";
    out_info->author = "SDK tests";
    out_info->on_frame = &OnFrame;
    return WOTBMOD_OK;
}
