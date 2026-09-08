#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION 1u

typedef enum WotbModV3ReplayCamera {
    WOTBMOD_V3_REPLAY_CAMERA_FREE = 0,
    WOTBMOD_V3_REPLAY_CAMERA_FOLLOW_VEHICLE = 1,
    WOTBMOD_V3_REPLAY_CAMERA_TOP_DOWN = 2,
    WOTBMOD_V3_REPLAY_CAMERA_CINEMATIC = 3,
    WOTBMOD_V3_REPLAY_CAMERA_FIRST_PERSON = 4
} WotbModV3ReplayCamera;

typedef struct WotbModV3ReplayMarker {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t id;
    float timestamp_seconds;
    char label[WOTBMOD_V3_MAX_NAME];
} WotbModV3ReplayMarker;

typedef struct WotbModV3GameplayReplayApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* set_speed)(
        WotbModV3Handle mod,
        float multiplier);
    WotbModV3Result(WOTBMOD_V3_CALL* seek)(
        WotbModV3Handle mod,
        float timestamp_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* get_duration)(
        WotbModV3Handle mod,
        float* out_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* get_position)(
        WotbModV3Handle mod,
        float* out_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* add_marker)(
        WotbModV3Handle mod,
        float timestamp_seconds,
        const char* label,
        uint32_t* out_marker_id);
    WotbModV3Result(WOTBMOD_V3_CALL* remove_marker)(
        WotbModV3Handle mod,
        uint32_t marker_id);
    WotbModV3Result(WOTBMOD_V3_CALL* set_camera_mode)(
        WotbModV3Handle mod,
        uint32_t mode);
    WotbModV3Result(WOTBMOD_V3_CALL* set_follow_vehicle)(
        WotbModV3Handle mod,
        uint32_t public_entity_id);
    WotbModV3Result(WOTBMOD_V3_CALL* export_clip)(
        WotbModV3Handle mod,
        float start_seconds,
        float end_seconds,
        const char* output_uri);
} WotbModV3GameplayReplayApiV1;

#ifdef __cplusplus
}
#endif
