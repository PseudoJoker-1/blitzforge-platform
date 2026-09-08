#include "lua_bindings.h"

#include "lua_convert.h"
#include "lua_permissions.h"
#include "lua_script.h"

#include "../../include/wotbmod/bigworld_rpc_v1.h"
#include "../../include/wotbmod/entity_public_v1.h"
#include "../../include/wotbmod/projectile_v2.h"
#include "../../include/wotbmod/session_cluster_v1.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

// wotb.events - the one interface where the client calls into Lua rather than
// the other way round, and therefore the one where the threading rule in
// lua_script.h is load bearing rather than decorative.
//
// The shape of the problem, stated once:
//
//   A WotbModV3Event carries a thread_role. A callback can be delivered on
//   the render thread or a worker while the main thread is already executing
//   script code in the same lua_State. Two threads inside one lua_State is
//   memory corruption - a half-grown stack, a half-linked GC object - not a
//   race for a value that resolves one way or the other. So every delivery
//   takes the script's own recursive lock through LuaScript::Entry, exactly
//   like every other entry into that state.
//
//   Marshalling deliveries to the main thread was considered and rejected in
//   design: it would make delivery deferred, and stop_propagation has to be
//   answerable synchronously - a handler must be able to say "do not pass
//   this on" before the dispatch moves to the next subscriber. Deferring
//   delivery would make that promise unkeepable. So the lock is the whole
//   mechanism, and everything below exists to make taking it safe.
//
// Three hazards follow from that, and each has a specific answer here:
//
//   1. Lock order, including one lock this host does not own. Three are in
//      play: this file's subscription table lock, a script's own lock, and
//      whatever lock the *client's* dispatcher holds while it is calling a
//      callback.
//
//      The two this file owns are ordered strictly script-then-table. A
//      binding called from Lua already holds the script lock and then takes
//      the table's (subscribe, unsubscribe). A delivery arrives holding
//      neither and needs both; taking the table lock and then the script
//      lock would close a cycle against every binding, so a delivery takes
//      the table lock, copies what it needs, *releases it*, and only then
//      takes the script lock. The two are never held at once in that order.
//
//      The client's lock cannot be ordered from here, and the residual
//      hazard is stated rather than hidden, because a reader will otherwise
//      take the paragraph above as the whole story. A delivery is
//      client-then-script: the dispatcher calls in, and DeliverEvent takes
//      the script lock underneath it. Every ABI call a script makes is
//      script-then-client: the script lock is held for as long as that
//      script is executing - Create's chunk, CallGlobal, a delivery - and
//      every binding calls the client from inside it. Those two are an ABBA
//      pair against any client that holds a dispatcher lock across a
//      callback and takes that same lock in unsubscribe, post or
//      set_priority, which is a normal client design.
//
//      It follows from delivery being synchronous, which is the decision
//      that bought stop_propagation an answer before the dispatch moves on,
//      and no locking on this side can undo it: a script that calls the ABI
//      is by definition running, and a running script holds its lock. What
//      this side can do is never make an ABI call under a lock it has a
//      choice about - ReleaseEventSubscriptions calls unsubscribe outside
//      both the table lock and the script lock, because it is the one path
//      here that is not already inside a script. A mock cannot find this
//      class of bug: it has no dispatcher lock to invert against.
//
//   2. A dangling script. A delivery that copied a LuaScript* and then
//      blocked on the script lock must not find that object destroyed when
//      it wakes. Every delivery marks the subscription in-flight under the
//      table lock before releasing it, and ReleaseEventSubscriptions - which
//      ~LuaScript calls before it closes the state - removes the
//      subscriptions from the table (so no *new* delivery can find them) and
//      then waits for the in-flight count to reach zero. It is called with
//      no script lock held, precisely so that the delivery it is waiting for
//      can acquire that lock and finish.
//
//   3. A stale registry reference. unsubscribe releases the luaL_ref, and it
//      runs from Lua with the script lock held. A delivery re-reads the ref
//      *after* taking the script lock, so it either sees a live subscription
//      and a valid ref, or sees the subscription already dead and returns.
//      It can never push a ref that has been released and recycled.
//
// The user_data handed to the client is a subscription id, never a pointer:
// a stale or corrupted user_data then fails a lookup instead of being
// dereferenced, and ids are never recycled, so a delivery arriving late for
// a long-gone subscription cannot address a new one that happens to occupy
// the same memory.

namespace wotbmod {
namespace lua {
namespace {

// ---------------------------------------------------------------------------
// The subscription table
// ---------------------------------------------------------------------------

struct Subscription {
    uint32_t id = 0u;
    LuaScript* script = nullptr;
    const WotbModV3EventsApiV1* api = nullptr;
    const WotbModV3CoreApiV1* core_api = nullptr;
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3EventToken token = 0u;
    int callback_ref = LUA_NOREF;
    bool live = false;         // false once unsubscribed or released
    int in_flight = 0;         // deliveries between lookup and completion
    // Set while ReleaseEventSubscriptions is holding this record outside the
    // table lock, across its wait. Without it, a delivery that drops
    // in_flight to zero in that window would free the record that release is
    // about to unsubscribe and unref through.
    bool held_for_release = false;
};

// Function-local statics rather than file-scope objects: this is a DLL, and
// a file-scope std::mutex would be constructed in an order relative to other
// translation units that nothing here controls.
std::mutex& TableLock() {
    static std::mutex lock;
    return lock;
}

std::condition_variable& TableIdle() {
    static std::condition_variable idle;
    return idle;
}

std::vector<Subscription*>& Table() {
    static std::vector<Subscription*> table;
    return table;
}

// Guarded by TableLock(). Monotonic and never recycled - see the file header
// for why that matters more than it looks.
uint32_t g_next_id = 1u;

// Guarded by TableLock(). Found by id, not by pointer.
Subscription* FindLocked(uint32_t id) {
    for (Subscription* entry : Table()) {
        if (entry->id == id) return entry;
    }
    return nullptr;
}

// Guarded by TableLock(). A dead subscription with no delivery in flight and
// nobody holding it is nobody's business any more.
void ReapLocked(Subscription* subscription) {
    if (!subscription || subscription->live || subscription->in_flight > 0 ||
        subscription->held_for_release) {
        return;
    }
    std::vector<Subscription*>& table = Table();
    for (auto it = table.begin(); it != table.end(); ++it) {
        if (*it == subscription) {
            table.erase(it);
            break;
        }
    }
    delete subscription;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Reports a callback fault. Every buffer here is a fixed stack array and
// nothing in scope has a destructor: this runs with the script lock held,
// straight after a lua_pcall, and must not be the thing that turns a script
// bug into a host failure. Rule 5.
void LogFault(const WotbModV3CoreApiV1* core_api, WotbModV3Handle mod,
              const char* topic, const char* detail) noexcept {
    char line[1024] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "event callback failed for '%s': %s", topic ? topic : "",
                detail ? detail : "<no message>");
    if (core_api && core_api->log) {
        core_api->log(mod, WOTBMOD_V3_LOG_ERROR, "lua", line);
        return;
    }
    char fallback[1152] = {};
    _snprintf_s(fallback, sizeof(fallback), _TRUNCATE, "[wotbmod.lua] %s\n",
                line);
    OutputDebugStringA(fallback);
}

// ---------------------------------------------------------------------------
// Delivery
// ---------------------------------------------------------------------------

// What the protected half of a delivery needs, passed as one light userdata
// so that pushing it cannot allocate.
struct DeliveryArgs {
    int callback_ref;
    const WotbModV3Event* event;
};

void SetInteger(lua_State* state, const char* name, lua_Integer value) {
    lua_pushinteger(state, value);
    lua_setfield(state, -2, name);
}

void SetNumber(lua_State* state, const char* name, lua_Number value) {
    lua_pushnumber(state, value);
    lua_setfield(state, -2, name);
}

void SetBoolean(lua_State* state, const char* name, bool value) {
    lua_pushboolean(state, value ? 1 : 0);
    lua_setfield(state, -2, name);
}

void SetString(lua_State* state, const char* name, const char* value) {
    lua_pushstring(state, value ? value : "");
    lua_setfield(state, -2, name);
}

void PushVec3(lua_State* state, const WotbModV3Vec3& value) {
    lua_createtable(state, 0, 3);
    SetNumber(state, "x", value.x);
    SetNumber(state, "y", value.y);
    SetNumber(state, "z", value.z);
}

void PushPublicEntitySnapshot(
    lua_State* state,
    const WotbModV3PublicEntitySnapshot& value) {
    lua_createtable(state, 0, 19);
    PushHandle(state, value.handle, kHandleEntity);
    lua_setfield(state, -2, "handle");
    SetInteger(state, "public_id", value.public_id);
    SetInteger(state, "type", value.type);
    SetBoolean(state, "visible_to_player", value.visible_to_player != 0u);
    SetBoolean(state, "local_player", value.local_player != 0u);
    SetInteger(state, "team", value.team);
    SetBoolean(state, "team_available", value.team != 0u);
    SetInteger(state, "health", value.health);
    SetInteger(state, "max_health", value.max_health);
    SetBoolean(state, "alive", value.health > 0);
    if (value.max_health > 0) {
        SetNumber(
            state,
            "health_percent",
            static_cast<lua_Number>(value.health) * 100.0 /
                static_cast<lua_Number>(value.max_health));
    } else {
        lua_pushnil(state);
        lua_setfield(state, -2, "health_percent");
    }
    PushVec3(state, value.position);
    lua_setfield(state, -2, "position");
    PushVec3(state, value.direction);
    lua_setfield(state, -2, "direction");
    // Provenance from the data, not a constant. The loader publishes a pose
    // only for a vehicle whose appearance chain it verified, and the ingress
    // accepts a direction only as a unit vector; an unsourced record keeps
    // the zero vector the frozen snapshot starts with. A unit-length
    // direction is therefore the honest sign of a sourced transform, and
    // position follows it because both come from one matrix. The same rule
    // is spelled in Lua for wotb.players (kPlayersLibrary, has_pose).
    const float length2 = value.direction.x * value.direction.x +
                          value.direction.y * value.direction.y +
                          value.direction.z * value.direction.z;
    const bool sourced = length2 > 0.98f && length2 < 1.02f;
    SetBoolean(state, "position_available", sourced);
    SetBoolean(state, "direction_available", sourced);
    SetString(state, "public_type", value.public_type);
    SetString(state, "display_name", value.display_name);
    SetBoolean(state, "display_name_available", value.display_name[0] != '\0');
}

void PushProjectileSnapshot(
    lua_State* state,
    const WotbModV3ProjectileSnapshot& value) {
    lua_createtable(state, 0, 18);
    PushHandle(state, value.projectile, kHandleProjectile);
    lua_setfield(state, -2, "projectile");
    SetInteger(state, "sequence_id", static_cast<lua_Integer>(value.sequence_id));
    SetInteger(state, "valid_fields", static_cast<lua_Integer>(value.valid_fields));
    SetInteger(
        state,
        "timestamp_microseconds",
        static_cast<lua_Integer>(value.timestamp_microseconds));
    SetInteger(state, "lifecycle_state", value.lifecycle_state);
    SetInteger(state, "source", value.source);
    SetInteger(state, "owner_scope", value.owner_scope);
    SetInteger(state, "shell_type", value.shell_type);
    SetInteger(state, "native_shot_id", value.native_shot_id);
    SetInteger(state, "primary_entity_id", value.primary_entity_id);
    SetInteger(state, "secondary_entity_id", value.secondary_entity_id);
    SetInteger(state, "native_flags", value.native_flags);
    SetInteger(state, "stock_shot_code", value.stock_shot_code);
    PushVec3(state, value.origin);
    lua_setfield(state, -2, "origin");
    PushVec3(state, value.visible_direction);
    lua_setfield(state, -2, "visible_direction");
    PushVec3(state, value.visible_position);
    lua_setfield(state, -2, "visible_position");
    PushVec3(state, value.impact_position);
    lua_setfield(state, -2, "impact_position");
}

template <typename T>
bool CopyEventPayload(const WotbModV3Event* event, T* output) {
    if (!event || !output || !event->payload ||
        event->payload_size < sizeof(T)) {
        return false;
    }
    std::memcpy(output, event->payload, sizeof(T));
    return true;
}

const char* PrimaryTopicForClientType(uint32_t type) {
    switch (type) {
        case WOTBMOD_V3_CLIENT_EVENT_UI_SCREEN_CHANGED:
            return WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED;
        case WOTBMOD_V3_CLIENT_EVENT_SCENE_ACTIVATED:
            return WOTBMOD_V3_EVENT_SCENE_ACTIVATED;
        case WOTBMOD_V3_CLIENT_EVENT_SCENE_DEACTIVATED:
            return WOTBMOD_V3_EVENT_SCENE_DEACTIVATED;
        case WOTBMOD_V3_CLIENT_EVENT_UI_INPUT:
            return WOTBMOD_V3_EVENT_UI_INPUT;
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED:
            return WOTBMOD_V3_EVENT_BATTLE_ENTERED;
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED:
            return WOTBMOD_V3_EVENT_BATTLE_STARTED;
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED:
            return WOTBMOD_V3_EVENT_BATTLE_ENDED;
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT:
            return WOTBMOD_V3_EVENT_BATTLE_LEFT;
        case WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED:
            return WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPAWNED:
            return WOTBMOD_V3_EVENT_VEHICLE_SPAWNED;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESPAWNED:
            return WOTBMOD_V3_EVENT_VEHICLE_DESPAWNED;
        case WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED:
            return WOTBMOD_V3_EVENT_SHOT_FIRED;
        case WOTBMOD_V3_CLIENT_EVENT_SHELL_HIT:
            return WOTBMOD_V3_EVENT_SHELL_HIT;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_HEALTH_CHANGED:
            return WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED:
            return WOTBMOD_V3_EVENT_VEHICLE_DAMAGED;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESTROYED:
            return WOTBMOD_V3_EVENT_VEHICLE_DESTROYED;
        case WOTBMOD_V3_CLIENT_EVENT_RELOAD_STATE_CHANGED:
            return WOTBMOD_V3_EVENT_RELOAD_STATE_CHANGED;
        case WOTBMOD_V3_CLIENT_EVENT_AMMO_CHANGED:
            return WOTBMOD_V3_EVENT_AMMO_CHANGED;
        case WOTBMOD_V3_CLIENT_EVENT_AIM_TARGET_CHANGED:
            return WOTBMOD_V3_EVENT_AIM_TARGET_CHANGED;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPOTTED:
            return WOTBMOD_V3_EVENT_VEHICLE_SPOTTED;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_UNSPOTTED:
            return WOTBMOD_V3_EVENT_VEHICLE_UNSPOTTED;
        case WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED:
            return WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED;
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_KILLED:
            return WOTBMOD_V3_EVENT_VEHICLE_KILLED;
        default:
            return nullptr;
    }
}

bool TopicAcceptsClientType(const char* topic, uint32_t type) {
    const char* primary = PrimaryTopicForClientType(type);
    if (!topic || !primary) return false;
    if (std::strcmp(topic, primary) == 0) return true;
    if (type == WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED &&
        std::strcmp(topic, WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CREATED) == 0) {
        return true;
    }
    if (type == WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESTROYED &&
        std::strcmp(topic, WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED) == 0) {
        return true;
    }
    if (type == WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED &&
        std::strcmp(topic, WOTBMOD_V3_EVENT_DAMAGE_RECEIVED) == 0) {
        return true;
    }
    return type == WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED &&
           (std::strcmp(topic, WOTBMOD_V3_EVENT_SNIPER_ENTERED) == 0 ||
            std::strcmp(topic, WOTBMOD_V3_EVENT_SNIPER_EXITED) == 0);
}

uint32_t ClientPayloadSize(uint32_t type) {
    switch (type) {
        case WOTBMOD_V3_CLIENT_EVENT_UI_INPUT:
            return sizeof(WotbModV3UiInputEventData);
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED:
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED:
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED:
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT:
            return sizeof(WotbModV3BattleEventData);
        case WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPAWNED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESPAWNED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_HEALTH_CHANGED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESTROYED:
        case WOTBMOD_V3_CLIENT_EVENT_AIM_TARGET_CHANGED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPOTTED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_UNSPOTTED:
            return sizeof(WotbModV3VehicleEventData);
        case WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED:
            return sizeof(WotbModV3ShotEventData);
        case WOTBMOD_V3_CLIENT_EVENT_SHELL_HIT:
            return sizeof(WotbModV3HitEventData);
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED:
            return sizeof(WotbModV3DamageEventData);
        case WOTBMOD_V3_CLIENT_EVENT_RELOAD_STATE_CHANGED:
            return sizeof(WotbModV3ReloadEventData);
        case WOTBMOD_V3_CLIENT_EVENT_AMMO_CHANGED:
            return sizeof(WotbModV3AmmoEventData);
        case WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED:
            return sizeof(WotbModV3CameraEventData);
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_KILLED:
            return sizeof(WotbModV3VehicleKillEventData);
        default:
            return 0u;
    }
}

bool PushClientEventData(lua_State* state, const WotbModV3Event* event) {
    WotbModV3ClientEventEnvelope envelope = {};
    if (!CopyEventPayload(event, &envelope) ||
        envelope.struct_size < sizeof(envelope) ||
        envelope.api_version != WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION ||
        envelope.payload_size > sizeof(envelope.payload) ||
        !TopicAcceptsClientType(event->topic, envelope.type)) {
        return false;
    }
    const uint32_t requiredPayload = ClientPayloadSize(envelope.type);
    if (requiredPayload != 0u && envelope.payload_size < requiredPayload) {
        return false;
    }

    lua_createtable(state, 0, 24);
    SetString(state, "kind", "client_event");
    SetInteger(state, "type", envelope.type);
    SetInteger(state, "flags", envelope.flags);
    SetInteger(state, "sequence", static_cast<lua_Integer>(envelope.sequence));
    SetInteger(state, "primary_entity_id", envelope.primary_entity_id);
    SetInteger(state, "other_entity_id", envelope.other_entity_id);
    SetInteger(state, "resource_type", envelope.resource_type);
    SetBoolean(state, "has_previous_resource", envelope.has_previous_resource != 0u);
    SetBoolean(state, "has_resource", envelope.has_resource != 0u);
    SetInteger(state, "payload_size", envelope.payload_size);

    switch (envelope.type) {
        case WOTBMOD_V3_CLIENT_EVENT_UI_INPUT: {
            const WotbModV3UiInputEventData& value = envelope.payload.ui_input;
            SetInteger(state, "action", value.action);
            SetInteger(state, "buttons", value.buttons);
            SetInteger(state, "pointer_id", value.pointer_id);
            SetInteger(state, "modifiers", value.modifiers);
            SetNumber(state, "screen_x", value.screen_x);
            SetNumber(state, "screen_y", value.screen_y);
            SetNumber(state, "local_x", value.local_x);
            SetNumber(state, "local_y", value.local_y);
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED:
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED:
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED:
        case WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT: {
            const WotbModV3BattleEventData& value = envelope.payload.battle;
            SetInteger(state, "battle_id", static_cast<lua_Integer>(value.battle_id));
            SetInteger(state, "arena_id", value.arena_id);
            SetInteger(state, "state", value.state);
            SetInteger(state, "winner_team", value.winner_team);
            SetInteger(state, "reason", value.reason);
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPAWNED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESPAWNED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_HEALTH_CHANGED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESTROYED:
        case WOTBMOD_V3_CLIENT_EVENT_AIM_TARGET_CHANGED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPOTTED:
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_UNSPOTTED: {
            const WotbModV3VehicleEventData& value = envelope.payload.vehicle;
            SetInteger(state, "entity_id", value.entity_id);
            SetInteger(state, "event_other_entity_id", value.other_entity_id);
            SetInteger(state, "previous_health", value.previous_health);
            SetInteger(state, "health", value.health);
            SetInteger(state, "vehicle_flags", value.flags);
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED: {
            const WotbModV3ShotEventData& value = envelope.payload.shot;
            SetInteger(state, "shot_code", value.shot_code);
            SetInteger(state, "shell_id", value.shell_id);
            PushVec3(state, value.position);
            lua_setfield(state, -2, "position");
            PushVec3(state, value.direction);
            lua_setfield(state, -2, "direction");
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_SHELL_HIT: {
            const WotbModV3HitEventData& value = envelope.payload.hit;
            SetInteger(state, "shot_id", value.shot_id);
            SetInteger(state, "shell_id", value.shell_id);
            SetInteger(state, "hit_flags", value.flags);
            PushVec3(state, value.position);
            lua_setfield(state, -2, "position");
            PushVec3(state, value.normal);
            lua_setfield(state, -2, "normal");
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED: {
            const WotbModV3DamageEventData& value = envelope.payload.damage;
            SetInteger(state, "damage", value.damage);
            SetInteger(state, "previous_health", value.previous_health);
            SetInteger(state, "health", value.health);
            SetInteger(state, "reason_code", value.reason_code);
            SetInteger(state, "source_entity_id", value.source_entity_id);
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_RELOAD_STATE_CHANGED: {
            const WotbModV3ReloadEventData& value = envelope.payload.reload;
            SetInteger(state, "reload_state", value.state);
            SetBoolean(state, "paused", value.paused != 0u);
            SetNumber(state, "duration_seconds", value.duration_seconds);
            SetNumber(state, "progress", value.progress);
            SetNumber(state, "remaining_seconds", value.remaining_seconds);
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_AMMO_CHANGED: {
            const WotbModV3AmmoEventData& value = envelope.payload.ammo;
            SetInteger(state, "previous_shell_id", value.previous_shell_id);
            SetInteger(state, "shell_id", value.shell_id);
            SetInteger(state, "count", value.count);
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED: {
            const WotbModV3CameraEventData& value = envelope.payload.camera;
            SetInteger(state, "previous_mode", value.previous_mode);
            SetInteger(state, "mode", value.mode);
            SetInteger(state, "native_mode", value.native_mode);
            SetInteger(state, "camera_flags", value.flags);
            break;
        }
        case WOTBMOD_V3_CLIENT_EVENT_VEHICLE_KILLED: {
            const WotbModV3VehicleKillEventData& value = envelope.payload.kill;
            SetInteger(state, "victim_id", value.victim_id);
            SetInteger(state, "killer_id", value.killer_id);
            SetInteger(state, "assist_id", value.assist_id);
            SetInteger(state, "reason", value.reason);
            SetBoolean(state, "ammo_bay_exploded", value.ammo_bay_exploded != 0u);
            break;
        }
        default:
            break;
    }
    return true;
}

bool PushTypedEventData(lua_State* state, const WotbModV3Event* event) {
    if (!event || !event->topic) return false;

    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED) == 0 ||
        std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED) == 0 ||
        std::strcmp(event->topic, WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED) == 0) {
        WotbModV3PublicEntityLifecycleEvent value = {};
        if (!CopyEventPayload(event, &value) ||
            value.struct_size < sizeof(value) ||
            value.api_version != WOTBMOD_V3_ENTITY_PUBLIC_VERSION) {
            return false;
        }
        lua_createtable(state, 0, 3);
        SetString(state, "kind", "public_entity");
        SetInteger(state, "reason", value.reason);
        PushPublicEntitySnapshot(state, value.snapshot);
        lua_setfield(state, -2, "snapshot");
        return true;
    }

    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_CREATED) == 0 ||
        std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_UPDATED) == 0 ||
        std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED) == 0 ||
        std::strcmp(event->topic, WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED) == 0) {
        WotbModV3ProjectileLifecycleEvent value = {};
        if (!CopyEventPayload(event, &value) ||
            value.struct_size < sizeof(value) ||
            value.api_version != WOTBMOD_V3_PROJECTILE_VERSION_2) {
            return false;
        }
        lua_createtable(state, 0, 4);
        SetString(state, "kind", "projectile");
        SetInteger(state, "previous_state", value.previous_state);
        SetInteger(state, "reason", value.reason);
        PushProjectileSnapshot(state, value.snapshot);
        lua_setfield(state, -2, "snapshot");
        return true;
    }

    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED) == 0) {
        WotbModV3LocalShellFiredEvent value = {};
        if (!CopyEventPayload(event, &value) ||
            value.struct_size < sizeof(value) ||
            value.api_version != WOTBMOD_V3_PROJECTILE_VERSION) {
            return false;
        }
        lua_createtable(state, 0, 7);
        SetString(state, "kind", "local_shell");
        PushHandle(state, value.projectile, kHandleProjectile);
        lua_setfield(state, -2, "projectile");
        SetInteger(state, "shell_public_id", value.shell_public_id);
        SetInteger(state, "shell_type", value.shell_type);
        PushVec3(state, value.origin);
        lua_setfield(state, -2, "origin");
        PushVec3(state, value.visible_direction);
        lua_setfield(state, -2, "visible_direction");
        return true;
    }

    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED) == 0) {
        WotbModV3ClusterChangedEvent value = {};
        if (!CopyEventPayload(event, &value) ||
            value.struct_size < sizeof(value) ||
            value.api_version != WOTBMOD_V3_SESSION_CLUSTER_VERSION) {
            return false;
        }
        lua_createtable(state, 0, 4);
        SetString(state, "kind", "cluster_changed");
        SetInteger(state, "from_cluster_id", value.from_cluster_id);
        SetInteger(state, "to_cluster_id", value.to_cluster_id);
        SetInteger(state, "status", value.status);
        return true;
    }

    if (std::strcmp(event->topic, WOTBMOD_V3_EVENT_RPC_OBSERVED) == 0) {
        WotbModV3ObservedRpc value = {};
        if (!CopyEventPayload(event, &value) ||
            value.struct_size < sizeof(value) ||
            value.api_version != WOTBMOD_V3_BIGWORLD_RPC_VERSION) {
            return false;
        }
        lua_createtable(state, 0, 8);
        SetString(state, "kind", "rpc_metadata");
        SetInteger(state, "direction", value.direction);
        SetInteger(state, "public_entity_id", value.public_entity_id);
        SetInteger(state, "sequence", static_cast<lua_Integer>(value.sequence));
        SetInteger(
            state,
            "timestamp_microseconds",
            static_cast<lua_Integer>(value.timestamp_microseconds));
        SetString(state, "entity_type", value.entity_type);
        SetString(state, "method_name", value.method_name);
        return true;
    }

    return PushClientEventData(state, event);
}

// Turns the ABI's event struct into the table a handler receives.
//
// dispatch is boxed with PushToken under kHandleEventDispatch rather than
// handed over as a number: it is the key to stop_propagation and to the four
// get_* slots, and a number would be forgeable from inside the sandbox
// (rule 3). payload crosses as a byte string with its exact length, so an
// embedded zero in a binary payload survives; a payload the client did not
// send is nil rather than an empty string, because "no payload" and "a
// zero-length payload" are different answers.
void PushEventTable(lua_State* state, const WotbModV3Event* event) {
    lua_createtable(state, 0, 9);

    lua_pushstring(state, event->topic);
    lua_setfield(state, -2, "topic");

    lua_pushinteger(state, static_cast<lua_Integer>(event->thread_role));
    lua_setfield(state, -2, "thread_role");

    lua_pushinteger(state, static_cast<lua_Integer>(event->timestamp_ns));
    lua_setfield(state, -2, "timestamp_ns");

    lua_pushinteger(state, static_cast<lua_Integer>(event->context_mask));
    lua_setfield(state, -2, "context_mask");

    lua_pushinteger(state, static_cast<lua_Integer>(event->flags));
    lua_setfield(state, -2, "flags");

    PushToken(state, event->dispatch_token, kHandleEventDispatch);
    lua_setfield(state, -2, "dispatch");

    PushHandle(state, event->publisher_mod, kHandleMod);
    lua_setfield(state, -2, "publisher");

    if (event->payload && event->payload_size) {
        lua_pushlstring(state, static_cast<const char*>(event->payload),
                        event->payload_size);
    } else {
        lua_pushnil(state);
    }
    lua_setfield(state, -2, "payload");

    if (!PushTypedEventData(state, event)) {
        lua_pushnil(state);
    }
    lua_setfield(state, -2, "data");
}

// The half of a delivery that touches Lua in ways that can raise. Everything
// in here - lua_rawgeti on the registry, lua_createtable, every field push -
// can fail with LUA_ERRMEM, and LUA_ERRMEM leaves by longjmp. Outside a
// protected frame that longjmp finds L->errorJmp == NULL and reaches the
// panic handler, which calls abort() and takes the client down with the
// script; the same failure mode CallGlobal's raw global lookup exists to
// avoid. So the whole of it runs inside a lua_pcall.
//
// The exhaustive list of what its caller does outside that pcall, because
// getting this list wrong is how the hazard comes back: lua_gettop and
// lua_settop (no allocation, cannot raise); lua_checkstack (answers with a
// value rather than raising, which is what makes it usable here);
// lua_pushcfunction, which is lua_pushcclosure with zero upvalues and so
// sets a value rather than allocating a closure; lua_pushlightuserdata (no
// allocation); and reading the error object, which is guarded by a
// lua_type check for exactly this reason - lua_tostring on a non-string
// error value calls luaO_tostring and luaC_checkGC, both of which allocate,
// can run a __gc metamethod, and can raise LUA_ERRMEM. error(42) would
// otherwise abort the client here.
int DeliverProtected(lua_State* state) {
    const DeliveryArgs* args =
        static_cast<const DeliveryArgs*>(lua_touserdata(state, 1));
    lua_rawgeti(state, LUA_REGISTRYINDEX, args->callback_ref);
    PushEventTable(state, args->event);
    lua_call(state, 1, 0);
    return 0;
}

// The C callback the client holds. Not a lua_CFunction: no Lua frame is
// running underneath it, and the thread it arrives on is the client's choice.
//
// noexcept, and it is the boundary rule rather than a claim that nothing in
// here can throw: LuaScript::Entry's lock and both lock_guards can raise
// std::system_error if the OS refuses the mutex. What is on the other side
// of this function is the client's own C dispatcher, so an exception leaving
// it is undefined behaviour, not a caught error. Terminating is the honest
// end state - a lock this host cannot take is a delivery it cannot make
// safe, and continuing without it is the memory corruption this whole file
// exists to prevent.
void WOTBMOD_V3_CALL DeliverEvent(WotbModV3Handle, WotbModV3Event* event,
                                  void* user_data) noexcept {
    if (!event) return;
    const uint32_t id = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(user_data));

    LuaScript* script = nullptr;
    Subscription* subscription = nullptr;
    {
        std::lock_guard<std::mutex> guard(TableLock());
        subscription = FindLocked(id);
        if (!subscription || !subscription->live ||
            subscription->script->InstructionLimitExceeded()) {
            return;
        }
        script = subscription->script;
        // Claims the subscription against teardown: ReleaseEventSubscriptions
        // will wait for this to drop back to zero before the LuaScript below
        // can be destroyed. Set before the lock is released, which is the
        // whole point of it.
        ++subscription->in_flight;
    }

    // Deliberately outside the table lock, and deliberately re-entrant: a
    // handler that calls wotb.events.subscribe takes this same lock again on
    // this same thread, which a recursive mutex allows and a non-recursive
    // one would deadlock on.
    {
        LuaScript::Entry entry(*script);
        lua_State* state = entry.state();

        int callback_ref = LUA_NOREF;
        const WotbModV3CoreApiV1* core_api = nullptr;
        WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
        {
            // Re-read under the table lock now that the script lock is held.
            // unsubscribe runs from Lua and so cannot have been running
            // between the two; if it ran before, live is already false and
            // callback_ref has already been released. This is what makes
            // pushing a recycled ref impossible.
            std::lock_guard<std::mutex> guard(TableLock());
            if (entry.active() && subscription->live &&
                !script->InstructionLimitExceeded()) {
                callback_ref = subscription->callback_ref;
                core_api = subscription->core_api;
                mod = subscription->mod;
            }
        }

        if (callback_ref != LUA_NOREF) {
            const int top = lua_gettop(state);
            // 4 slots: the protected function, its light-userdata argument,
            // and room for the pcall's error object. Answers rather than
            // raises, which is what makes this usable outside a frame.
            if (lua_checkstack(state, 4)) {
                DeliveryArgs args = {callback_ref, event};
                lua_pushcfunction(state, &DeliverProtected);
                lua_pushlightuserdata(state, &args);
                const LuaProtectedCallResult result =
                    script->ProtectedCall(state, 1, 0);
                if (result != LuaProtectedCallResult::kOk) {
                    // An ordinary Lua error is reported and leaves the
                    // subscription exactly as it was. An instruction-limit
                    // result is different: ProtectedCall has atomically
                    // faulted the script, later deliveries refuse it, and the
                    // main-thread frame pump will detach and destroy it after
                    // this Entry and this in-flight delivery have both ended.
                    //
                    // Read only when it already is a string. lua_tostring on
                    // anything else converts, and converting allocates and
                    // can raise - out here, where nothing catches it. See
                    // DeliverProtected's comment: error(42) is enough.
                    const char* detail =
                        result == LuaProtectedCallResult::kInstructionLimit
                            ? InstructionBudgetError()
                            : (lua_type(state, -1) == LUA_TSTRING
                                   ? lua_tostring(state, -1)
                                   : "<non-string error>");
                    LogFault(core_api, mod, event->topic, detail);
                }
            } else {
                LogFault(core_api, mod, event->topic,
                         "no room on the Lua stack to deliver it");
            }
            lua_settop(state, top);
        }
    }

    {
        std::lock_guard<std::mutex> guard(TableLock());
        --subscription->in_flight;
        // Whoever drops the count to zero on a subscription that is already
        // dead is the one that frees it: unsubscribe cannot wait for a
        // delivery (it runs holding the script lock that delivery is blocked
        // on), so it hands that job here.
        ReapLocked(subscription);
    }
    TableIdle().notify_all();
}

// ---------------------------------------------------------------------------
// The bindings
// ---------------------------------------------------------------------------

struct EventsContext {
    const WotbModV3EventsApiV1* api;
    const WotbModV3CoreApiV1* core_api;
    WotbModV3Handle mod;
    LuaScript* script;
};

// GuardedUpvalueIndex, not lua_upvalueindex: every slot in this file is
// installed behind the shared permission guard, which owns upvalue 1. See
// lua_permissions.h.
EventsContext* Context(lua_State* state) noexcept {
    return static_cast<EventsContext*>(
        lua_touserdata(state, GuardedUpvalueIndex(1)));
}

// The subscription handle a script holds is this host's own id, boxed under
// kHandleEventSubscription - not the client's WotbModV3EventToken. Two
// reasons, both real: the id is what finds the record, and the ABI makes no
// promise that a token is not reused after unsubscribe, so a script holding
// a stale subscription object could otherwise address a live subscription
// that merely inherited the number.
//
// Through rule 6's CheckArgHandle, like every other single-type handle
// argument in the host; the only thing either of these two wrappers still
// owns is the cast to the type this file actually indexes by.
bool CheckArgSubscription(lua_State* state, int index, uint32_t* out) noexcept {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgHandle(state, index, kHandleEventSubscription,
                        "a wotb.event_subscription handle", &handle)) {
        return false;
    }
    if (out) *out = static_cast<uint32_t>(handle);
    return true;
}

bool CheckArgDispatch(lua_State* state, int index,
                      WotbModV3Token* out) noexcept {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgHandle(state, index, kHandleEventDispatch,
                        "the dispatch token from an event", &handle)) {
        return false;
    }
    if (out) *out = static_cast<WotbModV3Token>(handle);
    return true;
}

// The five priorities WotbModV3EventPriority declares, and nothing else -
// checked by value through CheckArgEnum, the accessor storage's get_path had
// to hand-roll before rule 6 grew one.
constexpr lua_Integer kPriorityValues[] = {
    WOTBMOD_V3_EVENT_PRIORITY_LOWEST, WOTBMOD_V3_EVENT_PRIORITY_LOW,
    WOTBMOD_V3_EVENT_PRIORITY_NORMAL, WOTBMOD_V3_EVENT_PRIORITY_HIGH,
    WOTBMOD_V3_EVENT_PRIORITY_HIGHEST};

const EnumValues kPriority = Enum(
    kPriorityValues,
    "PRIORITY_LOWEST, PRIORITY_LOW, PRIORITY_NORMAL, PRIORITY_HIGH or "
    "PRIORITY_HIGHEST");

// post's flags are a union, not a set, so they go through CheckArgFlags: any
// combination of the four bits WotbModV3EventFlags declares is legal and a
// fifth bit is not.
constexpr lua_Integer kFlagsMask =
    WOTBMOD_V3_EVENT_FLAG_STOPPABLE | WOTBMOD_V3_EVENT_FLAG_MUTABLE_PAYLOAD |
    WOTBMOD_V3_EVENT_FLAG_SYSTEM | WOTBMOD_V3_EVENT_FLAG_COALESCIBLE;

// ---------------------------------------------------------------------------
// subscribe: the slot the rest of this file exists for.
// ---------------------------------------------------------------------------

// wotb.events.subscribe(pattern, handler [, priority [, receive_system]])
int EventsSubscribe(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    const char* pattern = nullptr;
    if (!CheckArgString(state, 1, &pattern)) return 2;
    if (!CheckArgFunction(state, 2)) return 2;
    lua_Integer priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    if (!CheckOptArgEnum(state, 3, kPriority, priority, &priority)) return 2;
    bool receive_system = false;
    if (!CheckOptArgBoolean(state, 4, false, &receive_system)) return 2;

    // Held in this state's registry, so the collector cannot take a handler
    // that only the client is holding a reference to. Done before anything
    // else that can fail, and released again on every path that does.
    lua_pushvalue(state, 2);
    const int callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);

    // new(std::nothrow) and a caught push_back: an allocation failure here is
    // a `nil, message` a script can handle, and a std::bad_alloc crossing a
    // lua_CFunction is a terminated process. Rule 5.
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
        lua_pushnil(state);
        lua_pushliteral(state,
                        "events.subscribe: ran out of memory recording the "
                        "subscription");
        return 2;
    }
    subscription->script = ctx->script;
    subscription->api = ctx->api;
    subscription->core_api = ctx->core_api;
    subscription->mod = ctx->mod;
    subscription->callback_ref = callback_ref;
    subscription->live = true;

    try {
        std::lock_guard<std::mutex> guard(TableLock());
        subscription->id = g_next_id;
        Table().push_back(subscription);
        ++g_next_id;
    } catch (...) {
        delete subscription;
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
        lua_pushnil(state);
        lua_pushliteral(state,
                        "events.subscribe: ran out of memory recording the "
                        "subscription");
        return 2;
    }

    WotbModV3EventSubscriptionInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_EVENTS_VERSION);
    info.topic_pattern = pattern;
    info.priority = static_cast<int32_t>(priority);
    info.receive_system_events = receive_system ? 1u : 0u;

    WotbModV3EventToken token = 0u;
    // The id crosses as the user_data, widened through uintptr_t rather than
    // handed over as a pointer into the table - see the file header.
    const WotbModV3Result result = ctx->api->subscribe(
        ctx->mod, &info, &DeliverEvent,
        reinterpret_cast<void*>(static_cast<uintptr_t>(subscription->id)),
        &token);

    if (result != WOTBMOD_V3_OK) {
        // Nothing was subscribed, so nothing can be delivered, so there is
        // no in-flight count to wait for: retire it here rather than leave a
        // record and a registry reference behind for a call that failed.
        {
            std::lock_guard<std::mutex> guard(TableLock());
            subscription->live = false;
            ReapLocked(subscription);
        }
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
        return PushResult(state, result, "events.subscribe");
    }

    uint32_t id = 0u;
    {
        std::lock_guard<std::mutex> guard(TableLock());
        subscription->token = token;
        id = subscription->id;
    }

    // The ownership ledger, and the one gate that makes a whole class of bug
    // unreachable rather than fixed in one place.
    //
    // The bug it closes: a handler is ordinary Lua code, and a handler running
    // in a delivery that ReleaseEventSubscriptions below is *waiting for* can
    // call subscribe. Step one has already gone past, so the record made here
    // would not be marked; step three only reaps records it marked, so the new
    // one would survive its own script - leaving the client holding a callback
    // into a freed LuaScript. Narrow, and a use-after-free in a DLL inside a
    // player's game.
    //
    // RevokeAll closes the ledger before that wait begins, so this record
    // fails and the subscription is taken straight back. Nothing here tests
    // for teardown itself: the ledger is the single place that knows, and any
    // future slot that mints something reachable from the client inherits the
    // same protection by asking it the same question.
    //
    // Retiring the record needs one thing the ordinary failure path above does
    // not: held_for_release cleared as well as live. Step one may already have
    // marked this record while it sat in the table waiting for its token, and
    // a record left marked would be unsubscribed a second time by step three -
    // against a token the ABI makes no promise about reusing.
    if (!ctx->script->Ownership().RecordSubscription(token)) {
        {
            std::lock_guard<std::mutex> guard(TableLock());
            subscription->live = false;
            subscription->held_for_release = false;
            subscription->callback_ref = LUA_NOREF;
            // Frees it unless a delivery claimed it in the moment it was live,
            // in which case that delivery frees it on its way out - and finds
            // live false, so it delivers nothing.
            ReapLocked(subscription);
        }
        ctx->api->unsubscribe(ctx->mod, token);
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
        lua_pushnil(state);
        lua_pushliteral(state,
                        "events.subscribe: this script is being unloaded");
        return 2;
    }

    // No lock and no destructor in scope from here on: everything below can
    // longjmp out on LUA_ERRMEM, and a lock_guard skipped by a longjmp is a
    // mutex nobody will ever release. Rule 5's own reason, applied to a lock
    // rather than to a std::string.
    //
    // PushToken, not PushHandle: the value boxed here is this host's own
    // subscription id, which makes no WOTBMOD_V3_INVALID_HANDLE-style
    // promise about the value 0. Today ids start at 1, so PushHandle would
    // behave identically - but that is a property of this file's counter,
    // not of the type, and the counter is exactly the sort of thing that
    // changes without anyone rereading this line.
    PushToken(state, static_cast<WotbModV3Token>(id),
              kHandleEventSubscription);
    return 1;
}

// ---------------------------------------------------------------------------
// unsubscribe, set_priority: the two slots that take a subscription back.
// ---------------------------------------------------------------------------

int EventsUnsubscribe(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    uint32_t id = 0u;
    if (!CheckArgSubscription(state, 1, &id)) return 2;

    WotbModV3EventToken token = 0u;
    int callback_ref = LUA_NOREF;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(TableLock());
        Subscription* subscription = FindLocked(id);
        if (subscription && subscription->live) {
            found = true;
            token = subscription->token;
            callback_ref = subscription->callback_ref;
            // Dead from here on: a delivery that has already claimed this
            // subscription re-reads `live` under this same lock once it has
            // the script lock - which this call holds - and gives up.
            subscription->live = false;
            subscription->callback_ref = LUA_NOREF;
            // Frees it only if no delivery is in flight; otherwise the
            // delivery frees it on its way out. Waiting here would deadlock
            // against a delivery blocked on the script lock this call holds.
            ReapLocked(subscription);
        }
    }
    if (!found) {
        // Already gone, or never ours. E_INVALID_HANDLE says that in the
        // convention's own words rather than inventing a sentence for it.
        return PushResult(state, WOTBMOD_V3_E_INVALID_HANDLE,
                          "events.unsubscribe");
    }
    // Or a long-running session leaks one registry slot per subscription.
    // Done with the script lock held, which is where every registry
    // operation on this state belongs.
    luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
    // The table lock is deliberately already released - see the scope above -
    // so this call is made holding none of the locks this file owns. It is
    // still made under the *script* lock, and that one cannot be dropped: a
    // lua_CFunction is by definition executing inside its own state, and
    // that state's lock is held for as long as it executes. So this call
    // site is the ABBA pair in the file header, and it is not fixable from
    // this side - unlike ReleaseEventSubscriptions, which is not inside a
    // script and therefore does have a choice. Recorded rather than papered
    // over: the constraint belongs to the client (a dispatcher lock must not
    // be held across a callback) and to the ABI's own documentation.
    const WotbModV3Result result = ctx->api->unsubscribe(ctx->mod, token);
    // Off the ownership ledger only when the client says it really let go -
    // the same rule ui.control_destroy and storage.commit follow, and now for
    // the same reason rather than a different one.
    //
    // This used to forget unconditionally, on the argument that this host had
    // called unsubscribe once and must never call it twice. That argument
    // protected the token at the cost of the subscription: a refused
    // unsubscribe (a permission the mod no longer has, a client that is busy)
    // left the client holding a live subscription for a script that is about
    // to die, permanently, because RevokeAll would never retry it. This
    // host's own record is already retired above, so nothing could be
    // delivered into the script either way - but "the client holds a
    // subscription nobody will ever revoke" is a leak, and leaks are what
    // this task is about.
    //
    // Keeping it is what makes the retry possible: the record is gone from
    // the table, so ReleaseEventSubscriptions will not find it, and the token
    // is left for the ownership registry's own sweep - which is the one path
    // that gives that sweep something to do. The residual risk is a client
    // that really did unsubscribe and reported a failure anyway, which would
    // see a second release; the ABI's own convention is that a call which
    // returns a failure did not do the thing, and one rule across all three
    // resource kinds is worth more than three separately-argued ones.
    if (result == WOTBMOD_V3_OK) {
        ctx->script->Ownership().ForgetSubscription(token);
    }
    return PushResult(state, result, "events.unsubscribe");
}

int EventsSetPriority(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    uint32_t id = 0u;
    if (!CheckArgSubscription(state, 1, &id)) return 2;
    lua_Integer priority = 0;
    if (!CheckArgEnum(state, 2, kPriority, &priority)) return 2;

    WotbModV3EventToken token = 0u;
    bool found = false;
    {
        std::lock_guard<std::mutex> guard(TableLock());
        Subscription* subscription = FindLocked(id);
        if (subscription && subscription->live) {
            found = true;
            token = subscription->token;
        }
    }
    if (!found) {
        return PushResult(state, WOTBMOD_V3_E_INVALID_HANDLE,
                          "events.set_priority");
    }
    return PushResult(
        state,
        ctx->api->set_priority(ctx->mod, token, static_cast<int32_t>(priority)),
        "events.set_priority");
}

// ---------------------------------------------------------------------------
// post: the other direction - a script publishing its own topic.
// ---------------------------------------------------------------------------

// wotb.events.post(topic [, payload [, flags]])
int EventsPost(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    const char* topic = nullptr;
    if (!CheckArgString(state, 1, &topic)) return 2;
    const char* payload = nullptr;
    size_t payload_size = 0u;
    // Bytes rather than a string: a payload is opaque to this host, so an
    // embedded zero in it is data.
    if (!CheckOptArgBytes(state, 2, nullptr, 0u, &payload, &payload_size)) {
        return 2;
    }
    lua_Integer flags = WOTBMOD_V3_EVENT_FLAG_NONE;
    if (!CheckOptArgFlags(state, 3, kFlagsMask,
                          "a mask of FLAG_STOPPABLE, FLAG_MUTABLE_PAYLOAD, "
                          "FLAG_SYSTEM and FLAG_COALESCIBLE",
                          flags, &flags)) {
        return 2;
    }
    return PushResult(
        state,
        ctx->api->post(ctx->mod, topic, payload,
                       static_cast<uint32_t>(payload_size),
                       static_cast<uint32_t>(flags)),
        "events.post");
}

// ---------------------------------------------------------------------------
// stop_propagation and the four dispatch readers. Each takes the dispatch
// token from the event table, which is why that token is boxed rather than
// numeric.
// ---------------------------------------------------------------------------

int EventsStopPropagation(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    WotbModV3Token dispatch = 0u;
    if (!CheckArgDispatch(state, 1, &dispatch)) return 2;
    return PushResult(state, ctx->api->stop_propagation(ctx->mod, dispatch),
                      "events.stop_propagation");
}

int EventsGetDispatchInfo(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    WotbModV3Token dispatch = 0u;
    if (!CheckArgDispatch(state, 1, &dispatch)) return 2;

    WotbModV3EventDispatchInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_EVENTS_VERSION);
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->get_dispatch_info(ctx->mod, dispatch, &info);

    lua_createtable(state, 0, 6);
    PushToken(state, info.dispatch_token, kHandleEventDispatch);
    lua_setfield(state, -2, "dispatch");
    lua_pushinteger(state, static_cast<lua_Integer>(info.timestamp_ns));
    lua_setfield(state, -2, "timestamp_ns");
    lua_pushinteger(state, static_cast<lua_Integer>(info.context_mask));
    lua_setfield(state, -2, "context_mask");
    lua_pushinteger(state, static_cast<lua_Integer>(info.thread_role));
    lua_setfield(state, -2, "thread_role");
    lua_pushinteger(state, static_cast<lua_Integer>(info.flags));
    lua_setfield(state, -2, "flags");
    lua_pushboolean(state, info.propagation_stopped != 0u);
    lua_setfield(state, -2, "propagation_stopped");
    return PushResultWith(state, result, mark, "events.get_dispatch_info");
}

int EventsGetThread(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    WotbModV3Token dispatch = 0u;
    if (!CheckArgDispatch(state, 1, &dispatch)) return 2;
    uint32_t thread_role = 0u;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->get_thread(ctx->mod, dispatch, &thread_role);
    // A bare integer, not the `true, value` shape: 0 is truthy in Lua, so a
    // number can be its own first return without ever reading as a failure
    // (rule 2). Only a boolean or an absent value needs wrapping.
    lua_pushinteger(state, static_cast<lua_Integer>(thread_role));
    return PushResultWith(state, result, mark, "events.get_thread");
}

int EventsGetTimestamp(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    WotbModV3Token dispatch = 0u;
    if (!CheckArgDispatch(state, 1, &dispatch)) return 2;
    uint64_t timestamp_ns = 0u;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->get_timestamp(ctx->mod, dispatch, &timestamp_ns);
    lua_pushinteger(state, static_cast<lua_Integer>(timestamp_ns));
    return PushResultWith(state, result, mark, "events.get_timestamp");
}

int EventsGetContext(lua_State* state) noexcept {
    EventsContext* ctx = Context(state);
    WotbModV3Token dispatch = 0u;
    if (!CheckArgDispatch(state, 1, &dispatch)) return 2;
    uint64_t context_mask = 0u;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->get_context(ctx->mod, dispatch, &context_mask);
    lua_pushinteger(state, static_cast<lua_Integer>(context_mask));
    return PushResultWith(state, result, mark, "events.get_context");
}

const luaL_Reg kEventsFuncs[] = {
    {"subscribe", &EventsSubscribe},
    {"unsubscribe", &EventsUnsubscribe},
    {"set_priority", &EventsSetPriority},
    {"post", &EventsPost},
    {"stop_propagation", &EventsStopPropagation},
    {"get_dispatch_info", &EventsGetDispatchInfo},
    {"get_thread", &EventsGetThread},
    {"get_timestamp", &EventsGetTimestamp},
    {"get_context", &EventsGetContext},
    {nullptr, nullptr},
};

struct NamedConstant {
    const char* name;
    lua_Integer value;
};

const NamedConstant kEventsConstants[] = {
    {"PRIORITY_LOWEST", WOTBMOD_V3_EVENT_PRIORITY_LOWEST},
    {"PRIORITY_LOW", WOTBMOD_V3_EVENT_PRIORITY_LOW},
    {"PRIORITY_NORMAL", WOTBMOD_V3_EVENT_PRIORITY_NORMAL},
    {"PRIORITY_HIGH", WOTBMOD_V3_EVENT_PRIORITY_HIGH},
    {"PRIORITY_HIGHEST", WOTBMOD_V3_EVENT_PRIORITY_HIGHEST},
    {"FLAG_NONE", WOTBMOD_V3_EVENT_FLAG_NONE},
    {"FLAG_STOPPABLE", WOTBMOD_V3_EVENT_FLAG_STOPPABLE},
    {"FLAG_MUTABLE_PAYLOAD", WOTBMOD_V3_EVENT_FLAG_MUTABLE_PAYLOAD},
    {"FLAG_SYSTEM", WOTBMOD_V3_EVENT_FLAG_SYSTEM},
    {"FLAG_COALESCIBLE", WOTBMOD_V3_EVENT_FLAG_COALESCIBLE},
    // The thread_role an event carries is a WotbModV3ThreadRole, and a
    // handler that behaves differently on the render thread should compare
    // against a name rather than against 2.
    {"THREAD_UNKNOWN", WOTBMOD_V3_THREAD_UNKNOWN},
    {"THREAD_MAIN", WOTBMOD_V3_THREAD_MAIN},
    {"THREAD_RENDER", WOTBMOD_V3_THREAD_RENDER},
    {"THREAD_AUDIO", WOTBMOD_V3_THREAD_AUDIO},
    {"THREAD_WORKER", WOTBMOD_V3_THREAD_WORKER},
    {"THREAD_IO", WOTBMOD_V3_THREAD_IO},
    {"TYPE_UI_SCREEN_CHANGED", WOTBMOD_V3_CLIENT_EVENT_UI_SCREEN_CHANGED},
    {"TYPE_SCENE_ACTIVATED", WOTBMOD_V3_CLIENT_EVENT_SCENE_ACTIVATED},
    {"TYPE_SCENE_DEACTIVATED", WOTBMOD_V3_CLIENT_EVENT_SCENE_DEACTIVATED},
    {"TYPE_UI_INPUT", WOTBMOD_V3_CLIENT_EVENT_UI_INPUT},
    {"TYPE_BATTLE_ENTERED", WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENTERED},
    {"TYPE_BATTLE_STARTED", WOTBMOD_V3_CLIENT_EVENT_BATTLE_STARTED},
    {"TYPE_BATTLE_ENDED", WOTBMOD_V3_CLIENT_EVENT_BATTLE_ENDED},
    {"TYPE_BATTLE_LEFT", WOTBMOD_V3_CLIENT_EVENT_BATTLE_LEFT},
    {"TYPE_LOCAL_VEHICLE_CHANGED", WOTBMOD_V3_CLIENT_EVENT_LOCAL_VEHICLE_CHANGED},
    {"TYPE_VEHICLE_SPAWNED", WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPAWNED},
    {"TYPE_VEHICLE_DESPAWNED", WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESPAWNED},
    {"TYPE_SHOT_FIRED", WOTBMOD_V3_CLIENT_EVENT_SHOT_FIRED},
    {"TYPE_SHELL_HIT", WOTBMOD_V3_CLIENT_EVENT_SHELL_HIT},
    {"TYPE_VEHICLE_HEALTH_CHANGED", WOTBMOD_V3_CLIENT_EVENT_VEHICLE_HEALTH_CHANGED},
    {"TYPE_VEHICLE_DAMAGED", WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DAMAGED},
    {"TYPE_VEHICLE_DESTROYED", WOTBMOD_V3_CLIENT_EVENT_VEHICLE_DESTROYED},
    {"TYPE_RELOAD_STATE_CHANGED", WOTBMOD_V3_CLIENT_EVENT_RELOAD_STATE_CHANGED},
    {"TYPE_AMMO_CHANGED", WOTBMOD_V3_CLIENT_EVENT_AMMO_CHANGED},
    {"TYPE_AIM_TARGET_CHANGED", WOTBMOD_V3_CLIENT_EVENT_AIM_TARGET_CHANGED},
    {"TYPE_VEHICLE_SPOTTED", WOTBMOD_V3_CLIENT_EVENT_VEHICLE_SPOTTED},
    {"TYPE_VEHICLE_UNSPOTTED", WOTBMOD_V3_CLIENT_EVENT_VEHICLE_UNSPOTTED},
    {"TYPE_CAMERA_MODE_CHANGED", WOTBMOD_V3_CLIENT_EVENT_CAMERA_MODE_CHANGED},
};

struct NamedStringConstant {
    const char* name;
    const char* value;
};

const NamedStringConstant kEventTopicConstants[] = {
    {"TOPIC_UI_SCREEN_CHANGED", WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED},
    {"TOPIC_UI_INPUT", WOTBMOD_V3_EVENT_UI_INPUT},
    {"TOPIC_SCENE_ACTIVATED", WOTBMOD_V3_EVENT_SCENE_ACTIVATED},
    {"TOPIC_SCENE_DEACTIVATED", WOTBMOD_V3_EVENT_SCENE_DEACTIVATED},
    {"TOPIC_BATTLE_ENTERED", WOTBMOD_V3_EVENT_BATTLE_ENTERED},
    {"TOPIC_BATTLE_STARTED", WOTBMOD_V3_EVENT_BATTLE_STARTED},
    {"TOPIC_BATTLE_ENDED", WOTBMOD_V3_EVENT_BATTLE_ENDED},
    {"TOPIC_BATTLE_LEFT", WOTBMOD_V3_EVENT_BATTLE_LEFT},
    {"TOPIC_LOCAL_VEHICLE_CHANGED", WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED},
    {"TOPIC_LOCAL_VEHICLE_CREATED", WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CREATED},
    {"TOPIC_LOCAL_VEHICLE_DESTROYED", WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED},
    {"TOPIC_VEHICLE_SPAWNED", WOTBMOD_V3_EVENT_VEHICLE_SPAWNED},
    {"TOPIC_VEHICLE_DESPAWNED", WOTBMOD_V3_EVENT_VEHICLE_DESPAWNED},
    {"TOPIC_VEHICLE_KILLED", WOTBMOD_V3_EVENT_VEHICLE_KILLED},
    {"TOPIC_SESSION_CLUSTER_CHANGED", WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED},
    {"TOPIC_SHOT_FIRED", WOTBMOD_V3_EVENT_SHOT_FIRED},
    {"TOPIC_SHELL_HIT", WOTBMOD_V3_EVENT_SHELL_HIT},
    {"TOPIC_VEHICLE_HEALTH_CHANGED", WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED},
    {"TOPIC_VEHICLE_DAMAGED", WOTBMOD_V3_EVENT_VEHICLE_DAMAGED},
    {"TOPIC_VEHICLE_DESTROYED", WOTBMOD_V3_EVENT_VEHICLE_DESTROYED},
    {"TOPIC_DAMAGE_RECEIVED", WOTBMOD_V3_EVENT_DAMAGE_RECEIVED},
    {"TOPIC_RELOAD_STATE_CHANGED", WOTBMOD_V3_EVENT_RELOAD_STATE_CHANGED},
    {"TOPIC_AMMO_CHANGED", WOTBMOD_V3_EVENT_AMMO_CHANGED},
    {"TOPIC_AIM_TARGET_CHANGED", WOTBMOD_V3_EVENT_AIM_TARGET_CHANGED},
    {"TOPIC_VEHICLE_SPOTTED", WOTBMOD_V3_EVENT_VEHICLE_SPOTTED},
    {"TOPIC_VEHICLE_UNSPOTTED", WOTBMOD_V3_EVENT_VEHICLE_UNSPOTTED},
    {"TOPIC_CAMERA_MODE_CHANGED", WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED},
    {"TOPIC_SNIPER_ENTERED", WOTBMOD_V3_EVENT_SNIPER_ENTERED},
    {"TOPIC_SNIPER_EXITED", WOTBMOD_V3_EVENT_SNIPER_EXITED},
    {"TOPIC_PUBLIC_ENTITY_ADDED", WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED},
    {"TOPIC_PUBLIC_ENTITY_UPDATED", WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED},
    {"TOPIC_PUBLIC_ENTITY_REMOVED", WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED},
    {"TOPIC_LOCAL_SHELL_FIRED", WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED},
    {"TOPIC_PROJECTILE_CREATED", WOTBMOD_V3_EVENT_PROJECTILE_CREATED},
    {"TOPIC_PROJECTILE_UPDATED", WOTBMOD_V3_EVENT_PROJECTILE_UPDATED},
    {"TOPIC_PROJECTILE_IMPACTED", WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED},
    {"TOPIC_PROJECTILE_DESTROYED", WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED},
    {"TOPIC_RPC_OBSERVED", WOTBMOD_V3_EVENT_RPC_OBSERVED},
};

}  // namespace

void RegisterEvents(lua_State* state, const WotbModV3EventsApiV1* api,
                    const WotbModV3CoreApiV1* core_api, WotbModV3Handle mod,
                    LuaScript* script) {
    // No script means nothing to deliver into: a subscription that cannot
    // name a state to re-enter is not a subscription, so the table is left
    // undefined rather than half-working, exactly as a null api leaves it.
    if (!state || !api || !script) return;

    PushWotbTable(state);                              // [wotb]
    lua_newtable(state);                               // [wotb, events]

    EventsContext* ctx = static_cast<EventsContext*>(
        lua_newuserdatauv(state, sizeof(EventsContext), 0));
    ctx->api = api;
    ctx->core_api = core_api;
    ctx->mod = mod;
    ctx->script = script;                               // [wotb, events, ctx]

    // Guarded, exactly as storage and ui are. subscribe is the slot that
    // matters most here: a delivery re-enters this state later, on whatever
    // thread the client dispatches on, so a subscription made without
    // events.public would be a fence breached long after the call that
    // breached it returned. Refusing at subscribe is refusing the delivery too.
    SetFuncsGuarded(state, kEventsFuncs, 1, script,
                    kPermissionNameEventsPublic);       // [wotb, events]

    for (const NamedConstant& constant : kEventsConstants) {
        lua_pushinteger(state, constant.value);
        lua_setfield(state, -2, constant.name);
    }
    for (const NamedStringConstant& constant : kEventTopicConstants) {
        lua_pushstring(state, constant.value);
        lua_setfield(state, -2, constant.name);
    }

    lua_setfield(state, -2, "events");                  // [wotb]
    lua_pop(state, 1);                                  // []
}

size_t EventSubscriptionRecordCount() {
    std::lock_guard<std::mutex> guard(TableLock());
    return Table().size();
}

void ReleaseEventSubscriptions(LuaScript* script) {
    if (!script) return;

    // Step one: nothing new. Marking dead under the table lock means no
    // delivery can claim this script from here on, because claiming happens
    // under the same lock.
    //
    // held_for_release is the marker, and the record's own place in the table
    // is the list - there is deliberately no vector of retiring records. One
    // used to be built here, and a throw from its push_back left the record
    // dead, unheld and unreachable by step three: nothing would ever have
    // reaped it, so a record leaked permanently rather than only its registry
    // slot. Marking a flag on a record that already exists cannot fail, so
    // that failure mode no longer has anywhere to live.
    {
        std::lock_guard<std::mutex> guard(TableLock());
        for (Subscription* entry : Table()) {
            if (entry->script == script && entry->live) {
                entry->live = false;
                entry->held_for_release = true;
            }
        }
    }

    // Step two: wait for whatever was already in flight. This runs with no
    // script lock held - ~LuaScript calls it before it takes its own lock -
    // so a delivery blocked on that lock can acquire it, finish, and drop the
    // count. Waiting while holding the lock would deadlock against exactly
    // the delivery being waited for.
    {
        std::unique_lock<std::mutex> guard(TableLock());
        TableIdle().wait(guard, [script]() {
            for (Subscription* entry : Table()) {
                if (entry->script == script && entry->in_flight > 0) {
                    return false;
                }
            }
            return true;
        });
    }

    // Step three: tell the client, release the registry references and free
    // the records. Safe to enter the state now: no delivery holds a pointer
    // to this script any more.
    //
    // One record at a time, each taken out of the table under the lock and
    // then acted on with nothing held. Two things fall out of that shape, and
    // both are the point rather than a side effect:
    //
    //   unsubscribe is called with no lock held at all - not the table's, and
    //   not the script's. This is the one path in this file that is not
    //   already running inside a script, so it is the one that can choose;
    //   calling the client from under the script lock is the ABBA pair
    //   described in the file header, and here it is avoidable.
    //
    //   The script lock is taken only around luaL_unref, which is the only
    //   part that touches the state.
    for (;;) {
        const WotbModV3EventsApiV1* api = nullptr;
        WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
        WotbModV3EventToken token = 0u;
        int callback_ref = LUA_NOREF;
        {
            std::lock_guard<std::mutex> guard(TableLock());
            Subscription* subscription = nullptr;
            for (Subscription* entry : Table()) {
                if (entry->script == script && entry->held_for_release) {
                    subscription = entry;
                    break;
                }
            }
            if (!subscription) break;
            api = subscription->api;
            mod = subscription->mod;
            token = subscription->token;
            callback_ref = subscription->callback_ref;
            subscription->callback_ref = LUA_NOREF;
            subscription->held_for_release = false;
            // in_flight is zero by now - step two waited for it - and live is
            // false, so this frees the record. Everything still needed below
            // has already been copied out.
            ReapLocked(subscription);
        }
        // The same rule as events.unsubscribe above and as the one
        // lua_ownership.h states for all three resource kinds: off the ledger
        // only when the client says it really let go.
        //
        // This used to forget unconditionally, and the comment here argued
        // that pairing the forget with the call kept the registry's sweep an
        // unconditional backstop that could never double-release. It does do
        // that - but at the price the rule was written to refuse. A client
        // that answers this unsubscribe with anything but OK still holds a
        // live subscription into a script that is being destroyed, and
        // forgetting the token here is what guarantees nothing ever asks
        // again: the record is already out of the table, so this loop will not
        // see it a second time, and the sweep would have been the only thing
        // left that could. Two files described that retry as the sweep's whole
        // reason to exist (lua_ownership.h's ForgetSubscription note, and
        // RevokeAll's own comment on the leftovers) while this line quietly
        // guaranteed the sweep never had anything to do.
        //
        // The double-release the old shape was protecting against is bounded
        // and already accepted everywhere else: it needs a client that
        // released the subscription and *reported a failure anyway*, against
        // the ABI's own convention that a call returning a failure did not do
        // the thing. One rule across three kinds beats three separately
        // argued ones.
        if (api && api->unsubscribe(mod, token) == WOTBMOD_V3_OK) {
            script->Ownership().ForgetSubscription(token);
        }
        if (callback_ref != LUA_NOREF) {
            // An accident, not a contract, and stated here because this is
            // where an optimiser stands. This Entry takes the script lock,
            // which a parked delivery holds - so this line *also* blocks until
            // that delivery leaves the state, and step two's wait can be
            // deleted without any test going red. Nobody may rely on that.
            // The wait is the barrier; a future release path that stops
            // needing a registry reference, or releases it without entering
            // the state, would take this second mechanism away and leave the
            // wait carrying the ordering rule alone - which is what it was
            // written to do. See step two.
            LuaScript::Entry entry(*script);
            luaL_unref(entry.state(), LUA_REGISTRYINDEX, callback_ref);
        }
    }
}

}  // namespace lua
}  // namespace wotbmod
