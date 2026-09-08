#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_ENTRY_NAME "WotbModLoadV3"

struct WotbModV3Bootstrap;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3QueryInterfaceFn)(
    WotbModV3Handle mod,
    const char* interface_name,
    uint32_t minimum_version,
    const void** out_interface);

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3GetInterfaceInfoFn)(
    WotbModV3Handle mod,
    const char* interface_name,
    WotbModV3InterfaceInfo* out_info);

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3GetLastErrorFn)(
    WotbModV3Handle mod,
    WotbModV3ErrorInfo* out_error);

typedef struct WotbModV3ClientInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t supported;
    uint32_t compatibility_state;
    uint32_t binding_pack_version;
    uint32_t process_architecture;
    char client_version[WOTBMOD_V3_MAX_VERSION];
    char executable_sha256[65];
    char missing_bindings[WOTBMOD_V3_MAX_CONTEXT_JSON];
} WotbModV3ClientInfo;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3GetClientInfoFn)(
    WotbModV3Handle mod,
    WotbModV3ClientInfo* out_info);

typedef struct WotbModV3Bootstrap {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t sdk_version;
    uint32_t bootstrap_version;
    WotbModV3QueryInterfaceFn query_interface;
    WotbModV3GetInterfaceInfoFn get_interface_info;
    WotbModV3GetLastErrorFn get_last_error;
    WotbModV3GetClientInfoFn get_client_info;
} WotbModV3Bootstrap;

typedef void(WOTBMOD_V3_CALL* WotbModV3LifecycleCallback)(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod);

typedef void(WOTBMOD_V3_CALL* WotbModV3FrameCallback)(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    uint64_t frame_index,
    double delta_seconds);

typedef struct WotbModV3Info {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t flags;
    uint32_t requested_permission_tier;
    char id[WOTBMOD_V3_MAX_ID];
    char name[WOTBMOD_V3_MAX_NAME];
    char version[WOTBMOD_V3_MAX_VERSION];
    char author[WOTBMOD_V3_MAX_NAME];
    char description[WOTBMOD_V3_MAX_MESSAGE];
    WotbModV3LifecycleCallback on_enable;
    WotbModV3LifecycleCallback on_disable;
    WotbModV3LifecycleCallback on_unload;
    WotbModV3FrameCallback on_frame;
} WotbModV3Info;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModLoadV3Fn)(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    WotbModV3Info* out_info);

#define WOTBMOD_V3_DEFINE_ENTRY(function_name)                         \
    WOTBMOD_V3_EXPORT WotbModV3Result WOTBMOD_V3_CALL function_name(  \
        const WotbModV3Bootstrap* bootstrap,                          \
        WotbModV3Handle mod,                                          \
        WotbModV3Info* out_info)

#define WOTBMOD_V3_ENTRY WOTBMOD_V3_DEFINE_ENTRY(WotbModLoadV3)

#ifdef __cplusplus
}
#endif
