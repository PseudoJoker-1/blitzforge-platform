#pragma once

#include <stdint.h>

#include "wotb_mod_api.h"
#include "wotbmod/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void(WOTBMOD_CALL* WotbModRuntimeLogSink)(
    WotbModLogLevel level,
    const char* message,
    void* user_data);

typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeHookCreate)(
    void* user_data,
    void* target,
    void* detour,
    void** original);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeHookOperation)(
    void* user_data,
    void* target);

/*
 * The runtime deliberately does not choose a hooking library. The loader
 * architecture supplies an adapter (for MinHook, PolyHook, or another engine).
 */
typedef struct WotbModRuntimeHookBackend {
    uint32_t struct_size;
    void* user_data;
    WotbModRuntimeHookCreate create;
    WotbModRuntimeHookOperation enable;
    WotbModRuntimeHookOperation disable;
    WotbModRuntimeHookOperation remove;
} WotbModRuntimeHookBackend;

typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceLoad)(
    void* user_data,
    const WotbModResourceLoadRequest* request,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceLoadResolved)(
    void* user_data,
    WotbModResourceType type,
    const char* resolved_file_path,
    const char* object_name,
    uint32_t flags,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceOperation)(
    void* user_data,
    void* native_resource);
typedef void(WOTBMOD_CALL* WotbModRuntimeResourceRegistryChanged)(
    void* user_data,
    uint64_t generation);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiSetGeometry)(
    void* user_data,
    void* native_resource,
    const WotbModUiControlGeometry* geometry);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiSetVisible)(
    void* user_data,
    void* native_resource,
    int32_t visible);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourcePairOperation)(
    void* user_data,
    void* parent_native_resource,
    void* child_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSceneSetTransform)(
    void* user_data,
    void* native_resource,
    const WotbModSceneTransform* transform);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiCreate)(
    void* user_data,
    const WotbModUiControlGeometry* geometry,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceCreate)(
    void* user_data,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceClone)(
    void* user_data,
    void* native_resource,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiFindByName)(
    void* user_data,
    void* root_native_resource,
    const char* name,
    int32_t recursive,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceGetRelated)(
    void* user_data,
    void* native_resource,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceGetCount)(
    void* user_data,
    void* native_resource,
    uint32_t* out_count);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeResourceGetAt)(
    void* user_data,
    void* native_resource,
    uint32_t index,
    void** out_native_resource);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiGetState)(
    void* user_data,
    void* native_resource,
    WotbModUiControlState* out_state);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiSetFlag)(
    void* user_data,
    void* native_resource,
    int32_t value,
    int32_t hierarchical);
/*
 * Live UTF-8 text of the engine's own text component on a UI control.
 * inout_size carries the buffer capacity in and the text length (without
 * the terminator) out. WOTBMOD_ERROR_NOT_FOUND when the control has no text
 * component; WOTBMOD_ERROR_BUFFER_TOO_SMALL with the required size when the
 * buffer is short.
 */
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiGetText)(
    void* user_data,
    void* native_resource,
    char* buffer,
    uint32_t* inout_size);

/*
 * 2026-09-05: identity of a game-owned control - its DAVA name (FastName at
 * UIControl+0x30, interned text) and its RTTI class ("DAVA::UIStaticText").
 * Either buffer may come back empty when the engine has no value; both are
 * NUL-terminated within their capacity.
 */
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiGetIdentity)(
    void* user_data,
    void* native_resource,
    char* name,
    uint32_t name_capacity,
    char* class_name,
    uint32_t class_capacity);

/*
 * 2026-09-05: the colour of a control's DAVA::UIControlBackground component,
 * the RGBA that tints its sprite or fill. NOT_FOUND when the control draws
 * nothing itself (a bare container has no background component). Both take
 * four floats in [0, 1].
 */
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiGetBackgroundColor)(
    void* user_data,
    void* native_resource,
    float* out_rgba);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeUiSetBackgroundColor)(
    void* user_data,
    void* native_resource,
    const float* rgba);

/*
 * The loader owns DAVA C++ types and supplies this bridge. The public mod ABI
 * remains compiler-neutral and exposes only typed, opaque resource handles.
 */
typedef struct WotbModRuntimeResourceBackend {
    uint32_t struct_size;
    void* user_data;
    WotbModRuntimeResourceLoad load;
    WotbModRuntimeResourceOperation reload;
    WotbModRuntimeResourceOperation release;
    WotbModRuntimeResourceRegistryChanged registry_changed;
    WotbModRuntimeUiSetGeometry ui_set_geometry;
    WotbModRuntimeUiSetVisible ui_set_visible;
    WotbModRuntimeResourcePairOperation ui_add_child;
    WotbModRuntimeResourcePairOperation ui_remove_child;
    WotbModRuntimeSceneSetTransform scene_set_transform;
    WotbModRuntimeResourcePairOperation scene_add_child;
    WotbModRuntimeResourcePairOperation scene_remove_child;
    WotbModRuntimeUiCreate ui_create;
    WotbModRuntimeResourceCreate ui_get_active_screen;
    WotbModRuntimeResourceCreate scene_entity_create;
    WotbModRuntimeResourceCreate scene_get_active;
    WotbModRuntimeResourceClone clone;
    WotbModRuntimeUiFindByName ui_find_by_name;
    WotbModRuntimeResourceGetRelated ui_get_parent;
    WotbModRuntimeResourceGetCount ui_get_child_count;
    WotbModRuntimeResourceGetAt ui_get_child_at;
    WotbModRuntimeUiGetState ui_get_state;
    WotbModRuntimeUiSetFlag ui_set_input_enabled;
    WotbModRuntimeUiSetFlag ui_set_disabled;
    /*
     * Loader-private trusted path. The runtime never forwards a mod-supplied
     * path here; V3 URI resolution first canonicalizes and containment-checks
     * mod:// or game://, then the loader may build a DAVA object directly.
     */
    WotbModRuntimeResourceLoadResolved load_resolved;
    /* 2026-09-04: live engine text (UITextComponent). Optional. */
    WotbModRuntimeUiGetText ui_get_text;
    /* 2026-09-05: DAVA name + RTTI class of a control. Optional. */
    WotbModRuntimeUiGetIdentity ui_get_identity;
    /* 2026-09-05: UIControlBackground colour of a game control. Optional. */
    WotbModRuntimeUiGetBackgroundColor ui_get_background_color;
    WotbModRuntimeUiSetBackgroundColor ui_set_background_color;
} WotbModRuntimeResourceBackend;

typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioPlay)(
    void* user_data,
    void* native_audio_clip,
    const WotbModAudioPlayInfo* play_info,
    void** out_native_playback);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioOperation)(
    void* user_data,
    void* native_playback);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioSetParameters)(
    void* user_data,
    void* native_playback,
    const WotbModAudioPlayInfo* parameters);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioGetState)(
    void* user_data,
    void* native_playback,
    WotbModAudioState* out_state);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioClipLoad)(
    void* user_data,
    const char* resolved_file_path,
    void** out_native_audio_clip);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioClipOperation)(
    void* user_data,
    void* native_audio_clip);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioSeek)(
    void* user_data,
    void* native_playback,
    double seconds);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeAudioGetTime)(
    void* user_data,
    void* native_object,
    double* out_seconds);

/*
 * The loader supplies a bridge to the client's sound engine. play and release
 * are required; pause/resume/stop/set_parameters/get_state are optional.
 * release must stop playback and free it even when stop is not provided.
 *
 * ABI 2.3 appends load_clip/reload_clip/release_clip. When load_clip and
 * release_clip are present, AUDIO_CLIP resource loads resolve the mod virtual
 * path to a real loose file and route it through this backend instead of the
 * generic DAVA resource backend. This is the custom-file path; older backends
 * that omit these fields remain compatible.
 *
 * The final three callbacks are append-only V3 timing extensions. seek and
 * get_position receive a native playback; get_duration receives a native
 * audio clip. Callers must gate every extension by struct_size.
 */
typedef struct WotbModRuntimeAudioBackend {
    uint32_t struct_size;
    void* user_data;
    WotbModRuntimeAudioPlay play;
    WotbModRuntimeAudioOperation pause;
    WotbModRuntimeAudioOperation resume;
    WotbModRuntimeAudioOperation stop;
    WotbModRuntimeAudioSetParameters set_parameters;
    WotbModRuntimeAudioGetState get_state;
    WotbModRuntimeAudioOperation release;
    WotbModRuntimeAudioClipLoad load_clip;
    WotbModRuntimeAudioClipOperation reload_clip;
    WotbModRuntimeAudioClipOperation release_clip;
    WotbModRuntimeAudioSeek seek;
    WotbModRuntimeAudioGetTime get_position;
    WotbModRuntimeAudioGetTime get_duration;
} WotbModRuntimeAudioBackend;

typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventCreate)(
    void* user_data,
    const char* event_name,
    void** out_native_event);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventOperation)(
    void* user_data,
    void* native_event);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventSetPaused)(
    void* user_data,
    void* native_event,
    int32_t paused);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventSetVolume)(
    void* user_data,
    void* native_event,
    float volume);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventSetPosition)(
    void* user_data,
    void* native_event,
    float x,
    float y,
    float z);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventGetState)(
    void* user_data,
    void* native_event,
    WotbModAudioState* out_state);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventSetParameter)(
    void* user_data,
    void* native_event,
    const char* parameter_name,
    float value);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventGetParameter)(
    void* user_data,
    void* native_event,
    const char* parameter_name,
    float* out_value);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventHasParameter)(
    void* user_data,
    void* native_event,
    const char* parameter_name,
    int32_t* out_has_parameter);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventStopWithForce)(
    void* user_data,
    void* native_event,
    int32_t force);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventSetScalar)(
    void* user_data,
    void* native_event,
    float value);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventSetVector3)(
    void* user_data,
    void* native_event,
    float x,
    float y,
    float z);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeSoundEventSetInteger)(
    void* user_data,
    void* native_event,
    int32_t value);

/*
 * Loader-owned bridge to DAVA::SoundSystem and DAVA::SoundEvent. create and
 * release are required; the other callbacks are optional. The callbacks after
 * release are append-only V3 extensions and must be gated by struct_size.
 * Third-party mods never receive DAVA object pointers.
 */
typedef struct WotbModRuntimeSoundBackend {
    uint32_t struct_size;
    void* user_data;
    WotbModRuntimeSoundEventCreate create;
    WotbModRuntimeSoundEventOperation trigger;
    WotbModRuntimeSoundEventOperation stop;
    WotbModRuntimeSoundEventSetPaused set_paused;
    WotbModRuntimeSoundEventSetVolume set_volume;
    WotbModRuntimeSoundEventSetPosition set_position;
    WotbModRuntimeSoundEventGetState get_state;
    WotbModRuntimeSoundEventSetParameter set_parameter;
    WotbModRuntimeSoundEventGetParameter get_parameter;
    WotbModRuntimeSoundEventHasParameter has_parameter;
    WotbModRuntimeSoundEventOperation release;
    WotbModRuntimeSoundEventStopWithForce stop_with_force;
    WotbModRuntimeSoundEventSetScalar set_speed;
    WotbModRuntimeSoundEventSetVector3 set_direction;
    WotbModRuntimeSoundEventSetVector3 set_velocity;
    WotbModRuntimeSoundEventSetInteger set_loop_count;
    WotbModRuntimeSoundEventSetInteger set_priority;
} WotbModRuntimeSoundBackend;

typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeVehicleGetLocal)(
    void* user_data,
    void** out_token);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeVehicleGetByEntityId)(
    void* user_data,
    uint32_t entity_id,
    void** out_token);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeVehicleGetCount)(
    void* user_data,
    uint32_t* out_count);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeVehicleGetAt)(
    void* user_data,
    uint32_t index,
    void** out_token);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeVehicleClone)(
    void* user_data,
    void* token,
    void** out_token);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeVehicleOperation)(
    void* user_data,
    void* token);
typedef WotbModResult(WOTBMOD_CALL* WotbModRuntimeVehicleGetInfo)(
    void* user_data,
    void* token,
    WotbModVehicleInfo* out_info);

/*
 * Loader-owned vehicle registry. Tokens are private to this backend and are
 * always released by the runtime. Public mods receive only opaque,
 * owner-checked handles.
 */
typedef struct WotbModRuntimeGameplayBackend {
    uint32_t struct_size;
    void* user_data;
    WotbModRuntimeVehicleGetLocal vehicle_get_local;
    WotbModRuntimeVehicleGetByEntityId vehicle_get_by_entity_id;
    WotbModRuntimeVehicleGetCount vehicle_get_count;
    WotbModRuntimeVehicleGetAt vehicle_get_at;
    WotbModRuntimeVehicleClone vehicle_clone;
    WotbModRuntimeVehicleOperation vehicle_release;
    WotbModRuntimeVehicleGetInfo vehicle_get_info;
} WotbModRuntimeGameplayBackend;

/*
 * Loader-owned ABI-3 client binding pack. The public V3 interfaces never
 * receive pointers from this backend: the runtime translates native tokens
 * into owner-checked generational handles.
 *
 * The binding pack is optional and appended to WotbModRuntimeOptions. Older
 * loaders remain ABI-compatible and V3 client-facing operations return
 * WOTBMOD_V3_E_NOT_SUPPORTED when it is absent.
 */
#define WOTBMOD_RUNTIME_V3_CLIENT_BACKEND_VERSION 1u

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModRuntimeV3ClientInvoke)(
    void* user_data,
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size);

typedef struct WotbModRuntimeV3ClientBackend {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t binding_pack_version;
    uint32_t compatibility_state;
    void* user_data;
    WotbModRuntimeV3ClientInvoke invoke;
    /*
     * Appended identity fields. Runtimes must gate each field by struct_size;
     * older binding packs remain ABI-compatible and simply provide no client
     * identity for manifest allow-list checks.
     */
    char client_build[WOTBMOD_V3_MAX_NAME];
    char client_executable_sha256[65];
} WotbModRuntimeV3ClientBackend;

typedef enum WotbModRuntimeOptionFlags {
    /*
     * Developer-only compatibility surface for ABI 2.x mods. It exposes raw
     * module addresses and arbitrary hook targets, so shipping loaders must
     * leave it disabled. V3 reviewed symbol/event APIs are unaffected.
     */
    WOTBMOD_RUNTIME_OPTION_ALLOW_LEGACY_RAW_PROCESS_API = 1u << 0
} WotbModRuntimeOptionFlags;

typedef struct WotbModRuntimeOptions {
    uint32_t struct_size;
    const char* game_directory;
    const char* mods_directory;
    void* game_module;
    WotbModRuntimeLogSink log_sink;
    void* log_user_data;
    const WotbModRuntimeHookBackend* hook_backend;
    const WotbModRuntimeResourceBackend* resource_backend;
    const WotbModRuntimeAudioBackend* audio_backend;
    const WotbModRuntimeSoundBackend* sound_backend;
    const WotbModRuntimeGameplayBackend* gameplay_backend;
    const WotbModRuntimeV3ClientBackend* v3_client_backend;
    uint32_t flags;
    /*
     * Optional ABI-3 hook broker backend. Appended for compatibility; older
     * loaders omit it and V3 hooks remain metadata-only.
     */
    const struct WotbModV3NativeHookBackend* v3_hook_backend;
} WotbModRuntimeOptions;

/*
 * Loader-only crash-loop preflight. Call this before MinHook, fixed-RVA
 * binding packs, or any third-party LoadLibrary. It claims the session marker
 * for a normal startup or reports an existing stale marker through
 * out_safe_mode. WotbModRuntime_Initialize reuses the same claim.
 */
WotbModResult WOTBMOD_CALL WotbModRuntime_PreflightCrashLoop(
    const char* mods_directory,
    WotbModRuntimeLogSink log_sink,
    void* log_user_data,
    int32_t* out_safe_mode);

/*
 * Best-effort loader phase attribution for failures during native backend
 * setup. Ignored when the current process does not own a session marker.
 */
void WOTBMOD_CALL WotbModRuntime_SetCrashLoopPhase(
    const char* phase);

/*
 * Loader-only orderly process-detach fallback. This performs only the
 * loader-lock-safe crash-loop marker cleanup needed when the host does not
 * execute the DLL CRT atexit table. Do not call it for crash recovery.
 */
void WOTBMOD_CALL WotbModRuntime_ProcessDetach(void);

WotbModResult WOTBMOD_CALL WotbModRuntime_Initialize(
    const WotbModRuntimeOptions* options);
WotbModResult WOTBMOD_CALL WotbModRuntime_LoadAll(void);
void WOTBMOD_CALL WotbModRuntime_DispatchFrame(
    void* swap_chain,
    void* device,
    void* device_context,
    uint32_t back_buffer_width,
    uint32_t back_buffer_height,
    double delta_seconds);
void WOTBMOD_CALL WotbModRuntime_Shutdown(void);
const WotbModHostApi* WOTBMOD_CALL WotbModRuntime_GetHostApi(void);

/*
 * Called by the loader's reviewed file-resolution integration. It returns a
 * loose or .dvpl file from the highest-priority enabled mod mount. NOT_FOUND
 * means the original game resolver should continue unchanged.
 */
WotbModResult WOTBMOD_CALL WotbModRuntime_ResolveResourcePath(
    const char* requested_path,
    char* buffer,
    uint32_t* inout_size);
uint64_t WOTBMOD_CALL WotbModRuntime_GetResourceGeneration(void);

/*
 * Called by the loader's DAVA::File::Create hook before the normal resource
 * overlay lookup. It resolves an enabled vehicle-skin stock-path alias to the
 * mounted loose/.dvpl file selected by skin priority.
 */
WotbModResult WOTBMOD_CALL WotbModRuntime_ResolveVehicleSkinPath(
    const char* requested_path,
    char* buffer,
    uint32_t* inout_size);

/*
 * Loader-side client event ingress. Resource arguments are borrowed backend
 * resource objects. The runtime clones them synchronously before returning,
 * queues the event, and releases its clones after main-thread delivery.
 */
WotbModResult WOTBMOD_CALL WotbModRuntime_NotifyUiScreenChanged(
    void* previous_native_resource,
    void* native_resource);
WotbModResult WOTBMOD_CALL WotbModRuntime_NotifySceneActivated(
    void* native_resource);
WotbModResult WOTBMOD_CALL WotbModRuntime_NotifySceneDeactivated(
    void* native_resource);

typedef struct WotbModRuntimeClientEvent {
    uint32_t struct_size;
    uint32_t type;
    WotbModResourceType resource_type;
    uint32_t flags;
    void* previous_native_resource;
    void* native_resource;
    uint32_t primary_entity_id;
    uint32_t other_entity_id;
    uint32_t payload_size;
    WotbModClientEventPayload payload;
} WotbModRuntimeClientEvent;

/*
 * Generic loader-side ingress for UI/gameplay events. It copies the complete
 * POD payload before returning. Optional resource wrappers are cloned before
 * queueing and released after main-thread delivery.
 */
WotbModResult WOTBMOD_CALL WotbModRuntime_NotifyClientEvent(
    const WotbModRuntimeClientEvent* event);

#ifdef __cplusplus
} /* extern "C" */
#endif
