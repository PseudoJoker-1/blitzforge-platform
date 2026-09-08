#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_INTERMOD_VERSION 1u
#define WOTBMOD_V3_MAX_SERVICE_ID 192u
#define WOTBMOD_V3_MAX_MESSAGE_TOPIC 192u
#define WOTBMOD_V3_MAX_INTERMOD_PAYLOAD (1024u * 1024u)

typedef struct WotbModV3ExportedInterfaceInfo {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Token export_token;
    WotbModV3Handle provider_mod;
    uint32_t interface_version;
    uint32_t table_size;
    char service_id[WOTBMOD_V3_MAX_SERVICE_ID];
} WotbModV3ExportedInterfaceInfo;

typedef struct WotbModV3ImportedInterface {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle provider_mod;
    uint32_t interface_version;
    uint32_t table_size;
    const void* table;
    char service_id[WOTBMOD_V3_MAX_SERVICE_ID];
} WotbModV3ImportedInterface;

typedef struct WotbModV3IntermodMessage {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle publisher_mod;
    uint64_t timestamp_ns;
    char topic[WOTBMOD_V3_MAX_MESSAGE_TOPIC];
    const void* payload;
    uint32_t payload_size;
    uint32_t reserved;
} WotbModV3IntermodMessage;

typedef void(WOTBMOD_V3_CALL* WotbModV3IntermodMessageCallback)(
    WotbModV3Handle mod,
    const WotbModV3IntermodMessage* message,
    void* user_data);

typedef struct WotbModV3IntermodApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* mod_find)(
        WotbModV3Handle mod,
        const char* mod_id,
        WotbModV3Handle* out_mod);
    WotbModV3Result(WOTBMOD_V3_CALL* mod_is_loaded)(
        WotbModV3Handle mod,
        const char* mod_id,
        uint32_t* out_loaded);
    WotbModV3Result(WOTBMOD_V3_CALL* mod_get_version)(
        WotbModV3Handle mod,
        WotbModV3Handle target_mod,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* mod_get_dependency)(
        WotbModV3Handle mod,
        const char* dependency_id,
        WotbModV3Handle* out_dependency,
        uint32_t* out_optional);
    WotbModV3Result(WOTBMOD_V3_CALL* export_interface)(
        WotbModV3Handle mod,
        const char* service_id,
        uint32_t interface_version,
        const void* table,
        uint32_t table_size,
        WotbModV3Token* out_export_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unexport_interface)(
        WotbModV3Handle mod,
        WotbModV3Token export_token);
    WotbModV3Result(WOTBMOD_V3_CALL* import_interface)(
        WotbModV3Handle mod,
        const char* service_id,
        uint32_t minimum_version,
        WotbModV3ImportedInterface* out_interface);
    WotbModV3Result(WOTBMOD_V3_CALL* enumerate_interfaces)(
        WotbModV3Handle mod,
        WotbModV3ExportedInterfaceInfo* interfaces,
        uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* message_publish)(
        WotbModV3Handle mod,
        const char* topic,
        const void* payload,
        uint32_t payload_size);
    WotbModV3Result(WOTBMOD_V3_CALL* message_subscribe)(
        WotbModV3Handle mod,
        const char* topic_pattern,
        int32_t priority,
        WotbModV3IntermodMessageCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* message_unsubscribe)(
        WotbModV3Handle mod,
        WotbModV3Token token);
} WotbModV3IntermodApiV1;

#ifdef __cplusplus
}
#endif
