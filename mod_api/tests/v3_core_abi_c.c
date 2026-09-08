#include "wotb_mod_api_v3.h"
#include "wotb_mod_runtime_v3.h"

#include <stddef.h>
#include <stdint.h>

typedef char WotbModV3AbiCheckHandle[
    sizeof(WotbModV3Handle) == sizeof(uint64_t) ? 1 : -1];
typedef char WotbModV3AbiCheckHeader[
    sizeof(WotbModV3StructHeader) == 8u ? 1 : -1];
typedef char WotbModV3AbiCheckVec3[
    sizeof(WotbModV3Vec3) == 12u ? 1 : -1];
typedef char WotbModV3AbiCheckResult[
    WOTBMOD_V3_E_TIMEOUT == 23 ? 1 : -1];
typedef char WotbModV3AbiCheckBootstrapOrder[
    offsetof(WotbModV3Bootstrap, query_interface) >
            offsetof(WotbModV3Bootstrap, bootstrap_version)
        ? 1
        : -1];
typedef char WotbModV3AbiCheckEntryCallbacks[
    offsetof(WotbModV3Info, on_frame) >
            offsetof(WotbModV3Info, on_enable)
        ? 1
        : -1];

static void WOTBMOD_V3_CALL V3COnLifecycle(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    (void)bootstrap;
    (void)mod;
}

static void WOTBMOD_V3_CALL V3COnFrame(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    uint64_t frame_index,
    double delta_seconds) {
    (void)bootstrap;
    (void)mod;
    (void)frame_index;
    (void)delta_seconds;
}

WotbModV3Result WOTBMOD_V3_CALL WotbModV3CHeaderProbe(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    WotbModV3Info* info) {
    const void* table = NULL;
    WotbModV3Result result;

    if (bootstrap == NULL || info == NULL) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    info->struct_size = (uint32_t)sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    info->on_enable = V3COnLifecycle;
    info->on_disable = V3COnLifecycle;
    info->on_unload = V3COnLifecycle;
    info->on_frame = V3COnFrame;

    result = bootstrap->query_interface(
        mod,
        WOTBMOD_V3_IFACE_CORE,
        WOTBMOD_V3_CORE_VERSION,
        &table);
    if (result == WOTBMOD_V3_OK && table != NULL) {
        const WotbModV3CoreApiV1* core =
            (const WotbModV3CoreApiV1*)table;
        uint64_t frame_index = 0u;
        return core->get_frame_index(mod, &frame_index);
    }
    return result;
}
