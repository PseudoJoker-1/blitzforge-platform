#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_CAPABILITIES_VERSION 1u

typedef void(WOTBMOD_V3_CALL* WotbModV3CapabilityChangedCallback)(
    WotbModV3Handle mod,
    const WotbModV3CapabilityInfo* capability,
    void* user_data);

typedef struct WotbModV3CapabilitiesApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_count)(
        WotbModV3Handle mod,
        uint32_t* out_count);
    WotbModV3Result(WOTBMOD_V3_CALL* get_at)(
        WotbModV3Handle mod,
        uint32_t index,
        WotbModV3CapabilityInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* query)(
        WotbModV3Handle mod,
        const char* capability_name,
        WotbModV3CapabilityInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* subscribe)(
        WotbModV3Handle mod,
        WotbModV3CapabilityChangedCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unsubscribe)(
        WotbModV3Handle mod,
        WotbModV3Token token);
} WotbModV3CapabilitiesApiV1;

#ifdef __cplusplus
}
#endif
