#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_DEVTOOLS_VERSION 1u

typedef struct WotbModV3ProfilerSpanInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t start_ticks;
    uint64_t elapsed_ticks;
    double elapsed_milliseconds;
    char category[WOTBMOD_V3_MAX_NAME];
    char name[WOTBMOD_V3_MAX_NAME];
} WotbModV3ProfilerSpanInfo;

typedef struct WotbModV3DevtoolsApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* marker)(
        WotbModV3Handle mod,
        const char* category,
        const char* name,
        const char* context_json);
    WotbModV3Result(WOTBMOD_V3_CALL* span_begin)(
        WotbModV3Handle mod,
        const char* category,
        const char* name,
        WotbModV3Handle* out_span);
    WotbModV3Result(WOTBMOD_V3_CALL* span_end)(
        WotbModV3Handle mod,
        WotbModV3Handle span,
        WotbModV3ProfilerSpanInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* counter_set)(
        WotbModV3Handle mod,
        const char* category,
        const char* name,
        double value);
} WotbModV3DevtoolsApiV1;

#ifdef __cplusplus
}
#endif
