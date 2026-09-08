#pragma once

#include "diagnostics_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_DIAGNOSTICS_VERSION_2 2u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_ENTRIES 32u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMBS 64u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_KEY 64u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_VALUE 512u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_ACTION 512u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CATEGORY 64u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_MESSAGE 512u
#define WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CONTEXT 1024u

typedef enum WotbModV3DiagnosticModState {
    WOTBMOD_V3_DIAGNOSTIC_MOD_CREATED = 0,
    WOTBMOD_V3_DIAGNOSTIC_MOD_LOADED = 1,
    WOTBMOD_V3_DIAGNOSTIC_MOD_ENABLED = 2,
    WOTBMOD_V3_DIAGNOSTIC_MOD_DISABLED = 3,
    WOTBMOD_V3_DIAGNOSTIC_MOD_UNLOADING = 4,
    WOTBMOD_V3_DIAGNOSTIC_MOD_FAULTED = 5
} WotbModV3DiagnosticModState;

/*
 * This structure intentionally exposes only values maintained by the runtime.
 * It does not estimate native heap use, FPS, crash probability, or stack state.
 */
typedef struct WotbModV3ModHealth {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t runtime_state;
    uint32_t enabled;
    uint32_t permission_tier;
    uint32_t reported_faults;
    uint32_t crash_context_entries;
    uint32_t retained_breadcrumbs;
    uint32_t active_profiler_spans;
    uint32_t reserved;
    uint64_t completed_profiler_spans;
    uint64_t cpu_measured_profiler_spans;
    uint64_t profiler_cpu_100ns;
    double profiler_cpu_milliseconds;
    uint64_t diagnostic_owned_bytes;
    uint64_t peak_diagnostic_owned_bytes;
    char last_action[WOTBMOD_V3_DIAGNOSTICS_MAX_ACTION];
} WotbModV3ModHealth;

/*
 * V2 preserves the complete WotbModV3DiagnosticsApiV1 prefix. A V1 consumer
 * may query minimum version 1 and use the returned table as V1.
 */
typedef struct WotbModV3DiagnosticsApiV2 {
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

    WotbModV3Result(WOTBMOD_V3_CALL* crash_add_context)(
        WotbModV3Handle mod,
        const char* key,
        const char* value);
    WotbModV3Result(WOTBMOD_V3_CALL* crash_set_last_action)(
        WotbModV3Handle mod,
        const char* action);
    WotbModV3Result(WOTBMOD_V3_CALL* crash_add_breadcrumb)(
        WotbModV3Handle mod,
        const char* category,
        const char* message,
        const char* context_json);
    /*
     * bundle_name is a single portable filename ending in ".json".
     * The runtime writes it below <mod-data>\diagnostics and returns the full
     * path. Directory components and traversal are rejected.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* export_bundle)(
        WotbModV3Handle mod,
        const char* bundle_name,
        char* out_path,
        uint32_t* inout_path_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_mod_health)(
        WotbModV3Handle mod,
        WotbModV3ModHealth* out_health);
} WotbModV3DiagnosticsApiV2;

#ifdef __cplusplus
}
#endif
