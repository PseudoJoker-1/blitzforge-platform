#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_LIFECYCLE_VERSION 1u

typedef enum WotbModV3LifecycleState {
    WOTBMOD_V3_MOD_STATE_UNKNOWN = 0,
    WOTBMOD_V3_MOD_STATE_LOADED = 1,
    WOTBMOD_V3_MOD_STATE_ENABLED = 2,
    WOTBMOD_V3_MOD_STATE_DISABLED = 3,
    WOTBMOD_V3_MOD_STATE_UNLOADING = 4,
    WOTBMOD_V3_MOD_STATE_UNLOADED = 5
} WotbModV3LifecycleState;

typedef enum WotbModV3LifecycleTransition {
    WOTBMOD_V3_MOD_TRANSITION_PRELOAD = 0,
    WOTBMOD_V3_MOD_TRANSITION_LOADED = 1,
    WOTBMOD_V3_MOD_TRANSITION_ENABLED = 2,
    WOTBMOD_V3_MOD_TRANSITION_DISABLED = 3,
    WOTBMOD_V3_MOD_TRANSITION_UNLOADING = 4,
    WOTBMOD_V3_MOD_TRANSITION_UNLOADED = 5
} WotbModV3LifecycleTransition;

typedef enum WotbModV3CleanupReason {
    WOTBMOD_V3_CLEANUP_RELEASED = 0,
    WOTBMOD_V3_CLEANUP_MOD_DISABLED = 1,
    WOTBMOD_V3_CLEANUP_MOD_UNLOADING = 2,
    WOTBMOD_V3_CLEANUP_RUNTIME_SHUTDOWN = 3
} WotbModV3CleanupReason;

typedef struct WotbModV3LifecycleInfo {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mod;
    uint32_t state;
    uint32_t permission_tier;
    uint32_t hot_reload_supported;
    uint32_t reserved;
    char id[WOTBMOD_V3_MAX_ID];
    char hot_reload_reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3LifecycleInfo;

typedef struct WotbModV3LifecycleEvent {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mod;
    uint32_t transition;
    uint32_t state;
    char id[WOTBMOD_V3_MAX_ID];
} WotbModV3LifecycleEvent;

typedef void(WOTBMOD_V3_CALL* WotbModV3CleanupCallback)(
    WotbModV3Handle mod,
    uint32_t reason,
    void* user_data);

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3LifecycleCopyPathFn)(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size);

typedef struct WotbModV3LifecycleApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_current)(
        WotbModV3Handle mod,
        WotbModV3Handle* out_current_mod);
    WotbModV3Result(WOTBMOD_V3_CALL* get_info)(
        WotbModV3Handle mod,
        WotbModV3LifecycleInfo* out_info);
    WotbModV3LifecycleCopyPathFn get_install_path;
    WotbModV3LifecycleCopyPathFn get_resource_path;
    WotbModV3LifecycleCopyPathFn get_data_path;
    WotbModV3LifecycleCopyPathFn get_cache_path;
    WotbModV3LifecycleCopyPathFn get_config_path;
    WotbModV3Result(WOTBMOD_V3_CALL* register_cleanup)(
        WotbModV3Handle mod,
        WotbModV3CleanupCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unregister_cleanup)(
        WotbModV3Handle mod,
        WotbModV3Token token);
    WotbModV3Result(WOTBMOD_V3_CALL* request_enable)(
        WotbModV3Handle mod,
        WotbModV3Handle target_mod);
    WotbModV3Result(WOTBMOD_V3_CALL* request_disable)(
        WotbModV3Handle mod,
        WotbModV3Handle target_mod);
    WotbModV3Result(WOTBMOD_V3_CALL* request_reload)(
        WotbModV3Handle mod,
        WotbModV3Handle target_mod);
    WotbModV3Result(WOTBMOD_V3_CALL* can_hot_reload)(
        WotbModV3Handle mod,
        WotbModV3Handle target_mod,
        uint32_t* out_can_hot_reload,
        char* reason,
        uint32_t* inout_reason_size);
} WotbModV3LifecycleApiV1;

#define WOTBMOD_V3_EVENT_MOD_PRELOAD "wotbmod.mod.preload"
#define WOTBMOD_V3_EVENT_MOD_LOADED "wotbmod.mod.loaded"
#define WOTBMOD_V3_EVENT_MOD_ENABLED "wotbmod.mod.enabled"
#define WOTBMOD_V3_EVENT_MOD_DISABLED "wotbmod.mod.disabled"
#define WOTBMOD_V3_EVENT_MOD_UNLOADING "wotbmod.mod.unloading"
#define WOTBMOD_V3_EVENT_MOD_UNLOADED "wotbmod.mod.unloaded"

#ifdef __cplusplus
}
#endif
