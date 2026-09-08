#ifndef WOTBMOD_LUA_PERMISSIONS_H_
#define WOTBMOD_LUA_PERMISSIONS_H_

// The inner fence: what one particular script is allowed to touch.
//
// The spec fixes a two-level fence. The outer one is native and belongs to the
// runtime: this host is a single mod with a single permission tier, and every
// script inside it lives under that tier whatever else happens here. The inner
// one is this file: the host cuts each script down to what its own manifest
// asked for, at every binding call.
//
// The two are not alternatives. The outer fence is the reason a mistake in the
// inner one is survivable - the spec says so plainly: "even a total bypass of
// the inner fence cannot get past events, UI and its own storage" - and that
// sentence is only true if the ceiling this file enforces is the host's real
// one rather than a constant that agrees with it today. So the ceiling is
// *measured*, through WotbModV3PermissionsApiV1, and not written down here.
// See MeasureHostCeiling.
//
// Two shapes of script, one API:
//
//   * a script from the dev folder is written by the machine's owner and gets
//     DevCeiling() - everything the host itself holds;
//   * a script installed from the catalogue is written by a stranger and gets
//     FromManifest(json), which is that manifest's request intersected with the
//     same ceiling. A manifest is a request, never a grant: a manifest naming
//     `unsafe_native` gets nothing extra, because the host does not hold it
//     either and could not pass on what it does not have.
//
// The API a script sees is identical either way - the same tables with the same
// functions - because a script written against the dev folder has to run
// unchanged once it is distributed. Only the answers differ.

#include <cstddef>
#include <cstdint>

#include "../../include/wotbmod/base.h"
#include "../../include/wotbmod/bootstrap.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
}

namespace wotbmod {
namespace lua {

class LuaScript;

// One bit per permission name this host understands, so that the question a
// binding asks on every single call - "may this script do this?" - is one AND
// rather than a walk over a list of strings. The mandated
// `bool Allows(const char*) const` is kept and still answers by name, but it is
// not what the fence calls: SetFuncsGuarded resolves the name to its bit once,
// at registration, and the guard tests the bit.
//
// A permission the host does not understand has no bit and can never be
// granted, which is exactly right: this host can only pass on what it can also
// enforce, and a name with no binding family behind it is a name it cannot.
enum PermissionBit : uint64_t {
    kPermissionNothing = UINT64_C(0),
    kPermissionCore = UINT64_C(1) << 0,
    kPermissionEventsPublic = UINT64_C(1) << 1,
    kPermissionStorage = UINT64_C(1) << 2,
    kPermissionUiModifyGame = UINT64_C(1) << 3,
};

// The hand-written families name these constants directly. Generated families
// use literals emitted from the same API model; all names resolve through the
// 54-entry table in lua_permission_runtime.cpp.
inline constexpr char kPermissionNameCore[] = "core";
inline constexpr char kPermissionNameEventsPublic[] = "events.public";
inline constexpr char kPermissionNameGesObserve[] = "ges.observe";
inline constexpr char kPermissionNameGesPublish[] = "ges.publish";
inline constexpr char kPermissionNameStorage[] = "storage";
inline constexpr char kPermissionNameUiModifyGame[] = "ui.modify.game";

// The bit for a name, or kPermissionNothing for a name this host does not
// understand (including null). Never raises; never allocates.
uint64_t PermissionBitFor(const char* name) noexcept;

// The same vocabulary as a listing rather than a lookup, for wotb.mod's
// permissions(): entry `index` of the known-permission table, in table order.
// An index at or past KnownPermissionCount() answers null / kPermissionNothing.
size_t KnownPermissionCount() noexcept;
const char* KnownPermissionName(size_t index) noexcept;
uint64_t KnownPermissionBit(size_t index) noexcept;

// One script's effective permissions. Immutable once built - a script's set is
// decided before its first line of code runs and never changes afterwards,
// which is what makes reading it from an event callback on the render thread a
// plain load rather than something needing a lock.
class ScriptPermissions {
  public:
    // Grants nothing. The default a caller gets by saying nothing, deliberately:
    // a new call site that forgets to state a trust level denies everything
    // rather than granting everything.
    ScriptPermissions() = default;

    // The permissions a manifest asks for, intersected with the ceiling.
    //
    // The scanner behind this is strict on purpose: it reads exactly the
    // top-level "permissions" key and its array of strings. Unknown plain-text
    // names grant no bit; malformed JSON and names hidden behind escapes are
    // refused rather than guessed. A permissive parser here is a security bug,
    // not a convenience - it decides what a stranger's script may touch.
    //
    // A refusal grants nothing at all and never a partial set. out_error, when
    // given, is set to why - and to null when there was nothing wrong, so it is
    // the signal as well as the message. The mandated one-argument form still
    // compiles and still fails closed; it simply cannot say what was wrong.
    //
    // A `const char**` rather than a `std::string*`, and that is not a style
    // choice: every message this can produce is a static literal, so nothing
    // here allocates and nothing here can throw. A std::string out-parameter
    // would make "was it refused?" a question about whether a string is empty,
    // which under memory exhaustion is a question that can answer wrongly in
    // the permissive direction - an empty message read as success, on the one
    // path where the answer decides what a stranger's script may touch.
    static ScriptPermissions FromManifest(const char* json,
                                          const char** out_error = nullptr);

    // Intersects permission bits already produced by ScanLuaManifest with the
    // measured host ceiling. Installed-mod loading uses this after one strict
    // scan so path metadata and effective permissions cannot come from two
    // parsers with subtly different refusal rules.
    static ScriptPermissions FromRequestedBits(uint64_t requested) noexcept;

    // Everything this host itself holds - the trust level a dev-folder script
    // runs at, and the ceiling every manifest is intersected with.
    //
    // Takes no arguments, as mandated, so the measurement it returns has to
    // live at host scope: MeasureHostCeiling below fills it in. Before the
    // first measurement this is empty, which denies everything - the safe
    // direction for a question asked before its answer exists.
    static ScriptPermissions DevCeiling();

    // Does this script hold `permission`? The public question, by name.
    bool Allows(const char* permission) const noexcept;

    // The same question by bit - what the fence actually calls, once per
    // binding call. kPermissionNothing is never allowed: an unmapped name
    // cannot be granted by an accident of zero being a subset of everything.
    bool AllowsBit(uint64_t bit) const noexcept {
        return bit != kPermissionNothing && (bits_ & bit) == bit;
    }

    bool AllowsAllBits(uint64_t bits) const noexcept {
        return bits != kPermissionNothing && (bits_ & bits) == bits;
    }

  private:
    explicit ScriptPermissions(uint64_t bits) noexcept : bits_(bits) {}

    uint64_t bits_ = kPermissionNothing;
};

// Reads the host's own granted permissions off WotbModV3PermissionsApiV1 and
// caches them as the ceiling every later DevCeiling() returns.
//
// Enumerated (get_count/get_at) rather than asked for by name (query), because
// enumeration is one pass and cannot be wrong about a name this file forgot to
// ask about. Only entries whose state is GRANTED count; a granted name this
// host does not understand contributes nothing, since this host could not
// enforce it on a script's behalf anyway.
//
// Returns true when it measured. **False means it could not, and then the
// ceiling is empty and the caller must not run any script at all.**
//
// There is no fallback ceiling, and its absence is the design. An earlier
// version granted the four names the spec fixes for this host's slice when the
// client could not be asked - deliberate, logged, and still the one place where
// doubt granted instead of denying. The spec's own principle settles it: honest
// refusal beats coverage. A host that cannot read the ceiling it exists to
// enforce does not know what it is guarding, and running scripts at the maximum
// in that state is what turns a fence into decoration, invisibly. So the
// contract is now: measure, or refuse and say so.
//
// The two are different facts, not one. A client that grants this host nothing
// produces an empty ceiling and returns *true* - it answered. A client that
// cannot be asked produces an empty ceiling and returns false. Only the second
// stops the host. See HostCeilingMeasured.
bool MeasureHostCeiling(const WotbModV3Bootstrap* bootstrap,
                        WotbModV3Handle mod) noexcept;

// Whether the ceiling in force right now came from a measurement.
//
// False before the first MeasureHostCeiling call and after one that failed;
// true after one that succeeded, even if what it measured was nothing. The host
// consults this before loading a script, so that a state it cannot vouch for
// runs no code rather than all of it.
bool HostCeilingMeasured() noexcept;

// The upvalue index a guarded binding's own upvalues live at.
//
// SetFuncsGuarded installs one shared guard closure in front of every binding,
// and that guard needs an upvalue of its own. It takes slot 1, so a binding's
// first upvalue - the family context every lua_bind_*.cpp shares across its
// closures - is slot 2. A binding therefore says GuardedUpvalueIndex(1) where
// it used to say lua_upvalueindex(1), and nothing else about it changes: the
// guard calls the binding as a plain C call, without a new Lua frame, so
// upvalues resolve against the guard closure that is still the running one.
inline int GuardedUpvalueIndex(int index) noexcept {
    return lua_upvalueindex(index + 1);
}

// luaL_setfuncs, with the fence in front of every entry.
//
// Same contract as luaL_setfuncs: `funcs` goes into the table just below the
// `nup` upvalues on top of the stack, each entry gets its own copy of those
// upvalues, and the upvalues are popped at the end. The difference is that what
// lands in the table is never the binding itself - it is a guard closure that
// refuses with `nil, "permission denied: <permission>"` unless the script holds
// `permission`, and otherwise calls straight through.
//
// Wrapping rather than checking inside each binding buys two things that
// hundreds of copy-pasted guards would not:
//
//   * the refusal happens before *any* of the binding runs, so no argument is
//     converted, no handle is looked up and no ABI call is made on a call the
//     fence is going to refuse. A guard that returns `nil, "permission denied"`
//     after the ABI call has already landed has denied nothing;
//   * the raw binding function is never a Lua value anywhere in the state, so
//     there is no reference for a script to capture, alias, or reach through a
//     metatable. There is nothing to walk around because there is no other way
//     in.
//
// `script` is the script whose permissions are consulted, read at call time
// rather than captured, so the fence is a question about the script rather than
// a snapshot that could drift from it.
void SetFuncsGuarded(lua_State* state, const luaL_Reg* funcs, int nup,
                     const LuaScript* script, const char* permission);

// The same registration shape for an interface whose native operations use
// several independent named permissions. Requiring the complete set is
// intentionally conservative: the native runtime sees the host's aggregate
// mod handle, not the inner Lua script, so allowing a script with only one
// member would let the host's other grants answer on its behalf.
void SetFuncsGuardedAll(lua_State* state, const luaL_Reg* funcs, int nup,
                        const LuaScript* script,
                        const char* const* permissions,
                        size_t permission_count,
                        const char* denial_label);

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_PERMISSIONS_H_
