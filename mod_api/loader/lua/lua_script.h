#ifndef WOTBMOD_LUA_SCRIPT_H_
#define WOTBMOD_LUA_SCRIPT_H_

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>

extern "C" {
#include "../../third_party/lua/lua.h"
}

#include "../../include/wotbmod/bootstrap.h"
#include "lua_ownership.h"
#include "lua_permissions.h"

namespace wotbmod {
namespace lua {

// How many LuaScript objects this module owns right now, including scripts
// created by its test-only entry points rather than by the dev-folder host.
//
// The six counters Task 9's reload test started with watched what a script had
// *made* - subscriptions, controls, transactions, and this host's records of
// them. None of them watches the script itself. A reload that revoked
// everything correctly and then forgot to `delete script` reads a perfect
// 1/1/1/1/3/1 on all six while leaking a lua_State per reload: every resource
// released, and the largest allocation hot reload churns - tens of kilobytes of
// VM, stack and string table - left behind fifty times over.
//
// So this is the seventh counter, and it is the only one that can see the
// object rather than its effects. Incremented in the constructor and
// decremented in the destructor, so it is exact rather than a high-water mark,
// and zero after every script the suite ran has gone.
size_t LiveLuaScriptCount() noexcept;

// Which stage of LuaScript::Create failed, so callers do not have to recover
// that information by sniffing the error message (both a compile error and a
// runtime error from a chunk named "@probe" contain "probe:", so the string
// alone cannot tell them apart).
enum class LuaScriptStage {
    kOk,
    kCompileFailed,
    kRuntimeFailed,
};

enum class LuaProtectedCallResult {
    kOk,
    kError,
    kInstructionLimit,
};

// Stable text for the one host-generated Lua execution failure. Callers must
// not recover this state by inspecting Lua's error object: a script-level
// pcall can catch the hook's first raise, while the host-side trip flag remains
// authoritative and still disables the script.
const char* InstructionBudgetError() noexcept;

// One script, one lua_State, one lock. The lock is recursive and is taken on
// every entry into this state - a callback, a frame, a reload. Event callbacks
// can arrive on the render or a worker thread (WotbModV3Event.thread_role) and
// a lua_State is not thread safe: two threads inside one state is memory
// corruption, not a race for a value.
class LuaScript {
  public:
    // bootstrap/mod are how this script's globals reach the real ABI: Create
    // queries wotbmod.core for print (using client logging only with the core
    // permission and otherwise falling back to OutputDebugStringA) and
    // wotbmod.storage for wotb.storage, before the chunk itself runs - a
    // top-level call to either must work, not only a call from a later
    // on_enable. bootstrap may be null (mod may then be anything), which every
    // query below treats the same as "that interface is unavailable" rather
    // than a crash; existing callers that have no bootstrap yet pass null and
    // lose only those globals.
    //
    // permissions is what this script is allowed to touch, and it is required
    // rather than defaulted on purpose: there is no safe default. Defaulting to
    // the ceiling would silently grant a new call site everything the host
    // holds, and defaulting to nothing would silently break one. Naming it is
    // one word at each of five call sites and makes the trust level of every
    // script in this host greppable. It is fixed for the life of the object -
    // see Permissions() - so it is taken before the state exists rather than
    // set afterwards.
    //
    // out_stage, if not null, is always written: kOk on success, and which
    // stage failed on failure. Callers that don't care may pass null.
    static LuaScript* Create(
        const char* id, const char* source,
        const WotbModV3Bootstrap* bootstrap, WotbModV3Handle mod,
        const ScriptPermissions& permissions,
        std::string* out_error, LuaScriptStage* out_stage = nullptr);
    ~LuaScript();

    LuaScript(const LuaScript&) = delete;
    LuaScript& operator=(const LuaScript&) = delete;

    // Calls a global function if it exists. Absent is not an error: a script
    // need not define on_enable. Returns false only when the call itself
    // failed, and then out_error carries Lua's message or the host's stable
    // instruction-budget diagnostic.
    bool CallGlobal(const char* name, std::string* out_error);

    // Calls the optional per-frame handler with the native lifecycle values.
    // A script that has begun deactivation is skipped. The active check and
    // the call happen under the same script lock, so disable/reload can never
    // close the state between them or let a frame begin after on_disable.
    bool CallFrame(uint64_t frame_index, double delta_seconds,
                   std::string* out_error);

    // Atomically closes this script to future frame/callback entry and offers
    // on_disable exactly once. The courtesy callback runs under the same lock
    // that changed active_, so any frame already inside finishes first and any
    // later frame observes inactive. Set call_handler=false for a faulted
    // script whose instruction hook has made further Lua execution unsafe.
    bool Deactivate(bool call_handler, std::string* out_error);

    // Runs a function already on this state's stack under the per-entry
    // instruction budget. The caller must hold Entry and must pass the state
    // obtained from that Entry. Nested protected calls (a synchronous event
    // delivered while a binding is running) share the outer budget rather
    // than resetting it.
    LuaProtectedCallResult ProtectedCall(lua_State* state, int nargs,
                                         int nresults) noexcept;

    // Set by the count hook before it raises. It is atomic because a delivery
    // can trip on a render/worker thread while the main-thread frame pump is
    // deciding which loaded scripts to detach and destroy.
    bool InstructionLimitExceeded() const noexcept {
        return instruction_limit_exceeded_.load(std::memory_order_acquire);
    }

    // The only way to reach the lua_State. Constructing one takes the
    // script's lock and leaving the scope releases it, so "hold this
    // script's lock for every entry into its state" is enforced by the type
    // rather than remembered by whoever writes the next binding. There is
    // deliberately no accessor that hands out the state without the lock:
    // one that existed would eventually be used.
    //
    // The lock is recursive, so an Entry taken inside another Entry on the
    // same thread - a binding called from a callback this host invoked - is
    // fine. An Entry must not outlive its LuaScript.
    class Entry {
      public:
        explicit Entry(LuaScript& script);
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;

        lua_State* state() const { return state_; }
        bool active() const { return active_; }

      private:
        std::lock_guard<std::recursive_mutex> guard_;
        lua_State* state_;
        bool active_;
    };

    const std::string& Id() const { return id_; }

    // Everything the client is holding on this script's behalf, and the only
    // thing that can take it back without the script's cooperation. Every
    // binding that creates a resource records it here; ~LuaScript revokes
    // whatever is left. See lua_ownership.h.
    OwnershipRegistry& Ownership() { return ownership_; }

    // What this script may touch. Const and set once in the constructor, which
    // is what makes it readable from an event callback on the render thread
    // without a lock: there is no store for a load to race with, so the fence
    // answers the same way whichever thread asks it.
    //
    // Deliberately not settable afterwards. A script that could widen its own
    // permissions - or a host that could widen them between the moment a
    // binding is registered and the moment it is called - is not a fence.
    const ScriptPermissions& Permissions() const { return permissions_; }

  private:
    static void InstructionHook(lua_State* state, lua_Debug* debug);

    LuaScript(lua_State* state, std::string id,
              const ScriptPermissions& permissions);

    lua_State* state_ = nullptr;
    std::string id_;
    const ScriptPermissions permissions_;
    std::recursive_mutex lock_;
    std::atomic<bool> instruction_limit_exceeded_{false};
    // Both are touched only while lock_ is held. The depth is needed because
    // a client can synchronously deliver an event back into this same state
    // from a binding; that nested pcall must not mint a fresh budget.
    unsigned int protected_call_depth_ = 0u;
    bool instruction_budget_tripped_ = false;
    // Touched only while lock_ is held. It serializes host lifecycle entry;
    // callbacks that already claimed an external subscription also inspect it
    // after taking Entry, before touching the state.
    bool active_ = true;
    // Last, and destroyed first: ~LuaScript has already emptied it by then
    // (RevokeAll runs before the state closes), so this only ever destroys an
    // empty ledger. Taking `this` in the member initialiser above is safe -
    // the registry stores the pointer and dereferences it only from
    // RevokeAll, long after construction finishes.
    OwnershipRegistry ownership_;
};

// Defined out of line so it can name members declared after it.
inline LuaScript::Entry::Entry(LuaScript& script)
    : guard_(script.lock_), state_(script.state_), active_(script.active_) {}

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_SCRIPT_H_
