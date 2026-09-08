#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION 1u

typedef enum WotbModV3HangarLighting {
    WOTBMOD_V3_HANGAR_LIGHTING_GAME_DEFAULT = 0,
    WOTBMOD_V3_HANGAR_LIGHTING_DAY = 1,
    WOTBMOD_V3_HANGAR_LIGHTING_NIGHT = 2,
    WOTBMOD_V3_HANGAR_LIGHTING_STUDIO = 3,
    WOTBMOD_V3_HANGAR_LIGHTING_CINEMATIC = 4
} WotbModV3HangarLighting;

typedef enum WotbModV3HangarUiElement {
    WOTBMOD_V3_HANGAR_UI_BANNERS = 1u << 0,
    WOTBMOD_V3_HANGAR_UI_NEWS = 1u << 1,
    WOTBMOD_V3_HANGAR_UI_OFFERS = 1u << 2,
    WOTBMOD_V3_HANGAR_UI_CHAT = 1u << 3
} WotbModV3HangarUiElement;

typedef struct WotbModV3GameplayHangarApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* set_background)(
        WotbModV3Handle mod,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_background_video)(
        WotbModV3Handle mod,
        const char* video_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_music)(
        WotbModV3Handle mod,
        const char* audio_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_lighting)(
        WotbModV3Handle mod,
        uint32_t preset);
    WotbModV3Result(WOTBMOD_V3_CALL* set_vehicle_preview_angle)(
        WotbModV3Handle mod,
        float yaw,
        float pitch);
    WotbModV3Result(WOTBMOD_V3_CALL* set_vehicle_preview_zoom)(
        WotbModV3Handle mod,
        float zoom);
    WotbModV3Result(WOTBMOD_V3_CALL* set_floor_texture)(
        WotbModV3Handle mod,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_skybox)(
        WotbModV3Handle mod,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* hide_ui_elements)(
        WotbModV3Handle mod,
        uint32_t mask);
    WotbModV3Result(WOTBMOD_V3_CALL* set_camera_orbit_speed)(
        WotbModV3Handle mod,
        float multiplier);
    WotbModV3Result(WOTBMOD_V3_CALL* reset)(
        WotbModV3Handle mod);
} WotbModV3GameplayHangarApiV1;

#ifdef __cplusplus
}
#endif
