#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/wotb_mod_runtime.h"
#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotb_mod_windows_audio.h"
#include "../include/wotbmod/camera_v1.h"

namespace {

const uint32_t kNativeMagic = 0x51534552u;
const uint32_t kPlaybackMagic = 0x59414C50u;
const uint32_t kSoundEventMagic = 0x444E5354u;
const uint32_t kMaxTrackedNative = 256u;
const uint32_t kMaxTrackedPlaybacks = 256u;
const uint32_t kMaxTrackedSoundEvents = 256u;
const uint32_t kMaxBackendHooks = 256u;
const uint32_t kMaxNativeChildren = 16u;
const uint32_t kVehicleTokenMagic = 0x4B4F5456u;

#pragma section(".wotbqa", read)
__declspec(allocate(".wotbqa")) const uint8_t g_imagePattern[] = {
    0xD3, 0x71, 0xA9, 0x4C, 0x16, 0xE2, 0x8B, 0x55,
    0xC7, 0x39, 0xF0, 0x6D, 0x22, 0x94, 0xBE, 0x41};

struct NativeResource {
    uint32_t magic;
    WotbModResourceType type;
    uint32_t flags;
    bool reloadable;
    int32_t visible;
    int32_t input_enabled;
    int32_t disabled;
    NativeResource* parent;
    NativeResource* children[kMaxNativeChildren];
    uint32_t child_count;
    WotbModUiControlGeometry geometry;
    WotbModSceneTransform transform;
    char path[WOTBMOD_MAX_RESOURCE_PATH];
    char object_name[128];
};

struct NativePlayback {
    uint32_t magic;
    NativeResource* clip;
    WotbModAudioPlayInfo parameters;
    WotbModAudioState state;
};

struct NativeSoundEvent {
    uint32_t magic;
    WotbModAudioState state;
    float volume;
    float position[3];
    float parameter_value;
    bool has_parameter;
    char name[WOTBMOD_MAX_SOUND_EVENT_NAME];
    char parameter_name[WOTBMOD_MAX_SOUND_EVENT_NAME];
};

struct TestVehicleToken {
    uint32_t magic;
    WotbModVehicleInfo info;
};

#pragma pack(push, 1)
struct TestWaveHeader {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char format_tag[4];
    uint32_t format_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_tag[4];
    uint32_t data_size;
};
#pragma pack(pop)

struct BackendHook {
    void* target;
    bool active;
    bool enabled;
};

struct ContractExports {
    WotbModHandle(WOTBMOD_CALL* get_handle)(void);
    const WotbModHostApi*(WOTBMOD_CALL* get_host)(void);
    uint32_t(WOTBMOD_CALL* get_enable_count)(void);
    uint32_t(WOTBMOD_CALL* get_disable_count)(void);
    uint32_t(WOTBMOD_CALL* get_unload_count)(void);
    uint32_t(WOTBMOD_CALL* get_frame_count)(void);
    int32_t(WOTBMOD_CALL* copy_last_frame)(WotbModFrameInfo*);
};

struct TestContext {
    int assertions;
    int failures;
};

struct ResolveThreadContext {
    const char* path;
    LONG failures;
};

struct ObjectWorkerContext {
    const WotbModHostApi* host;
    WotbModHandle owner;
    WotbModResourceHandle control;
    WotbModResourceHandle scene;
    WotbModResult visible_result;
    WotbModResult transform_result;
    WotbModSceneTransform transform;
};

struct FactoryWorkerContext {
    const WotbModHostApi* host;
    WotbModHandle owner;
    WotbModResult ui_create_result;
    WotbModResult scene_get_result;
    WotbModResourceHandle control;
    WotbModResourceHandle scene;
};

struct VehicleWorkerContext {
    const WotbModHostApi* host;
    WotbModHandle owner;
    WotbModResult count_result;
    WotbModResult local_result;
    uint32_t count;
    WotbModVehicleHandle vehicle;
};

struct ClientEventProbe {
    DWORD expected_thread_id;
    uint32_t callback_count;
    uint32_t types[8];
    uint64_t sequences[8];
    bool structures_valid;
    bool main_thread_only;
    bool payloads_valid;
    WotbModResult borrowed_release_result;
    WotbModResult borrowed_reload_result;
    WotbModResult clone_result;
    WotbModResult mutation_result;
    WotbModResourceHandle cloned_resource;
    WotbModResourceHandle stale_borrowed_resource;
};

struct GameplayEventProbe {
    DWORD expected_thread_id;
    uint32_t callback_count;
    uint32_t types[16];
    uint64_t sequences[16];
    bool structures_valid;
    bool main_thread_only;
    bool payloads_valid;
    bool vehicles_valid;
    WotbModResult borrowed_release_result;
    WotbModResult clone_result;
    WotbModVehicleHandle cloned_vehicle;
    WotbModVehicleHandle stale_borrowed_vehicle;
};

static TestContext g_test = {};
static NativeResource* g_native[kMaxTrackedNative] = {};
static NativePlayback* g_playbacks[kMaxTrackedPlaybacks] = {};
static NativeSoundEvent* g_soundEvents[kMaxTrackedSoundEvents] = {};
static BackendHook g_hooks[kMaxBackendHooks] = {};
static LONG g_nativeActive = 0;
static LONG g_resourceLoads = 0;
static LONG g_resourceReloads = 0;
static LONG g_resourceReleases = 0;
static LONG g_resourceClones = 0;
static LONG g_registryChanges = 0;
static LONG64 g_lastRegistryGeneration = 0;
static LONG g_hookCreates = 0;
static LONG g_hookEnables = 0;
static LONG g_hookDisables = 0;
static LONG g_hookRemoves = 0;
static LONG g_hookFailureMode = 0;
static LONG g_contractEnableLogs = 0;
static LONG g_contractDisableLogs = 0;
static LONG g_contractUnloadLogs = 0;
static LONG g_formattedLogs = 0;
static LONG g_resourceFaultMode = 0;
static LONG g_objectFaultMode = 0;
static LONG g_uiGeometrySets = 0;
static LONG g_uiVisibilitySets = 0;
static LONG g_uiChildAdds = 0;
static LONG g_uiChildRemoves = 0;
static LONG g_sceneTransformSets = 0;
static LONG g_sceneChildAdds = 0;
static LONG g_sceneChildRemoves = 0;
static LONG g_uiCreates = 0;
static LONG g_uiScreenGets = 0;
static LONG g_sceneEntityCreates = 0;
static LONG g_sceneGets = 0;
static LONG g_mainThreadCallbackCount = 0;
static LONG g_mainThreadCallbackOrderCount = 0;
static int32_t g_mainThreadCallbackOrder[128] = {};
static LONG g_faultingEventCallbackCount = 0;
static LONG g_audioFaultMode = 0;
static LONG g_playbackActive = 0;
static LONG g_audioPlays = 0;
static LONG g_audioPauses = 0;
static LONG g_audioResumes = 0;
static LONG g_audioStops = 0;
static LONG g_audioParameterSets = 0;
static LONG g_audioStateQueries = 0;
static LONG g_audioReleases = 0;
static LONG g_audioClipLoads = 0;
static LONG g_audioClipReloads = 0;
static LONG g_audioClipReleases = 0;
static LONG g_soundEventActive = 0;
static LONG g_soundEventCreates = 0;
static LONG g_soundEventTriggers = 0;
static LONG g_soundEventStops = 0;
static LONG g_soundEventReleases = 0;
static LONG g_soundFaultMode = 0;
static LONG g_vehicleTokensActive = 0;
static LONG g_vehicleTokensCreated = 0;
static LONG g_vehicleTokensReleased = 0;
static LONG g_gameplayFaultMode = 0;
static WotbModAudioPlayInfo g_lastAudioPlayInfo = {};
static char g_lastResourcePath[WOTBMOD_MAX_RESOURCE_PATH] = {};
static char g_lastResourceObject[128] = {};
static uint32_t g_lastResourceFlags = 0;
static WotbModResourceType g_lastResourceType = WOTBMOD_RESOURCE_GENERIC;
static uint8_t g_hookTargets[80] = {};

static bool Expect(bool condition, const char* label) {
    ++g_test.assertions;
    if (condition) return true;
    ++g_test.failures;
    fprintf(stderr, "API ASSERT FAILED: %s\n", label ? label : "unknown");
    return false;
}

static bool ExpectResult(
    WotbModResult actual,
    WotbModResult expected,
    const char* label) {
    if (actual == expected) {
        ++g_test.assertions;
        return true;
    }
    ++g_test.assertions;
    ++g_test.failures;
    fprintf(
        stderr,
        "API ASSERT FAILED: %s (actual=%d expected=%d)\n",
        label ? label : "result",
        (int)actual,
        (int)expected);
    return false;
}

static void WOTBMOD_CALL LogSink(
    WotbModLogLevel,
    const char* message,
    void*) {
    const char* text = message ? message : "";
    if (strstr(text, "[test.contract] contract:on_enable")) {
        InterlockedIncrement(&g_contractEnableLogs);
    }
    if (strstr(text, "[test.contract] contract:on_disable")) {
        InterlockedIncrement(&g_contractDisableLogs);
    }
    if (strstr(text, "[test.contract] contract:on_unload")) {
        InterlockedIncrement(&g_contractUnloadLogs);
    }
    if (strstr(text, "[test.contract] contract:formatted:42:ok")) {
        InterlockedIncrement(&g_formattedLogs);
    }
    printf("[api-test] %s\n", text);
}

static void TrackNative(NativeResource* resource) {
    if (!resource) return;
    for (uint32_t index = 0; index < kMaxTrackedNative; ++index) {
        if (!g_native[index]) {
            g_native[index] = resource;
            InterlockedIncrement(&g_nativeActive);
            return;
        }
    }
}

static bool UntrackNative(NativeResource* resource) {
    for (uint32_t index = 0; index < kMaxTrackedNative; ++index) {
        if (g_native[index] == resource) {
            g_native[index] = nullptr;
            InterlockedDecrement(&g_nativeActive);
            return true;
        }
    }
    return false;
}

static void CleanupNativeLeaks() {
    for (uint32_t index = 0; index < kMaxTrackedNative; ++index) {
        NativeResource* resource = g_native[index];
        if (!resource) continue;
        g_native[index] = nullptr;
        HeapFree(GetProcessHeap(), 0, resource);
    }
    InterlockedExchange(&g_nativeActive, 0);
}

static void TrackPlayback(NativePlayback* playback) {
    if (!playback) return;
    for (uint32_t index = 0; index < kMaxTrackedPlaybacks; ++index) {
        if (!g_playbacks[index]) {
            g_playbacks[index] = playback;
            InterlockedIncrement(&g_playbackActive);
            return;
        }
    }
}

static bool UntrackPlayback(NativePlayback* playback) {
    for (uint32_t index = 0; index < kMaxTrackedPlaybacks; ++index) {
        if (g_playbacks[index] == playback) {
            g_playbacks[index] = nullptr;
            InterlockedDecrement(&g_playbackActive);
            return true;
        }
    }
    return false;
}

static void CleanupPlaybackLeaks() {
    for (uint32_t index = 0; index < kMaxTrackedPlaybacks; ++index) {
        NativePlayback* playback = g_playbacks[index];
        if (!playback) continue;
        g_playbacks[index] = nullptr;
        HeapFree(GetProcessHeap(), 0, playback);
    }
    InterlockedExchange(&g_playbackActive, 0);
}

static void TrackSoundEvent(NativeSoundEvent* event) {
    if (!event) return;
    for (uint32_t index = 0; index < kMaxTrackedSoundEvents; ++index) {
        if (!g_soundEvents[index]) {
            g_soundEvents[index] = event;
            InterlockedIncrement(&g_soundEventActive);
            return;
        }
    }
}

static bool UntrackSoundEvent(NativeSoundEvent* event) {
    for (uint32_t index = 0; index < kMaxTrackedSoundEvents; ++index) {
        if (g_soundEvents[index] == event) {
            g_soundEvents[index] = nullptr;
            InterlockedDecrement(&g_soundEventActive);
            return true;
        }
    }
    return false;
}

static void CleanupSoundEventLeaks() {
    for (uint32_t index = 0; index < kMaxTrackedSoundEvents; ++index) {
        NativeSoundEvent* event = g_soundEvents[index];
        if (!event) continue;
        g_soundEvents[index] = nullptr;
        HeapFree(GetProcessHeap(), 0, event);
    }
    InterlockedExchange(&g_soundEventActive, 0);
}

static WotbModResult WOTBMOD_CALL ResourceLoad(
    void*,
    const WotbModResourceLoadRequest* request,
    void** outNativeResource) {
    const LONG faultMode = InterlockedExchange(&g_resourceFaultMode, 0);
    if (faultMode == 1) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    if (!request ||
        request->struct_size < sizeof(*request) ||
        !request->virtual_path ||
        strncmp(request->virtual_path, "~res:/", 6) != 0 ||
        request->type < WOTBMOD_RESOURCE_GENERIC ||
        request->type > WOTBMOD_RESOURCE_AUDIO_CLIP ||
        !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    CopyMemory(
        g_lastResourcePath,
        "",
        1);
    strncpy_s(
        g_lastResourcePath,
        sizeof(g_lastResourcePath),
        request->virtual_path,
        _TRUNCATE);
    strncpy_s(
        g_lastResourceObject,
        sizeof(g_lastResourceObject),
        request->object_name ? request->object_name : "",
        _TRUNCATE);
    g_lastResourceFlags = request->flags;
    g_lastResourceType = request->type;
    if (faultMode == 4) return WOTBMOD_OK;

    NativeResource* resource = static_cast<NativeResource*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(NativeResource)));
    if (!resource) return WOTBMOD_ERROR_PLATFORM;
    resource->magic = kNativeMagic;
    resource->type = request->type;
    resource->flags = request->flags;
    resource->reloadable = true;
    resource->visible = 1;
    resource->input_enabled = 1;
    resource->geometry.struct_size = sizeof(resource->geometry);
    resource->transform.struct_size = sizeof(resource->transform);
    resource->transform.rotation_w = 1.0f;
    resource->transform.scale_x = 1.0f;
    resource->transform.scale_y = 1.0f;
    resource->transform.scale_z = 1.0f;
    strncpy_s(
        resource->path,
        sizeof(resource->path),
        request->virtual_path,
        _TRUNCATE);
    strncpy_s(
        resource->object_name,
        sizeof(resource->object_name),
        request->object_name ? request->object_name : "",
        _TRUNCATE);
    TrackNative(resource);
    *outNativeResource = resource;
    InterlockedIncrement(&g_resourceLoads);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceReload(
    void*,
    void* nativeResource) {
    if (InterlockedExchange(&g_resourceFaultMode, 0) == 2) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* resource = static_cast<NativeResource*>(nativeResource);
    if (!resource || resource->magic != kNativeMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!resource->reloadable) return WOTBMOD_ERROR_PLATFORM;
    InterlockedIncrement(&g_resourceReloads);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceRelease(
    void*,
    void* nativeResource) {
    if (InterlockedExchange(&g_resourceFaultMode, 0) == 3) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* resource = static_cast<NativeResource*>(nativeResource);
    if (!resource ||
        resource->magic != kNativeMagic ||
        !UntrackNative(resource)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    resource->magic = 0;
    if (!HeapFree(GetProcessHeap(), 0, resource)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedIncrement(&g_resourceReleases);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceClone(
    void*,
    void* nativeResource,
    void** outNativeResource) {
    const LONG faultMode =
        InterlockedExchange(&g_resourceFaultMode, 0);
    if (faultMode == 5) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* source =
        static_cast<NativeResource*>(nativeResource);
    if (!source ||
        source->magic != kNativeMagic ||
        !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    if (faultMode == 6) return WOTBMOD_OK;
    NativeResource* clone = static_cast<NativeResource*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(NativeResource)));
    if (!clone) return WOTBMOD_ERROR_LIMIT_REACHED;
    memcpy(clone, source, sizeof(*clone));
    clone->parent = nullptr;
    clone->child_count = 0;
    ZeroMemory(clone->children, sizeof(clone->children));
    TrackNative(clone);
    *outNativeResource = clone;
    InterlockedIncrement(&g_resourceClones);
    return WOTBMOD_OK;
}

static bool ConsumeObjectFault(LONG mode);

static WotbModResult CreateObjectResource(
    WotbModResourceType type,
    const WotbModUiControlGeometry* geometry,
    LONG faultMode,
    LONG* counter,
    void** outNativeResource) {
    if (!outNativeResource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outNativeResource = nullptr;
    if (ConsumeObjectFault(faultMode)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* resource = static_cast<NativeResource*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(NativeResource)));
    if (!resource) return WOTBMOD_ERROR_LIMIT_REACHED;
    resource->magic = kNativeMagic;
    resource->type = type;
    resource->visible = 1;
    resource->input_enabled = 1;
    resource->reloadable = false;
    resource->geometry.struct_size = sizeof(resource->geometry);
    resource->transform.struct_size = sizeof(resource->transform);
    resource->transform.rotation_w = 1.0f;
    resource->transform.scale_x = 1.0f;
    resource->transform.scale_y = 1.0f;
    resource->transform.scale_z = 1.0f;
    if (geometry) resource->geometry = *geometry;
    TrackNative(resource);
    *outNativeResource = resource;
    InterlockedIncrement(counter);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiCreate(
    void*,
    const WotbModUiControlGeometry* geometry,
    void** outNativeResource) {
    return CreateObjectResource(
        WOTBMOD_RESOURCE_UI_CONTROL,
        geometry,
        8,
        &g_uiCreates,
        outNativeResource);
}

static WotbModResult WOTBMOD_CALL UiGetActiveScreen(
    void*,
    void** outNativeResource) {
    return CreateObjectResource(
        WOTBMOD_RESOURCE_UI_CONTROL,
        nullptr,
        9,
        &g_uiScreenGets,
        outNativeResource);
}

static WotbModResult WOTBMOD_CALL SceneEntityCreate(
    void*,
    void** outNativeResource) {
    return CreateObjectResource(
        WOTBMOD_RESOURCE_SCENE,
        nullptr,
        10,
        &g_sceneEntityCreates,
        outNativeResource);
}

static WotbModResult WOTBMOD_CALL SceneGetActive(
    void*,
    void** outNativeResource) {
    return CreateObjectResource(
        WOTBMOD_RESOURCE_SCENE,
        nullptr,
        11,
        &g_sceneGets,
        outNativeResource);
}

static NativeResource* ValidObjectResource(
    void* nativeResource,
    WotbModResourceType type) {
    NativeResource* resource =
        static_cast<NativeResource*>(nativeResource);
    return resource &&
                   resource->magic == kNativeMagic &&
                   resource->type == type
               ? resource
               : nullptr;
}

static bool ConsumeObjectFault(LONG mode) {
    return InterlockedCompareExchange(
               &g_objectFaultMode, 0, mode) == mode;
}

static WotbModResult WOTBMOD_CALL UiSetGeometry(
    void*,
    void* nativeResource,
    const WotbModUiControlGeometry* geometry) {
    if (ConsumeObjectFault(1)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !geometry) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    resource->geometry = *geometry;
    InterlockedIncrement(&g_uiGeometrySets);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiSetVisible(
    void*,
    void* nativeResource,
    int32_t visible) {
    if (ConsumeObjectFault(2)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    resource->visible = visible != 0;
    InterlockedIncrement(&g_uiVisibilitySets);
    return WOTBMOD_OK;
}

static WotbModResult ObjectPair(
    void* parentNativeResource,
    void* childNativeResource,
    WotbModResourceType type,
    bool add,
    LONG* counter) {
    NativeResource* parent =
        ValidObjectResource(parentNativeResource, type);
    NativeResource* child =
        ValidObjectResource(childNativeResource, type);
    if (!parent || !child || parent == child) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (add) {
        for (uint32_t index = 0; index < parent->child_count; ++index) {
            if (parent->children[index] == child) {
                return WOTBMOD_ERROR_ALREADY_EXISTS;
            }
        }
        if (parent->child_count >= kMaxNativeChildren) {
            return WOTBMOD_ERROR_LIMIT_REACHED;
        }
        parent->children[parent->child_count++] = child;
        child->parent = parent;
    } else if (child->parent == parent) {
        bool found = false;
        for (uint32_t index = 0; index < parent->child_count; ++index) {
            if (parent->children[index] != child) continue;
            for (uint32_t move = index + 1;
                 move < parent->child_count;
                 ++move) {
                parent->children[move - 1] =
                    parent->children[move];
            }
            parent->children[--parent->child_count] = nullptr;
            found = true;
            break;
        }
        if (!found) return WOTBMOD_ERROR_NOT_FOUND;
        child->parent = nullptr;
    } else {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    InterlockedIncrement(counter);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiAddChild(
    void*,
    void* parentNativeResource,
    void* childNativeResource) {
    if (ConsumeObjectFault(3)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return ObjectPair(
        parentNativeResource,
        childNativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL,
        true,
        &g_uiChildAdds);
}

static WotbModResult WOTBMOD_CALL UiRemoveChild(
    void*,
    void* parentNativeResource,
    void* childNativeResource) {
    if (ConsumeObjectFault(4)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return ObjectPair(
        parentNativeResource,
        childNativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL,
        false,
        &g_uiChildRemoves);
}

static WotbModResult CloneUiResource(
    NativeResource* resource,
    void** outNativeResource) {
    if (!resource || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    return ResourceClone(
        nullptr, resource, outNativeResource);
}

static NativeResource* FindUiChildByName(
    NativeResource* root,
    const char* name,
    bool recursive) {
    if (!root || !name) return nullptr;
    for (uint32_t index = 0; index < root->child_count; ++index) {
        NativeResource* child = root->children[index];
        if (!child || child->magic != kNativeMagic) continue;
        if (strcmp(child->object_name, name) == 0) {
            return child;
        }
        if (recursive) {
            NativeResource* nested =
                FindUiChildByName(child, name, true);
            if (nested) return nested;
        }
    }
    return nullptr;
}

static WotbModResult WOTBMOD_CALL UiFindByName(
    void*,
    void* rootNativeResource,
    const char* name,
    int32_t recursive,
    void** outNativeResource) {
    NativeResource* root = ValidObjectResource(
        rootNativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!root || !name || !name[0] || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    NativeResource* found = strcmp(root->object_name, name) == 0
        ? root
        : FindUiChildByName(root, name, recursive != 0);
    if (!found) return WOTBMOD_ERROR_NOT_FOUND;
    return CloneUiResource(found, outNativeResource);
}

static WotbModResult WOTBMOD_CALL UiGetParent(
    void*,
    void* nativeResource,
    void** outNativeResource) {
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    if (!resource->parent) return WOTBMOD_ERROR_NOT_FOUND;
    return CloneUiResource(
        resource->parent, outNativeResource);
}

static WotbModResult WOTBMOD_CALL UiGetChildCount(
    void*,
    void* nativeResource,
    uint32_t* outCount) {
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outCount) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outCount = resource->child_count;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiGetChildAt(
    void*,
    void* nativeResource,
    uint32_t index,
    void** outNativeResource) {
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    if (index >= resource->child_count ||
        !resource->children[index]) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    return CloneUiResource(
        resource->children[index], outNativeResource);
}

static WotbModResult WOTBMOD_CALL UiGetState(
    void*,
    void* nativeResource,
    WotbModUiControlState* outState) {
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outState) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    ZeroMemory(outState, sizeof(*outState));
    outState->struct_size = sizeof(*outState);
    outState->geometry = resource->geometry;
    outState->geometry.struct_size =
        sizeof(outState->geometry);
    if (resource->visible) {
        outState->flags |= WOTBMOD_UI_CONTROL_VISIBLE;
    }
    if (resource->input_enabled) {
        outState->flags |= WOTBMOD_UI_CONTROL_INPUT_ENABLED;
    }
    if (resource->disabled) {
        outState->flags |= WOTBMOD_UI_CONTROL_DISABLED;
    }
    return WOTBMOD_OK;
}

static void SetUiFlagRecursive(
    NativeResource* resource,
    bool inputFlag,
    int32_t value,
    bool hierarchical) {
    if (!resource) return;
    if (inputFlag) {
        resource->input_enabled = value != 0;
    } else {
        resource->disabled = value != 0;
    }
    if (!hierarchical) return;
    for (uint32_t index = 0; index < resource->child_count; ++index) {
        SetUiFlagRecursive(
            resource->children[index],
            inputFlag,
            value,
            true);
    }
}

static WotbModResult WOTBMOD_CALL UiSetInputEnabled(
    void*,
    void* nativeResource,
    int32_t enabled,
    int32_t hierarchical) {
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    SetUiFlagRecursive(
        resource, true, enabled, hierarchical != 0);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiSetDisabled(
    void*,
    void* nativeResource,
    int32_t disabled,
    int32_t hierarchical) {
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    SetUiFlagRecursive(
        resource, false, disabled, hierarchical != 0);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SceneSetTransform(
    void*,
    void* nativeResource,
    const WotbModSceneTransform* transform) {
    if (ConsumeObjectFault(5)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* resource = ValidObjectResource(
        nativeResource, WOTBMOD_RESOURCE_SCENE);
    if (!resource || !transform) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    resource->transform = *transform;
    InterlockedIncrement(&g_sceneTransformSets);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SceneAddChild(
    void*,
    void* parentNativeResource,
    void* childNativeResource) {
    if (ConsumeObjectFault(6)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return ObjectPair(
        parentNativeResource,
        childNativeResource,
        WOTBMOD_RESOURCE_SCENE,
        true,
        &g_sceneChildAdds);
}

static WotbModResult WOTBMOD_CALL SceneRemoveChild(
    void*,
    void* parentNativeResource,
    void* childNativeResource) {
    if (ConsumeObjectFault(7)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return ObjectPair(
        parentNativeResource,
        childNativeResource,
        WOTBMOD_RESOURCE_SCENE,
        false,
        &g_sceneChildRemoves);
}

static bool ConsumeAudioFault(LONG mode) {
    return InterlockedCompareExchange(
               &g_audioFaultMode, 0, mode) == mode;
}

static NativePlayback* ValidPlayback(void* nativePlayback) {
    NativePlayback* playback =
        static_cast<NativePlayback*>(nativePlayback);
    return playback && playback->magic == kPlaybackMagic
               ? playback
               : nullptr;
}

static WotbModResult WOTBMOD_CALL AudioPlay(
    void*,
    void* nativeAudioClip,
    const WotbModAudioPlayInfo* playInfo,
    void** outNativePlayback) {
    if (ConsumeAudioFault(1)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeResource* clip =
        static_cast<NativeResource*>(nativeAudioClip);
    if (!clip ||
        clip->magic != kNativeMagic ||
        clip->type != WOTBMOD_RESOURCE_AUDIO_CLIP ||
        !playInfo ||
        playInfo->struct_size < sizeof(*playInfo) ||
        !outNativePlayback) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativePlayback = nullptr;
    g_lastAudioPlayInfo = *playInfo;
    if (ConsumeAudioFault(8)) return WOTBMOD_OK;

    NativePlayback* playback = static_cast<NativePlayback*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(NativePlayback)));
    if (!playback) return WOTBMOD_ERROR_PLATFORM;
    playback->magic = kPlaybackMagic;
    playback->clip = clip;
    playback->parameters = *playInfo;
    playback->state =
        (playInfo->flags & WOTBMOD_AUDIO_PLAY_START_PAUSED)
            ? WOTBMOD_AUDIO_PAUSED
            : WOTBMOD_AUDIO_PLAYING;
    TrackPlayback(playback);
    *outNativePlayback = playback;
    InterlockedIncrement(&g_audioPlays);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioPause(
    void*,
    void* nativePlayback) {
    if (ConsumeAudioFault(2)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativePlayback* playback = ValidPlayback(nativePlayback);
    if (!playback) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    playback->state = WOTBMOD_AUDIO_PAUSED;
    InterlockedIncrement(&g_audioPauses);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioResume(
    void*,
    void* nativePlayback) {
    if (ConsumeAudioFault(3)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativePlayback* playback = ValidPlayback(nativePlayback);
    if (!playback) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    playback->state = WOTBMOD_AUDIO_PLAYING;
    InterlockedIncrement(&g_audioResumes);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioStop(
    void*,
    void* nativePlayback) {
    if (ConsumeAudioFault(4)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativePlayback* playback = ValidPlayback(nativePlayback);
    if (!playback) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    playback->state = WOTBMOD_AUDIO_STOPPED;
    InterlockedIncrement(&g_audioStops);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioSetParameters(
    void*,
    void* nativePlayback,
    const WotbModAudioPlayInfo* parameters) {
    if (ConsumeAudioFault(5)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativePlayback* playback = ValidPlayback(nativePlayback);
    if (!playback ||
        !parameters ||
        parameters->struct_size < sizeof(*parameters)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    playback->parameters = *parameters;
    InterlockedIncrement(&g_audioParameterSets);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioGetState(
    void*,
    void* nativePlayback,
    WotbModAudioState* outState) {
    if (ConsumeAudioFault(6)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativePlayback* playback = ValidPlayback(nativePlayback);
    if (!playback || !outState) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outState = ConsumeAudioFault(9)
                    ? static_cast<WotbModAudioState>(99)
                    : playback->state;
    InterlockedIncrement(&g_audioStateQueries);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioRelease(
    void*,
    void* nativePlayback) {
    if (ConsumeAudioFault(7)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativePlayback* playback = ValidPlayback(nativePlayback);
    if (!playback || !UntrackPlayback(playback)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    playback->magic = 0;
    if (!HeapFree(GetProcessHeap(), 0, playback)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedIncrement(&g_audioReleases);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioClipLoad(
    void*,
    const char* resolvedFilePath,
    void** outNativeAudioClip) {
    if (!resolvedFilePath ||
        !outNativeAudioClip ||
        GetFileAttributesA(resolvedFilePath) == INVALID_FILE_ATTRIBUTES) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    *outNativeAudioClip = nullptr;
    NativeResource* resource = static_cast<NativeResource*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(NativeResource)));
    if (!resource) return WOTBMOD_ERROR_PLATFORM;
    resource->magic = kNativeMagic;
    resource->type = WOTBMOD_RESOURCE_AUDIO_CLIP;
    strncpy_s(
        resource->path,
        sizeof(resource->path),
        resolvedFilePath,
        _TRUNCATE);
    TrackNative(resource);
    *outNativeAudioClip = resource;
    InterlockedIncrement(&g_audioClipLoads);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioClipReload(
    void*,
    void* nativeAudioClip) {
    NativeResource* resource =
        static_cast<NativeResource*>(nativeAudioClip);
    if (!resource ||
        resource->magic != kNativeMagic ||
        resource->type != WOTBMOD_RESOURCE_AUDIO_CLIP) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    InterlockedIncrement(&g_audioClipReloads);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL AudioClipRelease(
    void*,
    void* nativeAudioClip) {
    NativeResource* resource =
        static_cast<NativeResource*>(nativeAudioClip);
    if (!resource ||
        resource->magic != kNativeMagic ||
        resource->type != WOTBMOD_RESOURCE_AUDIO_CLIP ||
        !UntrackNative(resource)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    resource->magic = 0;
    if (!HeapFree(GetProcessHeap(), 0, resource)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedIncrement(&g_audioClipReleases);
    return WOTBMOD_OK;
}

static bool ConsumeSoundFault(LONG mode) {
    return InterlockedCompareExchange(
               &g_soundFaultMode, 0, mode) == mode;
}

static NativeSoundEvent* ValidSoundEvent(void* nativeEvent) {
    NativeSoundEvent* event =
        static_cast<NativeSoundEvent*>(nativeEvent);
    return event && event->magic == kSoundEventMagic ? event : nullptr;
}

static WotbModResult WOTBMOD_CALL SoundEventCreate(
    void*,
    const char* eventName,
    void** outNativeEvent) {
    if (ConsumeSoundFault(1)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    if (!eventName || !eventName[0] || !outNativeEvent) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeEvent = nullptr;
    if (ConsumeSoundFault(12)) return WOTBMOD_OK;
    NativeSoundEvent* event = static_cast<NativeSoundEvent*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(NativeSoundEvent)));
    if (!event) return WOTBMOD_ERROR_LIMIT_REACHED;
    event->magic = kSoundEventMagic;
    event->state = WOTBMOD_AUDIO_STOPPED;
    event->volume = 1.0f;
    strncpy_s(
        event->name,
        sizeof(event->name),
        eventName,
        _TRUNCATE);
    TrackSoundEvent(event);
    InterlockedIncrement(&g_soundEventCreates);
    *outNativeEvent = event;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventTrigger(
    void*,
    void* nativeEvent) {
    if (ConsumeSoundFault(2)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    event->state = WOTBMOD_AUDIO_PLAYING;
    InterlockedIncrement(&g_soundEventTriggers);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventStop(
    void*,
    void* nativeEvent) {
    if (ConsumeSoundFault(3)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    event->state = WOTBMOD_AUDIO_STOPPED;
    InterlockedIncrement(&g_soundEventStops);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventSetPaused(
    void*,
    void* nativeEvent,
    int32_t paused) {
    if (ConsumeSoundFault(4)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event || (paused != 0 && paused != 1)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    event->state = paused ? WOTBMOD_AUDIO_PAUSED : WOTBMOD_AUDIO_PLAYING;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventSetVolume(
    void*,
    void* nativeEvent,
    float volume) {
    if (ConsumeSoundFault(5)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    event->volume = volume;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventSetPosition(
    void*,
    void* nativeEvent,
    float x,
    float y,
    float z) {
    if (ConsumeSoundFault(6)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    event->position[0] = x;
    event->position[1] = y;
    event->position[2] = z;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventGetState(
    void*,
    void* nativeEvent,
    WotbModAudioState* outState) {
    if (ConsumeSoundFault(7)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event || !outState) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outState = ConsumeSoundFault(13)
                    ? static_cast<WotbModAudioState>(99)
                    : event->state;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventSetParameter(
    void*,
    void* nativeEvent,
    const char* parameterName,
    float value) {
    if (ConsumeSoundFault(8)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event || !parameterName || !parameterName[0]) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    strncpy_s(
        event->parameter_name,
        sizeof(event->parameter_name),
        parameterName,
        _TRUNCATE);
    event->parameter_value = value;
    event->has_parameter = true;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventGetParameter(
    void*,
    void* nativeEvent,
    const char* parameterName,
    float* outValue) {
    if (ConsumeSoundFault(9)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event || !parameterName || !outValue) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!event->has_parameter ||
        _stricmp(event->parameter_name, parameterName) != 0) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    *outValue = event->parameter_value;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventHasParameter(
    void*,
    void* nativeEvent,
    const char* parameterName,
    int32_t* outHasParameter) {
    if (ConsumeSoundFault(10)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event || !parameterName || !outHasParameter) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outHasParameter =
        event->has_parameter &&
                _stricmp(event->parameter_name, parameterName) == 0
            ? 1
            : 0;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SoundEventRelease(
    void*,
    void* nativeEvent) {
    if (ConsumeSoundFault(11)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    NativeSoundEvent* event = ValidSoundEvent(nativeEvent);
    if (!event || !UntrackSoundEvent(event)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    event->magic = 0;
    if (!HeapFree(GetProcessHeap(), 0, event)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedIncrement(&g_soundEventReleases);
    return WOTBMOD_OK;
}

static void WOTBMOD_CALL RegistryChanged(
    void*,
    uint64_t generation) {
    InterlockedIncrement(&g_registryChanges);
    InterlockedExchange64(
        &g_lastRegistryGeneration,
        (LONG64)generation);
}

static BackendHook* FindBackendHook(void* target) {
    for (uint32_t index = 0; index < kMaxBackendHooks; ++index) {
        if (g_hooks[index].active && g_hooks[index].target == target) {
            return &g_hooks[index];
        }
    }
    return nullptr;
}

static WotbModResult WOTBMOD_CALL HookCreate(
    void*,
    void* target,
    void* detour,
    void** original) {
    if (!target || !detour || !original) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(&g_hookFailureMode, 0, 1) == 1) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    if (FindBackendHook(target)) return WOTBMOD_ERROR_ALREADY_EXISTS;
    for (uint32_t index = 0; index < kMaxBackendHooks; ++index) {
        if (g_hooks[index].active) continue;
        g_hooks[index].target = target;
        g_hooks[index].active = true;
        g_hooks[index].enabled = false;
        *original = target;
        InterlockedIncrement(&g_hookCreates);
        return WOTBMOD_OK;
    }
    return WOTBMOD_ERROR_LIMIT_REACHED;
}

static WotbModResult WOTBMOD_CALL HookEnable(
    void*,
    void* target) {
    if (InterlockedCompareExchange(&g_hookFailureMode, 0, 2) == 2) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    BackendHook* hook = FindBackendHook(target);
    if (!hook) return WOTBMOD_ERROR_NOT_FOUND;
    hook->enabled = true;
    InterlockedIncrement(&g_hookEnables);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HookDisable(
    void*,
    void* target) {
    if (InterlockedCompareExchange(&g_hookFailureMode, 0, 3) == 3) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    BackendHook* hook = FindBackendHook(target);
    if (!hook) return WOTBMOD_ERROR_NOT_FOUND;
    hook->enabled = false;
    InterlockedIncrement(&g_hookDisables);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HookRemove(
    void*,
    void* target) {
    if (InterlockedCompareExchange(&g_hookFailureMode, 0, 4) == 4) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    BackendHook* hook = FindBackendHook(target);
    if (!hook) return WOTBMOD_ERROR_NOT_FOUND;
    hook->target = nullptr;
    hook->active = false;
    hook->enabled = false;
    InterlockedIncrement(&g_hookRemoves);
    return WOTBMOD_OK;
}

static void WOTBMOD_CALL DummyDetour(void) {}

template <typename T>
static T GetExport(HMODULE module, const char* name) {
    return reinterpret_cast<T>(
        module && name ? GetProcAddress(module, name) : nullptr);
}

static bool LoadContractExports(
    ContractExports* exports,
    WotbModHandle* peerHandle) {
    if (!exports || !peerHandle) return false;
    ZeroMemory(exports, sizeof(*exports));
    *peerHandle = nullptr;
    HMODULE contract = GetModuleHandleA("api_contract_mod.dll");
    HMODULE peer = GetModuleHandleA("api_peer_mod.dll");
    if (!Expect(contract != nullptr, "contract DLL loaded") ||
        !Expect(peer != nullptr, "peer DLL loaded")) {
        return false;
    }
    exports->get_handle =
        GetExport<decltype(exports->get_handle)>(
            contract, "WotbTestContract_GetHandle");
    exports->get_host =
        GetExport<decltype(exports->get_host)>(
            contract, "WotbTestContract_GetHost");
    exports->get_enable_count =
        GetExport<decltype(exports->get_enable_count)>(
            contract, "WotbTestContract_GetEnableCount");
    exports->get_disable_count =
        GetExport<decltype(exports->get_disable_count)>(
            contract, "WotbTestContract_GetDisableCount");
    exports->get_unload_count =
        GetExport<decltype(exports->get_unload_count)>(
            contract, "WotbTestContract_GetUnloadCount");
    exports->get_frame_count =
        GetExport<decltype(exports->get_frame_count)>(
            contract, "WotbTestContract_GetFrameCount");
    exports->copy_last_frame =
        GetExport<decltype(exports->copy_last_frame)>(
            contract, "WotbTestContract_CopyLastFrame");
    auto getPeer = GetExport<WotbModHandle(WOTBMOD_CALL*)(void)>(
        peer, "WotbTestPeer_GetHandle");
    const bool complete =
        exports->get_handle &&
        exports->get_host &&
        exports->get_enable_count &&
        exports->get_disable_count &&
        exports->get_unload_count &&
        exports->get_frame_count &&
        exports->copy_last_frame &&
        getPeer;
    Expect(complete, "contract test exports");
    if (!complete) return false;
    *peerHandle = getPeer();
    return Expect(
        exports->get_handle() != nullptr && *peerHandle != nullptr,
        "contract and peer handles");
}

static WotbModRuntimeHookBackend MakeHookBackend() {
    WotbModRuntimeHookBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.create = &HookCreate;
    backend.enable = &HookEnable;
    backend.disable = &HookDisable;
    backend.remove = &HookRemove;
    return backend;
}

static WotbModRuntimeResourceBackend MakeResourceBackend(bool withReload) {
    WotbModRuntimeResourceBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.load = &ResourceLoad;
    backend.reload = withReload ? &ResourceReload : nullptr;
    backend.release = &ResourceRelease;
    backend.registry_changed = &RegistryChanged;
    backend.ui_set_geometry = &UiSetGeometry;
    backend.ui_set_visible = &UiSetVisible;
    backend.ui_add_child = &UiAddChild;
    backend.ui_remove_child = &UiRemoveChild;
    backend.scene_set_transform = &SceneSetTransform;
    backend.scene_add_child = &SceneAddChild;
    backend.scene_remove_child = &SceneRemoveChild;
    backend.ui_create = &UiCreate;
    backend.ui_get_active_screen = &UiGetActiveScreen;
    backend.scene_entity_create = &SceneEntityCreate;
    backend.scene_get_active = &SceneGetActive;
    backend.clone = &ResourceClone;
    backend.ui_find_by_name = &UiFindByName;
    backend.ui_get_parent = &UiGetParent;
    backend.ui_get_child_count = &UiGetChildCount;
    backend.ui_get_child_at = &UiGetChildAt;
    backend.ui_get_state = &UiGetState;
    backend.ui_set_input_enabled = &UiSetInputEnabled;
    backend.ui_set_disabled = &UiSetDisabled;
    return backend;
}

static WotbModRuntimeAudioBackend MakeAudioBackend(
    bool fullBackend,
    bool customAudioFiles = false) {
    WotbModRuntimeAudioBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.play = &AudioPlay;
    backend.pause = fullBackend ? &AudioPause : nullptr;
    backend.resume = fullBackend ? &AudioResume : nullptr;
    backend.stop = fullBackend ? &AudioStop : nullptr;
    backend.set_parameters =
        fullBackend ? &AudioSetParameters : nullptr;
    backend.get_state = fullBackend ? &AudioGetState : nullptr;
    backend.release = &AudioRelease;
    backend.load_clip =
        customAudioFiles ? &AudioClipLoad : nullptr;
    backend.reload_clip =
        customAudioFiles ? &AudioClipReload : nullptr;
    backend.release_clip =
        customAudioFiles ? &AudioClipRelease : nullptr;
    return backend;
}

static WotbModRuntimeSoundBackend MakeSoundBackend(bool fullBackend) {
    WotbModRuntimeSoundBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.create = &SoundEventCreate;
    backend.trigger = fullBackend ? &SoundEventTrigger : nullptr;
    backend.stop = fullBackend ? &SoundEventStop : nullptr;
    backend.set_paused = fullBackend ? &SoundEventSetPaused : nullptr;
    backend.set_volume = fullBackend ? &SoundEventSetVolume : nullptr;
    backend.set_position = fullBackend ? &SoundEventSetPosition : nullptr;
    backend.get_state = fullBackend ? &SoundEventGetState : nullptr;
    backend.set_parameter =
        fullBackend ? &SoundEventSetParameter : nullptr;
    backend.get_parameter =
        fullBackend ? &SoundEventGetParameter : nullptr;
    backend.has_parameter =
        fullBackend ? &SoundEventHasParameter : nullptr;
    backend.release = &SoundEventRelease;
    return backend;
}

static void FillTestVehicleInfo(
    uint32_t entityId,
    WotbModVehicleInfo* outInfo) {
    ZeroMemory(outInfo, sizeof(*outInfo));
    outInfo->struct_size = sizeof(*outInfo);
    outInfo->entity_id = entityId;
    outInfo->team = entityId == 101u ? 1u : 2u;
    outInfo->flags = WOTBMOD_VEHICLE_ALIVE;
    if (entityId == 101u) {
        outInfo->flags |= WOTBMOD_VEHICLE_LOCAL;
        outInfo->health = 900;
        outInfo->max_health = 1000;
        strcpy_s(outInfo->player_name, "LocalTester");
        strcpy_s(outInfo->vehicle_name, "TestTankLocal");
    } else {
        outInfo->health = 700;
        outInfo->max_health = 800;
        strcpy_s(outInfo->player_name, "RemoteTester");
        strcpy_s(outInfo->vehicle_name, "TestTankRemote");
    }
}

static bool ConsumeGameplayFault(LONG mode) {
    return InterlockedCompareExchange(
               &g_gameplayFaultMode, 0, mode) == mode;
}

static WotbModResult AllocateVehicleToken(
    uint32_t entityId,
    void** outToken) {
    if (!outToken ||
        (entityId != 101u && entityId != 202u)) {
        return outToken
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outToken = nullptr;
    TestVehicleToken* token =
        static_cast<TestVehicleToken*>(HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(TestVehicleToken)));
    if (!token) return WOTBMOD_ERROR_LIMIT_REACHED;
    token->magic = kVehicleTokenMagic;
    FillTestVehicleInfo(entityId, &token->info);
    *outToken = token;
    InterlockedIncrement(&g_vehicleTokensActive);
    InterlockedIncrement(&g_vehicleTokensCreated);
    return WOTBMOD_OK;
}

static TestVehicleToken* ValidVehicleToken(void* value) {
    TestVehicleToken* token =
        static_cast<TestVehicleToken*>(value);
    return token && token->magic == kVehicleTokenMagic
               ? token
               : nullptr;
}

static WotbModResult WOTBMOD_CALL VehicleGetLocal(
    void*,
    void** outToken) {
    if (ConsumeGameplayFault(1)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return AllocateVehicleToken(101u, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleGetByEntityId(
    void*,
    uint32_t entityId,
    void** outToken) {
    if (ConsumeGameplayFault(2)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return AllocateVehicleToken(entityId, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleGetCount(
    void*,
    uint32_t* outCount) {
    if (!outCount) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (ConsumeGameplayFault(3)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    *outCount = 2;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleGetAt(
    void*,
    uint32_t index,
    void** outToken) {
    if (ConsumeGameplayFault(4)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    if (index > 1u) {
        if (outToken) *outToken = nullptr;
        return outToken
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    return AllocateVehicleToken(
        index == 0u ? 101u : 202u,
        outToken);
}

static WotbModResult WOTBMOD_CALL VehicleClone(
    void*,
    void* value,
    void** outToken) {
    TestVehicleToken* source = ValidVehicleToken(value);
    if (!source || !outToken) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (ConsumeGameplayFault(5)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    return AllocateVehicleToken(
        source->info.entity_id, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleRelease(
    void*,
    void* value) {
    TestVehicleToken* token = ValidVehicleToken(value);
    if (!token) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    token->magic = 0;
    const BOOL released =
        HeapFree(GetProcessHeap(), 0, token);
    if (!released) return WOTBMOD_ERROR_PLATFORM;
    InterlockedDecrement(&g_vehicleTokensActive);
    InterlockedIncrement(&g_vehicleTokensReleased);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleGetInfo(
    void*,
    void* value,
    WotbModVehicleInfo* outInfo) {
    TestVehicleToken* token = ValidVehicleToken(value);
    if (!token || !outInfo ||
        outInfo->struct_size <
            offsetof(WotbModVehicleInfo, health) +
                sizeof(outInfo->health)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (ConsumeGameplayFault(6)) {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }
    const uint32_t callerSize = outInfo->struct_size;
    const uint32_t copySize =
        callerSize < sizeof(token->info)
            ? callerSize
            : static_cast<uint32_t>(sizeof(token->info));
    memcpy(outInfo, &token->info, copySize);
    return callerSize < sizeof(token->info)
               ? WOTBMOD_ERROR_BUFFER_TOO_SMALL
               : WOTBMOD_OK;
}

static WotbModRuntimeGameplayBackend MakeGameplayBackend() {
    WotbModRuntimeGameplayBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.vehicle_get_local = &VehicleGetLocal;
    backend.vehicle_get_by_entity_id =
        &VehicleGetByEntityId;
    backend.vehicle_get_count = &VehicleGetCount;
    backend.vehicle_get_at = &VehicleGetAt;
    backend.vehicle_clone = &VehicleClone;
    backend.vehicle_release = &VehicleRelease;
    backend.vehicle_get_info = &VehicleGetInfo;
    return backend;
}

static WotbModResult InitializeRuntime(
    const char* gameDirectory,
    bool hookBackend,
    bool resourceBackend,
    bool resourceReload,
    bool audioBackend,
    bool fullAudioBackend,
    bool customAudioFiles = false,
    bool legacyAudioStruct = false,
    bool soundBackend = false,
    bool fullSoundBackend = true,
    bool legacyResourceStruct = false,
    bool gameplayBackend = false) {
    WotbModRuntimeHookBackend hooks = MakeHookBackend();
    WotbModRuntimeResourceBackend resources =
        MakeResourceBackend(resourceReload);
    WotbModRuntimeAudioBackend audio =
        MakeAudioBackend(fullAudioBackend, customAudioFiles);
    WotbModRuntimeSoundBackend sound =
        MakeSoundBackend(fullSoundBackend);
    WotbModRuntimeGameplayBackend gameplay =
        MakeGameplayBackend();
    if (legacyAudioStruct) {
        audio.struct_size =
            (uint32_t)offsetof(WotbModRuntimeAudioBackend, load_clip);
    }
    if (legacyResourceStruct) {
        resources.struct_size =
            (uint32_t)offsetof(
                WotbModRuntimeResourceBackend,
                ui_set_geometry);
    }
    WotbModRuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.game_directory = gameDirectory;
    options.game_module = GetModuleHandleA(nullptr);
    options.log_sink = &LogSink;
    options.hook_backend = hookBackend ? &hooks : nullptr;
    options.resource_backend = resourceBackend ? &resources : nullptr;
    options.audio_backend =
        (audioBackend || customAudioFiles) ? &audio : nullptr;
    options.sound_backend = soundBackend ? &sound : nullptr;
    options.gameplay_backend =
        gameplayBackend ? &gameplay : nullptr;
    options.flags =
        WOTBMOD_RUNTIME_OPTION_ALLOW_LEGACY_RAW_PROCESS_API;
    return WotbModRuntime_Initialize(&options);
}

static int FindModIndex(const WotbModHostApi* host, const char* id) {
    if (!host || !id) return -1;
    for (uint32_t index = 0; index < host->get_mod_count(); ++index) {
        WotbModPublicInfo info = {};
        info.struct_size = sizeof(info);
        if (host->get_mod_info(index, &info) == WOTBMOD_OK &&
            _stricmp(info.id, id) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static bool QueryPath(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModPath kind,
    char* output,
    uint32_t outputCapacity,
    const char* label) {
    uint32_t required = 0;
    bool ok = ExpectResult(
        host->get_path(mod, kind, nullptr, &required),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        label);
    ok = Expect(required > 1, "path required size") && ok;
    uint32_t shortSize = 1;
    char shortBuffer[1] = {};
    ok = ExpectResult(
             host->get_path(mod, kind, shortBuffer, &shortSize),
             WOTBMOD_ERROR_BUFFER_TOO_SMALL,
             "path short buffer") &&
         ok;
    ok = Expect(shortSize == required, "path short required update") && ok;
    uint32_t capacity = outputCapacity;
    ok = ExpectResult(
             host->get_path(mod, kind, output, &capacity),
             WOTBMOD_OK,
             "path exact call") &&
         ok;
    ok = Expect(capacity == required, "path success size") && ok;
    return ok;
}

static WotbModResult Mount(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const char* root,
    const char* source,
    int32_t priority,
    uint32_t flags,
    WotbModResourceMountId* id) {
    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = root;
    mount.source_directory = source;
    mount.priority = priority;
    mount.flags = flags;
    return host->resource_mount(mod, &mount, id);
}

static WotbModResult LoadResource(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModResourceType type,
    const char* path,
    const char* objectName,
    uint32_t flags,
    WotbModResourceHandle* resource) {
    WotbModResourceLoadRequest request = {};
    request.struct_size = sizeof(request);
    request.type = type;
    request.virtual_path = path;
    request.object_name = objectName;
    request.flags = flags;
    return host->resource_load(mod, &request, resource);
}

static DWORD WINAPI ResolveWorker(void* parameter) {
    ResolveThreadContext* context =
        static_cast<ResolveThreadContext*>(parameter);
    for (uint32_t iteration = 0; iteration < 500; ++iteration) {
        char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
        uint32_t size = sizeof(resolved);
        if (WotbModRuntime_ResolveResourcePath(
                context->path,
                resolved,
                &size) != WOTBMOD_OK ||
            strstr(resolved, "\\high\\value.txt") == nullptr) {
            InterlockedIncrement(&context->failures);
        }
    }
    return 0;
}

static void WOTBMOD_CALL MainThreadProbe(
    const WotbModHostApi*,
    WotbModHandle,
    void* userData) {
    const LONG slot =
        InterlockedIncrement(&g_mainThreadCallbackOrderCount) - 1;
    if (slot >= 0 &&
        slot < (LONG)(sizeof(g_mainThreadCallbackOrder) /
                      sizeof(g_mainThreadCallbackOrder[0]))) {
        g_mainThreadCallbackOrder[slot] =
            static_cast<int32_t>(
                reinterpret_cast<intptr_t>(userData));
    }
    InterlockedIncrement(&g_mainThreadCallbackCount);
}

static void WOTBMOD_CALL FaultingMainThreadProbe(
    const WotbModHostApi*,
    WotbModHandle,
    void*) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
}

static void WOTBMOD_CALL ClientEventCallback(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void* userData) {
    ClientEventProbe* probe =
        static_cast<ClientEventProbe*>(userData);
    if (!host || !probe || !event) return;
    const uint32_t index = probe->callback_count++;
    if (index < sizeof(probe->types) / sizeof(probe->types[0])) {
        probe->types[index] = event->type;
        probe->sequences[index] = event->sequence;
    }
    probe->structures_valid =
        probe->structures_valid &&
        event->struct_size == sizeof(*event) &&
        event->sequence != 0 &&
        (event->type & WOTBMOD_EVENT_ALL) != 0;
    probe->main_thread_only =
        probe->main_thread_only &&
        GetCurrentThreadId() == probe->expected_thread_id;
    const bool payloadValid =
        (event->type == WOTBMOD_EVENT_UI_SCREEN_CHANGED &&
         event->previous_resource &&
         event->resource) ||
        (event->type == WOTBMOD_EVENT_SCENE_ACTIVATED &&
         !event->previous_resource &&
         event->resource) ||
        (event->type == WOTBMOD_EVENT_SCENE_DEACTIVATED &&
         event->previous_resource &&
         !event->resource);
    probe->payloads_valid =
        probe->payloads_valid && payloadValid;

    if (event->type == WOTBMOD_EVENT_UI_SCREEN_CHANGED &&
        event->resource &&
        !probe->stale_borrowed_resource) {
        probe->stale_borrowed_resource = event->resource;
        probe->borrowed_release_result =
            host->resource_release(mod, event->resource);
        probe->borrowed_reload_result =
            host->resource_reload(mod, event->resource);
        probe->clone_result =
            host->resource_clone(
                mod,
                event->resource,
                &probe->cloned_resource);
        probe->mutation_result =
            host->ui_control_set_visible(
                mod,
                event->resource,
                0);
    }
}

static uint32_t ExpectedPrimaryEntity(uint32_t type) {
    switch (type) {
        case WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED:
        case WOTBMOD_EVENT_VEHICLE_SPAWNED:
        case WOTBMOD_EVENT_SHOT_FIRED:
        case WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED:
        case WOTBMOD_EVENT_VEHICLE_DAMAGED:
        case WOTBMOD_EVENT_RELOAD_STATE_CHANGED:
            return 101u;
        case WOTBMOD_EVENT_VEHICLE_DESPAWNED:
        case WOTBMOD_EVENT_VEHICLE_DESTROYED:
            return 202u;
        default:
            return 0;
    }
}

static bool GameplayPayloadValid(
    const WotbModClientEvent* event) {
    if (!event) return false;
    switch (event->type) {
        case WOTBMOD_EVENT_BATTLE_ENTERED:
            return event->payload_size ==
                       sizeof(WotbModBattleEventData) &&
                   event->payload.battle.state == 1u;
        case WOTBMOD_EVENT_BATTLE_STARTED:
            return event->payload_size ==
                       sizeof(WotbModBattleEventData) &&
                   event->payload.battle.state == 2u;
        case WOTBMOD_EVENT_BATTLE_ENDED:
            return event->payload_size ==
                       sizeof(WotbModBattleEventData) &&
                   event->payload.battle.state == 3u;
        case WOTBMOD_EVENT_BATTLE_LEFT:
            return event->payload_size ==
                       sizeof(WotbModBattleEventData) &&
                   event->payload.battle.state == 0u;
        case WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED:
            return event->payload_size ==
                       sizeof(WotbModVehicleEventData) &&
                   event->payload.vehicle.entity_id == 101u &&
                   event->payload.vehicle.other_entity_id == 202u;
        case WOTBMOD_EVENT_VEHICLE_SPAWNED:
            return event->payload_size ==
                       sizeof(WotbModVehicleEventData) &&
                   event->payload.vehicle.entity_id == 101u &&
                   event->payload.vehicle.health == 900;
        case WOTBMOD_EVENT_VEHICLE_DESPAWNED:
            return event->payload_size ==
                       sizeof(WotbModVehicleEventData) &&
                   event->payload.vehicle.entity_id == 202u;
        case WOTBMOD_EVENT_SHOT_FIRED:
            return event->payload_size ==
                       sizeof(WotbModShotEventData) &&
                   event->payload.shot.shot_code == 7u &&
                   event->payload.shot.direction.z == 1.0f;
        case WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED:
            return event->payload_size ==
                       sizeof(WotbModVehicleEventData) &&
                   event->payload.vehicle.previous_health == 900 &&
                   event->payload.vehicle.health == 700;
        case WOTBMOD_EVENT_VEHICLE_DAMAGED:
            return event->payload_size ==
                       sizeof(WotbModDamageEventData) &&
                   event->payload.damage.damage == 200 &&
                   event->payload.damage.source_entity_id == 202u;
        case WOTBMOD_EVENT_VEHICLE_DESTROYED:
            return event->payload_size ==
                       sizeof(WotbModVehicleEventData) &&
                   event->payload.vehicle.health == 0 &&
                   (event->payload.vehicle.flags &
                    WOTBMOD_VEHICLE_DESTROYED) != 0;
        case WOTBMOD_EVENT_RELOAD_STATE_CHANGED:
            return event->payload_size ==
                       sizeof(WotbModReloadEventData) &&
                   event->payload.reload.state == 2u &&
                   event->payload.reload.progress == 0.5f &&
                   event->payload.reload.remaining_seconds == 1.5f;
        case WOTBMOD_EVENT_CAMERA_MODE_CHANGED:
            return event->payload_size ==
                       sizeof(WotbModCameraEventData) &&
                   event->payload.camera.previous_mode ==
                       WOTBMOD_V3_CAMERA_MODE_ARCADE &&
                   event->payload.camera.mode ==
                       WOTBMOD_V3_CAMERA_MODE_SNIPER &&
                   event->payload.camera.native_mode == 1;
        default:
            return false;
    }
}

static void WOTBMOD_CALL GameplayEventCallback(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void* userData) {
    GameplayEventProbe* probe =
        static_cast<GameplayEventProbe*>(userData);
    if (!host || !mod || !event || !probe) return;
    const uint32_t index = probe->callback_count++;
    if (index < sizeof(probe->types) / sizeof(probe->types[0])) {
        probe->types[index] = event->type;
        probe->sequences[index] = event->sequence;
    }
    probe->structures_valid =
        probe->structures_valid &&
        event->struct_size == sizeof(*event) &&
        event->sequence != 0;
    probe->main_thread_only =
        probe->main_thread_only &&
        GetCurrentThreadId() == probe->expected_thread_id;
    probe->payloads_valid =
        probe->payloads_valid && GameplayPayloadValid(event);

    const uint32_t expectedEntity =
        ExpectedPrimaryEntity(event->type);
    if (expectedEntity == 0) {
        probe->vehicles_valid =
            probe->vehicles_valid &&
            !event->vehicle &&
            !event->other_vehicle;
        return;
    }

    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    const WotbModResult infoResult =
        host->vehicle_get_info(mod, event->vehicle, &info);
    probe->vehicles_valid =
        probe->vehicles_valid &&
        event->vehicle &&
        infoResult == WOTBMOD_OK &&
        info.entity_id == expectedEntity;

    if (event->type ==
        WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED) {
        WotbModVehicleInfo otherInfo = {};
        otherInfo.struct_size = sizeof(otherInfo);
        probe->vehicles_valid =
            probe->vehicles_valid &&
            event->other_vehicle &&
            host->vehicle_get_info(
                mod,
                event->other_vehicle,
                &otherInfo) == WOTBMOD_OK &&
            otherInfo.entity_id == 202u;
    } else {
        probe->vehicles_valid =
            probe->vehicles_valid &&
            !event->other_vehicle;
    }

    if (!probe->stale_borrowed_vehicle) {
        probe->stale_borrowed_vehicle = event->vehicle;
        probe->borrowed_release_result =
            host->vehicle_release(mod, event->vehicle);
        probe->clone_result =
            host->vehicle_clone(
                mod,
                event->vehicle,
                &probe->cloned_vehicle);
    }
}

static void WOTBMOD_CALL FaultingClientEventCallback(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModClientEvent*,
    void*) {
    InterlockedIncrement(&g_faultingEventCallbackCount);
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
}

static DWORD WINAPI ObjectWorker(void* parameter) {
    ObjectWorkerContext* context =
        static_cast<ObjectWorkerContext*>(parameter);
    if (!context || !context->host) return 1;
    context->visible_result =
        context->host->ui_control_set_visible(
            context->owner,
            context->control,
            1);
    context->transform_result =
        context->host->scene_set_transform(
            context->owner,
            context->scene,
            &context->transform);
    return 0;
}

static DWORD WINAPI FactoryWorker(void* parameter) {
    FactoryWorkerContext* context =
        static_cast<FactoryWorkerContext*>(parameter);
    if (!context || !context->host) return 1;
    context->ui_create_result =
        context->host->ui_control_create(
            context->owner, nullptr, &context->control);
    context->scene_get_result =
        context->host->scene_get_active(
            context->owner, &context->scene);
    return 0;
}

static DWORD WINAPI VehicleWorker(void* parameter) {
    VehicleWorkerContext* context =
        static_cast<VehicleWorkerContext*>(parameter);
    if (!context || !context->host) return 1;
    context->count_result =
        context->host->vehicle_get_count(
            context->owner, &context->count);
    context->local_result =
        context->host->vehicle_get_local(
            context->owner, &context->vehicle);
    return 0;
}

static void TestPreInitialize(const char* apiEnvironment, const char* executable) {
    const WotbModHostApi* host = WotbModRuntime_GetHostApi();
    Expect(host != nullptr, "GetHostApi before initialize");
    Expect(host == WotbModApi_GetHost(), "optional host export identity");
    Expect(WotbModApi_GetVersion() == WOTBMOD_ABI_VERSION, "version export");
    Expect(
        host &&
            host->struct_size == sizeof(*host) &&
            host->abi_version == WOTBMOD_ABI_VERSION &&
            host->host_version == WOTBMOD_HOST_VERSION &&
            host->log &&
            host->get_path &&
            host->get_game_module &&
            host->resolve_rva &&
            host->get_proc_address &&
            host->find_pattern &&
            host->hook_create &&
            host->hook_enable &&
            host->hook_disable &&
            host->hook_remove &&
            host->config_get_int &&
            host->config_set_int &&
            host->config_get_string &&
            host->config_set_string &&
            host->get_mod_count &&
            host->get_mod_info &&
            host->set_mod_enabled &&
            host->resource_mount &&
            host->resource_unmount &&
            host->resource_resolve &&
            host->resource_load &&
            host->resource_reload &&
            host->resource_release &&
            host->audio_play &&
            host->audio_pause &&
            host->audio_resume &&
            host->audio_stop &&
            host->audio_set_parameters &&
            host->audio_get_state &&
            host->audio_release &&
            host->audio_clip_load &&
            host->sound_event_create &&
            host->sound_event_trigger &&
            host->sound_event_stop &&
            host->sound_event_set_paused &&
            host->sound_event_set_volume &&
            host->sound_event_set_position &&
            host->sound_event_get_state &&
            host->sound_event_set_parameter &&
            host->sound_event_get_parameter &&
            host->sound_event_has_parameter &&
            host->sound_event_get_name &&
            host->sound_event_release &&
            host->main_thread_enqueue &&
            host->ui_control_set_geometry &&
            host->ui_control_set_visible &&
            host->ui_control_add_child &&
            host->ui_control_remove_child &&
            host->scene_set_transform &&
            host->scene_add_child &&
            host->scene_remove_child &&
            host->ui_control_create &&
            host->ui_get_active_screen &&
            host->scene_entity_create &&
            host->scene_get_active &&
            host->resource_clone &&
            host->event_subscribe &&
            host->event_unsubscribe &&
            host->ui_control_find_by_name &&
            host->ui_control_get_parent &&
            host->ui_control_get_child_count &&
            host->ui_control_get_child_at &&
            host->ui_control_get_state &&
            host->ui_control_set_input_enabled &&
            host->ui_control_set_disabled &&
            host->vehicle_get_local &&
            host->vehicle_get_by_entity_id &&
            host->vehicle_get_count &&
            host->vehicle_get_at &&
            host->vehicle_clone &&
            host->vehicle_release &&
            host->vehicle_get_info,
        "complete host function table");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_ERROR_DISABLED,
        "LoadAll before initialize");
    uint32_t size = 0;
    ExpectResult(
        WotbModRuntime_ResolveResourcePath("~res:/none", nullptr, &size),
        WOTBMOD_ERROR_DISABLED,
        "resolve before initialize");
    Expect(
        WotbModRuntime_GetResourceGeneration() == 0,
        "generation before initialize");
    ExpectResult(
        WotbModRuntime_NotifyUiScreenChanged(
            reinterpret_cast<void*>(1),
            nullptr),
        WOTBMOD_ERROR_DISABLED,
        "UI event ingress before initialize");
    ExpectResult(
        WotbModRuntime_NotifySceneActivated(
            reinterpret_cast<void*>(1)),
        WOTBMOD_ERROR_DISABLED,
        "scene activate ingress before initialize");
    ExpectResult(
        WotbModRuntime_NotifySceneDeactivated(
            reinterpret_cast<void*>(1)),
        WOTBMOD_ERROR_DISABLED,
        "scene deactivate ingress before initialize");
    WotbModRuntime_DispatchFrame(nullptr, nullptr, nullptr, 0, 0, 0.0);
    WotbModRuntime_Shutdown();
    ExpectResult(
        WotbModRuntime_Initialize(nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "initialize null options");

    WotbModRuntimeOptions shortOptions = {};
    shortOptions.struct_size =
        (uint32_t)offsetof(WotbModRuntimeOptions, hook_backend);
    ExpectResult(
        WotbModRuntime_Initialize(&shortOptions),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "initialize short options");

    WotbModRuntimeOptions fileAsDirectory = {};
    fileAsDirectory.struct_size = sizeof(fileAsDirectory);
    fileAsDirectory.game_directory = apiEnvironment;
    fileAsDirectory.mods_directory = executable;
    ExpectResult(
        WotbModRuntime_Initialize(&fileAsDirectory),
        WOTBMOD_ERROR_PLATFORM,
        "initialize rejects file as mods directory");
}

static void TestMetadataAndPaths(
    const WotbModHostApi* host,
    WotbModHandle contractHandle) {
    Expect(host->get_mod_count() == 5, "happy environment mod count");
    const char* expectedOrder[] = {
        "test.contract",
        "test.peer",
        "test.fault",
        "example.hello",
        "test.resources"};
    for (uint32_t index = 0; index < 5; ++index) {
        WotbModPublicInfo ordered = {};
        ordered.struct_size = sizeof(ordered);
        ExpectResult(
            host->get_mod_info(index, &ordered),
            WOTBMOD_OK,
            "alphabetical metadata");
        Expect(
            strcmp(ordered.id, expectedOrder[index]) == 0,
            "alphabetical DLL discovery");
    }
    ExpectResult(
        host->get_mod_info(0, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "get_mod_info null output");
    WotbModPublicInfo invalid = {};
    invalid.struct_size = 3;
    ExpectResult(
        host->get_mod_info(0, &invalid),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "get_mod_info tiny structure");
    WotbModPublicInfo outOfRange = {};
    outOfRange.struct_size = sizeof(outOfRange);
    ExpectResult(
        host->get_mod_info(host->get_mod_count(), &outOfRange),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "get_mod_info out of range");

    struct PartialInfo {
        uint32_t struct_size;
        uint32_t abi_version;
        uint32_t state;
    } partial = {};
    partial.struct_size = sizeof(partial);
    ExpectResult(
        host->get_mod_info(
            (uint32_t)FindModIndex(host, "test.contract"),
            reinterpret_cast<WotbModPublicInfo*>(&partial)),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "get_mod_info partial copy");
    Expect(
        partial.struct_size == sizeof(WotbModPublicInfo) &&
            partial.abi_version == WOTBMOD_ABI_VERSION &&
            partial.state == WOTBMOD_STATE_ENABLED,
        "get_mod_info partial values");

    const int contractIndex = FindModIndex(host, "test.contract");
    Expect(contractIndex >= 0, "contract metadata index");
    if (contractIndex >= 0) {
        WotbModPublicInfo info = {};
        info.struct_size = sizeof(info);
        ExpectResult(
            host->get_mod_info((uint32_t)contractIndex, &info),
            WOTBMOD_OK,
            "contract metadata");
        Expect(
            strcmp(info.name, "Full API Contract Test") == 0 &&
                strcmp(info.version, "2.9.0") == 0 &&
                strcmp(info.author, "SDK tests") == 0 &&
                info.flags == 0xA5u &&
                info.enabled == 1 &&
                info.state == WOTBMOD_STATE_ENABLED,
            "contract metadata fields");
    }

    char game[MAX_PATH] = {};
    char mods[MAX_PATH] = {};
    char module[MAX_PATH] = {};
    char data[MAX_PATH] = {};
    char config[MAX_PATH] = {};
    QueryPath(host, nullptr, WOTBMOD_PATH_GAME, game, sizeof(game), "game path");
    QueryPath(host, nullptr, WOTBMOD_PATH_MODS, mods, sizeof(mods), "mods path");
    QueryPath(
        host,
        contractHandle,
        WOTBMOD_PATH_MODULE,
        module,
        sizeof(module),
        "module path");
    QueryPath(
        host,
        contractHandle,
        WOTBMOD_PATH_DATA,
        data,
        sizeof(data),
        "data path");
    QueryPath(
        host,
        contractHandle,
        WOTBMOD_PATH_CONFIG,
        config,
        sizeof(config),
        "config path");
    Expect(strstr(module, "api_contract_mod.dll") != nullptr, "module path value");
    Expect(strstr(data, "\\data\\api_contract_mod") != nullptr, "data path value");
    Expect(
        strstr(config, "\\config\\api_contract_mod.ini") != nullptr,
        "config path value");
    uint32_t pathSize = 0;
    ExpectResult(
        host->get_path(
            reinterpret_cast<WotbModHandle>(1),
            WOTBMOD_PATH_MODULE,
            nullptr,
            &pathSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "module path invalid handle");
    ExpectResult(
        host->get_path(
            contractHandle,
            static_cast<WotbModPath>(99),
            nullptr,
            &pathSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "invalid path kind");
    ExpectResult(
        host->get_path(
            contractHandle,
            WOTBMOD_PATH_DATA,
            nullptr,
            nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "path null size");
}

static void TestSymbols(const WotbModHostApi* host) {
    HMODULE gameModule = GetModuleHandleA(nullptr);
    Expect(host->get_game_module() == gameModule, "get_game_module");
    Expect(host->resolve_rva(0) == gameModule, "resolve_rva base");
    Expect(host->resolve_rva(UINT32_MAX) == nullptr, "resolve_rva out of image");
    Expect(
        host->get_proc_address("kernel32.dll", "GetCurrentProcessId") != nullptr,
        "get_proc_address loaded module");
    Expect(
        host->get_proc_address(nullptr, "WotbModApi_GetVersion") != nullptr,
        "get_proc_address game export");
    Expect(
        host->get_proc_address("missing-wotb-test.dll", "x") == nullptr,
        "get_proc_address missing module");
    Expect(
        host->get_proc_address("kernel32.dll", nullptr) == nullptr &&
            host->get_proc_address("kernel32.dll", "") == nullptr,
        "get_proc_address invalid export");

    const char* exactMask = "xxxxxxxxxxxxxxxx";
    void* found = host->find_pattern(nullptr, g_imagePattern, exactMask);
    Expect(found != nullptr, "find_pattern exact");
    if (found) {
        Expect(
            memcmp(found, g_imagePattern, sizeof(g_imagePattern)) == 0,
            "find_pattern exact bytes");
    }
    uint8_t wildcardPattern[sizeof(g_imagePattern)] = {};
    memcpy(wildcardPattern, g_imagePattern, sizeof(wildcardPattern));
    wildcardPattern[4] ^= 0xFFu;
    Expect(
        host->find_pattern(
            nullptr,
            wildcardPattern,
            "xxxx?xxxxxxxxxxx") != nullptr,
        "find_pattern wildcard");
    Expect(
        host->find_pattern(nullptr, g_imagePattern, "xxxxzxxxxxxxxxxx") ==
            nullptr,
        "find_pattern rejects invalid mask");
    Expect(
        host->find_pattern(nullptr, nullptr, exactMask) == nullptr &&
            host->find_pattern(nullptr, g_imagePattern, nullptr) == nullptr &&
            host->find_pattern(nullptr, g_imagePattern, "") == nullptr &&
            host->find_pattern(
                "missing-wotb-test.dll",
                g_imagePattern,
                exactMask) == nullptr,
        "find_pattern invalid arguments");
}

static void TestConfig(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    Expect(
        host->config_get_int(nullptr, "qa", "number", 73) == 73 &&
            host->config_get_int(mod, nullptr, "number", 74) == 74 &&
            host->config_get_int(mod, "qa", nullptr, 75) == 75,
        "config_get_int invalid/default");
    ExpectResult(
        host->config_set_int(nullptr, "qa", "number", 1),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "config_set_int invalid handle");
    ExpectResult(
        host->config_set_int(mod, "qa", "number", -1234567),
        WOTBMOD_OK,
        "config_set_int");
    Expect(
        host->config_get_int(mod, "qa", "number", 0) == -1234567,
        "config_get_int roundtrip");
    ExpectResult(
        host->config_set_string(mod, "qa", "text", "abcdef"),
        WOTBMOD_OK,
        "config_set_string");
    char exact[7] = {};
    ExpectResult(
        host->config_get_string(
            mod, "qa", "text", "fallback", exact, sizeof(exact)),
        WOTBMOD_OK,
        "config_get_string exact fit");
    Expect(strcmp(exact, "abcdef") == 0, "config string exact content");
    char shortBuffer[4] = {};
    ExpectResult(
        host->config_get_string(
            mod,
            "qa",
            "text",
            "fallback",
            shortBuffer,
            sizeof(shortBuffer)),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "config_get_string short buffer");
    Expect(shortBuffer[sizeof(shortBuffer) - 1] == '\0', "config short NUL");
    char fallback[16] = {};
    ExpectResult(
        host->config_get_string(
            mod,
            "qa",
            "missing-key",
            "default",
            fallback,
            sizeof(fallback)),
        WOTBMOD_OK,
        "config default string");
    Expect(strcmp(fallback, "default") == 0, "config default content");
    ExpectResult(
        host->config_get_string(
            mod, "qa", "text", nullptr, nullptr, 0),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "config_get_string invalid buffer");
    ExpectResult(
        host->config_set_string(mod, "qa", "text", nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "config_set_string invalid value");
}

static void TestHooks(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    void* original = nullptr;
    void* target = &g_hookTargets[0];
    void* detour = reinterpret_cast<void*>(&DummyDetour);
    ExpectResult(
        host->hook_create(nullptr, target, detour, &original),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "hook_create invalid owner");
    ExpectResult(
        host->hook_create(owner, nullptr, detour, &original),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "hook_create null target");
    ExpectResult(
        host->hook_create(owner, target, nullptr, &original),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "hook_create null detour");
    ExpectResult(
        host->hook_create(owner, target, detour, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "hook_create null original");
    ExpectResult(
        host->hook_create(owner, target, detour, &original),
        WOTBMOD_OK,
        "hook_create");
    Expect(original == target, "hook original value");
    ExpectResult(
        host->hook_create(owner, target, detour, &original),
        WOTBMOD_ERROR_ALREADY_EXISTS,
        "hook_create duplicate");
    ExpectResult(
        host->hook_enable(peer, target),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "hook_enable foreign owner");
    ExpectResult(
        host->hook_disable(peer, target),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "hook_disable foreign owner");
    ExpectResult(
        host->hook_remove(peer, target),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "hook_remove foreign owner");
    ExpectResult(host->hook_enable(owner, target), WOTBMOD_OK, "hook_enable");
    ExpectResult(host->hook_disable(owner, target), WOTBMOD_OK, "hook_disable");
    ExpectResult(host->hook_remove(owner, target), WOTBMOD_OK, "hook_remove");
    ExpectResult(
        host->hook_remove(owner, target),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "hook_remove repeated");

    InterlockedExchange(&g_hookFailureMode, 1);
    ExpectResult(
        host->hook_create(owner, target, detour, &original),
        WOTBMOD_ERROR_PLATFORM,
        "hook_create backend error");
    ExpectResult(
        host->hook_create(owner, target, detour, &original),
        WOTBMOD_OK,
        "hook_create retry after backend error");
    InterlockedExchange(&g_hookFailureMode, 2);
    ExpectResult(
        host->hook_enable(owner, target),
        WOTBMOD_ERROR_PLATFORM,
        "hook_enable backend error");
    ExpectResult(
        host->hook_enable(owner, target),
        WOTBMOD_OK,
        "hook_enable retry after backend error");
    InterlockedExchange(&g_hookFailureMode, 3);
    ExpectResult(
        host->hook_disable(owner, target),
        WOTBMOD_ERROR_PLATFORM,
        "hook_disable backend error");
    ExpectResult(
        host->hook_disable(owner, target),
        WOTBMOD_OK,
        "hook_disable retry after backend error");
    InterlockedExchange(&g_hookFailureMode, 4);
    ExpectResult(
        host->hook_remove(owner, target),
        WOTBMOD_ERROR_PLATFORM,
        "hook_remove backend error");
    ExpectResult(
        host->hook_remove(owner, target),
        WOTBMOD_OK,
        "hook_remove retry after backend error");

    for (uint32_t index = 0; index < 80; ++index) {
        original = nullptr;
        if (!ExpectResult(
                host->hook_create(owner, target, detour, &original),
                WOTBMOD_OK,
                "hook slot churn create")) {
            break;
        }
        if (!ExpectResult(
                host->hook_remove(owner, target),
                WOTBMOD_OK,
                "hook slot churn remove")) {
            break;
        }
    }

    for (uint32_t index = 0; index < 64; ++index) {
        original = nullptr;
        ExpectResult(
            host->hook_create(
                owner,
                &g_hookTargets[index],
                detour,
                &original),
            WOTBMOD_OK,
            "hook concurrent limit fill");
    }
    ExpectResult(
        host->hook_create(
            owner,
            &g_hookTargets[64],
            detour,
            &original),
        WOTBMOD_ERROR_LIMIT_REACHED,
        "hook concurrent limit");
    for (uint32_t index = 0; index < 64; ++index) {
        ExpectResult(
            host->hook_remove(owner, &g_hookTargets[index]),
            WOTBMOD_OK,
            "hook concurrent cleanup");
    }
}

static void TestResourceMounts(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    WotbModResourceMountId id = 0;
    WotbModResourceMountInfo invalid = {};
    invalid.struct_size = sizeof(invalid);
    invalid.virtual_root = "~res:/Contract/";
    invalid.source_directory = "low";
    ExpectResult(
        host->resource_mount(nullptr, &invalid, &id),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_mount invalid owner");
    invalid.struct_size =
        (uint32_t)offsetof(WotbModResourceMountInfo, flags);
    ExpectResult(
        host->resource_mount(owner, &invalid, &id),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_mount short structure");
    invalid.struct_size = sizeof(invalid);
    invalid.flags = 0x80000000u;
    ExpectResult(
        host->resource_mount(owner, &invalid, &id),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_mount invalid flags");
    invalid.flags = 0;
    invalid.virtual_root = "~res:/../escape";
    ExpectResult(
        host->resource_mount(owner, &invalid, &id),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_mount virtual traversal");
    invalid.virtual_root = "~res:/Contract/";
    invalid.source_directory = "../escape";
    ExpectResult(
        host->resource_mount(owner, &invalid, &id),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_mount source traversal");
    invalid.source_directory = "C:\\escape";
    ExpectResult(
        host->resource_mount(owner, &invalid, &id),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_mount absolute source");
    invalid.source_directory = "missing";
    ExpectResult(
        host->resource_mount(owner, &invalid, &id),
        WOTBMOD_ERROR_NOT_FOUND,
        "resource_mount missing directory");

    WotbModResourceMountId low = 0;
    WotbModResourceMountId high = 0;
    WotbModResourceMountId tie = 0;
    ExpectResult(
        Mount(
            host,
            owner,
            "Data/Contract/Priority",
            "low",
            10,
            WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL,
            &low),
        WOTBMOD_OK,
        "resource_mount low");
    ExpectResult(
        Mount(
            host,
            owner,
            "res:/Contract/Priority/",
            "high",
            20,
            WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL,
            &high),
        WOTBMOD_OK,
        "resource_mount high");
    Expect(low != 0 && high > low, "resource mount ids");
    ExpectResult(
        host->resource_unmount(peer, high),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "resource_unmount foreign");
    ExpectResult(
        host->resource_unmount(owner, 0),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_unmount zero");

    uint32_t required = 0;
    ExpectResult(
        host->resource_resolve(
            owner,
            "~res:/Contract/Priority/value.txt",
            nullptr,
            &required),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "resource_resolve size query");
    Expect(required > 1, "resource_resolve required size");
    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t resolvedSize = sizeof(resolved);
    ExpectResult(
        host->resource_resolve(
            owner,
            "~res:/Contract/Priority/value.txt",
            resolved,
            &resolvedSize),
        WOTBMOD_OK,
        "resource_resolve high priority");
    Expect(strstr(resolved, "\\high\\value.txt") != nullptr, "resource high path");
    ResolveThreadContext threadContext = {
        "~res:/Contract/Priority/value.txt",
        0};
    HANDLE threads[4] = {};
    for (uint32_t index = 0; index < 4; ++index) {
        threads[index] = CreateThread(
            nullptr, 0, &ResolveWorker, &threadContext, 0, nullptr);
        Expect(threads[index] != nullptr, "parallel resolver thread create");
    }
    if (threads[0] && threads[1] && threads[2] && threads[3]) {
        Expect(
            WaitForMultipleObjects(4, threads, TRUE, 10000) ==
                WAIT_OBJECT_0,
            "parallel resolver threads complete");
    } else {
        for (uint32_t index = 0; index < 4; ++index) {
            if (threads[index]) {
                Expect(
                    WaitForSingleObject(threads[index], 10000) ==
                        WAIT_OBJECT_0,
                    "partial resolver thread completion");
            }
        }
    }
    for (uint32_t index = 0; index < 4; ++index) {
        if (threads[index]) CloseHandle(threads[index]);
    }
    Expect(threadContext.failures == 0, "parallel resolver consistency");
    uint32_t runtimeSize = sizeof(resolved);
    ExpectResult(
        WotbModRuntime_ResolveResourcePath(
            "Data/Contract/Priority/value.txt.dvpl",
            resolved,
            &runtimeSize),
        WOTBMOD_OK,
        "runtime resolver loose fallback");
    Expect(strstr(resolved, "\\high\\value.txt") != nullptr, "loose fallback path");
    uint32_t shortSize = 2;
    char shortBuffer[2] = {};
    ExpectResult(
        host->resource_resolve(
            owner,
            "Contract\\Priority\\value.txt",
            shortBuffer,
            &shortSize),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "resource_resolve short buffer");
    Expect(shortSize == required, "resource_resolve short required");
    ExpectResult(
        host->resource_resolve(
            reinterpret_cast<WotbModHandle>(1),
            "~res:/Contract/Priority/value.txt",
            resolved,
            &resolvedSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_resolve invalid owner");
    ExpectResult(
        host->resource_resolve(
            owner,
            "~res:/../escape",
            resolved,
            &resolvedSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_resolve traversal");
    ExpectResult(
        host->resource_resolve(
            owner,
            "C:\\outside\\asset.yaml",
            resolved,
            &resolvedSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_resolve foreign absolute path");
    char oversized[WOTBMOD_MAX_RESOURCE_PATH + 32] = {};
    memset(oversized, 'a', sizeof(oversized) - 1);
    ExpectResult(
        host->resource_resolve(owner, oversized, resolved, &resolvedSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_resolve oversized path");
    char gamePath[MAX_PATH] = {};
    uint32_t gamePathSize = sizeof(gamePath);
    if (host->get_path(
            owner,
            WOTBMOD_PATH_GAME,
            gamePath,
            &gamePathSize) == WOTBMOD_OK) {
        char absoluteResource[WOTBMOD_MAX_RESOURCE_PATH] = {};
        _snprintf_s(
            absoluteResource,
            sizeof(absoluteResource),
            _TRUNCATE,
            "%s\\Data\\Contract\\Priority\\value.txt",
            gamePath);
        resolvedSize = sizeof(resolved);
        ExpectResult(
            host->resource_resolve(
                owner,
                absoluteResource,
                resolved,
                &resolvedSize),
            WOTBMOD_OK,
            "resource_resolve game Data absolute path");
    } else {
        Expect(false, "game path for absolute resource test");
    }
    ExpectResult(
        host->resource_resolve(owner, nullptr, resolved, &resolvedSize),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_resolve null path");
    ExpectResult(
        host->resource_resolve(
            owner,
            "~res:/Contract/Priority/missing.txt",
            resolved,
            &resolvedSize),
        WOTBMOD_ERROR_NOT_FOUND,
        "resource_resolve missing");

    ExpectResult(
        Mount(
            host,
            owner,
            "~res:/Contract/Priority/",
            "low",
            20,
            WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL,
            &tie),
        WOTBMOD_OK,
        "resource_mount priority tie");
    resolvedSize = sizeof(resolved);
    ExpectResult(
        host->resource_resolve(
            owner,
            "~res:/Contract/Priority/value.txt",
            resolved,
            &resolvedSize),
        WOTBMOD_OK,
        "resource_resolve newest tie");
    Expect(strstr(resolved, "\\low\\value.txt") != nullptr, "newest mount tie wins");

    WotbModResourceMountId dvpl = 0;
    ExpectResult(
        Mount(
            host,
            owner,
            "~res:/Contract/Dvpl/",
            "dvpl",
            1,
            WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL,
            &dvpl),
        WOTBMOD_OK,
        "resource_mount DVPL");
    resolvedSize = sizeof(resolved);
    ExpectResult(
        host->resource_resolve(
            owner,
            "~res:/Contract/Dvpl/packed.yaml",
            resolved,
            &resolvedSize),
        WOTBMOD_OK,
        "resource_resolve DVPL fallback");
    Expect(strstr(resolved, "packed.yaml.dvpl") != nullptr, "DVPL fallback path");

    ExpectResult(host->resource_unmount(owner, tie), WOTBMOD_OK, "unmount tie");
    ExpectResult(host->resource_unmount(owner, high), WOTBMOD_OK, "unmount high");
    ExpectResult(host->resource_unmount(owner, low), WOTBMOD_OK, "unmount low");
    ExpectResult(host->resource_unmount(owner, dvpl), WOTBMOD_OK, "unmount DVPL");
    ExpectResult(
        host->resource_unmount(owner, dvpl),
        WOTBMOD_ERROR_NOT_FOUND,
        "unmount repeated");

    WotbModResourceMountId mounts[32] = {};
    for (uint32_t index = 0; index < 32; ++index) {
        ExpectResult(
            Mount(
                host,
                owner,
                "~res:/Contract/Limit/",
                "low",
                (int32_t)index,
                0,
                &mounts[index]),
            WOTBMOD_OK,
            "resource mount limit fill");
    }
    WotbModResourceMountId overflow = 0;
    ExpectResult(
        Mount(
            host,
            owner,
            "~res:/Contract/Limit/",
            "low",
            100,
            0,
            &overflow),
        WOTBMOD_ERROR_LIMIT_REACHED,
        "resource mount per-mod limit");
    for (uint32_t index = 0; index < 32; ++index) {
        ExpectResult(
            host->resource_unmount(owner, mounts[index]),
            WOTBMOD_OK,
            "resource mount limit cleanup");
    }
    Expect(
        WotbModRuntime_GetResourceGeneration() ==
                (uint64_t)g_lastRegistryGeneration &&
            g_registryChanges > 0,
        "resource generation callback");
}

static void TestVehicleSkins(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    Expect(
        WOTBMOD_HOST_HAS(host, vehicle_skin_register) &&
            WOTBMOD_HOST_HAS(host, vehicle_skin_set_enabled) &&
            WOTBMOD_HOST_HAS(host, vehicle_skin_get_info) &&
            WOTBMOD_HOST_HAS(host, vehicle_skin_release),
        "vehicle skin host surface");

    WotbModResourceMountId lowMount = 0;
    WotbModResourceMountId highMount = 0;
    ExpectResult(
        Mount(
            host,
            owner,
            "~res:/Contract/Skin/low/",
            "low",
            1,
            WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL,
            &lowMount),
        WOTBMOD_OK,
        "vehicle skin low mount");
    ExpectResult(
        Mount(
            host,
            owner,
            "~res:/Contract/Skin/high/",
            "high",
            1,
            WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL,
            &highMount),
        WOTBMOD_OK,
        "vehicle skin high mount");

    WotbModVehicleSkinAsset lowAsset = {};
    lowAsset.struct_size = sizeof(lowAsset);
    lowAsset.kind = WOTBMOD_SKIN_ASSET_MESH;
    lowAsset.stock_virtual_path =
        "~res:/3d/Tanks/Test/hull.sc2";
    lowAsset.replacement_virtual_path =
        "~res:/Contract/Skin/low/value.txt";
    WotbModVehicleSkinDescriptor low = {};
    low.struct_size = sizeof(low);
    low.skin_id = "test-skin-low";
    low.vehicle_name = "TestTank";
    low.assets = &lowAsset;
    low.asset_count = 1;
    low.priority = 10;
    low.flags = WOTBMOD_VEHICLE_SKIN_ENABLED;

    WotbModVehicleSkinHandle lowSkin = nullptr;
    ExpectResult(
        host->vehicle_skin_register(
            nullptr, &low, &lowSkin),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "vehicle skin invalid owner");
    WotbModVehicleSkinDescriptor shortDescriptor = low;
    shortDescriptor.struct_size =
        (uint32_t)offsetof(
            WotbModVehicleSkinDescriptor, flags);
    ExpectResult(
        host->vehicle_skin_register(
            owner, &shortDescriptor, &lowSkin),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "vehicle skin short descriptor");
    WotbModVehicleSkinAsset invalidAsset = lowAsset;
    invalidAsset.stock_virtual_path = "~res:/../escape.sc2";
    WotbModVehicleSkinDescriptor invalid = low;
    invalid.assets = &invalidAsset;
    ExpectResult(
        host->vehicle_skin_register(
            owner, &invalid, &lowSkin),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "vehicle skin traversal rejected");
    ExpectResult(
        host->vehicle_skin_register(
            owner, &low, &lowSkin),
        WOTBMOD_OK,
        "vehicle skin register low");
    Expect(lowSkin != nullptr, "vehicle skin low handle");
    WotbModVehicleSkinHandle duplicate = nullptr;
    ExpectResult(
        host->vehicle_skin_register(
            owner, &low, &duplicate),
        WOTBMOD_ERROR_ALREADY_EXISTS,
        "vehicle skin duplicate id");

    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t resolvedSize = sizeof(resolved);
    ExpectResult(
        WotbModRuntime_ResolveVehicleSkinPath(
            "Data/3d/Tanks/Test/hull.sc2",
            resolved,
            &resolvedSize),
        WOTBMOD_OK,
        "vehicle skin native resolve low");
    Expect(
        strstr(resolved, "\\low\\value.txt") != nullptr,
        "vehicle skin low path");
    resolvedSize = sizeof(resolved);
    ExpectResult(
        WotbModRuntime_ResolveVehicleSkinPath(
            "Data/3d/Tanks/Test/hull.sc2.dvpl",
            resolved,
            &resolvedSize),
        WOTBMOD_OK,
        "vehicle skin native DVPL resolve");
    Expect(
        strstr(resolved, "\\low\\value.txt") != nullptr,
        "vehicle skin DVPL maps to logical stock path");

    WotbModVehicleSkinAsset highAsset = lowAsset;
    highAsset.kind = WOTBMOD_SKIN_ASSET_MATERIAL;
    highAsset.replacement_virtual_path =
        "~res:/Contract/Skin/high/value.txt";
    WotbModVehicleSkinDescriptor high = low;
    high.skin_id = "test-skin-high";
    high.assets = &highAsset;
    high.priority = 20;
    WotbModVehicleSkinHandle highSkin = nullptr;
    ExpectResult(
        host->vehicle_skin_register(
            owner, &high, &highSkin),
        WOTBMOD_OK,
        "vehicle skin register high");
    resolvedSize = sizeof(resolved);
    ExpectResult(
        WotbModRuntime_ResolveVehicleSkinPath(
            "~res:/3d/Tanks/Test/hull.sc2",
            resolved,
            &resolvedSize),
        WOTBMOD_OK,
        "vehicle skin priority resolve");
    Expect(
        strstr(resolved, "\\high\\value.txt") != nullptr,
        "vehicle skin high priority wins");

    WotbModVehicleSkinInfo info = {};
    info.struct_size = sizeof(info);
    ExpectResult(
        host->vehicle_skin_get_info(
            owner, highSkin, &info),
        WOTBMOD_OK,
        "vehicle skin get info");
    Expect(
        info.enabled == 1 &&
            info.asset_count == 1 &&
            info.priority == 20 &&
            strcmp(info.skin_id, "test-skin-high") == 0 &&
            strcmp(info.vehicle_name, "TestTank") == 0,
        "vehicle skin info values");
    WotbModVehicleSkinInfo shortInfo = {};
    shortInfo.struct_size =
        (uint32_t)(
            offsetof(WotbModVehicleSkinInfo, priority) +
            sizeof(shortInfo.priority));
    ExpectResult(
        host->vehicle_skin_get_info(
            owner, highSkin, &shortInfo),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "vehicle skin short info");
    ExpectResult(
        host->vehicle_skin_get_info(
            peer, highSkin, &info),
        WOTBMOD_ERROR_NOT_FOUND,
        "vehicle skin owner isolation");

    ExpectResult(
        host->vehicle_skin_set_enabled(
            owner, highSkin, 0),
        WOTBMOD_OK,
        "vehicle skin disable high");
    resolvedSize = sizeof(resolved);
    ExpectResult(
        WotbModRuntime_ResolveVehicleSkinPath(
            "~res:/3d/Tanks/Test/hull.sc2",
            resolved,
            &resolvedSize),
        WOTBMOD_OK,
        "vehicle skin fallback after disable");
    Expect(
        strstr(resolved, "\\low\\value.txt") != nullptr,
        "vehicle skin low fallback");
    ExpectResult(
        host->vehicle_skin_set_enabled(
            owner, highSkin, 1),
        WOTBMOD_OK,
        "vehicle skin re-enable high");

    ExpectResult(
        host->vehicle_skin_release(owner, highSkin),
        WOTBMOD_OK,
        "vehicle skin release high");
    ExpectResult(
        host->vehicle_skin_release(owner, highSkin),
        WOTBMOD_ERROR_NOT_FOUND,
        "vehicle skin stale handle");
    ExpectResult(
        host->vehicle_skin_release(owner, lowSkin),
        WOTBMOD_OK,
        "vehicle skin release low");
    resolvedSize = sizeof(resolved);
    ExpectResult(
        WotbModRuntime_ResolveVehicleSkinPath(
            "~res:/3d/Tanks/Test/hull.sc2",
            resolved,
            &resolvedSize),
        WOTBMOD_ERROR_NOT_FOUND,
        "vehicle skin restore stock path");
    ExpectResult(
        host->resource_unmount(owner, highMount),
        WOTBMOD_OK,
        "vehicle skin high unmount");
    ExpectResult(
        host->resource_unmount(owner, lowMount),
        WOTBMOD_OK,
        "vehicle skin low unmount");
}

static void TestTypedResources(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    WotbModResourceLoadRequest invalid = {};
    invalid.struct_size = sizeof(invalid);
    invalid.type = WOTBMOD_RESOURCE_GENERIC;
    invalid.virtual_path = "~res:/Typed/item";
    WotbModResourceHandle resource = nullptr;
    ExpectResult(
        host->resource_load(nullptr, &invalid, &resource),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_load invalid owner");
    invalid.struct_size =
        (uint32_t)offsetof(WotbModResourceLoadRequest, flags);
    ExpectResult(
        host->resource_load(owner, &invalid, &resource),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_load short structure");
    invalid.struct_size = sizeof(invalid);
    invalid.type = static_cast<WotbModResourceType>(99);
    ExpectResult(
        host->resource_load(owner, &invalid, &resource),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_load invalid type");
    invalid.type = WOTBMOD_RESOURCE_GENERIC;
    invalid.virtual_path = "~res:/../escape";
    ExpectResult(
        host->resource_load(owner, &invalid, &resource),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_load traversal");

    for (uint32_t type = WOTBMOD_RESOURCE_GENERIC;
         type <= WOTBMOD_RESOURCE_AUDIO_CLIP;
         ++type) {
        resource = nullptr;
        ExpectResult(
            LoadResource(
                host,
                owner,
                static_cast<WotbModResourceType>(type),
                "Data/Typed/item.asset",
                type == WOTBMOD_RESOURCE_UI_CONTROL ? "NamedControl" : nullptr,
                0xC0000000u | type,
                &resource),
            WOTBMOD_OK,
            "resource_load typed");
        Expect(resource != nullptr, "resource opaque handle");
        Expect(
            strcmp(g_lastResourcePath, "~res:/Typed/item.asset") == 0 &&
                g_lastResourceType ==
                    static_cast<WotbModResourceType>(type) &&
                g_lastResourceFlags == (0xC0000000u | type),
            "resource backend normalized request");
        if (type == WOTBMOD_RESOURCE_UI_CONTROL) {
            Expect(
                strcmp(g_lastResourceObject, "NamedControl") == 0,
                "resource object name");
        }
        ExpectResult(
            host->resource_reload(owner, resource),
            WOTBMOD_OK,
            "resource_reload");
        ExpectResult(
            host->resource_reload(peer, resource),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "resource_reload foreign");
        ExpectResult(
            host->resource_release(peer, resource),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "resource_release foreign");
        ExpectResult(
            host->resource_release(owner, resource),
            WOTBMOD_OK,
            "resource_release");
        ExpectResult(
            host->resource_release(owner, resource),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "resource_release repeated");
    }

    WotbModResourceHandle oldHandle = nullptr;
    WotbModResourceHandle newHandle = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/old",
            nullptr,
            0,
            &oldHandle),
        WOTBMOD_OK,
        "stale handle first load");
    ExpectResult(
        host->resource_release(owner, oldHandle),
        WOTBMOD_OK,
        "stale handle first release");
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/new",
            nullptr,
            0,
            &newHandle),
        WOTBMOD_OK,
        "stale handle second load");
    Expect(oldHandle != newHandle, "resource handle generation changes");
    ExpectResult(
        host->resource_release(owner, oldHandle),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "stale resource handle rejected");
    ExpectResult(
        host->resource_reload(owner, newHandle),
        WOTBMOD_OK,
        "new resource survives stale handle");
    ExpectResult(
        host->resource_release(owner, newHandle),
        WOTBMOD_OK,
        "new resource release");

    WotbModResourceHandle resources[64] = {};
    for (uint32_t index = 0; index < 64; ++index) {
        ExpectResult(
            LoadResource(
                host,
                owner,
                WOTBMOD_RESOURCE_GENERIC,
                "~res:/Typed/limit",
                nullptr,
                index,
                &resources[index]),
            WOTBMOD_OK,
            "resource handle limit fill");
    }
    resource = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/overflow",
            nullptr,
            0,
            &resource),
        WOTBMOD_ERROR_LIMIT_REACHED,
        "resource handle limit");
    for (uint32_t index = 0; index < 64; ++index) {
        ExpectResult(
            host->resource_release(owner, resources[index]),
            WOTBMOD_OK,
            "resource handle limit cleanup");
    }

    InterlockedExchange(&g_resourceFaultMode, 1);
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/fault-load",
            nullptr,
            0,
            &resource),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "resource load backend fault isolation");
    InterlockedExchange(&g_resourceFaultMode, 4);
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/null-native",
            nullptr,
            0,
            &resource),
        WOTBMOD_ERROR_PLATFORM,
        "resource load OK with null native");

    resource = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/fault-ops",
            nullptr,
            0,
            &resource),
        WOTBMOD_OK,
        "resource operation fault setup");
    InterlockedExchange(&g_resourceFaultMode, 2);
    ExpectResult(
        host->resource_reload(owner, resource),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "resource reload backend fault isolation");
    InterlockedExchange(&g_resourceFaultMode, 3);
    ExpectResult(
        host->resource_release(owner, resource),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "resource release backend fault isolation");
    ExpectResult(
        host->resource_release(owner, resource),
        WOTBMOD_OK,
        "resource release retry after backend fault");

    ExpectResult(
        host->resource_reload(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_reload null");
    ExpectResult(
        host->resource_release(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource_release null");
}

static void TestClientEvents(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    const DWORD dispatchThreadId = GetCurrentThreadId();

    WotbModResourceHandle cloned = nullptr;
    ExpectResult(
        host->resource_clone(nullptr, nullptr, &cloned),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource clone invalid owner");
    ExpectResult(
        host->resource_clone(owner, nullptr, &cloned),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource clone null source");
    ExpectResult(
        host->resource_clone(
            owner,
            reinterpret_cast<WotbModResourceHandle>(0xFFFFu),
            &cloned),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "resource clone unknown source");

    WotbModEventSubscriptionId subscription = 0;
    ExpectResult(
        host->event_subscribe(
            nullptr,
            WOTBMOD_EVENT_ALL,
            &ClientEventCallback,
            nullptr,
            &subscription),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "event subscribe invalid owner");
    ExpectResult(
        host->event_subscribe(
            owner,
            0,
            &ClientEventCallback,
            nullptr,
            &subscription),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "event subscribe empty mask");
    ExpectResult(
        host->event_subscribe(
            owner,
            1u << 30,
            &ClientEventCallback,
            nullptr,
            &subscription),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "event subscribe unknown mask");
    ExpectResult(
        host->event_subscribe(
            owner,
            WOTBMOD_EVENT_ALL,
            nullptr,
            nullptr,
            &subscription),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "event subscribe null callback");
    ExpectResult(
        host->event_subscribe(
            owner,
            WOTBMOD_EVENT_ALL,
            &ClientEventCallback,
            nullptr,
            nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "event subscribe null output");
    ExpectResult(
        host->event_unsubscribe(owner, 0),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "event unsubscribe zero id");
    ExpectResult(
        host->event_unsubscribe(owner, UINT64_MAX),
        WOTBMOD_ERROR_NOT_FOUND,
        "event unsubscribe unknown id");
    ExpectResult(
        WotbModRuntime_NotifyUiScreenChanged(nullptr, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI event rejects empty payload");
    ExpectResult(
        WotbModRuntime_NotifySceneActivated(nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene activate rejects empty payload");
    ExpectResult(
        WotbModRuntime_NotifySceneDeactivated(nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene deactivate rejects empty payload");

    ClientEventProbe allProbe = {};
    allProbe.expected_thread_id = dispatchThreadId;
    allProbe.structures_valid = true;
    allProbe.main_thread_only = true;
    allProbe.payloads_valid = true;
    allProbe.borrowed_release_result = WOTBMOD_ERROR_PLATFORM;
    allProbe.borrowed_reload_result = WOTBMOD_ERROR_PLATFORM;
    allProbe.clone_result = WOTBMOD_ERROR_PLATFORM;
    allProbe.mutation_result = WOTBMOD_ERROR_PLATFORM;
    ClientEventProbe uiProbe = {};
    uiProbe.expected_thread_id = dispatchThreadId;
    uiProbe.structures_valid = true;
    uiProbe.main_thread_only = true;
    uiProbe.payloads_valid = true;

    WotbModEventSubscriptionId allSubscription = 0;
    WotbModEventSubscriptionId uiSubscription = 0;
    ExpectResult(
        host->event_subscribe(
            owner,
            WOTBMOD_EVENT_ALL,
            &ClientEventCallback,
            &allProbe,
            &allSubscription),
        WOTBMOD_OK,
        "event subscribe all");
    ExpectResult(
        host->event_subscribe(
            owner,
            WOTBMOD_EVENT_UI_SCREEN_CHANGED,
            &ClientEventCallback,
            &uiProbe,
            &uiSubscription),
        WOTBMOD_OK,
        "event subscribe filtered");
    Expect(allSubscription != 0, "event subscription id published");
    ExpectResult(
        host->event_unsubscribe(peer, allSubscription),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "event unsubscribe foreign owner");

    void* previousUi = nullptr;
    void* currentUi = nullptr;
    void* scene = nullptr;
    ExpectResult(
        CreateObjectResource(
            WOTBMOD_RESOURCE_UI_CONTROL,
            nullptr,
            100,
            &g_uiCreates,
            &previousUi),
        WOTBMOD_OK,
        "event previous UI fixture");
    ExpectResult(
        CreateObjectResource(
            WOTBMOD_RESOURCE_UI_CONTROL,
            nullptr,
            100,
            &g_uiCreates,
            &currentUi),
        WOTBMOD_OK,
        "event current UI fixture");
    ExpectResult(
        CreateObjectResource(
            WOTBMOD_RESOURCE_SCENE,
            nullptr,
            100,
            &g_sceneEntityCreates,
            &scene),
        WOTBMOD_OK,
        "event scene fixture");
    ExpectResult(
        WotbModRuntime_NotifyUiScreenChanged(
            previousUi,
            currentUi),
        WOTBMOD_OK,
        "queue UI screen changed");
    ExpectResult(
        WotbModRuntime_NotifySceneActivated(scene),
        WOTBMOD_OK,
        "queue scene activated");
    ExpectResult(
        WotbModRuntime_NotifySceneDeactivated(scene),
        WOTBMOD_OK,
        "queue scene deactivated");
    Expect(
        allProbe.callback_count == 0 &&
            uiProbe.callback_count == 0,
        "client events deferred until DispatchFrame");
    ExpectResult(
        ResourceRelease(nullptr, previousUi),
        WOTBMOD_OK,
        "release previous UI fixture");
    ExpectResult(
        ResourceRelease(nullptr, currentUi),
        WOTBMOD_OK,
        "release current UI fixture");
    ExpectResult(
        ResourceRelease(nullptr, scene),
        WOTBMOD_OK,
        "release scene fixture");

    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    Expect(
        allProbe.callback_count == 3 &&
            allProbe.types[0] ==
                WOTBMOD_EVENT_UI_SCREEN_CHANGED &&
            allProbe.types[1] ==
                WOTBMOD_EVENT_SCENE_ACTIVATED &&
            allProbe.types[2] ==
                WOTBMOD_EVENT_SCENE_DEACTIVATED &&
            allProbe.sequences[0] < allProbe.sequences[1] &&
            allProbe.sequences[1] < allProbe.sequences[2],
        "client event FIFO and type order");
    Expect(
        uiProbe.callback_count == 1 &&
            uiProbe.types[0] ==
                WOTBMOD_EVENT_UI_SCREEN_CHANGED,
        "client event mask filtering");
    Expect(
        allProbe.structures_valid &&
            allProbe.main_thread_only &&
            allProbe.payloads_valid &&
            uiProbe.structures_valid &&
            uiProbe.main_thread_only &&
            uiProbe.payloads_valid,
        "client event ABI payload and main-thread delivery");
    ExpectResult(
        allProbe.borrowed_release_result,
        WOTBMOD_ERROR_ACCESS_DENIED,
        "borrowed event handle cannot be released");
    ExpectResult(
        allProbe.borrowed_reload_result,
        WOTBMOD_ERROR_ACCESS_DENIED,
        "borrowed event handle cannot be reloaded");
    ExpectResult(
        allProbe.clone_result,
        WOTBMOD_OK,
        "borrowed event handle cloned");
    ExpectResult(
        allProbe.mutation_result,
        WOTBMOD_OK,
        "borrowed event handle usable in callback");
    Expect(
        allProbe.cloned_resource != nullptr,
        "resource clone publishes owned handle");
    ExpectResult(
        host->resource_clone(
            owner,
            allProbe.stale_borrowed_resource,
            &cloned),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "borrowed event handle invalid after callback");
    ExpectResult(
        host->ui_control_set_visible(
            owner,
            allProbe.cloned_resource,
            1),
        WOTBMOD_OK,
        "cloned event resource survives callback");
    ExpectResult(
        host->resource_release(
            owner,
            allProbe.cloned_resource),
        WOTBMOD_OK,
        "cloned event resource release");

    ExpectResult(
        host->event_unsubscribe(owner, allSubscription),
        WOTBMOD_OK,
        "event unsubscribe all");
    ExpectResult(
        host->event_unsubscribe(owner, allSubscription),
        WOTBMOD_ERROR_NOT_FOUND,
        "event unsubscribe repeated");
    ExpectResult(
        host->event_unsubscribe(owner, uiSubscription),
        WOTBMOD_OK,
        "event unsubscribe filtered");

    void* unobservedUi = nullptr;
    ExpectResult(
        CreateObjectResource(
            WOTBMOD_RESOURCE_UI_CONTROL,
            nullptr,
            100,
            &g_uiCreates,
            &unobservedUi),
        WOTBMOD_OK,
        "unobserved UI fixture");
    ExpectResult(
        WotbModRuntime_NotifyUiScreenChanged(
            nullptr,
            unobservedUi),
        WOTBMOD_OK,
        "queue event without subscribers");
    ExpectResult(
        ResourceRelease(nullptr, unobservedUi),
        WOTBMOD_OK,
        "release unobserved UI fixture");
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    Expect(
        allProbe.callback_count == 3 &&
            uiProbe.callback_count == 1,
        "unsubscribed callbacks not invoked");

    WotbModResourceHandle ownedUi = nullptr;
    WotbModResourceHandle ownedClone = nullptr;
    ExpectResult(
        host->ui_control_create(owner, nullptr, &ownedUi),
        WOTBMOD_OK,
        "owned resource clone fixture");
    ExpectResult(
        host->resource_clone(owner, ownedUi, &ownedClone),
        WOTBMOD_OK,
        "owned resource clone");
    ExpectResult(
        host->resource_release(owner, ownedClone),
        WOTBMOD_OK,
        "owned resource clone release");
    InterlockedExchange(&g_resourceFaultMode, 5);
    ownedClone = nullptr;
    ExpectResult(
        host->resource_clone(owner, ownedUi, &ownedClone),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "resource clone backend fault isolation");
    Expect(
        ownedClone == nullptr,
        "faulted resource clone publishes no handle");
    InterlockedExchange(&g_resourceFaultMode, 6);
    ExpectResult(
        host->resource_clone(owner, ownedUi, &ownedClone),
        WOTBMOD_ERROR_PLATFORM,
        "resource clone rejects null native result");
    ExpectResult(
        host->resource_release(owner, ownedUi),
        WOTBMOD_OK,
        "owned resource clone fixture release");

    WotbModEventSubscriptionId limitSubscriptions[16] = {};
    for (uint32_t index = 0; index < 16; ++index) {
        ExpectResult(
            host->event_subscribe(
                owner,
                WOTBMOD_EVENT_SCENE_ACTIVATED,
                &ClientEventCallback,
                &allProbe,
                &limitSubscriptions[index]),
            WOTBMOD_OK,
            "event subscription limit fill");
    }
    ExpectResult(
        host->event_subscribe(
            owner,
            WOTBMOD_EVENT_SCENE_ACTIVATED,
            &ClientEventCallback,
            &allProbe,
            &subscription),
        WOTBMOD_ERROR_LIMIT_REACHED,
        "event subscription per-mod limit");
    for (uint32_t index = 0; index < 16; ++index) {
        ExpectResult(
            host->event_unsubscribe(
                owner,
                limitSubscriptions[index]),
            WOTBMOD_OK,
            "event subscription limit cleanup");
    }

    ClientEventProbe disabledProbe = {};
    disabledProbe.expected_thread_id = dispatchThreadId;
    disabledProbe.structures_valid = true;
    disabledProbe.main_thread_only = true;
    disabledProbe.payloads_valid = true;
    WotbModEventSubscriptionId disabledSubscription = 0;
    ExpectResult(
        host->event_subscribe(
            peer,
            WOTBMOD_EVENT_UI_SCREEN_CHANGED,
            &ClientEventCallback,
            &disabledProbe,
            &disabledSubscription),
        WOTBMOD_OK,
        "disabled cleanup subscription");
    ExpectResult(
        host->set_mod_enabled("test.peer", 0),
        WOTBMOD_OK,
        "disable removes event subscriptions");
    ExpectResult(
        host->set_mod_enabled("test.peer", 1),
        WOTBMOD_OK,
        "re-enable after event subscription cleanup");
    void* disabledUi = nullptr;
    ExpectResult(
        CreateObjectResource(
            WOTBMOD_RESOURCE_UI_CONTROL,
            nullptr,
            100,
            &g_uiCreates,
            &disabledUi),
        WOTBMOD_OK,
        "disabled cleanup UI fixture");
    ExpectResult(
        WotbModRuntime_NotifyUiScreenChanged(
            nullptr,
            disabledUi),
        WOTBMOD_OK,
        "queue after subscription owner re-enable");
    ExpectResult(
        ResourceRelease(nullptr, disabledUi),
        WOTBMOD_OK,
        "release disabled cleanup UI fixture");
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    Expect(
        disabledProbe.callback_count == 0,
        "disable permanently removes prior subscriptions");
    ExpectResult(
        host->event_unsubscribe(peer, disabledSubscription),
        WOTBMOD_ERROR_NOT_FOUND,
        "disabled subscription id invalidated");
}

static WotbModResult QueueGameplayTestEvent(
    uint32_t type,
    uint32_t primaryEntityId,
    uint32_t otherEntityId,
    const void* payload,
    uint32_t payloadSize) {
    WotbModRuntimeClientEvent event = {};
    event.struct_size = sizeof(event);
    event.type = type;
    event.resource_type = WOTBMOD_RESOURCE_GENERIC;
    event.primary_entity_id = primaryEntityId;
    event.other_entity_id = otherEntityId;
    event.payload_size = payloadSize;
    if (payload && payloadSize <= sizeof(event.payload)) {
        memcpy(&event.payload, payload, payloadSize);
    }
    return WotbModRuntime_NotifyClientEvent(&event);
}

static void TestGameplayApiAndEvents(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    uint32_t vehicleCount = 99;
    WotbModVehicleHandle localVehicle = nullptr;
    WotbModVehicleHandle remoteVehicle = nullptr;
    WotbModVehicleHandle indexedVehicle = nullptr;
    WotbModVehicleHandle clonedVehicle = nullptr;

    ExpectResult(
        host->vehicle_get_count(nullptr, &vehicleCount),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "vehicle count invalid owner");
    ExpectResult(
        host->vehicle_get_count(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "vehicle count null output");
    ExpectResult(
        host->vehicle_get_local(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "local vehicle null output");
    ExpectResult(
        host->vehicle_get_by_entity_id(
            owner, 0, &remoteVehicle),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "vehicle lookup zero entity");
    ExpectResult(
        host->vehicle_get_count(owner, &vehicleCount),
        WOTBMOD_OK,
        "vehicle count");
    Expect(vehicleCount == 2, "vehicle count value");
    ExpectResult(
        host->vehicle_get_local(owner, &localVehicle),
        WOTBMOD_OK,
        "local vehicle snapshot");
    ExpectResult(
        host->vehicle_get_by_entity_id(
            owner, 202u, &remoteVehicle),
        WOTBMOD_OK,
        "vehicle lookup by entity id");
    ExpectResult(
        host->vehicle_get_at(owner, 1, &indexedVehicle),
        WOTBMOD_OK,
        "vehicle lookup by index");
    WotbModVehicleHandle missingVehicle = nullptr;
    ExpectResult(
        host->vehicle_get_at(owner, 2, &missingVehicle),
        WOTBMOD_ERROR_NOT_FOUND,
        "vehicle index bounds");
    ExpectResult(
        host->vehicle_get_by_entity_id(
            owner, 999u, &missingVehicle),
        WOTBMOD_ERROR_NOT_FOUND,
        "vehicle entity not found");

    WotbModVehicleInfo localInfo = {};
    localInfo.struct_size = sizeof(localInfo);
    ExpectResult(
        host->vehicle_get_info(
            owner, localVehicle, &localInfo),
        WOTBMOD_OK,
        "local vehicle info");
    Expect(
        localInfo.entity_id == 101u &&
            localInfo.team == 1u &&
            localInfo.health == 900 &&
            localInfo.max_health == 1000 &&
            (localInfo.flags & WOTBMOD_VEHICLE_LOCAL) != 0 &&
            strcmp(localInfo.player_name, "LocalTester") == 0 &&
            strcmp(localInfo.vehicle_name, "TestTankLocal") == 0,
        "local vehicle info values");
    WotbModVehicleInfo partialInfo = {};
    partialInfo.struct_size =
        static_cast<uint32_t>(
            offsetof(WotbModVehicleInfo, max_health));
    ExpectResult(
        host->vehicle_get_info(
            owner, localVehicle, &partialInfo),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "vehicle info partial structure");
    Expect(
        partialInfo.struct_size == sizeof(WotbModVehicleInfo) &&
            partialInfo.entity_id == 101u &&
            partialInfo.health == 900,
        "vehicle info partial values");
    WotbModVehicleInfo remoteInfo = {};
    remoteInfo.struct_size = sizeof(remoteInfo);
    ExpectResult(
        host->vehicle_get_info(
            owner, remoteVehicle, &remoteInfo),
        WOTBMOD_OK,
        "remote vehicle info");
    Expect(
        remoteInfo.entity_id == 202u &&
            remoteInfo.team == 2u,
        "remote vehicle info values");
    WotbModVehicleInfo indexedInfo = {};
    indexedInfo.struct_size = sizeof(indexedInfo);
    ExpectResult(
        host->vehicle_get_info(
            owner, indexedVehicle, &indexedInfo),
        WOTBMOD_OK,
        "indexed vehicle info");
    Expect(
        indexedInfo.entity_id == 202u,
        "indexed vehicle value");

    ExpectResult(
        host->vehicle_clone(
            peer, localVehicle, &clonedVehicle),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "vehicle handle owner isolation");
    ExpectResult(
        host->vehicle_clone(
            owner, localVehicle, &clonedVehicle),
        WOTBMOD_OK,
        "vehicle clone");
    InterlockedExchange(&g_gameplayFaultMode, 5);
    ExpectResult(
        host->vehicle_clone(
            owner, localVehicle, &missingVehicle),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "vehicle clone backend fault isolation");
    InterlockedExchange(&g_gameplayFaultMode, 6);
    WotbModVehicleInfo faultedInfo = {};
    faultedInfo.struct_size = sizeof(faultedInfo);
    ExpectResult(
        host->vehicle_get_info(
            owner, localVehicle, &faultedInfo),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "vehicle info backend fault isolation");
    InterlockedExchange(&g_gameplayFaultMode, 3);
    ExpectResult(
        host->vehicle_get_count(owner, &vehicleCount),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "vehicle count backend fault isolation");
    InterlockedExchange(&g_gameplayFaultMode, 1);
    ExpectResult(
        host->vehicle_get_local(owner, &missingVehicle),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "local vehicle backend fault isolation");

    VehicleWorkerContext worker = {};
    worker.host = host;
    worker.owner = owner;
    HANDLE workerThread = CreateThread(
        nullptr, 0, &VehicleWorker, &worker, 0, nullptr);
    Expect(workerThread != nullptr, "vehicle worker thread create");
    if (workerThread) {
        Expect(
            WaitForSingleObject(workerThread, 5000) ==
                WAIT_OBJECT_0,
            "vehicle worker thread join");
        CloseHandle(workerThread);
        ExpectResult(
            worker.count_result,
            WOTBMOD_ERROR_WRONG_THREAD,
            "vehicle count rejects worker thread");
        ExpectResult(
            worker.local_result,
            WOTBMOD_ERROR_WRONG_THREAD,
            "vehicle factory rejects worker thread");
        Expect(
            !worker.vehicle,
            "wrong-thread vehicle factory publishes no handle");
    }

    ExpectResult(
        host->vehicle_release(owner, indexedVehicle),
        WOTBMOD_OK,
        "indexed vehicle release");
    ExpectResult(
        host->vehicle_release(owner, remoteVehicle),
        WOTBMOD_OK,
        "remote vehicle release");
    ExpectResult(
        host->vehicle_release(owner, clonedVehicle),
        WOTBMOD_OK,
        "cloned vehicle release");
    ExpectResult(
        host->vehicle_release(owner, localVehicle),
        WOTBMOD_OK,
        "local vehicle release");
    ExpectResult(
        host->vehicle_release(owner, localVehicle),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "stale vehicle handle rejected");

    WotbModRuntimeClientEvent invalidEvent = {};
    invalidEvent.struct_size =
        static_cast<uint32_t>(
            offsetof(WotbModRuntimeClientEvent, payload));
    invalidEvent.type = WOTBMOD_EVENT_SHOT_FIRED;
    ExpectResult(
        WotbModRuntime_NotifyClientEvent(&invalidEvent),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "generic event ingress rejects short structure");
    invalidEvent.struct_size = sizeof(invalidEvent);
    invalidEvent.type =
        WOTBMOD_EVENT_SHOT_FIRED |
        WOTBMOD_EVENT_VEHICLE_DAMAGED;
    ExpectResult(
        WotbModRuntime_NotifyClientEvent(&invalidEvent),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "generic event ingress rejects multi-bit type");

    WotbModClientEventPayload shortPayload = {};
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED,
            101u,
            0u,
            &shortPayload,
            sizeof(WotbModVehicleEventData) - 1u),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "local vehicle ingress rejects a short typed payload");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_DAMAGED,
            101u,
            202u,
            &shortPayload,
            sizeof(WotbModDamageEventData) - 1u),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "damage ingress rejects a short typed payload");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_DESTROYED,
            101u,
            0u,
            &shortPayload,
            sizeof(WotbModVehicleEventData) - 1u),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "destroy ingress rejects a short typed payload");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_CAMERA_MODE_CHANGED,
            0u,
            0u,
            &shortPayload,
            sizeof(WotbModCameraEventData) - 1u),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "camera ingress rejects a short typed payload");

    WotbModV3Handle contextProbe = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Bootstrap* v3Bootstrap =
        WotbModV3Runtime_GetBootstrap();
    const WotbModV3CoreApiV1* v3Core = nullptr;
    const bool contextProbeReady =
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_NONE) ==
            WOTBMOD_V3_OK &&
        WotbModV3Runtime_CreateMod(
            "battle-context-probe",
            WOTBMOD_V3_PERMISSION_SAFE,
            &contextProbe) == WOTBMOD_V3_OK &&
        v3Bootstrap && v3Bootstrap->query_interface &&
        v3Bootstrap->query_interface(
            contextProbe,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION,
            reinterpret_cast<const void**>(&v3Core)) == WOTBMOD_V3_OK &&
        v3Core && v3Core->get_context;
    Expect(
        contextProbeReady,
        "battle context regression probe setup");
    if (contextProbeReady) {
        WotbModBattleEventData boundary = {};
        boundary.state = 1u;
        ExpectResult(
            QueueGameplayTestEvent(
                WOTBMOD_EVENT_BATTLE_ENTERED,
                0u,
                0u,
                &boundary,
                sizeof(boundary)),
            WOTBMOD_OK,
            "queue context-regression battle entered");
        WotbModRuntime_DispatchFrame(
            nullptr, nullptr, nullptr, 1280, 720, 0.016);
        uint64_t context = WOTBMOD_V3_CONTEXT_NONE;
        Expect(
            v3Core->get_context(contextProbe, &context) == WOTBMOD_V3_OK &&
                context == WOTBMOD_V3_CONTEXT_BATTLE,
            "battle-entered ingress changes the public V3 context to BATTLE");

        boundary.state = 0u;
        ExpectResult(
            QueueGameplayTestEvent(
                WOTBMOD_EVENT_BATTLE_LEFT,
                0u,
                0u,
                &boundary,
                sizeof(boundary)),
            WOTBMOD_OK,
            "queue context-regression battle left");
        WotbModRuntime_DispatchFrame(
            nullptr, nullptr, nullptr, 1280, 720, 0.016);
        context = WOTBMOD_V3_CONTEXT_NONE;
        Expect(
            v3Core->get_context(contextProbe, &context) == WOTBMOD_V3_OK &&
                context == WOTBMOD_V3_CONTEXT_HANGAR,
            "battle-left ingress changes the public V3 context to HANGAR");
    }
    if (contextProbe != WOTBMOD_V3_INVALID_HANDLE) {
        Expect(
            WotbModV3Runtime_DestroyMod(contextProbe) == WOTBMOD_V3_OK,
            "battle context regression probe cleanup");
    }

    const uint32_t gameplayMask =
        WOTBMOD_EVENT_BATTLE_ENTERED |
        WOTBMOD_EVENT_BATTLE_STARTED |
        WOTBMOD_EVENT_BATTLE_ENDED |
        WOTBMOD_EVENT_BATTLE_LEFT |
        WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED |
        WOTBMOD_EVENT_VEHICLE_SPAWNED |
        WOTBMOD_EVENT_VEHICLE_DESPAWNED |
        WOTBMOD_EVENT_SHOT_FIRED |
        WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED |
        WOTBMOD_EVENT_VEHICLE_DAMAGED |
        WOTBMOD_EVENT_VEHICLE_DESTROYED |
        WOTBMOD_EVENT_RELOAD_STATE_CHANGED |
        WOTBMOD_EVENT_CAMERA_MODE_CHANGED;
    GameplayEventProbe probe = {};
    probe.expected_thread_id = GetCurrentThreadId();
    probe.structures_valid = true;
    probe.main_thread_only = true;
    probe.payloads_valid = true;
    probe.vehicles_valid = true;
    probe.borrowed_release_result = WOTBMOD_ERROR_PLATFORM;
    probe.clone_result = WOTBMOD_ERROR_PLATFORM;
    WotbModEventSubscriptionId subscription = 0;
    ExpectResult(
        host->event_subscribe(
            owner,
            gameplayMask,
            &GameplayEventCallback,
            &probe,
            &subscription),
        WOTBMOD_OK,
        "gameplay event subscribe");

    WotbModBattleEventData battleEntered = {};
    battleEntered.state = 1;
    WotbModBattleEventData battleStarted = {};
    battleStarted.state = 2;
    WotbModVehicleEventData localChanged = {};
    localChanged.entity_id = 101u;
    localChanged.other_entity_id = 202u;
    localChanged.flags = WOTBMOD_VEHICLE_LOCAL;
    WotbModVehicleEventData spawned = {};
    spawned.entity_id = 101u;
    spawned.health = 900;
    spawned.flags = WOTBMOD_VEHICLE_ALIVE;
    WotbModVehicleEventData despawned = {};
    despawned.entity_id = 202u;
    WotbModShotEventData shot = {};
    shot.shot_code = 7u;
    shot.direction.z = 1.0f;
    WotbModVehicleEventData healthChanged = {};
    healthChanged.entity_id = 101u;
    healthChanged.previous_health = 900;
    healthChanged.health = 700;
    WotbModVehicleEventData healthSuperseded = healthChanged;
    healthSuperseded.health = 800;
    WotbModDamageEventData damaged = {};
    damaged.damage = 200;
    damaged.previous_health = 900;
    damaged.health = 700;
    damaged.source_entity_id = 202u;
    WotbModVehicleEventData destroyed = {};
    destroyed.entity_id = 202u;
    destroyed.previous_health = 100;
    destroyed.health = 0;
    destroyed.flags = WOTBMOD_VEHICLE_DESTROYED;
    WotbModReloadEventData reload = {};
    reload.state = 2u;
    reload.duration_seconds = 3.0f;
    reload.progress = 0.5f;
    reload.remaining_seconds = 1.5f;
    WotbModBattleEventData battleLeft = {};
    WotbModBattleEventData battleEnded = {};
    battleEnded.state = 3;
    WotbModCameraEventData cameraMode = {};
    cameraMode.previous_mode = WOTBMOD_V3_CAMERA_MODE_ARCADE;
    cameraMode.mode = WOTBMOD_V3_CAMERA_MODE_SNIPER;
    cameraMode.native_mode = 1;

    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_BATTLE_ENTERED,
            0,
            0,
            &battleEntered,
            sizeof(battleEntered)),
        WOTBMOD_OK,
        "queue battle entered");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_BATTLE_STARTED,
            0,
            0,
            &battleStarted,
            sizeof(battleStarted)),
        WOTBMOD_OK,
        "queue battle started");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED,
            101u,
            202u,
            &localChanged,
            sizeof(localChanged)),
        WOTBMOD_OK,
        "queue local vehicle changed");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_SPAWNED,
            101u,
            0,
            &spawned,
            sizeof(spawned)),
        WOTBMOD_OK,
        "queue vehicle spawned");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_DESPAWNED,
            202u,
            0,
            &despawned,
            sizeof(despawned)),
        WOTBMOD_OK,
        "queue vehicle despawned");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_SHOT_FIRED,
            101u,
            0,
            &shot,
            sizeof(shot)),
        WOTBMOD_OK,
        "queue shot fired");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED,
            101u,
            0,
            &healthSuperseded,
            sizeof(healthSuperseded)),
        WOTBMOD_OK,
        "queue superseded vehicle health state");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED,
            101u,
            0,
            &healthChanged,
            sizeof(healthChanged)),
        WOTBMOD_OK,
        "queue vehicle health changed");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_DAMAGED,
            101u,
            0,
            &damaged,
            sizeof(damaged)),
        WOTBMOD_OK,
        "queue vehicle damaged");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_VEHICLE_DESTROYED,
            202u,
            0,
            &destroyed,
            sizeof(destroyed)),
        WOTBMOD_OK,
        "queue vehicle destroyed");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_RELOAD_STATE_CHANGED,
            101u,
            0,
            &reload,
            sizeof(reload)),
        WOTBMOD_OK,
        "queue reload state changed");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_CAMERA_MODE_CHANGED,
            0,
            0,
            &cameraMode,
            sizeof(cameraMode)),
        WOTBMOD_OK,
        "queue camera mode changed");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_BATTLE_ENDED,
            0,
            0,
            &battleEnded,
            sizeof(battleEnded)),
        WOTBMOD_OK,
        "queue battle ended");
    ExpectResult(
        QueueGameplayTestEvent(
            WOTBMOD_EVENT_BATTLE_LEFT,
            0,
            0,
            &battleLeft,
            sizeof(battleLeft)),
        WOTBMOD_OK,
        "queue battle left");
    Expect(
        probe.callback_count == 0,
        "gameplay events deferred until DispatchFrame");

    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    const uint32_t expectedTypes[] = {
        WOTBMOD_EVENT_BATTLE_ENTERED,
        WOTBMOD_EVENT_BATTLE_STARTED,
        WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED,
        WOTBMOD_EVENT_VEHICLE_SPAWNED,
        WOTBMOD_EVENT_VEHICLE_DESPAWNED,
        WOTBMOD_EVENT_SHOT_FIRED,
        WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED,
        WOTBMOD_EVENT_VEHICLE_DAMAGED,
        WOTBMOD_EVENT_VEHICLE_DESTROYED,
        WOTBMOD_EVENT_RELOAD_STATE_CHANGED,
        WOTBMOD_EVENT_CAMERA_MODE_CHANGED,
        WOTBMOD_EVENT_BATTLE_ENDED,
        WOTBMOD_EVENT_BATTLE_LEFT};
    bool ordered = probe.callback_count ==
                   sizeof(expectedTypes) /
                       sizeof(expectedTypes[0]);
    for (uint32_t index = 0;
         index < sizeof(expectedTypes) /
                     sizeof(expectedTypes[0]) &&
         ordered;
         ++index) {
        ordered =
            probe.types[index] == expectedTypes[index] &&
            (index == 0 ||
             probe.sequences[index - 1] <
                 probe.sequences[index]);
    }
    Expect(ordered, "gameplay event FIFO and type order");
    Expect(
        probe.structures_valid &&
            probe.main_thread_only &&
            probe.payloads_valid &&
            probe.vehicles_valid,
        "gameplay event payloads and borrowed vehicles");
    ExpectResult(
        probe.borrowed_release_result,
        WOTBMOD_ERROR_ACCESS_DENIED,
        "borrowed event vehicle cannot be released");
    ExpectResult(
        probe.clone_result,
        WOTBMOD_OK,
        "borrowed event vehicle cloned");
    WotbModVehicleInfo staleInfo = {};
    staleInfo.struct_size = sizeof(staleInfo);
    ExpectResult(
        host->vehicle_get_info(
            owner,
            probe.stale_borrowed_vehicle,
            &staleInfo),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "borrowed event vehicle invalid after callback");
    WotbModVehicleInfo clonedInfo = {};
    clonedInfo.struct_size = sizeof(clonedInfo);
    ExpectResult(
        host->vehicle_get_info(
            owner,
            probe.cloned_vehicle,
            &clonedInfo),
        WOTBMOD_OK,
        "cloned event vehicle survives callback");
    Expect(
        clonedInfo.entity_id == 101u,
        "cloned event vehicle value");
    ExpectResult(
        host->vehicle_release(
            owner, probe.cloned_vehicle),
        WOTBMOD_OK,
        "cloned event vehicle release");
    ExpectResult(
        host->event_unsubscribe(owner, subscription),
        WOTBMOD_OK,
        "gameplay event unsubscribe");
    Expect(
        g_vehicleTokensActive == 0 &&
            g_vehicleTokensCreated ==
                g_vehicleTokensReleased,
        "gameplay token ownership balanced");
}

static void TestMainThreadAndObjects(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    WotbModResourceHandle uiParent = nullptr;
    WotbModResourceHandle uiChild = nullptr;
    WotbModResourceHandle sceneParent = nullptr;
    WotbModResourceHandle sceneChild = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_UI_CONTROL,
            "~res:/Typed/object-parent.yaml",
            "Parent",
            0,
            &uiParent),
        WOTBMOD_OK,
        "UI object parent load");
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_UI_CONTROL,
            "~res:/Typed/object-child.yaml",
            "Child",
            0,
            &uiChild),
        WOTBMOD_OK,
        "UI object child load");
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_SCENE,
            "~res:/Typed/object-parent.sc2",
            nullptr,
            0,
            &sceneParent),
        WOTBMOD_OK,
        "scene object parent load");
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_SCENE,
            "~res:/Typed/object-child.sc2",
            nullptr,
            0,
            &sceneChild),
        WOTBMOD_OK,
        "scene object child load");

    WotbModUiControlGeometry geometry = {};
    geometry.struct_size = sizeof(geometry);
    geometry.x = 10.0f;
    geometry.y = 20.0f;
    geometry.width = 320.0f;
    geometry.height = 180.0f;
    WotbModSceneTransform transform = {};
    transform.struct_size = sizeof(transform);
    transform.position_x = 1.0f;
    transform.position_y = 2.0f;
    transform.position_z = 3.0f;
    transform.rotation_w = 1.0f;
    transform.scale_x = 1.0f;
    transform.scale_y = 2.0f;
    transform.scale_z = 3.0f;

    WotbModResourceHandle createdUi = nullptr;
    WotbModResourceHandle activeScreen = nullptr;
    WotbModResourceHandle createdEntity = nullptr;
    WotbModResourceHandle activeScene = nullptr;
    ExpectResult(
        host->ui_control_create(owner, &geometry, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI create null output");
    ExpectResult(
        host->ui_get_active_screen(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "active UI screen null output");
    ExpectResult(
        host->scene_entity_create(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene entity create null output");
    ExpectResult(
        host->scene_get_active(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "active scene null output");

    const LONG uiCreatesBefore = g_uiCreates;
    const LONG screenGetsBefore = g_uiScreenGets;
    const LONG entityCreatesBefore = g_sceneEntityCreates;
    const LONG sceneGetsBefore = g_sceneGets;
    ExpectResult(
        host->ui_control_create(owner, &geometry, &createdUi),
        WOTBMOD_OK,
        "UI control factory");
    ExpectResult(
        host->ui_get_active_screen(owner, &activeScreen),
        WOTBMOD_OK,
        "active UI screen");
    ExpectResult(
        host->scene_entity_create(owner, &createdEntity),
        WOTBMOD_OK,
        "scene entity factory");
    ExpectResult(
        host->scene_get_active(owner, &activeScene),
        WOTBMOD_OK,
        "active scene");
    Expect(
        createdUi && activeScreen && createdEntity && activeScene &&
            g_uiCreates == uiCreatesBefore + 1 &&
            g_uiScreenGets == screenGetsBefore + 1 &&
            g_sceneEntityCreates == entityCreatesBefore + 1 &&
            g_sceneGets == sceneGetsBefore + 1,
        "object factories publish owned handles");
    ExpectResult(
        host->resource_reload(owner, createdEntity),
        WOTBMOD_ERROR_PLATFORM,
        "created scene entity is not reloadable");
    ExpectResult(
        host->ui_control_add_child(owner, activeScreen, createdUi),
        WOTBMOD_OK,
        "attach created UI control to active screen");
    ExpectResult(
        host->scene_add_child(owner, activeScene, createdEntity),
        WOTBMOD_OK,
        "attach created entity to active scene");
    ExpectResult(
        host->scene_set_transform(owner, createdEntity, &transform),
        WOTBMOD_OK,
        "transform created entity");
    ExpectResult(
        host->ui_control_remove_child(owner, activeScreen, createdUi),
        WOTBMOD_OK,
        "detach created UI control");
    ExpectResult(
        host->scene_remove_child(owner, activeScene, createdEntity),
        WOTBMOD_OK,
        "detach created scene entity");

    InterlockedExchange(&g_objectFaultMode, 8);
    WotbModResourceHandle faultedCreate = nullptr;
    ExpectResult(
        host->ui_control_create(owner, nullptr, &faultedCreate),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "UI factory backend fault isolation");
    Expect(
        faultedCreate == nullptr,
        "faulted UI factory does not publish a handle");

    FactoryWorkerContext factoryWorker = {};
    factoryWorker.host = host;
    factoryWorker.owner = owner;
    HANDLE factoryThread = CreateThread(
        nullptr, 0, &FactoryWorker, &factoryWorker, 0, nullptr);
    Expect(factoryThread != nullptr, "factory worker thread create");
    if (factoryThread) {
        Expect(
            WaitForSingleObject(factoryThread, 5000) == WAIT_OBJECT_0,
            "factory worker thread join");
        CloseHandle(factoryThread);
        ExpectResult(
            factoryWorker.ui_create_result,
            WOTBMOD_ERROR_WRONG_THREAD,
            "UI factory rejects worker thread");
        ExpectResult(
            factoryWorker.scene_get_result,
            WOTBMOD_ERROR_WRONG_THREAD,
            "active scene getter rejects worker thread");
        Expect(
            !factoryWorker.control && !factoryWorker.scene,
            "wrong-thread factories publish no handles");
    }

    ExpectResult(
        host->main_thread_enqueue(nullptr, &MainThreadProbe, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "main thread enqueue invalid owner");
    ExpectResult(
        host->main_thread_enqueue(owner, nullptr, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "main thread enqueue null callback");

    WotbModUiControlGeometry shortGeometry = geometry;
    shortGeometry.struct_size =
        (uint32_t)offsetof(WotbModUiControlGeometry, height);
    WotbModResourceHandle invalidCreatedUi = nullptr;
    ExpectResult(
        host->ui_control_create(
            owner, &shortGeometry, &invalidCreatedUi),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI factory rejects short geometry");
    ExpectResult(
        host->ui_control_set_geometry(
            owner, uiParent, &shortGeometry),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI geometry short structure");
    WotbModUiControlGeometry negativeGeometry = geometry;
    negativeGeometry.width = -1.0f;
    ExpectResult(
        host->ui_control_set_geometry(
            owner, uiParent, &negativeGeometry),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI geometry negative size");

    WotbModSceneTransform shortTransform = transform;
    shortTransform.struct_size =
        (uint32_t)offsetof(WotbModSceneTransform, scale_z);
    ExpectResult(
        host->scene_set_transform(
            owner, sceneParent, &shortTransform),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene transform short structure");
    WotbModSceneTransform noRotation = transform;
    noRotation.rotation_w = 0.0f;
    ExpectResult(
        host->scene_set_transform(
            owner, sceneParent, &noRotation),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene transform zero quaternion");
    WotbModSceneTransform zeroScale = transform;
    zeroScale.scale_y = 0.0f;
    ExpectResult(
        host->scene_set_transform(
            owner, sceneParent, &zeroScale),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene transform zero scale");

    ExpectResult(
        host->ui_control_set_visible(owner, sceneParent, 1),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI operation rejects scene");
    ExpectResult(
        host->scene_set_transform(owner, uiParent, &transform),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene operation rejects UI control");
    ExpectResult(
        host->ui_control_set_visible(peer, uiParent, 1),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI operation rejects foreign owner");
    ExpectResult(
        host->ui_control_add_child(owner, uiParent, uiParent),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "UI hierarchy rejects self parent");
    ExpectResult(
        host->scene_add_child(owner, sceneParent, sceneParent),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "scene hierarchy rejects self parent");

    const LONG geometryBefore = g_uiGeometrySets;
    const LONG visibilityBefore = g_uiVisibilitySets;
    const LONG uiAddsBefore = g_uiChildAdds;
    const LONG uiRemovesBefore = g_uiChildRemoves;
    const LONG transformBefore = g_sceneTransformSets;
    const LONG sceneAddsBefore = g_sceneChildAdds;
    const LONG sceneRemovesBefore = g_sceneChildRemoves;
    ExpectResult(
        host->ui_control_set_geometry(owner, uiParent, &geometry),
        WOTBMOD_OK,
        "UI geometry immediate");
    ExpectResult(
        host->ui_control_set_visible(owner, uiParent, 0),
        WOTBMOD_OK,
        "UI visibility immediate");
    ExpectResult(
        host->ui_control_add_child(owner, uiParent, uiChild),
        WOTBMOD_OK,
        "UI add child immediate");

    uint32_t childCount = 0;
    WotbModResourceHandle childAt = nullptr;
    WotbModResourceHandle foundChild = nullptr;
    WotbModResourceHandle foundRoot = nullptr;
    WotbModResourceHandle queriedParent = nullptr;
    ExpectResult(
        host->ui_control_get_child_count(
            owner, uiParent, &childCount),
        WOTBMOD_OK,
        "UI child count query");
    Expect(childCount == 1, "UI child count value");
    ExpectResult(
        host->ui_control_get_child_at(
            owner, uiParent, 0, &childAt),
        WOTBMOD_OK,
        "UI child index query");
    ExpectResult(
        host->ui_control_find_by_name(
            owner,
            uiParent,
            "Child",
            0,
            &foundChild),
        WOTBMOD_OK,
        "UI find existing control by name");
    ExpectResult(
        host->ui_control_find_by_name(
            owner,
            uiParent,
            "Parent",
            1,
            &foundRoot),
        WOTBMOD_OK,
        "UI find includes the supplied matching root");
    ExpectResult(
        host->ui_control_get_parent(
            owner, uiChild, &queriedParent),
        WOTBMOD_OK,
        "UI parent query");

    WotbModUiControlState parentState = {};
    parentState.struct_size = sizeof(parentState);
    ExpectResult(
        host->ui_control_get_state(
            owner, uiParent, &parentState),
        WOTBMOD_OK,
        "UI state query");
    Expect(
        parentState.geometry.x == geometry.x &&
            parentState.geometry.width == geometry.width &&
            (parentState.flags &
             WOTBMOD_UI_CONTROL_VISIBLE) == 0 &&
            (parentState.flags &
             WOTBMOD_UI_CONTROL_INPUT_ENABLED) != 0,
        "UI state geometry and flags");
    ExpectResult(
        host->ui_control_set_input_enabled(
            owner, uiParent, 0, 1),
        WOTBMOD_OK,
        "UI hierarchical input disable");
    ExpectResult(
        host->ui_control_set_disabled(
            owner, uiParent, 1, 1),
        WOTBMOD_OK,
        "UI hierarchical disabled state");
    WotbModUiControlState childState = {};
    childState.struct_size = sizeof(childState);
    ExpectResult(
        host->ui_control_get_state(
            owner, uiChild, &childState),
        WOTBMOD_OK,
        "UI child state after hierarchical flags");
    Expect(
        (childState.flags &
         WOTBMOD_UI_CONTROL_INPUT_ENABLED) == 0 &&
            (childState.flags &
             WOTBMOD_UI_CONTROL_DISABLED) != 0,
        "UI hierarchical flags reached child");
    WotbModResourceHandle missingControl = nullptr;
    ExpectResult(
        host->ui_control_get_child_at(
            owner, uiParent, 1, &missingControl),
        WOTBMOD_ERROR_NOT_FOUND,
        "UI child index bounds");
    ExpectResult(
        host->ui_control_find_by_name(
            owner,
            uiParent,
            "Missing",
            1,
            &missingControl),
        WOTBMOD_ERROR_NOT_FOUND,
        "UI missing name");
    ExpectResult(
        host->resource_release(owner, childAt),
        WOTBMOD_OK,
        "UI child query handle release");
    ExpectResult(
        host->resource_release(owner, foundChild),
        WOTBMOD_OK,
        "UI find query handle release");
    ExpectResult(
        host->resource_release(owner, foundRoot),
        WOTBMOD_OK,
        "UI root find query handle release");
    ExpectResult(
        host->resource_release(owner, queriedParent),
        WOTBMOD_OK,
        "UI parent query handle release");

    ExpectResult(
        host->ui_control_remove_child(owner, uiParent, uiChild),
        WOTBMOD_OK,
        "UI remove child immediate");
    ExpectResult(
        host->scene_set_transform(owner, sceneParent, &transform),
        WOTBMOD_OK,
        "scene transform immediate");
    ExpectResult(
        host->scene_add_child(owner, sceneParent, sceneChild),
        WOTBMOD_OK,
        "scene add child immediate");
    ExpectResult(
        host->scene_remove_child(owner, sceneParent, sceneChild),
        WOTBMOD_OK,
        "scene remove child immediate");
    Expect(
        g_uiGeometrySets == geometryBefore + 1 &&
            g_uiVisibilitySets == visibilityBefore + 1 &&
            g_uiChildAdds == uiAddsBefore + 1 &&
            g_uiChildRemoves == uiRemovesBefore + 1 &&
            g_sceneTransformSets == transformBefore + 1 &&
            g_sceneChildAdds == sceneAddsBefore + 1 &&
            g_sceneChildRemoves == sceneRemovesBefore + 1,
        "main-thread object operations execute synchronously");

    InterlockedExchange(&g_objectFaultMode, 1);
    ExpectResult(
        host->ui_control_set_geometry(owner, uiParent, &geometry),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "UI backend fault isolation");
    ExpectResult(
        host->ui_control_set_geometry(owner, uiParent, &geometry),
        WOTBMOD_OK,
        "UI backend retry after fault");

    InterlockedExchange(&g_mainThreadCallbackCount, 0);
    InterlockedExchange(&g_mainThreadCallbackOrderCount, 0);
    ZeroMemory(
        g_mainThreadCallbackOrder,
        sizeof(g_mainThreadCallbackOrder));
    for (int32_t index = 0; index < 64; ++index) {
        ExpectResult(
            host->main_thread_enqueue(
                owner,
                &MainThreadProbe,
                reinterpret_cast<void*>(
                    static_cast<intptr_t>(index + 1))),
            WOTBMOD_OK,
            "main thread queue fill");
    }
    ExpectResult(
        host->main_thread_enqueue(owner, &MainThreadProbe, nullptr),
        WOTBMOD_ERROR_LIMIT_REACHED,
        "main thread queue per-mod limit");
    Expect(
        g_mainThreadCallbackCount == 0,
        "main thread callbacks always deferred");
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    bool fifo = g_mainThreadCallbackCount == 64 &&
                g_mainThreadCallbackOrderCount == 64;
    for (int32_t index = 0; index < 64 && fifo; ++index) {
        fifo = g_mainThreadCallbackOrder[index] == index + 1;
    }
    Expect(fifo, "main thread callback FIFO");

    ObjectWorkerContext worker = {};
    worker.host = host;
    worker.owner = owner;
    worker.control = uiChild;
    worker.scene = sceneChild;
    worker.visible_result = WOTBMOD_ERROR_PLATFORM;
    worker.transform_result = WOTBMOD_ERROR_PLATFORM;
    worker.transform = transform;
    const LONG workerVisibilityBefore = g_uiVisibilitySets;
    const LONG workerTransformBefore = g_sceneTransformSets;
    HANDLE thread = CreateThread(
        nullptr, 0, &ObjectWorker, &worker, 0, nullptr);
    Expect(thread != nullptr, "object worker thread create");
    if (thread) {
        Expect(
            WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0,
            "object worker thread join");
        CloseHandle(thread);
        ExpectResult(
            worker.visible_result,
            WOTBMOD_OK,
            "worker UI operation accepted");
        ExpectResult(
            worker.transform_result,
            WOTBMOD_OK,
            "worker scene operation accepted");
        Expect(
            g_uiVisibilitySets == workerVisibilityBefore &&
                g_sceneTransformSets == workerTransformBefore,
            "worker object operations deferred");
        WotbModRuntime_DispatchFrame(
            nullptr, nullptr, nullptr, 1280, 720, 0.016);
        Expect(
            g_uiVisibilitySets == workerVisibilityBefore + 1 &&
                g_sceneTransformSets == workerTransformBefore + 1,
            "worker object operations drained on main thread");
    }

    InterlockedExchange(&g_mainThreadCallbackCount, 0);
    InterlockedExchange(&g_mainThreadCallbackOrderCount, 0);
    ExpectResult(
        host->main_thread_enqueue(
            peer,
            &MainThreadProbe,
            reinterpret_cast<void*>(99)),
        WOTBMOD_OK,
        "peer callback queued for cancellation");
    ExpectResult(
        host->set_mod_enabled("test.peer", 0),
        WOTBMOD_OK,
        "disable peer cancels queue");
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    Expect(
        g_mainThreadCallbackCount == 0,
        "disabled mod callback discarded");
    ExpectResult(
        host->main_thread_enqueue(peer, &MainThreadProbe, nullptr),
        WOTBMOD_ERROR_DISABLED,
        "disabled mod cannot enqueue");
    ExpectResult(
        host->set_mod_enabled("test.peer", 1),
        WOTBMOD_OK,
        "peer re-enabled after queue cancellation");

    ExpectResult(
        host->resource_release(owner, uiChild),
        WOTBMOD_OK,
        "UI child release");
    ExpectResult(
        host->ui_control_set_visible(owner, uiChild, 1),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "stale UI object handle rejected");
    ExpectResult(
        host->resource_release(owner, uiParent),
        WOTBMOD_OK,
        "UI parent release");
    ExpectResult(
        host->resource_release(owner, sceneChild),
        WOTBMOD_OK,
        "scene child release");
    ExpectResult(
        host->resource_release(owner, sceneParent),
        WOTBMOD_OK,
        "scene parent release");
    ExpectResult(
        host->resource_release(owner, createdUi),
        WOTBMOD_OK,
        "created UI control release");
    ExpectResult(
        host->resource_release(owner, activeScreen),
        WOTBMOD_OK,
        "active UI screen release");
    ExpectResult(
        host->resource_release(owner, createdEntity),
        WOTBMOD_OK,
        "created scene entity release");
    ExpectResult(
        host->resource_release(owner, activeScene),
        WOTBMOD_OK,
        "active scene release");

    ExpectResult(
        host->main_thread_enqueue(
            peer, &FaultingMainThreadProbe, nullptr),
        WOTBMOD_OK,
        "faulting main thread callback queued");
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    const int peerIndex = FindModIndex(host, "test.peer");
    WotbModPublicInfo peerInfo = {};
    peerInfo.struct_size = sizeof(peerInfo);
    Expect(
        peerIndex >= 0 &&
            host->get_mod_info(
                static_cast<uint32_t>(peerIndex),
                &peerInfo) == WOTBMOD_OK &&
            peerInfo.state == WOTBMOD_STATE_FAULTED &&
            peerInfo.enabled == 0 &&
            peerInfo.fault_count == 1,
        "main thread callback fault isolates owner mod");
}

static void TestClientEventFaultCleanup(
    const WotbModHostApi* host,
    WotbModHandle owner) {
    InterlockedExchange(&g_faultingEventCallbackCount, 0);
    WotbModEventSubscriptionId subscription = 0;
    ExpectResult(
        host->event_subscribe(
            owner,
            WOTBMOD_EVENT_SCENE_ACTIVATED,
            &FaultingClientEventCallback,
            nullptr,
            &subscription),
        WOTBMOD_OK,
        "faulting client event subscription");

    void* scene = nullptr;
    ExpectResult(
        CreateObjectResource(
            WOTBMOD_RESOURCE_SCENE,
            nullptr,
            100,
            &g_sceneEntityCreates,
            &scene),
        WOTBMOD_OK,
        "faulting event scene fixture");
    ExpectResult(
        WotbModRuntime_NotifySceneActivated(scene),
        WOTBMOD_OK,
        "queue faulting client event");
    ExpectResult(
        ResourceRelease(nullptr, scene),
        WOTBMOD_OK,
        "release faulting event scene fixture");
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);

    const int ownerIndex = FindModIndex(host, "test.contract");
    WotbModPublicInfo ownerInfo = {};
    ownerInfo.struct_size = sizeof(ownerInfo);
    Expect(
        g_faultingEventCallbackCount == 1 &&
            ownerIndex >= 0 &&
            host->get_mod_info(
                static_cast<uint32_t>(ownerIndex),
                &ownerInfo) == WOTBMOD_OK &&
            ownerInfo.state == WOTBMOD_STATE_FAULTED &&
            ownerInfo.enabled == 0 &&
            ownerInfo.fault_count == 1,
        "client event callback fault isolates owner mod");
    ExpectResult(
        host->event_unsubscribe(owner, subscription),
        WOTBMOD_ERROR_NOT_FOUND,
        "fault cleanup removes event subscription");

    scene = nullptr;
    ExpectResult(
        CreateObjectResource(
            WOTBMOD_RESOURCE_SCENE,
            nullptr,
            100,
            &g_sceneEntityCreates,
            &scene),
        WOTBMOD_OK,
        "post-fault event scene fixture");
    ExpectResult(
        WotbModRuntime_NotifySceneActivated(scene),
        WOTBMOD_OK,
        "queue event after subscription fault cleanup");
    ExpectResult(
        ResourceRelease(nullptr, scene),
        WOTBMOD_OK,
        "release post-fault event scene fixture");
    WotbModRuntime_DispatchFrame(
        nullptr, nullptr, nullptr, 1280, 720, 0.016);
    Expect(
        g_faultingEventCallbackCount == 1,
        "faulted subscription never invoked again");
}

static WotbModAudioPlayInfo MakeAudioPlayInfo(uint32_t flags) {
    WotbModAudioPlayInfo info = {};
    info.struct_size = sizeof(info);
    info.flags = flags;
    info.volume = 0.75f;
    info.pitch = 1.25f;
    info.pan = -0.25f;
    info.position_x = 10.0f;
    info.position_y = 20.0f;
    info.position_z = 30.0f;
    info.min_distance = 2.0f;
    info.max_distance = 250.0f;
    return info;
}

static bool WriteSilentPcmWave(const char* path) {
    if (!path || !path[0]) return false;
    const uint32_t sampleRate = 8000u;
    const uint32_t sampleCount = sampleRate / 2u;
    const uint32_t dataSize = sampleCount * sizeof(int16_t);
    TestWaveHeader header = {};
    memcpy(header.riff, "RIFF", 4);
    header.riff_size =
        sizeof(TestWaveHeader) - 8u + dataSize;
    memcpy(header.wave, "WAVE", 4);
    memcpy(header.format_tag, "fmt ", 4);
    header.format_size = 16u;
    header.audio_format = 1u;
    header.channels = 1u;
    header.sample_rate = sampleRate;
    header.byte_rate = sampleRate * sizeof(int16_t);
    header.block_align = sizeof(int16_t);
    header.bits_per_sample = 16u;
    memcpy(header.data_tag, "data", 4);
    header.data_size = dataSize;

    HANDLE file = CreateFileA(
        path,
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok =
        WriteFile(
            file, &header, sizeof(header), &written, nullptr) &&
        written == sizeof(header);
    int16_t samples[4000] = {};
    if (ok) {
        written = 0;
        ok = WriteFile(
                 file,
                 samples,
                 sizeof(samples),
                 &written,
                 nullptr) &&
             written == sizeof(samples);
    }
    CloseHandle(file);
    return ok;
}

static void TestAudio(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    WotbModResourceHandle generic = nullptr;
    WotbModResourceHandle clip = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Audio/not-a-clip",
            nullptr,
            0,
            &generic),
        WOTBMOD_OK,
        "audio generic resource setup");
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_AUDIO_CLIP,
            "Data/Audio/test.wav",
            nullptr,
            0x44u,
            &clip),
        WOTBMOD_OK,
        "audio clip resource load");

    WotbModAudioPlaybackHandle playback = nullptr;
    WotbModAudioPlayInfo info =
        MakeAudioPlayInfo(WOTBMOD_AUDIO_PLAY_LOOP);
    ExpectResult(
        host->audio_play(nullptr, clip, &info, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play invalid owner");
    ExpectResult(
        host->audio_play(owner, nullptr, &info, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play null clip");
    ExpectResult(
        host->audio_play(owner, generic, &info, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play rejects generic resource");
    ExpectResult(
        host->audio_play(owner, clip, &info, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play null output");

    WotbModAudioPlayInfo invalid = info;
    invalid.struct_size =
        (uint32_t)offsetof(WotbModAudioPlayInfo, max_distance);
    ExpectResult(
        host->audio_play(owner, clip, &invalid, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play short structure");
    invalid = info;
    invalid.flags = 0x80000000u;
    ExpectResult(
        host->audio_play(owner, clip, &invalid, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play invalid flags");
    invalid = info;
    invalid.volume = -0.01f;
    ExpectResult(
        host->audio_play(owner, clip, &invalid, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play negative volume");
    invalid = info;
    invalid.pitch = 0.0f;
    ExpectResult(
        host->audio_play(owner, clip, &invalid, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play zero pitch");
    invalid = info;
    invalid.pan = 1.01f;
    ExpectResult(
        host->audio_play(owner, clip, &invalid, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play invalid pan");
    invalid = info;
    invalid.min_distance = 300.0f;
    invalid.max_distance = 200.0f;
    ExpectResult(
        host->audio_play(owner, clip, &invalid, &playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_play invalid distance range");

    ExpectResult(
        host->audio_play(owner, clip, nullptr, &playback),
        WOTBMOD_OK,
        "audio_play defaults");
    Expect(
        playback != nullptr &&
            g_lastAudioPlayInfo.struct_size ==
                sizeof(g_lastAudioPlayInfo) &&
            g_lastAudioPlayInfo.flags == WOTBMOD_AUDIO_PLAY_NONE &&
            g_lastAudioPlayInfo.volume == 1.0f &&
            g_lastAudioPlayInfo.pitch == 1.0f &&
            g_lastAudioPlayInfo.pan == 0.0f &&
            g_lastAudioPlayInfo.min_distance == 1.0f &&
            g_lastAudioPlayInfo.max_distance == 100.0f,
        "audio default parameters normalized");
    ExpectResult(
        host->resource_release(owner, clip),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "audio clip retained while playback active");

    WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
    ExpectResult(
        host->audio_get_state(owner, playback, &state),
        WOTBMOD_OK,
        "audio_get_state playing");
    Expect(state == WOTBMOD_AUDIO_PLAYING, "audio initial state");
    ExpectResult(
        host->audio_pause(owner, playback),
        WOTBMOD_OK,
        "audio_pause");
    ExpectResult(
        host->audio_get_state(owner, playback, &state),
        WOTBMOD_OK,
        "audio_get_state paused");
    Expect(state == WOTBMOD_AUDIO_PAUSED, "audio paused state");
    ExpectResult(
        host->audio_resume(owner, playback),
        WOTBMOD_OK,
        "audio_resume");

    WotbModAudioPlayInfo updated = MakeAudioPlayInfo(
        WOTBMOD_AUDIO_PLAY_LOOP |
        WOTBMOD_AUDIO_PLAY_SPATIAL);
    ExpectResult(
        host->audio_set_parameters(owner, playback, &updated),
        WOTBMOD_OK,
        "audio_set_parameters");
    invalid = updated;
    invalid.flags |= WOTBMOD_AUDIO_PLAY_START_PAUSED;
    ExpectResult(
        host->audio_set_parameters(owner, playback, &invalid),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio parameters reject start-paused");
    Expect(g_audioParameterSets > 0, "audio parameters reached backend");

    ExpectResult(
        host->audio_stop(owner, playback),
        WOTBMOD_OK,
        "audio_stop");
    ExpectResult(
        host->audio_get_state(owner, playback, &state),
        WOTBMOD_OK,
        "audio_get_state stopped");
    Expect(state == WOTBMOD_AUDIO_STOPPED, "audio stopped state");
    ExpectResult(
        host->audio_pause(peer, playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_pause foreign handle");
    ExpectResult(
        host->audio_release(peer, playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_release foreign handle");
    ExpectResult(
        host->audio_release(owner, playback),
        WOTBMOD_OK,
        "audio_release");
    ExpectResult(
        host->audio_release(owner, playback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_release repeated");

    WotbModAudioPlaybackHandle oldPlayback = nullptr;
    WotbModAudioPlaybackHandle newPlayback = nullptr;
    ExpectResult(
        host->audio_play(owner, clip, &info, &oldPlayback),
        WOTBMOD_OK,
        "audio stale first play");
    ExpectResult(
        host->audio_release(owner, oldPlayback),
        WOTBMOD_OK,
        "audio stale first release");
    ExpectResult(
        host->audio_play(owner, clip, &info, &newPlayback),
        WOTBMOD_OK,
        "audio stale second play");
    Expect(oldPlayback != newPlayback, "audio handle generation changes");
    ExpectResult(
        host->audio_release(owner, oldPlayback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "stale audio handle rejected");
    ExpectResult(
        host->audio_get_state(owner, newPlayback, &state),
        WOTBMOD_OK,
        "new audio survives stale handle");
    ExpectResult(
        host->audio_release(owner, newPlayback),
        WOTBMOD_OK,
        "new audio release");

    WotbModAudioPlaybackHandle playbacks[64] = {};
    for (uint32_t index = 0; index < 64; ++index) {
        ExpectResult(
            host->audio_play(owner, clip, &info, &playbacks[index]),
            WOTBMOD_OK,
            "audio playback limit fill");
    }
    playback = nullptr;
    ExpectResult(
        host->audio_play(owner, clip, &info, &playback),
        WOTBMOD_ERROR_LIMIT_REACHED,
        "audio playback per-mod limit");
    for (uint32_t index = 0; index < 64; ++index) {
        ExpectResult(
            host->audio_release(owner, playbacks[index]),
            WOTBMOD_OK,
            "audio playback limit cleanup");
    }

    InterlockedExchange(&g_audioFaultMode, 1);
    ExpectResult(
        host->audio_play(owner, clip, &info, &playback),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "audio play backend fault isolation");
    InterlockedExchange(&g_audioFaultMode, 8);
    ExpectResult(
        host->audio_play(owner, clip, &info, &playback),
        WOTBMOD_ERROR_PLATFORM,
        "audio play OK with null native");
    ExpectResult(
        host->audio_play(owner, clip, &info, &playback),
        WOTBMOD_OK,
        "audio operation fault setup");

    InterlockedExchange(&g_audioFaultMode, 2);
    ExpectResult(
        host->audio_pause(owner, playback),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "audio pause backend fault isolation");
    InterlockedExchange(&g_audioFaultMode, 5);
    ExpectResult(
        host->audio_set_parameters(owner, playback, &updated),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "audio parameters backend fault isolation");
    InterlockedExchange(&g_audioFaultMode, 6);
    ExpectResult(
        host->audio_get_state(owner, playback, &state),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "audio state backend fault isolation");
    InterlockedExchange(&g_audioFaultMode, 9);
    ExpectResult(
        host->audio_get_state(owner, playback, &state),
        WOTBMOD_ERROR_PLATFORM,
        "audio invalid backend state");
    InterlockedExchange(&g_audioFaultMode, 4);
    ExpectResult(
        host->audio_stop(owner, playback),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "audio stop backend fault isolation");
    InterlockedExchange(&g_audioFaultMode, 7);
    ExpectResult(
        host->audio_release(owner, playback),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "audio release backend fault isolation");
    ExpectResult(
        host->resource_release(owner, clip),
        WOTBMOD_ERROR_ACCESS_DENIED,
        "audio release fault keeps clip retained");
    ExpectResult(
        host->audio_release(owner, playback),
        WOTBMOD_OK,
        "audio release retry after backend fault");

    ExpectResult(
        host->audio_pause(owner, nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_pause null");
    ExpectResult(
        host->audio_get_state(owner, nullptr, &state),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_get_state null");
    ExpectResult(
        host->audio_get_state(
            owner,
            reinterpret_cast<WotbModAudioPlaybackHandle>(1),
            nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "audio_get_state null output");

    ExpectResult(
        host->resource_release(owner, generic),
        WOTBMOD_OK,
        "audio generic resource cleanup");
    ExpectResult(
        host->resource_release(owner, clip),
        WOTBMOD_OK,
        "audio clip resource cleanup");
    Expect(g_playbackActive == 0, "audio playback allocations balanced");
}

static void TestSoundEvents(
    const WotbModHostApi* host,
    WotbModHandle owner,
    WotbModHandle peer) {
    WotbModSoundEventHandle event = nullptr;
    ExpectResult(
        host->sound_event_create(nullptr, "ui.test", &event),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound create invalid owner");
    ExpectResult(
        host->sound_event_create(owner, nullptr, &event),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound create null name");
    ExpectResult(
        host->sound_event_create(owner, "", &event),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound create empty name");
    ExpectResult(
        host->sound_event_create(owner, "ui.test", nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound create null output");
    ExpectResult(
        host->sound_event_create(owner, "ui.test", &event),
        WOTBMOD_OK,
        "sound create");
    Expect(event != nullptr, "sound create handle");

    uint32_t nameSize = 0;
    ExpectResult(
        host->sound_event_get_name(owner, event, nullptr, &nameSize),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "sound name size query");
    Expect(nameSize == 8, "sound name required size");
    char shortName[4] = {};
    uint32_t shortNameSize = sizeof(shortName);
    ExpectResult(
        host->sound_event_get_name(
            owner, event, shortName, &shortNameSize),
        WOTBMOD_ERROR_BUFFER_TOO_SMALL,
        "sound name short buffer");
    char name[WOTBMOD_MAX_SOUND_EVENT_NAME] = {};
    uint32_t fullNameSize = sizeof(name);
    ExpectResult(
        host->sound_event_get_name(
            owner, event, name, &fullNameSize),
        WOTBMOD_OK,
        "sound name");
    Expect(strcmp(name, "ui.test") == 0, "sound name value");

    WotbModAudioState state = WOTBMOD_AUDIO_PLAYING;
    ExpectResult(
        host->sound_event_get_state(owner, event, &state),
        WOTBMOD_OK,
        "sound initial state");
    Expect(state == WOTBMOD_AUDIO_STOPPED, "sound initially stopped");
    ExpectResult(
        host->sound_event_trigger(owner, event),
        WOTBMOD_OK,
        "sound trigger");
    ExpectResult(
        host->sound_event_get_state(owner, event, &state),
        WOTBMOD_OK,
        "sound playing state");
    Expect(state == WOTBMOD_AUDIO_PLAYING, "sound playing");
    ExpectResult(
        host->sound_event_set_paused(owner, event, 1),
        WOTBMOD_OK,
        "sound pause");
    ExpectResult(
        host->sound_event_get_state(owner, event, &state),
        WOTBMOD_OK,
        "sound paused state");
    Expect(state == WOTBMOD_AUDIO_PAUSED, "sound paused");
    ExpectResult(
        host->sound_event_set_paused(owner, event, 0),
        WOTBMOD_OK,
        "sound resume");
    ExpectResult(
        host->sound_event_set_paused(owner, event, 2),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound invalid pause value");
    ExpectResult(
        host->sound_event_set_volume(owner, event, 0.35f),
        WOTBMOD_OK,
        "sound volume");
    ExpectResult(
        host->sound_event_set_volume(owner, event, -0.1f),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound negative volume");
    ExpectResult(
        host->sound_event_set_position(owner, event, 1.0f, 2.0f, 3.0f),
        WOTBMOD_OK,
        "sound position");

    int32_t hasParameter = 1;
    ExpectResult(
        host->sound_event_has_parameter(
            owner, event, "speed", &hasParameter),
        WOTBMOD_OK,
        "sound parameter absent query");
    Expect(hasParameter == 0, "sound parameter initially absent");
    ExpectResult(
        host->sound_event_set_parameter(
            owner, event, "speed", 0.75f),
        WOTBMOD_OK,
        "sound set parameter");
    ExpectResult(
        host->sound_event_has_parameter(
            owner, event, "speed", &hasParameter),
        WOTBMOD_OK,
        "sound parameter present query");
    Expect(hasParameter == 1, "sound parameter present");
    float parameterValue = 0.0f;
    ExpectResult(
        host->sound_event_get_parameter(
            owner, event, "speed", &parameterValue),
        WOTBMOD_OK,
        "sound get parameter");
    Expect(parameterValue == 0.75f, "sound parameter value");
    ExpectResult(
        host->sound_event_get_parameter(
            owner, event, "missing", &parameterValue),
        WOTBMOD_ERROR_NOT_FOUND,
        "sound missing parameter");
    ExpectResult(
        host->sound_event_set_parameter(
            owner, event, nullptr, 1.0f),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound null parameter");
    ExpectResult(
        host->sound_event_has_parameter(
            owner, event, "speed", nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound null has output");
    ExpectResult(
        host->sound_event_get_parameter(
            owner, event, "speed", nullptr),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound null parameter output");

    ExpectResult(
        host->sound_event_trigger(peer, event),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound foreign trigger");
    ExpectResult(
        host->sound_event_release(peer, event),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound foreign release");
    ExpectResult(
        host->sound_event_stop(owner, event),
        WOTBMOD_OK,
        "sound stop");
    ExpectResult(
        host->sound_event_release(owner, event),
        WOTBMOD_OK,
        "sound release");
    ExpectResult(
        host->sound_event_release(owner, event),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound repeated release");

    WotbModSoundEventHandle oldEvent = nullptr;
    WotbModSoundEventHandle newEvent = nullptr;
    ExpectResult(
        host->sound_event_create(owner, "ui.old", &oldEvent),
        WOTBMOD_OK,
        "sound stale first create");
    ExpectResult(
        host->sound_event_release(owner, oldEvent),
        WOTBMOD_OK,
        "sound stale first release");
    ExpectResult(
        host->sound_event_create(owner, "ui.new", &newEvent),
        WOTBMOD_OK,
        "sound stale second create");
    Expect(oldEvent != newEvent, "sound handle generation changes");
    ExpectResult(
        host->sound_event_trigger(owner, oldEvent),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "sound stale handle rejected");
    ExpectResult(
        host->sound_event_release(owner, newEvent),
        WOTBMOD_OK,
        "sound stale second release");

    WotbModSoundEventHandle events[64] = {};
    for (uint32_t index = 0; index < 64; ++index) {
        ExpectResult(
            host->sound_event_create(owner, "ui.limit", &events[index]),
            WOTBMOD_OK,
            "sound event limit fill");
    }
    event = nullptr;
    ExpectResult(
        host->sound_event_create(owner, "ui.overflow", &event),
        WOTBMOD_ERROR_LIMIT_REACHED,
        "sound event per-mod limit");
    for (uint32_t index = 0; index < 64; ++index) {
        ExpectResult(
            host->sound_event_release(owner, events[index]),
            WOTBMOD_OK,
            "sound event limit cleanup");
    }

    InterlockedExchange(&g_soundFaultMode, 1);
    ExpectResult(
        host->sound_event_create(owner, "ui.fault", &event),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound create fault isolation");
    InterlockedExchange(&g_soundFaultMode, 12);
    ExpectResult(
        host->sound_event_create(owner, "ui.null", &event),
        WOTBMOD_ERROR_PLATFORM,
        "sound create OK with null native");
    ExpectResult(
        host->sound_event_create(owner, "ui.ops", &event),
        WOTBMOD_OK,
        "sound fault operation setup");

    InterlockedExchange(&g_soundFaultMode, 2);
    ExpectResult(
        host->sound_event_trigger(owner, event),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound trigger fault isolation");
    InterlockedExchange(&g_soundFaultMode, 4);
    ExpectResult(
        host->sound_event_set_paused(owner, event, 1),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound pause fault isolation");
    InterlockedExchange(&g_soundFaultMode, 5);
    ExpectResult(
        host->sound_event_set_volume(owner, event, 1.0f),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound volume fault isolation");
    InterlockedExchange(&g_soundFaultMode, 6);
    ExpectResult(
        host->sound_event_set_position(owner, event, 0.0f, 0.0f, 0.0f),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound position fault isolation");
    InterlockedExchange(&g_soundFaultMode, 8);
    ExpectResult(
        host->sound_event_set_parameter(owner, event, "speed", 1.0f),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound set parameter fault isolation");
    InterlockedExchange(&g_soundFaultMode, 9);
    ExpectResult(
        host->sound_event_get_parameter(
            owner, event, "speed", &parameterValue),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound get parameter fault isolation");
    InterlockedExchange(&g_soundFaultMode, 10);
    ExpectResult(
        host->sound_event_has_parameter(
            owner, event, "speed", &hasParameter),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound has parameter fault isolation");
    InterlockedExchange(&g_soundFaultMode, 7);
    ExpectResult(
        host->sound_event_get_state(owner, event, &state),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound state fault isolation");
    InterlockedExchange(&g_soundFaultMode, 13);
    ExpectResult(
        host->sound_event_get_state(owner, event, &state),
        WOTBMOD_ERROR_PLATFORM,
        "sound invalid backend state");
    InterlockedExchange(&g_soundFaultMode, 3);
    ExpectResult(
        host->sound_event_stop(owner, event),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound stop fault isolation");
    InterlockedExchange(&g_soundFaultMode, 11);
    ExpectResult(
        host->sound_event_release(owner, event),
        WOTBMOD_ERROR_CALLBACK_FAULT,
        "sound release fault isolation");
    ExpectResult(
        host->sound_event_release(owner, event),
        WOTBMOD_OK,
        "sound release retry");

    ExpectResult(
        host->sound_event_create(owner, "ui.cleanup", &event),
        WOTBMOD_OK,
        "sound lifecycle cleanup setup");
    ExpectResult(
        host->sound_event_trigger(owner, event),
        WOTBMOD_OK,
        "sound lifecycle cleanup trigger");
    Expect(g_soundEventActive == 1, "sound lifecycle event active");
}

static void TestLifecycle(
    const char* apiEnvironment,
    const WotbModHostApi* host,
    const ContractExports& contract,
    WotbModHandle owner) {
    Expect(contract.get_enable_count() == 1, "initial enable callback count");
    Expect(
        g_contractEnableLogs >= 1 && g_formattedLogs >= 1,
        "log and logf delivery");
    host->log(
        reinterpret_cast<WotbModHandle>(1),
        WOTBMOD_LOG_INFO,
        nullptr);

    WotbModRuntime_DispatchFrame(
        reinterpret_cast<void*>(0x11),
        reinterpret_cast<void*>(0x22),
        reinterpret_cast<void*>(0x33),
        1920,
        1080,
        0.016);
    WotbModRuntime_DispatchFrame(nullptr, nullptr, nullptr, 1280, 720, 0.0);
    Expect(contract.get_frame_count() == 2, "frame callback count");
    WotbModFrameInfo frame = {};
    frame.struct_size = sizeof(frame);
    Expect(contract.copy_last_frame(&frame) == 1, "copy last frame");
    Expect(
        frame.struct_size == sizeof(frame) &&
            frame.frame_index == 2 &&
            frame.back_buffer_width == 1280 &&
            frame.back_buffer_height == 720 &&
            frame.delta_seconds >= 0.0,
        "frame payload");

    const int faultIndex = FindModIndex(host, "test.fault");
    Expect(faultIndex >= 0, "fault mod index");
    if (faultIndex >= 0) {
        WotbModPublicInfo fault = {};
        fault.struct_size = sizeof(fault);
        ExpectResult(
            host->get_mod_info((uint32_t)faultIndex, &fault),
            WOTBMOD_OK,
            "fault mod info");
        Expect(
            fault.state == WOTBMOD_STATE_FAULTED &&
                fault.enabled == 0 &&
                fault.fault_count == 1,
            "fault isolation state");
        ExpectResult(
            host->set_mod_enabled("test.fault", 1),
            WOTBMOD_ERROR_CALLBACK_FAULT,
            "faulted mod cannot re-enable");
    }

    void* original = nullptr;
    ExpectResult(
        host->hook_create(
            owner,
            &g_hookTargets[79],
            reinterpret_cast<void*>(&DummyDetour),
            &original),
        WOTBMOD_OK,
        "cleanup hook setup");
    WotbModResourceMountId cleanupMount = 0;
    ExpectResult(
        Mount(
            host,
            owner,
            "~res:/Contract/Cleanup/",
            "low",
            1,
            0,
            &cleanupMount),
        WOTBMOD_OK,
        "cleanup mount setup");
    WotbModResourceHandle cleanupResource = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/cleanup",
            nullptr,
            0,
            &cleanupResource),
        WOTBMOD_OK,
        "cleanup resource setup");
    WotbModResourceHandle cleanupAudioClip = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_AUDIO_CLIP,
            "~res:/Audio/cleanup.wav",
            nullptr,
            0,
            &cleanupAudioClip),
        WOTBMOD_OK,
        "cleanup audio clip setup");
    WotbModAudioPlaybackHandle cleanupPlayback = nullptr;
    ExpectResult(
        host->audio_play(
            owner,
            cleanupAudioClip,
            nullptr,
            &cleanupPlayback),
        WOTBMOD_OK,
        "cleanup audio playback setup");
    WotbModVehicleSkinAsset cleanupSkinAsset = {};
    cleanupSkinAsset.struct_size = sizeof(cleanupSkinAsset);
    cleanupSkinAsset.kind = WOTBMOD_SKIN_ASSET_TEXTURE;
    cleanupSkinAsset.stock_virtual_path =
        "~res:/3d/Tanks/Test/cleanup.tex";
    cleanupSkinAsset.replacement_virtual_path =
        "~res:/Contract/Cleanup/value.txt";
    WotbModVehicleSkinDescriptor cleanupSkinDescriptor = {};
    cleanupSkinDescriptor.struct_size =
        sizeof(cleanupSkinDescriptor);
    cleanupSkinDescriptor.skin_id = "cleanup-skin";
    cleanupSkinDescriptor.vehicle_name = "TestTank";
    cleanupSkinDescriptor.assets = &cleanupSkinAsset;
    cleanupSkinDescriptor.asset_count = 1;
    cleanupSkinDescriptor.priority = 1;
    cleanupSkinDescriptor.flags =
        WOTBMOD_VEHICLE_SKIN_ENABLED;
    WotbModVehicleSkinHandle cleanupSkin = nullptr;
    ExpectResult(
        host->vehicle_skin_register(
            owner,
            &cleanupSkinDescriptor,
            &cleanupSkin),
        WOTBMOD_OK,
        "cleanup vehicle skin setup");
    const LONG releasesBeforeDisable = g_resourceReleases;
    const LONG audioStopsBeforeDisable = g_audioStops;
    const LONG audioReleasesBeforeDisable = g_audioReleases;
    const LONG hookRemovesBeforeDisable = g_hookRemoves;

    ExpectResult(
        host->set_mod_enabled(nullptr, 0),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "set_mod_enabled null id");
    ExpectResult(
        host->set_mod_enabled("", 0),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "set_mod_enabled empty id");
    ExpectResult(
        host->set_mod_enabled("missing.mod", 0),
        WOTBMOD_ERROR_NOT_FOUND,
        "set_mod_enabled missing id");
    InterlockedExchange(&g_hookFailureMode, 4);
    ExpectResult(
        host->set_mod_enabled("TEST.CONTRACT", 0),
        WOTBMOD_ERROR_PLATFORM,
        "disable reports hook cleanup failure");
    Expect(contract.get_disable_count() == 1, "disable callback count");
    Expect(
        g_resourceReleases == releasesBeforeDisable + 2 &&
            g_audioStops == audioStopsBeforeDisable + 1 &&
            g_audioReleases == audioReleasesBeforeDisable + 1 &&
            g_hookRemoves == hookRemovesBeforeDisable,
        "audio and resource cleanup continue after hook cleanup failure");
    ExpectResult(
        host->audio_release(owner, cleanupPlayback),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "cleanup audio handle invalidated");
    ExpectResult(
        host->hook_remove(owner, &g_hookTargets[79]),
        WOTBMOD_OK,
        "retry hook cleanup while mod disabled");
    Expect(
        g_hookRemoves == hookRemovesBeforeDisable + 1,
        "hook cleanup retry reached backend");
    original = nullptr;
    ExpectResult(
        host->hook_create(
            owner,
            &g_hookTargets[79],
            reinterpret_cast<void*>(&DummyDetour),
            &original),
        WOTBMOD_ERROR_DISABLED,
        "disabled mod cannot create hook");
    WotbModResourceMountId disabledMount = 0;
    ExpectResult(
        Mount(
            host,
            owner,
            "~res:/Contract/Disabled/",
            "low",
            1,
            0,
            &disabledMount),
        WOTBMOD_ERROR_DISABLED,
        "disabled mod cannot mount resource");
    WotbModResourceHandle disabledResource = nullptr;
    ExpectResult(
        LoadResource(
            host,
            owner,
            WOTBMOD_RESOURCE_GENERIC,
            "~res:/Typed/disabled",
            nullptr,
            0,
            &disabledResource),
        WOTBMOD_ERROR_DISABLED,
        "disabled mod cannot load resource");
    ExpectResult(
        host->audio_play(
            owner,
            reinterpret_cast<WotbModResourceHandle>(1),
            nullptr,
            &cleanupPlayback),
        WOTBMOD_ERROR_DISABLED,
        "disabled mod cannot play audio");
    char path[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t pathSize = sizeof(path);
    ExpectResult(
        WotbModRuntime_ResolveResourcePath(
            "~res:/Contract/Cleanup/value.txt",
            path,
            &pathSize),
        WOTBMOD_ERROR_NOT_FOUND,
        "mount cleanup on disable");
    pathSize = sizeof(path);
    ExpectResult(
        WotbModRuntime_ResolveVehicleSkinPath(
            "~res:/3d/Tanks/Test/cleanup.tex",
            path,
            &pathSize),
        WOTBMOD_ERROR_NOT_FOUND,
        "vehicle skin cleanup on disable");
    ExpectResult(
        host->vehicle_skin_release(owner, cleanupSkin),
        WOTBMOD_ERROR_NOT_FOUND,
        "cleaned vehicle skin handle invalidated");
    ExpectResult(
        host->set_mod_enabled("test.contract", 0),
        WOTBMOD_OK,
        "disable idempotent");
    Expect(contract.get_disable_count() == 1, "disable idempotent callback");

    char settingsPath[MAX_PATH] = {};
    _snprintf_s(
        settingsPath,
        sizeof(settingsPath),
        _TRUNCATE,
        "%s\\mods\\mods.ini",
        apiEnvironment);
    Expect(
        GetPrivateProfileIntA(
            "mods", "api_contract_mod", -1, settingsPath) == 0,
        "disabled state persisted");
    ExpectResult(
        host->set_mod_enabled("test.contract", 1),
        WOTBMOD_OK,
        "re-enable mod");
    Expect(contract.get_enable_count() == 2, "re-enable callback count");
    Expect(
        GetPrivateProfileIntA(
            "mods", "api_contract_mod", -1, settingsPath) == 1,
        "enabled state persisted");
}

static void RunHappyCycle(const char* apiEnvironment) {
    ExpectResult(
        InitializeRuntime(
            apiEnvironment,
            true,
            true,
            true,
            true,
            true,
            false,
            false,
            true,
            true,
            false,
            true),
        WOTBMOD_OK,
        "happy initialize");
    ExpectResult(
        InitializeRuntime(
            apiEnvironment,
            true,
            true,
            true,
            true,
            true,
            false,
            false,
            true,
            true,
            false,
            true),
        WOTBMOD_ERROR_ALREADY_EXISTS,
        "initialize repeated");
    ExpectResult(WotbModRuntime_LoadAll(), WOTBMOD_OK, "happy LoadAll");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_ERROR_ALREADY_EXISTS,
        "LoadAll repeated");

    const WotbModHostApi* host = WotbModRuntime_GetHostApi();
    ContractExports contract = {};
    WotbModHandle peer = nullptr;
    if (!LoadContractExports(&contract, &peer)) {
        WotbModRuntime_Shutdown();
        return;
    }
    WotbModHandle owner = contract.get_handle();
    Expect(contract.get_host() == host, "mod received host table");
    TestMetadataAndPaths(host, owner);
    TestSymbols(host);
    TestConfig(host, owner);
    TestHooks(host, owner, peer);
    TestResourceMounts(host, owner, peer);
    TestVehicleSkins(host, owner, peer);
    TestTypedResources(host, owner, peer);
    TestAudio(host, owner, peer);
    TestSoundEvents(host, owner, peer);
    TestLifecycle(apiEnvironment, host, contract, owner);
    TestClientEvents(host, owner, peer);
    TestGameplayApiAndEvents(host, owner, peer);
    TestMainThreadAndObjects(host, owner, peer);
    TestClientEventFaultCleanup(host, owner);
    Expect(
        g_soundEventActive == 0,
        "native sound events balanced after disable");

    const LONG unloadLogsBefore = g_contractUnloadLogs;
    WotbModRuntime_Shutdown();
    Expect(
        g_contractUnloadLogs == unloadLogsBefore + 1,
        "unload callback log");
    Expect(g_nativeActive == 0, "native resources balanced after shutdown");
    Expect(
        g_playbackActive == 0,
        "native audio playbacks balanced after shutdown");
    Expect(
        g_soundEventActive == 0,
        "native sound events balanced after shutdown");
    Expect(
        g_vehicleTokensActive == 0,
        "native vehicle tokens balanced after shutdown");
    Expect(host->get_mod_count() == 0, "mod count reset after shutdown");
    Expect(
        WotbModRuntime_GetResourceGeneration() == 0,
        "resource generation reset after shutdown");
    uint32_t size = 0;
    ExpectResult(
        WotbModRuntime_ResolveResourcePath("~res:/none", nullptr, &size),
        WOTBMOD_ERROR_DISABLED,
        "resolver disabled after shutdown");
    WotbModRuntime_Shutdown();
}

static void RunNoBackendCycle(const char* apiEnvironment) {
    WotbModRuntimeHookBackend shortHooks = {};
    shortHooks.struct_size =
        (uint32_t)offsetof(WotbModRuntimeHookBackend, remove);
    WotbModRuntimeOptions options = {};
    options.struct_size =
        (uint32_t)offsetof(WotbModRuntimeOptions, resource_backend);
    options.game_directory = apiEnvironment;
    options.game_module = GetModuleHandleA(nullptr);
    options.log_sink = &LogSink;
    options.hook_backend = &shortHooks;
    ExpectResult(
        WotbModRuntime_Initialize(&options),
        WOTBMOD_OK,
        "legacy options and short backend initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_OK,
        "no-backend LoadAll");
    ContractExports contract = {};
    WotbModHandle peer = nullptr;
    if (LoadContractExports(&contract, &peer)) {
        const WotbModHostApi* host = WotbModRuntime_GetHostApi();
        WotbModHandle owner = contract.get_handle();
        void* original = nullptr;
        ExpectResult(
            host->hook_create(
                owner,
                &g_hookTargets[0],
                reinterpret_cast<void*>(&DummyDetour),
                &original),
            WOTBMOD_ERROR_ACCESS_DENIED,
            "legacy raw hook API defaults to denied");
        WotbModResourceHandle resource = nullptr;
        ExpectResult(
            LoadResource(
                host,
                owner,
                WOTBMOD_RESOURCE_GENERIC,
                "~res:/Typed/no-backend",
                nullptr,
                0,
                &resource),
            WOTBMOD_ERROR_PLATFORM,
            "resource backend unavailable");
        WotbModSoundEventHandle event = nullptr;
        ExpectResult(
            host->sound_event_create(
                owner, "ui.no_backend", &event),
            WOTBMOD_ERROR_PLATFORM,
            "sound backend unavailable");
        WotbModUiControlGeometry geometry = {};
        geometry.struct_size = sizeof(geometry);
        geometry.width = 100.0f;
        geometry.height = 50.0f;
        WotbModSceneTransform transform = {};
        transform.struct_size = sizeof(transform);
        transform.rotation_w = 1.0f;
        transform.scale_x = 1.0f;
        transform.scale_y = 1.0f;
        transform.scale_z = 1.0f;
        WotbModResourceHandle first =
            reinterpret_cast<WotbModResourceHandle>(1);
        WotbModResourceHandle second =
            reinterpret_cast<WotbModResourceHandle>(2);
        ExpectResult(
            host->ui_control_set_geometry(owner, first, &geometry),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "UI geometry validates handle without backend");
        ExpectResult(
            host->ui_control_set_visible(owner, first, 1),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "UI visibility validates handle without backend");
        ExpectResult(
            host->ui_control_add_child(owner, first, second),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "UI add child validates handles without backend");
        ExpectResult(
            host->ui_control_remove_child(owner, first, second),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "UI remove child validates handles without backend");
        ExpectResult(
            host->scene_set_transform(owner, first, &transform),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "scene transform validates handle without backend");
        ExpectResult(
            host->scene_add_child(owner, first, second),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "scene add child validates handles without backend");
        ExpectResult(
            host->scene_remove_child(owner, first, second),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "scene remove child validates handles without backend");
        InterlockedExchange(&g_mainThreadCallbackCount, 0);
        InterlockedExchange(&g_mainThreadCallbackOrderCount, 0);
        ExpectResult(
            host->main_thread_enqueue(
                owner,
                &MainThreadProbe,
                reinterpret_cast<void*>(7)),
            WOTBMOD_OK,
            "main thread callback independent of resource backend");
        WotbModRuntime_DispatchFrame(
            nullptr, nullptr, nullptr, 1, 1, 0.0);
        Expect(
            g_mainThreadCallbackCount == 1 &&
                g_mainThreadCallbackOrder[0] == 7,
            "main thread callback without resource backend");
    }
    WotbModRuntime_Shutdown();
}

static void RunNoReloadCycle(const char* apiEnvironment) {
    ExpectResult(
        InitializeRuntime(apiEnvironment, false, true, false, false, false),
        WOTBMOD_OK,
        "no-reload initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_OK,
        "no-reload LoadAll");
    ContractExports contract = {};
    WotbModHandle peer = nullptr;
    if (LoadContractExports(&contract, &peer)) {
        const WotbModHostApi* host = WotbModRuntime_GetHostApi();
        WotbModResourceHandle resource = nullptr;
        ExpectResult(
            LoadResource(
                host,
                contract.get_handle(),
                WOTBMOD_RESOURCE_GENERIC,
                "~res:/Typed/no-reload",
                nullptr,
                0,
                &resource),
            WOTBMOD_OK,
            "resource load without reload callback");
        ExpectResult(
            host->resource_reload(contract.get_handle(), resource),
            WOTBMOD_ERROR_PLATFORM,
            "resource reload callback unavailable");
        ExpectResult(
            host->resource_release(contract.get_handle(), resource),
            WOTBMOD_OK,
            "resource release without reload callback");
    }
    WotbModRuntime_Shutdown();
}

static void RunLegacyResourceBackendCycle(const char* apiEnvironment) {
    ExpectResult(
        InitializeRuntime(
            apiEnvironment,
            false,
            true,
            true,
            false,
            false,
            false,
            false,
            false,
            true,
            true),
        WOTBMOD_OK,
        "legacy resource backend initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_OK,
        "legacy resource backend LoadAll");
    ContractExports contract = {};
    WotbModHandle peer = nullptr;
    if (LoadContractExports(&contract, &peer)) {
        const WotbModHostApi* host = WotbModRuntime_GetHostApi();
        const WotbModHandle owner = contract.get_handle();
        WotbModResourceHandle control = nullptr;
        ExpectResult(
            LoadResource(
                host,
                owner,
                WOTBMOD_RESOURCE_UI_CONTROL,
                "~res:/Typed/legacy-control.yaml",
                "LegacyControl",
                0,
                &control),
            WOTBMOD_OK,
            "legacy backend resource load");
        WotbModUiControlGeometry geometry = {};
        geometry.struct_size = sizeof(geometry);
        geometry.width = 10.0f;
        geometry.height = 10.0f;
        ExpectResult(
            host->ui_control_set_geometry(
                owner, control, &geometry),
            WOTBMOD_ERROR_PLATFORM,
            "legacy backend has no UI object callbacks");
        ExpectResult(
            host->resource_reload(owner, control),
            WOTBMOD_OK,
            "legacy backend resource reload");
        ExpectResult(
            host->resource_release(owner, control),
            WOTBMOD_OK,
            "legacy backend resource release");
    }
    WotbModRuntime_Shutdown();
}

static void RunSoundBackendVariants(const char* apiEnvironment) {
    ExpectResult(
        InitializeRuntime(
            apiEnvironment,
            false,
            false,
            false,
            false,
            false,
            false,
            false,
            true,
            false),
        WOTBMOD_OK,
        "minimal sound backend initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_OK,
        "minimal sound backend LoadAll");
    ContractExports contract = {};
    WotbModHandle peer = nullptr;
    if (LoadContractExports(&contract, &peer)) {
        const WotbModHostApi* host = WotbModRuntime_GetHostApi();
        WotbModHandle owner = contract.get_handle();
        WotbModSoundEventHandle event = nullptr;
        ExpectResult(
            host->sound_event_create(owner, "ui.minimal", &event),
            WOTBMOD_OK,
            "minimal sound create");
        ExpectResult(
            host->sound_event_trigger(owner, event),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound trigger unavailable");
        ExpectResult(
            host->sound_event_stop(owner, event),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound stop unavailable");
        ExpectResult(
            host->sound_event_set_paused(owner, event, 1),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound pause unavailable");
        ExpectResult(
            host->sound_event_set_volume(owner, event, 1.0f),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound volume unavailable");
        ExpectResult(
            host->sound_event_set_position(
                owner, event, 0.0f, 0.0f, 0.0f),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound position unavailable");
        WotbModAudioState state = WOTBMOD_AUDIO_PLAYING;
        ExpectResult(
            host->sound_event_get_state(owner, event, &state),
            WOTBMOD_OK,
            "minimal sound cached state");
        Expect(
            state == WOTBMOD_AUDIO_STOPPED,
            "minimal sound cached stopped state");
        ExpectResult(
            host->sound_event_set_parameter(
                owner, event, "speed", 1.0f),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound set parameter unavailable");
        float value = 0.0f;
        ExpectResult(
            host->sound_event_get_parameter(
                owner, event, "speed", &value),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound get parameter unavailable");
        int32_t hasParameter = 0;
        ExpectResult(
            host->sound_event_has_parameter(
                owner, event, "speed", &hasParameter),
            WOTBMOD_ERROR_PLATFORM,
            "minimal sound has parameter unavailable");
        ExpectResult(
            host->sound_event_release(owner, event),
            WOTBMOD_OK,
            "minimal sound release");
    }
    WotbModRuntime_Shutdown();
}

static void RunAudioBackendVariants(const char* apiEnvironment) {
    ExpectResult(
        InitializeRuntime(
            apiEnvironment,
            false,
            true,
            true,
            false,
            false),
        WOTBMOD_OK,
        "no-audio-backend initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_OK,
        "no-audio-backend LoadAll");
    ContractExports contract = {};
    WotbModHandle peer = nullptr;
    if (LoadContractExports(&contract, &peer)) {
        const WotbModHostApi* host = WotbModRuntime_GetHostApi();
        WotbModResourceHandle clip = nullptr;
        ExpectResult(
            LoadResource(
                host,
                contract.get_handle(),
                WOTBMOD_RESOURCE_AUDIO_CLIP,
                "~res:/Audio/no-backend.wav",
                nullptr,
                0,
                &clip),
            WOTBMOD_OK,
            "audio clip without audio backend");
        WotbModAudioPlaybackHandle playback = nullptr;
        ExpectResult(
            host->audio_play(
                contract.get_handle(),
                clip,
                nullptr,
                &playback),
            WOTBMOD_ERROR_PLATFORM,
            "audio backend unavailable");
        ExpectResult(
            host->resource_release(contract.get_handle(), clip),
            WOTBMOD_OK,
            "no-audio-backend clip release");
    }
    WotbModRuntime_Shutdown();

    ExpectResult(
        InitializeRuntime(
            apiEnvironment,
            false,
            true,
            true,
            true,
            false,
            false,
            true),
        WOTBMOD_OK,
        "minimal-audio-backend initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_OK,
        "minimal-audio-backend LoadAll");
    ZeroMemory(&contract, sizeof(contract));
    peer = nullptr;
    if (LoadContractExports(&contract, &peer)) {
        const WotbModHostApi* host = WotbModRuntime_GetHostApi();
        WotbModHandle owner = contract.get_handle();
        WotbModResourceHandle clip = nullptr;
        ExpectResult(
            LoadResource(
                host,
                owner,
                WOTBMOD_RESOURCE_AUDIO_CLIP,
                "~res:/Audio/minimal.wav",
                nullptr,
                0,
                &clip),
            WOTBMOD_OK,
            "minimal audio clip load");
        WotbModAudioPlaybackHandle playback = nullptr;
        ExpectResult(
            host->audio_play(owner, clip, nullptr, &playback),
            WOTBMOD_OK,
            "minimal audio play");
        WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
        ExpectResult(
            host->audio_get_state(owner, playback, &state),
            WOTBMOD_OK,
            "minimal backend cached state");
        Expect(
            state == WOTBMOD_AUDIO_PLAYING,
            "minimal backend cached state value");
        ExpectResult(
            host->audio_pause(owner, playback),
            WOTBMOD_ERROR_PLATFORM,
            "minimal backend pause unavailable");
        ExpectResult(
            host->audio_resume(owner, playback),
            WOTBMOD_ERROR_PLATFORM,
            "minimal backend resume unavailable");
        ExpectResult(
            host->audio_stop(owner, playback),
            WOTBMOD_ERROR_PLATFORM,
            "minimal backend stop unavailable");
        WotbModAudioPlayInfo info =
            MakeAudioPlayInfo(WOTBMOD_AUDIO_PLAY_LOOP);
        ExpectResult(
            host->audio_set_parameters(owner, playback, &info),
            WOTBMOD_ERROR_PLATFORM,
            "minimal backend parameters unavailable");
        ExpectResult(
            host->audio_release(owner, playback),
            WOTBMOD_OK,
            "minimal backend audio release");
        ExpectResult(
            host->resource_release(owner, clip),
            WOTBMOD_OK,
            "minimal backend clip release");
    }
    WotbModRuntime_Shutdown();
}

static void RunCustomAudioFileCycle(const char* apiEnvironment) {
    const LONG loadsBefore = g_audioClipLoads;
    const LONG reloadsBefore = g_audioClipReloads;
    const LONG releasesBefore = g_audioClipReleases;
    ExpectResult(
        InitializeRuntime(
            apiEnvironment,
            false,
            false,
            false,
            true,
            true,
            true),
        WOTBMOD_OK,
        "custom-audio initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_OK,
        "custom-audio LoadAll");

    ContractExports contract = {};
    WotbModHandle peer = nullptr;
    if (LoadContractExports(&contract, &peer)) {
        const WotbModHostApi* host = WotbModRuntime_GetHostApi();
        WotbModHandle owner = contract.get_handle();
        Expect(
            host->struct_size >=
                    offsetof(WotbModHostApi, audio_clip_load) +
                        sizeof(host->audio_clip_load) &&
                host->audio_clip_load != nullptr,
            "ABI 2.3 custom audio entry present");

        WotbModResourceHandle clip = nullptr;
        ExpectResult(
            host->audio_clip_load(
                owner,
                "~res:/Contract/CustomAudio/value.txt",
                &clip),
            WOTBMOD_ERROR_NOT_FOUND,
            "custom audio requires a mounted file");
        ExpectResult(
            host->audio_clip_load(owner, nullptr, &clip),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "custom audio null path");
        ExpectResult(
            host->audio_clip_load(owner, "~res:/missing", nullptr),
            WOTBMOD_ERROR_INVALID_ARGUMENT,
            "custom audio null output");

        WotbModResourceMountInfo mount = {};
        mount.struct_size = sizeof(mount);
        mount.virtual_root = "~res:/Contract/CustomAudio/";
        mount.source_directory = "low";
        mount.priority = 50;
        WotbModResourceMountId mountId = 0;
        ExpectResult(
            host->resource_mount(owner, &mount, &mountId),
            WOTBMOD_OK,
            "custom audio source mount");

        WotbModResourceHandle generic = nullptr;
        ExpectResult(
            LoadResource(
                host,
                owner,
                WOTBMOD_RESOURCE_GENERIC,
                "~res:/Contract/CustomAudio/value.txt",
                nullptr,
                0,
                &generic),
            WOTBMOD_ERROR_PLATFORM,
            "custom audio backend does not replace generic resources");
        ExpectResult(
            host->audio_clip_load(
                owner,
                "~res:/Contract/CustomAudio/value.txt",
                &clip),
            WOTBMOD_OK,
            "custom audio file load");
        Expect(clip != nullptr, "custom audio clip handle");
        Expect(
            g_audioClipLoads == loadsBefore + 1,
            "custom audio file reached clip backend");

        WotbModAudioPlayInfo info =
            MakeAudioPlayInfo(WOTBMOD_AUDIO_PLAY_LOOP);
        WotbModAudioPlaybackHandle playback = nullptr;
        ExpectResult(
            host->audio_play(owner, clip, &info, &playback),
            WOTBMOD_OK,
            "custom audio playback");
        ExpectResult(
            host->resource_reload(owner, clip),
            WOTBMOD_ERROR_ACCESS_DENIED,
            "active custom audio blocks reload");
        ExpectResult(
            host->resource_release(owner, clip),
            WOTBMOD_ERROR_ACCESS_DENIED,
            "active custom audio blocks release");
        ExpectResult(
            host->audio_pause(owner, playback),
            WOTBMOD_OK,
            "custom audio pause");
        ExpectResult(
            host->audio_resume(owner, playback),
            WOTBMOD_OK,
            "custom audio resume");
        ExpectResult(
            host->audio_stop(owner, playback),
            WOTBMOD_OK,
            "custom audio stop");
        ExpectResult(
            host->audio_release(owner, playback),
            WOTBMOD_OK,
            "custom audio playback release");
        ExpectResult(
            host->resource_reload(owner, clip),
            WOTBMOD_OK,
            "custom audio reload");
        Expect(
            g_audioClipReloads == reloadsBefore + 1,
            "custom audio reload reached clip backend");
        ExpectResult(
            host->resource_release(owner, clip),
            WOTBMOD_OK,
            "custom audio clip release");
        Expect(
            g_audioClipReleases == releasesBefore + 1,
            "custom audio release reached clip backend");
        ExpectResult(
            host->resource_unmount(owner, mountId),
            WOTBMOD_OK,
            "custom audio source unmount");
    }
    WotbModRuntime_Shutdown();
}

static void RunWindowsAudioBackendTest(const char* apiEnvironment) {
    char wavePath[MAX_PATH] = {};
    _snprintf_s(
        wavePath,
        sizeof(wavePath),
        _TRUNCATE,
        "%s\\custom_audio_backend_test.wav",
        apiEnvironment);
    Expect(
        WriteSilentPcmWave(wavePath),
        "write custom audio WAV fixture");

    WotbModRuntimeAudioBackend backend = {};
    WotbModWindowsAudioHandle audio = nullptr;
    ExpectResult(
        WotbModWindowsAudio_Create(nullptr, nullptr, &backend),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "Windows audio create null handle");
    WotbModWindowsAudioOptions shortOptions = {};
    shortOptions.struct_size =
        (uint32_t)offsetof(
            WotbModWindowsAudioOptions,
            max_decoded_bytes);
    ExpectResult(
        WotbModWindowsAudio_Create(
            &shortOptions, &audio, &backend),
        WOTBMOD_ERROR_INVALID_ARGUMENT,
        "Windows audio short options");

    WotbModWindowsAudioOptions options = {};
    options.struct_size = sizeof(options);
    const WotbModResult createResult =
        WotbModWindowsAudio_Create(
            &options, &audio, &backend);
    ExpectResult(
        createResult,
        WOTBMOD_OK,
        "Windows custom audio backend create");
    if (createResult == WOTBMOD_OK) {
        Expect(
            backend.struct_size == sizeof(backend) &&
                backend.load_clip &&
                backend.reload_clip &&
                backend.release_clip &&
                backend.play &&
                backend.pause &&
                backend.resume &&
                backend.stop &&
                backend.set_parameters &&
                backend.get_state &&
                backend.release,
            "Windows audio backend callbacks complete");

        void* clip = nullptr;
        ExpectResult(
            backend.load_clip(
                backend.user_data, wavePath, &clip),
            WOTBMOD_OK,
            "Windows backend decodes WAV");
        Expect(clip != nullptr, "Windows backend native clip");
        ExpectResult(
            backend.reload_clip(backend.user_data, clip),
            WOTBMOD_OK,
            "Windows backend reloads WAV");

        WotbModAudioPlayInfo spatial =
            MakeAudioPlayInfo(WOTBMOD_AUDIO_PLAY_SPATIAL);
        void* playback = nullptr;
        ExpectResult(
            backend.play(
                backend.user_data,
                clip,
                &spatial,
                &playback),
            WOTBMOD_ERROR_PLATFORM,
            "Windows backend rejects unsupported spatial audio");

        WotbModAudioPlayInfo info =
            MakeAudioPlayInfo(WOTBMOD_AUDIO_PLAY_LOOP);
        ExpectResult(
            backend.play(
                backend.user_data,
                clip,
                &info,
                &playback),
            WOTBMOD_OK,
            "Windows backend starts custom WAV");
        Expect(playback != nullptr, "Windows backend native playback");
        ExpectResult(
            backend.release_clip(backend.user_data, clip),
            WOTBMOD_ERROR_ACCESS_DENIED,
            "Windows backend retains active clip");
        ExpectResult(
            WotbModWindowsAudio_Destroy(audio),
            WOTBMOD_ERROR_ACCESS_DENIED,
            "Windows backend destroy blocked while active");

        WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
        ExpectResult(
            backend.get_state(
                backend.user_data, playback, &state),
            WOTBMOD_OK,
            "Windows backend playing state query");
        Expect(
            state == WOTBMOD_AUDIO_PLAYING,
            "Windows backend playing state");
        ExpectResult(
            backend.pause(backend.user_data, playback),
            WOTBMOD_OK,
            "Windows backend pause");
        ExpectResult(
            backend.get_state(
                backend.user_data, playback, &state),
            WOTBMOD_OK,
            "Windows backend paused state query");
        Expect(
            state == WOTBMOD_AUDIO_PAUSED,
            "Windows backend paused state");
        ExpectResult(
            backend.resume(backend.user_data, playback),
            WOTBMOD_OK,
            "Windows backend resume");

        WotbModAudioPlayInfo updated =
            MakeAudioPlayInfo(WOTBMOD_AUDIO_PLAY_NONE);
        updated.volume = 0.4f;
        updated.pitch = 0.75f;
        updated.pan = 0.5f;
        ExpectResult(
            backend.set_parameters(
                backend.user_data, playback, &updated),
            WOTBMOD_OK,
            "Windows backend updates volume pitch pan and loop");
        ExpectResult(
            backend.stop(backend.user_data, playback),
            WOTBMOD_OK,
            "Windows backend stop");
        ExpectResult(
            backend.get_state(
                backend.user_data, playback, &state),
            WOTBMOD_OK,
            "Windows backend stopped state query");
        Expect(
            state == WOTBMOD_AUDIO_STOPPED,
            "Windows backend stopped state");
        ExpectResult(
            backend.resume(backend.user_data, playback),
            WOTBMOD_ERROR_PLATFORM,
            "Windows backend cannot resume flushed playback");
        ExpectResult(
            backend.release(backend.user_data, playback),
            WOTBMOD_OK,
            "Windows backend playback release");
        ExpectResult(
            backend.reload_clip(backend.user_data, clip),
            WOTBMOD_OK,
            "Windows backend reload after playback");
        ExpectResult(
            backend.release_clip(backend.user_data, clip),
            WOTBMOD_OK,
            "Windows backend clip release");
        ExpectResult(
            WotbModWindowsAudio_Destroy(audio),
            WOTBMOD_OK,
            "Windows custom audio backend destroy");
    }
    DeleteFileA(wavePath);
}

static void RunEmptyCycle(const char* emptyEnvironment) {
    char explicitMods[MAX_PATH] = {};
    _snprintf_s(
        explicitMods,
        sizeof(explicitMods),
        _TRUNCATE,
        "%s\\mods",
        emptyEnvironment);
    WotbModRuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.mods_directory = explicitMods;
    options.log_sink = &LogSink;
    ExpectResult(
        WotbModRuntime_Initialize(&options),
        WOTBMOD_OK,
        "derived game and explicit mods initialize");
    ExpectResult(WotbModRuntime_LoadAll(), WOTBMOD_OK, "empty LoadAll");
    const WotbModHostApi* host = WotbModRuntime_GetHostApi();
    Expect(host->get_mod_count() == 0, "empty mod count");
    Expect(
        host->get_game_module() == nullptr,
        "legacy raw process API defaults to denied");
    char modsPath[MAX_PATH] = {};
    uint32_t modsPathSize = sizeof(modsPath);
    ExpectResult(
        host->get_path(
            nullptr,
            WOTBMOD_PATH_MODS,
            modsPath,
            &modsPathSize),
        WOTBMOD_OK,
        "explicit mods path");
    char expectedMods[MAX_PATH] = {};
    GetFullPathNameA(
        explicitMods,
        (DWORD)sizeof(expectedMods),
        expectedMods,
        nullptr);
    Expect(_stricmp(modsPath, expectedMods) == 0, "explicit mods path value");
    WotbModRuntime_Shutdown();
}

static void RunMalformedCycle(const char* malformedEnvironment) {
    ExpectResult(
        InitializeRuntime(
            malformedEnvironment,
            false,
            false,
            false,
            false,
            false),
        WOTBMOD_OK,
        "malformed initialize");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_ERROR_PLATFORM,
        "malformed LoadAll aggregate failure");
    const WotbModHostApi* host = WotbModRuntime_GetHostApi();
    Expect(host->get_mod_count() == 1, "malformed successful mod remains");
    Expect(FindModIndex(host, "test.contract") >= 0, "malformed valid mod metadata");
    ExpectResult(
        WotbModRuntime_LoadAll(),
        WOTBMOD_ERROR_ALREADY_EXISTS,
        "malformed repeated LoadAll");
    WotbModRuntime_Shutdown();
}

} /* namespace */

int main(int argc, char** argv) {
    if (argc != 4) {
        fprintf(
            stderr,
            "usage: api_full_host.exe <api-env> <empty-env> <malformed-env>\n");
        return 2;
    }

    TestPreInitialize(argv[1], argv[0]);
    RunHappyCycle(argv[1]);
    RunNoBackendCycle(argv[1]);
    RunNoReloadCycle(argv[1]);
    RunLegacyResourceBackendCycle(argv[1]);
    RunSoundBackendVariants(argv[1]);
    RunAudioBackendVariants(argv[1]);
    RunCustomAudioFileCycle(argv[1]);
    RunWindowsAudioBackendTest(argv[1]);
    RunEmptyCycle(argv[2]);
    RunMalformedCycle(argv[3]);

    Expect(g_nativeActive == 0, "final native allocation balance");
    Expect(g_playbackActive == 0, "final audio allocation balance");
    Expect(g_soundEventActive == 0, "final native sound allocation balance");
    Expect(
        g_vehicleTokensActive == 0,
        "final native vehicle token balance");
    if (g_playbackActive != 0) CleanupPlaybackLeaks();
    if (g_soundEventActive != 0) CleanupSoundEventLeaks();
    if (g_nativeActive != 0) CleanupNativeLeaks();

    if (g_test.failures != 0) {
        fprintf(
            stderr,
            "API FULL FAILED: assertions=%d failures=%d\n",
            g_test.assertions,
            g_test.failures);
        return 1;
    }
    printf(
        "API FULL OK: assertions=%d all public functions passed\n",
        g_test.assertions);
    return 0;
}
