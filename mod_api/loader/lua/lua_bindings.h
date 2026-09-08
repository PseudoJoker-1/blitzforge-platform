#ifndef WOTBMOD_LUA_BINDINGS_H_
#define WOTBMOD_LUA_BINDINGS_H_

// One Register* function per ABI interface this host binds into Lua. Each
// installs its slots under the shared `wotb` global table (creating it if no
// earlier Register* call has), following the convention in lua_convert.h.
//
// Storage is bound first — see lua_bind_storage.cpp — because its 14 slots
// sample every shape the remaining interfaces will need: a sized string
// (get_json, get_path), an opaque token (begin_transaction), an
// out-parameter boolean (contains), a raw byte buffer (get_bytes/set_bytes)
// and a plain command (flush). The remaining Register* functions are added
// by later tasks, one per interface, alongside their own .cpp — each one
// gets its own entry in RegisterAll below, not its own query-and-register
// block copied into lua_script.cpp.

#include "../../include/wotbmod/base.h"
#include "../../include/wotbmod/bootstrap.h"
#include "../../include/wotbmod/core_v1.h"
#include "../../include/wotbmod/events_v1.h"
#include "../../include/wotbmod/ges_v1.h"
#include "../../include/wotbmod/storage_v1.h"
#include "../../include/wotbmod/ui_v2.h"

extern "C" {
#include "../../third_party/lua/lua.h"
}

#include <cstddef>

namespace wotbmod {
namespace lua {

class LuaScript;
class ScriptPermissions;

// Ensures the shared `wotb` global table exists and leaves it on top of the
// stack, creating it the first time any Register* function runs. Every
// Register* function needs exactly this opening step - storage, events and
// ui each carried their own copy of the same six lines until this task's own
// review round found it, at three copies rather than the 46 a full generator
// would produce. Called at the start of a Register* function; the caller
// then pushes its own sub-table, fills it in, and finishes with
// `lua_setfield(state, -2, "name"); lua_pop(state, 1);` to leave the stack
// exactly as it found it - that half stays per-caller because it is the one
// part that differs (the field name, and what goes in the table).
void PushWotbTable(lua_State* state);

// Installs wotb.storage.* for the 14 WotbModV3StorageApiV1 slots, bound
// against `mod`. api may be null — the interface was not available from
// this client's bootstrap — in which case this is a no-op and `wotb.storage`
// is left undefined rather than a table of functions that all fail the same
// way; a script testing `if wotb.storage then` sees the truth directly.
//
// script, like api, is required rather than optional: begin_transaction opens
// something the client then holds until this script commits, rolls back or is
// destroyed, and the registry that would take it back on that last path lives
// on the script. Binding these slots without one would hand a script a way to
// leave an open transaction behind that nothing could close, so a null script
// leaves wotb.storage undefined exactly as a null api does.
void RegisterStorage(lua_State* state, const WotbModV3StorageApiV1* api,
                     WotbModV3Handle mod, LuaScript* script);

// Installs wotb.events.* for the 9 WotbModV3EventsApiV1 slots. Null api is
// the same no-op RegisterStorage's is.
//
// Two parameters this interface needs that storage did not:
//
//   script — the only family where the client calls *into* Lua. A delivered
//     event has to re-enter this script's state, and every entry into a
//     state takes that script's lock (LuaScript::Entry), because a callback
//     can arrive on the render thread or a worker while the main thread is
//     already inside the same state. The subscription therefore has to know
//     which LuaScript to enter, not merely which lua_State — a raw
//     lua_State* could outlive the script that owns it, and there would be
//     no lock to take.
//
//   core_api — an error raised inside a callback must be caught, reported
//     and leave the subscription intact. "Reported" has to mean somewhere a
//     mod author can read: the host already routes print() at
//     WotbModV3CoreApiV1::log for exactly that reason, and a callback fault
//     is more worth logging than a print is. Null falls back to
//     OutputDebugStringA, the same channel and format print() falls back to.
void RegisterEvents(lua_State* state, const WotbModV3EventsApiV1* api,
                    const WotbModV3CoreApiV1* core_api, WotbModV3Handle mod,
                    LuaScript* script);

// Installs wotb.ges.* for the GES::GameEventSystem bus (ges_v1.h): types,
// subscribe, unsubscribe behind ges.observe and publish behind ges.publish.
// A GES subscription rides on wotb.events.subscribe (the trampoline it
// installs there is what receives the delivery), so events_api is required
// and null leaves wotb.ges undefined, exactly as a null api or script does.
// core_api is carried for the same reason RegisterEvents carries it.
void RegisterGes(lua_State* state, const WotbModV3GesApiV1* api,
                 const WotbModV3EventsApiV1* events_api,
                 const WotbModV3CoreApiV1* core_api, WotbModV3Handle mod,
                 LuaScript* script);

// Installs wotb.ui.* for 18 of WotbModV3UiApiV2's 74 slots — the tree
// (create/destroy/add_child/remove_child/set_id/find_by_id/is_alive), text
// and visibility, geometry (position/size/anchor/pivot) and named slots
// (slot_find/slot_attach/slot_detach). Null api is the same no-op
// RegisterStorage's is; the other 56 slots are simply never registered,
// exactly as storage leaves the interfaces it does not bind absent rather
// than present and always failing.
//
// No core_api parameter: unlike events, nothing here calls back into Lua, so
// there is no callback fault to log. script is needed all the same, and for a
// different reason than events needs it — not to re-enter this state, but
// because control_create mints something the client holds until this script
// destroys it or is destroyed, and the registry that takes it back on that
// second path lives on the script. Null leaves wotb.ui undefined, as a null
// api does.
void RegisterUi(lua_State* state, const WotbModV3UiApiV2* api,
                WotbModV3Handle mod, LuaScript* script);

// wotb.packages: the loader-private bridge from a script to <game>\wotbmod\
// wotbmod.exe (list, info, uninstall, enable, disable, launcher-open,
// restart-client, sync), guarded by packages.manage. Not part of the frozen
// C ABI: it spawns the player's own tool, hidden, and hands the exit code and
// output back through poll(). core is needed for the game directory; a null
// core still defines the table, whose run() then answers with an error.
void RegisterPackages(lua_State* state, const WotbModV3CoreApiV1* core,
                      WotbModV3Handle mod, LuaScript* script);

// wotbmod.core, or null if this client does not offer it.
//
// Exists for the one caller that needs the log outside any script's state: the
// reload path, whose whole job on a compile error is to say so somewhere a mod
// author can read. It cannot use a script's print() - the script it would have
// printed from is the one that failed to compile - and it must not repeat the
// query-and-cast that RegisterAll already owns, so the query lives here beside
// its siblings rather than being copied into lua_host_mod.cpp.
const WotbModV3CoreApiV1* QueryCoreApi(const WotbModV3Bootstrap* bootstrap,
                                       WotbModV3Handle mod);

// events' half of ReleaseScriptBindings below. Separate so that the list of
// interfaces to release lives in one place (ReleaseScriptBindings) rather
// than inside any one interface's file.
void ReleaseEventSubscriptions(LuaScript* script);

// How many subscription records this host is holding, across every script
// that has ever run in this process. Not part of the mod ABI: it exists
// because the client's own view cannot see one particular leak.
//
// A subscription is two records, not one - the client's, and this host's own,
// which holds the LuaScript to re-enter and the Lua registry reference to the
// handler. Revoking the client's says nothing about this one, and a host
// record that outlives its script is the dangerous half: it names a freed
// LuaScript, and a client that delivers into it anyway - because it never got
// the unsubscribe, or ignored it - is an access violation inside a player's
// game rather than a leak.
//
// Every script that has been destroyed must leave this at whatever it was
// before that script ran. Zero, when no script is alive.
size_t EventSubscriptionRecordCount();

// Number of scripts currently asking the host to keep the game cursor
// released. This is host-internal state, not part of the public mod ABI. It is
// exposed to the test DLL so teardown can prove that a fault/reload cannot
// leave the player's mouse unlocked forever.
size_t InputCursorOwnerCount();

// Drops everything the Register* functions above left holding a reference to
// this script: unsubscribes its event subscriptions from the client so no
// further callback can be delivered into it, releases the registry
// references those subscriptions held, and waits for any callback already in
// flight on another thread to finish.
//
// Called by OwnershipRegistry::RevokeAll as its first step, which ~LuaScript
// calls before it closes the state — subscriptions before controls, so that a
// callback cannot fire against a control that is already gone. It names no
// interface for the same reason RegisterAll does not: the caller knows that a
// script's bindings must be released first, not which bindings exist.
//
// Each subscription it unsubscribes it also forgets from that registry, at the
// moment it unsubscribes it — so the registry's own sweep afterwards can be an
// unconditional backstop without ever releasing a token twice.
//
// Must be called with no script lock held, and before lua_close: it takes
// the script's own lock to release registry references, and it blocks until
// in-flight deliveries return.
void ReleaseScriptBindings(LuaScript* script);

// Queries every interface this host currently binds and installs their
// globals into state, in one call. lua_script.cpp — which owns the sandbox
// itself, not any one interface — calls this once from LuaScript::Create,
// before the chunk's own top-level body runs, rather than including each
// interface's header and repeating QueryInterfaceOrNull + Register* inline
// per interface; at 46 interfaces that inline form would be a 90-line list
// growing inside the script host, one entry added here instead keeps
// lua_script.cpp interface-agnostic no matter how many more of these there
// end up being.
//
// out_core_api, if not null, is set to the WotbModV3CoreApiV1 this bound (or
// null if it was unavailable) — the one interface print() itself needs.
// Queried here rather than a second time in lua_script.cpp, since this
// function already has to query wotbmod.core's neighbours anyway.
//
// script is the LuaScript these globals are being installed into. It exists
// before its own chunk has run — LuaScript::Create builds the object, then
// calls this, then runs the body — because an interface that the client can
// call back into (events) has to record which script to re-enter, and a
// subscription made from a script's top-level body is as legitimate as one
// made from a later on_enable.
// Installs the C half of wotb.mod: id(), permissions() and has_permission(),
// the three answers a script cannot compute for itself - the script id is
// otherwise reachable only by print(), and the effective permission set
// (manifest intersected with the measured host ceiling) lives on the
// LuaScript. The Lua half (capability, info, on_disable) is a prelude that
// extends this table. Both arguments may be null for a state built without a
// script; id() then answers nil plus a message and has_permission() false.
// The permissions pointer must outlive the state, which it does: it points at
// the LuaScript that owns the state.
void RegisterModLibrary(lua_State* state, const char* id,
                        const ScriptPermissions* permissions);

void RegisterAll(lua_State* state, const WotbModV3Bootstrap* bootstrap,
                 WotbModV3Handle mod, LuaScript* script,
                 const WotbModV3CoreApiV1** out_core_api);

// Generated from the structural header model. Installs every current slot not
// owned by the hand-written storage/events/UI convention layer.
void RegisterGeneratedBindings(lua_State* state,
                               const WotbModV3Bootstrap* bootstrap,
                               WotbModV3Handle mod, LuaScript* script);
void ReleaseGeneratedCallbacks(LuaScript* script);

// Also generated from the header model: the named integer/string constants
// every one of those slots is meant to be called with. Separate from
// RegisterGeneratedBindings because it needs no bootstrap, no mod handle and
// no script - a constant is a value, not a call, and there is nothing about it
// to guard or to own.
//
// RegisterAll calls this *after* RegisterStorage, RegisterEvents, RegisterUi
// and RegisterGeneratedBindings, and the order is load-bearing in a way that
// is easy to get backwards. Those three hand-written functions do not merge
// into wotb.<name>: each builds a fresh table with lua_newtable and assigns it
// wholesale, so anything installed on wotb.storage, wotb.events or wotb.ui
// before them is silently discarded. wotb.storage.PATH_* would disappear
// entirely, since all 14 storage slots are hand-written and this is the only
// thing that ever puts a constant on that table. This function merges
// (OpenConstantTable), so running it last is safe.
//
// Where a name would exist in both sets, the reviewed hand-written spelling
// wins by *exclusion* rather than by ordering: the generator drops such
// constants from its output - WOTBMOD_V3_CONTEXT_* is excluded because
// wotb.context already publishes it - and tests/test_lua_api_model.py checks
// both the exclusion list and the rule that no table publishes one name twice.
// See the call site in lua_bindings.cpp for the full argument.
void RegisterGeneratedConstants(lua_State* state) noexcept;

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_BINDINGS_H_
