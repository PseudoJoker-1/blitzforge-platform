#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_CORE_VERSION 1u

typedef enum WotbModV3LogLevel {
    WOTBMOD_V3_LOG_TRACE = 0,
    WOTBMOD_V3_LOG_DEBUG = 1,
    WOTBMOD_V3_LOG_INFO = 2,
    WOTBMOD_V3_LOG_WARNING = 3,
    WOTBMOD_V3_LOG_ERROR = 4,
    WOTBMOD_V3_LOG_FATAL = 5
} WotbModV3LogLevel;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3CoreCopyPathFn)(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size);

typedef struct WotbModV3CoreApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* log)(
        WotbModV3Handle mod,
        uint32_t level,
        const char* category,
        const char* message);
    WotbModV3Result(WOTBMOD_V3_CALL* get_context)(
        WotbModV3Handle mod,
        uint64_t* out_context_mask);
    WotbModV3Result(WOTBMOD_V3_CALL* get_thread_role)(
        WotbModV3Handle mod,
        uint32_t* out_thread_role);
    WotbModV3Result(WOTBMOD_V3_CALL* get_frame_index)(
        WotbModV3Handle mod,
        uint64_t* out_frame_index);
    WotbModV3CoreCopyPathFn get_game_directory;
    WotbModV3CoreCopyPathFn get_mod_data_directory;
    WotbModV3CoreCopyPathFn get_mod_cache_directory;
    WotbModV3CoreCopyPathFn get_mod_config_directory;
} WotbModV3CoreApiV1;

#ifdef __cplusplus
}
#endif
