#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_EVENTS_VERSION 1u
#define WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION 1u
#define WOTBMOD_V3_MAX_EVENT_TOPIC 192u
#define WOTBMOD_V3_MAX_EVENT_PAYLOAD (1024u * 1024u)

typedef enum WotbModV3EventPriority {
    WOTBMOD_V3_EVENT_PRIORITY_LOWEST = -1000,
    WOTBMOD_V3_EVENT_PRIORITY_LOW = -100,
    WOTBMOD_V3_EVENT_PRIORITY_NORMAL = 0,
    WOTBMOD_V3_EVENT_PRIORITY_HIGH = 100,
    WOTBMOD_V3_EVENT_PRIORITY_HIGHEST = 1000
} WotbModV3EventPriority;

typedef enum WotbModV3EventFlags {
    WOTBMOD_V3_EVENT_FLAG_NONE = 0,
    WOTBMOD_V3_EVENT_FLAG_STOPPABLE = 1u << 0,
    WOTBMOD_V3_EVENT_FLAG_MUTABLE_PAYLOAD = 1u << 1,
    WOTBMOD_V3_EVENT_FLAG_SYSTEM = 1u << 2,
    /* Delivery may be skipped after the receiver exhausts its per-frame
     * callback budget. State/lifecycle events must not set this flag. */
    WOTBMOD_V3_EVENT_FLAG_COALESCIBLE = 1u << 3
} WotbModV3EventFlags;

typedef struct WotbModV3Event {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Token dispatch_token;
    WotbModV3Handle publisher_mod;
    uint64_t timestamp_ns;
    uint64_t context_mask;
    uint32_t thread_role;
    uint32_t flags;
    char topic[WOTBMOD_V3_MAX_EVENT_TOPIC];
    void* payload;
    uint32_t payload_size;
    uint32_t propagation_stopped;
} WotbModV3Event;

/*
 * topic_pattern is either an exact topic or a prefix ending in '*'.
 * subscribe returns E_NOT_SUPPORTED without creating a token when the pattern
 * cannot match a currently available publisher. System-only patterns require
 * receive_system_events to be non-zero; mod.<id>.* topics are published by
 * mods through post().
 */
typedef struct WotbModV3EventSubscriptionInfo {
    uint32_t struct_size;
    uint32_t api_version;
    const char* topic_pattern;
    int32_t priority;
    uint32_t receive_system_events;
    uint32_t reserved;
} WotbModV3EventSubscriptionInfo;

typedef struct WotbModV3EventDispatchInfo {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Token dispatch_token;
    uint64_t timestamp_ns;
    uint64_t context_mask;
    uint32_t thread_role;
    uint32_t flags;
    uint32_t propagation_stopped;
    uint32_t reserved;
} WotbModV3EventDispatchInfo;

/*
 * Normalized main-thread bridge for native client events. The type values are
 * stable bit values so multiple types can also be represented as a mask by
 * tooling. Resource presence is reported without exposing native pointers.
 */
typedef enum WotbModV3ClientEventType {
    WOTBMOD_V3_CLIENT_EVENT_UI_SCREEN_CHANGED = 1u << 0,
    WOTBMOD_V3_CLIENT_EVENT_SCENE_ACTIVATED = 1u << 1,
    WOTBMOD_V3_CLIENT_EVENT_SCENE_DEACTIVATED = 1u << 2,
    WOTBMOD_V3_CLIENT_EVENT_UI_INPUT = 1u << 3,
    WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED = 1u << 4,
    WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED = 1u << 5,
    WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED = 1u << 6,
    WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT = 1u << 7,
    WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED = 1u << 8,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPAWNED = 1u << 9,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESPAWNED = 1u << 10,
    WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED = 1u << 11,
    WOTBMOD_V3_CLIENT_EVENT_SHELL_HIT = 1u << 12,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_HEALTH_CHANGED = 1u << 13,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED = 1u << 14,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESTROYED = 1u << 15,
    WOTBMOD_V3_CLIENT_EVENT_RELOAD_STATE_CHANGED = 1u << 16,
    WOTBMOD_V3_CLIENT_EVENT_AMMO_CHANGED = 1u << 17,
    WOTBMOD_V3_CLIENT_EVENT_AIM_TARGET_CHANGED = 1u << 18,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPOTTED = 1u << 19,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_UNSPOTTED = 1u << 20,
    WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED = 1u << 21,
    WOTBMOD_V3_CLIENT_EVENT_VEHICLE_KILLED = 1u << 22
} WotbModV3ClientEventType;

typedef struct WotbModV3UiInputEventData {
    uint32_t action;
    uint32_t buttons;
    int32_t pointer_id;
    uint32_t modifiers;
    float screen_x;
    float screen_y;
    float local_x;
    float local_y;
} WotbModV3UiInputEventData;

typedef struct WotbModV3BattleEventData {
    uint64_t battle_id;
    uint32_t arena_id;
    uint32_t state;
    uint32_t winner_team;
    uint32_t reason;
} WotbModV3BattleEventData;

typedef struct WotbModV3VehicleEventData {
    uint32_t entity_id;
    uint32_t other_entity_id;
    int32_t previous_health;
    int32_t health;
    uint32_t flags;
} WotbModV3VehicleEventData;

typedef struct WotbModV3ShotEventData {
    uint32_t shot_code;
    uint32_t shell_id;
    WotbModV3Vec3 position;
    WotbModV3Vec3 direction;
} WotbModV3ShotEventData;

typedef struct WotbModV3HitEventData {
    uint32_t shot_id;
    uint32_t shell_id;
    uint32_t flags;
    WotbModV3Vec3 position;
    WotbModV3Vec3 normal;
} WotbModV3HitEventData;

typedef struct WotbModV3DamageEventData {
    int32_t damage;
    int32_t previous_health;
    int32_t health;
    uint32_t reason_code;
    uint32_t source_entity_id;
} WotbModV3DamageEventData;

typedef struct WotbModV3ReloadEventData {
    uint32_t state;
    uint32_t paused;
    float duration_seconds;
    float progress;
    float remaining_seconds;
} WotbModV3ReloadEventData;

typedef struct WotbModV3AmmoEventData {
    uint32_t previous_shell_id;
    uint32_t shell_id;
    int32_t count;
} WotbModV3AmmoEventData;

typedef struct WotbModV3CameraEventData {
    uint32_t previous_mode;
    uint32_t mode;
    int32_t native_mode;
    uint32_t flags;
} WotbModV3CameraEventData;

/*
 * wotbmod.vehicle.killed: a kill confirmed by the server's arena packet.
 * victim_id is the destroyed vehicle, killer_id the credited vehicle (0 for
 * nobody, e.g. drowning), assist_id the assisting vehicle or 0, reason the
 * arena's attack reason code, ammo_bay_exploded 1 when the victim's ammo
 * rack went off. Ids match public_id of wotbmod.entity.public.
 */
typedef struct WotbModV3VehicleKillEventData {
    uint32_t victim_id;
    uint32_t killer_id;
    uint32_t assist_id;
    uint32_t reason;
    uint32_t ammo_bay_exploded;
} WotbModV3VehicleKillEventData;

typedef union WotbModV3ClientEventData {
    WotbModV3UiInputEventData ui_input;
    WotbModV3BattleEventData battle;
    WotbModV3VehicleEventData vehicle;
    WotbModV3ShotEventData shot;
    WotbModV3HitEventData hit;
    WotbModV3DamageEventData damage;
    WotbModV3ReloadEventData reload;
    WotbModV3AmmoEventData ammo;
    WotbModV3CameraEventData camera;
    WotbModV3VehicleKillEventData kill;
} WotbModV3ClientEventData;

typedef struct WotbModV3ClientEventEnvelope {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t flags;
    uint64_t sequence;
    uint32_t primary_entity_id;
    uint32_t other_entity_id;
    uint32_t resource_type;
    uint32_t has_previous_resource;
    uint32_t has_resource;
    uint32_t payload_size;
    WotbModV3ClientEventData payload;
} WotbModV3ClientEventEnvelope;

typedef void(WOTBMOD_V3_CALL* WotbModV3EventCallback)(
    WotbModV3Handle mod,
    WotbModV3Event* event,
    void* user_data);

typedef struct WotbModV3EventsApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* subscribe)(
        WotbModV3Handle mod,
        const WotbModV3EventSubscriptionInfo* info,
        WotbModV3EventCallback callback,
        void* user_data,
        WotbModV3EventToken* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unsubscribe)(
        WotbModV3Handle mod,
        WotbModV3EventToken token);
    WotbModV3Result(WOTBMOD_V3_CALL* set_priority)(
        WotbModV3Handle mod,
        WotbModV3EventToken token,
        int32_t priority);
    WotbModV3Result(WOTBMOD_V3_CALL* post)(
        WotbModV3Handle mod,
        const char* topic,
        const void* payload,
        uint32_t payload_size,
        uint32_t flags);
    WotbModV3Result(WOTBMOD_V3_CALL* stop_propagation)(
        WotbModV3Handle mod,
        WotbModV3Token dispatch_token);
    WotbModV3Result(WOTBMOD_V3_CALL* get_dispatch_info)(
        WotbModV3Handle mod,
        WotbModV3Token dispatch_token,
        WotbModV3EventDispatchInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* get_thread)(
        WotbModV3Handle mod,
        WotbModV3Token dispatch_token,
        uint32_t* out_thread_role);
    WotbModV3Result(WOTBMOD_V3_CALL* get_timestamp)(
        WotbModV3Handle mod,
        WotbModV3Token dispatch_token,
        uint64_t* out_timestamp_ns);
    WotbModV3Result(WOTBMOD_V3_CALL* get_context)(
        WotbModV3Handle mod,
        WotbModV3Token dispatch_token,
        uint64_t* out_context_mask);
} WotbModV3EventsApiV1;

#define WOTBMOD_V3_EVENT_CLIENT_READY "wotbmod.client.ready"
#define WOTBMOD_V3_EVENT_CLIENT_SHUTTING_DOWN "wotbmod.client.shutting_down"
#define WOTBMOD_V3_EVENT_GAME_STATE_CHANGED "wotbmod.game.state_changed"
#define WOTBMOD_V3_EVENT_CAPABILITIES_CHANGED "wotbmod.capabilities.changed"
#define WOTBMOD_V3_EVENT_FRAME_UPDATE "wotbmod.frame.update"
#define WOTBMOD_V3_EVENT_FIXED_UPDATE "wotbmod.frame.fixed_update"
#define WOTBMOD_V3_EVENT_UI_ROOT_READY "wotbmod.ui.root_ready"
#define WOTBMOD_V3_EVENT_UI_ROOT_DESTROYED "wotbmod.ui.root_destroyed"
#define WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED "wotbmod.ui.screen_changed"
#define WOTBMOD_V3_EVENT_UI_INPUT "wotbmod.ui.input"
#define WOTBMOD_V3_EVENT_UI_SCALE_CHANGED "wotbmod.ui.scale_changed"
#define WOTBMOD_V3_EVENT_SAFE_AREA_CHANGED "wotbmod.ui.safe_area_changed"
#define WOTBMOD_V3_EVENT_HUD_READY "wotbmod.hud.ready"
#define WOTBMOD_V3_EVENT_HUD_DESTROYED "wotbmod.hud.destroyed"
#define WOTBMOD_V3_EVENT_BATTLE_ENTERED "wotbmod.battle.entered"
#define WOTBMOD_V3_EVENT_BATTLE_COUNTDOWN_STARTED "wotbmod.battle.countdown_started"
#define WOTBMOD_V3_EVENT_BATTLE_STARTED "wotbmod.battle.started"
#define WOTBMOD_V3_EVENT_BATTLE_ENDED "wotbmod.battle.ended"
#define WOTBMOD_V3_EVENT_BATTLE_LEFT "wotbmod.battle.left"
#define WOTBMOD_V3_EVENT_SCENE_ACTIVATED "wotbmod.scene.activated"
#define WOTBMOD_V3_EVENT_SCENE_DEACTIVATED "wotbmod.scene.deactivated"
#define WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED "wotbmod.vehicle.local.changed"
#define WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CREATED "wotbmod.vehicle.local.created"
#define WOTBMOD_V3_EVENT_LOCAL_VEHICLE_APPEARANCE_READY "wotbmod.vehicle.local.appearance_ready"
#define WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED "wotbmod.vehicle.local.destroyed"
#define WOTBMOD_V3_EVENT_VEHICLE_SPAWNED "wotbmod.vehicle.spawned"
#define WOTBMOD_V3_EVENT_VEHICLE_DESPAWNED "wotbmod.vehicle.despawned"
#define WOTBMOD_V3_EVENT_SHOT_FIRED "wotbmod.gameplay.shot_fired"
#define WOTBMOD_V3_EVENT_SHELL_HIT "wotbmod.gameplay.shell_hit"
#define WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED "wotbmod.vehicle.health_changed"
#define WOTBMOD_V3_EVENT_VEHICLE_DAMAGED "wotbmod.vehicle.damaged"
#define WOTBMOD_V3_EVENT_VEHICLE_DESTROYED "wotbmod.vehicle.destroyed"
#define WOTBMOD_V3_EVENT_RELOAD_STATE_CHANGED "wotbmod.gameplay.reload_state_changed"
#define WOTBMOD_V3_EVENT_AMMO_CHANGED "wotbmod.gameplay.ammo_changed"
#define WOTBMOD_V3_EVENT_AIM_TARGET_CHANGED "wotbmod.gameplay.aim_target_changed"
#define WOTBMOD_V3_EVENT_VEHICLE_SPOTTED "wotbmod.vehicle.spotted"
#define WOTBMOD_V3_EVENT_VEHICLE_UNSPOTTED "wotbmod.vehicle.unspotted"
#define WOTBMOD_V3_EVENT_VEHICLE_KILLED "wotbmod.vehicle.killed"
#define WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED "wotbmod.render.device_created"
#define WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST "wotbmod.render.device_lost"
#define WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED "wotbmod.render.device_restored"
#define WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED "wotbmod.render.swapchain_resized"
#define WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED "wotbmod.render.backend_changed"
#define WOTBMOD_V3_EVENT_AUDIO_DEVICE_CHANGED "wotbmod.audio.device_changed"
#define WOTBMOD_V3_EVENT_SOUND_EVENT_CREATED "wotbmod.audio.sound_created"
#define WOTBMOD_V3_EVENT_SOUND_EVENT_FINISHED "wotbmod.audio.sound_finished"
#define WOTBMOD_V3_EVENT_INPUT_DEVICE_CHANGED "wotbmod.input.device_changed"
#define WOTBMOD_V3_EVENT_INPUT_ACTION "wotbmod.input.action"
#define WOTBMOD_V3_EVENT_FOV_CHANGED "wotbmod.gameplay.fov_changed"
#define WOTBMOD_V3_EVENT_ZOOM_CHANGED "wotbmod.gameplay.zoom_changed"
#define WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED "wotbmod.gameplay.camera_mode_changed"
#define WOTBMOD_V3_EVENT_CAMERA_TRANSITION_STARTED "wotbmod.gameplay.camera_transition_started"
#define WOTBMOD_V3_EVENT_CAMERA_TRANSITION_ENDED "wotbmod.gameplay.camera_transition_ended"
#define WOTBMOD_V3_EVENT_SNIPER_ENTERED "wotbmod.gameplay.sniper_entered"
#define WOTBMOD_V3_EVENT_SNIPER_EXITED "wotbmod.gameplay.sniper_exited"
#define WOTBMOD_V3_EVENT_POSTPROCESSING_TOGGLED "wotbmod.gameplay.postprocessing_toggled"
#define WOTBMOD_V3_EVENT_REPLAY_SPEED_CHANGED "wotbmod.replay.speed_changed"
#define WOTBMOD_V3_EVENT_REPLAY_SEEK "wotbmod.replay.seek"
#define WOTBMOD_V3_EVENT_HANGAR_BACKGROUND_CHANGED "wotbmod.hangar.background_changed"
#define WOTBMOD_V3_EVENT_RETICLE_CHANGED "wotbmod.hud.reticle_changed"
#define WOTBMOD_V3_EVENT_DAMAGE_DEALT "wotbmod.gameplay.damage_dealt"
#define WOTBMOD_V3_EVENT_DAMAGE_RECEIVED "wotbmod.gameplay.damage_received"
#define WOTBMOD_V3_EVENT_SIXTH_SENSE_TRIGGERED "wotbmod.gameplay.sixth_sense_triggered"
#define WOTBMOD_V3_EVENT_MINIMAP_MARKER_ADDED "wotbmod.gameplay.minimap_marker_added"
#define WOTBMOD_V3_EVENT_SESSION_STATS_UPDATED "wotbmod.gameplay.session_stats_updated"
#ifndef WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED
#define WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED "wotbmod.entity.public.added"
#endif
#ifndef WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED
#define WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED "wotbmod.entity.public.updated"
#endif
#ifndef WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED
#define WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED "wotbmod.entity.public.removed"
#endif
#ifndef WOTBMOD_V3_EVENT_RPC_OBSERVED
#define WOTBMOD_V3_EVENT_RPC_OBSERVED "wotbmod.bigworld.rpc.observed"
#endif

#ifdef __cplusplus
}
#endif
