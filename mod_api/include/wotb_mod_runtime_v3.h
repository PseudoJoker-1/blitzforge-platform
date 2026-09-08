#pragma once

#include "wotb_mod_api_v3.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void(WOTBMOD_V3_CALL* WotbModV3RuntimeLogSink)(
    uint32_t level,
    const char* category,
    const char* message,
    void* user_data);

/*
 * Version 1 backends stop at `remove` and get AROUND/REPLACE only. Version 2
 * appends the four observer slots (all required) and enables OBSERVE/AFTER
 * on the targets describe_target reports.
 */
#define WOTBMOD_V3_NATIVE_HOOK_BACKEND_VERSION 2u

typedef WotbModV3Result(WOTBMOD_V3_CALL*
    WotbModV3NativeHookResolveSymbol)(
        void* user_data,
        const char* symbol,
        void** out_target);
typedef WotbModV3Result(WOTBMOD_V3_CALL*
    WotbModV3NativeHookCreate)(
        void* user_data,
        void* target,
        void* detour,
        void** out_original);
typedef WotbModV3Result(WOTBMOD_V3_CALL*
    WotbModV3NativeHookOperation)(
        void* user_data,
        void* target);
/* Mask of (1u << WotbModV3HookMode) bits the backend can observe on this
 * target; OK with mask 0 for a target it does not describe. */
typedef WotbModV3Result(WOTBMOD_V3_CALL*
    WotbModV3NativeHookDescribe)(
        void* user_data,
        void* target,
        uint32_t* out_mode_mask);
/* Attaches an OBSERVE/AFTER callback with the target's native signature.
 * The token is never reused. */
typedef WotbModV3Result(WOTBMOD_V3_CALL*
    WotbModV3NativeHookAttach)(
        void* user_data,
        void* target,
        uint32_t mode,
        int32_t priority,
        void* detour,
        uint64_t* out_token);
typedef WotbModV3Result(WOTBMOD_V3_CALL*
    WotbModV3NativeHookDetach)(
        void* user_data,
        uint64_t token);
typedef WotbModV3Result(WOTBMOD_V3_CALL*
    WotbModV3NativeHookSetAttachedEnabled)(
        void* user_data,
        uint64_t token,
        uint32_t enabled);

/*
 * Loader-owned native hook bridge. The runtime copies this table during
 * initialization, so the caller may keep the descriptor itself on the stack.
 * resolve_symbol must expose only reviewed, build-validated symbolic targets.
 */
typedef struct WotbModV3NativeHookBackend {
    uint32_t struct_size;
    uint32_t api_version;
    void* user_data;
    WotbModV3NativeHookResolveSymbol resolve_symbol;
    WotbModV3NativeHookCreate create;
    WotbModV3NativeHookOperation enable;
    WotbModV3NativeHookOperation disable;
    WotbModV3NativeHookOperation remove;
    /* Version 2 and later. */
    WotbModV3NativeHookDescribe describe_target;
    WotbModV3NativeHookAttach attach;
    WotbModV3NativeHookDetach detach;
    WotbModV3NativeHookSetAttachedEnabled set_attached_enabled;
} WotbModV3NativeHookBackend;

typedef struct WotbModV3RuntimeOptions {
    uint32_t struct_size;
    uint32_t api_version;
    const char* game_directory;
    const char* mods_directory;
    const char* cache_directory;
    const char* config_directory;
    const char* client_version;
    const char* executable_sha256;
    uint32_t binding_pack_version;
    uint32_t process_architecture;
    WotbModV3RuntimeLogSink log_sink;
    void* log_user_data;
    const WotbModV3NativeHookBackend* native_hook_backend;
} WotbModV3RuntimeOptions;

/*
 * Loader-owned event ingress availability. These bits describe native or
 * polled sources that were actually installed for the current client build.
 * Runtime-owned lifecycle, frame.update, and client shutdown topics do not
 * need a source bit and remain available while the runtime is active.
 */
#define WOTBMOD_V3_EVENT_SOURCE_UI_SCREEN \
    (UINT64_C(1) << 0)
#define WOTBMOD_V3_EVENT_SOURCE_SCENE_ACTIVATED \
    (UINT64_C(1) << 1)
#define WOTBMOD_V3_EVENT_SOURCE_SCENE_DEACTIVATED \
    (UINT64_C(1) << 2)
#define WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD \
    (UINT64_C(1) << 3)
#define WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD \
    (UINT64_C(1) << 4)
#define WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING \
    (UINT64_C(1) << 5)
#define WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH \
    (UINT64_C(1) << 6)
#define WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE \
    (UINT64_C(1) << 7)
#define WOTBMOD_V3_EVENT_SOURCE_RELOAD_STATE \
    (UINT64_C(1) << 8)
#define WOTBMOD_V3_EVENT_SOURCE_UI_INPUT \
    (UINT64_C(1) << 9)
#define WOTBMOD_V3_EVENT_SOURCE_AMMO_CHANGED \
    (UINT64_C(1) << 10)
#define WOTBMOD_V3_EVENT_SOURCE_AIM_TARGET \
    (UINT64_C(1) << 11)
#define WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS \
    (UINT64_C(1) << 12)
#define WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT \
    (UINT64_C(1) << 13)
#define WOTBMOD_V3_EVENT_SOURCE_LOCAL_SHELL \
    (UINT64_C(1) << 14)
#define WOTBMOD_V3_EVENT_SOURCE_CAMERA_MODE \
    (UINT64_C(1) << 15)
/*
 * The visible-tracer ingress. Set only when the TracerManager hooks actually
 * installed for this client build, so a mod that subscribes to the tracer
 * topics learns up front whether anything can ever publish them. Bit 16 is
 * additive: bits 0..15 keep their meaning and a mod built against the older
 * header keeps working unchanged.
 */
#define WOTBMOD_V3_EVENT_SOURCE_VISIBLE_TRACER \
    (UINT64_C(1) << 16)
/*
 * GES bus ingress. Set only when the loader's GES backend installed on this
 * client build; every "wotbmod.ges.*" topic requires it.
 */
#define WOTBMOD_V3_EVENT_SOURCE_GES \
    (UINT64_C(1) << 17)
/*
 * ClientArena kill ingress (2026-09-06): set when the loader's hook on the
 * arena's VehicleKilled handler installed; "wotbmod.vehicle.killed" needs it.
 */
#define WOTBMOD_V3_EVENT_SOURCE_VEHICLE_KILLED \
    (UINT64_C(1) << 18)
/*
 * Login-manager capture (2026-09-08): set when the loader's OnHostChosen
 * detour installed; "wotbmod.session.cluster.changed" needs it.
 */
#define WOTBMOD_V3_EVENT_SOURCE_SESSION_CLUSTER \
    (UINT64_C(1) << 19)
#define WOTBMOD_V3_EVENT_SOURCE_ALL \
    ((UINT64_C(1) << 20) - UINT64_C(1))

typedef struct WotbModV3RuntimeModuleInfo {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mod;
    uint32_t permission_tier;
    uint32_t state;
    char id[WOTBMOD_V3_MAX_ID];
    char name[WOTBMOD_V3_MAX_NAME];
    char version[WOTBMOD_V3_MAX_VERSION];
    char author[WOTBMOD_V3_MAX_NAME];
    char description[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3RuntimeModuleInfo;

#define WOTBMOD_V3_RUNTIME_MAX_PERMISSION_GRANTS 256u
#define WOTBMOD_V3_RUNTIME_MAX_DEPENDENCIES 128u

WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_Initialize(
    const WotbModV3RuntimeOptions* options);
void WOTBMOD_V3_CALL WotbModV3Runtime_Shutdown(void);
const WotbModV3Bootstrap* WOTBMOD_V3_CALL WotbModV3Runtime_GetBootstrap(void);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_CreateMod(
    const char* module_path,
    uint32_t granted_permission_tier,
    WotbModV3Handle* out_mod);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_InvokeEntry(
    WotbModV3Handle mod,
    WotbModLoadV3Fn entry,
    WotbModV3RuntimeModuleInfo* out_info);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_Enable(
    WotbModV3Handle mod);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_Disable(
    WotbModV3Handle mod);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_DestroyMod(
    WotbModV3Handle mod);
void WOTBMOD_V3_CALL WotbModV3Runtime_DispatchFrame(
    uint64_t frame_index,
    double delta_seconds);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_SetContext(
    uint64_t context_mask);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_SetCapability(
    const WotbModV3CapabilityInfo* capability);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_SetPermissionTier(
    WotbModV3Handle mod,
    uint32_t granted_permission_tier);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_SetPermissionGrants(
    WotbModV3Handle mod,
    const char* const* permission_names,
    uint32_t permission_count,
    uint32_t restrict_to_names);
/*
 * Loader-only package metadata handoff. Package roots and dependency
 * descriptors are copied by the runtime and may be released by the caller
 * after these functions return. Metadata can only be installed before the
 * module entry point is invoked.
 */
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_SetPackageRoot(
    WotbModV3Handle mod,
    const char* package_root);
WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_SetDependencies(
    WotbModV3Handle mod,
    const WotbModV3ManifestDependency* dependencies,
    uint32_t dependency_count);
void WOTBMOD_V3_CALL WotbModV3Runtime_SetEventSourceMask(
    uint64_t source_mask);

#ifdef __cplusplus
}
#endif
