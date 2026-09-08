#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../include/wotb_mod_runtime.h"
#include "../include/wotbmod/bigworld_rpc_v1.h"
#include "../include/wotbmod/entity_public_v1.h"
#include "../include/wotbmod/projectile_v2.h"
#include "../include/wotbmod/session_cluster_v1.h"
#include "../include/wotb_mod_dava_resources.h"

/*
 * Native bindings are private to the loader. They validate the exact client
 * fingerprint and fixed-RVA prologues before installing hooks, then expose one
 * compiler-neutral invoke bridge to the V3 runtime.
 */

typedef void(WOTBMOD_CALL* WotbModV3NativeBindingsLog)(
    const char* message,
    void* user_data);

typedef void(WOTBMOD_CALL* WotbModV3NativeAvatarObserved)(
    void* avatar,
    void* user_data);

typedef struct WotbModV3NativeBindingsOptions {
    uint32_t struct_size;
    HMODULE game_module;
    const char* game_executable_path;
    /* Optional UTF-8 path for the atomic machine-readable validation report. */
    const char* binding_validation_report_path;
    WotbModV3NativeBindingsLog log;
    void* log_user_data;
    const WotbModRuntimeResourceBackend* resource_backend;
    const WotbModRuntimeAudioBackend* audio_backend;
    const WotbModRuntimeSoundBackend* sound_backend;
    const WotbModRuntimeGameplayBackend* gameplay_backend;
    /* Private resource-wrapper identity used to compare retained DAVA
     * wrappers that refer to the same native object. */
    void* (WOTBMOD_CALL* resource_identity)(
        void* user_data,
        void* resource);
    void* resource_identity_user_data;
    WotbModResult (WOTBMOD_CALL* resource_bring_ui_to_front)(
        void* user_data,
        void* resource);
    void* resource_bring_ui_to_front_user_data;
    /*
     * Optional private loader callback. Camera mode events carry the Avatar
     * instance even when a replay never emits updateVehicleHealth, allowing
     * the loader to recover the observed/local Vehicle through its already
     * fingerprint-gated resolver.
     */
    WotbModV3NativeAvatarObserved avatar_observed;
    void* avatar_observed_user_data;
} WotbModV3NativeBindingsOptions;

enum WotbModV3NativePublicVehicleFlags {
    WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE = 1u << 0,
    WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL = 1u << 1,
    WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_DESTROYED = 1u << 2,
    /* Same team as the independently resolved local/observed vehicle. */
    WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY = 1u << 3
};

enum WotbModV3NativePublicEntityField {
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ID = 1u << 0,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TYPE = 1u << 1,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VISIBILITY = 1u << 2,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_LOCAL = 1u << 3,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM = 1u << 4,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH = 1u << 5,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH = 1u << 6,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_POSITION = 1u << 7,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DIRECTION = 1u << 8,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE = 1u << 9,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DISPLAY_NAME = 1u << 10,
    /* arena roster extras, mirrored by CLIENT_HOST_PUBLIC_ENTITY_FIELD_* */
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_CLAN_TAG = 1u << 11,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ACCOUNT_ID = 1u << 12,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_KILLS = 1u << 13,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_NAME = 1u << 14,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME = 1u << 15,
    WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELDS_ALL =
        (1u << 16) - 1u
};

enum WotbModV3NativeInstalledSource {
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CAMERA_CTOR = 1u << 0,
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CAMERA_DTOR = 1u << 1,
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CLIENT_INITIALIZE = 1u << 2,
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_LOCAL_SHELL = 1u << 3,
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_ENTITY_CTOR = 1u << 4,
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_ENTITY_DTOR = 1u << 5,
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_TRACER_MANAGER_CTOR = 1u << 6,
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CAMERA_MODE = 1u << 7,
    /*
     * The ShowTracer hook, which is what actually announces a visible tracer.
     * Kept separate from TRACER_MANAGER_CTOR: the ctor only caches the manager
     * so a mod can request a stock tracer, and reporting the public
     * visible-tracer ingress from that bit would claim a publisher exists when
     * nothing would ever publish.
     */
    WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_TRACER_SHOW = 1u << 8
};

static inline int
WotbModV3NativeBindings_IsPublicVehicleFlags(uint32_t flags) {
    return (flags &
            (WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE |
             WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL |
             WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY)) != 0u;
}

typedef struct WotbModV3NativePublicVehicle {
    uint32_t struct_size;
    uint32_t public_id;
    int32_t health;
    int32_t max_health;
    uint32_t flags;
    uint32_t team;
    /*
     * Private native-ingress provenance mask. Zero selects the current stock
     * vehicle bridge's conservative field set for source compatibility.
     */
    uint64_t valid_fields;
    void* native_entity;
    void* native_game_logic;
    char display_name[WOTBMOD_V3_MAX_NAME];
    /*
     * World-space pose, meaningful only with FIELD_POSITION / FIELD_DIRECTION
     * in valid_fields: position is the hull origin, direction the hull's unit
     * forward axis. The loader fills them for the controlled vehicle from the
     * PlayerController pose block.
     */
    float position[3];
    float direction[3];
    /*
     * Arena roster extras (ClientArena::VehicleInfo, 2026-09-06), each
     * meaningful only with its FIELD_* bit: Wargaming account id, kills so
     * far in this battle, clan tag and the vehicle's name.
     */
    int64_t account_id;
    int32_t kills;
    char clan_tag[32];
    char vehicle_name[WOTBMOD_V3_MAX_NAME];
    /* localized through the client's own LocalizationSystem tables */
    char vehicle_display_name[WOTBMOD_V3_MAX_NAME];
} WotbModV3NativePublicVehicle;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Verifies the running executable against the frozen binding-pack SHA-256.
 *
 * The loader must call this before installing any fixed-RVA hook: the DAVA
 * sound/resource backends, the Scene trackers, the file resolver and the
 * gameplay hooks are all gated on the result, so a detour can never land on a
 * build this pack was not generated for. The digest is computed once per
 * process and reused by WotbModV3NativeBindings_Create.
 *
 * Returns WOTBMOD_V3_OK only on an exact match. Any other result - including
 * E_IO or E_PLATFORM when the executable cannot be read - must be treated as
 * "do not install native bindings".
 */
WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_VerifyClientFingerprint(
    HMODULE game_module,
    const char* game_executable_path,
    WotbModV3NativeBindingsLog log,
    void* log_user_data);

WotbModV3Result WOTBMOD_CALL WotbModV3NativeBindings_Create(
    const WotbModV3NativeBindingsOptions* options,
    WotbModRuntimeV3ClientBackend* out_backend);

/*
 * Offers the loader half of the five interfaces declared 2026-08-16
 * (wotbmod.ui.read, wotbmod.camera.state, wotbmod.audio.intercept,
 * wotbmod.scene.enumerate, wotbmod.tracer) to the V3 runtime.
 *
 * Must be called AFTER WotbModRuntime_Initialize has succeeded: installing
 * the table republishes those five interfaces' availability, which needs a
 * live interface registry. Refused with WOTBMOD_V3_E_CLIENT_MISMATCH unless
 * the binding pack proved this exact client; a group whose evidence is
 * missing is left null and its interface stays UNAVAILABLE with the frozen
 * reason. WotbModV3NativeBindings_Shutdown removes the table again, which
 * quiesces every in-flight call and disarms the sound detour.
 */
WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_InstallDeclaredBackend(void);

/* wotbmod.ges slots for the declared backend, handed over by the loader once
 * its SubscribeImpl capture hook and type index are installed. This unit is
 * also linked into host tests without the loader, so it never references
 * loader symbols itself; a null in any slot withdraws the whole group (the
 * backend is all-or-nothing there). Call before (re-)installing the declared
 * backend. The signatures are ClientHostGes*Fn from client_services_backend.h,
 * spelled out because that header is not part of this one. */
typedef void (*WotbModV3NativeGesVisitFn)(void* visit_data, const char* type_name);
void WOTBMOD_CALL WotbModV3NativeBindings_SetGesProvider(
    WotbModV3Result (*list_types)(void* user, WotbModV3NativeGesVisitFn visit, void* visit_data),
    WotbModV3Result (*observe)(void* user, const char* type_name, uint32_t observe),
    WotbModV3Result (*publish)(void* user, const char* type_name, const void* payload,
                               uint32_t payload_size, uint32_t flags));

/* wotbmod.session.cluster slots for the declared backend, handed over by the
 * loader once its OnHostChosen capture hook is installed. Same rules as the
 * ges provider: all four or none. Signatures are ClientHostSessionCluster*Fn
 * from client_services_backend.h. */
void WOTBMOD_CALL WotbModV3NativeBindings_SetSessionClusterProvider(
    WotbModV3Result (*enumerate)(void* user, WotbModV3ClusterInfo* items, uint32_t* inout_count),
    WotbModV3Result (*get_current)(void* user, WotbModV3ClusterInfo* out_info),
    WotbModV3Result (*change)(void* user, int32_t cluster_id),
    WotbModV3Result (*set_manual)(void* user, uint32_t manual));

/* Exact-client, main-thread-only stock tracer creation. This is consumed by
 * the loader-private DAVA provider and never exposed as a raw native call to
 * mods or Lua. */
WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_CreateStockTracer(
    void* user_data,
    const WotbModDavaNativeTracerRequest* request);

/* Loader-private dynamic thread-role query for exact DAVA providers. */
uint32_t WOTBMOD_CALL
WotbModV3NativeBindings_IsMainThread(void* user_data);

/* Loader-private blocking bridge into the proven DAVA main-thread pump. */
WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_InvokeMainThread(
    void* user_data,
    WotbModDavaMainThreadCallFn call,
    void* call_data);

WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ResolveHookSymbol(
    const char* symbol,
    void** out_target);

uint32_t WOTBMOD_CALL
WotbModV3NativeBindings_GetInstalledSourceMask(void);

void WOTBMOD_CALL WotbModV3NativeBindings_UpdateFrame(
    void* swap_chain,
    void* device,
    void* device_context,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index,
    double delta_seconds);

/*
 * Safe gameplay ingress. Upsert rejects vehicles that are neither local,
 * visible nor confirmed to share the independently resolved local team.
 * ObserveRpcMetadata additionally requires the public id to be present in the
 * public registry, so an accidental hidden-entity call cannot leak metadata.
 */
WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_UpsertPublicVehicle(
    const WotbModV3NativePublicVehicle* vehicle,
    uint32_t reason);

WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_RemovePublicVehicle(
    uint32_t public_id,
    uint32_t reason);

WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveRpcMetadata(
    uint32_t direction,
    uint32_t public_entity_id,
    const char* entity_type,
    const char* method_name);

/*
 * Provenance-safe projectile ingress from reviewed stock callbacks. A shot
 * observation contains only the fields supplied by showShooting. An impact
 * contains the exact native shot id, shell kind, flags and world position
 * supplied by OnVehicleHitDamage; the two streams are not guessed together.
 */
WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveShot(
    uint32_t primary_public_entity_id,
    uint32_t shot_code);

WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveImpact(
    uint32_t primary_public_entity_id,
    uint32_t secondary_public_entity_id,
    const WotbModV3Vec3* position,
    uint32_t shell_type,
    uint32_t native_flags,
    uint32_t native_shot_id);

/* Queues a reviewed DAVA UIEvent phase for frame-bound V3 dispatch. */
WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveUiInput(
    uint32_t phase,
    float screen_x,
    float screen_y,
    float delta_x,
    float delta_y,
    uint32_t modifiers);

void WOTBMOD_CALL
WotbModV3NativeBindings_SetGameplayBridgeSources(
    uint32_t installed_hook_mask);

void WOTBMOD_CALL
WotbModV3NativeBindings_ResetPublicGameplayState(
    uint32_t reason);

void WOTBMOD_CALL WotbModV3NativeBindings_Shutdown(void);

#ifdef __cplusplus
}
#endif
