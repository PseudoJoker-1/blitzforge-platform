#include <string.h>

#include "../include/wotb_mod_api.h"

WOTBMOD_ENTRY {
    if (!host || !mod || !out_info) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = 0x00630000u;
    out_info->id = "test.bad-abi";
    return WOTBMOD_OK;
}
