#pragma once

#include "base.h"
#include "yaml_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_LOADERS_VERSION 1u

typedef enum WotbModV3LoaderBackend {
    WOTBMOD_V3_LOADER_UTF8_TEXT = 1,
    WOTBMOD_V3_LOADER_BINARY = 2,
    WOTBMOD_V3_LOADER_MINIMAL_YAML = 3,
    WOTBMOD_V3_LOADER_DAVA_YAML = 4,
    WOTBMOD_V3_LOADER_DVPL = 5,
    WOTBMOD_V3_LOADER_DAVA_ARCHIVE = 6
} WotbModV3LoaderBackend;

typedef struct WotbModV3LoaderBackendInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t backend;
    uint32_t available;
    char name[WOTBMOD_V3_MAX_NAME];
    char unavailable_reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3LoaderBackendInfo;

typedef struct WotbModV3LoadersApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* load_text_utf8)(
        WotbModV3Handle mod,
        const char* uri,
        uint64_t max_bytes,
        WotbModV3Buffer* inout_buffer);
    WotbModV3Result(WOTBMOD_V3_CALL* load_binary)(
        WotbModV3Handle mod,
        const char* uri,
        uint64_t max_bytes,
        WotbModV3Buffer* inout_buffer);
    WotbModV3Result(WOTBMOD_V3_CALL* load_yaml)(
        WotbModV3Handle mod,
        const char* uri,
        const WotbModV3YamlLimits* limits,
        WotbModV3Handle* out_document);
    WotbModV3Result(WOTBMOD_V3_CALL* load_dava_yaml)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3Handle* out_document);
    WotbModV3Result(WOTBMOD_V3_CALL* unpack_dvpl)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3Buffer* inout_buffer);
    WotbModV3Result(WOTBMOD_V3_CALL* open_dava_archive)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3ArchiveHandle* out_archive);
    WotbModV3Result(WOTBMOD_V3_CALL* get_backend_info)(
        WotbModV3Handle mod,
        uint32_t backend,
        WotbModV3LoaderBackendInfo* out_info);
} WotbModV3LoadersApiV1;

#ifdef __cplusplus
}
#endif
