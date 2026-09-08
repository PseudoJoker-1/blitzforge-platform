#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_CLIENT_VERSION 1u

typedef enum WotbModV3ClientCompatibility {
    WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN = 0,
    WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED = 1,
    WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED = 2,
    WOTBMOD_V3_CLIENT_COMPATIBILITY_BINDINGS_MISSING = 3,
    WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH = 4
} WotbModV3ClientCompatibility;

typedef struct WotbModV3MissingBinding {
    uint32_t struct_size;
    uint32_t api_version;
    char capability[WOTBMOD_V3_MAX_CAPABILITY_NAME];
    char symbol[WOTBMOD_V3_MAX_NAME];
    char reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3MissingBinding;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3MissingBindingVisitor)(
    WotbModV3Handle mod,
    const WotbModV3MissingBinding* binding,
    void* user_data);

typedef struct WotbModV3ClientApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* is_supported)(
        WotbModV3Handle mod,
        uint32_t* out_supported);
    WotbModV3Result(WOTBMOD_V3_CALL* get_binding_pack_version)(
        WotbModV3Handle mod,
        uint32_t* out_version);
    WotbModV3Result(WOTBMOD_V3_CALL* get_compatibility_state)(
        WotbModV3Handle mod,
        uint32_t* out_state);
    WotbModV3Result(WOTBMOD_V3_CALL* enumerate_missing_bindings)(
        WotbModV3Handle mod,
        WotbModV3MissingBindingVisitor visitor,
        void* user_data);
    WotbModV3Result(WOTBMOD_V3_CALL* leave_to_hangar)(
        WotbModV3Handle mod);
} WotbModV3ClientApiV1;

#ifdef __cplusplus
}
#endif
