#pragma once

#include "base.h"
#include "hooks_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_UNSAFE_NATIVE_VERSION 1u

typedef struct WotbModV3UnsafeNativeApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* create_address_hook)(
        WotbModV3Handle mod,
        void* target,
        const WotbModV3HookCreateInfo* info,
        WotbModV3HookHandle* out_hook);
} WotbModV3UnsafeNativeApiV1;

#ifdef __cplusplus
}
#endif
