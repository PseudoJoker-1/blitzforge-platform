#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_CAMERA_VERSION 1u

typedef enum WotbModV3CameraMode {
    WOTBMOD_V3_CAMERA_MODE_UNKNOWN = 0,
    WOTBMOD_V3_CAMERA_MODE_HANGAR = 1,
    WOTBMOD_V3_CAMERA_MODE_ARCADE = 2,
    WOTBMOD_V3_CAMERA_MODE_SNIPER = 3,
    WOTBMOD_V3_CAMERA_MODE_POSTMORTEM = 4,
    WOTBMOD_V3_CAMERA_MODE_REPLAY = 5,
    WOTBMOD_V3_CAMERA_MODE_FREE = 6,
    WOTBMOD_V3_CAMERA_MODE_CINEMATIC = 7
} WotbModV3CameraMode;

typedef enum WotbModV3CameraModifierPhase {
    WOTBMOD_V3_CAMERA_MODIFIER_BEFORE_GAME = 0,
    WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME = 1
} WotbModV3CameraModifierPhase;

typedef struct WotbModV3CameraState {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Transform transform;
    float fov_degrees;
    float near_plane;
    float far_plane;
    uint32_t mode;
    uint32_t reserved;
} WotbModV3CameraState;

typedef struct WotbModV3CameraTransition {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3CameraState target;
    float duration_seconds;
    uint32_t easing;
    uint32_t preserve_game_control;
} WotbModV3CameraTransition;

typedef struct WotbModV3CameraShake {
    uint32_t struct_size;
    uint32_t api_version;
    float amplitude;
    float frequency;
    float duration_seconds;
    float falloff;
} WotbModV3CameraShake;

typedef void(WOTBMOD_V3_CALL* WotbModV3CameraModifierCallback)(
    WotbModV3Handle mod,
    WotbModV3CameraState* inout_state,
    void* user_data);

typedef struct WotbModV3CameraApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_active)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle* out_camera);
    WotbModV3Result(WOTBMOD_V3_CALL* get_mode)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        uint32_t* out_mode);
    WotbModV3Result(WOTBMOD_V3_CALL* get_transform)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        WotbModV3Transform* out_transform);
    WotbModV3Result(WOTBMOD_V3_CALL* set_transform)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        const WotbModV3Transform* transform);
    WotbModV3Result(WOTBMOD_V3_CALL* get_fov)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        float* out_degrees);
    WotbModV3Result(WOTBMOD_V3_CALL* set_fov)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        float degrees);
    WotbModV3Result(WOTBMOD_V3_CALL* get_near_plane)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        float* out_near_plane);
    WotbModV3Result(WOTBMOD_V3_CALL* get_far_plane)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        float* out_far_plane);
    WotbModV3Result(WOTBMOD_V3_CALL* world_to_screen)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        const WotbModV3Vec3* world,
        WotbModV3Vec3* out_screen);
    WotbModV3Result(WOTBMOD_V3_CALL* screen_to_world)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        const WotbModV3Vec3* screen,
        WotbModV3Vec3* out_world);
    WotbModV3Result(WOTBMOD_V3_CALL* add_modifier)(
        WotbModV3Handle mod,
        uint32_t phase,
        int32_t priority,
        WotbModV3CameraModifierCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* remove_modifier)(
        WotbModV3Handle mod,
        WotbModV3Token token);
    WotbModV3Result(WOTBMOD_V3_CALL* transition_to)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        const WotbModV3CameraTransition* transition);
    WotbModV3Result(WOTBMOD_V3_CALL* add_shake)(
        WotbModV3Handle mod,
        WotbModV3CameraHandle camera,
        const WotbModV3CameraShake* shake);
} WotbModV3CameraApiV1;

#ifdef __cplusplus
}
#endif
