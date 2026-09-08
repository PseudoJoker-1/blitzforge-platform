#include "lua_permissions.h"

#include "../../include/wotbmod/interface_ids.h"
#include "../../include/wotbmod/permissions_v1.h"
#include "lua_manifest_scanner.h"
#include "lua_script.h"

#include <atomic>
#include <cstring>

namespace wotbmod {
namespace lua {
namespace {

struct NamedPermission {
    const char* name;
    uint64_t bit;
};

// The whole vocabulary, in one table. Both directions of the mapping and every
// spelling of every name come from here.
constexpr NamedPermission kKnownPermissions[] = {
    {kPermissionNameCore, kPermissionCore},
    {kPermissionNameEventsPublic, kPermissionEventsPublic},
    {kPermissionNameStorage, kPermissionStorage},
    {kPermissionNameUiModifyGame, kPermissionUiModifyGame},
    {"ui", UINT64_C(1) << 4},
    {"ui.create", UINT64_C(1) << 5},
    {"ui.modify.own", UINT64_C(1) << 6},
    {"localization", UINT64_C(1) << 7},
    {"audio", UINT64_C(1) << 8},
    {"audio.custom", UINT64_C(1) << 9},
    {"audio.events", UINT64_C(1) << 10},
    {"resources", UINT64_C(1) << 11},
    {"resources.mod", UINT64_C(1) << 12},
    {"filesystem.mod_data", UINT64_C(1) << 13},
    {"input", UINT64_C(1) << 14},
    {"content", UINT64_C(1) << 15},
    {"hangar.scene", UINT64_C(1) << 16},
    {"vehicle.local.cosmetic", UINT64_C(1) << 17},
    {"camera.hangar", UINT64_C(1) << 18},
    {"camera.replay", UINT64_C(1) << 19},
    {"network.http.allowlisted", UINT64_C(1) << 20},
    {"settings", UINT64_C(1) << 21},
    {"input.actions", UINT64_C(1) << 22},
    {"entity.public.visible", UINT64_C(1) << 23},
    {"gameplay.tweak.camera", UINT64_C(1) << 24},
    {"gameplay.tweak.hud", UINT64_C(1) << 25},
    {"gameplay.tweak.hangar", UINT64_C(1) << 26},
    {"gameplay.tweak.replay", UINT64_C(1) << 27},
    {"gameplay.tweak.cosmetic", UINT64_C(1) << 28},
    {"gameplay.tweak.vehicle", UINT64_C(1) << 29},
    {"gameplay.tweak.projectile_visual", UINT64_C(1) << 30},
    {"gameplay.tweak.freecam", UINT64_C(1) << 31},
    {"battle.ui", UINT64_C(1) << 32},
    {"resources.overlay.game", UINT64_C(1) << 33},
    {"hooks.symbol", UINT64_C(1) << 34},
    {"render.callbacks", UINT64_C(1) << 35},
    {"battle.render.overlay", UINT64_C(1) << 36},
    {"camera.battle.read", UINT64_C(1) << 37},
    {"visible.projectile.events", UINT64_C(1) << 38},
    {"game.entity.public", UINT64_C(1) << 39},
    {"bigworld.observe", UINT64_C(1) << 40},
    {"bigworld.rpc.observe", UINT64_C(1) << 41},
    {"bigworld.rpc.metadata", UINT64_C(1) << 42},
    {"client.leave_to_hangar", UINT64_C(1) << 43},
    {"network.http", UINT64_C(1) << 44},
    {"native.memory", UINT64_C(1) << 45},
    {"native.memory_patch", UINT64_C(1) << 46},
    {"native.hook.address", UINT64_C(1) << 47},
    {"native.hooks", UINT64_C(1) << 48},
    {"render.native", UINT64_C(1) << 49},
    {"bigworld.rpc.modify", UINT64_C(1) << 50},
    // Creating files, which `resources.mod` deliberately does not imply: that
    // one is held by anything that reads a game file at all, and reading is not
    // the same authority as writing. VFS V2's four write routes take both.
    {"resources.write.mod_data", UINT64_C(1) << 51},
    {"ges.observe", UINT64_C(1) << 52},
    {"ges.publish", UINT64_C(1) << 53},
    {"session.cluster.read", UINT64_C(1) << 54},
    {"session.cluster.change", UINT64_C(1) << 55},
    {"packages.manage", UINT64_C(1) << 56},
};

static_assert(sizeof(kKnownPermissions) / sizeof(kKnownPermissions[0]) == 57u,
              "the Lua permission vocabulary must match the runtime");

// No fallback ceiling. If the client cannot be measured, doubt denies rather
// than granting this host's maximum and silently turning the fence decorative.
std::atomic<uint64_t>& CeilingBits() noexcept {
    static std::atomic<uint64_t> bits{kPermissionNothing};
    return bits;
}

// Separate because "measured and empty" is a valid answer, while "could not
// measure" is a reason not to run scripts at all.
std::atomic<bool>& CeilingMeasured() noexcept {
    static std::atomic<bool> measured{false};
    return measured;
}

struct Gate {
    const LuaScript* script;
    lua_CFunction binding;
    uint64_t bits;
    const char* permission;  // static literal, not owned
};

int PermissionGate(lua_State* state) noexcept {
    const Gate* gate =
        static_cast<const Gate*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (!gate->script->Permissions().AllowsAllBits(gate->bits)) {
        lua_pushnil(state);
        lua_pushfstring(state, "permission denied: %s", gate->permission);
        return 2;
    }
    return gate->binding(state);
}

}  // namespace

uint64_t PermissionBitFor(const char* name) noexcept {
    if (!name) return kPermissionNothing;
    for (const NamedPermission& known : kKnownPermissions) {
        if (std::strcmp(known.name, name) == 0) return known.bit;
    }
    return kPermissionNothing;
}

bool ScriptPermissions::Allows(const char* permission) const noexcept {
    return AllowsBit(PermissionBitFor(permission));
}

size_t KnownPermissionCount() noexcept {
    return sizeof(kKnownPermissions) / sizeof(kKnownPermissions[0]);
}

const char* KnownPermissionName(size_t index) noexcept {
    if (index >= KnownPermissionCount()) return nullptr;
    return kKnownPermissions[index].name;
}

uint64_t KnownPermissionBit(size_t index) noexcept {
    if (index >= KnownPermissionCount()) return kPermissionNothing;
    return kKnownPermissions[index].bit;
}

ScriptPermissions ScriptPermissions::DevCeiling() {
    return ScriptPermissions(CeilingBits().load());
}

ScriptPermissions ScriptPermissions::FromManifest(const char* json,
                                                   const char** out_error) {
    if (out_error) *out_error = nullptr;
    if (!json || *json == '\0') {
        if (out_error) *out_error = "the manifest is empty";
        return ScriptPermissions();
    }
    uint64_t requested = kPermissionNothing;
    const char* error = "the manifest could not be read";
    if (!ScanPermissionManifest(json, &PermissionBitFor, &requested, &error)) {
        if (out_error) *out_error = error;
        // Nothing, never the partial set collected before doubt.
        return ScriptPermissions();
    }
    return FromRequestedBits(requested);
}

ScriptPermissions ScriptPermissions::FromRequestedBits(
    uint64_t requested) noexcept {
    return ScriptPermissions(requested & CeilingBits().load());
}

bool MeasureHostCeiling(const WotbModV3Bootstrap* bootstrap,
                        WotbModV3Handle mod) noexcept {
    const WotbModV3PermissionsApiV1* api = nullptr;
    if (bootstrap && bootstrap->query_interface) {
        const void* iface = nullptr;
        if (bootstrap->query_interface(mod, WOTBMOD_V3_IFACE_PERMISSIONS,
                                       WOTBMOD_V3_PERMISSIONS_VERSION,
                                       &iface) == WOTBMOD_V3_OK) {
            api = static_cast<const WotbModV3PermissionsApiV1*>(iface);
        }
    }
    if (!api || !api->get_count || !api->get_at) {
        CeilingBits().store(kPermissionNothing);
        CeilingMeasured().store(false);
        return false;
    }
    uint32_t count = 0u;
    if (api->get_count(mod, &count) != WOTBMOD_V3_OK) {
        CeilingBits().store(kPermissionNothing);
        CeilingMeasured().store(false);
        return false;
    }
    uint64_t bits = kPermissionNothing;
    for (uint32_t index = 0u; index < count; ++index) {
        WotbModV3PermissionInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_PERMISSIONS_VERSION);
        // An unreadable entry can only narrow the derived ceiling, so skip it
        // rather than turning the remaining true grants into a guessed set.
        if (api->get_at(mod, index, &info) != WOTBMOD_V3_OK) continue;
        if (info.state != WOTBMOD_V3_PERMISSION_STATE_GRANTED) continue;
        info.name[sizeof(info.name) - 1u] = '\0';
        bits |= PermissionBitFor(info.name);
    }
    CeilingBits().store(bits);
    CeilingMeasured().store(true);
    return true;
}

bool HostCeilingMeasured() noexcept { return CeilingMeasured().load(); }

void SetFuncsGuarded(lua_State* state, const luaL_Reg* funcs, int nup,
                     const LuaScript* script, const char* permission) {
    const char* permissions[] = {permission};
    SetFuncsGuardedAll(state, funcs, nup, script, permissions, 1u,
                       permission);
}

void SetFuncsGuardedAll(lua_State* state, const luaL_Reg* funcs, int nup,
                        const LuaScript* script,
                        const char* const* permissions,
                        size_t permission_count,
                        const char* denial_label) {
    luaL_checkstack(state, nup + 1, "too many upvalues");
    uint64_t bits = kPermissionNothing;
    bool known = permission_count != 0u;
    for (size_t index = 0u; index < permission_count; ++index) {
        const uint64_t bit = PermissionBitFor(permissions[index]);
        if (bit == kPermissionNothing) known = false;
        bits |= bit;
    }
    if (!known) bits = kPermissionNothing;
    for (; funcs->name != nullptr; ++funcs) {
        if (funcs->func == nullptr) {
            lua_pushboolean(state, 0);
        } else {
            // Gate first, then the family's own upvalues. lua_pushcclosure
            // numbers from the bottom of the group, so this makes the gate
            // upvalue 1 and keeps guarded binding contexts starting at 2.
            Gate* gate =
                static_cast<Gate*>(lua_newuserdatauv(state, sizeof(Gate), 0));
            gate->script = script;
            gate->binding = funcs->func;
            gate->bits = bits;
            gate->permission = denial_label;
            for (int i = 0; i < nup; ++i) lua_pushvalue(state, -(nup + 1));
            lua_pushcclosure(state, &PermissionGate, nup + 1);
        }
        lua_setfield(state, -(nup + 2), funcs->name);
    }
    lua_pop(state, nup);
}

}  // namespace lua
}  // namespace wotbmod
