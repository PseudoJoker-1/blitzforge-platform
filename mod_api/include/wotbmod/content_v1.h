#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_CONTENT_VERSION 1u

typedef enum WotbModV3ContentOverrideKind {
    WOTBMOD_V3_CONTENT_AUDIO = 1,
    WOTBMOD_V3_CONTENT_TEXTURE = 2,
    WOTBMOD_V3_CONTENT_UI = 3,
    WOTBMOD_V3_CONTENT_HANGAR = 4,
    WOTBMOD_V3_CONTENT_MODEL = 5,
    WOTBMOD_V3_CONTENT_LOCALIZATION = 6
} WotbModV3ContentOverrideKind;

typedef struct WotbModV3ContentInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t override_count;
    uint32_t reserved;
    char id[WOTBMOD_V3_MAX_ID];
    char name[WOTBMOD_V3_MAX_NAME];
    char version[WOTBMOD_V3_MAX_VERSION];
} WotbModV3ContentInfo;

typedef struct WotbModV3ContentOverride {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t kind;
    int32_t priority;
    char target[WOTBMOD_V3_MAX_PATH];
    char asset_path[WOTBMOD_V3_MAX_PATH];
} WotbModV3ContentOverride;

typedef struct WotbModV3ContentApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* parse_json)(
        WotbModV3Handle mod,
        const WotbModV3ConstBuffer* json_utf8,
        WotbModV3Handle* out_content);
    WotbModV3Result(WOTBMOD_V3_CALL* parse_uri)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3Handle* out_content);
    WotbModV3Result(WOTBMOD_V3_CALL* validate)(
        WotbModV3Handle mod,
        WotbModV3Handle content);
    WotbModV3Result(WOTBMOD_V3_CALL* get_info)(
        WotbModV3Handle mod,
        WotbModV3Handle content,
        WotbModV3ContentInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* get_override)(
        WotbModV3Handle mod,
        WotbModV3Handle content,
        uint32_t index,
        WotbModV3ContentOverride* out_override);
    WotbModV3Result(WOTBMOD_V3_CALL* apply)(
        WotbModV3Handle mod,
        WotbModV3Handle content);
    WotbModV3Result(WOTBMOD_V3_CALL* unapply)(
        WotbModV3Handle mod,
        WotbModV3Handle content);
} WotbModV3ContentApiV1;

#ifdef __cplusplus
}
#endif
