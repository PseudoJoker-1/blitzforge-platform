#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_RESOURCES_VERSION 1u
#define WOTBMOD_V3_RESOURCE_WATCH_EVENT_VERSION 1u
#define WOTBMOD_V3_EVENT_RESOURCE_INVALIDATED \
    "wotbmod.resources.invalidated"
#define WOTBMOD_V3_EVENT_RESOURCE_RELOADED \
    "wotbmod.resources.reloaded"

typedef enum WotbModV3ResourceType {
    WOTBMOD_V3_RESOURCE_BINARY = 1,
    WOTBMOD_V3_RESOURCE_TEXT = 2,
    WOTBMOD_V3_RESOURCE_IMAGE = 3,
    WOTBMOD_V3_RESOURCE_AUDIO = 4,
    WOTBMOD_V3_RESOURCE_MODEL = 5,
    WOTBMOD_V3_RESOURCE_YAML = 6,
    WOTBMOD_V3_RESOURCE_JSON = 7
} WotbModV3ResourceType;

typedef enum WotbModV3ResourceState {
    WOTBMOD_V3_RESOURCE_LOADING = 1,
    WOTBMOD_V3_RESOURCE_READY = 2,
    WOTBMOD_V3_RESOURCE_FAILED = 3,
    WOTBMOD_V3_RESOURCE_CANCELLED = 4
} WotbModV3ResourceState;

typedef enum WotbModV3ResourceLoadFlags {
    WOTBMOD_V3_RESOURCE_LOAD_DEFAULT = 0,
    WOTBMOD_V3_RESOURCE_LOAD_REQUIRE_NATIVE_CLIENT = 1u << 0
} WotbModV3ResourceLoadFlags;

typedef enum WotbModV3ResourceBacking {
    WOTBMOD_V3_RESOURCE_BACKING_NONE = 0,
    WOTBMOD_V3_RESOURCE_BACKING_RAW_VFS_BYTES = 1,
    WOTBMOD_V3_RESOURCE_BACKING_NATIVE_CLIENT_OBJECT = 2
} WotbModV3ResourceBacking;

typedef struct WotbModV3ResourceLoadDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t expected_type;
    uint32_t flags;
    uint64_t max_bytes;
    char uri[WOTBMOD_V3_MAX_PATH];
    char group[WOTBMOD_V3_MAX_ID];
    char expected_sha256[65];
} WotbModV3ResourceLoadDesc;

typedef struct WotbModV3ResourceInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t state;
    float progress;
    uint32_t backing;
    uint64_t memory_bytes;
    char uri[WOTBMOD_V3_MAX_PATH];
    char sha256[65];
    char error[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3ResourceInfo;

typedef struct WotbModV3ResourceWatchEvent {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Token watch_token;
    WotbModV3ResourceHandle resource;
    uint32_t change_kind;
    uint32_t type;
    uint32_t state;
    uint32_t reserved;
    uint64_t memory_bytes;
    char uri[WOTBMOD_V3_MAX_PATH];
    char sha256[65];
} WotbModV3ResourceWatchEvent;

typedef struct WotbModV3ResourcesApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* load)(
        WotbModV3Handle mod,
        const WotbModV3ResourceLoadDesc* desc,
        WotbModV3ResourceHandle* out_resource);
    WotbModV3Result(WOTBMOD_V3_CALL* load_async_ex)(
        WotbModV3Handle mod,
        const WotbModV3ResourceLoadDesc* desc,
        WotbModV3ResourceHandle* out_resource);
    WotbModV3Result(WOTBMOD_V3_CALL* get_info)(
        WotbModV3Handle mod,
        WotbModV3ResourceHandle resource,
        WotbModV3ResourceInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* copy_data)(
        WotbModV3Handle mod,
        WotbModV3ResourceHandle resource,
        WotbModV3Buffer* inout_buffer);
    WotbModV3Result(WOTBMOD_V3_CALL* retain)(
        WotbModV3Handle mod,
        WotbModV3ResourceHandle resource);
    WotbModV3Result(WOTBMOD_V3_CALL* release)(
        WotbModV3Handle mod,
        WotbModV3ResourceHandle resource);
    WotbModV3Result(WOTBMOD_V3_CALL* preload_group)(
        WotbModV3Handle mod,
        const char* group,
        const WotbModV3ResourceLoadDesc* resources,
        uint32_t resource_count);
    WotbModV3Result(WOTBMOD_V3_CALL* unload_group)(
        WotbModV3Handle mod,
        const char* group);
    WotbModV3Result(WOTBMOD_V3_CALL* reload)(
        WotbModV3Handle mod,
        WotbModV3ResourceHandle resource);
    WotbModV3Result(WOTBMOD_V3_CALL* watch)(
        WotbModV3Handle mod,
        WotbModV3ResourceHandle resource,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* get_total_memory_usage)(
        WotbModV3Handle mod,
        uint64_t* out_bytes);
} WotbModV3ResourcesApiV1;

#ifdef __cplusplus
}
#endif
