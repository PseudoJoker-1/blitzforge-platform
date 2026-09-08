#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_GAMEPLAY_HUD_VERSION 1u

typedef enum WotbModV3HudAnchor {
    WOTBMOD_V3_HUD_ANCHOR_TOP_LEFT = 0,
    WOTBMOD_V3_HUD_ANCHOR_TOP = 1,
    WOTBMOD_V3_HUD_ANCHOR_TOP_RIGHT = 2,
    WOTBMOD_V3_HUD_ANCHOR_LEFT = 3,
    WOTBMOD_V3_HUD_ANCHOR_CENTER = 4,
    WOTBMOD_V3_HUD_ANCHOR_RIGHT = 5,
    WOTBMOD_V3_HUD_ANCHOR_BOTTOM_LEFT = 6,
    WOTBMOD_V3_HUD_ANCHOR_BOTTOM = 7,
    WOTBMOD_V3_HUD_ANCHOR_BOTTOM_RIGHT = 8
} WotbModV3HudAnchor;

typedef enum WotbModV3HitIndicatorStyle {
    WOTBMOD_V3_HIT_STYLE_GAME_DEFAULT = 0,
    WOTBMOD_V3_HIT_STYLE_COMPACT = 1,
    WOTBMOD_V3_HIT_STYLE_DIRECTIONAL = 2,
    WOTBMOD_V3_HIT_STYLE_MINIMAL = 3
} WotbModV3HitIndicatorStyle;

typedef enum WotbModV3SessionStat {
    WOTBMOD_V3_STAT_DAMAGE = 1u << 0,
    WOTBMOD_V3_STAT_KILLS = 1u << 1,
    WOTBMOD_V3_STAT_WN8 = 1u << 2,
    WOTBMOD_V3_STAT_WINRATE = 1u << 3,
    WOTBMOD_V3_STAT_SHOTS = 1u << 4,
    WOTBMOD_V3_STAT_PENETRATION_RATE = 1u << 5
} WotbModV3SessionStat;

typedef struct WotbModV3GameplayHudApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* reticle_set_texture)(
        WotbModV3Handle mod,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* reticle_set_color)(
        WotbModV3Handle mod,
        uint32_t rgba);
    WotbModV3Result(WOTBMOD_V3_CALL* reticle_set_size)(
        WotbModV3Handle mod,
        float scale);
    WotbModV3Result(WOTBMOD_V3_CALL* reticle_set_sniper_texture)(
        WotbModV3Handle mod,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* reticle_set_reloading_indicator)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* reticle_set_dispersion_circle)(
        WotbModV3Handle mod,
        uint32_t enabled);

    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_position)(
        WotbModV3Handle mod,
        uint32_t anchor);
    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_max_entries)(
        WotbModV3Handle mod,
        uint32_t count);
    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_show_blocked)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_show_ricochet)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_show_module_damage)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_format)(
        WotbModV3Handle mod,
        const char* format);
    WotbModV3Result(WOTBMOD_V3_CALL* damagelog_set_filter_own)(
        WotbModV3Handle mod,
        uint32_t enabled);

    WotbModV3Result(WOTBMOD_V3_CALL* session_stats_set_enabled)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* session_stats_set_fields)(
        WotbModV3Handle mod,
        uint32_t fields);

    WotbModV3Result(WOTBMOD_V3_CALL* minimap_set_size)(
        WotbModV3Handle mod,
        float scale);
    WotbModV3Result(WOTBMOD_V3_CALL* minimap_set_opacity)(
        WotbModV3Handle mod,
        float opacity);
    WotbModV3Result(WOTBMOD_V3_CALL* minimap_set_show_last_known)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* minimap_set_show_artillery_range)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* minimap_set_show_drawing)(
        WotbModV3Handle mod,
        uint32_t enabled);
    WotbModV3Result(WOTBMOD_V3_CALL* minimap_add_marker)(
        WotbModV3Handle mod,
        float world_x,
        float world_z,
        const char* label,
        uint32_t color,
        uint32_t* out_marker_id);
    WotbModV3Result(WOTBMOD_V3_CALL* minimap_remove_marker)(
        WotbModV3Handle mod,
        uint32_t marker_id);

    WotbModV3Result(WOTBMOD_V3_CALL* sixth_sense_set_texture)(
        WotbModV3Handle mod,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* sixth_sense_set_sound)(
        WotbModV3Handle mod,
        const char* audio_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* sixth_sense_set_position)(
        WotbModV3Handle mod,
        WotbModV3Vec2 position);
    WotbModV3Result(WOTBMOD_V3_CALL* sixth_sense_set_scale)(
        WotbModV3Handle mod,
        float scale);
    WotbModV3Result(WOTBMOD_V3_CALL* sixth_sense_set_delay_ms)(
        WotbModV3Handle mod,
        float milliseconds);

    WotbModV3Result(WOTBMOD_V3_CALL* hit_indicator_set_style)(
        WotbModV3Handle mod,
        uint32_t style);
    WotbModV3Result(WOTBMOD_V3_CALL* hit_indicator_set_color_hit)(
        WotbModV3Handle mod,
        uint32_t rgba);
    WotbModV3Result(WOTBMOD_V3_CALL* hit_indicator_set_color_pen)(
        WotbModV3Handle mod,
        uint32_t rgba);
    WotbModV3Result(WOTBMOD_V3_CALL* hit_indicator_set_color_ricochet)(
        WotbModV3Handle mod,
        uint32_t rgba);
    WotbModV3Result(WOTBMOD_V3_CALL* hit_indicator_set_color_crit)(
        WotbModV3Handle mod,
        uint32_t rgba);
    WotbModV3Result(WOTBMOD_V3_CALL* reset)(
        WotbModV3Handle mod);
} WotbModV3GameplayHudApiV1;

#ifdef __cplusplus
}
#endif
