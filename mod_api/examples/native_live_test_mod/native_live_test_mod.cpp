#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../include/wotb_mod_api.h"

namespace {

const WotbModHostApi* g_host = nullptr;
WotbModHandle g_mod = nullptr;
volatile LONG g_nativeHookHits = 0;
volatile LONG g_uiHookHits = 0;
volatile LONG g_sceneHookHits = 0;
volatile LONG g_localHookHits = 0;
void* g_nativeOriginal = nullptr;
void* g_uiOriginal = nullptr;
void* g_sceneOriginal = nullptr;
typedef int(__cdecl* LocalHookFn)(int);
LocalHookFn g_localOriginal = nullptr;
uint32_t g_passes = 0;
uint32_t g_failures = 0;
uint32_t g_skips = 0;
uint32_t g_soundStage = 0;
uint64_t g_stageFrame = 0;
uint32_t g_createAttempts = 0;
bool g_summaryWritten = false;
WotbModSoundEventHandle g_clickEvent = nullptr;
const char* g_selectedEventName = nullptr;
WotbModResourceMountId g_fixtureMount = 0;
WotbModVehicleSkinHandle g_testSkin = nullptr;
WotbModResourceHandle g_uiParent = nullptr;
WotbModResourceHandle g_uiChild = nullptr;
WotbModResourceHandle g_sceneParent = nullptr;
WotbModResourceHandle g_sceneChild = nullptr;
WotbModResourceHandle g_activeSceneModel = nullptr;
WotbModResourceHandle g_factoryActiveScene = nullptr;
WotbModResourceHandle g_factoryEntity = nullptr;
bool g_factoryModelAttached = false;
bool g_factoryEntityAttached = false;
volatile LONG g_mainThreadProbeHits = 0;
volatile LONG g_uiScreenEventCount = 0;
volatile LONG g_sceneActivatedEventCount = 0;
volatile LONG g_sceneDeactivatedEventCount = 0;
volatile LONG g_eventTypeCounts[22] = {};
bool g_nativeObjectsTested = false;
bool g_factoryObjectsTested = false;
bool g_requiredEventsReported = false;
bool g_eventThreadChecked = false;
bool g_eventSequenceChecked = false;
bool g_uiBorrowedChecked = false;
bool g_uiCloneChecked = false;
bool g_sceneCloneChecked = false;
bool g_staleEventHandleChecked = false;
bool g_vehicleBorrowedChecked = false;
bool g_staleVehicleHandleChecked = false;
uint32_t g_factoryAttempts = 0;
uint32_t g_factoryStage = 0;
uint32_t g_factoryStableFrames = 0;
uint64_t g_factoryAttachedFrame = 0;
ULONGLONG g_factoryWaitStarted = 0;
DWORD g_enableThreadId = 0;
uint64_t g_lastEventSequence = 0;
WotbModEventSubscriptionId g_clientEventSubscription = 0;
WotbModResourceHandle g_eventUiClone = nullptr;
WotbModResourceHandle g_eventSceneClone = nullptr;
WotbModResourceHandle g_staleEventHandle = nullptr;
WotbModVehicleHandle g_staleVehicleHandle = nullptr;

constexpr ULONGLONG kActiveSceneWaitMilliseconds = 10ull * 60ull * 1000ull;
constexpr uint32_t kActiveSceneHoldFrames = 180;

static void LogResult(const char* status, const char* test) {
    char line[256] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "live-api:%s:%s",
        status,
        test);
    g_host->log(
        g_mod,
        strcmp(status, "FAIL") == 0
            ? WOTBMOD_LOG_ERROR
            : WOTBMOD_LOG_INFO,
        line);
}

static void Pass(const char* test) {
    ++g_passes;
    LogResult("PASS", test);
}

static void Fail(const char* test) {
    ++g_failures;
    LogResult("FAIL", test);
}

static void Skip(const char* test) {
    ++g_skips;
    LogResult("SKIP", test);
}

static bool ExpectResult(
    const char* test,
    WotbModResult actual,
    WotbModResult expected = WOTBMOD_OK) {
    if (actual == expected) {
        Pass(test);
        return true;
    }
    char name[192] = {};
    _snprintf_s(
        name,
        sizeof(name),
        _TRUNCATE,
        "%s:actual=%d:expected=%d",
        test,
        static_cast<int>(actual),
        static_cast<int>(expected));
    Fail(name);
    return false;
}

extern "C" void __cdecl MarkNativeHook() {
    InterlockedIncrement(&g_nativeHookHits);
}

extern "C" void __cdecl MarkUiHook() {
    InterlockedIncrement(&g_uiHookHits);
}

extern "C" void __cdecl MarkSceneHook() {
    InterlockedIncrement(&g_sceneHookHits);
}

__declspec(naked) void NativeHookDetour() {
    __asm {
        pushfd
        pushad
        call MarkNativeHook
        popad
        popfd
        jmp dword ptr [g_nativeOriginal]
    }
}

__declspec(naked) void UiHookDetour() {
    __asm {
        pushfd
        pushad
        call MarkUiHook
        popad
        popfd
        jmp dword ptr [g_uiOriginal]
    }
}

__declspec(naked) void SceneHookDetour() {
    __asm {
        pushfd
        pushad
        call MarkSceneHook
        popad
        popfd
        jmp dword ptr [g_sceneOriginal]
    }
}

static bool CreateAndEnableHook(
    const char* name,
    uint32_t rva,
    void* detour,
    void** original) {
    void* target = g_host->resolve_rva(rva);
    if (!target) {
        Fail(name);
        return false;
    }
    char createName[160] = {};
    _snprintf_s(
        createName,
        sizeof(createName),
        _TRUNCATE,
        "%s:create",
        name);
    if (!ExpectResult(
            createName,
            g_host->hook_create(
                g_mod,
                target,
                detour,
                original))) {
        return false;
    }
    char enableName[160] = {};
    _snprintf_s(
        enableName,
        sizeof(enableName),
        _TRUNCATE,
        "%s:enable",
        name);
    return ExpectResult(
        enableName,
        g_host->hook_enable(g_mod, target));
}

__declspec(noinline) int __cdecl LocalHookTarget(int value) {
    volatile int result = value;
    result += 3;
    result ^= 0x55;
    result ^= 0x55;
    result += 4;
    return result;
}

static int __cdecl LocalHookDetour(int value) {
    InterlockedIncrement(&g_localHookHits);
    return g_localOriginal ? g_localOriginal(value) + 1 : -1;
}

static void TestFullHookLifecycle() {
    void* target = reinterpret_cast<void*>(&LocalHookTarget);
    if (!ExpectResult(
            "hook:local:create",
            g_host->hook_create(
                g_mod,
                target,
                reinterpret_cast<void*>(&LocalHookDetour),
                reinterpret_cast<void**>(&g_localOriginal)))) {
        return;
    }
    if (!ExpectResult(
            "hook:local:enable",
            g_host->hook_enable(g_mod, target))) {
        return;
    }

    volatile LocalHookFn callTarget = &LocalHookTarget;
    const LONG hitsBefore = InterlockedCompareExchange(
        &g_localHookHits, 0, 0);
    const int hookedResult = callTarget(10);
    const LONG hitsAfter = InterlockedCompareExchange(
        &g_localHookHits, 0, 0);
    if (hookedResult == 18 && hitsAfter == hitsBefore + 1) {
        Pass("hook:local:detour-hit");
    } else {
        Fail("hook:local:detour-hit");
    }

    if (ExpectResult(
            "hook:local:disable",
            g_host->hook_disable(g_mod, target))) {
        const LONG disabledHits = InterlockedCompareExchange(
            &g_localHookHits, 0, 0);
        if (callTarget(10) == 17 &&
            InterlockedCompareExchange(
                &g_localHookHits, 0, 0) == disabledHits) {
            Pass("hook:local:disable-effective");
        } else {
            Fail("hook:local:disable-effective");
        }
    }
    if (ExpectResult(
            "hook:local:reenable",
            g_host->hook_enable(g_mod, target))) {
        const LONG reenableHits = InterlockedCompareExchange(
            &g_localHookHits, 0, 0);
        if (callTarget(10) == 18 &&
            InterlockedCompareExchange(
                &g_localHookHits, 0, 0) == reenableHits + 1) {
            Pass("hook:local:reenable-effective");
        } else {
            Fail("hook:local:reenable-effective");
        }
    }
    ExpectResult(
        "hook:local:disable-before-remove",
        g_host->hook_disable(g_mod, target));
    if (ExpectResult(
            "hook:local:remove",
            g_host->hook_remove(g_mod, target))) {
        if (callTarget(10) == 17) {
            Pass("hook:local:remove-effective");
        } else {
            Fail("hook:local:remove-effective");
        }
    }
}

static void TestHostSurface() {
    const size_t requiredSize =
        offsetof(WotbModHostApi, vehicle_skin_release) +
        sizeof(g_host->vehicle_skin_release);
    if (g_host->struct_size >= requiredSize &&
        g_host->abi_version == WOTBMOD_ABI_VERSION_2_9 &&
        g_host->log &&
        g_host->get_path &&
        g_host->get_game_module &&
        g_host->resolve_rva &&
        g_host->get_proc_address &&
        g_host->find_pattern &&
        g_host->hook_create &&
        g_host->hook_enable &&
        g_host->hook_disable &&
        g_host->hook_remove &&
        g_host->config_get_int &&
        g_host->config_set_int &&
        g_host->config_get_string &&
        g_host->config_set_string &&
        g_host->get_mod_count &&
        g_host->get_mod_info &&
        g_host->set_mod_enabled &&
        g_host->resource_mount &&
        g_host->resource_unmount &&
        g_host->resource_resolve &&
        g_host->resource_load &&
        g_host->resource_reload &&
        g_host->resource_release &&
        g_host->audio_play &&
        g_host->audio_pause &&
        g_host->audio_resume &&
        g_host->audio_stop &&
        g_host->audio_set_parameters &&
        g_host->audio_get_state &&
        g_host->audio_release &&
        g_host->audio_clip_load &&
        g_host->sound_event_create &&
        g_host->sound_event_trigger &&
        g_host->sound_event_stop &&
        g_host->sound_event_set_paused &&
        g_host->sound_event_set_volume &&
        g_host->sound_event_set_position &&
        g_host->sound_event_get_state &&
        g_host->sound_event_set_parameter &&
        g_host->sound_event_get_parameter &&
        g_host->sound_event_has_parameter &&
        g_host->sound_event_get_name &&
        g_host->sound_event_release &&
        g_host->main_thread_enqueue &&
        g_host->ui_control_set_geometry &&
        g_host->ui_control_set_visible &&
        g_host->ui_control_add_child &&
        g_host->ui_control_remove_child &&
        g_host->scene_set_transform &&
        g_host->scene_add_child &&
        g_host->scene_remove_child &&
        g_host->ui_control_create &&
        g_host->ui_get_active_screen &&
        g_host->scene_entity_create &&
        g_host->scene_get_active &&
        g_host->resource_clone &&
        g_host->event_subscribe &&
        g_host->event_unsubscribe &&
        g_host->ui_control_find_by_name &&
        g_host->ui_control_get_parent &&
        g_host->ui_control_get_child_count &&
        g_host->ui_control_get_child_at &&
        g_host->ui_control_get_state &&
        g_host->ui_control_set_input_enabled &&
        g_host->ui_control_set_disabled &&
        g_host->vehicle_get_local &&
        g_host->vehicle_get_by_entity_id &&
        g_host->vehicle_get_count &&
        g_host->vehicle_get_at &&
        g_host->vehicle_clone &&
        g_host->vehicle_release &&
        g_host->vehicle_get_info &&
        g_host->vehicle_skin_register &&
        g_host->vehicle_skin_set_enabled &&
        g_host->vehicle_skin_get_info &&
        g_host->vehicle_skin_release) {
        Pass("host-surface:76-functions");
    } else {
        Fail("host-surface:76-functions");
    }

    for (int pathType = WOTBMOD_PATH_GAME;
         pathType <= WOTBMOD_PATH_CONFIG;
         ++pathType) {
        char path[WOTBMOD_MAX_PATH] = {};
        uint32_t size = sizeof(path);
        char test[80] = {};
        _snprintf_s(
            test,
            sizeof(test),
            _TRUNCATE,
            "get_path:%d",
            pathType);
        if (g_host->get_path(
                g_mod,
                static_cast<WotbModPath>(pathType),
                path,
                &size) == WOTBMOD_OK &&
            path[0]) {
            Pass(test);
        } else {
            Fail(test);
        }
    }

    void* gameModule = g_host->get_game_module();
    if (gameModule) {
        Pass("get_game_module");
        if (g_host->resolve_rva(0) == gameModule) {
            Pass("resolve_rva");
        } else {
            Fail("resolve_rva");
        }
        if (g_host->get_proc_address(
                "kernel32.dll",
                "GetTickCount")) {
            Pass("get_proc_address");
        } else {
            Fail("get_proc_address");
        }
        const uint8_t mz[2] = {'M', 'Z'};
        if (g_host->find_pattern(nullptr, mz, "xx") == gameModule) {
            Pass("find_pattern");
        } else {
            Fail("find_pattern");
        }
    } else {
        Skip("get_game_module:legacy-raw-api-disabled");
        Skip("resolve_rva:legacy-raw-api-disabled");
        Skip("get_proc_address:legacy-raw-api-disabled");
        Skip("find_pattern:legacy-raw-api-disabled");
    }

    ExpectResult(
        "config_set_int",
        g_host->config_set_int(g_mod, "live", "number", 42));
    if (g_host->config_get_int(g_mod, "live", "number", 0) == 42) {
        Pass("config_get_int");
    } else {
        Fail("config_get_int");
    }
    ExpectResult(
        "config_set_string",
        g_host->config_set_string(g_mod, "live", "text", "ok"));
    char configText[32] = {};
    if (g_host->config_get_string(
            g_mod,
            "live",
            "text",
            "",
            configText,
            sizeof(configText)) == WOTBMOD_OK &&
        strcmp(configText, "ok") == 0) {
        Pass("config_get_string");
    } else {
        Fail("config_get_string");
    }

    const uint32_t modCount = g_host->get_mod_count();
    if (modCount > 0) Pass("get_mod_count");
    else Fail("get_mod_count");
    WotbModPublicInfo info = {};
    info.struct_size = sizeof(info);
    if (modCount > 0 &&
        g_host->get_mod_info(0, &info) == WOTBMOD_OK &&
        info.id[0]) {
        Pass("get_mod_info");
    } else {
        Fail("get_mod_info");
    }
    ExpectResult(
        "set_mod_enabled:not-found",
        g_host->set_mod_enabled("test.does-not-exist", 1),
        WOTBMOD_ERROR_NOT_FOUND);
}

static void TestResources() {
    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Mods/test.native-live/";
    mount.source_directory = "fixtures";
    mount.priority = 100;
    mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;
    g_fixtureMount = 0;
    if (!ExpectResult(
            "resource_mount",
            g_host->resource_mount(
                g_mod, &mount, &g_fixtureMount))) {
        return;
    }

    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t resolvedSize = sizeof(resolved);
    if (g_host->resource_resolve(
            g_mod,
            "~res:/Mods/test.native-live/probe.yaml",
            resolved,
            &resolvedSize) == WOTBMOD_OK &&
        GetFileAttributesA(resolved) != INVALID_FILE_ATTRIBUTES) {
        Pass("resource_resolve");
    } else {
        Fail("resource_resolve");
    }

    struct ResourceCase {
        WotbModResourceType type;
        const char* path;
        const char* objectName;
        const char* name;
    };
    const ResourceCase cases[] = {
        {WOTBMOD_RESOURCE_GENERIC,
         "~res:/Mods/test.native-live/probe.bin",
         nullptr,
         "generic"},
        {WOTBMOD_RESOURCE_YAML_DOCUMENT,
         "~res:/Mods/test.native-live/probe.yaml",
         nullptr,
         "yaml"},
        {WOTBMOD_RESOURCE_UI_PACKAGE,
         "~res:/Mods/test.native-live/probe.yaml",
         nullptr,
         "ui-package"},
        {WOTBMOD_RESOURCE_UI_CONTROL,
         "~res:/Mods/test.native-live/probe.yaml",
         "ProbeControl",
         "ui-control"},
        {WOTBMOD_RESOURCE_SCENE,
         "~res:/Mods/test.native-live/probe.sc2",
         nullptr,
         "scene"},
        {WOTBMOD_RESOURCE_TEXTURE,
         "~res:/Mods/test.native-live/probe.tex",
         nullptr,
         "texture"},
    };

    for (size_t index = 0;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        WotbModResourceLoadRequest request = {};
        request.struct_size = sizeof(request);
        request.type = cases[index].type;
        request.virtual_path = cases[index].path;
        request.object_name = cases[index].objectName;
        WotbModResourceHandle resource = nullptr;
        char test[128] = {};
        _snprintf_s(
            test,
            sizeof(test),
            _TRUNCATE,
            "resource_load:%s",
            cases[index].name);
        if (!ExpectResult(
                test,
                g_host->resource_load(g_mod, &request, &resource))) {
            continue;
        }
        _snprintf_s(
            test,
            sizeof(test),
            _TRUNCATE,
            "resource_reload:%s",
            cases[index].name);
        ExpectResult(
            test,
            g_host->resource_reload(g_mod, resource));
        _snprintf_s(
            test,
            sizeof(test),
            _TRUNCATE,
            "resource_release:%s",
            cases[index].name);
        ExpectResult(
            test,
            g_host->resource_release(g_mod, resource));
    }

    struct NativeObjectCase {
        WotbModResourceType type;
        const char* path;
        const char* objectName;
        const char* test;
        WotbModResourceHandle* output;
    };
    NativeObjectCase objectCases[] = {
        {WOTBMOD_RESOURCE_UI_CONTROL,
         "~res:/Mods/test.native-live/probe.yaml",
         "ProbeControl",
         "resource_load:ui-parent",
         &g_uiParent},
        {WOTBMOD_RESOURCE_UI_CONTROL,
         "~res:/Mods/test.native-live/probe.yaml",
         "ProbeControl",
         "resource_load:ui-child",
         &g_uiChild},
        {WOTBMOD_RESOURCE_SCENE,
         "~res:/Mods/test.native-live/probe.sc2",
         nullptr,
         "resource_load:scene-parent",
         &g_sceneParent},
        {WOTBMOD_RESOURCE_SCENE,
         "~res:/Mods/test.native-live/probe.sc2",
         nullptr,
         "resource_load:scene-child",
         &g_sceneChild},
        {WOTBMOD_RESOURCE_SCENE,
         "~res:/Mods/test.native-live/probe.sc2",
         nullptr,
         "resource_load:active-scene-model",
         &g_activeSceneModel},
    };
    for (size_t index = 0;
         index < sizeof(objectCases) / sizeof(objectCases[0]);
         ++index) {
        WotbModResourceLoadRequest request = {};
        request.struct_size = sizeof(request);
        request.type = objectCases[index].type;
        request.virtual_path = objectCases[index].path;
        request.object_name = objectCases[index].objectName;
        ExpectResult(
            objectCases[index].test,
            g_host->resource_load(
                g_mod,
                &request,
                objectCases[index].output));
    }
}

static void TestVehicleSkins() {
    if (!g_fixtureMount) {
        Skip("vehicle-skin:no-fixture-mount");
        return;
    }
    WotbModVehicleSkinAsset asset = {};
    asset.struct_size = sizeof(asset);
    asset.kind = WOTBMOD_SKIN_ASSET_MESH;
    asset.stock_virtual_path =
        "~res:/3d/Tanks/BlitzForgeLiveTest/hull.sc2";
    asset.replacement_virtual_path =
        "~res:/Mods/test.native-live/probe.sc2";

    WotbModVehicleSkinDescriptor descriptor = {};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.skin_id = "native-live-probe";
    descriptor.vehicle_name = "BlitzForgeLiveTest";
    descriptor.assets = &asset;
    descriptor.asset_count = 1;
    descriptor.priority = 100;
    descriptor.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;
    if (!ExpectResult(
            "vehicle_skin_register",
            g_host->vehicle_skin_register(
                g_mod, &descriptor, &g_testSkin))) {
        return;
    }

    WotbModVehicleSkinInfo info = {};
    info.struct_size = sizeof(info);
    const WotbModResult infoResult =
        g_host->vehicle_skin_get_info(
            g_mod, g_testSkin, &info);
    if (infoResult == WOTBMOD_OK &&
        info.enabled == 1 &&
        info.asset_count == 1 &&
        info.priority == 100 &&
        strcmp(info.skin_id, "native-live-probe") == 0 &&
        strcmp(
            info.vehicle_name,
            "BlitzForgeLiveTest") == 0) {
        Pass("vehicle_skin_get_info");
    } else {
        Fail("vehicle_skin_get_info");
    }
    ExpectResult(
        "vehicle_skin_set_enabled:disable",
        g_host->vehicle_skin_set_enabled(
            g_mod, g_testSkin, 0));
    ExpectResult(
        "vehicle_skin_set_enabled:enable",
        g_host->vehicle_skin_set_enabled(
            g_mod, g_testSkin, 1));
}

static void ReleaseResource(
    const char* test,
    WotbModResourceHandle* resource) {
    if (!resource || !*resource) return;
    ExpectResult(
        test,
        g_host->resource_release(g_mod, *resource));
    *resource = nullptr;
}

static void ReleaseTestSkin(bool report) {
    if (!g_testSkin) return;
    if (report) {
        ExpectResult(
            "vehicle_skin_release",
            g_host->vehicle_skin_release(
                g_mod, g_testSkin));
    } else {
        g_host->vehicle_skin_release(g_mod, g_testSkin);
    }
    g_testSkin = nullptr;
}

static void CleanupNativeObjectFixtures() {
    ReleaseResource("resource_release:ui-child", &g_uiChild);
    ReleaseResource("resource_release:ui-parent", &g_uiParent);
    ReleaseResource("resource_release:scene-child", &g_sceneChild);
    ReleaseResource("resource_release:scene-parent", &g_sceneParent);
    if (g_fixtureMount && !g_activeSceneModel) {
        ExpectResult(
            "resource_unmount",
            g_host->resource_unmount(g_mod, g_fixtureMount));
        g_fixtureMount = 0;
    }
}

static void TestNativeObjects() {
    if (g_nativeObjectsTested) return;
    g_nativeObjectsTested = true;
    if (!g_uiParent || !g_uiChild ||
        !g_sceneParent || !g_sceneChild) {
        Fail("native-objects:fixtures");
        CleanupNativeObjectFixtures();
        return;
    }

    WotbModUiControlGeometry geometry = {};
    geometry.struct_size = sizeof(geometry);
    geometry.x = 24.0f;
    geometry.y = 32.0f;
    geometry.width = 256.0f;
    geometry.height = 128.0f;
    ExpectResult(
        "ui_control_set_geometry",
        g_host->ui_control_set_geometry(
            g_mod, g_uiParent, &geometry));
    ExpectResult(
        "ui_control_set_visible:false",
        g_host->ui_control_set_visible(
            g_mod, g_uiParent, 0));
    ExpectResult(
        "ui_control_set_visible:true",
        g_host->ui_control_set_visible(
            g_mod, g_uiParent, 1));
    ExpectResult(
        "ui_control_add_child",
        g_host->ui_control_add_child(
            g_mod, g_uiParent, g_uiChild));
    ExpectResult(
        "ui_control_remove_child",
        g_host->ui_control_remove_child(
            g_mod, g_uiParent, g_uiChild));

    WotbModSceneTransform transform = {};
    transform.struct_size = sizeof(transform);
    transform.position_x = 1.0f;
    transform.position_y = 2.0f;
    transform.position_z = 3.0f;
    transform.rotation_w = 1.0f;
    transform.scale_x = 1.0f;
    transform.scale_y = 1.0f;
    transform.scale_z = 1.0f;
    ExpectResult(
        "scene_set_transform",
        g_host->scene_set_transform(
            g_mod, g_sceneParent, &transform));
    ExpectResult(
        "scene_add_child",
        g_host->scene_add_child(
            g_mod, g_sceneParent, g_sceneChild));
    ExpectResult(
        "scene_remove_child",
        g_host->scene_remove_child(
            g_mod, g_sceneParent, g_sceneChild));

    CleanupNativeObjectFixtures();
}

static void CleanupFactoryObjects(bool report) {
    if (g_factoryActiveScene &&
        g_activeSceneModel &&
        g_factoryModelAttached) {
        const WotbModResult result = g_host->scene_remove_child(
            g_mod, g_factoryActiveScene, g_activeSceneModel);
        if (report) {
            ExpectResult("custom-sc2:detach-active-scene", result);
        }
    }
    g_factoryModelAttached = false;
    if (g_factoryActiveScene &&
        g_factoryEntity &&
        g_factoryEntityAttached) {
        const WotbModResult result = g_host->scene_remove_child(
            g_mod, g_factoryActiveScene, g_factoryEntity);
        if (report) {
            ExpectResult("scene_entity_create:detach-active-scene", result);
        }
    }
    g_factoryEntityAttached = false;
    ReleaseTestSkin(report);
    if (report) {
        ReleaseResource(
            "resource_release:created-entity", &g_factoryEntity);
        ReleaseResource(
            "resource_release:active-scene-model", &g_activeSceneModel);
        ReleaseResource(
            "resource_release:active-scene", &g_factoryActiveScene);
        if (g_fixtureMount) {
            ExpectResult(
                "resource_unmount",
                g_host->resource_unmount(g_mod, g_fixtureMount));
            g_fixtureMount = 0;
        }
        return;
    }
    if (g_factoryEntity) {
        g_host->resource_release(g_mod, g_factoryEntity);
        g_factoryEntity = nullptr;
    }
    if (g_activeSceneModel) {
        g_host->resource_release(g_mod, g_activeSceneModel);
        g_activeSceneModel = nullptr;
    }
    if (g_factoryActiveScene) {
        g_host->resource_release(g_mod, g_factoryActiveScene);
        g_factoryActiveScene = nullptr;
    }
    if (g_fixtureMount) {
        g_host->resource_unmount(g_mod, g_fixtureMount);
        g_fixtureMount = 0;
    }
}

static void TestObjectFactories(const WotbModFrameInfo* frame) {
    if (g_factoryObjectsTested || !frame) return;

    if (g_factoryStage == 1) {
        WotbModResourceHandle currentScene = nullptr;
        const WotbModResult currentResult =
            g_host->scene_get_active(g_mod, &currentScene);
        if (currentResult == WOTBMOD_OK && currentScene) {
            ++g_factoryStableFrames;
            g_host->resource_release(g_mod, currentScene);
        } else {
            ExpectResult(
                "custom-sc2:active-scene-remained-available",
                currentResult);
            if (currentScene) {
                g_host->resource_release(g_mod, currentScene);
            }
        }
        if (frame->frame_index <
            g_factoryAttachedFrame + kActiveSceneHoldFrames) {
            return;
        }
        if (g_factoryStableFrames >= kActiveSceneHoldFrames) {
            Pass("custom-sc2:attached-for-180-frames");
        } else {
            Fail("custom-sc2:attached-for-180-frames");
        }
        CleanupFactoryObjects(true);
        g_factoryObjectsTested = true;
        g_factoryStage = 2;
        return;
    }

    if (frame->frame_index % 10 != 0) return;
    ++g_factoryAttempts;
    if (!g_factoryWaitStarted) {
        g_factoryWaitStarted = GetTickCount64();
    }

    WotbModResourceHandle activeScreen = nullptr;
    WotbModResourceHandle activeScene = nullptr;
    const WotbModResult screenResult =
        g_host->ui_get_active_screen(g_mod, &activeScreen);
    const WotbModResult sceneResult =
        g_host->scene_get_active(g_mod, &activeScene);
    if (screenResult == WOTBMOD_ERROR_NOT_FOUND ||
        sceneResult == WOTBMOD_ERROR_NOT_FOUND) {
        if (activeScreen) {
            g_host->resource_release(g_mod, activeScreen);
        }
        if (activeScene) {
            g_host->resource_release(g_mod, activeScene);
        }
        if (GetTickCount64() - g_factoryWaitStarted <
            kActiveSceneWaitMilliseconds) {
            return;
        }
        Fail("scene_get_active:timeout-waiting-for-hangar-or-battle");
        CleanupFactoryObjects(true);
        g_factoryObjectsTested = true;
        g_factoryStage = 2;
        return;
    }

    const bool haveScreen = ExpectResult(
        "ui_get_active_screen", screenResult);
    const bool haveScene =
        sceneResult == WOTBMOD_OK && activeScene;
    if (haveScene) {
        Pass("scene_get_active");
    } else {
        ExpectResult("scene_get_active", sceneResult);
    }

    WotbModUiControlGeometry geometry = {};
    geometry.struct_size = sizeof(geometry);
    geometry.x = 8.0f;
    geometry.y = 8.0f;
    geometry.width = 32.0f;
    geometry.height = 32.0f;
    WotbModResourceHandle control = nullptr;
    const bool haveControl = ExpectResult(
        "ui_control_create",
        g_host->ui_control_create(
            g_mod, &geometry, &control));
    if (haveScreen && haveControl) {
        ExpectResult(
            "ui_control_create:set-invisible",
            g_host->ui_control_set_visible(
                g_mod, control, 0));
        ExpectResult(
            "ui_control_create:attach-active-screen",
            g_host->ui_control_add_child(
                g_mod, activeScreen, control));
        ExpectResult(
            "ui_control_create:detach-active-screen",
            g_host->ui_control_remove_child(
                g_mod, activeScreen, control));
        ExpectResult(
            "ui_control_create:reload-rejected",
            g_host->resource_reload(g_mod, control),
            WOTBMOD_ERROR_PLATFORM);
    }
    ReleaseResource("resource_release:created-ui", &control);
    ReleaseResource(
        "resource_release:active-screen", &activeScreen);

    const bool haveEntity = ExpectResult(
        "scene_entity_create",
        g_host->scene_entity_create(
            g_mod, &g_factoryEntity));
    WotbModSceneTransform transform = {};
    transform.struct_size = sizeof(transform);
    transform.position_x = 0.0f;
    transform.position_y = 0.0f;
    transform.position_z = 0.0f;
    transform.rotation_w = 1.0f;
    transform.scale_x = 1.0f;
    transform.scale_y = 1.0f;
    transform.scale_z = 1.0f;
    if (haveEntity) {
        ExpectResult(
            "scene_entity_create:set-transform",
            g_host->scene_set_transform(
                g_mod, g_factoryEntity, &transform));
        ExpectResult(
            "scene_entity_create:reload-rejected",
            g_host->resource_reload(
                g_mod, g_factoryEntity),
            WOTBMOD_ERROR_PLATFORM);
    }
    const bool haveModel = g_activeSceneModel != nullptr;
    if (haveModel) {
        ExpectResult(
            "custom-sc2:set-transform",
            g_host->scene_set_transform(
                g_mod, g_activeSceneModel, &transform));
    } else {
        Fail("custom-sc2:loaded-model-handle");
    }

    if (haveScene) {
        g_factoryActiveScene = activeScene;
    }
    if (haveScene && haveEntity) {
        g_factoryEntityAttached = ExpectResult(
            "scene_entity_create:attach-active-scene",
            g_host->scene_add_child(
                g_mod, activeScene, g_factoryEntity));
    }
    if (haveScene && haveModel) {
        g_factoryModelAttached = ExpectResult(
            "custom-sc2:attach-active-scene",
            g_host->scene_add_child(
                g_mod, activeScene, g_activeSceneModel));
    }
    if (haveScene &&
        g_factoryEntityAttached &&
        g_factoryModelAttached) {
        g_factoryAttachedFrame = frame->frame_index;
        g_factoryStableFrames = 0;
        g_factoryStage = 1;
        return;
    }

    if (activeScene && !g_factoryActiveScene) {
        g_host->resource_release(g_mod, activeScene);
    }
    CleanupFactoryObjects(true);
    g_factoryObjectsTested = true;
    g_factoryStage = 2;
}

static void WOTBMOD_CALL MainThreadProbe(
    const WotbModHostApi*,
    WotbModHandle,
    void*) {
    InterlockedIncrement(&g_mainThreadProbeHits);
    Pass("main_thread_enqueue:callback");
}

static void TestMainThreadQueue() {
    if (ExpectResult(
            "main_thread_enqueue:accepted",
            g_host->main_thread_enqueue(
                g_mod, &MainThreadProbe, nullptr))) {
        if (InterlockedCompareExchange(
                &g_mainThreadProbeHits, 0, 0) == 0) {
            Pass("main_thread_enqueue:deferred");
        } else {
            Fail("main_thread_enqueue:deferred");
        }
    }
}

static uint32_t EventBitIndex(uint32_t type) {
    uint32_t index = 0;
    while (index < 22 && (1u << index) != type) {
        ++index;
    }
    return index;
}

static const char* EventName(uint32_t type) {
    switch (type) {
    case WOTBMOD_EVENT_UI_SCREEN_CHANGED: return "ui-screen-changed";
    case WOTBMOD_EVENT_SCENE_ACTIVATED: return "scene-activated";
    case WOTBMOD_EVENT_SCENE_DEACTIVATED: return "scene-deactivated";
    case WOTBMOD_EVENT_UI_INPUT: return "ui-input";
    case WOTBMOD_EVENT_BATTLE_ENTERED: return "battle-entered";
    case WOTBMOD_EVENT_BATTLE_STARTED: return "battle-started";
    case WOTBMOD_EVENT_BATTLE_ENDED: return "battle-ended";
    case WOTBMOD_EVENT_BATTLE_LEFT: return "battle-left";
    case WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED: return "local-vehicle-changed";
    case WOTBMOD_EVENT_VEHICLE_SPAWNED: return "vehicle-spawned";
    case WOTBMOD_EVENT_VEHICLE_DESPAWNED: return "vehicle-despawned";
    case WOTBMOD_EVENT_SHOT_FIRED: return "shot-fired";
    case WOTBMOD_EVENT_SHELL_HIT: return "shell-hit";
    case WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED: return "vehicle-health-changed";
    case WOTBMOD_EVENT_VEHICLE_DAMAGED: return "vehicle-damaged";
    case WOTBMOD_EVENT_VEHICLE_DESTROYED: return "vehicle-destroyed";
    case WOTBMOD_EVENT_RELOAD_STATE_CHANGED: return "reload-state-changed";
    case WOTBMOD_EVENT_AMMO_CHANGED: return "ammo-changed";
    case WOTBMOD_EVENT_AIM_TARGET_CHANGED: return "aim-target-changed";
    case WOTBMOD_EVENT_VEHICLE_SPOTTED: return "vehicle-spotted";
    case WOTBMOD_EVENT_VEHICLE_UNSPOTTED: return "vehicle-unspotted";
    case WOTBMOD_EVENT_CAMERA_MODE_CHANGED: return "camera-mode-changed";
    default: return "unknown";
    }
}

static bool PayloadAtLeast(
    const WotbModClientEvent* event,
    uint32_t required) {
    return event && event->payload_size >= required;
}

static void ValidateBorrowedVehicle(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event) {
    if (!event->vehicle || g_vehicleBorrowedChecked) {
        return;
    }
    g_vehicleBorrowedChecked = true;

    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    if (host->vehicle_get_info(
            mod, event->vehicle, &info) == WOTBMOD_OK &&
        info.entity_id != 0) {
        Pass("event:vehicle-info");
    } else {
        Fail("event:vehicle-info");
    }
    ExpectResult(
        "event:vehicle-borrowed-release-denied",
        host->vehicle_release(mod, event->vehicle),
        WOTBMOD_ERROR_ACCESS_DENIED);

    WotbModVehicleHandle clone = nullptr;
    if (ExpectResult(
            "event:vehicle-clone",
            host->vehicle_clone(mod, event->vehicle, &clone))) {
        ExpectResult(
            "event:vehicle-clone-release",
            host->vehicle_release(mod, clone));
    }
    g_staleVehicleHandle = event->vehicle;
}

static void WOTBMOD_CALL OnClientEvent(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void*) {
    if (!host || mod != g_mod || !event ||
        event->struct_size < sizeof(WotbModClientEvent)) {
        Fail("event:callback-contract");
        return;
    }

    if (!g_eventThreadChecked) {
        g_eventThreadChecked = true;
        if (GetCurrentThreadId() == g_enableThreadId) {
            Pass("event:main-thread");
        } else {
            Fail("event:main-thread");
        }
    }

    if (event->sequence == 0 ||
        (g_lastEventSequence != 0 &&
         event->sequence <= g_lastEventSequence)) {
        Fail("event:sequence-monotonic");
    } else if (!g_eventSequenceChecked) {
        g_eventSequenceChecked = true;
        Pass("event:sequence-monotonic");
    }
    g_lastEventSequence = event->sequence;

    const uint32_t eventIndex = EventBitIndex(event->type);
    if (eventIndex >= 22) {
        Fail("event:unknown-type");
        return;
    }
    const bool firstEvent =
        InterlockedIncrement(&g_eventTypeCounts[eventIndex]) == 1;
    ValidateBorrowedVehicle(host, mod, event);

    switch (event->type) {
    case WOTBMOD_EVENT_UI_SCREEN_CHANGED:
        InterlockedIncrement(&g_uiScreenEventCount);
        if (!g_uiBorrowedChecked) {
            g_uiBorrowedChecked = true;
            if (!event->resource) {
                Fail("event:ui-screen-changed:payload");
                break;
            }
            Pass("event:ui-screen-changed:payload");
            ExpectResult(
                "event:borrowed-release-denied",
                host->resource_release(mod, event->resource),
                WOTBMOD_ERROR_ACCESS_DENIED);
            ExpectResult(
                "event:borrowed-reload-denied",
                host->resource_reload(mod, event->resource),
                WOTBMOD_ERROR_ACCESS_DENIED);
            if (ExpectResult(
                    "event:ui-resource-clone",
                    host->resource_clone(
                        mod,
                        event->resource,
                        &g_eventUiClone))) {
                g_staleEventHandle = event->resource;
            }
        }
        break;

    case WOTBMOD_EVENT_SCENE_ACTIVATED:
        InterlockedIncrement(&g_sceneActivatedEventCount);
        if (!g_sceneCloneChecked && !g_eventSceneClone) {
            if (!event->resource) {
                Fail("event:scene-activated:payload");
                break;
            }
            Pass("event:scene-activated:payload");
            ExpectResult(
                "event:scene-resource-clone",
                host->resource_clone(
                    mod,
                    event->resource,
                    &g_eventSceneClone));
        }
        break;

    case WOTBMOD_EVENT_SCENE_DEACTIVATED:
        InterlockedIncrement(&g_sceneDeactivatedEventCount);
        if (!event->previous_resource) {
            Fail("event:scene-deactivated:payload");
        }
        break;

    case WOTBMOD_EVENT_BATTLE_ENTERED:
    case WOTBMOD_EVENT_BATTLE_STARTED:
    case WOTBMOD_EVENT_BATTLE_ENDED:
    case WOTBMOD_EVENT_BATTLE_LEFT:
        if (!PayloadAtLeast(
                event,
                static_cast<uint32_t>(
                    sizeof(WotbModBattleEventData)))) {
            Fail("event:battle:payload");
            return;
        }
        break;

    case WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED:
    case WOTBMOD_EVENT_VEHICLE_SPAWNED:
    case WOTBMOD_EVENT_VEHICLE_DESPAWNED:
    case WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED:
        if (!PayloadAtLeast(
                event,
                static_cast<uint32_t>(
                    sizeof(WotbModVehicleEventData)))) {
            Fail("event:vehicle:payload");
            return;
        }
        break;

    case WOTBMOD_EVENT_SHOT_FIRED:
        if (!PayloadAtLeast(
                event,
                static_cast<uint32_t>(
                    sizeof(WotbModShotEventData)))) {
            Fail("event:shot:payload");
            return;
        }
        break;

    case WOTBMOD_EVENT_VEHICLE_DAMAGED:
        if (!PayloadAtLeast(
                event,
                static_cast<uint32_t>(
                    sizeof(WotbModDamageEventData)))) {
            Fail("event:damage:payload");
            return;
        }
        break;

    case WOTBMOD_EVENT_VEHICLE_DESTROYED:
        if (!PayloadAtLeast(
                event,
                static_cast<uint32_t>(
                    sizeof(WotbModVehicleEventData))) ||
            (event->payload.vehicle.flags &
             WOTBMOD_VEHICLE_DESTROYED) == 0) {
            Fail("event:destroyed:payload");
            return;
        }
        break;

    case WOTBMOD_EVENT_RELOAD_STATE_CHANGED:
        if (!PayloadAtLeast(
                event,
                static_cast<uint32_t>(
                    sizeof(WotbModReloadEventData)))) {
            Fail("event:reload:payload");
            return;
        }
        break;

    case WOTBMOD_EVENT_UI_INPUT:
    case WOTBMOD_EVENT_SHELL_HIT:
    case WOTBMOD_EVENT_AMMO_CHANGED:
    case WOTBMOD_EVENT_AIM_TARGET_CHANGED:
    case WOTBMOD_EVENT_VEHICLE_SPOTTED:
    case WOTBMOD_EVENT_VEHICLE_UNSPOTTED:
        break;

    case WOTBMOD_EVENT_CAMERA_MODE_CHANGED:
        if (!PayloadAtLeast(
                event,
                static_cast<uint32_t>(
                    sizeof(WotbModCameraEventData))) ||
            event->payload.camera.mode == 0u ||
            event->payload.camera.native_mode < 0) {
            Fail("event:camera-mode:payload");
            return;
        }
        break;

    default:
        Fail("event:unknown-type");
        return;
    }

    if (firstEvent) {
        char test[96] = {};
        _snprintf_s(
            test,
            sizeof(test),
            _TRUNCATE,
            "event:%s:first",
            EventName(event->type));
        Pass(test);
    }
}

static void TestClientEvents() {
    if (g_eventUiClone && !g_uiCloneChecked) {
        g_uiCloneChecked = true;
        ExpectResult(
            "event:ui-clone-survived-callback",
            g_host->resource_release(g_mod, g_eventUiClone));
        g_eventUiClone = nullptr;
    }
    if (g_eventSceneClone && !g_sceneCloneChecked) {
        g_sceneCloneChecked = true;
        ExpectResult(
            "event:scene-clone-survived-callback",
            g_host->resource_release(g_mod, g_eventSceneClone));
        g_eventSceneClone = nullptr;
    }
    if (g_staleEventHandle && !g_staleEventHandleChecked) {
        g_staleEventHandleChecked = true;
        WotbModResourceHandle clone = nullptr;
        ExpectResult(
            "event:borrowed-invalid-after-callback",
            g_host->resource_clone(
                g_mod,
                g_staleEventHandle,
                &clone),
            WOTBMOD_ERROR_INVALID_ARGUMENT);
        if (clone) {
            g_host->resource_release(g_mod, clone);
        }
        g_staleEventHandle = nullptr;
    }
    if (g_staleVehicleHandle && !g_staleVehicleHandleChecked) {
        g_staleVehicleHandleChecked = true;
        WotbModVehicleInfo info = {};
        info.struct_size = sizeof(info);
        ExpectResult(
            "event:vehicle-borrowed-invalid-after-callback",
            g_host->vehicle_get_info(
                g_mod, g_staleVehicleHandle, &info),
            WOTBMOD_ERROR_INVALID_ARGUMENT);
        g_staleVehicleHandle = nullptr;
    }

    if (!g_requiredEventsReported &&
        InterlockedCompareExchange(
            &g_uiScreenEventCount, 0, 0) > 0 &&
        InterlockedCompareExchange(
            &g_sceneActivatedEventCount, 0, 0) > 0 &&
        g_uiCloneChecked &&
        g_sceneCloneChecked &&
        g_staleEventHandleChecked) {
        g_requiredEventsReported = true;
        Pass("event:ui-screen-changed");
        Pass("event:scene-activated");
    }
}

static void TestHooks() {
    if (!g_host->get_game_module()) {
        Skip("hooks:legacy-raw-api-disabled");
        return;
    }
    TestFullHookLifecycle();
    CreateAndEnableHook(
        "hook:native-wwise-is-active",
        0x02101110u,
        &NativeHookDetour,
        &g_nativeOriginal);
    CreateAndEnableHook(
        "hook:dava-ui-load-package",
        0x00813990u,
        &UiHookDetour,
        &g_uiOriginal);
    CreateAndEnableHook(
        "hook:dava-scene-load-file",
        0x00909BC0u,
        &SceneHookDetour,
        &g_sceneOriginal);
}

static void WriteSummary() {
    if (g_summaryWritten ||
        !g_nativeObjectsTested ||
        !g_factoryObjectsTested ||
        !g_requiredEventsReported) {
        return;
    }
    g_summaryWritten = true;
    if (InterlockedCompareExchange(&g_nativeHookHits, 0, 0) > 0) {
        Pass("hook:native-wwise-is-active:hit");
    } else {
        Skip("hook:native-wwise-is-active:no-late-call");
    }
    if (InterlockedCompareExchange(&g_uiHookHits, 0, 0) > 0) {
        Pass("hook:dava-ui-load-package:hit");
    } else {
        Skip("hook:dava-ui-load-package:no-late-load");
    }
    if (InterlockedCompareExchange(&g_sceneHookHits, 0, 0) > 0) {
        Pass("hook:dava-scene-load-file:hit");
    } else {
        Skip("hook:dava-scene-load-file:no-late-load");
    }
    if (InterlockedCompareExchange(
            &g_sceneDeactivatedEventCount, 0, 0) > 0) {
        Pass("event:scene-deactivated");
    } else {
        Skip("event:scene-deactivated:no-transition");
    }

    char summary[256] = {};
    _snprintf_s(
        summary,
        sizeof(summary),
        _TRUNCATE,
        "live-api:SUMMARY passes=%u skips=%u failures=%u native_hits=%ld ui_hits=%ld scene_hits=%ld ui_events=%ld scene_activated=%ld scene_deactivated=%ld",
        g_passes,
        g_skips,
        g_failures,
        InterlockedCompareExchange(&g_nativeHookHits, 0, 0),
        InterlockedCompareExchange(&g_uiHookHits, 0, 0),
        InterlockedCompareExchange(&g_sceneHookHits, 0, 0),
        InterlockedCompareExchange(&g_uiScreenEventCount, 0, 0),
        InterlockedCompareExchange(&g_sceneActivatedEventCount, 0, 0),
        InterlockedCompareExchange(&g_sceneDeactivatedEventCount, 0, 0));
    g_host->log(
        g_mod,
        g_failures ? WOTBMOD_LOG_ERROR : WOTBMOD_LOG_INFO,
        summary);

    char dataPath[WOTBMOD_MAX_PATH] = {};
    uint32_t pathSize = sizeof(dataPath);
    if (g_host->get_path(
            g_mod,
            WOTBMOD_PATH_DATA,
            dataPath,
            &pathSize) == WOTBMOD_OK) {
        char donePath[WOTBMOD_MAX_PATH] = {};
        _snprintf_s(
            donePath,
            sizeof(donePath),
            _TRUNCATE,
            "%s\\live_test.done",
            dataPath);
        FILE* done = nullptr;
        fopen_s(&done, donePath, "wb");
        if (done) {
            fprintf(
                done,
                "passes=%u\nskips=%u\nfailures=%u\n",
                g_passes,
                g_skips,
                g_failures);
            fclose(done);
        }
    }
}

static void AdvanceSoundTest(const WotbModFrameInfo* frame) {
    if (g_soundStage == 0) {
        if (frame->frame_index < 10 ||
            frame->frame_index % 10 != 0) {
            return;
        }
        const char* eventCandidates[] = {
            "guns/tracers/tracer_hard",
            "sound_camera_switch",
            "play_ui_result_achiev_score_click",
            "music_hangar_event",
            "GUI_buttons_ok",
        };
        const size_t candidateCount =
            sizeof(eventCandidates) / sizeof(eventCandidates[0]);
        const size_t candidateIndex =
            g_createAttempts % candidateCount;
        const char* candidate = eventCandidates[candidateIndex];
        ++g_createAttempts;
        const WotbModResult result = g_host->sound_event_create(
            g_mod,
            candidate,
            &g_clickEvent);
        if (g_createAttempts <= candidateCount) {
            char probe[192] = {};
            _snprintf_s(
                probe,
                sizeof(probe),
                _TRUNCATE,
                "live-api:PROBE:sound_event_create:%s:result=%d",
                candidate,
                static_cast<int>(result));
            g_host->log(g_mod, WOTBMOD_LOG_INFO, probe);
        }
        if (result == WOTBMOD_ERROR_PLATFORM ||
            result == WOTBMOD_ERROR_NOT_FOUND) {
            if (g_createAttempts < 120) return;
            Fail("sound_event_create:timeout");
            g_soundStage = 100;
            WriteSummary();
            return;
        }
        if (!ExpectResult("sound_event_create", result)) {
            g_soundStage = 100;
            WriteSummary();
            return;
        }
        g_selectedEventName = candidate;
        char selected[192] = {};
        _snprintf_s(
            selected,
            sizeof(selected),
            _TRUNCATE,
            "sound_event_create:alias=%s",
            g_selectedEventName);
        Pass(selected);

        uint32_t nameSize = 0;
        if (g_host->sound_event_get_name(
                g_mod,
                g_clickEvent,
                nullptr,
                &nameSize) == WOTBMOD_ERROR_BUFFER_TOO_SMALL &&
            nameSize > 1) {
            Pass("sound_event_get_name:size");
        } else {
            Fail("sound_event_get_name:size");
        }
        char eventName[WOTBMOD_MAX_SOUND_EVENT_NAME] = {};
        nameSize = sizeof(eventName);
        if (g_host->sound_event_get_name(
                g_mod,
                g_clickEvent,
                eventName,
                &nameSize) == WOTBMOD_OK &&
            strcmp(eventName, g_selectedEventName) == 0) {
            Pass("sound_event_get_name:value");
        } else {
            Fail("sound_event_get_name:value");
        }
        ExpectResult(
            "sound_event_set_volume",
            g_host->sound_event_set_volume(
                g_mod,
                g_clickEvent,
                0.25f));
        ExpectResult(
            "sound_event_set_position",
            g_host->sound_event_set_position(
                g_mod,
                g_clickEvent,
                0.0f,
                0.0f,
                0.0f));
        WotbModAudioState state = WOTBMOD_AUDIO_PLAYING;
        if (g_host->sound_event_get_state(
                g_mod,
                g_clickEvent,
                &state) == WOTBMOD_OK &&
            state == WOTBMOD_AUDIO_STOPPED) {
            Pass("sound_event_get_state:initial");
        } else {
            Fail("sound_event_get_state:initial");
        }
        if (ExpectResult(
                "sound_event_trigger",
                g_host->sound_event_trigger(g_mod, g_clickEvent))) {
            g_stageFrame = frame->frame_index;
            g_soundStage = 1;
        }
        return;
    }

    if (g_soundStage == 1 &&
        frame->frame_index >= g_stageFrame + 3) {
        WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
        if (g_host->sound_event_get_state(
                g_mod,
                g_clickEvent,
                &state) == WOTBMOD_OK &&
            state != WOTBMOD_AUDIO_STOPPED) {
            Pass("sound_event_get_state:triggered");
        } else {
            Fail("sound_event_get_state:triggered");
        }
        ExpectResult(
            "sound_event_set_paused:true",
            g_host->sound_event_set_paused(g_mod, g_clickEvent, 1));
        state = WOTBMOD_AUDIO_STOPPED;
        if (g_host->sound_event_get_state(
                g_mod,
                g_clickEvent,
                &state) == WOTBMOD_OK &&
            state == WOTBMOD_AUDIO_PAUSED) {
            Pass("sound_event_get_state:paused");
        } else {
            Fail("sound_event_get_state:paused");
        }
        ExpectResult(
            "sound_event_set_paused:false",
            g_host->sound_event_set_paused(g_mod, g_clickEvent, 0));
        ExpectResult(
            "sound_event_stop",
            g_host->sound_event_stop(g_mod, g_clickEvent));
        state = WOTBMOD_AUDIO_PLAYING;
        if (g_host->sound_event_get_state(
                g_mod,
                g_clickEvent,
                &state) == WOTBMOD_OK &&
            state == WOTBMOD_AUDIO_STOPPED) {
            Pass("sound_event_get_state:stopped");
        } else {
            Fail("sound_event_get_state:stopped");
        }
        ExpectResult(
            "sound_event_release",
            g_host->sound_event_release(g_mod, g_clickEvent));
        g_clickEvent = nullptr;
        g_soundStage = 2;
        g_stageFrame = frame->frame_index;
        return;
    }

    if (g_soundStage == 2 &&
        frame->frame_index >= g_stageFrame + 1) {
        WotbModSoundEventHandle parameterEvent = nullptr;
        if (ExpectResult(
                "sound_event_create:parameter-event",
                g_host->sound_event_create(
                    g_mod,
                    g_selectedEventName,
                    &parameterEvent))) {
            const char* parameterCandidates[] = {
                "RTPC_int_azimuth",
                "distance",
                "speed",
                "fly",
                "hardness",
                "hangar",
                "length",
                "velocity",
            };
            const size_t parameterCount =
                sizeof(parameterCandidates) /
                sizeof(parameterCandidates[0]);
            const char* selectedParameter = nullptr;
            bool hasCallsOk = true;
            for (size_t index = 0; index < parameterCount; ++index) {
                int32_t hasParameter = 0;
                if (g_host->sound_event_has_parameter(
                        g_mod,
                        parameterEvent,
                        parameterCandidates[index],
                        &hasParameter) != WOTBMOD_OK) {
                    hasCallsOk = false;
                    break;
                }
                if (hasParameter) {
                    selectedParameter = parameterCandidates[index];
                    break;
                }
            }
            if (hasCallsOk) {
                Pass("sound_event_has_parameter");
            } else {
                Fail("sound_event_has_parameter");
            }
            if (selectedParameter) {
                char selected[160] = {};
                _snprintf_s(
                    selected,
                    sizeof(selected),
                    _TRUNCATE,
                    "sound_event_has_parameter:true:%s",
                    selectedParameter);
                Pass(selected);
                ExpectResult(
                    "sound_event_set_parameter",
                    g_host->sound_event_set_parameter(
                        g_mod,
                        parameterEvent,
                        selectedParameter,
                        0.5f));
                float value = 0.0f;
                const WotbModResult getParameterResult =
                    g_host->sound_event_get_parameter(
                        g_mod,
                        parameterEvent,
                        selectedParameter,
                        &value);
                if (getParameterResult == WOTBMOD_OK &&
                    _finite(value)) {
                    Pass("sound_event_get_parameter");
                    if (fabsf(value - 0.5f) >= 0.001f) {
                        char diagnostic[192] = {};
                        _snprintf_s(
                            diagnostic,
                            sizeof(diagnostic),
                            _TRUNCATE,
                            "live-api:PROBE:sound_event_get_parameter:%s:native-value=%.6f",
                            selectedParameter,
                            value);
                        g_host->log(
                            g_mod,
                            WOTBMOD_LOG_INFO,
                            diagnostic);
                    }
                } else {
                    char diagnostic[192] = {};
                    _snprintf_s(
                        diagnostic,
                        sizeof(diagnostic),
                        _TRUNCATE,
                        "live-api:PROBE:sound_event_get_parameter:%s:result=%d:value=%.6f",
                        selectedParameter,
                        static_cast<int>(getParameterResult),
                        value);
                    g_host->log(
                        g_mod,
                        WOTBMOD_LOG_WARNING,
                        diagnostic);
                    Fail("sound_event_get_parameter");
                }
            } else {
                Skip("sound_event_set/get_parameter:no-supported-parameter");
            }
            ExpectResult(
                "sound_event_release:parameter-event",
                g_host->sound_event_release(g_mod, parameterEvent));
        }
        g_soundStage = 3;
        WriteSummary();
    }
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    g_host = host;
    g_mod = mod;
    g_nativeObjectsTested = false;
    g_factoryObjectsTested = false;
    g_requiredEventsReported = false;
    g_eventThreadChecked = false;
    g_eventSequenceChecked = false;
    g_uiBorrowedChecked = false;
    g_uiCloneChecked = false;
    g_sceneCloneChecked = false;
    g_staleEventHandleChecked = false;
    g_vehicleBorrowedChecked = false;
    g_staleVehicleHandleChecked = false;
    g_factoryAttempts = 0;
    g_factoryStage = 0;
    g_factoryStableFrames = 0;
    g_factoryAttachedFrame = 0;
    g_factoryWaitStarted = 0;
    g_activeSceneModel = nullptr;
    g_factoryActiveScene = nullptr;
    g_factoryEntity = nullptr;
    g_factoryModelAttached = false;
    g_factoryEntityAttached = false;
    g_enableThreadId = GetCurrentThreadId();
    g_lastEventSequence = 0;
    g_clientEventSubscription = 0;
    g_eventUiClone = nullptr;
    g_eventSceneClone = nullptr;
    g_staleEventHandle = nullptr;
    g_staleVehicleHandle = nullptr;
    g_testSkin = nullptr;
    InterlockedExchange(&g_mainThreadProbeHits, 0);
    InterlockedExchange(&g_uiScreenEventCount, 0);
    InterlockedExchange(&g_sceneActivatedEventCount, 0);
    InterlockedExchange(&g_sceneDeactivatedEventCount, 0);
    for (uint32_t index = 0; index < 22; ++index) {
        InterlockedExchange(&g_eventTypeCounts[index], 0);
    }
    host->log(mod, WOTBMOD_LOG_INFO, "live-api:BEGIN ABI 2.9");
    TestHostSurface();
    ExpectResult(
        "event_subscribe:all",
        host->event_subscribe(
            mod,
            WOTBMOD_EVENT_ALL,
            &OnClientEvent,
            nullptr,
            &g_clientEventSubscription));
    TestHooks();
    TestResources();
    TestVehicleSkins();
    TestMainThreadQueue();
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    if (host && mod) {
        g_host = host;
        g_mod = mod;
        CleanupFactoryObjects(false);
        if (g_eventUiClone) {
            host->resource_release(mod, g_eventUiClone);
        }
        if (g_eventSceneClone) {
            host->resource_release(mod, g_eventSceneClone);
        }
    }
    g_fixtureMount = 0;
    g_uiParent = nullptr;
    g_uiChild = nullptr;
    g_sceneParent = nullptr;
    g_sceneChild = nullptr;
    g_clientEventSubscription = 0;
    g_eventUiClone = nullptr;
    g_eventSceneClone = nullptr;
    g_staleEventHandle = nullptr;
    g_staleVehicleHandle = nullptr;
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi*,
    WotbModHandle) {
}

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModFrameInfo* frame) {
    if (!frame || g_summaryWritten) return;
    TestClientEvents();
    TestNativeObjects();
    TestObjectFactories(frame);
    AdvanceSoundTest(frame);
    if (g_soundStage >= 3) WriteSummary();
}

} /* namespace */

extern "C" __declspec(dllexport) WotbModResult WOTBMOD_CALL WotbModLoad(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModInfo* outInfo) {
    if (!host || !mod || !outInfo) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    outInfo->struct_size = sizeof(*outInfo);
    outInfo->abi_version = WOTBMOD_ABI_VERSION_2_9;
    outInfo->id = "test.native-live";
    outInfo->name = "Native Client API Live Test";
    outInfo->version = "1.6.0";
    outInfo->author = "BlitzForge SDK";
    outInfo->description =
        "Live ABI 2.9 hooks, UI/Scene, vehicle/gameplay events, audio, skins and host API test";
    outInfo->flags = 0;
    outInfo->on_enable = &OnEnable;
    outInfo->on_disable = &OnDisable;
    outInfo->on_unload = &OnUnload;
    outInfo->on_frame = &OnFrame;
    return WOTBMOD_OK;
}
