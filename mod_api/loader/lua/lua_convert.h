#ifndef WOTBMOD_LUA_CONVERT_H_
#define WOTBMOD_LUA_CONVERT_H_

// The binding convention: every rule for moving a value between the frozen C
// ABI and a Lua script lives here, and nowhere else.
//
// The current ABI model has 589 function slots across 40 interfaces. Forty-one
// are hand-written (storage's 14, events' 9, ui's 18), 546 are generated, and
// two raw-native-pointer operations are intentionally unavailable without FFI.
// Every generated slot applies exactly the rules in this file. So each rule
// below is written to be applied several hundred times
// without a per-slot decision: a bound slot should be a call to the ABI
// wrapped in one of these helpers, never a hand-made choice about how a
// failure or a handle or a string is shaped.
//
// The inventory is parsed structurally from include/wotbmod rather than counted
// by a token grep, so callback typedefs are not mistaken for interface slots.
//
// The six rules, stated once:
//
//   1. A failure is `nil, message`. Never an error() raised at the script, and
//      never a numeric code. This is the Lua idiom for a failure the caller is
//      expected to handle; a script that ignores it gets nil and fails loudly
//      at the point of use rather than continuing on a plausible-looking zero.
//      A bad argument is a failure of this same shape - PushArgumentError
//      produces it, and the CheckArg* family below produces it automatically
//      so a slot's own code never composes the sentence by hand.
//
//   2. Every bound call returns at least one value, and that first value is
//      falsy only when the script did not get what it asked for. `if not
//      thing() then` is therefore always correct. Two shapes follow from this
//      and are the only two a slot may use:
//        - a slot whose result is *required* returns it directly, so the
//          value itself is the truthy first return;
//        - a slot whose result is *optional* - "the current camera, if there
//          is one" - returns `true, value_or_nil`, so that an absent value is
//          `true, nil` and never a bare nil that would read as a failure.
//      See PushOptionalHandle. Getting this wrong is not a cosmetic problem:
//      a bare nil on a successful call makes `if not f() then` report a
//      failure that never happened.
//
//   3. A handle crosses as userdata carrying a type name, never as a number.
//      A number is forgeable from inside the sandbox; userdata is not. The
//      type name is always one of the kHandle* constants below, never a
//      literal typed at the call site. A bare uint64_t token that has no
//      entry in WotbModV3HandleType (WotbModV3Token is the one the ABI
//      defines today) is exactly as forgeable and gets exactly the same
//      treatment, through PushToken rather than PushHandle - see the token
//      section below for why the two must not share PushHandle's own
//      zero-is-invalid assumption.
//
//   4. A sized string is read by the helper here, never by hand. The two-call
//      buffer dance appears exactly once in this codebase - one retry loop,
//      shared by FetchSizedString (a value that is text, where the ABI's
//      size includes a terminator not every slot honours) and
//      FetchByteBuffer (a value that is binary, where the exact byte count
//      is the whole contract and an embedded zero is data, not a
//      terminator). The two differ only in what they do with the bytes the
//      loop reports, never in the loop itself.
//
//   5. No C++ exception may leave this code, and nothing with a destructor may
//      be alive when this code calls into Lua. The vendored Lua is compiled as
//      C - tests\build_lua_vendor_tests.cmd compiles third_party\lua\*.c with
//      no /TP - so LUAI_THROW is longjmp, not throw. Two consequences, both
//      fatal if ignored:
//        - lua_pcall catches a longjmp, not a C++ exception. An exception
//          raised inside a lua_CFunction unwinds straight through Lua's C
//          frames and terminates the process. A script asking for a value the
//          host cannot allocate would then be able to kill the client, which
//          is precisely what this host exists to prevent. Every entry point
//          here that can allocate therefore has a catch-all that turns the
//          failure into `nil, message`.
//        - a Lua API call that raises LUA_ERRMEM leaves by longjmp, and a
//          longjmp is not a C++ unwind. Whether it runs a destructor is a
//          property of the toolchain, not of the language: measured at
//          build.cmd's own flags, MSVC's x86 longjmp *does* locally unwind the
//          /EHsc frames it passes (a 64 KB std::string held across a raising
//          luaL_tolstring reports dtor_ran=1, and 400 raises move private
//          bytes by nothing against a 26.5 MB control leak). This file
//          asserted the opposite for most of the branch, on the strength of a
//          Task S3 measurement that was really about an SEH unwind to an
//          __except - a different mechanism, where destructors genuinely are
//          skipped.
//
//          The rule is unchanged by that correction, and the reason is the
//          point: it holds only while *every* frame between the raise and the
//          lua_pcall is /EHsc, and the Lua next door is compiled as C with no
//          /EH flag at all (tests\build_lua_vendor_tests.cmd). One C callback
//          of Lua's own in that chain, or one non-x86 target, and the
//          guarantee is gone with nothing to notice it. A rule that costs a
//          scope is not worth trading for a compiler switch's current
//          behaviour. So every allocation still happens inside a scope that
//          closes before the first Lua call, and the bytes handed to Lua still
//          live in storage with no destructor to skip.
//          A binding is written as one expression and cannot scope its own
//          temporaries, so anything that must be dead before a push is taken
//          by rvalue reference here and emptied here rather than left to the
//          call site. Note the exact limit of that, because it is easy to
//          overstate: it disposes of the *destructor*, not of the
//          *constructor*. Building the argument happens in the caller's frame,
//          before this file's noexcept boundary, so a callable whose captures
//          do not fit std::function's small-object buffer can still throw
//          where nothing catches it. See PushSizedString.
//
//   6. An argument a slot cannot use is read by one of the CheckArg*
//      accessors below, never by a hand-rolled `lua_type(...) != LUA_T...`
//      check. Each one both validates and, on failure, pushes the standard
//      `nil, "argument N: expected ..., got ..."` itself (rule 1's shape),
//      so the only choice left at a call site is which accessor matches the
//      value it needs, not how to phrase the refusal. Rules 1-5 govern what
//      a slot returns; nothing above them governed what it reads, and the
//      first real interface hand-rolled a key check and hand-inlined two
//      more type checks before that gap got a name. CheckHandle predates
//      this rule and stays the primitive underneath it - a binding that
//      accepts more than one handle type, or wants a bespoke "expected"
//      phrase, still composes PushArgumentError itself, because a caller
//      composing that by hand is unavoidable there - but a plain string,
//      byte string, integer, number, boolean, function, enum value, flag
//      mask or single-type handle argument, and the optional form of each,
//      always goes through here.
//
//      This rule said all of that from the day it was written, and for a
//      required handle it was not true: only the *optional* form,
//      CheckOptArgHandle, was ever declared. Five slots across three
//      interfaces each wrote out CheckHandle-then-PushArgumentError instead,
//      and two of them justified it in a comment citing a sentence in this
//      file that had already been edited away. That is what a rule with a
//      missing member costs: not the five copies, which are cheap, but the
//      five *arguments* for them, which read as design. CheckArgHandle
//      exists below now and all five call it.
//
//      The family was string/bytes/integer only when the first interface
//      was bound, which is why storage's get_path still had to hand-roll a
//      value check (an integer of the right *type* that is not one of the
//      four path kinds the ABI defines) next to a CheckArg* call that had
//      already checked the type. That gap is closed below by CheckArgEnum:
//      a hand-rolled value check is exactly the sort of per-slot decision
//      this file exists to remove, and events - which has an integer
//      priority, a flag mask and two optional arguments in one interface -
//      was the second slot family to hit it. ui was the third: an
//      *optional* handle argument (control_find_by_id's root, "search from
//      the top" when absent) had no accessor here at all, and the gap's own
//      first draft was hand-rolled at the call site as exactly the
//      `lua_isnoneornil(state, i) ? default : Check...` spelling the
//      opening paragraph of this rule exists to delete. CheckOptArgHandle
//      below closes it, single-type and fixed-phrase like every other
//      optional accessor - ui_v2.h alone has more optional-root slots
//      waiting (control_find_by_path, the parent and child queries) that
//      never got bound this task but will want the same accessor.

#include "../../include/wotbmod/base.h"

extern "C" {
#include "../../third_party/lua/lua.h"
}

#include <functional>
#include <string>

namespace wotbmod {
namespace lua {

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

// A sentence a mod author can act on for every code base.h defines. Never a
// number on its own: "7" tells a script author nothing, "permission denied:
// this script's manifest does not grant it" tells them what to edit.
//
// The returned pointer is a static string literal for every code the ABI
// declares. Only for a code outside the ABI does it point at a per-thread
// buffer that the next call on the same thread overwrites, so a caller must
// copy the text before calling again - every caller in this file pushes it
// into Lua immediately, which copies.
const char* ResultMessage(WotbModV3Result result);

// The depth of the Lua stack at a moment in time: taken by a binding before it
// pushes anything, handed to PushResultWith afterwards.
//
// A distinct type rather than a bare int, deliberately. PushResultWith used to
// take a *count* of values already pushed, which every one of several hundred
// call sites would have had to state correctly by hand. A count that is wrong
// by one is invisible twice over: the number looks plausible in review, and
// the values a script receives can still be right while the stack underneath
// them is not, so a test that checks return values passes over a corrupted
// stack. A mark cannot disagree with reality, because it is measured rather
// than asserted - and because it has its own type, a leftover literal count no
// longer compiles.
struct StackMark {
    int top;
};

inline StackMark MarkStack(lua_State* state) {
    StackMark mark;
    mark.top = lua_gettop(state);
    return mark;
}

// Pushes the outcome of a call that has nothing else to return: `true` on
// success, `nil, message` on failure. Returns how many values it pushed.
//
// context, when given, is prefixed to the message: "storage.get_json: not
// found". The generator fills it with the slot's own name. One flat table of
// messages cannot say which of 46 interfaces produced a bare "not found", and
// the slot name is the piece that is actually missing.
int PushResult(lua_State* state, WotbModV3Result result,
               const char* context = nullptr);

// The same, for a call that pushed its return values before it knew whether
// the call succeeded - the usual shape, since the out-parameters are only
// meaningful once the result is OK.
//
//   const StackMark mark = MarkStack(state);
//   PushHandle(state, control, kHandleUiControl);
//   return PushResultWith(state, result, mark, "ui.create_control");
//
// On success, everything pushed since the mark is the result. On failure the
// stack is cut back to the mark and replaced by the `nil, message` pair.
//
// The cut is not about what the script receives: a lua_CFunction that returns
// 2 delivers only the top two values, and luaD_poscall discards whatever is
// below them. It is about the stack this code leaves behind. Cutting to a
// measured mark restores the stack exactly as the binding found it and cannot
// underflow into the caller's own values, which a hand-written lua_pop(n) can
// and, across hundreds of call sites, eventually would.
//
// Nothing pushed since the mark, with a successful result, pushes `true`, so
// rule 2 holds even for a slot that returns nothing.
int PushResultWith(lua_State* state, WotbModV3Result result, StackMark mark,
                   const char* context = nullptr);

// nil, "argument 2: expected a wotb.control handle, got wotb.subscription".
// Returns 2. Every argument a binding rejects goes through here so the message
// is the same shape across all 589 slots, and so it names both what was wanted
// and what actually arrived - the difference between a bug a mod author fixes
// in a minute and one they file against the host.
int PushArgumentError(lua_State* state, int index, const char* expected);

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

// Rule 6. Each of these reads the argument at index and, on success, returns
// true with nothing pushed and the value written to out (out may be null if
// a slot only needs the pass/fail). On failure each pushes `nil, message`
// itself, through PushArgumentError, with a fixed "expected" phrase owned by
// the accessor rather than typed at the call site - so a slot's only job on
// failure is to propagate what already happened:
//
//   const char* key = nullptr;
//   if (!CheckArgString(state, 1, &key)) return 2;
//
// Unlike CheckHandle below, which returns only a bool and leaves the message
// to the caller (because a *required* handle check legitimately needs to
// name which handle type was expected, and a caller composing that by hand
// is unavoidable there), a plain string, byte string or integer has exactly
// one sensible "expected" phrase regardless of which slot is asking for it,
// so there is nothing for a call site to add by naming its own - and
// CheckOptArgHandle below settles on one fixed phrase per call site the same
// way, for the optional case.

// A required string, read strictly: LUA_TSTRING only, no numeric coercion -
// a key silently becoming "42" from the number 42 is a worse surprise than
// an argument error. *out is a pointer into the Lua string's own storage,
// valid for as long as that value stays reachable (an argument does, for the
// life of the call), so nothing here copies it.
bool CheckArgString(lua_State* state, int index, const char** out);

// The byte-safe form: out_size is the exact length, so an embedded zero
// byte in a binary payload (WotbModV3ConstBuffer's contents, not a C
// string) is neither lost nor mistaken for a terminator. Use this for a
// value that is binary; CheckArgString for one that is text.
bool CheckArgBytes(lua_State* state, int index, const char** out_data,
                   size_t* out_size);

// A required integer, read strictly: LUA_TNUMBER whose value lua_isinteger
// confirms is a genuine integer subtype.
//
// This used to say lua_tointeger "silently truncates" a non-integral float,
// and that is not what it does - checked against the vendored source, not
// remembered. lua_tointeger goes through luaV_flttointeger in F2Ieq mode
// (lvm.c:123-131), which *fails* on a value that is not already integral and
// returns 0 with no conversion. So get_path(2.5) was never in danger of
// arriving as path kind 2; it would have arrived as path kind 0, which is a
// different bug with the same fix but a different explanation, and a file a
// generator reads several hundred times does not get to be approximately
// right about the API it is wrapping.
//
// What this accessor actually guards, both of which lua_tointeger alone would
// let through:
//
//   - numeric *strings*. lua_tointeger applies string coercion (luaV_tointeger
//     -> l_strton), so get_path("2") reads as 2. LUA_TNUMBER-only refuses it,
//     mirroring CheckArgString's refusal of the number 42 as a key.
//   - integral-valued *floats*. lua_tointeger(2.0) succeeds - 2.0 is integral,
//     so F2Ieq is satisfied - but lua_isinteger is false for it, because the
//     value's subtype is float. So this refuses get_path(2.0) as well as
//     get_path(2.5). That is deliberate and worth stating outright, because it
//     is the one case where this accessor is stricter than Lua itself: a
//     script author who computes an index with `/` gets an argument error
//     rather than silence, and `//` or math.floor is the fix.
bool CheckArgInteger(lua_State* state, int index, lua_Integer* out);

// A required number, integral or not - a coordinate, a duration, a volume.
// Still LUA_TNUMBER only: a numeric string is refused here exactly as it is
// by CheckArgString's mirror-image strictness, so "1e400" cannot arrive as
// an infinity nobody asked for.
bool CheckArgNumber(lua_State* state, int index, lua_Number* out);

// A required boolean, read strictly: LUA_TBOOLEAN only. Deliberately not
// lua_toboolean's truthiness test, which accepts every value in the language
// and therefore can never fail - a table passed where a flag belongs would
// silently mean `true`, and the mistake would surface as behaviour rather
// than as an argument error.
bool CheckArgBoolean(lua_State* state, int index, bool* out);

// A required callable, for the one direction of travel the rest of this file
// does not cover: a value the host keeps and calls later. LUA_TFUNCTION
// accepts a Lua function or a C one and refuses everything else, including a
// table with a __call metamethod that lua_pcall itself would honour - a
// callback is stored in the registry now and invoked from a client thread
// much later, so "it will probably turn out to be callable" is not a good
// enough answer at the point of subscription. Nothing is written out: the
// value stays where the caller can luaL_ref it.
bool CheckArgFunction(lua_State* state, int index);

// The set of values one enum-typed argument accepts, and the phrase that
// names them when one of them is not what arrived. Both halves live next to
// the slot that uses them, so the generator emits one array per ABI enum
// rather than one hand-written switch per slot.
struct EnumValues {
    const lua_Integer* values;
    size_t count;
    const char* expected;
};

// Builds an EnumValues from an array without the caller restating its
// length. Same reasoning as StackMark: a hand-typed count that is wrong by
// one silently narrows or widens what a slot will accept, and there is
// nothing at the call site for a reviewer to compare it against.
template <size_t N>
constexpr EnumValues Enum(const lua_Integer (&values)[N],
                          const char* expected) {
    return EnumValues{values, N, expected};
}

// An integer that must also be one of a named set of values. Type first
// (CheckArgInteger's own strictness, so 2.5 never reaches the value test),
// then membership - and exactly one refusal is pushed either way, never two
// pairs stacked on each other.
bool CheckArgEnum(lua_State* state, int index, const EnumValues& allowed,
                  lua_Integer* out);

// An integer used as a bit mask: every bit set in it must appear in
// allowed_mask, and it may not be negative. An enum's values are a set, a
// mask's are a union, so a mask cannot go through CheckArgEnum without
// enumerating all 2^n combinations - but an unchecked mask hands the client
// bits this ABI never defined, which is precisely the "plausible-looking
// value that is not one the ABI declares" CheckArgEnum exists to stop.
bool CheckArgFlags(lua_State* state, int index, lua_Integer allowed_mask,
                   const char* expected, lua_Integer* out);

// The optional form of each accessor above. An argument that is absent -
// either past the end of the call or explicitly nil, which are the same
// thing to a Lua caller writing f(a, nil, c) - yields the fallback and
// pushes nothing. An argument that is *present* but of the wrong type is a
// mistake, not an omission, and is refused exactly as the required form
// refuses it.
//
// Absence is deliberately not spelled at the call site as
// `lua_isnoneornil(state, i) ? default : Check...`: that spelling has to be
// got right once per optional argument across several hundred slots, and it
// reads as if nil and none were two cases rather than one.
bool CheckOptArgString(lua_State* state, int index, const char* fallback,
                       const char** out);
bool CheckOptArgBytes(lua_State* state, int index, const char* fallback,
                      size_t fallback_size, const char** out_data,
                      size_t* out_size);
bool CheckOptArgInteger(lua_State* state, int index, lua_Integer fallback,
                        lua_Integer* out);
bool CheckOptArgNumber(lua_State* state, int index, lua_Number fallback,
                       lua_Number* out);
bool CheckOptArgBoolean(lua_State* state, int index, bool fallback, bool* out);
bool CheckOptArgEnum(lua_State* state, int index, const EnumValues& allowed,
                     lua_Integer fallback, lua_Integer* out);
bool CheckOptArgFlags(lua_State* state, int index, lua_Integer allowed_mask,
                      const char* expected, lua_Integer fallback,
                      lua_Integer* out);

// A required single-type handle argument: rule 6's member for the shape rule
// 6 was written about, and for most of this branch the one member that did not
// exist.
//
// CheckHandle below is the primitive - it answers true or false and never
// pushes, because a binding accepting *more than one* handle type genuinely
// has to compose its own "expected" phrase. But a slot that wants exactly one
// handle type has exactly one sensible refusal, and there were five identical
// hand-written copies of it: storage's transaction, ui's control and slot,
// events' subscription and dispatch. Each was CheckHandle, then
// PushArgumentError with a literal phrase, then `return false` - the
// per-slot decision this whole file exists to delete, sitting under a comment
// that cited a note here saying handle arguments were exempt. That note is
// gone; this is what replaced it. The generator would otherwise have
// replicated the copy, not the primitive.
//
// type_name is one of the kHandle* constants; expected is the phrase the
// refusal names, owned once per handle type at the interface that uses it
// rather than at each of its slots.
bool CheckArgHandle(lua_State* state, int index, const char* type_name,
                    const char* expected, WotbModV3Handle* out);

// The optional form - "search from the top" when control_find_by_id's root is
// absent or nil, and every future slot with the same shape (ui_v2.h alone has
// more: control_find_by_path, the parent and child queries). fallback is
// almost always WOTBMOD_V3_INVALID_HANDLE, but is a parameter rather than
// baked in so this reads the same as every other CheckOptArg* signature above
// instead of being the one exception with an implicit default.
//
// Not composed at the call site as CheckHandle plus
// `lua_isnoneornil(state, i) ? default : Check...`: that is precisely the
// spelling this rule's opening paragraph exists to delete, and a handle
// argument does not get to be the one type that keeps it merely because
// CheckHandle itself predates this rule.
bool CheckOptArgHandle(lua_State* state, int index, const char* type_name,
                       const char* expected, WotbModV3Handle fallback,
                       WotbModV3Handle* out);

// ---------------------------------------------------------------------------
// Handle type names
// ---------------------------------------------------------------------------

// One constant per WotbModV3HandleType. Both the push site and the check site
// name one of these; neither types the literal. A misspelled literal mints a
// handle that no CheckHandle will ever accept, and nothing but a test of that
// exact pair would catch it - so the names exist in one place where a typo is
// a compile error instead. This is 33 lines now and unaddable once a generator
// has emitted several hundred call sites around hand-typed strings.
//
// Compared by content, never by pointer: the generator emits these from a
// table and a translation unit may hold its own copy of the bytes.
inline constexpr char kHandleMod[] = "wotb.mod";
inline constexpr char kHandleResource[] = "wotb.resource";
inline constexpr char kHandleUiControl[] = "wotb.control";
inline constexpr char kHandleUiSlot[] = "wotb.slot";
inline constexpr char kHandleSceneEntity[] = "wotb.scene_entity";
inline constexpr char kHandleScene[] = "wotb.scene";
inline constexpr char kHandleAudio[] = "wotb.audio";
inline constexpr char kHandleSoundEvent[] = "wotb.sound_event";
inline constexpr char kHandleRenderResource[] = "wotb.render_resource";
inline constexpr char kHandleCamera[] = "wotb.camera";
inline constexpr char kHandleEntity[] = "wotb.entity";
inline constexpr char kHandleProjectile[] = "wotb.projectile";
inline constexpr char kHandleArchive[] = "wotb.archive";
inline constexpr char kHandleTask[] = "wotb.task";
inline constexpr char kHandleTimer[] = "wotb.timer";
inline constexpr char kHandleHttpRequest[] = "wotb.http_request";
inline constexpr char kHandleHook[] = "wotb.hook";
inline constexpr char kHandleSubscription[] = "wotb.subscription";
inline constexpr char kHandleStyleOverride[] = "wotb.style_override";
inline constexpr char kHandleInputAction[] = "wotb.input_action";
inline constexpr char kHandleProfilerSpan[] = "wotb.profiler_span";
inline constexpr char kHandleDiagnosticScope[] = "wotb.diagnostic_scope";
inline constexpr char kHandleCapabilitySubscription[] =
    "wotb.capability_subscription";
inline constexpr char kHandleLifecycleCleanup[] = "wotb.lifecycle_cleanup";
inline constexpr char kHandleEventSubscription[] = "wotb.event_subscription";
inline constexpr char kHandleIntermodExport[] = "wotb.intermod_export";
inline constexpr char kHandleIntermodSubscription[] =
    "wotb.intermod_subscription";
inline constexpr char kHandleRenderCallback[] = "wotb.render_callback";
inline constexpr char kHandleInputBinding[] = "wotb.input_binding";
inline constexpr char kHandleResourceMount[] = "wotb.resource_mount";
inline constexpr char kHandleAudioOverride[] = "wotb.audio_override";
inline constexpr char kHandleProjectileStyle[] = "wotb.projectile_style";
inline constexpr char kHandleUiEventSubscription[] =
    "wotb.ui_event_subscription";

// Not one of the 33 above: a WotbModV3Token (storage's begin_transaction,
// and any later interface that hands out an opaque token the same way) has
// no entry in WotbModV3HandleType. Rule 3 does not carve out an exception for
// that gap - "a number is forgeable from inside the sandbox" is exactly as
// true of a transaction id as of a control id, and a forged one would let a
// script commit or roll back a transaction it never opened. Boxed with
// PushHandle/CheckHandle exactly like the 33, under its own name, the same
// way any future non-HandleType token should be.
inline constexpr char kHandleStorageTransaction[] = "wotb.storage_transaction";

// The same, for the WotbModV3Token an event dispatch is identified by:
// events' stop_propagation, get_dispatch_info, get_thread, get_timestamp and
// get_context all take one, and a forged one would let a script stop a
// dispatch belonging to another mod's handler. Its own name rather than
// kHandleStorageTransaction's, so a transaction cannot be passed to
// stop_propagation: CheckHandle compares the name, which is the whole point
// of there being one per meaning rather than one per underlying C type.
inline constexpr char kHandleEventDispatch[] = "wotb.event_dispatch";

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

// Handles are full userdata carrying the value plus a type name, with a
// metatable this host owns. A number would be forgeable: a script could pass
// any integer and address an object it never created. Userdata cannot be
// manufactured from inside the sandbox, and the type name stops a control
// handle being passed where a subscription token belongs.
//
// Pushes exactly one value, and is the shape a *required* handle uses:
//
//   PushHandle(state, control, kHandleUiControl);
//   return PushResultWith(state, result, mark, "ui.create_control");
//
// WOTBMOD_V3_INVALID_HANDLE is pushed as nil rather than as a box around zero:
// a live-looking object that is not alive is the worse of the two failures.
// On a required slot that nil is the honest answer - the client answered OK
// and produced nothing, so the script did not get what it asked for and rule 2
// says the first value must be falsy. A slot where absence is *expected* must
// use PushOptionalHandle instead, or it will report failures that did not
// happen.
void PushHandle(lua_State* state, WotbModV3Handle handle,
                const char* type_name);

// Pushes two values, `true` followed by the handle or nil. The shape for a
// slot where "there isn't one" is a successful answer rather than a failure.
void PushOptionalHandle(lua_State* state, WotbModV3Handle handle,
                        const char* type_name);

// The same box as PushHandle, for a WotbModV3Token rather than a
// WotbModV3Handle - begin_transaction and any later slot that hands out an
// opaque token the ABI defines outside WotbModV3HandleType. Deliberately
// NOT "PushHandle(state, static_cast<WotbModV3Handle>(token), type_name)":
// PushHandle treats the value 0 as WOTBMOD_V3_INVALID_HANDLE and pushes nil
// for it, and that sentinel is a real, ABI-level promise about
// WotbModV3Handle (base.h: `#define WOTBMOD_V3_INVALID_HANDLE ((uint64_t)0)`)
// that WotbModV3Token never makes. Nothing in the ABI reserves 0 as an
// invalid token, and a conforming client with a zero-based transaction
// counter could legitimately hand out 0 as its first one - through
// PushHandle that turns a successful begin_transaction into `nil`, rule 2's
// own named failure mode, for the one client implementation a test with a
// hard-coded nonzero mock token would never see. PushToken boxes the value
// unconditionally: for a token, there is no value that looks alive but
// is not.
void PushToken(lua_State* state, WotbModV3Token token, const char* type_name);

// Reads a handle of exactly type_name. Returns false - it never raises - for
// anything else, including a handle of another type, so a binding can answer
// with the `nil, message` shape rather than a Lua error. out may be null.
bool CheckHandle(lua_State* state, int index, const char* type_name,
                 WotbModV3Handle* out);

// The type name of the handle at index, or nullptr when the value is not a
// handle this host pushed. Used by PushArgumentError; exposed because a
// binding that accepts more than one handle type needs it too.
const char* HandleTypeName(lua_State* state, int index);

// ---------------------------------------------------------------------------
// Vectors
// ---------------------------------------------------------------------------

// There is no PushVec2/CheckVec2 here, and there was for a while: a table
// with x and y, one value in and one value out, so a slot taking two
// vectors would not turn into four positional arguments. ui - the first
// interface with any Vec2 slots at all - never called either helper.
// control_set_position/control_set_size/control_set_anchor/control_set_pivot
// take two raw numbers and control_get_position/control_get_size return two
// raw values, because that is the shape a round trip reads best in at the
// call site (`control_set_size(c, 200, 80)`, `local w, h =
// control_get_size(c)`), and every one of the four ABI Vec2-typed slots this
// host has bound so far agreed. A helper nothing calls is worse than no
// helper: a generator reading this section would emit table-shaped
// arguments for all of them, contradicting the one shipped binding that
// actually uses a Vec2. So the rule this file states is the rule the code
// follows - a Vec2 *argument* is two numbers (CheckArgNumber twice, or
// CheckOptArgNumber twice for an optional one); a Vec2 *return* is two
// values (two pushes before PushResultWith's mark, exactly like any other
// multi-value getter in this file) - and PushVec2/CheckVec2 are deleted
// rather than kept "for later". Revive them the day a slot actually needs a
// composed table (nested struct fields, an array of vectors, anything a
// flat pair of numbers cannot express) - not before, because dead surface
// in a file whose whole purpose is removing per-slot decisions is worse
// than no surface.

// ---------------------------------------------------------------------------
// Sized strings
// ---------------------------------------------------------------------------

// Every string the ABI returns uses the same protocol: pass a buffer and its
// size, get back either the value or WOTBMOD_V3_E_BUFFER_TOO_SMALL with the
// size it needs written through inout_size. Around forty slots do this.
//
// The callable binds whatever else that particular slot needs - the mod
// handle, a key, a path kind - so a binding site reads:
//
//   return PushSizedString(state, [&](char* buffer, uint32_t* size) {
//       return api->get_json(mod, key, buffer, size);
//   });
//
// (The brief sketched this as a bare function pointer plus a void* context.
// The real slots differ in what they bind - get_json takes a key, get_path a
// path kind - so a callable is what actually serves them. On 32-bit MSVC a
// lambda capturing three words fits std::function's small-object buffer, so
// this costs no allocation.)
using SizedStringFetch = std::function<WotbModV3Result(char* buffer,
                                                       uint32_t* inout_size)>;

// Runs the two-call buffer dance. Nothing else in this codebase may run it: a
// second copy is a second place to get the terminator, the retry, or the size
// units wrong.
//
// The bool is the signal, not the emptiness of either output. An empty string
// is a perfectly good value, and under memory exhaustion even building the
// error message can fail - so a false return with an empty out_error is still
// a failure. noexcept per rule 5: this runs inside a lua_CFunction, where an
// escaping exception is a dead process rather than a caught error.
bool FetchSizedString(const SizedStringFetch& fetch, std::string* out_value,
                      std::string* out_error) noexcept;

// The same read, pushed: the string on success, nil and the reason on failure.
// Returns how many values it pushed.
//
// Every std::string it uses is confined to a scope that closes before the
// first Lua call, per rule 5. The bytes handed to lua_pushlstring live either
// in a stack buffer or in a malloc block - storage with no destructor for a
// longjmp to skip. A raise from that final push leaks the malloc block, which
// only exists for values over 512 bytes and only in an out-of-memory teardown;
// that is the residual cost of the ABI's copy-out protocol and is preferable
// to the undefined behaviour of unwinding past a live std::string.
//
// The callable is taken by rvalue reference and emptied before the push. Both
// halves of that are load bearing, and neither buys as much as it looks like.
//
// *Emptied*, because a binding site is one expression -
// `return PushSizedString(state, [&](char* b, uint32_t* n) { ... });` - and
// the std::function it builds lives to the end of that full expression, which
// includes the push inside this function. Left alone it would be an object
// with a live destructor sitting in the caller's frame at the moment Lua can
// longjmp. Emptying it means the destructor that gets skipped has nothing to
// do.
//
// *By rvalue reference*, because that is what turns "pass a prvalue" from a
// sentence into a compile error. Emptying the parameter is no help at all if
// the caller kept its own copy:
//
//     SizedStringFetch fetch = [&](char* b, uint32_t* n) { ... };
//     return PushSizedString(state, fetch);        // does not compile
//
// That lvalue would stay alive across the push whatever this function did to
// its own parameter. Pass the lambda directly. A caller that genuinely holds
// one must std::move it, and must expect it emptied.
//
// What neither of them buys - stated plainly, because a header that teaches
// hundreds of generated slots must not overclaim: the argument is *constructed* in the
// caller's frame, before this function's noexcept boundary begins. A callable
// whose captures do not fit std::function's small-object buffer (36 bytes on
// 32-bit MSVC) heap-allocates during that construction, and a bad_alloc thrown
// there escapes the generated lua_CFunction into Lua's C frames and terminates
// the process - rule 5's own failure, in the one spot rule 5 cannot reach.
// Unreachable with today's call sites, which capture about three words.
//
// So the rule a generator must follow is: a fetch closure captures a handful
// of pointers and nothing larger. Bounding the hazard is what std::function
// costs; removing it would mean going back to a function pointer plus a void*
// context, which is trivially destructible and cannot allocate, at the price
// of a context struct and a trampoline at every one of those call sites.
//
// context, when given, is prefixed to the message exactly as PushResult's
// own context does - "storage.get_json: not found" - and for the same
// reason. Defaulted to nullptr so every existing call site (and every test
// probe built directly against this function) still compiles unchanged.
int PushSizedString(lua_State* state, SizedStringFetch&& fetch,
                    const char* context = nullptr) noexcept;

// ---------------------------------------------------------------------------
// Byte buffers
// ---------------------------------------------------------------------------

// The other size-and-retry protocol the ABI uses, for a value that is
// genuinely binary - a WotbModV3Buffer's contents - rather than the
// NUL-terminated text FetchSizedString/PushSizedString exist for.
// Structurally the identical callable (a buffer and an in/out size), kept as
// its own type name so a call site states which contract it is promising
// rather than which mechanics happen to run.
using ByteBufferFetch = std::function<WotbModV3Result(char* buffer,
                                                       uint32_t* inout_size)>;

// FetchSizedString's own two-call dance, exactly - both run the same shared
// loop (constants, retry bound, growth headroom, the noexcept/try-catch
// boundary) - minus the one step that would corrupt binary data:
// FetchSizedString stops the result at the first embedded '\0', because the
// ABI's size for a text value includes a terminator not every slot honours;
// FetchByteBuffer does not, because a byte buffer's embedded zero bytes are
// real data and there is no terminator convention to honour. See rule 4.
bool FetchByteBuffer(const ByteBufferFetch& fetch, std::string* out_value,
                     std::string* out_error) noexcept;

// The same read, pushed: the exact bytes as a Lua string on success -
// lua_pushlstring takes an explicit length, so an embedded zero survives the
// push as well as the fetch - nil and the reason on failure. Same rvalue-
// reference rule as PushSizedString, for the same reason: pass a lambda
// literal, not an lvalue. context behaves exactly as PushSizedString's does.
//
// This and PushSizedString are one function with one flag underneath (see
// PushFetched in the .cpp). They were character-for-character identical
// apart from which Fetch* they called: rule 4 collapsed the duplication at
// the retry loop, and it grew back one level up, in the buffer-tier and
// error-message dance both of them wrap that loop in. The pair stays two
// names because a call site should state which contract it is promising -
// text that stops at a terminator, or bytes that do not - and one function
// with a bool at every call site would state the mechanics instead.
int PushByteBuffer(lua_State* state, ByteBufferFetch&& fetch,
                   const char* context = nullptr) noexcept;

}  // namespace lua
}  // namespace wotbmod

#endif  // WOTBMOD_LUA_CONVERT_H_
