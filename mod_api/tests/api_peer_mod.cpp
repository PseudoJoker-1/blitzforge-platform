#include <string.h>

#include "../include/wotb_mod_api.h"

static WotbModHandle g_mod = nullptr;

WOTBMOD_EXPORT WotbModHandle WOTBMOD_CALL
WotbTestPeer_GetHandle(void) {
    return g_mod;
}

WOTBMOD_ENTRY {
    if (!host || !mod || !out_info) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    g_mod = mod;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.peer";
    out_info->name = "API Ownership Peer";
    out_info->version = "1.2.0";
    out_info->author = "SDK tests";
    return WOTBMOD_OK;
}
