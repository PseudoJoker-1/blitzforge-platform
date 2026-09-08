#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_HANDLES_VERSION 1u

typedef struct WotbModV3HandlesApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* retain)(
        WotbModV3Handle mod,
        WotbModV3Handle handle);
    WotbModV3Result(WOTBMOD_V3_CALL* release)(
        WotbModV3Handle mod,
        WotbModV3Handle handle);
    WotbModV3Result(WOTBMOD_V3_CALL* get_info)(
        WotbModV3Handle mod,
        WotbModV3Handle handle,
        WotbModV3HandleInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* is_alive)(
        WotbModV3Handle mod,
        WotbModV3Handle handle,
        uint32_t* out_alive);
} WotbModV3HandlesApiV1;

#ifdef __cplusplus
}
#endif
