#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION 1u

typedef enum WotbModV3CameraTransitionMode {
    WOTBMOD_V3_CAMERA_TRANSITION_SMOOTH = 0,
    WOTBMOD_V3_CAMERA_TRANSITION_INSTANT = 1,
    WOTBMOD_V3_CAMERA_TRANSITION_CUSTOM = 2
} WotbModV3CameraTransitionMode;

typedef enum WotbModV3Easing {
    WOTBMOD_V3_EASING_LINEAR = 0,
    WOTBMOD_V3_EASING_IN_QUAD = 1,
    WOTBMOD_V3_EASING_OUT_QUAD = 2,
    WOTBMOD_V3_EASING_IN_OUT_QUAD = 3,
    WOTBMOD_V3_EASING_IN_CUBIC = 4,
    WOTBMOD_V3_EASING_OUT_CUBIC = 5,
    WOTBMOD_V3_EASING_IN_OUT_CUBIC = 6
} WotbModV3Easing;

typedef struct WotbModV3GameplayCameraConfig {
    uint32_t struct_size;
    uint32_t api_version;
    float fov_hangar;
    float fov_battle;
    float fov_sniper;
    float zoom_multiplier;
    uint32_t transition_mode;
    float transition_duration_ms;
    uint32_t transition_easing;
    uint32_t shake_enabled;
    float shake_intensity;
    uint32_t postprocessing_enabled;
    uint32_t bloom_enabled;
    uint32_t motion_blur_enabled;
    uint32_t vignette_enabled;
    uint32_t dof_enabled;
} WotbModV3GameplayCameraConfig;

typedef struct WotbModV3GameplayCameraApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* set_fov)(
        WotbModV3Handle mod,
        float degrees);
    WotbModV3Result(WOTBMOD_V3_CALL* get_fov)(
        WotbModV3Handle mod,
        float* out_degrees);
    WotbModV3Result(WOTBMOD_V3_CALL* set_fov_hangar)(
        WotbModV3Handle mod,
        float degrees);
    WotbModV3Result(WOTBMOD_V3_CALL* set_fov_battle)(
        WotbModV3Handle mod,
        float degrees);
    WotbModV3Result(WOTBMOD_V3_CALL* set_fov_sniper)(
        WotbModV3Handle mod,
        float degrees);
    WotbModV3Result(WOTBMOD_V3_CALL* reset_fov)(
        WotbModV3Handle mod);
    WotbModV3Result(WOTBMOD_V3_CALL* set_zoom_steps)(
        WotbModV3Handle mod,
        const float* steps,
        uint32_t count);
    WotbModV3Result(WOTBMOD_V3_CALL* get_zoom_steps)(
        WotbModV3Handle mod,
        float* out_steps,
        uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* set_zoom_multiplier)(
        WotbModV3Handle mod,
        float multiplier);
    WotbModV3Result(WOTBMOD_V3_CALL* set_max_zoom)(
        WotbModV3Handle mod,
        float multiplier);
    WotbModV3Result(WOTBMOD_V3_CALL* set_transition_mode)(
        WotbModV3Handle mod,
        uint32_t mode);
    WotbModV3Result(WOTBMOD_V3_CALL* set_transition_duration)(
        WotbModV3Handle mod,
        float milliseconds);
    WotbModV3Result(WOTBMOD_V3_CALL* set_transition_easing)(
        WotbModV3Handle mod,
        uint32_t easing);
    WotbModV3Result(WOTBMOD_V3_CALL* set_shake_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* set_shake_intensity)(
        WotbModV3Handle mod,
        float multiplier);
    WotbModV3Result(WOTBMOD_V3_CALL* set_postprocessing_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* set_bloom_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* set_motion_blur_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* set_vignette_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* set_color_grading)(
        WotbModV3Handle mod,
        const char* lut_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_dof_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* set_dof_params)(
        WotbModV3Handle mod,
        float focus,
        float aperture,
        float max_blur);
    WotbModV3Result(WOTBMOD_V3_CALL* free_enable)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* free_set_speed)(
        WotbModV3Handle mod,
        float units_per_second);
    WotbModV3Result(WOTBMOD_V3_CALL* free_set_position)(
        WotbModV3Handle mod,
        WotbModV3Vec3 position);
    WotbModV3Result(WOTBMOD_V3_CALL* free_set_rotation)(
        WotbModV3Handle mod,
        WotbModV3Vec3 pitch_yaw_roll);
    WotbModV3Result(WOTBMOD_V3_CALL* free_get_position)(
        WotbModV3Handle mod,
        WotbModV3Vec3* out_position);
    WotbModV3Result(WOTBMOD_V3_CALL* free_get_rotation)(
        WotbModV3Handle mod,
        WotbModV3Vec3* out_pitch_yaw_roll);
    WotbModV3Result(WOTBMOD_V3_CALL* get_config)(
        WotbModV3Handle mod,
        WotbModV3GameplayCameraConfig* out_config);
    WotbModV3Result(WOTBMOD_V3_CALL* apply_config)(
        WotbModV3Handle mod,
        const WotbModV3GameplayCameraConfig* config);
} WotbModV3GameplayCameraApiV1;

#ifdef __cplusplus
}
#endif
