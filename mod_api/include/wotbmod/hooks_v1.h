#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_HOOKS_VERSION 1u
#define WOTBMOD_V3_MAX_HOOK_TARGET 192u

typedef enum WotbModV3HookMode {
    WOTBMOD_V3_HOOK_BEFORE = 0,
    WOTBMOD_V3_HOOK_AFTER = 1,
    WOTBMOD_V3_HOOK_AROUND = 2,
    WOTBMOD_V3_HOOK_REPLACE = 3,
    WOTBMOD_V3_HOOK_OBSERVE = 4
} WotbModV3HookMode;

typedef enum WotbModV3HookStatus {
    WOTBMOD_V3_HOOK_STATUS_PENDING_BACKEND = 0,
    WOTBMOD_V3_HOOK_STATUS_DISABLED = 1,
    WOTBMOD_V3_HOOK_STATUS_ENABLED = 2,
    WOTBMOD_V3_HOOK_STATUS_CONFLICT = 3,
    WOTBMOD_V3_HOOK_STATUS_REMOVED = 4,
    WOTBMOD_V3_HOOK_STATUS_FAILED = 5
} WotbModV3HookStatus;

typedef enum WotbModV3HookCreateFlags {
    WOTBMOD_V3_HOOK_CREATE_NONE = 0,
    WOTBMOD_V3_HOOK_CREATE_ALLOW_PENDING = 1u << 0,
    WOTBMOD_V3_HOOK_CREATE_ALLOW_CONFLICT = 1u << 1
} WotbModV3HookCreateFlags;

typedef struct WotbModV3HookCreateInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t mode;
    int32_t priority;
    uint32_t flags;
    uint32_t reserved;
    void* detour;
} WotbModV3HookCreateInfo;

typedef struct WotbModV3HookInfo {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3HookHandle hook;
    WotbModV3Handle owner_mod;
    uint64_t creation_order;
    uint32_t mode;
    uint32_t status;
    int32_t priority;
    uint32_t conflict_count;
    char target[WOTBMOD_V3_MAX_HOOK_TARGET];
} WotbModV3HookInfo;

typedef struct WotbModV3HookConflictInfo {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3HookHandle hook;
    WotbModV3HookHandle conflicting_hook;
    WotbModV3Handle conflicting_owner;
    uint32_t conflicting_mode;
    char reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3HookConflictInfo;

typedef struct WotbModV3HooksApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* create_symbol)(
        WotbModV3Handle mod,
        const char* symbol,
        const WotbModV3HookCreateInfo* info,
        WotbModV3HookHandle* out_hook);
    WotbModV3Result(WOTBMOD_V3_CALL* create_vtable)(
        WotbModV3Handle mod,
        void* object,
        uint32_t slot,
        const WotbModV3HookCreateInfo* info,
        WotbModV3HookHandle* out_hook);
    WotbModV3Result(WOTBMOD_V3_CALL* enable)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook);
    WotbModV3Result(WOTBMOD_V3_CALL* disable)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook);
    WotbModV3Result(WOTBMOD_V3_CALL* remove)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook);
    WotbModV3Result(WOTBMOD_V3_CALL* set_priority)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        int32_t priority);
    WotbModV3Result(WOTBMOD_V3_CALL* run_before)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        WotbModV3HookHandle other_hook);
    WotbModV3Result(WOTBMOD_V3_CALL* run_after)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        WotbModV3HookHandle other_hook);
    WotbModV3Result(WOTBMOD_V3_CALL* get_original)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        void** out_original);
    WotbModV3Result(WOTBMOD_V3_CALL* call_next)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        const WotbModV3ConstBuffer* arguments,
        WotbModV3Buffer* result);
    WotbModV3Result(WOTBMOD_V3_CALL* get_info)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        WotbModV3HookInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* get_owner)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        WotbModV3Handle* out_owner);
    WotbModV3Result(WOTBMOD_V3_CALL* get_status)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        uint32_t* out_status);
    WotbModV3Result(WOTBMOD_V3_CALL* get_chain)(
        WotbModV3Handle mod,
        const char* target,
        WotbModV3HookInfo* hooks,
        uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* get_conflicts)(
        WotbModV3Handle mod,
        WotbModV3HookHandle hook,
        WotbModV3HookConflictInfo* conflicts,
        uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* enumerate)(
        WotbModV3Handle mod,
        WotbModV3HookInfo* hooks,
        uint32_t* inout_count);
} WotbModV3HooksApiV1;

#ifdef __cplusplus
}
#endif
