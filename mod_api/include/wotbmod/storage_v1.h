#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_STORAGE_VERSION 1u
#define WOTBMOD_V3_STORAGE_KEY_MAX 128u

typedef enum WotbModV3StoragePathKind {
    WOTBMOD_V3_STORAGE_PATH_DATA = 1,
    WOTBMOD_V3_STORAGE_PATH_CONFIG = 2,
    WOTBMOD_V3_STORAGE_PATH_CACHE = 3,
    WOTBMOD_V3_STORAGE_PATH_TEMP = 4
} WotbModV3StoragePathKind;

typedef struct WotbModV3StorageApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_json)(
        WotbModV3Handle mod,
        const char* key,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* set_json)(
        WotbModV3Handle mod,
        const char* key,
        const char* json_utf8);
    WotbModV3Result(WOTBMOD_V3_CALL* get_bytes)(
        WotbModV3Handle mod,
        const char* key,
        WotbModV3Buffer* inout_buffer);
    WotbModV3Result(WOTBMOD_V3_CALL* set_bytes)(
        WotbModV3Handle mod,
        const char* key,
        const WotbModV3ConstBuffer* value);
    WotbModV3Result(WOTBMOD_V3_CALL* erase)(
        WotbModV3Handle mod,
        const char* key);
    WotbModV3Result(WOTBMOD_V3_CALL* contains)(
        WotbModV3Handle mod,
        const char* key,
        uint32_t* out_contains);
    WotbModV3Result(WOTBMOD_V3_CALL* flush)(WotbModV3Handle mod);
    WotbModV3Result(WOTBMOD_V3_CALL* begin_transaction)(
        WotbModV3Handle mod,
        WotbModV3Token* out_transaction);
    WotbModV3Result(WOTBMOD_V3_CALL* transaction_set_json)(
        WotbModV3Handle mod,
        WotbModV3Token transaction,
        const char* key,
        const char* json_utf8);
    WotbModV3Result(WOTBMOD_V3_CALL* transaction_set_bytes)(
        WotbModV3Handle mod,
        WotbModV3Token transaction,
        const char* key,
        const WotbModV3ConstBuffer* value);
    WotbModV3Result(WOTBMOD_V3_CALL* transaction_erase)(
        WotbModV3Handle mod,
        WotbModV3Token transaction,
        const char* key);
    WotbModV3Result(WOTBMOD_V3_CALL* commit)(
        WotbModV3Handle mod,
        WotbModV3Token transaction);
    WotbModV3Result(WOTBMOD_V3_CALL* rollback)(
        WotbModV3Handle mod,
        WotbModV3Token transaction);
    WotbModV3Result(WOTBMOD_V3_CALL* get_path)(
        WotbModV3Handle mod,
        uint32_t path_kind,
        char* buffer,
        uint32_t* inout_size);
} WotbModV3StorageApiV1;

#ifdef __cplusplus
}
#endif
