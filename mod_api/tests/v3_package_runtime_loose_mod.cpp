#include <string.h>

#include "../include/wotb_mod_api_v3.h"

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    (void)mod;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    strcpy_s(
        out_info->id,
        sizeof(out_info->id),
        "tests.package-loose");
    strcpy_s(
        out_info->name,
        sizeof(out_info->name),
        "Package Runtime Loose Test");
    strcpy_s(
        out_info->version,
        sizeof(out_info->version),
        "1.0.0");
    strcpy_s(
        out_info->author,
        sizeof(out_info->author),
        "tests");
    return WOTBMOD_V3_OK;
}
