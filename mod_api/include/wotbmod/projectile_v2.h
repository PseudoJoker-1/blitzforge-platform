#pragma once

#include "projectile_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_PROJECTILE_VERSION_2 2u

#define WOTBMOD_V3_EVENT_PROJECTILE_CREATED \
    "wotbmod.gameplay.projectile.created"
#define WOTBMOD_V3_EVENT_PROJECTILE_UPDATED \
    "wotbmod.gameplay.projectile.updated"
#define WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED \
    "wotbmod.gameplay.projectile.impacted"
#define WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED \
    "wotbmod.gameplay.projectile.destroyed"

typedef enum WotbModV3ProjectileLifecycleState {
    WOTBMOD_V3_PROJECTILE_STATE_CREATED = 1,
    WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT = 2,
    WOTBMOD_V3_PROJECTILE_STATE_IMPACTED = 3,
    WOTBMOD_V3_PROJECTILE_STATE_DESTROYED = 4
} WotbModV3ProjectileLifecycleState;

typedef enum WotbModV3ProjectileSource {
    /* VehicleGameLogic::showShooting; no physical Bullet/Tracer pointer. */
    WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT = 1,
    /* GameSceneController::OnVehicleHitDamage with native shot id/position. */
    WOTBMOD_V3_PROJECTILE_SOURCE_NATIVE_IMPACT = 2,
    /* Reserved for a future ownership-safe stock Tracer lifecycle hook. */
    WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_TRACER = 3,
    WOTBMOD_V3_PROJECTILE_SOURCE_API_MANAGED = 4
} WotbModV3ProjectileSource;

typedef enum WotbModV3ProjectileField {
    WOTBMOD_V3_PROJECTILE_FIELD_NATIVE_SHOT_ID = UINT64_C(1) << 0,
    WOTBMOD_V3_PROJECTILE_FIELD_PRIMARY_ENTITY = UINT64_C(1) << 1,
    WOTBMOD_V3_PROJECTILE_FIELD_SECONDARY_ENTITY = UINT64_C(1) << 2,
    WOTBMOD_V3_PROJECTILE_FIELD_SHELL_TYPE = UINT64_C(1) << 3,
    WOTBMOD_V3_PROJECTILE_FIELD_ORIGIN = UINT64_C(1) << 4,
    WOTBMOD_V3_PROJECTILE_FIELD_DIRECTION = UINT64_C(1) << 5,
    WOTBMOD_V3_PROJECTILE_FIELD_POSITION = UINT64_C(1) << 6,
    WOTBMOD_V3_PROJECTILE_FIELD_IMPACT_POSITION = UINT64_C(1) << 7,
    WOTBMOD_V3_PROJECTILE_FIELD_STOCK_SHOT_CODE = UINT64_C(1) << 8,
    WOTBMOD_V3_PROJECTILE_FIELDS_ALL = (UINT64_C(1) << 9) - UINT64_C(1)
} WotbModV3ProjectileField;

typedef enum WotbModV3ProjectileDestroyReason {
    WOTBMOD_V3_PROJECTILE_DESTROY_NATIVE = 1,
    WOTBMOD_V3_PROJECTILE_DESTROY_IMPACT_COMPLETE = 2,
    WOTBMOD_V3_PROJECTILE_DESTROY_TIMEOUT = 3,
    WOTBMOD_V3_PROJECTILE_DESTROY_ENTITY_REMOVED = 4,
    WOTBMOD_V3_PROJECTILE_DESTROY_SHUTDOWN = 5
} WotbModV3ProjectileDestroyReason;

/*
 * A snapshot is provenance-aware: valid_fields is authoritative. In
 * particular, STOCK_SHOT never invents an origin, direction, or native shot
 * id when the stock callback did not provide one. primary/secondary_entity_id
 * deliberately avoid claiming attacker/target semantics that the current
 * native callback has not proved.
 */
typedef struct WotbModV3ProjectileSnapshot {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3ProjectileHandle projectile;
    uint64_t sequence_id;
    uint64_t valid_fields;
    uint64_t timestamp_microseconds;
    uint32_t lifecycle_state;
    uint32_t source;
    uint32_t owner_scope;
    uint32_t shell_type;
    uint32_t native_shot_id;
    uint32_t primary_entity_id;
    uint32_t secondary_entity_id;
    uint32_t native_flags;
    uint32_t stock_shot_code;
    uint32_t reserved;
    WotbModV3Vec3 origin;
    WotbModV3Vec3 visible_direction;
    WotbModV3Vec3 visible_position;
    WotbModV3Vec3 impact_position;
} WotbModV3ProjectileSnapshot;

typedef struct WotbModV3ProjectileLifecycleEvent {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t previous_state;
    uint32_t reason;
    WotbModV3ProjectileSnapshot snapshot;
} WotbModV3ProjectileLifecycleEvent;

typedef enum WotbModV3ImpactVisualFlag {
    WOTBMOD_V3_IMPACT_VISUAL_NONE = 0,
    /* Attach a native DAVA scene entity to the active scene for its lifetime. */
    WOTBMOD_V3_IMPACT_VISUAL_NATIVE_SCENE = 1u << 0
} WotbModV3ImpactVisualFlag;

typedef struct WotbModV3ImpactVisualDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    const char* id;
    const char* scene_uri;
    uint32_t flags;
    uint32_t owner_scope_mask;
    uint32_t shell_type; /* zero matches every shell type */
    uint32_t max_instances;
    float lifetime_seconds;
    float uniform_scale;
    int32_t priority;
    uint32_t reserved;
} WotbModV3ImpactVisualDescriptor;

/* V2 is prefix-compatible with WotbModV3ProjectileApiV1. */
typedef struct WotbModV3ProjectileApiV2 {
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

    WotbModV3Result(WOTBMOD_V3_CALL* projectile_get_snapshot)(
        WotbModV3Handle mod,
        WotbModV3ProjectileHandle projectile,
        WotbModV3ProjectileSnapshot* out_snapshot);
    WotbModV3Result(WOTBMOD_V3_CALL* impact_visual_register)(
        WotbModV3Handle mod,
        const WotbModV3ImpactVisualDescriptor* descriptor,
        WotbModV3Handle* out_visual);
    WotbModV3Result(WOTBMOD_V3_CALL* impact_visual_update)(
        WotbModV3Handle mod,
        WotbModV3Handle visual,
        const WotbModV3ImpactVisualDescriptor* descriptor);
    WotbModV3Result(WOTBMOD_V3_CALL* impact_visual_unregister)(
        WotbModV3Handle mod,
        WotbModV3Handle visual);
} WotbModV3ProjectileApiV2;

#ifdef __cplusplus
}
#endif
