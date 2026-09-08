#pragma once

#include "devtools_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_DEVTOOLS_VERSION_3 3u

typedef struct WotbModV3CallbackProfile {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t budget_microseconds;
    uint32_t max_callbacks_per_frame;
    uint64_t frame_index;
    uint64_t frame_callbacks;
    uint64_t frame_cpu_100ns;
    uint64_t previous_frame_callbacks;
    uint64_t previous_frame_cpu_100ns;
    uint64_t total_callbacks;
    uint64_t total_cpu_100ns;
    uint64_t max_callback_100ns;
    uint64_t slow_callbacks;
    uint64_t coalesced_callbacks;
    uint64_t over_budget_frames;
} WotbModV3CallbackProfile;

/* V3 preserves the complete WotbModV3DevtoolsApiV2 prefix. The callback
 * profile measures runtime-dispatched callbacks, independently of explicit
 * profiler spans created by the mod. */
typedef struct WotbModV3DevtoolsApiV3 {
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
    WotbModV3DevtoolsInspectFn inspect_ui;
    WotbModV3DevtoolsInspectFn inspect_scene;
    WotbModV3DevtoolsInspectFn inspect_material;
    WotbModV3DevtoolsInspectFn inspect_resource;
    WotbModV3DevtoolsInspectFn inspect_hook_chain;
    WotbModV3DevtoolsInspectFn inspect_events;
    WotbModV3Result(WOTBMOD_V3_CALL* profiler_get_mod_cpu_time)(
        WotbModV3Handle mod,
        WotbModV3ProfilerAggregate* out_aggregate);
    WotbModV3Result(WOTBMOD_V3_CALL* profiler_get_mod_memory)(
        WotbModV3Handle mod,
        WotbModV3ProfilerMemory* out_memory);
    WotbModV3Result(WOTBMOD_V3_CALL* get_callback_profile)(
        WotbModV3Handle mod,
        WotbModV3CallbackProfile* out_profile);
    WotbModV3Result(WOTBMOD_V3_CALL* set_callback_budget)(
        WotbModV3Handle mod,
        uint32_t budget_microseconds,
        uint32_t max_callbacks_per_frame);
    WotbModV3Result(WOTBMOD_V3_CALL* reset_callback_profile)(
        WotbModV3Handle mod);
} WotbModV3DevtoolsApiV3;

#ifdef __cplusplus
}
#endif
