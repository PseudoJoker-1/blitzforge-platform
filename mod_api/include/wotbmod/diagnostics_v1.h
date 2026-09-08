#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_DIAGNOSTICS_VERSION 1u

typedef struct WotbModV3DiagnosticsStats {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t loaded_mods;
    uint32_t enabled_mods;
    uint32_t live_handles;
    uint32_t registered_interfaces;
    uint32_t registered_capabilities;
    uint32_t reserved;
    uint64_t frame_index;
    uint64_t context_mask;
} WotbModV3DiagnosticsStats;

typedef struct WotbModV3DiagnosticsApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_stats)(
        WotbModV3Handle mod,
        WotbModV3DiagnosticsStats* out_stats);
    WotbModV3Result(WOTBMOD_V3_CALL* copy_report_json)(
        WotbModV3Handle mod,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* report_fault)(
        WotbModV3Handle mod,
        const char* category,
        const char* message,
        const char* context_json);
} WotbModV3DiagnosticsApiV1;

#ifdef __cplusplus
}
#endif
