#pragma once

/*
 * WoT Blitz Mod API ABI 2.9
 *
 * This header is the binary contract between a loader host and third-party
 * mod DLLs. Keep it C-compatible: do not place STL types, C++
 * classes, exceptions, or compiler-owned allocations in public structures.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define WOTBMOD_EXTERN_C extern "C"
#else
#define WOTBMOD_EXTERN_C extern
#endif

#if defined(_MSC_VER)
#define WOTBMOD_CALL __cdecl
#define WOTBMOD_EXPORT WOTBMOD_EXTERN_C __declspec(dllexport)
#else
#define WOTBMOD_CALL
#define WOTBMOD_EXPORT WOTBMOD_EXTERN_C
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ABI 2 removed the anchor calls from WotbModHostApi. A mod built against
 * ABI 1 expects a larger host table, so the major bump makes the loader
 * reject it rather than let it read past the end.
 */
#define WOTBMOD_ABI_VERSION_2 0x00020000u
#define WOTBMOD_ABI_VERSION_2_1 0x00020001u
#define WOTBMOD_ABI_VERSION_2_2 0x00020002u
#define WOTBMOD_ABI_VERSION_2_3 0x00020003u
#define WOTBMOD_ABI_VERSION_2_4 0x00020004u
#define WOTBMOD_ABI_VERSION_2_5 0x00020005u
#define WOTBMOD_ABI_VERSION_2_6 0x00020006u
#define WOTBMOD_ABI_VERSION_2_7 0x00020007u
#define WOTBMOD_ABI_VERSION_2_8 0x00020008u
#define WOTBMOD_ABI_VERSION_2_9 0x00020009u
#define WOTBMOD_ABI_VERSION WOTBMOD_ABI_VERSION_2_9
#define WOTBMOD_HOST_VERSION 0x00010009u
#define WOTBMOD_ABI_MAJOR(version) ((uint32_t)(version) >> 16)
#define WOTBMOD_ABI_MINOR(version) ((uint32_t)(version) & 0xFFFFu)

#if defined(_MSC_VER) && defined(WOTBMOD_RUNTIME_BUILD)
#define WOTBMOD_HOST_EXPORT __declspec(dllexport)
#else
#define WOTBMOD_HOST_EXPORT
#endif

#define WOTBMOD_ENTRY_NAME "WotbModLoad"
#define WOTBMOD_MAX_ID 64u
#define WOTBMOD_MAX_NAME 96u
#define WOTBMOD_MAX_VERSION 32u
#define WOTBMOD_MAX_AUTHOR 96u
#define WOTBMOD_MAX_DESCRIPTION 256u
#define WOTBMOD_MAX_PATH 260u
#define WOTBMOD_MAX_RESOURCE_PATH 1024u
#define WOTBMOD_MAX_SOUND_EVENT_NAME 128u
#define WOTBMOD_MAX_PLAYER_NAME 64u
#define WOTBMOD_MAX_VEHICLE_NAME 96u
#define WOTBMOD_MAX_SKIN_ID 64u

typedef void* WotbModHandle;
typedef uint64_t WotbModResourceMountId;
typedef void* WotbModResourceHandle;
typedef void* WotbModAudioPlaybackHandle;
typedef void* WotbModSoundEventHandle;
typedef void* WotbModVehicleHandle;
typedef void* WotbModVehicleSkinHandle;
typedef uint64_t WotbModEventSubscriptionId;

typedef enum WotbModResult {
    WOTBMOD_OK = 0,
    WOTBMOD_ERROR_INVALID_ARGUMENT = 1,
    WOTBMOD_ERROR_UNSUPPORTED_ABI = 2,
    WOTBMOD_ERROR_NOT_FOUND = 3,
    WOTBMOD_ERROR_ALREADY_EXISTS = 4,
    WOTBMOD_ERROR_PLATFORM = 5,
    WOTBMOD_ERROR_ACCESS_DENIED = 6,
    WOTBMOD_ERROR_BUFFER_TOO_SMALL = 7,
    WOTBMOD_ERROR_DISABLED = 8,
    WOTBMOD_ERROR_LIMIT_REACHED = 9,
    WOTBMOD_ERROR_CALLBACK_FAULT = 10,
    WOTBMOD_ERROR_WRONG_THREAD = 11
} WotbModResult;

typedef enum WotbModLogLevel {
    WOTBMOD_LOG_TRACE = 0,
    WOTBMOD_LOG_INFO = 1,
    WOTBMOD_LOG_WARNING = 2,
    WOTBMOD_LOG_ERROR = 3
} WotbModLogLevel;

typedef enum WotbModPath {
    WOTBMOD_PATH_GAME = 0,
    WOTBMOD_PATH_MODS = 1,
    WOTBMOD_PATH_MODULE = 2,
    WOTBMOD_PATH_DATA = 3,
    WOTBMOD_PATH_CONFIG = 4
} WotbModPath;

typedef enum WotbModState {
    WOTBMOD_STATE_DISCOVERED = 0,
    WOTBMOD_STATE_LOADED = 1,
    WOTBMOD_STATE_ENABLED = 2,
    WOTBMOD_STATE_DISABLED = 3,
    WOTBMOD_STATE_FAULTED = 4
} WotbModState;

typedef enum WotbModResourceType {
    WOTBMOD_RESOURCE_GENERIC = 0,
    WOTBMOD_RESOURCE_YAML_DOCUMENT = 1,
    WOTBMOD_RESOURCE_UI_PACKAGE = 2,
    WOTBMOD_RESOURCE_UI_CONTROL = 3,
    WOTBMOD_RESOURCE_SCENE = 4,
    WOTBMOD_RESOURCE_TEXTURE = 5,
    WOTBMOD_RESOURCE_AUDIO_CLIP = 6
} WotbModResourceType;

typedef enum WotbModResourceMountFlags {
    WOTBMOD_RESOURCE_MOUNT_NONE = 0,
    /*
     * If the exact loose file is absent, also try the same path with ".dvpl".
     * This matches the packaged UI, YAML, texture, and .sc2 layout used by the
     * client while still allowing unpacked files during development.
     */
    WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL = 1u << 0
} WotbModResourceMountFlags;

/*
 * source_directory is relative to the calling mod's WOTBMOD_PATH_DATA
 * directory. Absolute paths and ".." traversal are rejected.
 *
 * virtual_root accepts "~res:/...", "res:/...", "Data/...", or a path
 * relative to the DAVA resource root. The runtime normalizes it to ~res:/.
 */
typedef struct WotbModResourceMountInfo {
    uint32_t struct_size;
    const char* virtual_root;
    const char* source_directory;
    int32_t priority;
    uint32_t flags;
} WotbModResourceMountInfo;

/*
 * object_name is optional. A resource bridge may use it for a named control
 * inside a UI package; scene and YAML loads normally leave it null.
 */
typedef struct WotbModResourceLoadRequest {
    uint32_t struct_size;
    WotbModResourceType type;
    const char* virtual_path;
    const char* object_name;
    uint32_t flags;
} WotbModResourceLoadRequest;

typedef enum WotbModAudioPlayFlags {
    WOTBMOD_AUDIO_PLAY_NONE = 0,
    WOTBMOD_AUDIO_PLAY_LOOP = 1u << 0,
    WOTBMOD_AUDIO_PLAY_SPATIAL = 1u << 1,
    WOTBMOD_AUDIO_PLAY_START_PAUSED = 1u << 2
} WotbModAudioPlayFlags;

typedef enum WotbModAudioState {
    WOTBMOD_AUDIO_STOPPED = 0,
    WOTBMOD_AUDIO_PLAYING = 1,
    WOTBMOD_AUDIO_PAUSED = 2
} WotbModAudioState;

/*
 * Audio clips are loaded through resource_load with
 * WOTBMOD_RESOURCE_AUDIO_CLIP. A backend receives normalized values:
 * volume >= 0, pitch > 0, pan in [-1, 1], and 0 <= min_distance <=
 * max_distance. Position and distance are used only for SPATIAL playback.
 */
typedef struct WotbModAudioPlayInfo {
    uint32_t struct_size;
    uint32_t flags;
    float volume;
    float pitch;
    float pan;
    float position_x;
    float position_y;
    float position_z;
    float min_distance;
    float max_distance;
} WotbModAudioPlayInfo;

/*
 * UI geometry uses DAVA virtual coordinates. The native bridge applies
 * position and size on the runtime main thread.
 */
typedef struct WotbModUiControlGeometry {
    uint32_t struct_size;
    float x;
    float y;
    float width;
    float height;
} WotbModUiControlGeometry;

/*
 * Local transform for a loaded Scene/Entity. Quaternion order is x,y,z,w.
 * Scale components must be finite and non-zero.
 */
typedef struct WotbModSceneTransform {
    uint32_t struct_size;
    float position_x;
    float position_y;
    float position_z;
    float rotation_x;
    float rotation_y;
    float rotation_z;
    float rotation_w;
    float scale_x;
    float scale_y;
    float scale_z;
} WotbModSceneTransform;

typedef enum WotbModUiControlStateFlags {
    WOTBMOD_UI_CONTROL_VISIBLE = 1u << 0,
    WOTBMOD_UI_CONTROL_INPUT_ENABLED = 1u << 1,
    WOTBMOD_UI_CONTROL_DISABLED = 1u << 2
} WotbModUiControlStateFlags;

typedef struct WotbModUiControlState {
    uint32_t struct_size;
    WotbModUiControlGeometry geometry;
    uint32_t flags;
} WotbModUiControlState;

typedef enum WotbModVehicleFlags {
    WOTBMOD_VEHICLE_ALIVE = 1u << 0,
    WOTBMOD_VEHICLE_LOCAL = 1u << 1,
    WOTBMOD_VEHICLE_DESTROYED = 1u << 2
} WotbModVehicleFlags;

/*
 * A vehicle handle is an opaque runtime snapshot. Unknown client fields are
 * zeroed instead of exposing raw BigWorld/DAVA memory to third-party mods.
 */
typedef struct WotbModVehicleInfo {
    uint32_t struct_size;
    uint32_t entity_id;
    uint32_t team;
    uint32_t flags;
    int32_t health;
    int32_t max_health;
    char player_name[WOTBMOD_MAX_PLAYER_NAME];
    char vehicle_name[WOTBMOD_MAX_VEHICLE_NAME];
} WotbModVehicleInfo;

typedef enum WotbModVehicleSkinAssetKind {
    WOTBMOD_SKIN_ASSET_GENERIC = 0,
    WOTBMOD_SKIN_ASSET_MESH = 1,
    WOTBMOD_SKIN_ASSET_MATERIAL = 2,
    WOTBMOD_SKIN_ASSET_TEXTURE = 3
} WotbModVehicleSkinAssetKind;

typedef enum WotbModVehicleSkinFlags {
    WOTBMOD_VEHICLE_SKIN_NONE = 0,
    WOTBMOD_VEHICLE_SKIN_ENABLED = 1u << 0
} WotbModVehicleSkinFlags;

/*
 * A skin asset redirects one exact stock DAVA resource path to either a
 * resource supplied through the calling mod's resource mounts or an existing
 * stock resource under the local game's Data directory. Mesh .sc2/.scg files,
 * material/FX YAML and texture .tex files use the same path contract.
 */
typedef struct WotbModVehicleSkinAsset {
    uint32_t struct_size;
    uint32_t kind;
    const char* stock_virtual_path;
    const char* replacement_virtual_path;
} WotbModVehicleSkinAsset;

typedef struct WotbModVehicleSkinDescriptor {
    uint32_t struct_size;
    const char* skin_id;
    const char* vehicle_name;
    const WotbModVehicleSkinAsset* assets;
    uint32_t asset_count;
    int32_t priority;
    uint32_t flags;
} WotbModVehicleSkinDescriptor;

typedef struct WotbModVehicleSkinInfo {
    uint32_t struct_size;
    uint32_t enabled;
    uint32_t asset_count;
    int32_t priority;
    uint32_t flags;
    char skin_id[WOTBMOD_MAX_SKIN_ID];
    char vehicle_name[WOTBMOD_MAX_VEHICLE_NAME];
} WotbModVehicleSkinInfo;

typedef struct WotbModVec3 {
    float x;
    float y;
    float z;
} WotbModVec3;

typedef struct WotbModUiInputEventData {
    uint32_t action;
    uint32_t buttons;
    int32_t pointer_id;
    uint32_t modifiers;
    float screen_x;
    float screen_y;
    float local_x;
    float local_y;
} WotbModUiInputEventData;

typedef struct WotbModBattleEventData {
    uint64_t battle_id;
    uint32_t arena_id;
    uint32_t state;
    uint32_t winner_team;
    uint32_t reason;
} WotbModBattleEventData;

typedef struct WotbModVehicleEventData {
    uint32_t entity_id;
    uint32_t other_entity_id;
    int32_t previous_health;
    int32_t health;
    uint32_t flags;
} WotbModVehicleEventData;

typedef struct WotbModShotEventData {
    uint32_t shot_code;
    uint32_t shell_id;
    WotbModVec3 position;
    WotbModVec3 direction;
} WotbModShotEventData;

typedef struct WotbModHitEventData {
    uint32_t shot_id;
    uint32_t shell_id;
    uint32_t flags;
    WotbModVec3 position;
    WotbModVec3 normal;
} WotbModHitEventData;

typedef struct WotbModDamageEventData {
    int32_t damage;
    int32_t previous_health;
    int32_t health;
    uint32_t reason_code;
    uint32_t source_entity_id;
} WotbModDamageEventData;

typedef struct WotbModReloadEventData {
    uint32_t state;
    uint32_t paused;
    float duration_seconds;
    float progress;
    float remaining_seconds;
} WotbModReloadEventData;

typedef struct WotbModAmmoEventData {
    uint32_t previous_shell_id;
    uint32_t shell_id;
    int32_t count;
} WotbModAmmoEventData;

typedef struct WotbModCameraEventData {
    uint32_t previous_mode;
    uint32_t mode;
    int32_t native_mode;
    uint32_t flags;
} WotbModCameraEventData;

/*
 * Server-confirmed kill (WOTBMOD_EVENT_VEHICLE_KILLED): who destroyed whom,
 * who assisted and why, straight from the arena's VehicleKilled packet.
 * killer_id / assist_id are 0 when nobody is credited.
 */
typedef struct WotbModVehicleKillEventData {
    uint32_t victim_id;
    uint32_t killer_id;
    uint32_t assist_id;
    uint32_t reason;
    uint32_t ammo_bay_exploded;
} WotbModVehicleKillEventData;

typedef union WotbModClientEventPayload {
    WotbModUiInputEventData ui_input;
    WotbModBattleEventData battle;
    WotbModVehicleEventData vehicle;
    WotbModShotEventData shot;
    WotbModHitEventData hit;
    WotbModDamageEventData damage;
    WotbModReloadEventData reload;
    WotbModAmmoEventData ammo;
    WotbModCameraEventData camera;
    WotbModVehicleKillEventData kill;
} WotbModClientEventPayload;

typedef enum WotbModClientEventType {
    WOTBMOD_EVENT_UI_SCREEN_CHANGED = 1u << 0,
    WOTBMOD_EVENT_SCENE_ACTIVATED = 1u << 1,
    WOTBMOD_EVENT_SCENE_DEACTIVATED = 1u << 2,
    WOTBMOD_EVENT_UI_INPUT = 1u << 3,
    WOTBMOD_EVENT_BATTLE_ENTERED = 1u << 4,
    WOTBMOD_EVENT_BATTLE_STARTED = 1u << 5,
    WOTBMOD_EVENT_BATTLE_ENDED = 1u << 6,
    WOTBMOD_EVENT_BATTLE_LEFT = 1u << 7,
    WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED = 1u << 8,
    WOTBMOD_EVENT_VEHICLE_SPAWNED = 1u << 9,
    WOTBMOD_EVENT_VEHICLE_DESPAWNED = 1u << 10,
    WOTBMOD_EVENT_SHOT_FIRED = 1u << 11,
    WOTBMOD_EVENT_SHELL_HIT = 1u << 12,
    WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED = 1u << 13,
    WOTBMOD_EVENT_VEHICLE_DAMAGED = 1u << 14,
    WOTBMOD_EVENT_VEHICLE_DESTROYED = 1u << 15,
    WOTBMOD_EVENT_RELOAD_STATE_CHANGED = 1u << 16,
    WOTBMOD_EVENT_AMMO_CHANGED = 1u << 17,
    WOTBMOD_EVENT_AIM_TARGET_CHANGED = 1u << 18,
    WOTBMOD_EVENT_VEHICLE_SPOTTED = 1u << 19,
    WOTBMOD_EVENT_VEHICLE_UNSPOTTED = 1u << 20,
    WOTBMOD_EVENT_CAMERA_MODE_CHANGED = 1u << 21,
    WOTBMOD_EVENT_VEHICLE_KILLED = 1u << 22,
    WOTBMOD_EVENT_ALL = 0x007FFFFFu
} WotbModClientEventType;

/*
 * Client events are delivered synchronously from DispatchFrame on the runtime
 * main thread. previous_resource and resource are borrowed callback-scoped
 * handles: they become invalid when the callback returns and must not be
 * released. Use resource_clone inside the callback to keep either object.
 *
 * UI_SCREEN_CHANGED uses previous_resource for the old UIControl and resource
 * for the new UIControl; either may be null. SCENE_ACTIVATED uses resource for
 * the activated Scene. SCENE_DEACTIVATED uses previous_resource for the
 * deactivated Scene.
 *
 * Gameplay events use payload_size plus the matching member of payload.
 * vehicle and other_vehicle are also borrowed callback-scoped snapshots. Use
 * vehicle_clone during the callback to retain one, then vehicle_release the
 * owned clone. A vehicle handle may be null when the entity has already left
 * the loader registry; the typed POD payload remains available.
 */
typedef struct WotbModClientEvent {
    uint32_t struct_size;
    uint32_t type;
    uint64_t sequence;
    WotbModResourceHandle previous_resource;
    WotbModResourceHandle resource;
    WotbModVehicleHandle vehicle;
    WotbModVehicleHandle other_vehicle;
    uint32_t payload_size;
    uint32_t flags;
    WotbModClientEventPayload payload;
} WotbModClientEvent;

typedef struct WotbModFrameInfo {
    uint32_t struct_size;
    uint64_t frame_index;
    double delta_seconds;
    void* swap_chain;      /* IDXGISwapChain*, kept opaque in the ABI */
    void* device;          /* ID3D11Device* */
    void* device_context;  /* ID3D11DeviceContext* */
    uint32_t back_buffer_width;
    uint32_t back_buffer_height;
} WotbModFrameInfo;

struct WotbModHostApi;

typedef void(WOTBMOD_CALL* WotbModEnableCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod);
typedef void(WOTBMOD_CALL* WotbModDisableCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod);
typedef void(WOTBMOD_CALL* WotbModUnloadCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod);
typedef void(WOTBMOD_CALL* WotbModFrameCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModFrameInfo* frame);
typedef void(WOTBMOD_CALL* WotbModMainThreadCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod,
    void* user_data);
typedef void(WOTBMOD_CALL* WotbModClientEventCallback)(
    const struct WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void* user_data);

/*
 * A mod fills this descriptor from WotbModLoad. String storage must remain
 * valid until on_unload, although the current host copies every string
 * immediately.
 */
typedef struct WotbModInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* id;
    const char* name;
    const char* version;
    const char* author;
    const char* description;
    uint32_t flags;
    WotbModEnableCallback on_enable;
    WotbModDisableCallback on_disable;
    WotbModUnloadCallback on_unload;
    WotbModFrameCallback on_frame;
} WotbModInfo;

/*
 * Fixed-size copy used by launchers and UI layers. Callers must set
 * struct_size before passing this structure to get_mod_info.
 */
typedef struct WotbModPublicInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t state;
    uint32_t enabled;
    uint32_t fault_count;
    uint32_t flags;
    char id[WOTBMOD_MAX_ID];
    char name[WOTBMOD_MAX_NAME];
    char version[WOTBMOD_MAX_VERSION];
    char author[WOTBMOD_MAX_AUTHOR];
    char description[WOTBMOD_MAX_DESCRIPTION];
    char module_path[WOTBMOD_MAX_PATH];
} WotbModPublicInfo;

typedef struct WotbModHostApi {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t host_version;
    uint32_t reserved;

    void(WOTBMOD_CALL* log)(
        WotbModHandle mod,
        WotbModLogLevel level,
        const char* message);

    /*
     * Buffer contract: set *inout_size to buffer capacity. On success it is
     * the byte count including the trailing NUL. With a null/short buffer,
     * WOTBMOD_ERROR_BUFFER_TOO_SMALL is returned and the required size is set.
     */
    WotbModResult(WOTBMOD_CALL* get_path)(
        WotbModHandle mod,
        WotbModPath path,
        char* buffer,
        uint32_t* inout_size);

    void*(WOTBMOD_CALL* get_game_module)(void);
    void*(WOTBMOD_CALL* resolve_rva)(uint32_t rva);
    void*(WOTBMOD_CALL* get_proc_address)(
        const char* loaded_module_name,
        const char* export_name);

    /*
     * mask uses 'x' for an exact byte and '?' for a wildcard. A null module
     * name scans the main executable image.
     */
    void*(WOTBMOD_CALL* find_pattern)(
        const char* loaded_module_name,
        const uint8_t* pattern,
        const char* mask);

    WotbModResult(WOTBMOD_CALL* hook_create)(
        WotbModHandle mod,
        void* target,
        void* detour,
        void** original);
    WotbModResult(WOTBMOD_CALL* hook_enable)(
        WotbModHandle mod,
        void* target);
    WotbModResult(WOTBMOD_CALL* hook_disable)(
        WotbModHandle mod,
        void* target);
    WotbModResult(WOTBMOD_CALL* hook_remove)(
        WotbModHandle mod,
        void* target);

    int32_t(WOTBMOD_CALL* config_get_int)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        int32_t default_value);
    WotbModResult(WOTBMOD_CALL* config_set_int)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        int32_t value);
    WotbModResult(WOTBMOD_CALL* config_get_string)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        const char* default_value,
        char* buffer,
        uint32_t buffer_size);
    WotbModResult(WOTBMOD_CALL* config_set_string)(
        WotbModHandle mod,
        const char* section,
        const char* key,
        const char* value);

    uint32_t(WOTBMOD_CALL* get_mod_count)(void);
    WotbModResult(WOTBMOD_CALL* get_mod_info)(
        uint32_t index,
        WotbModPublicInfo* out_info);
    WotbModResult(WOTBMOD_CALL* set_mod_enabled)(
        const char* id,
        int32_t enabled);

    /*
     * Resource overlays are resolved by priority and are active only while
     * their owner mod is enabled. Mount ids and loaded resource handles are
     * owned by the registering mod and cannot be released by another mod.
     */
    WotbModResult(WOTBMOD_CALL* resource_mount)(
        WotbModHandle mod,
        const WotbModResourceMountInfo* mount,
        WotbModResourceMountId* out_mount_id);
    WotbModResult(WOTBMOD_CALL* resource_unmount)(
        WotbModHandle mod,
        WotbModResourceMountId mount_id);
    WotbModResult(WOTBMOD_CALL* resource_resolve)(
        WotbModHandle mod,
        const char* virtual_path,
        char* buffer,
        uint32_t* inout_size);
    WotbModResult(WOTBMOD_CALL* resource_load)(
        WotbModHandle mod,
        const WotbModResourceLoadRequest* request,
        WotbModResourceHandle* out_resource);
    WotbModResult(WOTBMOD_CALL* resource_reload)(
        WotbModHandle mod,
        WotbModResourceHandle resource);
    WotbModResult(WOTBMOD_CALL* resource_release)(
        WotbModHandle mod,
        WotbModResourceHandle resource);

    /*
     * The audio backend owns the native playback object. Playback handles are
     * unique, mod-owned, and automatically stopped/released before the mod's
     * loaded resources are released. Passing null play_info uses safe 2D
     * defaults: volume=1, pitch=1, pan=0, distances=1..100.
     */
    WotbModResult(WOTBMOD_CALL* audio_play)(
        WotbModHandle mod,
        WotbModResourceHandle audio_clip,
        const WotbModAudioPlayInfo* play_info,
        WotbModAudioPlaybackHandle* out_playback);
    WotbModResult(WOTBMOD_CALL* audio_pause)(
        WotbModHandle mod,
        WotbModAudioPlaybackHandle playback);
    WotbModResult(WOTBMOD_CALL* audio_resume)(
        WotbModHandle mod,
        WotbModAudioPlaybackHandle playback);
    WotbModResult(WOTBMOD_CALL* audio_stop)(
        WotbModHandle mod,
        WotbModAudioPlaybackHandle playback);
    WotbModResult(WOTBMOD_CALL* audio_set_parameters)(
        WotbModHandle mod,
        WotbModAudioPlaybackHandle playback,
        const WotbModAudioPlayInfo* parameters);
    WotbModResult(WOTBMOD_CALL* audio_get_state)(
        WotbModHandle mod,
        WotbModAudioPlaybackHandle playback,
        WotbModAudioState* out_state);
    WotbModResult(WOTBMOD_CALL* audio_release)(
        WotbModHandle mod,
        WotbModAudioPlaybackHandle playback);

    /*
     * ABI 2.3 convenience wrapper for loading a custom audio file. The path
     * uses the resource overlay namespace and must resolve to a loose file
     * supplied by an enabled mod mount. It is equivalent to resource_load
     * with WOTBMOD_RESOURCE_AUDIO_CLIP, but avoids exposing resource request
     * boilerplate to audio-only mods.
     */
    WotbModResult(WOTBMOD_CALL* audio_clip_load)(
        WotbModHandle mod,
        const char* virtual_path,
        WotbModResourceHandle* out_audio_clip);

    /*
     * ABI 2.4 native sound events. These functions use events already
     * registered in the client's Wwise banks. They do not load arbitrary
     * external files; use audio_clip_load/audio_* for custom WAV/MP3/etc.
     *
     * Event handles are mod-owned and automatically stopped/released on
     * disable, fault, or unload. Position uses the DAVA world coordinate
     * system. Parameter names are converted to DAVA::FastName by the loader.
     */
    WotbModResult(WOTBMOD_CALL* sound_event_create)(
        WotbModHandle mod,
        const char* event_name,
        WotbModSoundEventHandle* out_event);
    WotbModResult(WOTBMOD_CALL* sound_event_trigger)(
        WotbModHandle mod,
        WotbModSoundEventHandle event);
    WotbModResult(WOTBMOD_CALL* sound_event_stop)(
        WotbModHandle mod,
        WotbModSoundEventHandle event);
    WotbModResult(WOTBMOD_CALL* sound_event_set_paused)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        int32_t paused);
    WotbModResult(WOTBMOD_CALL* sound_event_set_volume)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        float volume);
    WotbModResult(WOTBMOD_CALL* sound_event_set_position)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        float x,
        float y,
        float z);
    WotbModResult(WOTBMOD_CALL* sound_event_get_state)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        WotbModAudioState* out_state);
    WotbModResult(WOTBMOD_CALL* sound_event_set_parameter)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        const char* parameter_name,
        float value);
    WotbModResult(WOTBMOD_CALL* sound_event_get_parameter)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        const char* parameter_name,
        float* out_value);
    WotbModResult(WOTBMOD_CALL* sound_event_has_parameter)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        const char* parameter_name,
        int32_t* out_has_parameter);
    WotbModResult(WOTBMOD_CALL* sound_event_get_name)(
        WotbModHandle mod,
        WotbModSoundEventHandle event,
        char* buffer,
        uint32_t* inout_size);
    WotbModResult(WOTBMOD_CALL* sound_event_release)(
        WotbModHandle mod,
        WotbModSoundEventHandle event);

    /*
     * ABI 2.5 main-thread/mutation operations and ABI 2.6 object factories.
     *
     * main_thread_enqueue always defers the callback until the beginning of a
     * later DispatchFrame. Queued callbacks are owned by the calling mod and
     * are discarded before disable, fault cleanup, unload, or DLL release.
     *
     * UI/Scene operations execute immediately when called by the bound runtime
     * main thread. Calls from other threads are validated and queued; WOTBMOD_OK
     * then means the operation was accepted. Backend failures are logged.
     */
    WotbModResult(WOTBMOD_CALL* main_thread_enqueue)(
        WotbModHandle mod,
        WotbModMainThreadCallback callback,
        void* user_data);
    WotbModResult(WOTBMOD_CALL* ui_control_set_geometry)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        const WotbModUiControlGeometry* geometry);
    WotbModResult(WOTBMOD_CALL* ui_control_set_visible)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        int32_t visible);
    WotbModResult(WOTBMOD_CALL* ui_control_add_child)(
        WotbModHandle mod,
        WotbModResourceHandle parent,
        WotbModResourceHandle child);
    WotbModResult(WOTBMOD_CALL* ui_control_remove_child)(
        WotbModHandle mod,
        WotbModResourceHandle parent,
        WotbModResourceHandle child);
    WotbModResult(WOTBMOD_CALL* scene_set_transform)(
        WotbModHandle mod,
        WotbModResourceHandle scene,
        const WotbModSceneTransform* transform);
    WotbModResult(WOTBMOD_CALL* scene_add_child)(
        WotbModHandle mod,
        WotbModResourceHandle parent,
        WotbModResourceHandle child);
    WotbModResult(WOTBMOD_CALL* scene_remove_child)(
        WotbModHandle mod,
        WotbModResourceHandle parent,
        WotbModResourceHandle child);

    /*
     * ABI 2.6 direct native object factories and active roots.
     *
     * These four calls return a new mod-owned resource handle and therefore
     * must execute synchronously on the DispatchFrame thread. Calls from any
     * other thread return WOTBMOD_ERROR_WRONG_THREAD; use
     * main_thread_enqueue to enter the correct thread first.
     *
     * ui_control_create accepts a null geometry for the engine default rect.
     * ui_get_active_screen and scene_get_active retain the current native
     * object before publishing the handle, so resource_release is always the
     * correct way to release every returned handle.
     *
     * WOTBMOD_RESOURCE_SCENE handles represent DAVA Entity-compatible scene
     * graph nodes. scene_entity_create creates an empty Entity that can receive
     * a transform and be attached to a loaded or active Scene.
     */
    WotbModResult(WOTBMOD_CALL* ui_control_create)(
        WotbModHandle mod,
        const WotbModUiControlGeometry* geometry,
        WotbModResourceHandle* out_control);
    WotbModResult(WOTBMOD_CALL* ui_get_active_screen)(
        WotbModHandle mod,
        WotbModResourceHandle* out_screen);
    WotbModResult(WOTBMOD_CALL* scene_entity_create)(
        WotbModHandle mod,
        WotbModResourceHandle* out_entity);
    WotbModResult(WOTBMOD_CALL* scene_get_active)(
        WotbModHandle mod,
        WotbModResourceHandle* out_scene);

    /*
     * ABI 2.7 main-thread client events.
     *
     * resource_clone is the only supported way to retain a borrowed event
     * handle after its callback returns. The cloned handle is a normal
     * mod-owned resource and is released with resource_release or
     * automatically during disable, fault, or unload cleanup.
     *
     * event_mask is a bitwise OR of WotbModClientEventType values.
     * Subscriptions are mod-owned and automatically removed before disable,
     * fault cleanup, unload, or DLL release. Callbacks run only from
     * DispatchFrame and are isolated with the same SEH policy as lifecycle
     * and frame callbacks.
     */
    WotbModResult(WOTBMOD_CALL* resource_clone)(
        WotbModHandle mod,
        WotbModResourceHandle resource,
        WotbModResourceHandle* out_resource);
    WotbModResult(WOTBMOD_CALL* event_subscribe)(
        WotbModHandle mod,
        uint32_t event_mask,
        WotbModClientEventCallback callback,
        void* user_data,
        WotbModEventSubscriptionId* out_subscription_id);
    WotbModResult(WOTBMOD_CALL* event_unsubscribe)(
        WotbModHandle mod,
        WotbModEventSubscriptionId subscription_id);

    /*
     * ABI 2.8 existing UI tree access. Query calls are synchronous and must
     * run on the DispatchFrame thread. Returned parent/child handles own a
     * native Retain and are released with resource_release.
     */
    WotbModResult(WOTBMOD_CALL* ui_control_find_by_name)(
        WotbModHandle mod,
        WotbModResourceHandle root,
        const char* name,
        int32_t recursive,
        WotbModResourceHandle* out_control);
    WotbModResult(WOTBMOD_CALL* ui_control_get_parent)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        WotbModResourceHandle* out_parent);
    WotbModResult(WOTBMOD_CALL* ui_control_get_child_count)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        uint32_t* out_count);
    WotbModResult(WOTBMOD_CALL* ui_control_get_child_at)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        uint32_t index,
        WotbModResourceHandle* out_child);
    WotbModResult(WOTBMOD_CALL* ui_control_get_state)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        WotbModUiControlState* out_state);
    WotbModResult(WOTBMOD_CALL* ui_control_set_input_enabled)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        int32_t enabled,
        int32_t hierarchical);
    WotbModResult(WOTBMOD_CALL* ui_control_set_disabled)(
        WotbModHandle mod,
        WotbModResourceHandle control,
        int32_t disabled,
        int32_t hierarchical);

    /*
     * ABI 2.8 typed vehicle snapshots. No raw game-client pointers cross
     * this boundary. Query factories are dispatch-thread-only.
     */
    WotbModResult(WOTBMOD_CALL* vehicle_get_local)(
        WotbModHandle mod,
        WotbModVehicleHandle* out_vehicle);
    WotbModResult(WOTBMOD_CALL* vehicle_get_by_entity_id)(
        WotbModHandle mod,
        uint32_t entity_id,
        WotbModVehicleHandle* out_vehicle);
    WotbModResult(WOTBMOD_CALL* vehicle_get_count)(
        WotbModHandle mod,
        uint32_t* out_count);
    WotbModResult(WOTBMOD_CALL* vehicle_get_at)(
        WotbModHandle mod,
        uint32_t index,
        WotbModVehicleHandle* out_vehicle);
    WotbModResult(WOTBMOD_CALL* vehicle_clone)(
        WotbModHandle mod,
        WotbModVehicleHandle vehicle,
        WotbModVehicleHandle* out_vehicle);
    WotbModResult(WOTBMOD_CALL* vehicle_release)(
        WotbModHandle mod,
        WotbModVehicleHandle vehicle);
    WotbModResult(WOTBMOD_CALL* vehicle_get_info)(
        WotbModHandle mod,
        WotbModVehicleHandle vehicle,
        WotbModVehicleInfo* out_info);

    /*
     * ABI 2.9 stock vehicle asset replacement. Registration copies every
     * string and asset entry. replacement_virtual_path is resolved through
     * the registering mod's active resource mounts. Higher priority wins;
     * equal priority uses the most recently registered skin.
     *
     * set_enabled affects new native asset opens immediately. DAVA objects
     * already resident in memory are rebuilt by the client on its normal
     * model lifecycle (garage/battle/model reconnect).
     */
    WotbModResult(WOTBMOD_CALL* vehicle_skin_register)(
        WotbModHandle mod,
        const WotbModVehicleSkinDescriptor* descriptor,
        WotbModVehicleSkinHandle* out_skin);
    WotbModResult(WOTBMOD_CALL* vehicle_skin_set_enabled)(
        WotbModHandle mod,
        WotbModVehicleSkinHandle skin,
        int32_t enabled);
    WotbModResult(WOTBMOD_CALL* vehicle_skin_get_info)(
        WotbModHandle mod,
        WotbModVehicleSkinHandle skin,
        WotbModVehicleSkinInfo* out_info);
    WotbModResult(WOTBMOD_CALL* vehicle_skin_release)(
        WotbModHandle mod,
        WotbModVehicleSkinHandle skin);

    /*
     * There is deliberately no named raw-address registry here. Loader code
     * wires the reviewed resource bridge to the verified DAVA functions;
     * third-party mods receive only typed resource operations.
     */
} WotbModHostApi;

#define WOTBMOD_HOST_HAS(host, field)                                      \
    ((host) &&                                                            \
     (host)->struct_size >=                                                \
         offsetof(WotbModHostApi, field) + sizeof((host)->field) &&       \
     (host)->field != 0)

typedef WotbModResult(WOTBMOD_CALL* WotbModLoadFn)(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModInfo* out_info);

/*
 * Optional exports a loader may provide. Mods normally use the host pointer
 * passed to WotbModLoad; these exports are intended for the loader's own UI.
 */
WOTBMOD_HOST_EXPORT const WotbModHostApi* WOTBMOD_CALL
WotbModApi_GetHost(void);
WOTBMOD_HOST_EXPORT uint32_t WOTBMOD_CALL WotbModApi_GetVersion(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#define WOTBMOD_DEFINE_ENTRY(function_name)                                      \
    WOTBMOD_EXPORT WotbModResult WOTBMOD_CALL function_name(                    \
        const WotbModHostApi* host, WotbModHandle mod, WotbModInfo* out_info)

#define WOTBMOD_ENTRY WOTBMOD_DEFINE_ENTRY(WotbModLoad)

#ifdef __cplusplus
#include <stdarg.h>
#include <stdio.h>

static inline void wotbmod_logf(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModLogLevel level,
    const char* format,
    ...) {
    if (!host || !host->log || !format) return;
    char message[1024];
    va_list args;
    va_start(args, format);
#if defined(_MSC_VER)
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);
#else
    vsnprintf(message, sizeof(message), format, args);
#endif
    va_end(args);
    host->log(mod, level, message);
}
#endif
