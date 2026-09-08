#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../include/wotb_mod_api.h"

namespace {

const WotbModHostApi* g_host = nullptr;
WotbModHandle g_mod = nullptr;
WotbModVehicleSkinHandle g_skin = nullptr;
volatile LONG g_registered = 0;
volatile LONG g_failures = 0;

static void Log(
    WotbModLogLevel level,
    const char* status,
    const char* detail) {
    if (!g_host || !g_mod || !g_host->log) return;
    char message[320] = {};
    _snprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        "object260-t3485:%s:%s",
        status ? status : "INFO",
        detail ? detail : "");
    g_host->log(g_mod, level, message);
}

static void Fail(const char* detail) {
    InterlockedIncrement(&g_failures);
    Log(WOTBMOD_LOG_ERROR, "FAIL", detail);
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    g_host = host;
    g_mod = mod;
    g_skin = nullptr;
    InterlockedExchange(&g_registered, 0);
    InterlockedExchange(&g_failures, 0);

    if (!WOTBMOD_HOST_HAS(host, vehicle_skin_register) ||
        !WOTBMOD_HOST_HAS(host, vehicle_skin_get_info) ||
        !WOTBMOD_HOST_HAS(host, vehicle_skin_release)) {
        Fail("host-surface");
        return;
    }

    const WotbModVehicleSkinAsset assets[] = {
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_MESH,
            "~res:/3d/Tanks/USSR/R110_Object_260.sc2",
            "~res:/3d/Tanks/USSR/T-34-85.sc2",
        },
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_MESH,
            "~res:/3d/Tanks/USSR/R110_Object_260.scg",
            "~res:/3d/Tanks/USSR/T-34-85.scg",
        },
    };

    WotbModVehicleSkinDescriptor descriptor = {};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.skin_id = "object260-as-t3485";
    descriptor.vehicle_name = "R110_Object_260";
    descriptor.assets = assets;
    descriptor.asset_count =
        static_cast<uint32_t>(sizeof(assets) / sizeof(assets[0]));
    descriptor.priority = 200;
    descriptor.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;

    const WotbModResult result =
        host->vehicle_skin_register(mod, &descriptor, &g_skin);
    if (result != WOTBMOD_OK || !g_skin) {
        char detail[96] = {};
        _snprintf_s(
            detail,
            sizeof(detail),
            _TRUNCATE,
            "vehicle_skin_register result=%d",
            static_cast<int>(result));
        Fail(detail);
        return;
    }

    WotbModVehicleSkinInfo info = {};
    info.struct_size = sizeof(info);
    const WotbModResult infoResult =
        host->vehicle_skin_get_info(mod, g_skin, &info);
    if (infoResult != WOTBMOD_OK ||
        info.enabled != 1 ||
        info.asset_count != descriptor.asset_count ||
        info.priority != descriptor.priority ||
        strcmp(info.skin_id, descriptor.skin_id) != 0 ||
        strcmp(info.vehicle_name, descriptor.vehicle_name) != 0) {
        host->vehicle_skin_release(mod, g_skin);
        g_skin = nullptr;
        Fail("vehicle_skin_get_info");
        return;
    }

    InterlockedExchange(&g_registered, 1);
    Log(
        WOTBMOD_LOG_INFO,
        "READY",
        "R110_Object_260.sc2/.scg -> T-34-85.sc2/.scg");
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    InterlockedExchange(&g_registered, 0);
    if (g_skin && host && host->vehicle_skin_release) {
        host->vehicle_skin_release(mod, g_skin);
    }
    g_skin = nullptr;
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi*,
    WotbModHandle) {
    g_host = nullptr;
    g_mod = nullptr;
    g_skin = nullptr;
}

} /* namespace */

extern "C" __declspec(dllexport) int32_t WOTBMOD_CALL
WotbObject260T3485_IsRegistered(void) {
    return static_cast<int32_t>(
        InterlockedCompareExchange(&g_registered, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t WOTBMOD_CALL
WotbObject260T3485_GetFailures(void) {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_failures, 0, 0));
}

extern "C" __declspec(dllexport) WotbModResult WOTBMOD_CALL
WotbModLoad(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModInfo* outInfo) {
    if (!host || !mod || !outInfo) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    outInfo->struct_size = sizeof(*outInfo);
    outInfo->abi_version = WOTBMOD_ABI_VERSION_2_9;
    outInfo->id = "object260-t3485-model";
    outInfo->name = "Object 260 as T-34-85";
    outInfo->version = "1.0.0";
    outInfo->author = "BlitzForge SDK";
    outInfo->description =
        "Client-side visual replacement of Object 260 model with stock T-34-85";
    outInfo->flags = 0;
    outInfo->on_enable = &OnEnable;
    outInfo->on_disable = &OnDisable;
    outInfo->on_unload = &OnUnload;
    outInfo->on_frame = nullptr;
    return WOTBMOD_OK;
}
