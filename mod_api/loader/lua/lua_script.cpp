#include "lua_script.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lualib.h"
}

// Only core_v1.h, not interface_ids.h or storage_v1.h: print() below needs
// WotbModV3CoreApiV1's own layout directly, but which interfaces exist and
// how to query them is lua_bindings.h's job now (RegisterAll), not this
// file's — see lua_bindings.h for why that split exists.
#include "../../include/wotbmod/core_v1.h"
#include "lua_bindings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace wotbmod {
namespace lua {
namespace {

// One outer entry gets this many Lua VM instructions. It is high enough for
// ordinary event/UI glue and low enough to bound a runaway callback to a small
// slice of a frame on the target 32-bit client. lua_sethook's count is itself
// measured in VM instructions, not source lines or wall-clock time.
constexpr int kInstructionBudget = 100000;
char kInstructionBudgetSentinel = 0;

// lua_tolstring (and the lua_tostring macro built on it) returns NULL when
// the value is neither a string nor number-convertible - error({}), error(nil)
// and error(setmetatable({}, {})) all produce a value like that. Assigning a
// NULL const char* to a std::string is undefined behaviour, so every site
// that turns a pcall's error object into a std::string goes through here
// instead of calling lua_tostring directly.
//
// The type is checked *first*, and that guard is not a tidiness measure: it
// is the same one lua_bind_events.cpp's delivery path documents as mandatory,
// for the same reason, and this function was missing it. lua_tostring on a
// *number* converts the value in place - it allocates a Lua string for it -
// and an allocation failure there leaves by LUA_ERRMEM, i.e. luaD_throw. Every
// caller of this function is reading the error object a lua_pcall already
// returned, so there is no protected frame left: L->errorJmp is NULL, the
// throw reaches the panic handler, and the panic handler calls abort(). As
// that file puts it: `error(42)` is enough. Nothing here may call a Lua API
// that can allocate.
//
// A number is still rendered rather than flattened into the placeholder,
// because "42" is the message a script author wrote and losing it would trade
// one defect for a worse diagnostic. It is formatted into a stack buffer with
// the C runtime instead, which cannot raise into Lua.
std::string SafeErrorMessage(lua_State* state, int index) {
    const int type = lua_type(state, index);
    if (type == LUA_TSTRING) {
        size_t length = 0u;
        const char* message = lua_tolstring(state, index, &length);
        // lua_tolstring on a value already known to be a string neither
        // converts nor allocates; the null test is belt and braces.
        return message ? std::string(message, length)
                       : std::string("<non-string error>");
    }
    if (type == LUA_TNUMBER) {
        char buffer[64] = {};
        if (lua_isinteger(state, index)) {
            _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%lld",
                        static_cast<long long>(lua_tointeger(state, index)));
        } else {
            _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%.14g",
                        static_cast<double>(lua_tonumber(state, index)));
        }
        return buffer;
    }
    return "<non-string error>";
}

// A script can run setmetatable(_G, {__index = function() error(...) end}).
// lua_getglobal's normal lookup path (auxgetstr -> luaV_finishget) runs that
// __index metamethod as ordinary Lua code whenever the requested name is not
// already a real key in _G. When that lookup is issued directly from this
// C++ code - as CallGlobal's probe for an optional handler is - it is not
// nested inside any lua_pcall frame yet, so if the metamethod raises,
// luaD_throw finds L->errorJmp == NULL and reaches the panic handler, which
// calls abort() and takes the whole host process down, not just the script.
// A raw lookup through the registry's globals table (the same table
// lua_getglobal itself starts from - see lua_pushglobaltable) can never
// invoke a metamethod, so every global lookup issued from outside a pcall
// uses this instead of lua_getglobal.
void PushGlobalRaw(lua_State* state, const char* name) {
    lua_pushglobaltable(state);   // ... globals
    lua_pushstring(state, name);  // ... globals, name
    lua_rawget(state, -2);        // ... globals, value
    lua_remove(state, -2);        // ... value
}

// Opened deliberately, one at a time. luaL_openlibs would also bring io, os,
// package and debug, and removing them afterwards leaves them loaded and
// reachable through other references.
void OpenSafeLibraries(lua_State* state) {
    static const luaL_Reg kSafe[] = {
        {LUA_GNAME, luaopen_base},
        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string},
        {LUA_MATHLIBNAME, luaopen_math},
        {nullptr, nullptr}};
    for (const luaL_Reg* entry = kSafe; entry->func; ++entry) {
        luaL_requiref(state, entry->name, entry->func, 1);
        lua_pop(state, 1);
    }
}

// print() is redirected rather than left as the stock luaB_print that
// luaopen_base installs, which writes through the C runtime's stdout - a
// stream this host does not own and the client offers no guarantee about
// (no console, a redirected handle, or nothing at all).
//
// Routed at WotbModV3CoreApiV1::log, now that Create() below can query
// wotbmod.core through the bootstrap it is handed - the carry-forward item
// from Task 3. OutputDebugStringA remains as the fallback for exactly the
// two cases where core.log is not reachable: no bootstrap was given (a
// caller with nothing to query yet), or the client answered the query but
// left the log slot null. Both fall back to the same channel and format
// src/v3/wotb_mod_v3_runtime.cpp's own RuntimeLog() falls back to when no
// log sink is registered.
//
// The script id, the core API pointer and the mod handle are all bound as
// upvalues when this closure is installed - id as an interned Lua string, so
// no manual lifetime management; core_api as light userdata, since it is a
// pointer with static lifetime (the ABI's own promise: "the bootstrap
// pointer belongs to the runtime and stays valid for the life of the mod");
// mod as a full 64-bit lua_Integer rather than light userdata, because light
// userdata on this 32-bit build is pointer-sized and would truncate a
// WotbModV3Handle that used its upper 32 bits.
//
// The line is assembled on the *Lua* stack rather than in a std::string, and
// that is rule 5. luaL_tolstring runs the value's own __tostring metamethod -
// arbitrary script code - and raises whenever that metamethod errors or
// returns a non-string:
//
//     print(setmetatable({}, {__tostring = function() error"x" end}))
//
// A raise is a longjmp out of luaD_throw, and print() used to hold a live
// std::string across it. **On this toolchain that particular call did not in
// fact leak, and the claim that it did is worth correcting rather than
// repeating.** Measured directly - a scratch lua_CFunction compiled at
// build.cmd's own flags (/std:c++17 /O2 /MD /EHsc /W4), holding a 64 KB
// std::string across a raising luaL_tolstring, reports dtor_ran=1, and 400
// consecutive raises move process private bytes by nothing (a deliberate
// 400 x 64 KB control leak measures 26.5 MB through the same counter). MSVC's
// x86 longjmp performs a local unwind of the /EHsc frames it passes. Task S3's
// dtor_ran=0 is a real measurement of a *different* mechanism - an SEH unwind
// to an __except after an access violation - and does not transfer to this
// one.
//
// The string is gone anyway, for three reasons that survive that correction:
//
//   - Rule 5 is not a restatement of what MSVC x86 happens to do. It holds
//     only while every frame between the raise and lua_pcall is /EHsc; the
//     vendored Lua next door is compiled as C with no /EH flag at all
//     (tests\build_lua_vendor_tests.cmd), so a future C callback of Lua's
//     own between the two would break it silently, and nothing in the ABI or
//     the language promises any of it.
//   - The other half of the finding needs no longjmp at all and was real:
//     `message.append` can throw std::bad_alloc out of a lua_CFunction into
//     Lua's C frames, where nothing catches it, and that is a terminated
//     game rather than a leak. Assembling on the Lua stack removes every
//     host-side allocation, which is what makes the noexcept below honest
//     rather than a promise the body cannot keep.
//   - print() is the most-used call in any script, so it is the worst place
//     in the host for a rule to hold only by accident.
int SandboxedPrint(lua_State* state) noexcept {
    const char* id = lua_tostring(state, lua_upvalueindex(1));
    const auto* core_api = static_cast<const WotbModV3CoreApiV1*>(
        lua_touserdata(state, lua_upvalueindex(2)));
    const WotbModV3Handle mod =
        static_cast<WotbModV3Handle>(lua_tointeger(state, lua_upvalueindex(3)));
    const auto* script = static_cast<const LuaScript*>(
        lua_touserdata(state, lua_upvalueindex(4)));
    const bool may_log_to_client =
        script && script->Permissions().AllowsBit(kPermissionCore);
    const int argc = lua_gettop(state);

    // Two slots per argument (its rendering, and the tab before it) plus room
    // for lua_concat's own working space. Asked for rather than assumed:
    // LUA_MINSTACK is 20, so print() with more than about nine arguments would
    // otherwise push past the top of the stack.
    //
    // A refusal here is a log line, not a raised error: print is the most-used
    // call in any script and the one a script author reaches for when
    // something else has already gone wrong, so it does not get to be the
    // thing that kills the frame.
    if (!lua_checkstack(state, 2 * argc + 4)) {
        if (may_log_to_client && core_api && core_api->log) {
            core_api->log(mod, WOTBMOD_V3_LOG_WARNING, "lua",
                          "print: no room on the Lua stack for this many "
                          "arguments");
        }
        return 0;
    }

    for (int i = 1; i <= argc; ++i) {
        if (i > 1) lua_pushliteral(state, "\t");
        // May raise - see above. Nothing of this function's own is alive to
        // be skipped, and the stack values already pushed are the pcall's to
        // unwind, not this function's.
        luaL_tolstring(state, i, nullptr);
    }
    if (argc == 0) {
        lua_pushliteral(state, "");
    } else {
        // Every value being concatenated is already a string, so this runs no
        // __concat metamethod and cannot re-enter script code. It can still
        // raise LUA_ERRMEM or "string length overflow", which is exactly why
        // it is reached with nothing of ours alive.
        lua_concat(state, 2 * argc - 1);
    }
    const char* message = lua_tostring(state, -1);
    if (!message) message = "";

    if (may_log_to_client && core_api && core_api->log) {
        core_api->log(mod, WOTBMOD_V3_LOG_INFO, "lua", message);
        return 0;
    }
    char line[2048] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[wotbmod.lua][%s] %s\n",
                id ? id : "", message);
    OutputDebugStringA(line);
    return 0;
}

void InstallSandboxedPrint(lua_State* state, const char* id,
                           const WotbModV3CoreApiV1* core_api,
                           WotbModV3Handle mod, LuaScript* script) {
    lua_pushstring(state, id);
    lua_pushlightuserdata(state, const_cast<WotbModV3CoreApiV1*>(core_api));
    lua_pushinteger(state, static_cast<lua_Integer>(mod));
    lua_pushlightuserdata(state, script);
    lua_pushcclosure(state, &SandboxedPrint, 4);
    lua_setglobal(state, "print");
}

// Base brings a few globals that reach outside the sandbox even without the
// io/os libraries.
void RemoveUnsafeGlobals(lua_State* state) {
    static const char* const kRemove[] = {
        "dofile", "loadfile", "require", "collectgarbage", "warn", nullptr};
    for (const char* const* name = kRemove; *name; ++name) {
        lua_pushnil(state);
        lua_setglobal(state, *name);
    }
}

// Replaces load() with one that accepts source text only. Bytecode is a
// verifier-free path straight into the VM's internals; a crafted chunk is
// arbitrary code, so text is the only mode a sandbox may offer.
// Standard load() is load(chunk, chunkname, mode, env). Both trailing
// arguments are deliberately ignored here: `mode` because "t" is the only mode
// this host will offer and letting a script ask for "b" would defeat the
// point, and `env` because a custom environment is a sandbox-escape surface
// the prototype has no need for. A script passing either gets text mode and
// the standard environment, silently - document this in the author-facing
// notes rather than pretending the arguments are honoured.
//
// Stock load() also accepts a reader *function* as chunk, called repeatedly
// to stream chunk pieces, instead of a string. This sandbox does not support
// that form - there is no benefit to it when every script is already one
// fixed piece of source in memory - but stock load()'s own contract for
// input it cannot use is (nil, message), never a raised error, and that
// contract is kept here too: lua_tolstring (unlike luaL_checklstring) simply
// returns NULL instead of raising when the argument is not a string or
// number, which is used below to detect the reader-function shape and fail
// the same graceful way rather than crash out through an unprotected raise.
int SafeLoad(lua_State* state) {
    size_t length = 0u;
    const char* chunk = lua_tolstring(state, 1, &length);
    if (!chunk) {
        lua_pushnil(state);
        lua_pushliteral(
            state, "load: only string chunks are supported by this sandbox");
        return 2;
    }
    const char* name = luaL_optstring(state, 2, "=(load)");
    if (luaL_loadbufferx(state, chunk, length, name, "t") != LUA_OK) {
        lua_pushnil(state);
        lua_insert(state, -2);
        return 2;   // nil, message - the contract load() already has
    }
    return 1;
}

// Read from a thread that is not necessarily the one that changed it - the
// tests ask across a DLL boundary while a delivery may be running - so atomic,
// like every other cross-thread counter in this host.
std::atomic<size_t>& LiveScriptCounter() noexcept {
    static std::atomic<size_t> count{0u};
    return count;
}

}  // namespace

size_t LiveLuaScriptCount() noexcept { return LiveScriptCounter().load(); }

// The counter is bumped here rather than in Create, and that is deliberate:
// Create's own failure path deletes the script, so counting construction and
// destruction rather than success and unload makes the two sides symmetric by
// construction. Nothing can add a script to the count that the destructor will
// not take back out.
LuaScript::LuaScript(lua_State* state, std::string id,
                     const ScriptPermissions& permissions)
    : state_(state),
      id_(std::move(id)),
      permissions_(permissions),
      ownership_(this) {
    *static_cast<LuaScript**>(lua_getextraspace(state_)) = this;
    LiveScriptCounter().fetch_add(1u);
}

const char* InstructionBudgetError() noexcept {
    return "instruction budget exceeded";
}

void LuaScript::InstructionHook(lua_State* state, lua_Debug*) {
    LuaScript* script =
        *static_cast<LuaScript**>(lua_getextraspace(state));
    if (script) {
        script->instruction_budget_tripped_ = true;
        script->instruction_limit_exceeded_.store(true,
                                                   std::memory_order_release);
    }

    // A script can catch the first hook error with its own pcall. Once the
    // budget has tripped, checking every following instruction ensures the
    // error reaches the host's outer pcall instead of granting another full
    // budget to a loop that repeatedly catches it. The host-side flag above is
    // still authoritative if the script catches at its final instruction and
    // returns without executing another one.
    lua_sethook(state, &LuaScript::InstructionHook, LUA_MASKCOUNT, 1);
    lua_pushlightuserdata(state, &kInstructionBudgetSentinel);
    lua_error(state);
}

LuaProtectedCallResult LuaScript::ProtectedCall(
    lua_State* state, int nargs, int nresults) noexcept {
    const int base = lua_gettop(state) - nargs - 1;
    if (InstructionLimitExceeded()) {
        lua_settop(state, base);
        lua_pushlightuserdata(state, &kInstructionBudgetSentinel);
        return LuaProtectedCallResult::kInstructionLimit;
    }

    const bool outermost = protected_call_depth_ == 0u;
    ++protected_call_depth_;
    if (outermost) {
        instruction_budget_tripped_ = false;
        lua_sethook(state, &LuaScript::InstructionHook, LUA_MASKCOUNT,
                    kInstructionBudget);
    }

    const int status = lua_pcall(state, nargs, nresults, 0);
    --protected_call_depth_;
    const bool tripped = instruction_budget_tripped_;
    if (outermost) lua_sethook(state, nullptr, 0, 0);

    if (tripped) {
        // Normalise both shapes to one sentinel at base + 1. Usually lua_pcall
        // left the hook's error object there; a script pcall can catch that
        // object and let the outer call return LUA_OK, but that is still a
        // budget violation and must look identical to every caller.
        lua_settop(state, base);
        lua_pushlightuserdata(state, &kInstructionBudgetSentinel);
        return LuaProtectedCallResult::kInstructionLimit;
    }
    return status == LUA_OK ? LuaProtectedCallResult::kOk
                            : LuaProtectedCallResult::kError;
}

LuaScript* LuaScript::Create(
    const char* id, const char* source, const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod, const ScriptPermissions& permissions,
    std::string* out_error, LuaScriptStage* out_stage) {
    if (out_stage) *out_stage = LuaScriptStage::kOk;
    if (!id || !source) {
        if (out_error) *out_error = "script id and source are required";
        if (out_stage) *out_stage = LuaScriptStage::kCompileFailed;
        return nullptr;
    }
    lua_State* state = luaL_newstate();
    if (!state) {
        if (out_error) *out_error = "out of memory creating a Lua state";
        if (out_stage) *out_stage = LuaScriptStage::kCompileFailed;
        return nullptr;
    }
    OpenSafeLibraries(state);
    // The object exists before its own chunk runs, and before the bindings
    // are installed. An interface the client can call back into records
    // which script to re-enter, not which lua_State - a raw lua_State* could
    // outlive the script that owns it, and there would be no lock left to
    // take. So the identity has to exist first; from here on every failure
    // path deletes the script rather than closing the state directly, which
    // is also what releases any subscription its top-level body managed to
    // make before it failed.
    //
    // The permissions go in here, before RegisterAll installs a single slot and
    // therefore before any script code can run. That ordering is the reason
    // there is no window in which a binding exists without a fence in front of
    // it: there is no moment at which a script could capture an unguarded
    // reference, because the unguarded function was never a Lua value.
    LuaScript* script = new LuaScript(state, id, permissions);

    // Under the script's own lock, for the whole of the setup and the chunk.
    //
    // This is not belt and braces. RegisterAll installs wotb.events, whose
    // subscribe publishes this LuaScript into a table the client's dispatcher
    // reads - so from the moment a top-level `wotb.events.subscribe(...)`
    // returns, a delivery on the render thread can be entering this state
    // while the chunk that made the subscription is still executing. Before
    // events existed nothing could re-enter a state Create had not returned
    // yet, and every entry point that existed (CallGlobal, the destructor)
    // took the lock; this task is what made the gap reachable, so this task
    // closes it.
    bool failed = false;
    {
        Entry entry(*script);
        // Every interface this host binds, installed before the chunk below
        // runs - a script's top-level body, not only a later on_enable, is
        // entitled to call wotb.storage.* or anything else RegisterAll
        // installs. out_core_api comes back from the same call rather than a
        // second query, since print (below) is the one interface this file
        // still needs directly.
        const WotbModV3CoreApiV1* core_api = nullptr;
        RegisterAll(state, bootstrap, mod, script, &core_api);
        InstallSandboxedPrint(state, id, core_api, mod, script);
        RemoveUnsafeGlobals(state);
        lua_pushcfunction(state, &SafeLoad);
        lua_setglobal(state, "load");

        std::string chunk_name = std::string("@") + id;
        if (luaL_loadbufferx(state, source, std::strlen(source),
                             chunk_name.c_str(), "t") != LUA_OK) {
            if (out_error) *out_error = SafeErrorMessage(state, -1);
            if (out_stage) *out_stage = LuaScriptStage::kCompileFailed;
            failed = true;
        } else {
            const LuaProtectedCallResult result =
                script->ProtectedCall(state, 0, 0);
            if (result != LuaProtectedCallResult::kOk) {
                if (out_error) {
                    *out_error = result ==
                                         LuaProtectedCallResult::kInstructionLimit
                                     ? InstructionBudgetError()
                                     : SafeErrorMessage(state, -1);
                }
                if (out_stage) *out_stage = LuaScriptStage::kRuntimeFailed;
                failed = true;
            }
        }
    }
    // Outside that scope, deliberately. Deleting the script destroys the very
    // mutex a live Entry would still be holding, which is undefined behaviour
    // rather than a lock ordering question - so the guard has to be gone
    // before the delete, not merely released in the right order.
    if (failed) {
        delete script;
        return nullptr;
    }
    return script;
}

LuaScript::~LuaScript() {
    // Before the lock, deliberately, and before the state closes. Anything
    // the client can still call back into this script has to be revoked
    // first, and revoking it means waiting for a delivery already running on
    // another thread to finish - which that delivery cannot do while this
    // thread holds the lock it is blocked on. Symmetric with the
    // RegisterAll() in Create above, and interface-agnostic for the same
    // reason: this file knows that everything a script made the client hold
    // has to be taken back before its state closes, not what any of it is.
    //
    // RevokeAll, not ReleaseScriptBindings directly: releasing a script's
    // bindings is one step *inside* revoking its resources - the first, since
    // a callback must not be able to fire against a control that is already
    // gone - and one entry point means there is no ordering here for a later
    // reader to get wrong. It also closes the ledger before it releases
    // anything, so a handler still running on another thread cannot create
    // something new for a script that is already being destroyed.
    //
    // Nothing after this line depends on the script having cooperated. A
    // script that defined no on_disable, or whose on_disable raised halfway
    // through, or that deliberately hoarded controls, all end here the same
    // way.
    ownership_.RevokeAll();
    {
        std::lock_guard<std::recursive_mutex> guard(lock_);
        active_ = false;
        if (state_) lua_close(state_);
        state_ = nullptr;
    }
    // Last, after the state is closed rather than before: the counter answers
    // "does this object still exist", and a test that saw it drop while
    // lua_close was still running would be reading a script that is not gone
    // yet. The lock is released first so nothing here is holding it.
    LiveScriptCounter().fetch_sub(1u);
}

bool LuaScript::CallGlobal(const char* name, std::string* out_error) {
    Entry entry(*this);
    lua_State* state = entry.state();
    PushGlobalRaw(state, name);
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return true;      // not defined is not a failure
    }
    const LuaProtectedCallResult result = ProtectedCall(state, 0, 0);
    if (result != LuaProtectedCallResult::kOk) {
        if (out_error) {
            *out_error = result == LuaProtectedCallResult::kInstructionLimit
                             ? InstructionBudgetError()
                             : SafeErrorMessage(state, -1);
        }
        lua_pop(state, 1);
        return false;
    }
    return true;
}

bool LuaScript::CallFrame(uint64_t frame_index, double delta_seconds,
                          std::string* out_error) {
    Entry entry(*this);
    if (!entry.active()) return true;
    lua_State* state = entry.state();
    PushGlobalRaw(state, "on_frame");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(frame_index));
    lua_pushnumber(state, static_cast<lua_Number>(delta_seconds));
    const LuaProtectedCallResult result = ProtectedCall(state, 2, 0);
    if (result != LuaProtectedCallResult::kOk) {
        if (out_error) {
            *out_error = result == LuaProtectedCallResult::kInstructionLimit
                             ? InstructionBudgetError()
                             : SafeErrorMessage(state, -1);
        }
        lua_pop(state, 1);
        return false;
    }
    return true;
}

bool LuaScript::Deactivate(bool call_handler, std::string* out_error) {
    Entry entry(*this);
    if (!entry.active()) return true;
    active_ = false;
    if (!call_handler) return true;
    lua_State* state = entry.state();

    // wotb.mod.on_disable handlers first, the author's global second. The
    // facade keeps its handlers in Lua and cannot be found by the raw global
    // lookup below - that lookup is raw on purpose, so a metatable on the
    // script's globals cannot run code during its own teardown - so the host
    // calls the facade's dispatcher by name. Raw lookups all the way down for
    // the same reason. Both calls are courtesy calls under the same budget: a
    // failure is reported and the other still runs.
    bool ok = true;
    std::string failure;
    PushGlobalRaw(state, "wotb");
    if (lua_istable(state, -1)) {
        lua_pushliteral(state, "mod");
        lua_rawget(state, -2);
        if (lua_istable(state, -1)) {
            lua_pushliteral(state, "__run_disable_handlers");
            lua_rawget(state, -2);
            if (lua_isfunction(state, -1)) {
                const LuaProtectedCallResult facade =
                    ProtectedCall(state, 0, 0);
                if (facade != LuaProtectedCallResult::kOk) {
                    ok = false;
                    failure = facade == LuaProtectedCallResult::kInstructionLimit
                                  ? InstructionBudgetError()
                                  : SafeErrorMessage(state, -1);
                    lua_pop(state, 1);
                }
            } else {
                lua_pop(state, 1);
            }
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);

    PushGlobalRaw(state, "on_disable");
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
    } else {
        const LuaProtectedCallResult result = ProtectedCall(state, 0, 0);
        if (result != LuaProtectedCallResult::kOk) {
            ok = false;
            if (!failure.empty()) failure += "; ";
            failure += result == LuaProtectedCallResult::kInstructionLimit
                           ? InstructionBudgetError()
                           : SafeErrorMessage(state, -1);
            lua_pop(state, 1);
        }
    }
    if (!ok && out_error) *out_error = failure;
    return ok;
}

}  // namespace lua
}  // namespace wotbmod
