#pragma once

#include "devtools_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_DEVTOOLS_VERSION_2 2u

typedef struct WotbModV3ProfilerAggregate {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t active_spans;
    uint32_t reserved;
    uint64_t completed_spans;
    uint64_t cpu_measured_spans;
    uint64_t total_cpu_100ns;
    double total_cpu_milliseconds;
} WotbModV3ProfilerAggregate;

/*
 * Bytes are persistent allocations owned and tracked by diagnostics/devtools:
 * retained context, breadcrumbs, profiler aggregates, and live span objects.
 * This is not an estimate of the mod DLL's native heap.
 */
typedef struct WotbModV3ProfilerMemory {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t retained_diagnostics_bytes;
    uint64_t active_span_bytes;
    uint64_t total_owned_bytes;
    uint64_t peak_owned_bytes;
} WotbModV3ProfilerMemory;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3DevtoolsInspectFn)(
    WotbModV3Handle mod,
    const char* selector,
    char* buffer,
    uint32_t* inout_size);

/*
 * V2 preserves the complete WotbModV3DevtoolsApiV1 prefix. Inspector entries
 * truthfully return E_NOT_SUPPORTED until a native enumeration backend exists.
 */
typedef struct WotbModV3DevtoolsApiV2 {
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
} WotbModV3DevtoolsApiV2;

#ifdef __cplusplus
}
#endif
