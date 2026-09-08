#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../include/wotb_mod_api.h"

static const WotbModHostApi* g_host = nullptr;
static WotbModHandle g_mod = nullptr;
static WotbModResourceMountId g_mount = 0;
static WotbModResourceHandle g_resource = nullptr;
static WotbModVehicleSkinHandle g_vehicleSkin = nullptr;
static volatile LONG g_passes = 0;
static volatile LONG g_failures = 0;
static volatile LONG g_skips = 0;
static volatile LONG g_mainThreadSeen = 0;
static volatile LONG g_clientEventSeen = 0;
static bool g_frameSeen = false;
static WotbModEventSubscriptionId g_liveEventSubscription = 0;

static void LogResult(
    const char* state,
    const char* name,
    WotbModLogLevel level) {
    wotbmod_logf(
        g_host,
        g_mod,
        level,
        "api-selftest:%s:%s",
        state,
        name);
}

static void Pass(const char* name) {
    InterlockedIncrement(&g_passes);
    LogResult("PASS", name, WOTBMOD_LOG_INFO);
}

static void Fail(const char* name, WotbModResult result) {
    InterlockedIncrement(&g_failures);
    wotbmod_logf(
        g_host,
        g_mod,
        WOTBMOD_LOG_ERROR,
        "api-selftest:FAIL:%s result=%d",
        name,
        (int)result);
}

static void Skip(const char* name, WotbModResult result) {
    InterlockedIncrement(&g_skips);
    wotbmod_logf(
        g_host,
        g_mod,
        WOTBMOD_LOG_WARNING,
        "api-selftest:SKIP:%s result=%d",
        name,
        (int)result);
}

static bool Expect(
    const char* name,
    WotbModResult actual,
    WotbModResult expected) {
    if (actual == expected) {
        Pass(name);
        return true;
    }
    Fail(name, actual);
    return false;
}

static bool GetPathValue(
    WotbModPath kind,
    char* buffer,
    uint32_t capacity,
    const char* name) {
    uint32_t required = 0;
    if (g_host->get_path(g_mod, kind, nullptr, &required) !=
            WOTBMOD_ERROR_BUFFER_TOO_SMALL ||
        required == 0 ||
        required > capacity) {
        Fail(name, WOTBMOD_ERROR_PLATFORM);
        return false;
    }
    uint32_t size = capacity;
    const WotbModResult result =
        g_host->get_path(g_mod, kind, buffer, &size);
    if (result != WOTBMOD_OK || size != required || !buffer[0]) {
        Fail(name, result);
        return false;
    }
    Pass(name);
    return true;
}

static bool WriteMarkerFile(char* outDataPath, uint32_t capacity) {
    if (!GetPathValue(
            WOTBMOD_PATH_DATA,
            outDataPath,
            capacity,
            "get_path:data")) {
        return false;
    }

    char directory[WOTBMOD_MAX_PATH] = {};
    const int directoryLength = _snprintf_s(
        directory,
        sizeof(directory),
        _TRUNCATE,
        "%s\\selftest",
        outDataPath);
    if (directoryLength <= 0) {
        Fail("marker:directory-path", WOTBMOD_ERROR_BUFFER_TOO_SMALL);
        return false;
    }
    if (!CreateDirectoryA(directory, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        Fail("marker:create-directory", WOTBMOD_ERROR_PLATFORM);
        return false;
    }

    char filePath[WOTBMOD_MAX_PATH] = {};
    const int fileLength = _snprintf_s(
        filePath,
        sizeof(filePath),
        _TRUNCATE,
        "%s\\marker.txt",
        directory);
    if (fileLength <= 0) {
        Fail("marker:file-path", WOTBMOD_ERROR_BUFFER_TOO_SMALL);
        return false;
    }

    HANDLE file = CreateFileA(
        filePath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Fail("marker:create", WOTBMOD_ERROR_PLATFORM);
        return false;
    }
    static const char payload[] = "BlitzForge API self-test\n";
    DWORD written = 0;
    const BOOL writeOk = WriteFile(
        file,
        payload,
        (DWORD)(sizeof(payload) - 1),
        &written,
        nullptr);
    CloseHandle(file);
    if (!writeOk || written != sizeof(payload) - 1) {
        Fail("marker:write", WOTBMOD_ERROR_PLATFORM);
        return false;
    }
    Pass("marker:write");
    return true;
}

static void CheckSurface(void) {
    const bool complete =
        g_host->struct_size >=
            offsetof(WotbModHostApi, vehicle_skin_release) +
                sizeof(g_host->vehicle_skin_release) &&
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
        g_host->vehicle_skin_release;
    if (complete) {
        Pass("host-surface:76-functions");
    } else {
        Fail("host-surface:76-functions", WOTBMOD_ERROR_UNSUPPORTED_ABI);
    }
}

static void CheckPaths(void) {
    char path[WOTBMOD_MAX_PATH] = {};
    GetPathValue(WOTBMOD_PATH_GAME, path, sizeof(path), "get_path:game");
    GetPathValue(WOTBMOD_PATH_MODS, path, sizeof(path), "get_path:mods");
    GetPathValue(WOTBMOD_PATH_MODULE, path, sizeof(path), "get_path:module");
    GetPathValue(WOTBMOD_PATH_CONFIG, path, sizeof(path), "get_path:config");
}

static void CheckSymbols(void) {
    void* gameModule = g_host->get_game_module();
    if (!gameModule) {
        Skip(
            "get_game_module:legacy-raw-disabled",
            WOTBMOD_ERROR_ACCESS_DENIED);
        Skip(
            "resolve_rva:legacy-raw-disabled",
            WOTBMOD_ERROR_ACCESS_DENIED);
        Skip(
            "get_proc_address:legacy-raw-disabled",
            WOTBMOD_ERROR_ACCESS_DENIED);
        Skip(
            "find_pattern:legacy-raw-disabled",
            WOTBMOD_ERROR_ACCESS_DENIED);
        return;
    }
    Pass("get_game_module");

    if (gameModule && g_host->resolve_rva(0) == gameModule) {
        Pass("resolve_rva");
    } else {
        Fail("resolve_rva", WOTBMOD_ERROR_NOT_FOUND);
    }

    if (g_host->get_proc_address(
            "kernel32.dll",
            "GetModuleHandleA")) {
        Pass("get_proc_address");
    } else {
        Fail("get_proc_address", WOTBMOD_ERROR_NOT_FOUND);
    }

    const uint8_t dosSignature[] = {'M', 'Z'};
    if (g_host->find_pattern(nullptr, dosSignature, "xx") == gameModule) {
        Pass("find_pattern");
    } else {
        Fail("find_pattern", WOTBMOD_ERROR_NOT_FOUND);
    }
}

static void CheckHookValidation(void) {
    void* original = nullptr;
    Expect(
        "hook_create:invalid",
        g_host->hook_create(g_mod, nullptr, nullptr, &original),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "hook_enable:invalid",
        g_host->hook_enable(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "hook_disable:invalid",
        g_host->hook_disable(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "hook_remove:invalid",
        g_host->hook_remove(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
}

static void CheckConfig(void) {
    Expect(
        "config_set_int",
        g_host->config_set_int(
            g_mod, "selftest", "integer", 20260728),
        WOTBMOD_OK);
    if (g_host->config_get_int(
            g_mod, "selftest", "integer", -1) == 20260728) {
        Pass("config_get_int");
    } else {
        Fail("config_get_int", WOTBMOD_ERROR_PLATFORM);
    }

    Expect(
        "config_set_string",
        g_host->config_set_string(
            g_mod, "selftest", "string", "api-2.9-ok"),
        WOTBMOD_OK);
    char value[32] = {};
    const WotbModResult result = g_host->config_get_string(
        g_mod,
        "selftest",
        "string",
        "missing",
        value,
        sizeof(value));
    if (result == WOTBMOD_OK &&
        strcmp(value, "api-2.9-ok") == 0) {
        Pass("config_get_string");
    } else {
        Fail("config_get_string", result);
    }
}

static void CheckModRegistry(void) {
    const uint32_t count = g_host->get_mod_count();
    if (count > 0) Pass("get_mod_count");
    else Fail("get_mod_count", WOTBMOD_ERROR_NOT_FOUND);

    bool foundSelf = false;
    for (uint32_t index = 0; index < count; ++index) {
        WotbModPublicInfo info = {};
        info.struct_size = sizeof(info);
        if (g_host->get_mod_info(index, &info) == WOTBMOD_OK &&
            strcmp(info.id, "test.api-selftest") == 0) {
            foundSelf = true;
            break;
        }
    }
    if (foundSelf) Pass("get_mod_info");
    else Fail("get_mod_info", WOTBMOD_ERROR_NOT_FOUND);

    Expect(
        "set_mod_enabled:not-found",
        g_host->set_mod_enabled(
            "test.api-selftest.missing",
            1),
        WOTBMOD_ERROR_NOT_FOUND);
}

static void CheckResources(void) {
    char dataPath[WOTBMOD_MAX_PATH] = {};
    if (!WriteMarkerFile(dataPath, sizeof(dataPath))) return;

    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Mods/test.api-selftest/";
    mount.source_directory = "selftest";
    mount.priority = 100;
    mount.flags = WOTBMOD_RESOURCE_MOUNT_NONE;
    if (!Expect(
            "resource_mount",
            g_host->resource_mount(g_mod, &mount, &g_mount),
            WOTBMOD_OK)) {
        return;
    }

    static const char virtualPath[] =
        "~res:/Mods/test.api-selftest/marker.txt";
    uint32_t required = 0;
    WotbModResult result = g_host->resource_resolve(
        g_mod, virtualPath, nullptr, &required);
    if (result != WOTBMOD_ERROR_BUFFER_TOO_SMALL ||
        required == 0 ||
        required > WOTBMOD_MAX_RESOURCE_PATH) {
        Fail("resource_resolve:size", result);
    } else {
        char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
        uint32_t capacity = sizeof(resolved);
        result = g_host->resource_resolve(
            g_mod, virtualPath, resolved, &capacity);
        if (result == WOTBMOD_OK && resolved[0]) {
            Pass("resource_resolve");
        } else {
            Fail("resource_resolve", result);
        }
    }

    WotbModResourceLoadRequest request = {};
    request.struct_size = sizeof(request);
    request.type = WOTBMOD_RESOURCE_GENERIC;
    request.virtual_path = virtualPath;
    result = g_host->resource_load(g_mod, &request, &g_resource);
    if (result == WOTBMOD_OK) {
        Pass("resource_load:generic");
        result = g_host->resource_reload(g_mod, g_resource);
        if (result == WOTBMOD_OK) Pass("resource_reload:generic");
        else Skip("resource_reload:generic", result);
    } else {
        Skip("resource_load:generic", result);
        Expect(
            "resource_reload:invalid",
            g_host->resource_reload(g_mod, nullptr),
            WOTBMOD_ERROR_INVALID_ARGUMENT);
        Expect(
            "resource_release:invalid",
            g_host->resource_release(g_mod, nullptr),
            WOTBMOD_ERROR_INVALID_ARGUMENT);
    }
}

static void CheckVehicleSkins(void) {
    WotbModVehicleSkinAsset asset = {};
    asset.struct_size = sizeof(asset);
    asset.kind = WOTBMOD_SKIN_ASSET_MESH;
    asset.stock_virtual_path =
        "~res:/3d/Tanks/SelfTest/hull.sc2";
    asset.replacement_virtual_path =
        "~res:/Mods/test.api-selftest/marker.txt";
    WotbModVehicleSkinDescriptor descriptor = {};
    descriptor.struct_size = sizeof(descriptor);
    descriptor.skin_id = "selftest-skin";
    descriptor.vehicle_name = "SelfTestTank";
    descriptor.assets = &asset;
    descriptor.asset_count = 1;
    descriptor.priority = 100;
    descriptor.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;
    if (!Expect(
            "vehicle_skin_register",
            g_host->vehicle_skin_register(
                g_mod, &descriptor, &g_vehicleSkin),
            WOTBMOD_OK)) {
        return;
    }

    WotbModVehicleSkinInfo info = {};
    info.struct_size = sizeof(info);
    WotbModResult result =
        g_host->vehicle_skin_get_info(
            g_mod, g_vehicleSkin, &info);
    if (result == WOTBMOD_OK &&
        info.enabled == 1 &&
        info.asset_count == 1 &&
        strcmp(info.skin_id, "selftest-skin") == 0 &&
        strcmp(info.vehicle_name, "SelfTestTank") == 0) {
        Pass("vehicle_skin_get_info");
    } else {
        Fail("vehicle_skin_get_info", result);
    }
    Expect(
        "vehicle_skin_set_enabled:disable",
        g_host->vehicle_skin_set_enabled(
            g_mod, g_vehicleSkin, 0),
        WOTBMOD_OK);
    Expect(
        "vehicle_skin_set_enabled:enable",
        g_host->vehicle_skin_set_enabled(
            g_mod, g_vehicleSkin, 1),
        WOTBMOD_OK);
    Expect(
        "vehicle_skin_register:invalid",
        g_host->vehicle_skin_register(
            g_mod, nullptr, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_skin_get_info:invalid",
        g_host->vehicle_skin_get_info(
            g_mod, nullptr, &info),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_skin_release:invalid",
        g_host->vehicle_skin_release(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
}

static void CheckAudioValidation(void) {
    WotbModAudioPlaybackHandle playback = nullptr;
    WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
    WotbModAudioPlayInfo parameters = {};
    parameters.struct_size = sizeof(parameters);
    parameters.volume = 1.0f;
    parameters.pitch = 1.0f;
    parameters.min_distance = 1.0f;
    parameters.max_distance = 100.0f;

    Expect(
        "audio_clip_load:invalid",
        g_host->audio_clip_load(g_mod, nullptr, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "audio_play:invalid",
        g_host->audio_play(g_mod, nullptr, nullptr, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "audio_pause:invalid",
        g_host->audio_pause(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "audio_resume:invalid",
        g_host->audio_resume(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "audio_stop:invalid",
        g_host->audio_stop(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "audio_set_parameters:invalid",
        g_host->audio_set_parameters(g_mod, nullptr, &parameters),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "audio_get_state:invalid",
        g_host->audio_get_state(g_mod, nullptr, &state),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "audio_release:invalid",
        g_host->audio_release(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
}

static void CheckSoundValidation(void) {
    WotbModSoundEventHandle event = nullptr;
    WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
    float value = 0.0f;
    int32_t hasParameter = 0;
    uint32_t nameSize = 0;
    Expect(
        "sound_event_create:invalid",
        g_host->sound_event_create(g_mod, nullptr, &event),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_trigger:invalid",
        g_host->sound_event_trigger(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_stop:invalid",
        g_host->sound_event_stop(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_set_paused:invalid",
        g_host->sound_event_set_paused(g_mod, nullptr, 1),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_set_volume:invalid",
        g_host->sound_event_set_volume(g_mod, nullptr, 1.0f),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_set_position:invalid",
        g_host->sound_event_set_position(
            g_mod, nullptr, 0.0f, 0.0f, 0.0f),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_get_state:invalid",
        g_host->sound_event_get_state(g_mod, nullptr, &state),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_set_parameter:invalid",
        g_host->sound_event_set_parameter(
            g_mod, nullptr, "speed", 1.0f),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_get_parameter:invalid",
        g_host->sound_event_get_parameter(
            g_mod, nullptr, "speed", &value),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_has_parameter:invalid",
        g_host->sound_event_has_parameter(
            g_mod, nullptr, "speed", &hasParameter),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_get_name:invalid",
        g_host->sound_event_get_name(
            g_mod, nullptr, nullptr, &nameSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "sound_event_release:invalid",
        g_host->sound_event_release(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
}

static void CheckObjectValidation(void) {
    WotbModResourceHandle fake =
        reinterpret_cast<WotbModResourceHandle>(1);
    Expect(
        "ui_control_set_geometry:invalid",
        g_host->ui_control_set_geometry(g_mod, fake, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_set_visible:invalid",
        g_host->ui_control_set_visible(g_mod, fake, 1),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_add_child:invalid",
        g_host->ui_control_add_child(g_mod, fake, fake),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_remove_child:invalid",
        g_host->ui_control_remove_child(g_mod, fake, fake),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "scene_set_transform:invalid",
        g_host->scene_set_transform(g_mod, fake, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "scene_add_child:invalid",
        g_host->scene_add_child(g_mod, fake, fake),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "scene_remove_child:invalid",
        g_host->scene_remove_child(g_mod, fake, fake),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_create:invalid",
        g_host->ui_control_create(g_mod, nullptr, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_get_active_screen:invalid",
        g_host->ui_get_active_screen(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "scene_entity_create:invalid",
        g_host->scene_entity_create(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "scene_get_active:invalid",
        g_host->scene_get_active(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    WotbModResourceHandle control = nullptr;
    Expect(
        "ui_control_find_by_name:invalid",
        g_host->ui_control_find_by_name(
            g_mod, fake, nullptr, 1, &control),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_get_parent:invalid",
        g_host->ui_control_get_parent(
            g_mod, fake, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_get_child_count:invalid",
        g_host->ui_control_get_child_count(
            g_mod, fake, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_get_child_at:invalid",
        g_host->ui_control_get_child_at(
            g_mod, fake, 0, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_get_state:invalid",
        g_host->ui_control_get_state(
            g_mod, fake, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_set_input_enabled:invalid",
        g_host->ui_control_set_input_enabled(
            g_mod, fake, 1, 0),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "ui_control_set_disabled:invalid",
        g_host->ui_control_set_disabled(
            g_mod, fake, 1, 0),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
}

static void CheckVehicleValidation(void) {
    WotbModVehicleHandle vehicle = nullptr;
    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    Expect(
        "vehicle_get_local:invalid",
        g_host->vehicle_get_local(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_get_by_entity_id:invalid",
        g_host->vehicle_get_by_entity_id(
            g_mod, 0, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_get_count:invalid",
        g_host->vehicle_get_count(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_get_at:invalid",
        g_host->vehicle_get_at(g_mod, 0, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_clone:invalid",
        g_host->vehicle_clone(g_mod, nullptr, &vehicle),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_release:invalid",
        g_host->vehicle_release(g_mod, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Expect(
        "vehicle_get_info:invalid",
        g_host->vehicle_get_info(g_mod, nullptr, &info),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
}

static void WOTBMOD_CALL LiveClientEvent(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void*) {
    if (!host || !mod || !event ||
        event->struct_size < sizeof(*event) ||
        event->type == 0 ||
        (event->type & ~WOTBMOD_EVENT_ALL) != 0) {
        Fail("event:delivery", WOTBMOD_ERROR_PLATFORM);
        return;
    }
    if (event->vehicle) {
        WotbModVehicleInfo info = {};
        info.struct_size = sizeof(info);
        const WotbModResult result =
            host->vehicle_get_info(
                mod, event->vehicle, &info);
        if (result != WOTBMOD_OK || info.entity_id == 0) {
            Fail("event:vehicle", result);
            return;
        }
    }
    if (InterlockedCompareExchange(
            &g_clientEventSeen, 1, 0) == 0) {
        Pass("event:delivery");
    }
}

static void CheckClientEvents(void) {
    WotbModResourceHandle resource = nullptr;
    if (g_resource &&
        Expect(
            "resource_clone:owned",
            g_host->resource_clone(
                g_mod,
                g_resource,
                &resource),
            WOTBMOD_OK)) {
        Expect(
            "resource_clone:owned-release",
            g_host->resource_release(g_mod, resource),
            WOTBMOD_OK);
        resource = nullptr;
    }
    Expect(
        "resource_clone:invalid",
        g_host->resource_clone(
            g_mod,
            reinterpret_cast<WotbModResourceHandle>(
                ~static_cast<uintptr_t>(0)),
            &resource),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    WotbModEventSubscriptionId subscription = 0;
    Expect(
        "event_subscribe:invalid-mask",
            g_host->event_subscribe(
                g_mod,
                0,
                &LiveClientEvent,
                nullptr,
                &subscription),
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    if (Expect(
            "event_subscribe",
            g_host->event_subscribe(
                g_mod,
                WOTBMOD_EVENT_ALL,
                &LiveClientEvent,
                nullptr,
                &subscription),
            WOTBMOD_OK)) {
        Expect(
            "event_unsubscribe",
            g_host->event_unsubscribe(g_mod, subscription),
            WOTBMOD_OK);
        Expect(
            "event_unsubscribe:stale",
            g_host->event_unsubscribe(g_mod, subscription),
            WOTBMOD_ERROR_NOT_FOUND);
    }
    const WotbModResult liveResult =
        g_host->event_subscribe(
            g_mod,
            WOTBMOD_EVENT_ALL,
            &LiveClientEvent,
            nullptr,
            &g_liveEventSubscription);
    if (liveResult == WOTBMOD_OK) {
        Pass("event_subscribe:live");
    } else {
        Fail("event_subscribe:live", liveResult);
    }
}

static void WOTBMOD_CALL MainThreadCheck(
    const WotbModHostApi* host,
    WotbModHandle mod,
    void*) {
    InterlockedExchange(&g_mainThreadSeen, 1);
    Pass("main_thread_enqueue:callback");

    WotbModUiControlGeometry geometry = {};
    geometry.struct_size = sizeof(geometry);
    geometry.x = 20.0f;
    geometry.y = 20.0f;
    geometry.width = 160.0f;
    geometry.height = 80.0f;
    WotbModResourceHandle handles[4] = {};
    const WotbModResult results[4] = {
        host->ui_control_create(mod, &geometry, &handles[0]),
        host->ui_get_active_screen(mod, &handles[1]),
        host->scene_entity_create(mod, &handles[2]),
        host->scene_get_active(mod, &handles[3]),
    };
    const char* labels[4] = {
        "ui_control_create",
        "ui_get_active_screen",
        "scene_entity_create",
        "scene_get_active",
    };

    if (results[0] == WOTBMOD_OK && handles[0]) {
        WotbModUiControlState state = {};
        state.struct_size = sizeof(state);
        WotbModResult result =
            host->ui_control_get_state(
                mod, handles[0], &state);
        if (result == WOTBMOD_OK) {
            Pass("ui_control_get_state");
        } else if (result == WOTBMOD_ERROR_PLATFORM) {
            Skip("ui_control_get_state", result);
        } else {
            Fail("ui_control_get_state", result);
        }
        result = host->ui_control_set_input_enabled(
            mod, handles[0], 1, 0);
        if (result == WOTBMOD_OK) {
            Pass("ui_control_set_input_enabled");
        } else if (result == WOTBMOD_ERROR_PLATFORM) {
            Skip("ui_control_set_input_enabled", result);
        } else {
            Fail("ui_control_set_input_enabled", result);
        }
        result = host->ui_control_set_disabled(
            mod, handles[0], 0, 0);
        if (result == WOTBMOD_OK) {
            Pass("ui_control_set_disabled");
        } else if (result == WOTBMOD_ERROR_PLATFORM) {
            Skip("ui_control_set_disabled", result);
        } else {
            Fail("ui_control_set_disabled", result);
        }
    }

    if (results[0] == WOTBMOD_OK && handles[0] &&
        results[1] == WOTBMOD_OK && handles[1]) {
        const WotbModResult addResult =
            host->ui_control_add_child(
                mod, handles[1], handles[0]);
        if (addResult == WOTBMOD_OK) {
            Pass("ui_control_add_child:live");
            uint32_t childCount = 0;
            WotbModResult result =
                host->ui_control_get_child_count(
                    mod, handles[1], &childCount);
            if (result == WOTBMOD_OK && childCount > 0) {
                Pass("ui_control_get_child_count");
            } else {
                Fail("ui_control_get_child_count", result);
            }

            WotbModResourceHandle child = nullptr;
            result = host->ui_control_get_child_at(
                mod,
                handles[1],
                childCount > 0 ? childCount - 1 : 0,
                &child);
            if (result == WOTBMOD_OK && child) {
                Pass("ui_control_get_child_at");
                const WotbModResult releaseResult =
                    host->resource_release(mod, child);
                if (releaseResult == WOTBMOD_OK) {
                    Pass("ui_control_get_child_at:release");
                } else {
                    Fail(
                        "ui_control_get_child_at:release",
                        releaseResult);
                }
            } else {
                Fail("ui_control_get_child_at", result);
            }

            WotbModResourceHandle parent = nullptr;
            result = host->ui_control_get_parent(
                mod, handles[0], &parent);
            if (result == WOTBMOD_OK && parent) {
                Pass("ui_control_get_parent");
                const WotbModResult releaseResult =
                    host->resource_release(mod, parent);
                if (releaseResult == WOTBMOD_OK) {
                    Pass("ui_control_get_parent:release");
                } else {
                    Fail(
                        "ui_control_get_parent:release",
                        releaseResult);
                }
            } else {
                Fail("ui_control_get_parent", result);
            }

            result = host->ui_control_remove_child(
                mod, handles[1], handles[0]);
            if (result == WOTBMOD_OK) {
                Pass("ui_control_remove_child:live");
            } else {
                Fail("ui_control_remove_child:live", result);
            }
        } else if (addResult == WOTBMOD_ERROR_PLATFORM) {
            Skip("ui-tree:live", addResult);
        } else {
            Fail("ui_control_add_child:live", addResult);
        }
    }

    uint32_t vehicleCount = 0;
    WotbModResult vehicleResult =
        host->vehicle_get_count(mod, &vehicleCount);
    if (vehicleResult == WOTBMOD_OK) {
        Pass("vehicle_get_count");
        if (vehicleCount > 0) {
            WotbModVehicleHandle vehicle = nullptr;
            vehicleResult =
                host->vehicle_get_local(mod, &vehicle);
            if (vehicleResult == WOTBMOD_OK && vehicle) {
                Pass("vehicle_get_local");
                WotbModVehicleInfo info = {};
                info.struct_size = sizeof(info);
                vehicleResult = host->vehicle_get_info(
                    mod, vehicle, &info);
                if (vehicleResult == WOTBMOD_OK &&
                    info.entity_id != 0) {
                    Pass("vehicle_get_info");
                } else {
                    Fail("vehicle_get_info", vehicleResult);
                }
                WotbModVehicleHandle clone = nullptr;
                vehicleResult = host->vehicle_clone(
                    mod, vehicle, &clone);
                if (vehicleResult == WOTBMOD_OK && clone) {
                    Pass("vehicle_clone");
                    if (host->vehicle_release(mod, clone) ==
                        WOTBMOD_OK) {
                        Pass("vehicle_clone:release");
                    } else {
                        Fail(
                            "vehicle_clone:release",
                            WOTBMOD_ERROR_PLATFORM);
                    }
                } else {
                    Fail("vehicle_clone", vehicleResult);
                }
                if (host->vehicle_release(mod, vehicle) ==
                    WOTBMOD_OK) {
                    Pass("vehicle_release");
                } else {
                    Fail(
                        "vehicle_release",
                        WOTBMOD_ERROR_PLATFORM);
                }
            } else if (
                vehicleResult == WOTBMOD_ERROR_NOT_FOUND) {
                Skip("vehicle_get_local", vehicleResult);
            } else {
                Fail("vehicle_get_local", vehicleResult);
            }
        } else {
            Skip(
                "vehicle_get_local",
                WOTBMOD_ERROR_NOT_FOUND);
        }
    } else if (vehicleResult == WOTBMOD_ERROR_PLATFORM) {
        Skip("vehicle_get_count", vehicleResult);
    } else {
        Fail("vehicle_get_count", vehicleResult);
    }

    for (uint32_t index = 0; index < 4; ++index) {
        if (results[index] == WOTBMOD_OK && handles[index]) {
            Pass(labels[index]);
            const WotbModResult releaseResult =
                host->resource_release(mod, handles[index]);
            if (releaseResult == WOTBMOD_OK) {
                char releaseLabel[96] = {};
                _snprintf_s(
                    releaseLabel,
                    sizeof(releaseLabel),
                    _TRUNCATE,
                    "%s:release",
                    labels[index]);
                Pass(releaseLabel);
            } else {
                Fail(labels[index], releaseResult);
            }
        } else if (
            results[index] == WOTBMOD_ERROR_PLATFORM ||
            results[index] == WOTBMOD_ERROR_NOT_FOUND) {
            Skip(labels[index], results[index]);
        } else {
            Fail(labels[index], results[index]);
        }
    }
}

static void CheckMainThreadQueue(void) {
    const WotbModResult result =
        g_host->main_thread_enqueue(
            g_mod, &MainThreadCheck, nullptr);
    if (result != WOTBMOD_OK) {
        Fail("main_thread_enqueue:accepted", result);
        return;
    }
    Pass("main_thread_enqueue:accepted");
    if (InterlockedCompareExchange(&g_mainThreadSeen, 0, 0) == 0) {
        Pass("main_thread_enqueue:deferred");
    } else {
        Fail(
            "main_thread_enqueue:deferred",
            WOTBMOD_ERROR_PLATFORM);
    }
}

static void Cleanup(void) {
    g_liveEventSubscription = 0;
    if (g_vehicleSkin) {
        const WotbModResult result =
            g_host->vehicle_skin_release(
                g_mod, g_vehicleSkin);
        if (result == WOTBMOD_OK) {
            Pass("vehicle_skin_release");
        } else {
            Fail("vehicle_skin_release", result);
        }
        g_vehicleSkin = nullptr;
    }
    if (g_resource) {
        const WotbModResult result =
            g_host->resource_release(g_mod, g_resource);
        if (result == WOTBMOD_OK) Pass("resource_release:generic");
        else Fail("resource_release:generic", result);
        g_resource = nullptr;
    }
    if (g_mount) {
        const WotbModResult result =
            g_host->resource_unmount(g_mod, g_mount);
        if (result == WOTBMOD_OK) Pass("resource_unmount");
        else Fail("resource_unmount", result);
        g_mount = 0;
    }
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    g_host = host;
    g_mod = mod;
    g_frameSeen = false;
    InterlockedExchange(&g_passes, 0);
    InterlockedExchange(&g_failures, 0);
    InterlockedExchange(&g_skips, 0);
    InterlockedExchange(&g_mainThreadSeen, 0);
    InterlockedExchange(&g_clientEventSeen, 0);
    g_liveEventSubscription = 0;

    host->log(
        mod,
        WOTBMOD_LOG_INFO,
        "api-selftest:BEGIN ABI 2.9");
    CheckSurface();
    CheckPaths();
    CheckSymbols();
    CheckHookValidation();
    CheckConfig();
    CheckModRegistry();
    CheckResources();
    CheckVehicleSkins();
    CheckAudioValidation();
    CheckSoundValidation();
    CheckObjectValidation();
    CheckVehicleValidation();
    CheckClientEvents();
    CheckMainThreadQueue();
}

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModFrameInfo* frame) {
    if (!g_frameSeen && frame &&
        frame->struct_size >= sizeof(*frame)) {
        g_frameSeen = true;
        Pass("on_frame");
        wotbmod_logf(
            g_host,
            g_mod,
            WOTBMOD_LOG_INFO,
            "api-selftest:SUMMARY passes=%ld skips=%ld failures=%ld",
            InterlockedCompareExchange(&g_passes, 0, 0),
            InterlockedCompareExchange(&g_skips, 0, 0),
            InterlockedCompareExchange(&g_failures, 0, 0));
    }
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi*,
    WotbModHandle) {
    Cleanup();
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi*,
    WotbModHandle) {
    g_host = nullptr;
    g_mod = nullptr;
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbApiSelfTest_GetPasses(void) {
    return (uint32_t)InterlockedCompareExchange(&g_passes, 0, 0);
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbApiSelfTest_GetSkips(void) {
    return (uint32_t)InterlockedCompareExchange(&g_skips, 0, 0);
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbApiSelfTest_GetFailures(void) {
    return (uint32_t)InterlockedCompareExchange(&g_failures, 0, 0);
}

WOTBMOD_ENTRY {
    if (!host || !mod || !out_info ||
        WOTBMOD_ABI_MAJOR(host->abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.api-selftest";
    out_info->name = "API 2.9 Self-Test";
    out_info->version = "2.9.0";
    out_info->author = "BlitzForge SDK";
    out_info->description =
        "Runs non-invasive checks across all 76 host API functions.";
    out_info->on_enable = &OnEnable;
    out_info->on_frame = &OnFrame;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    return WOTBMOD_OK;
}
