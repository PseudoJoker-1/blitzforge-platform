#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_PROJECTILE_VERSION 1u

#define WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED \
    "wotbmod.gameplay.local_shell_fired"
#define WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED \
    "wotbmod.gameplay.visible_tracer_created"
#define WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED \
    "wotbmod.gameplay.visible_tracer_destroyed"

typedef enum WotbModV3ProjectileOwnerScope {
    WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN = 0,
    WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER = 1,
    WOTBMOD_V3_PROJECTILE_OWNER_ALLY_VISIBLE = 2,
    WOTBMOD_V3_PROJECTILE_OWNER_ENEMY_VISIBLE = 3,
    WOTBMOD_V3_PROJECTILE_OWNER_REPLAY = 4
} WotbModV3ProjectileOwnerScope;

/*
 * UNKNOWN is deliberately not publishable. The loader must prove one of the
 * remaining scopes from stock visibility/local-player state before a shell or
 * tracer may enter the public projectile registry.
 */

typedef struct WotbModV3TracerStyleDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    const char* id;
    const char* texture_uri;
    WotbModV3Color color;
    float width;
    float lifetime_seconds;
    float fade_start;
    int32_t priority;
} WotbModV3TracerStyleDescriptor;

typedef struct WotbModV3LocalShellFiredEvent {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3ProjectileHandle projectile;
    uint32_t shell_public_id;
    uint32_t shell_type;
    WotbModV3Vec3 origin;
    WotbModV3Vec3 visible_direction;
} WotbModV3LocalShellFiredEvent;

typedef struct WotbModV3VisibleTracerEvent {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3ProjectileHandle projectile;
    uint32_t owner_scope;
    uint32_t shell_type;
    WotbModV3Vec3 visible_position;
    WotbModV3Vec3 visible_direction;
} WotbModV3VisibleTracerEvent;

typedef struct WotbModV3ProjectileApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* tracer_style_register)(
        WotbModV3Handle mod,
        const WotbModV3TracerStyleDescriptor* descriptor,
        WotbModV3Handle* out_style);
    WotbModV3Result(WOTBMOD_V3_CALL* tracer_style_unregister)(
        WotbModV3Handle mod,
        WotbModV3Handle style);
    WotbModV3Result(WOTBMOD_V3_CALL* tracer_set_texture)(
        WotbModV3Handle mod,
        WotbModV3Handle style,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* tracer_set_color)(
        WotbModV3Handle mod,
        WotbModV3Handle style,
        WotbModV3Color color);
    WotbModV3Result(WOTBMOD_V3_CALL* tracer_set_width)(
        WotbModV3Handle mod,
        WotbModV3Handle style,
        float width);
    WotbModV3Result(WOTBMOD_V3_CALL* tracer_set_lifetime)(
        WotbModV3Handle mod,
        WotbModV3Handle style,
        float seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* tracer_set_fade)(
        WotbModV3Handle mod,
        WotbModV3Handle style,
        float fade_start);
    WotbModV3Result(WOTBMOD_V3_CALL* projectile_get_visual_entity)(
        WotbModV3Handle mod,
        WotbModV3ProjectileHandle projectile,
        WotbModV3SceneHandle* out_entity);
    WotbModV3Result(WOTBMOD_V3_CALL* projectile_attach_visual)(
        WotbModV3Handle mod,
        WotbModV3ProjectileHandle projectile,
        WotbModV3SceneHandle entity,
        uint32_t lifetime_policy);
    WotbModV3Result(WOTBMOD_V3_CALL* projectile_get_owner_scope)(
        WotbModV3Handle mod,
        WotbModV3ProjectileHandle projectile,
        uint32_t* out_scope);
} WotbModV3ProjectileApiV1;

#ifdef __cplusplus
}
#endif
