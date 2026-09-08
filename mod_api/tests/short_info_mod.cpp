#include <string.h>

#include "../include/wotb_mod_api.h"

WOTBMOD_ENTRY {
    if (!host || !mod || !out_info) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size =
        (uint32_t)offsetof(WotbModInfo, on_frame);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.short-info";
    return WOTBMOD_OK;
}
