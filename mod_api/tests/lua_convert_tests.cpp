#include "../loader/lua/lua_convert.h"

extern "C" {
#include "../third_party/lua/lauxlib.h"
#include "../third_party/lua/lualib.h"
}

#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace {
uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
void Check(bool condition, const char* label) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::printf("FAIL: %s\n", label);
}

// lua_isstring is also true for numbers - Lua coerces them - so a check built
// on it passes when a binding returned 7 where it promised a sentence. Every
// "is this the message" assertion in this file goes through here instead.
bool IsString(lua_State* state, int index) {
    return lua_type(state, index) == LUA_TSTRING;
}

bool Mentions(lua_State* state, int index, const char* needle) {
    if (!IsString(state, index)) return false;
    return std::strstr(lua_tostring(state, index), needle) != nullptr;
}

// Runs a chunk that calls a C function registered as `probe`.
bool RunWithProbe(lua_State* state, lua_CFunction probe, const char* chunk) {
    lua_pushcfunction(state, probe);
    lua_setglobal(state, "probe");
    if (luaL_loadstring(state, chunk) != LUA_OK) return false;
    return lua_pcall(state, 0, LUA_MULTRET, 0) == LUA_OK;
}

int ProbeOk(lua_State* state) {
    return wotbmod::lua::PushResult(state, WOTBMOD_V3_OK);
}
int ProbeDenied(lua_State* state) {
    return wotbmod::lua::PushResult(state, WOTBMOD_V3_E_PERMISSION_DENIED);
}
int ProbeDeniedWithContext(lua_State* state) {
    return wotbmod::lua::PushResult(state, WOTBMOD_V3_E_PERMISSION_DENIED,
                                    "storage.get_json");
}
int ProbeHandle(lua_State* state) {
    wotbmod::lua::PushHandle(state, 4242u, wotbmod::lua::kHandleUiControl);
    return 1;
}
int ProbeTakesControl(lua_State* state) {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!wotbmod::lua::CheckHandle(state, 1, wotbmod::lua::kHandleUiControl,
                                   &handle)) {
        lua_pushnil(state);
        lua_pushstring(state, "expected a wotb.control handle");
        return 2;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(handle));
    return 1;
}

// The type name must be compared by content, not by pointer: a generator emits
// these from a table and each translation unit may hold its own copy of the
// bytes. This builds the name at runtime so no compiler can merge it with the
// constant, then checks against that copy.
int ProbeTakesControlByCopiedName(lua_State* state) {
    char name[64] = {};
    strcpy_s(name, sizeof(name), wotbmod::lua::kHandleUiControl);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!wotbmod::lua::CheckHandle(state, 1, name, &handle)) {
        return wotbmod::lua::PushArgumentError(state, 1,
                                               "a wotb.control handle");
    }
    lua_pushinteger(state, static_cast<lua_Integer>(handle));
    return 1;
}

// A handle of a different type, and the same numeric value under a different
// type name: both must fail to pass where a wotb.control belongs.
int ProbeSubscriptionHandle(lua_State* state) {
    wotbmod::lua::PushHandle(state, 4242u, wotbmod::lua::kHandleSubscription);
    return 1;
}
int ProbeOtherControlHandle(lua_State* state) {
    wotbmod::lua::PushHandle(state, 4243u, wotbmod::lua::kHandleUiControl);
    return 1;
}
int ProbeInvalidHandle(lua_State* state) {
    wotbmod::lua::PushHandle(state, WOTBMOD_V3_INVALID_HANDLE,
                             wotbmod::lua::kHandleUiControl);
    return 1;
}
int ProbeTakesSubscription(lua_State* state) {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!wotbmod::lua::CheckHandle(state, 1, wotbmod::lua::kHandleSubscription,
                                   &handle)) {
        return wotbmod::lua::PushArgumentError(state, 1,
                                               "a wotb.subscription handle");
    }
    lua_pushinteger(state, static_cast<lua_Integer>(handle));
    return 1;
}

// The three shapes an optional-handle slot can produce. "The current camera,
// if there is one" is a successful call whether or not there is one, so the
// absent case must not read as a failure.
int ProbeOptionalPresent(lua_State* state) {
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    wotbmod::lua::PushOptionalHandle(state, 4242u,
                                     wotbmod::lua::kHandleCamera);
    return wotbmod::lua::PushResultWith(state, WOTBMOD_V3_OK, mark);
}
int ProbeOptionalAbsent(lua_State* state) {
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    wotbmod::lua::PushOptionalHandle(state, WOTBMOD_V3_INVALID_HANDLE,
                                     wotbmod::lua::kHandleCamera);
    return wotbmod::lua::PushResultWith(state, WOTBMOD_V3_OK, mark);
}
int ProbeOptionalFailed(lua_State* state) {
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    wotbmod::lua::PushOptionalHandle(state, WOTBMOD_V3_INVALID_HANDLE,
                                     wotbmod::lua::kHandleCamera);
    return wotbmod::lua::PushResultWith(state, WOTBMOD_V3_E_NOT_SUPPORTED,
                                        mark);
}
// The required shape, for contrast: the handle itself is the first value.
int ProbeRequiredHandle(lua_State* state) {
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    wotbmod::lua::PushHandle(state, 4242u, wotbmod::lua::kHandleUiControl);
    return wotbmod::lua::PushResultWith(state, WOTBMOD_V3_OK, mark);
}

// A getter that pushed its values before it knew whether the call succeeded.
int ProbeTwoValuesOk(lua_State* state) {
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    lua_pushinteger(state, 7);
    lua_pushinteger(state, 8);
    return wotbmod::lua::PushResultWith(state, WOTBMOD_V3_OK, mark);
}
int ProbeTwoValuesFailed(lua_State* state) {
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    lua_pushinteger(state, 7);
    lua_pushinteger(state, 8);
    return wotbmod::lua::PushResultWith(state, WOTBMOD_V3_E_NOT_FOUND, mark);
}
int ProbeNothingOk(lua_State* state) {
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    return wotbmod::lua::PushResultWith(state, WOTBMOD_V3_OK, mark);
}

// The stack depth a probe saw on entry and left behind, so a test can assert
// on the stack itself rather than only on the values a script received. A
// binding that miscounts what it pushed still returns plausible-looking
// values; only the depth gives it away.
int g_top_on_entry = 0;
int g_top_on_exit = 0;
int g_values_returned = 0;

// Pushes three values speculatively, then takes the failure path.
int ProbeStackRestoredOnFailure(lua_State* state) {
    g_top_on_entry = lua_gettop(state);
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    lua_pushinteger(state, 7);
    lua_pushinteger(state, 8);
    lua_pushliteral(state, "half built");
    g_values_returned =
        wotbmod::lua::PushResultWith(state, WOTBMOD_V3_E_NOT_FOUND, mark);
    g_top_on_exit = lua_gettop(state);
    return g_values_returned;
}

// The same shape on the success path: nothing of the caller's is consumed and
// nothing extra is left behind.
int ProbeStackKeptOnSuccess(lua_State* state) {
    g_top_on_entry = lua_gettop(state);
    const wotbmod::lua::StackMark mark = wotbmod::lua::MarkStack(state);
    lua_pushinteger(state, 7);
    lua_pushinteger(state, 8);
    lua_pushliteral(state, "third");
    g_values_returned =
        wotbmod::lua::PushResultWith(state, WOTBMOD_V3_OK, mark);
    g_top_on_exit = lua_gettop(state);
    return g_values_returned;
}

// Counts every crossing so the tests can say how many the dance cost.
uint32_t g_fetch_calls = 0u;

// Behaves exactly like WotbModV3StorageApiV1::get_json: the size it reports
// includes the terminator, a null or short buffer is BUFFER_TOO_SMALL, and the
// size it wants is written back through inout_size.
WotbModV3Result FetchValue(const char* value, char* buffer,
                           uint32_t* inout_size) {
    ++g_fetch_calls;
    if (!inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const uint32_t needed = static_cast<uint32_t>(std::strlen(value)) + 1u;
    if (!buffer || *inout_size < needed) {
        *inout_size = needed;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, value, needed);
    *inout_size = needed;
    return WOTBMOD_V3_OK;
}

int ProbeSizedString(lua_State* state) {
    return wotbmod::lua::PushSizedString(
        state, [](char* buffer, uint32_t* inout_size) {
            return FetchValue("{\"probe\":1}", buffer, inout_size);
        });
}
int ProbeSizedStringDenied(lua_State* state) {
    return wotbmod::lua::PushSizedString(
        state, [](char*, uint32_t*) {
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        });
}
// A value past the inline payload buffer, so the malloc path is exercised
// through Lua and not only through FetchSizedString.
int ProbeLargeSizedString(lua_State* state) {
    return wotbmod::lua::PushSizedString(
        state, [](char* buffer, uint32_t* inout_size) {
            static std::string large(4096u, 'x');
            return FetchValue(large.c_str(), buffer, inout_size);
        });
}

// A client that answers OK without writing anything. The inline buffer must
// have been zeroed, or the script receives whatever was on the stack.
WotbModV3Result FetchNothingButSucceed(char*, uint32_t* inout_size) {
    *inout_size = 32u;
    return WOTBMOD_V3_OK;
}

// The vendored Lua is built as C, so lua_pcall catches a longjmp and not a
// throw. An exception raised inside a lua_CFunction unwinds through Lua's C
// frames and terminates the process - a script would be able to kill the
// client by asking for a value the host cannot allocate. These two probes
// stand in for that: if the catch-alls are ever removed, this test binary
// does not fail, it dies.
WotbModV3Result FetchThatThrows(char*, uint32_t*) {
    throw std::bad_alloc();
}
int ProbeSizedStringThrows(lua_State* state) {
    return wotbmod::lua::PushSizedString(state, &FetchThatThrows);
}

// context, added in the storage fix round: prefixed to the message on
// failure exactly like PushResult's own context, and untouched on success.
int ProbeSizedStringDeniedWithContext(lua_State* state) {
    return wotbmod::lua::PushSizedString(
        state, [](char*, uint32_t*) { return WOTBMOD_V3_E_PERMISSION_DENIED; },
        "storage.get_json");
}

// ---------------------------------------------------------------------------
// Byte buffers - PushByteBuffer/FetchByteBuffer, added in the storage fix
// round. Deliberately mirrors the SizedString probes above one for one, so
// the one behavioural difference (no BoundedLength truncation) is the only
// thing that can explain a different result.
// ---------------------------------------------------------------------------

uint32_t g_byte_fetch_calls = 0u;

// Like FetchValue, but for exact-length binary data rather than a
// NUL-terminated C string: no strlen, the caller states the length.
WotbModV3Result FetchBytes(const char* data, size_t length, char* buffer,
                           uint32_t* inout_size) {
    ++g_byte_fetch_calls;
    if (!inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    const uint32_t needed = static_cast<uint32_t>(length);
    if (!buffer || *inout_size < needed) {
        *inout_size = needed;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, data, needed);
    *inout_size = needed;
    return WOTBMOD_V3_OK;
}

// The value FetchSizedString's own BoundedLength would truncate to 2 bytes
// ("AB"): this is the whole point of the probe.
int ProbeByteBuffer(lua_State* state) {
    static const char kValue[] = {'A', 'B', '\0', 'C', 'D'};   // 5 bytes
    return wotbmod::lua::PushByteBuffer(
        state, [](char* buffer, uint32_t* inout_size) {
            return FetchBytes(kValue, sizeof(kValue), buffer, inout_size);
        });
}
// Past the inline payload buffer, so the malloc path is exercised through
// FetchByteBuffer specifically, not only through FetchSizedString.
int ProbeLargeByteBuffer(lua_State* state) {
    return wotbmod::lua::PushByteBuffer(
        state, [](char* buffer, uint32_t* inout_size) {
            static const std::string large(4096u, 'y');
            return FetchBytes(large.data(), large.size(), buffer, inout_size);
        });
}
int ProbeByteBufferDenied(lua_State* state) {
    return wotbmod::lua::PushByteBuffer(
        state, [](char*, uint32_t*) {
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        },
        "storage.get_bytes");
}
WotbModV3Result FetchBytesThatThrows(char*, uint32_t*) {
    throw std::bad_alloc();
}
int ProbeByteBufferThrows(lua_State* state) {
    return wotbmod::lua::PushByteBuffer(state, &FetchBytesThatThrows);
}

// ---------------------------------------------------------------------------
// Tokens - PushToken, added in the storage fix round. WotbModV3Token has no
// WOTBMOD_V3_HANDLE-style invalid sentinel, so unlike PushHandle, the value
// 0 must box exactly as any other value does.
// ---------------------------------------------------------------------------

const char kTestTokenType[] = "wotb.test_token";

int ProbeTokenZero(lua_State* state) {
    wotbmod::lua::PushToken(state, 0u, kTestTokenType);
    return 1;
}
int ProbeTokenNonzero(lua_State* state) {
    wotbmod::lua::PushToken(state, 4242u, kTestTokenType);
    return 1;
}
int ProbeTakesTestToken(lua_State* state) {
    WotbModV3Handle out = WOTBMOD_V3_INVALID_HANDLE;
    if (!wotbmod::lua::CheckHandle(state, 1, kTestTokenType, &out)) {
        return wotbmod::lua::PushArgumentError(state, 1, "a test token");
    }
    // static_cast, not lua_Integer: the value can legitimately be 0, which
    // this must still report as "found", not as "absent" the way returning
    // a value through a boolean might invite.
    lua_pushinteger(state, static_cast<lua_Integer>(out));
    return 1;
}

// ---------------------------------------------------------------------------
// Arguments - CheckArgString/CheckArgBytes/CheckArgInteger, added in the
// storage fix round (rule 6). Each pushes its own refusal, so the probe's
// only job on failure is to propagate the 2 values already pushed.
// ---------------------------------------------------------------------------

int ProbeCheckArgString(lua_State* state) {
    const char* value = nullptr;
    if (!wotbmod::lua::CheckArgString(state, 1, &value)) return 2;
    lua_pushstring(state, value);
    return 1;
}
int ProbeCheckArgBytes(lua_State* state) {
    const char* data = nullptr;
    size_t length = 0u;
    if (!wotbmod::lua::CheckArgBytes(state, 1, &data, &length)) return 2;
    lua_pushlstring(state, data, length);
    return 1;
}
int ProbeCheckArgInteger(lua_State* state) {
    lua_Integer value = 0;
    if (!wotbmod::lua::CheckArgInteger(state, 1, &value)) return 2;
    lua_pushinteger(state, value);
    return 1;
}
int ProbeCheckArgNumber(lua_State* state) {
    lua_Number value = 0.0;
    if (!wotbmod::lua::CheckArgNumber(state, 1, &value)) return 2;
    lua_pushnumber(state, value);
    return 1;
}
int ProbeCheckArgBoolean(lua_State* state) {
    bool value = false;
    if (!wotbmod::lua::CheckArgBoolean(state, 1, &value)) return 2;
    // Pushed as a string, so the test can tell "it answered false" from "it
    // refused and pushed nil" without the two collapsing into one falsy.
    lua_pushstring(state, value ? "true" : "false");
    return 1;
}
int ProbeCheckArgFunction(lua_State* state) {
    if (!wotbmod::lua::CheckArgFunction(state, 1)) return 2;
    lua_pushliteral(state, "callable");
    return 1;
}

// The enum accessor, against a set that deliberately is not 0..n: the values
// an ABI enum takes are rarely contiguous, and an accessor that only checked
// a range would accept 1 here.
constexpr lua_Integer kProbeEnumValues[] = {-1000, 0, 100};
const wotbmod::lua::EnumValues kProbeEnum =
    wotbmod::lua::Enum(kProbeEnumValues, "LOWEST, NORMAL or HIGH");

int ProbeCheckArgEnum(lua_State* state) {
    lua_Integer value = 0;
    if (!wotbmod::lua::CheckArgEnum(state, 1, kProbeEnum, &value)) return 2;
    lua_pushinteger(state, value);
    return 1;
}
int ProbeCheckArgFlags(lua_State* state) {
    lua_Integer value = 0;
    if (!wotbmod::lua::CheckArgFlags(state, 1, 0x0F, "a mask of the four bits",
                                     &value)) {
        return 2;
    }
    lua_pushinteger(state, value);
    return 1;
}

// The optional forms. Each returns what it read, so a test can tell the
// fallback from a value that was actually given.
int ProbeOptString(lua_State* state) {
    const char* value = nullptr;
    if (!wotbmod::lua::CheckOptArgString(state, 1, "fallback", &value)) return 2;
    lua_pushstring(state, value);
    return 1;
}
int ProbeOptBytes(lua_State* state) {
    const char* data = nullptr;
    size_t length = 0u;
    if (!wotbmod::lua::CheckOptArgBytes(state, 1, "de\0ad", 5u, &data,
                                        &length)) {
        return 2;
    }
    lua_pushlstring(state, data, length);
    return 1;
}
int ProbeOptInteger(lua_State* state) {
    lua_Integer value = 0;
    if (!wotbmod::lua::CheckOptArgInteger(state, 1, 77, &value)) return 2;
    lua_pushinteger(state, value);
    return 1;
}
int ProbeOptNumber(lua_State* state) {
    lua_Number value = 0.0;
    if (!wotbmod::lua::CheckOptArgNumber(state, 1, 0.5, &value)) return 2;
    lua_pushnumber(state, value);
    return 1;
}
int ProbeOptBoolean(lua_State* state) {
    bool value = false;
    if (!wotbmod::lua::CheckOptArgBoolean(state, 1, true, &value)) return 2;
    lua_pushstring(state, value ? "true" : "false");
    return 1;
}
int ProbeOptEnum(lua_State* state) {
    lua_Integer value = 0;
    if (!wotbmod::lua::CheckOptArgEnum(state, 1, kProbeEnum, 0, &value)) {
        return 2;
    }
    lua_pushinteger(state, value);
    return 1;
}
int ProbeOptFlags(lua_State* state) {
    lua_Integer value = 0;
    if (!wotbmod::lua::CheckOptArgFlags(state, 1, 0x0F, "a mask", 4, &value)) {
        return 2;
    }
    lua_pushinteger(state, value);
    return 1;
}
// The required form, rule 6's CheckArgHandle. Same type name and phrase as
// ProbeOptHandle below, so the pair differ only in whether an absent argument
// is an omission or a refusal - which is the whole distinction between them.
int ProbeArgHandle(lua_State* state) {
    WotbModV3Handle value = WOTBMOD_V3_INVALID_HANDLE;
    if (!wotbmod::lua::CheckArgHandle(state, 1, wotbmod::lua::kHandleUiControl,
                                      "a wotb.control handle", &value)) {
        return 2;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(value));
    return 1;
}

// 9999, not 4242 (ProbeHandle's own value above): a distinct fallback so a
// test can tell "fell back" from "read the real handle" by the number
// alone, rather than the two cases happening to look the same.
int ProbeOptHandle(lua_State* state) {
    WotbModV3Handle value = WOTBMOD_V3_INVALID_HANDLE;
    if (!wotbmod::lua::CheckOptArgHandle(state, 1, wotbmod::lua::kHandleUiControl,
                                         "a wotb.control handle", 9999u,
                                         &value)) {
        return 2;
    }
    lua_pushinteger(state, static_cast<lua_Integer>(value));
    return 1;
}

// Every code the ABI defines must have a sentence of its own, not the
// numeric fallback. WOTBMOD_V3_E_TIMEOUT is the last one base.h declares.
bool EveryAbiCodeIsNamed() {
    for (int code = WOTBMOD_V3_OK; code <= WOTBMOD_V3_E_TIMEOUT; ++code) {
        const char* message =
            wotbmod::lua::ResultMessage(static_cast<WotbModV3Result>(code));
        if (!message || !*message) return false;
        if (std::strstr(message, "unexpected error") != nullptr) return false;
    }
    return true;
}
}  // namespace

int main() {
    lua_State* state = luaL_newstate();
    luaL_openlibs(state);

    Check(RunWithProbe(state, &ProbeOk, "return probe()") &&
              lua_toboolean(state, -1) == 1,
          "success is a bare true");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeDenied,
                       "local v, e = probe(); return v == nil and e") &&
              IsString(state, -1),
          "failure is nil plus a message");
    Check(Mentions(state, -1, "permission"),
          "the message names the reason, not a number");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeDeniedWithContext,
                       "local v, e = probe(); return e") &&
              Mentions(state, -1, "storage.get_json: ") &&
              Mentions(state, -1, "permission"),
          "and it can name the slot it came from, which one flat table of "
          "messages cannot");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeHandle, "return type(probe())") &&
              std::strcmp(lua_tostring(state, -1), "userdata") == 0,
          "a handle crosses as userdata, never a number");
    lua_settop(state, 0);

    // The point of userdata: a script cannot forge one from an integer.
    lua_pushcfunction(state, &ProbeHandle);
    lua_setglobal(state, "make");
    Check(RunWithProbe(state, &ProbeTakesControl,
                       "local h = make(); return probe(h)") &&
              lua_tointeger(state, -1) == 4242,
          "a real handle is accepted and unwraps to its value");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeTakesControl,
                       "local v, e = probe(4242); return e") &&
              IsString(state, -1),
          "an integer cannot pass as a handle");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeTakesControl,
                       "local v, e = probe({}); return e") &&
              IsString(state, -1),
          "a table cannot pass as a handle either");
    lua_settop(state, 0);

    // ---- the type name is load bearing, not decoration --------------------

    Check(RunWithProbe(state, &ProbeTakesControlByCopiedName,
                       "local h = make(); return probe(h)") &&
              lua_tointeger(state, -1) == 4242,
          "the type name is matched by content, so a second copy of the same "
          "bytes still works");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeTakesSubscription,
                       "local h = make(); local v, e = probe(h); return e") &&
              Mentions(state, -1, "wotb.subscription") &&
              Mentions(state, -1, "wotb.control"),
          "a control handle is refused where a subscription belongs, and the "
          "message names what was wanted and what arrived");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeTakesControl,
                       "local v, e = probe(nil); return e") &&
              IsString(state, -1),
          "nil cannot pass as a handle");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeInvalidHandle, "return probe() == nil") &&
              lua_toboolean(state, -1) == 1,
          "the ABI's invalid handle crosses as nil, not a live-looking object");
    lua_settop(state, 0);

    // Two userdata for one object are two userdata. Without __eq a script
    // could never tell that the handle an event delivered is the control it
    // created, because every push mints a fresh box.
    Check(RunWithProbe(state, &ProbeHandle,
                       "local a = make(); return a == probe()") &&
              lua_toboolean(state, -1) == 1,
          "two boxes around one handle compare equal");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOtherControlHandle,
                       "local a = make(); return a == probe()") &&
              lua_toboolean(state, -1) == 0,
          "two different handles of one type do not");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeSubscriptionHandle,
                       "local a = make(); return a == probe()") &&
              lua_toboolean(state, -1) == 0,
          "and neither do one number under two type names");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeHandle,
                       "return type(getmetatable(probe()))") &&
              std::strcmp(lua_tostring(state, -1), "string") == 0,
          "getmetatable answers the __metatable guard, not the table this "
          "host owns");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeHandle, "return tostring(probe())") &&
              Mentions(state, -1, "wotb.control") &&
              Mentions(state, -1, "4242"),
          "a handle prints as its type and its value");
    lua_settop(state, 0);

    // ---- required and optional handles ------------------------------------

    Check(RunWithProbe(state, &ProbeRequiredHandle,
                       "local h = probe(); return h ~= nil and "
                       "type(h) == 'userdata'") &&
              lua_toboolean(state, -1) == 1,
          "a required handle is the first value, so it is its own truthiness");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptionalPresent,
                       "local ok, h = probe(); return ok == true and "
                       "type(h) == 'userdata'") &&
              lua_toboolean(state, -1) == 1,
          "an optional handle that is present is true plus the handle");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptionalAbsent,
                       "local ok, h = probe(); return ok == true and "
                       "h == nil") &&
              lua_toboolean(state, -1) == 1,
          "an optional handle that is absent is true plus nil");
    lua_settop(state, 0);

    // The whole point of the two shapes. Under a bare nil this reads as a
    // failure that never happened, which is rule 2 broken.
    Check(RunWithProbe(state, &ProbeOptionalAbsent, "return not probe()") &&
              lua_toboolean(state, -1) == 0,
          "and `if not f() then` does not fire on it");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptionalFailed,
                       "local v, e = probe(); return v == nil and e") &&
              IsString(state, -1),
          "while a genuine failure of the same slot is still nil plus a "
          "message");
    lua_settop(state, 0);

    // ---- results that carry values ----------------------------------------

    Check(RunWithProbe(state, &ProbeTwoValuesOk,
                       "local a, b = probe(); return a + b") &&
              lua_tointeger(state, -1) == 15,
          "on success the values already pushed are the result");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeTwoValuesFailed,
                       "local v, e = probe(); return v == nil and e") &&
              IsString(state, -1),
          "on failure the half-built values are dropped, so nil and the "
          "message still land first");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeNothingOk, "return probe() == true") &&
              lua_toboolean(state, -1) == 1,
          "a call with nothing to return still answers true, never nothing");
    lua_settop(state, 0);

    // The stack itself, not just the values. A binding that got its own count
    // wrong returns values a script cannot tell apart from correct ones, so
    // checking the returned values would pass over a corrupt stack.
    g_top_on_entry = 0;
    g_top_on_exit = 0;
    g_values_returned = 0;
    Check(RunWithProbe(state, &ProbeStackRestoredOnFailure,
                       "local v, e = probe(1, 2, 3); return v == nil and e") &&
              IsString(state, -1),
          "a failure after three speculative pushes still answers nil first");
    Check(g_top_on_entry == 3 && g_values_returned == 2 &&
              g_top_on_exit == g_top_on_entry + 2,
          "and the stack is cut back to the mark, whatever was pushed past it");
    lua_settop(state, 0);

    g_top_on_entry = 0;
    g_top_on_exit = 0;
    g_values_returned = 0;
    Check(RunWithProbe(state, &ProbeStackKeptOnSuccess,
                       "local a, b, c = probe(1, 2, 3); return a + b") &&
              lua_tointeger(state, -1) == 15,
          "on success everything pushed past the mark is returned");
    Check(g_top_on_entry == 3 && g_values_returned == 3 &&
              g_top_on_exit == g_top_on_entry + 3,
          "and the count comes from the stack rather than from the call site");
    lua_settop(state, 0);

    Check(EveryAbiCodeIsNamed(),
          "every result code the ABI defines has a sentence of its own");
    // 31 rather than something like 4242: an unscoped enum's value range is
    // the smallest bit field holding its enumerators, five bits here, and
    // converting a value outside that range is undefined.
    Check(std::strcmp(wotbmod::lua::ResultMessage(
                          static_cast<WotbModV3Result>(31)),
                      "unexpected error 31") == 0,
          "and a code that is not in the ABI still says more than nothing");

    // There is no vectors test block here, deliberately: lua_convert.h no
    // longer declares PushVec2/CheckVec2 at all (see its own "Vectors"
    // section comment) - a Vec2 argument is CheckArgNumber twice and a Vec2
    // return is two pushed values, both already covered by the plain
    // CheckArgNumber/PushResultWith tests elsewhere in this file, so there
    // is nothing Vec2-specific left to test at the convert layer.

    // ---- the sized string dance -------------------------------------------

    std::string error;
    std::string got;
    const std::string large(4096u, 'x');

    g_fetch_calls = 0u;
    Check(wotbmod::lua::FetchSizedString(
              [&large](char* buffer, uint32_t* inout_size) {
                  return FetchValue(large.c_str(), buffer, inout_size);
              },
              &got, &error) &&
              got == large && error.empty(),
          "a value past the inline buffer is read whole");
    Check(g_fetch_calls == 2u,
          "and it cost exactly two calls: one to learn the size, one to read");

    g_fetch_calls = 0u;
    Check(wotbmod::lua::FetchSizedString(
              [](char* buffer, uint32_t* inout_size) {
                  return FetchValue("{\"probe\":1}", buffer, inout_size);
              },
              &got, &error) &&
              got == "{\"probe\":1}" && error.empty() && g_fetch_calls == 1u,
          "a value that fits the inline buffer costs one call, not two");

    Check(wotbmod::lua::FetchSizedString(
              [](char* buffer, uint32_t* inout_size) {
                  return FetchValue("", buffer, inout_size);
              },
              &got, &error) &&
              got.empty() && error.empty(),
          "an empty value is an empty string, not a failure");

    // The inline buffer is zeroed, so a client that answers OK without
    // writing hands the script an empty string rather than stack garbage.
    Check(wotbmod::lua::FetchSizedString(&FetchNothingButSucceed, &got,
                                         &error) &&
              got.empty(),
          "a client that succeeds without writing yields nothing, not "
          "whatever was on the stack");

    Check(!wotbmod::lua::FetchSizedString(
              [](char*, uint32_t*) { return WOTBMOD_V3_E_PERMISSION_DENIED; },
              &got, &error) &&
              got.empty() && error.find("permission") != std::string::npos,
          "a refused read reports the reason rather than an empty value");

    // A value that grew between the two calls is the ordinary case for live
    // client data, not a pathology.
    uint32_t attempts = 0u;
    const std::string grown(2500u, 'g');
    Check(wotbmod::lua::FetchSizedString(
              [&attempts, &grown](char* buffer, uint32_t* inout_size)
                  -> WotbModV3Result {
                  ++attempts;
                  const uint32_t claimed =
                      attempts == 1u ? 1024u
                                     : static_cast<uint32_t>(grown.size()) + 1u;
                  if (!buffer || *inout_size < claimed) {
                      *inout_size = claimed;
                      return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
                  }
                  std::memcpy(buffer, grown.c_str(), claimed);
                  *inout_size = claimed;
                  return WOTBMOD_V3_OK;
              },
              &got, &error) &&
              got == grown && error.empty() && attempts == 3u,
          "a value that grows between the calls is still read whole");

    // The headroom the buffer is grown by: a value that gains one byte after
    // the size probe fits without spending another attempt.
    attempts = 0u;
    const std::string crept(3000u, 'c');
    Check(wotbmod::lua::FetchSizedString(
              [&attempts, &crept](char* buffer, uint32_t* inout_size)
                  -> WotbModV3Result {
                  ++attempts;
                  // Understates by one byte on the probe, the way a value
                  // that gains a character between the calls would.
                  const uint32_t claimed =
                      attempts == 1u ? static_cast<uint32_t>(crept.size())
                                     : static_cast<uint32_t>(crept.size()) + 1u;
                  if (!buffer || *inout_size < claimed) {
                      *inout_size = claimed;
                      return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
                  }
                  std::memcpy(buffer, crept.c_str(), claimed);
                  *inout_size = claimed;
                  return WOTBMOD_V3_OK;
              },
              &got, &error) &&
              got == crept && attempts == 2u,
          "the buffer is grown with headroom, so a value that creeps up by a "
          "byte does not spend another attempt");

    Check(!wotbmod::lua::FetchSizedString(
              [](char*, uint32_t* inout_size) {
                  *inout_size += 1024u;
                  return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
              },
              &got, &error) &&
              got.empty() && !error.empty(),
          "a value that never stops growing gives up instead of looping");

    Check(!wotbmod::lua::FetchSizedString(
              [](char*, uint32_t* inout_size) {
                  *inout_size = 0x7FFFFFFFu;
                  return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
              },
              &got, &error) &&
              got.empty() && !error.empty(),
          "and an absurd size is refused rather than allocated");

    Check(RunWithProbe(state, &ProbeSizedString, "return probe()") &&
              IsString(state, -1) &&
              std::strcmp(lua_tostring(state, -1), "{\"probe\":1}") == 0,
          "a sized string arrives in Lua as a plain string");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeLargeSizedString,
                       "local s = probe(); return #s") &&
              lua_tointeger(state, -1) == 4096,
          "and one past the inline payload arrives whole, through the owned "
          "buffer");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeSizedStringDenied,
                       "local v, e = probe(); return v == nil and e") &&
              Mentions(state, -1, "permission"),
          "a refused one fails the same way every other call does");
    lua_settop(state, 0);

    // Rule 5. Reaching either of these Checks at all is the assertion: an
    // escaping exception would have taken the process down before here, not
    // failed a comparison.
    Check(!wotbmod::lua::FetchSizedString(&FetchThatThrows, &got, &error) &&
              got.empty() && !error.empty(),
          "an exception from the client is turned into a failure, not let out "
          "into Lua's C frames");

    Check(RunWithProbe(state, &ProbeSizedStringThrows,
                       "local v, e = probe(); return v == nil and e") &&
              IsString(state, -1),
          "and a binding that throws answers nil plus a message rather than "
          "terminating the process");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeSizedStringDeniedWithContext,
                       "local v, e = probe(); return e") &&
              Mentions(state, -1, "storage.get_json: ") &&
              Mentions(state, -1, "permission"),
          "PushSizedString's own context prefixes the message exactly like "
          "PushResult's does");
    lua_settop(state, 0);

    // ---- byte buffers -------------------------------------------------

    g_byte_fetch_calls = 0u;
    Check(RunWithProbe(state, &ProbeByteBuffer, "local v = probe(); return #v") &&
              lua_tointeger(state, -1) == 5,
          "a byte buffer with an embedded zero arrives whole - 5 bytes, not "
          "2 - proving the string helper's BoundedLength truncation does not "
          "run here");
    lua_settop(state, 0);

    Check(RunWithProbe(
              state, &ProbeByteBuffer,
              "local v = probe(); "
              "local b1,b2,b3,b4,b5 = string.byte(v, 1, 5); "
              "if b1~=65 or b2~=66 or b3~=0 or b4~=67 or b5~=68 then "
              "error('bytes wrong') end; return true"),
          "and every byte, including the zero at position 3, is exactly "
          "what the client wrote");
    lua_settop(state, 0);

    g_byte_fetch_calls = 0u;
    Check(RunWithProbe(state, &ProbeLargeByteBuffer,
                       "local s = probe(); return #s") &&
              lua_tointeger(state, -1) == 4096,
          "one past the inline payload arrives whole, through the owned "
          "buffer - the malloc/heap tier, exercised through PushByteBuffer "
          "specifically");
    lua_settop(state, 0);
    Check(g_byte_fetch_calls == 2u,
          "and it cost exactly two calls: one to learn the size, one to "
          "read, the same headroom-free shape FetchSizedString proves above");

    Check(RunWithProbe(state, &ProbeByteBufferDenied,
                       "local v, e = probe(); return v == nil and e") &&
              Mentions(state, -1, "storage.get_bytes: ") &&
              Mentions(state, -1, "permission"),
          "a refused byte buffer fails the same way every other call does, "
          "context and all");
    lua_settop(state, 0);

    // Rule 5, for the byte-buffer path specifically.
    Check(!wotbmod::lua::FetchByteBuffer(&FetchBytesThatThrows, &got, &error) &&
              got.empty() && !error.empty(),
          "an exception from a byte-buffer client is turned into a failure, "
          "not let out into Lua's C frames");
    Check(RunWithProbe(state, &ProbeByteBufferThrows,
                       "local v, e = probe(); return v == nil and e") &&
              IsString(state, -1),
          "and a byte-buffer binding that throws answers nil plus a "
          "message rather than terminating the process");
    lua_settop(state, 0);

    // ---- tokens ---------------------------------------------------------

    Check(RunWithProbe(state, &ProbeTokenZero, "return probe() ~= nil") &&
              lua_toboolean(state, -1) == 1,
          "PushToken boxes the value 0, unlike PushHandle - there is no "
          "WOTBMOD_V3_INVALID_HANDLE-style sentinel for a token, so a "
          "successful call that hands back token 0 must not read as nil");
    lua_settop(state, 0);

    lua_pushcfunction(state, &ProbeTokenZero);
    lua_setglobal(state, "make_zero");
    Check(RunWithProbe(state, &ProbeTakesTestToken,
                       "local t = make_zero(); return probe(t)") &&
              lua_tointeger(state, -1) == 0,
          "and it reads back as exactly 0 through CheckHandle, not merely "
          "as \"present\"");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeTokenNonzero, "return type(probe())") &&
              std::strcmp(lua_tostring(state, -1), "userdata") == 0,
          "a nonzero token still crosses as userdata, not a number");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeTakesTestToken,
                       "local v, e = probe(4242); return e") &&
              IsString(state, -1),
          "and an integer still cannot forge a token, the same as any "
          "other handle");
    lua_settop(state, 0);

    // ---- arguments (rule 6) ----------------------------------------------

    Check(RunWithProbe(state, &ProbeCheckArgString, "return probe('hello')") &&
              std::strcmp(lua_tostring(state, -1), "hello") == 0,
          "CheckArgString accepts a string and hands it back unchanged");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgString,
                       "local v, e = probe(42); return v == nil and e") &&
              Mentions(state, -1, "argument 1") &&
              Mentions(state, -1, "a string"),
          "and refuses a number without coercing it, pushing its own "
          "argument-N message rather than the call site inventing one");
    lua_settop(state, 0);

    Check(RunWithProbe(
              state, &ProbeCheckArgBytes,
              "local v = probe(string.char(65,66,0,67,68)); return #v"),
          "CheckArgBytes reads the exact length");
    Check(lua_tointeger(state, -1) == 5,
          "including a byte string's embedded zero, which CheckArgString's "
          "NUL-terminated read would have lost");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgBytes,
                       "local v, e = probe(true); return v == nil and e") &&
              Mentions(state, -1, "argument 1"),
          "CheckArgBytes refuses a non-string the same way CheckArgString "
          "does");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgInteger, "return probe(7)") &&
              lua_tointeger(state, -1) == 7,
          "CheckArgInteger accepts a genuine integer");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgInteger,
                       "local v, e = probe(2.5); return v == nil and e") &&
              Mentions(state, -1, "argument 1") &&
              Mentions(state, -1, "an integer"),
          "and refuses a non-integral float - which lua_tointeger does not "
          "truncate but *fails*, F2Ieq in lvm.c, handing back a 0 that reads "
          "as path kind 0");
    lua_settop(state, 0);

    // The stricter-than-Lua case, and the one the header now states outright.
    // lua_tointeger(2.0) succeeds - 2.0 is integral, so F2Ieq is satisfied -
    // so this refusal comes from lua_isinteger's subtype test alone, and
    // nothing else in the suite pins it. A regression that dropped
    // lua_isinteger would still refuse 2.5 and pass every check above.
    Check(RunWithProbe(state, &ProbeCheckArgInteger,
                       "local v, e = probe(2.0); return v == nil and e") &&
              Mentions(state, -1, "argument 1") &&
              Mentions(state, -1, "an integer"),
          "and refuses an integral-valued float, which lua_tointeger would "
          "have accepted - the subtype is the test, not the value");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgInteger,
                       "local v, e = probe('7'); return v == nil and e") &&
              IsString(state, -1),
          "and refuses a numeric string too - no coercion, matching "
          "CheckArgString's own strictness");
    lua_settop(state, 0);

    // ---- the rest of the argument family ---------------------------------
    //
    // CheckArgString/Bytes/Integer were the whole of rule 6 when storage was
    // bound, which is why get_path had to hand-roll a value check next to a
    // CheckArg* call. These close that gap.

    Check(RunWithProbe(state, &ProbeCheckArgNumber, "return probe(2.5)") &&
              lua_tonumber(state, -1) == 2.5,
          "CheckArgNumber accepts a non-integral number, which "
          "CheckArgInteger deliberately refuses");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgNumber,
                       "local v, e = probe('2.5'); return v == nil and e") &&
              Mentions(state, -1, "a number"),
          "and still refuses a numeric string - the strictness is about "
          "coercion, not about integrality");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgBoolean, "return probe(false)") &&
              std::strcmp(lua_tostring(state, -1), "false") == 0,
          "CheckArgBoolean reads false as a value rather than as an absence");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgBoolean,
                       "local v, e = probe({}); return v == nil and e") &&
              Mentions(state, -1, "a boolean"),
          "and refuses a table, which lua_toboolean's truthiness test would "
          "have silently read as true");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgFunction,
                       "return probe(function() end)") &&
              std::strcmp(lua_tostring(state, -1), "callable") == 0,
          "CheckArgFunction accepts a Lua function");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgFunction,
                       "local v, e = probe(setmetatable({}, "
                       "{__call = function() end})); return v == nil and e") &&
              Mentions(state, -1, "a function"),
          "and refuses a callable table - a handler is stored now and "
          "invoked from a client thread much later, so 'probably callable' "
          "is not good enough at the point of subscription");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgEnum, "return probe(-1000)") &&
              lua_tointeger(state, -1) == -1000,
          "CheckArgEnum accepts a declared value, including a negative one");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgEnum,
                       "local v, e = probe(1); return v == nil and e") &&
              Mentions(state, -1, "LOWEST, NORMAL or HIGH"),
          "and refuses an integer that is in range but not one of the values, "
          "naming them - the exact check get_path had to write by hand");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgEnum,
                       "local v, e = probe(2.5); return v == nil and e") &&
              Mentions(state, -1, "an integer"),
          "a float is refused by the type check before the value check runs, "
          "and only one refusal is pushed, not two stacked pairs");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgEnum,
                       "local v, e = probe(2.5); return v == nil and e") &&
              !Mentions(state, -1, "LOWEST"),
          "- and the message is the type refusal, not the value refusal "
          "pushed on top of it, which is what a second pair would leave at "
          "the top of the stack");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgFlags, "return probe(9)") &&
              lua_tointeger(state, -1) == 9,
          "CheckArgFlags accepts any combination of the bits in the mask");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgFlags, "return probe(0)") &&
              lua_tointeger(state, -1) == 0,
          "including none of them");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgFlags,
                       "local v, e = probe(16); return v == nil and e") &&
              IsString(state, -1),
          "and refuses a bit the ABI never defined");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeCheckArgFlags,
                       "local v, e = probe(-1); return v == nil and e") &&
              IsString(state, -1),
          "and refuses a negative mask, which would reach the client's "
          "uint32_t flags field as every high bit set");
    lua_settop(state, 0);

    // ---- the optional form -----------------------------------------------

    Check(RunWithProbe(state, &ProbeOptString, "return probe()") &&
              std::strcmp(lua_tostring(state, -1), "fallback") == 0,
          "an omitted optional argument yields the fallback and pushes no "
          "refusal");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptString, "return probe(nil)") &&
              std::strcmp(lua_tostring(state, -1), "fallback") == 0,
          "and an explicit nil is the same case, not a different one - "
          "f(a, nil, c) is how a Lua caller omits a middle argument");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptString, "return probe('given')") &&
              std::strcmp(lua_tostring(state, -1), "given") == 0,
          "a value that is given is read, not replaced by the fallback");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptString,
                       "local v, e = probe(42); return v == nil and e") &&
              Mentions(state, -1, "argument 1"),
          "and a present argument of the wrong type is still a refusal - "
          "optional means omissible, not unchecked");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptBytes, "local v = probe(); return #v") &&
              lua_tointeger(state, -1) == 5,
          "the optional byte form falls back to an exact length, embedded "
          "zero included");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptInteger, "return probe()") &&
              lua_tointeger(state, -1) == 77,
          "CheckOptArgInteger falls back");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptInteger,
                       "local v, e = probe(2.5); return v == nil and e") &&
              IsString(state, -1),
          "and still refuses a float when one is given");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptNumber, "return probe()") &&
              lua_tonumber(state, -1) == 0.5,
          "CheckOptArgNumber falls back");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptBoolean, "return probe()") &&
              std::strcmp(lua_tostring(state, -1), "true") == 0,
          "CheckOptArgBoolean falls back to its default");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptBoolean, "return probe(false)") &&
              std::strcmp(lua_tostring(state, -1), "false") == 0,
          "and an explicit false is a value, not an omission - the one case "
          "an is-it-truthy fallback test would get wrong");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptEnum, "return probe()") &&
              lua_tointeger(state, -1) == 0,
          "CheckOptArgEnum falls back");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptEnum,
                       "local v, e = probe(1); return v == nil and e") &&
              IsString(state, -1),
          "and still checks the value of one that is given");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptFlags, "return probe()") &&
              lua_tointeger(state, -1) == 4,
          "CheckOptArgFlags falls back");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptFlags,
                       "local v, e = probe(16); return v == nil and e") &&
              IsString(state, -1),
          "and still refuses an undefined bit in one that is given");
    lua_settop(state, 0);

    // ---- the required handle form, rule 6's own missing member -------------
    //
    // `make` is still the global ProbeHandle registered above, boxing 4242u
    // under kHandleUiControl - reused here rather than redeclared, the same
    // way the required-handle tests above reuse it.

    Check(RunWithProbe(state, &ProbeArgHandle,
                       "local h = make(); return probe(h)") &&
              lua_tointeger(state, -1) == 4242,
          "CheckArgHandle reads a handle of the right type");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeArgHandle,
                       "local v, e = probe(4242); return v == nil and e") &&
              Mentions(state, -1, "argument 1") &&
              Mentions(state, -1, "wotb.control"),
          "and refuses a bare number with rule 1's shape and the phrase the "
          "call site named - the refusal five slots used to write out by hand");
    lua_settop(state, 0);

    // The difference between the two members, stated as a test rather than as
    // a comment: absence is an omission for the optional form and a refusal
    // for this one.
    Check(RunWithProbe(state, &ProbeArgHandle,
                       "local v, e = probe(); return v == nil and e") &&
              IsString(state, -1),
          "and an omitted argument is a refusal here, not a fallback");
    lua_settop(state, 0);

    // ---- the optional handle form ------------------------------------------

    Check(RunWithProbe(state, &ProbeOptHandle, "return probe()") &&
              lua_tointeger(state, -1) == 9999,
          "CheckOptArgHandle falls back when the argument is omitted");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptHandle, "return probe(nil)") &&
              lua_tointeger(state, -1) == 9999,
          "and an explicit nil is the same case, not a different one - "
          "f(a, nil, c) is how a Lua caller omits a middle argument");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptHandle,
                       "local h = make(); return probe(h)") &&
              lua_tointeger(state, -1) == 4242,
          "a real handle of the right type is read, not replaced by the "
          "fallback");
    lua_settop(state, 0);

    Check(RunWithProbe(state, &ProbeOptHandle,
                       "local v, e = probe(4242); return v == nil and e") &&
              IsString(state, -1),
          "a present argument of the wrong type is still a refusal - "
          "optional means omissible, not unchecked");
    lua_settop(state, 0);

    lua_pushcfunction(state, &ProbeSubscriptionHandle);
    lua_setglobal(state, "subscription");
    Check(RunWithProbe(state, &ProbeOptHandle,
                       "local s = subscription(); local v, e = probe(s); "
                       "return e") &&
              Mentions(state, -1, "wotb.control"),
          "and a handle of the wrong *type* is refused too, with the phrase "
          "this call site named rather than a generic one");
    lua_settop(state, 0);

    lua_close(state);
    std::printf("Lua convert: %u passed, %u failed\n",
                g_checks - g_failures, g_failures);
    return g_failures == 0u ? 0 : 1;
}
