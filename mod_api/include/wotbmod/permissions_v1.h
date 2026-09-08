#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_PERMISSIONS_VERSION 1u

typedef enum WotbModV3PermissionState {
    WOTBMOD_V3_PERMISSION_STATE_DENIED = 0,
    WOTBMOD_V3_PERMISSION_STATE_GRANTED = 1,
    WOTBMOD_V3_PERMISSION_STATE_REVIEW_REQUIRED = 2,
    WOTBMOD_V3_PERMISSION_STATE_UNAVAILABLE = 3
} WotbModV3PermissionState;

typedef struct WotbModV3PermissionInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t state;
    uint32_t tier;
    char name[WOTBMOD_V3_MAX_PERMISSION_NAME];
    char reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3PermissionInfo;

typedef struct WotbModV3PermissionsApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_granted_tier)(
        WotbModV3Handle mod,
        uint32_t* out_tier);
    WotbModV3Result(WOTBMOD_V3_CALL* query)(
        WotbModV3Handle mod,
        const char* permission_name,
        WotbModV3PermissionInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* get_count)(
        WotbModV3Handle mod,
        uint32_t* out_count);
    WotbModV3Result(WOTBMOD_V3_CALL* get_at)(
        WotbModV3Handle mod,
        uint32_t index,
        WotbModV3PermissionInfo* out_info);
} WotbModV3PermissionsApiV1;

#ifdef __cplusplus
}
#endif
