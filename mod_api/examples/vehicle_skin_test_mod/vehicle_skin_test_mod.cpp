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
WotbModResourceMountId g_mount = 0;
WotbModVehicleSkinHandle g_controlSkin = nullptr;
WotbModVehicleSkinHandle g_liveSkin = nullptr;
volatile LONG g_passes = 0;
volatile LONG g_failures = 0;
volatile LONG g_finished = 0;

static void Log(
    WotbModLogLevel level,
    const char* status,
    const char* test) {
    if (!g_host || !g_mod) return;
    char message[256] = {};
    _snprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        "vehicle-skin-test:%s:%s",
        status ? status : "INFO",
        test ? test : "");
    g_host->log(g_mod, level, message);
}

static void Pass(const char* test) {
    InterlockedIncrement(&g_passes);
    Log(WOTBMOD_LOG_INFO, "PASS", test);
}

static void Fail(const char* test) {
    InterlockedIncrement(&g_failures);
    Log(WOTBMOD_LOG_ERROR, "FAIL", test);
}

static bool ExpectResult(
    const char* test,
    WotbModResult actual,
    WotbModResult expected = WOTBMOD_OK) {
    if (actual == expected) {
        Pass(test);
        return true;
    }
    char detail[192] = {};
    _snprintf_s(
        detail,
        sizeof(detail),
        _TRUNCATE,
        "%s:actual=%d:expected=%d",
        test,
        static_cast<int>(actual),
        static_cast<int>(expected));
    Fail(detail);
    return false;
}

static bool CheckInfo(
    const char* test,
    WotbModVehicleSkinHandle skin,
    const char* expectedId,
    uint32_t expectedEnabled,
    uint32_t expectedAssets,
    int32_t expectedPriority) {
    WotbModVehicleSkinInfo info = {};
    info.struct_size = sizeof(info);
    const WotbModResult result =
        g_host->vehicle_skin_get_info(g_mod, skin, &info);
    if (result == WOTBMOD_OK &&
        info.enabled == expectedEnabled &&
        info.asset_count == expectedAssets &&
        info.priority == expectedPriority &&
        strcmp(info.skin_id, expectedId) == 0 &&
        strcmp(info.vehicle_name, "T-34-85") == 0) {
        Pass(test);
        return true;
    }
    Fail(test);
    return false;
}

static void VerifyMountedFile(
    const char* test,
    const char* virtualPath) {
    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t size = sizeof(resolved);
    if (g_host->resource_resolve(
            g_mod,
            virtualPath,
            resolved,
            &size) == WOTBMOD_OK &&
        GetFileAttributesA(resolved) != INVALID_FILE_ATTRIBUTES) {
        Pass(test);
    } else {
        Fail(test);
    }
}

static void FinishTest() {
    InterlockedExchange(&g_finished, 1);
    char summary[160] = {};
    _snprintf_s(
        summary,
        sizeof(summary),
        _TRUNCATE,
        "passes=%ld failures=%ld live-skin=%s",
        InterlockedCompareExchange(&g_passes, 0, 0),
        InterlockedCompareExchange(&g_failures, 0, 0),
        g_liveSkin ? "enabled" : "missing");
    Log(
        InterlockedCompareExchange(&g_failures, 0, 0) == 0
            ? WOTBMOD_LOG_INFO
            : WOTBMOD_LOG_ERROR,
        InterlockedCompareExchange(&g_failures, 0, 0) == 0
            ? "SUMMARY"
            : "SUMMARY-FAILED",
        summary);
}

static void RegisterAndExerciseSkins() {
    const WotbModVehicleSkinAsset controlAssets[] = {
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_MESH,
            "~res:/3d/Tanks/USSR/T-34-85.sc2",
            "~res:/Mods/test.vehicle-skin-new/low/T-34-85.sc2",
        },
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_MATERIAL,
            "~res:/3d/Tanks/USSR/T-34-85.material.yaml",
            "~res:/Mods/test.vehicle-skin-new/low/T-34-85.material.yaml",
        },
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_TEXTURE,
            "~res:/3d/Tanks/USSR/images/T-34-85.dx11.dds",
            "~res:/Mods/test.vehicle-skin-new/low/T-34-85.dx11.dds",
        },
    };
    WotbModVehicleSkinDescriptor control = {};
    control.struct_size = sizeof(control);
    control.skin_id = "t34-control-copy";
    control.vehicle_name = "T-34-85";
    control.assets = controlAssets;
    control.asset_count =
        static_cast<uint32_t>(
            sizeof(controlAssets) / sizeof(controlAssets[0]));
    control.priority = 10;
    control.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;
    if (!ExpectResult(
            "register-control",
            g_host->vehicle_skin_register(
                g_mod,
                &control,
                &g_controlSkin))) {
        return;
    }
    CheckInfo(
        "info-control",
        g_controlSkin,
        "t34-control-copy",
        1,
        control.asset_count,
        10);

    const WotbModVehicleSkinAsset liveAssets[] = {
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_MESH,
            "~res:/3d/Tanks/USSR/T-34-85.sc2",
            "~res:/Mods/test.vehicle-skin-new/high/T-34-85.sc2",
        },
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_MATERIAL,
            "~res:/3d/Tanks/USSR/T-34-85.material.yaml",
            "~res:/Mods/test.vehicle-skin-new/high/T-34-85.material.yaml",
        },
        {
            sizeof(WotbModVehicleSkinAsset),
            WOTBMOD_SKIN_ASSET_TEXTURE,
            "~res:/3d/Tanks/USSR/images/T-34-85.dx11.dds",
            "~res:/Mods/test.vehicle-skin-new/high/T-34-85.dx11.dds",
        },
    };
    WotbModVehicleSkinDescriptor live = {};
    live.struct_size = sizeof(live);
    live.skin_id = "t34-visible-test";
    live.vehicle_name = "T-34-85";
    live.assets = liveAssets;
    live.asset_count =
        static_cast<uint32_t>(
            sizeof(liveAssets) / sizeof(liveAssets[0]));
    live.priority = 100;
    live.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;
    if (!ExpectResult(
            "register-live",
            g_host->vehicle_skin_register(
                g_mod,
                &live,
                &g_liveSkin))) {
        return;
    }
    CheckInfo(
        "info-live",
        g_liveSkin,
        "t34-visible-test",
        1,
        live.asset_count,
        100);

    if (ExpectResult(
            "disable-live",
            g_host->vehicle_skin_set_enabled(
                g_mod,
                g_liveSkin,
                0))) {
        CheckInfo(
            "info-live-disabled",
            g_liveSkin,
            "t34-visible-test",
            0,
            live.asset_count,
            100);
    }
    if (ExpectResult(
            "enable-live",
            g_host->vehicle_skin_set_enabled(
                g_mod,
                g_liveSkin,
                1))) {
        CheckInfo(
            "info-live-enabled",
            g_liveSkin,
            "t34-visible-test",
            1,
            live.asset_count,
            100);
    }

}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    g_host = host;
    g_mod = mod;
    g_mount = 0;
    g_controlSkin = nullptr;
    g_liveSkin = nullptr;
    InterlockedExchange(&g_passes, 0);
    InterlockedExchange(&g_failures, 0);
    InterlockedExchange(&g_finished, 0);

    Log(WOTBMOD_LOG_INFO, "BEGIN", "ABI-2.9:T-34-85");
    if (!WOTBMOD_HOST_HAS(host, resource_mount) ||
        !WOTBMOD_HOST_HAS(host, resource_resolve) ||
        !WOTBMOD_HOST_HAS(host, vehicle_skin_register) ||
        !WOTBMOD_HOST_HAS(host, vehicle_skin_set_enabled) ||
        !WOTBMOD_HOST_HAS(host, vehicle_skin_get_info) ||
        !WOTBMOD_HOST_HAS(host, vehicle_skin_release)) {
        Fail("host-surface");
        return;
    }
    Pass("host-surface");

    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Mods/test.vehicle-skin-new/";
    mount.source_directory = "skin";
    mount.priority = 100;
    mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;
    if (!ExpectResult(
            "resource-mount",
            host->resource_mount(
                mod,
                &mount,
                &g_mount))) {
        return;
    }

    VerifyMountedFile(
        "resolve-high-mesh",
        "~res:/Mods/test.vehicle-skin-new/high/T-34-85.sc2");
    VerifyMountedFile(
        "resolve-high-material",
        "~res:/Mods/test.vehicle-skin-new/high/T-34-85.material.yaml");
    VerifyMountedFile(
        "resolve-high-texture",
        "~res:/Mods/test.vehicle-skin-new/high/T-34-85.dx11.dds");

    RegisterAndExerciseSkins();
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi*,
    WotbModHandle) {
    g_mount = 0;
    g_controlSkin = nullptr;
    g_liveSkin = nullptr;
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi*,
    WotbModHandle) {
}

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModFrameInfo*) {
    if (InterlockedCompareExchange(&g_finished, 0, 0) != 0) {
        return;
    }
    if (!g_controlSkin) {
        Fail("control-skin-missing-before-release");
        FinishTest();
        return;
    }

    WotbModVehicleSkinHandle stale = g_controlSkin;
    if (ExpectResult(
            "release-control",
            g_host->vehicle_skin_release(
                g_mod,
                g_controlSkin))) {
        g_controlSkin = nullptr;
        WotbModVehicleSkinInfo staleInfo = {};
        staleInfo.struct_size = sizeof(staleInfo);
        ExpectResult(
            "control-stale",
            g_host->vehicle_skin_get_info(
                g_mod,
                stale,
                &staleInfo),
            WOTBMOD_ERROR_NOT_FOUND);
    }
    FinishTest();
}

} /* namespace */

extern "C" __declspec(dllexport) uint32_t WOTBMOD_CALL
WotbVehicleSkinTest_GetPasses(void) {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_passes, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t WOTBMOD_CALL
WotbVehicleSkinTest_GetFailures(void) {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_failures, 0, 0));
}

extern "C" __declspec(dllexport) int32_t WOTBMOD_CALL
WotbVehicleSkinTest_IsFinished(void) {
    return static_cast<int32_t>(
        InterlockedCompareExchange(&g_finished, 0, 0));
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
    outInfo->id = "test.vehicle-skin-new";
    outInfo->name = "Vehicle Skin API Test";
    outInfo->version = "1.0.0";
    outInfo->author = "BlitzForge SDK";
    outInfo->description =
        "Exercises ABI 2.9 skin mount/register/info/toggle/release and redirects T-34-85 assets";
    outInfo->flags = 0;
    outInfo->on_enable = &OnEnable;
    outInfo->on_disable = &OnDisable;
    outInfo->on_unload = &OnUnload;
    outInfo->on_frame = &OnFrame;
    return WOTBMOD_OK;
}
