// wotb.ges - the GES::GameEventSystem bus (include/wotbmod/ges_v1.h).
//
// Subscriptions ride on wotb.events. ges.subscribe(pattern, fn) calls the
// installed wotb.events.subscribe with the topic "wotbmod.ges.<pattern>",
// receive_system_events = true, and a C trampoline in place of fn. A GES
// subscription therefore *is* an events subscription: the same record table,
// the same delivery lock, the same fault logging, the same ownership ledger
// and the same release when the script dies. lua_bind_events.cpp owns all of
// that and this file adds nothing to it. wotb.events.subscribe is reached
// through the wotb table, guard included, so a script needs events.public as
// well as ges.observe to subscribe: the delivery it asks for is an events
// delivery.
//
// The trampoline is the only place a GesEvent userdata is made. The runtime
// hands every subscriber a copy of WotbModV3GesEvent as the event payload,
// and its read_* slots identify the delivery by the *payload* pointer inside
// that struct (src/v3/ges_services.cpp), so the trampoline rebuilds the
// struct from e.payload into the userdata, marks it live, calls fn(ev) and
// marks it dead on the way out - whether fn returned or raised. A stashed ev
// answers every later read with "ges event expired". The runtime refuses such
// a read on its own as well, so the userdata is a second fence, not the only
// one.
//
// Stack and error conventions follow lua_convert.h: ABI failures come back as
// nil, message through PushResult; a wrong self or an expired event object is
// a programming error and raises.
#include "lua_bindings.h"
#include "lua_convert.h"
#include "lua_permissions.h"
#include "lua_script.h"
#include "../../include/wotbmod/events_v1.h"
#include "../../include/wotbmod/ges_v1.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wotbmod {
namespace lua {
namespace {

struct GesContext {
    const WotbModV3GesApiV1* api;
    const WotbModV3CoreApiV1* core_api;
    WotbModV3Handle mod;
    LuaScript* script;
};

// GuardedUpvalueIndex, not lua_upvalueindex: every slot in this file is
// installed behind the shared permission guard, which owns upvalue 1. See
// lua_permissions.h. The trampoline is the one exception and says so.
GesContext* Context(lua_State* state) noexcept {
    return static_cast<GesContext*>(
        lua_touserdata(state, GuardedUpvalueIndex(1)));
}

struct GesEventBox {
    WotbModV3GesEvent event;
    const WotbModV3GesApiV1* api;
    bool live;
};

constexpr char kGesEventMeta[] = "wotbmod.ges.event";
constexpr char kTopicPrefix[] = WOTBMOD_V3_GES_TOPIC_PREFIX;
constexpr size_t kTopicCapacity =
    sizeof(kTopicPrefix) + WOTBMOD_V3_GES_TYPE_NAME_SIZE;
constexpr uint32_t kStringCapacity = 256u;
constexpr uint32_t kMaxSchemaFields = 256u;

// "Avatar::CameraModeChanged" and "Avatar.CameraModeChanged" both name the
// topic wotbmod.ges.Avatar.CameraModeChanged, and "Avatar::*" / "Avatar.*"
// both name the pattern under it - the runtime spells a type's topic with
// dots (GesTopicFromTypeName), a script may keep the C++ spelling. False when
// the result is empty or would not fit.
bool BuildTopic(const char* pattern, char* out, size_t capacity) {
    const size_t prefix = std::strlen(kTopicPrefix);
    if (prefix >= capacity) return false;
    std::memcpy(out, kTopicPrefix, prefix);
    size_t n = prefix;
    for (const char* p = pattern; *p != '\0'; ++p) {
        if (n + 1u >= capacity) return false;
        if (p[0] == ':' && p[1] == ':') {
            out[n++] = '.';
            ++p;
            continue;
        }
        out[n++] = *p;
    }
    out[n] = '\0';
    return n > prefix;
}

// ---------------------------------------------------------------------------
// The event object
// ---------------------------------------------------------------------------

GesEventBox* CheckBox(lua_State* state) {
    return static_cast<GesEventBox*>(
        luaL_checkudata(state, 1, kGesEventMeta));
}

GesEventBox* CheckLiveBox(lua_State* state) {
    GesEventBox* box = CheckBox(state);
    if (!box->live) luaL_error(state, "ges event expired");
    return box;
}

// One typed read through the ABI. kind is a WotbModV3GesFieldKind; the two
// kinds Lua cannot hold (FASTNAME, BYTES) answer nil, message rather than a
// guess at their layout.
int PushRead(lua_State* state, GesEventBox* box, uint32_t kind,
             uint32_t offset, const char* context) {
    const WotbModV3GesApiV1* api = box->api;
    const WotbModV3GesEvent* event = &box->event;
    WotbModV3Result result = WOTBMOD_V3_E_NOT_SUPPORTED;
    switch (kind) {
        case WOTBMOD_V3_GES_FIELD_I32: {
            int32_t value = 0;
            result = api->read_i32(event, offset, &value);
            if (result == WOTBMOD_V3_OK) {
                lua_pushinteger(state, static_cast<lua_Integer>(value));
                return 1;
            }
            break;
        }
        case WOTBMOD_V3_GES_FIELD_U32: {
            uint32_t value = 0u;
            result = api->read_u32(event, offset, &value);
            if (result == WOTBMOD_V3_OK) {
                lua_pushinteger(state, static_cast<lua_Integer>(value));
                return 1;
            }
            break;
        }
        case WOTBMOD_V3_GES_FIELD_F32: {
            float value = 0.0f;
            result = api->read_f32(event, offset, &value);
            if (result == WOTBMOD_V3_OK) {
                lua_pushnumber(state, static_cast<lua_Number>(value));
                return 1;
            }
            break;
        }
        case WOTBMOD_V3_GES_FIELD_BOOL: {
            uint8_t value = 0u;
            result = api->read_bool(event, offset, &value);
            if (result == WOTBMOD_V3_OK) {
                lua_pushboolean(state, value != 0u);
                return 1;
            }
            break;
        }
        case WOTBMOD_V3_GES_FIELD_U8: {
            uint8_t value = 0u;
            result = api->read_bool(event, offset, &value);
            if (result == WOTBMOD_V3_OK) {
                lua_pushinteger(state, static_cast<lua_Integer>(value));
                return 1;
            }
            break;
        }
        case WOTBMOD_V3_GES_FIELD_PTR: {
            const void* value = nullptr;
            result = api->read_ptr(event, offset, &value);
            if (result == WOTBMOD_V3_OK) {
                lua_pushinteger(state, static_cast<lua_Integer>(
                                           reinterpret_cast<uintptr_t>(value)));
                return 1;
            }
            break;
        }
        case WOTBMOD_V3_GES_FIELD_CSTR: {
            char buffer[kStringCapacity] = {};
            result = api->read_cstring(event, offset, buffer, kStringCapacity);
            if (result == WOTBMOD_V3_OK) {
                buffer[kStringCapacity - 1u] = '\0';
                lua_pushstring(state, buffer);
                return 1;
            }
            break;
        }
        default:
            lua_pushnil(state);
            lua_pushliteral(state, "field kind not readable from Lua");
            return 2;
    }
    return PushResult(state, result, context);
}

int ReadAt(lua_State* state, uint32_t kind, const char* context) {
    GesEventBox* box = CheckLiveBox(state);
    lua_Integer offset = 0;
    if (!CheckArgInteger(state, 2, &offset)) return 2;
    if (offset < 0 || offset > 0x7FFFFFFF) {
        lua_pushnil(state);
        lua_pushliteral(state, "offset out of range");
        return 2;
    }
    return PushRead(state, box, kind, static_cast<uint32_t>(offset), context);
}

int EvI32(lua_State* state) {
    return ReadAt(state, WOTBMOD_V3_GES_FIELD_I32, "ges.event.i32");
}
int EvU32(lua_State* state) {
    return ReadAt(state, WOTBMOD_V3_GES_FIELD_U32, "ges.event.u32");
}
int EvF32(lua_State* state) {
    return ReadAt(state, WOTBMOD_V3_GES_FIELD_F32, "ges.event.f32");
}
int EvBool(lua_State* state) {
    return ReadAt(state, WOTBMOD_V3_GES_FIELD_BOOL, "ges.event.bool");
}
int EvPtr(lua_State* state) {
    return ReadAt(state, WOTBMOD_V3_GES_FIELD_PTR, "ges.event.ptr");
}
int EvStr(lua_State* state) {
    return ReadAt(state, WOTBMOD_V3_GES_FIELD_CSTR, "ges.event.str");
}

// ev:field(name) - a read at the offset and kind the schema gives the field.
int EvField(lua_State* state) {
    GesEventBox* box = CheckLiveBox(state);
    const char* name = nullptr;
    if (!CheckArgString(state, 2, &name)) return 2;
    const uint32_t id = box->event.schema_id;
    if (id == 0u) {
        lua_pushnil(state);
        lua_pushliteral(state, "no schema");
        return 2;
    }
    for (uint32_t i = 0u; i < kMaxSchemaFields; ++i) {
        WotbModV3GesField field = {};
        WOTBMOD_V3_INIT_STRUCT(field, WOTBMOD_V3_GES_VERSION);
        if (box->api->schema_field(id, i, &field) != WOTBMOD_V3_OK) break;
        if (!field.name || std::strcmp(field.name, name) != 0) continue;
        return PushRead(state, box, field.kind, field.offset,
                        "ges.event.field");
    }
    lua_pushnil(state);
    lua_pushliteral(state, "unknown field");
    return 2;
}

int EvExpired(lua_State* state) {
    GesEventBox* box = CheckBox(state);
    lua_pushboolean(state, !box->live);
    return 1;
}

int EvToString(lua_State* state) {
    GesEventBox* box = CheckBox(state);
    lua_pushfstring(state, "wotb.ges.event(%s%s)", box->event.type_name,
                    box->live ? "" : ", expired");
    return 1;
}

const luaL_Reg kGesEventMethods[] = {
    {"i32", &EvI32},
    {"u32", &EvU32},
    {"f32", &EvF32},
    {"bool", &EvBool},
    {"ptr", &EvPtr},
    {"str", &EvStr},
    {"field", &EvField},
    {"expired", &EvExpired},
    {nullptr, nullptr},
};

// __index: the view table (type, size, schema, publisher_rva, flags,
// schema_id) held as the userdata's user value first, the methods second.
// Upvalue 1 is the methods table.
int EvIndex(lua_State* state) {
    CheckBox(state);
    lua_getiuservalue(state, 1, 1);          // [ud, key, view]
    if (lua_type(state, -1) == LUA_TTABLE) {
        lua_pushvalue(state, 2);
        lua_rawget(state, -2);               // [ud, key, view, value]
        if (!lua_isnil(state, -1)) return 1;
        lua_pop(state, 1);
    }
    lua_pop(state, 1);                       // [ud, key]
    lua_pushvalue(state, 2);
    lua_rawget(state, lua_upvalueindex(1));  // [ud, key, method-or-nil]
    return 1;
}

// The read-only view a handler sees as plain fields. schema is an array of
// {name, offset, kind, size} plus id and size, or absent when the runtime
// knows none.
void PushEventView(lua_State* state, const WotbModV3GesApiV1* api,
                   const WotbModV3GesEvent* event) {
    lua_createtable(state, 0, 6);
    lua_pushstring(state, event->type_name);
    lua_setfield(state, -2, "type");
    lua_pushinteger(state, static_cast<lua_Integer>(event->payload_size));
    lua_setfield(state, -2, "size");
    lua_pushinteger(state, static_cast<lua_Integer>(event->publisher_rva));
    lua_setfield(state, -2, "publisher_rva");
    lua_pushinteger(state, static_cast<lua_Integer>(event->flags));
    lua_setfield(state, -2, "flags");
    lua_pushinteger(state, static_cast<lua_Integer>(event->schema_id));
    lua_setfield(state, -2, "schema_id");
    if (event->schema_id == 0u) return;

    lua_newtable(state);                     // [view, schema]
    lua_pushinteger(state, static_cast<lua_Integer>(event->schema_id));
    lua_setfield(state, -2, "id");
    lua_pushinteger(state, static_cast<lua_Integer>(event->payload_size));
    lua_setfield(state, -2, "size");
    for (uint32_t i = 0u; i < kMaxSchemaFields; ++i) {
        WotbModV3GesField field = {};
        WOTBMOD_V3_INIT_STRUCT(field, WOTBMOD_V3_GES_VERSION);
        if (api->schema_field(event->schema_id, i, &field) != WOTBMOD_V3_OK) {
            break;
        }
        lua_createtable(state, 0, 4);
        lua_pushstring(state, field.name ? field.name : "");
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, static_cast<lua_Integer>(field.offset));
        lua_setfield(state, -2, "offset");
        lua_pushinteger(state, static_cast<lua_Integer>(field.kind));
        lua_setfield(state, -2, "kind");
        lua_pushinteger(state, static_cast<lua_Integer>(field.size));
        lua_setfield(state, -2, "size");
        lua_rawseti(state, -2, static_cast<lua_Integer>(i) + 1);
    }
    lua_setfield(state, -2, "schema");       // [view]
}

// The function wotb.events.subscribe actually holds. Called by
// lua_bind_events.cpp's delivery with the event table; upvalue 1 is the
// script's handler, upvalue 2 the GesContext as a light userdata - plain
// lua_upvalueindex here, this closure was never installed behind the guard
// (the guard on ges.subscribe already answered before it existed).
//
// A delivery on the topic that does not carry a WotbModV3GesEvent of the
// version this file knows is dropped, not guessed at.
int GesTrampoline(lua_State* state) {
    GesContext* ctx = static_cast<GesContext*>(
        lua_touserdata(state, lua_upvalueindex(2)));
    if (!ctx || !ctx->api || lua_type(state, 1) != LUA_TTABLE) return 0;

    lua_getfield(state, 1, "payload");
    size_t size = 0u;
    const char* bytes = lua_type(state, -1) == LUA_TSTRING
                            ? lua_tolstring(state, -1, &size)
                            : nullptr;
    if (!bytes || size < sizeof(WotbModV3GesEvent)) {
        lua_pop(state, 1);
        return 0;
    }
    WotbModV3GesEvent event = {};
    std::memcpy(&event, bytes, sizeof(event));
    lua_pop(state, 1);
    if (event.struct_size < sizeof(WotbModV3GesEvent) ||
        event.api_version != WOTBMOD_V3_GES_VERSION) {
        return 0;
    }
    event.type_name[WOTBMOD_V3_GES_TYPE_NAME_SIZE - 1u] = '\0';

    GesEventBox* box = static_cast<GesEventBox*>(
        lua_newuserdatauv(state, sizeof(GesEventBox), 1));   // [e, box]
    box->event = event;
    box->api = ctx->api;
    box->live = true;
    luaL_setmetatable(state, kGesEventMeta);
    PushEventView(state, ctx->api, &box->event);           // [e, box, view]
    lua_setiuservalue(state, -2, 1);                       // [e, box]

    lua_pushvalue(state, lua_upvalueindex(1));             // [e, box, fn]
    lua_pushvalue(state, -2);                              // [e, box, fn, box]
    const int status = lua_pcall(state, 1, 0, 0);          // [e, box(, err)]
    // Dead before anything else happens, on both paths: this is the one
    // moment that knows the delivery is over.
    box->live = false;
    if (status != LUA_OK) return lua_error(state);
    return 0;
}

// ---------------------------------------------------------------------------
// The bindings
// ---------------------------------------------------------------------------

// Pushes wotb.events.<name> or nil.
void PushEventsSlot(lua_State* state, const char* name) {
    PushWotbTable(state);                                  // [wotb]
    lua_getfield(state, -1, "events");                     // [wotb, events]
    if (lua_type(state, -1) == LUA_TTABLE) {
        lua_getfield(state, -1, name);                     // [wotb, events, f]
    } else {
        lua_pushnil(state);
    }
    lua_replace(state, -3);                                // [f, events]
    lua_pop(state, 1);                                     // [f]
}

// ges.subscribe(pattern, fn) -> subscription | nil, message
int GesSubscribe(lua_State* state) {
    GesContext* ctx = Context(state);
    const char* pattern = nullptr;
    if (!CheckArgString(state, 1, &pattern)) return 2;
    if (!CheckArgFunction(state, 2)) return 2;
    char topic[kTopicCapacity] = {};
    if (!BuildTopic(pattern, topic, sizeof(topic))) {
        lua_pushnil(state);
        lua_pushliteral(state, "ges.subscribe: pattern is empty or too long");
        return 2;
    }
    lua_settop(state, 2);
    PushEventsSlot(state, "subscribe");                    // [p, fn, sub]
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        return PushResult(state, WOTBMOD_V3_E_NOT_SUPPORTED, "ges.subscribe");
    }
    lua_pushstring(state, topic);
    lua_pushvalue(state, 2);
    lua_pushlightuserdata(state, ctx);
    lua_pushcclosure(state, &GesTrampoline, 2);
    lua_pushinteger(state, WOTBMOD_V3_EVENT_PRIORITY_NORMAL);
    lua_pushboolean(state, 1);                             // system events
    lua_call(state, 4, 2);                                 // [p, fn, r1, r2]
    if (lua_isnil(state, -2)) return 2;
    lua_pop(state, 1);
    return 1;
}

// ges.unsubscribe(subscription) -> whatever events.unsubscribe answers
int GesUnsubscribe(lua_State* state) {
    lua_settop(state, 1);
    PushEventsSlot(state, "unsubscribe");                  // [sub, f]
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        lua_pop(state, 1);
        return PushResult(state, WOTBMOD_V3_E_NOT_SUPPORTED,
                          "ges.unsubscribe");
    }
    lua_pushvalue(state, 1);                               // [sub, f, sub]
    lua_call(state, 1, LUA_MULTRET);                       // [sub, results...]
    return lua_gettop(state) - 1;
}

// ges.types() -> array of "Owner::Name"
int GesTypes(lua_State* state) {
    GesContext* ctx = Context(state);
    uint32_t count = 0u;
    WotbModV3Result result = ctx->api->list_types(ctx->mod, nullptr, 0u, &count);
    if (result != WOTBMOD_V3_OK && result != WOTBMOD_V3_E_BUFFER_TOO_SMALL) {
        return PushResult(state, result, "ges.types");
    }
    std::vector<const char*> names(count ? count : 1u, nullptr);
    if (count != 0u) {
        result = ctx->api->list_types(ctx->mod, names.data(), count, &count);
        if (result != WOTBMOD_V3_OK) return PushResult(state, result, "ges.types");
        if (count > names.size()) count = static_cast<uint32_t>(names.size());
    }
    lua_createtable(state, static_cast<int>(count), 0);
    for (uint32_t i = 0u; i < count; ++i) {
        lua_pushstring(state, names[i] ? names[i] : "");
        lua_rawseti(state, -2, static_cast<lua_Integer>(i) + 1);
    }
    return 1;
}

// ges.schema(type_name) -> { id, size, field_count, fields = { {name,
// offset, kind, size}, ... } } | nil, message
//
// The two ABI slots (get_schema, schema_field) a handler already sees as
// ev.schema, reachable without waiting for a delivery: an author can check
// what a type carries before subscribing. The spelling is the one types()
// lists ("Avatar::CameraModeChanged"); the runtime owns the lookup.
int GesSchema(lua_State* state) {
    GesContext* ctx = Context(state);
    const char* name = nullptr;
    if (!CheckArgString(state, 1, &name)) return 2;
    uint32_t id = 0u;
    uint32_t size = 0u;
    uint32_t count = 0u;
    const WotbModV3Result result =
        ctx->api->get_schema(name, &id, &size, &count);
    if (result != WOTBMOD_V3_OK) return PushResult(state, result, "ges.schema");
    lua_createtable(state, 0, 4);                            // [schema]
    lua_pushinteger(state, static_cast<lua_Integer>(id));
    lua_setfield(state, -2, "id");
    lua_pushinteger(state, static_cast<lua_Integer>(size));
    lua_setfield(state, -2, "size");
    lua_pushinteger(state, static_cast<lua_Integer>(count));
    lua_setfield(state, -2, "field_count");
    lua_createtable(state, static_cast<int>(count), 0);      // [schema, fields]
    for (uint32_t i = 0u; i < count && i < kMaxSchemaFields; ++i) {
        WotbModV3GesField field = {};
        WOTBMOD_V3_INIT_STRUCT(field, WOTBMOD_V3_GES_VERSION);
        if (ctx->api->schema_field(id, i, &field) != WOTBMOD_V3_OK) break;
        lua_createtable(state, 0, 4);
        lua_pushstring(state, field.name ? field.name : "");
        lua_setfield(state, -2, "name");
        lua_pushinteger(state, static_cast<lua_Integer>(field.offset));
        lua_setfield(state, -2, "offset");
        lua_pushinteger(state, static_cast<lua_Integer>(field.kind));
        lua_setfield(state, -2, "kind");
        lua_pushinteger(state, static_cast<lua_Integer>(field.size));
        lua_setfield(state, -2, "size");
        lua_rawseti(state, -2, static_cast<lua_Integer>(i) + 1);
    }
    lua_setfield(state, -2, "fields");                       // [schema]
    return 1;
}

// ges.publish(type_name, fields[, flags]) -> true | nil, message
//
// The payload is laid out from the schema: every schema field must be
// present in the table and of a kind Lua can encode (I32/U32/F32/BOOL/PTR).
// A type without a schema cannot be published from Lua at all - there is no
// way to say what the bytes are.
int GesPublish(lua_State* state) {
    GesContext* ctx = Context(state);
    const char* type_name = nullptr;
    if (!CheckArgString(state, 1, &type_name)) return 2;
    if (lua_type(state, 2) != LUA_TTABLE) {
        lua_pushnil(state);
        lua_pushliteral(state, "ges.publish: expected a table of fields");
        return 2;
    }
    lua_Integer flags = 0;
    if (!CheckOptArgInteger(state, 3, 0, &flags)) return 2;

    uint32_t id = 0u;
    uint32_t size = 0u;
    uint32_t field_count = 0u;
    WotbModV3Result result =
        ctx->api->get_schema(type_name, &id, &size, &field_count);
    if (result != WOTBMOD_V3_OK) return PushResult(state, result, "ges.publish");
    if (size == 0u || size > 0x10000u) {
        lua_pushnil(state);
        lua_pushliteral(state, "ges.publish: schema has no payload size");
        return 2;
    }
    std::vector<uint8_t> buffer(size, 0u);
    for (uint32_t i = 0u; i < field_count; ++i) {
        WotbModV3GesField field = {};
        WOTBMOD_V3_INIT_STRUCT(field, WOTBMOD_V3_GES_VERSION);
        result = ctx->api->schema_field(id, i, &field);
        if (result != WOTBMOD_V3_OK) {
            return PushResult(state, result, "ges.publish");
        }
        if (!field.name || field.offset > size || field.size > size - field.offset) {
            lua_pushnil(state);
            lua_pushliteral(state, "ges.publish: schema field out of range");
            return 2;
        }
        lua_getfield(state, 2, field.name);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            lua_pushnil(state);
            lua_pushfstring(state, "ges.publish: missing field '%s'", field.name);
            return 2;
        }
        uint8_t* at = buffer.data() + field.offset;
        int is_number = 0;
        switch (field.kind) {
            case WOTBMOD_V3_GES_FIELD_I32: {
                const lua_Integer v = lua_tointegerx(state, -1, &is_number);
                const int32_t w = static_cast<int32_t>(v);
                if (is_number && field.size >= sizeof(w)) std::memcpy(at, &w, sizeof(w));
                break;
            }
            case WOTBMOD_V3_GES_FIELD_U32: {
                const lua_Integer v = lua_tointegerx(state, -1, &is_number);
                const uint32_t w = static_cast<uint32_t>(v);
                if (is_number && field.size >= sizeof(w)) std::memcpy(at, &w, sizeof(w));
                break;
            }
            case WOTBMOD_V3_GES_FIELD_F32: {
                const lua_Number v = lua_tonumberx(state, -1, &is_number);
                const float w = static_cast<float>(v);
                if (is_number && field.size >= sizeof(w)) std::memcpy(at, &w, sizeof(w));
                break;
            }
            case WOTBMOD_V3_GES_FIELD_BOOL: {
                is_number = lua_isboolean(state, -1);
                const uint8_t w = lua_toboolean(state, -1) ? 1u : 0u;
                if (is_number && field.size >= sizeof(w)) std::memcpy(at, &w, sizeof(w));
                break;
            }
            case WOTBMOD_V3_GES_FIELD_U8: {
                const lua_Integer v = lua_tointegerx(state, -1, &is_number);
                const uint8_t w = static_cast<uint8_t>(v);
                if (is_number && (v < 0 || v > 255)) is_number = 0;
                if (is_number && field.size >= sizeof(w)) std::memcpy(at, &w, sizeof(w));
                break;
            }
            case WOTBMOD_V3_GES_FIELD_PTR: {
                const lua_Integer v = lua_tointegerx(state, -1, &is_number);
                const uintptr_t w = static_cast<uintptr_t>(v);
                if (is_number && field.size >= sizeof(w)) std::memcpy(at, &w, sizeof(w));
                break;
            }
            default:
                lua_pop(state, 1);
                lua_pushnil(state);
                lua_pushfstring(state,
                                "ges.publish: field '%s' has a kind not "
                                "encodable from Lua",
                                field.name);
                return 2;
        }
        lua_pop(state, 1);
        if (!is_number) {
            lua_pushnil(state);
            lua_pushfstring(state, "ges.publish: field '%s' has the wrong type",
                            field.name);
            return 2;
        }
    }
    result = ctx->api->publish(ctx->mod, type_name, buffer.data(), size,
                               static_cast<uint32_t>(flags));
    if (result != WOTBMOD_V3_OK) return PushResult(state, result, "ges.publish");
    lua_pushboolean(state, 1);
    return 1;
}

const luaL_Reg kGesObserveFuncs[] = {
    {"types", &GesTypes},
    {"subscribe", &GesSubscribe},
    {"unsubscribe", &GesUnsubscribe},
    {"schema", &GesSchema},
    {nullptr, nullptr},
};

const luaL_Reg kGesPublishFuncs[] = {
    {"publish", &GesPublish},
    {nullptr, nullptr},
};

}  // namespace

void RegisterGes(lua_State* state, const WotbModV3GesApiV1* api,
                 const WotbModV3EventsApiV1* events_api,
                 const WotbModV3CoreApiV1* core_api, WotbModV3Handle mod,
                 LuaScript* script) {
    // Same rule as events: no script, no state to deliver into, no table.
    // events_api is what subscribe rides on; without it wotb.events does not
    // exist either, so the whole table stays absent rather than half-present.
    if (!state || !api || !events_api || !script) return;

    if (luaL_newmetatable(state, kGesEventMeta)) {         // [meta]
        lua_createtable(state, 0, 8);                       // [meta, methods]
        luaL_setfuncs(state, kGesEventMethods, 0);
        lua_pushcclosure(state, &EvIndex, 1);               // [meta, __index]
        lua_setfield(state, -2, "__index");
        lua_pushcfunction(state, &EvToString);
        lua_setfield(state, -2, "__tostring");
        lua_pushliteral(state, "locked");
        lua_setfield(state, -2, "__metatable");
    }
    lua_pop(state, 1);                                      // []

    PushWotbTable(state);                                   // [wotb]
    lua_newtable(state);                                    // [wotb, ges]

    GesContext* ctx = static_cast<GesContext*>(
        lua_newuserdatauv(state, sizeof(GesContext), 0));
    ctx->api = api;
    ctx->core_api = core_api;
    ctx->mod = mod;
    ctx->script = script;                                   // [wotb, ges, ctx]

    // Two fences on one table: observing (types, subscribe, unsubscribe) is
    // SAFE-tier ges.observe, publishing is REVIEWED-tier ges.publish -
    // exactly the split src/v3/wotb_mod_v3_runtime.cpp's kPermissions makes.
    // SetFuncsGuarded wants the table directly under its nup upvalues and
    // pops them, so the spare ctx is parked below the table between calls.
    lua_pushvalue(state, -1);                               // [wotb, ges, ctx, ctx]
    lua_insert(state, -3);                                  // [wotb, ctx, ges, ctx]
    SetFuncsGuarded(state, kGesObserveFuncs, 1, script,
                    kPermissionNameGesObserve);             // [wotb, ctx, ges]
    lua_pushvalue(state, -2);                               // [wotb, ctx, ges, ctx]
    SetFuncsGuarded(state, kGesPublishFuncs, 1, script,
                    kPermissionNameGesPublish);             // [wotb, ctx, ges]
    lua_remove(state, -2);                                  // [wotb, ges]

    lua_pushinteger(state, WOTBMOD_V3_GES_PUBLISH_ECHO);
    lua_setfield(state, -2, "PUBLISH_ECHO");

    lua_setfield(state, -2, "ges");                         // [wotb]
    lua_pop(state, 1);                                      // []
}

}  // namespace lua
}  // namespace wotbmod
