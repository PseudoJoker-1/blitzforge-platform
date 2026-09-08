#include "lua_convert.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace wotbmod {
namespace lua {
namespace {

// One metatable for every handle type, with the type name carried in the box
// rather than in a metatable per type. 33 handle types across the ABI would
// otherwise be 33 registry entries to create and keep in step, and the check a
// binding needs - "is this the type I asked for" - is a string compare either
// way.
const char kHandleMetatable[] = "wotbmod.lua.handle";

struct HandleBox {
    WotbModV3Handle handle;
    const char* type_name;   // static string literal, not owned
};

const HandleBox* AsHandleBox(lua_State* state, int index) {
    return static_cast<const HandleBox*>(
        luaL_testudata(state, index, kHandleMetatable));
}

int HandleToString(lua_State* state) {
    const HandleBox* box = AsHandleBox(state, 1);
    if (!box) {
        lua_pushliteral(state, "<not a wotbmod handle>");
        return 1;
    }
    // %llu rather than lua_pushfstring's %I: a handle is a uint64_t and
    // lua_Integer is signed, so the top of the range would print negative.
    char text[96] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%s(%llu)", box->type_name,
                static_cast<unsigned long long>(box->handle));
    lua_pushstring(state, text);
    return 1;
}

// Without this, every push of a handle mints a fresh userdata and two boxes
// around one object never compare equal - so a script could not tell that the
// control an event just handed it is the one it created. Lua only reaches this
// when both operands are userdata, and both then share this metatable.
int HandleEquals(lua_State* state) {
    const HandleBox* left = AsHandleBox(state, 1);
    const HandleBox* right = AsHandleBox(state, 2);
    const bool equal = left && right && left->handle == right->handle &&
                       std::strcmp(left->type_name, right->type_name) == 0;
    lua_pushboolean(state, equal ? 1 : 0);
    return 1;
}

struct HandleMethod {
    const char* method;
    const char* function;
};

const HandleMethod kUiHandleMethods[] = {
    {"clone", "control_clone"},
    {"destroy", "control_destroy"},
    {"add_child", "control_add_child"},
    {"remove_child", "control_remove_child"},
    {"set_parent", "control_set_parent"},
    {"get_parent", "control_get_parent"},
    {"get_child_count", "control_get_child_count"},
    {"get_child_at", "control_get_child_at"},
    {"set_id", "control_set_id"},
    {"get_id", "control_get_id"},
    {"is_alive", "control_is_alive"},
    {"set_text", "control_set_text"},
    {"set_texture", "control_set_texture"},
    {"set_color", "control_set_color"},
    {"set_opacity", "control_set_opacity"},
    {"set_visible", "control_set_visible"},
    {"set_font", "control_set_font"},
    {"set_font_size", "control_set_font_size"},
    {"set_text_alignment", "control_set_text_alignment"},
    {"set_text_wrap", "control_set_text_wrap"},
    {"set_rich_text", "control_set_rich_text"},
    {"set_localization_key", "control_set_localization_key"},
    {"set_tooltip", "control_set_tooltip"},
    {"set_accessibility_label", "control_set_accessibility_label"},
    {"set_enabled", "control_set_enabled"},
    {"set_interactable", "control_set_interactable"},
    {"set_focus", "control_set_focus"},
    {"set_position", "control_set_position"},
    {"get_position", "control_get_position"},
    {"set_size", "control_set_size"},
    {"get_size", "control_get_size"},
    {"set_anchor", "control_set_anchor"},
    {"set_pivot", "control_set_pivot"},
    {"set_margin", "control_set_margin"},
    {"set_padding", "control_set_padding"},
    {"set_min_size", "control_set_min_size"},
    {"set_max_size", "control_set_max_size"},
    {"set_z_order", "control_set_z_order"},
    {"set_layout", "layout_set"},
    {"set_layout_type", "layout_set_type"},
    {"set_layout_direction", "layout_set_direction"},
    {"set_layout_spacing", "layout_set_spacing"},
    {"set_layout_alignment", "layout_set_alignment"},
    {"set_layout_weight", "layout_set_weight"},
    {"invalidate_layout", "layout_invalidate"},
    {"on", "event_subscribe"},
    {"push_style", "style_push"},
    {"update", "style_update"},
    {"pop", "style_pop"},
    {nullptr, nullptr},
};

int HandleIndex(lua_State* state) {
    const HandleBox* box = AsHandleBox(state, 1);
    const char* method = lua_tostring(state, 2);
    if (!box || !method ||
        (std::strcmp(box->type_name, kHandleUiControl) != 0 &&
         std::strcmp(box->type_name, "wotb.generated_handle") != 0)) {
        lua_pushnil(state);
        return 1;
    }
    const char* function = nullptr;
    for (const HandleMethod* candidate = kUiHandleMethods;
         candidate->method;
         ++candidate) {
        if (std::strcmp(candidate->method, method) == 0) {
            function = candidate->function;
            break;
        }
    }
    if (!function) {
        lua_pushnil(state);
        return 1;
    }

    lua_pushglobaltable(state);
    lua_pushliteral(state, "wotb");
    lua_rawget(state, -2);
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushliteral(state, "ui");
    lua_rawget(state, -2);
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pushnil(state);
        return 1;
    }
    lua_pushstring(state, function);
    lua_rawget(state, -2);
    return 1;
}

void EnsureHandleMetatable(lua_State* state) {
    if (luaL_newmetatable(state, kHandleMetatable)) {
        lua_pushcfunction(state, &HandleToString);
        lua_setfield(state, -2, "__tostring");
        lua_pushcfunction(state, &HandleEquals);
        lua_setfield(state, -2, "__eq");
        lua_pushcfunction(state, &HandleIndex);
        lua_setfield(state, -2, "__index");
        // getmetatable(handle) answers this string instead of the table, so a
        // script cannot read the metatable this host owns, nor add a __gc or
        // an __index to it that would then run for every handle in the state.
        // The C API's lua_getmetatable ignores __metatable, so CheckHandle is
        // unaffected.
        lua_pushliteral(state, "wotbmod handle");
        lua_setfield(state, -2, "__metatable");
        // UI handles expose method aliases only. Their data remains opaque,
        // and the aliases resolve back to the same guarded wotb.ui closures,
        // so colon syntax does not bypass permissions or ownership checks.
    }
    lua_pop(state, 1);
}

// Describes whatever is at index for an argument message: the handle's own
// type name if it is one of ours, otherwise Lua's name for the type.
const char* DescribeValue(lua_State* state, int index) {
    const char* handle_type = HandleTypeName(state, index);
    if (handle_type) return handle_type;
    return luaL_typename(state, index);
}

// The buffer tried before any allocation. Most sized strings in the ABI are
// short - a path, an id, a small JSON object - so this turns the usual case
// into one crossing instead of two. Values past it fall through to the dance.
const uint32_t kInlineCapacity = 512u;

// A value larger than this is refused rather than allocated. A host that
// answers the size probe with garbage should not be able to ask this process
// for an arbitrary allocation.
const uint32_t kMaxFetchedBytes = 16u * 1024u * 1024u;

// The value may legitimately grow between the size probe and the read, so a
// second and third attempt are normal. A fourth means something is wrong.
const int kMaxFetchAttempts = 4;

// Room for the longest sentence this file produces, on the stack, so the
// failure path needs no allocation at the point where allocation is what
// failed.
const size_t kMessageCapacity = 256u;

const char kOutOfMemory[] = "ran out of memory reading the value";

// The ABI's convention is that the size includes the terminator, but not every
// slot is written that way. Reading it as a C string is right under both.
size_t BoundedLength(const char* data, size_t limit) {
    size_t length = 0u;
    while (length < limit && data[length] != '\0') ++length;
    return length;
}

// Copies into a fixed buffer with no allocation and no CRT dependency, so it
// is usable from a catch handler under memory exhaustion.
void CopyMessage(char* destination, size_t capacity,
                 const char* text) noexcept {
    if (!destination || capacity == 0u) return;
    size_t i = 0u;
    if (text) {
        while (i + 1u < capacity && text[i] != '\0') {
            destination[i] = text[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

// Best effort. The boolean FetchSizedString returns is the real signal: under
// memory exhaustion even a short assign can throw, and a caller must not read
// an empty message as success.
void SetError(std::string* out_error, const char* text) noexcept {
    if (!out_error) return;
    try {
        out_error->assign(text ? text : "");
    } catch (...) {
        out_error->clear();   // noexcept
    }
}

// The box-creation half of PushHandle, shared with PushToken: both mint the
// identical userdata, and differ only in whether the value 0 is refused
// before reaching here. See PushToken's header comment for why that check
// cannot be shared too.
void PushHandleBoxUnconditional(lua_State* state, WotbModV3Handle value,
                                const char* type_name) {
    EnsureHandleMetatable(state);
    HandleBox* box =
        static_cast<HandleBox*>(lua_newuserdatauv(state, sizeof(HandleBox), 0));
    box->handle = value;
    box->type_name = type_name;   // static string, not owned
    luaL_setmetatable(state, kHandleMetatable);
}

}  // namespace

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------

const char* ResultMessage(WotbModV3Result result) {
    switch (result) {
        case WOTBMOD_V3_OK:
            return "ok";
        case WOTBMOD_V3_E_INVALID_ARGUMENT:
            return "invalid argument";
        case WOTBMOD_V3_E_INVALID_HANDLE:
            return "invalid handle: it was never created here, or it has "
                   "already been released";
        case WOTBMOD_V3_E_NOT_SUPPORTED:
            return "not supported by this client build";
        case WOTBMOD_V3_E_NOT_FOUND:
            return "not found";
        case WOTBMOD_V3_E_ALREADY_EXISTS:
            return "already exists";
        case WOTBMOD_V3_E_WRONG_THREAD:
            return "wrong thread: this call must run on the thread the client "
                   "owns for it";
        case WOTBMOD_V3_E_PERMISSION_DENIED:
            return "permission denied: this script's manifest does not grant "
                   "it";
        case WOTBMOD_V3_E_CLIENT_MISMATCH:
            return "client mismatch: this game build is not the one this call "
                   "was written against";
        case WOTBMOD_V3_E_OBJECT_DESTROYED:
            return "the object was already destroyed";
        case WOTBMOD_V3_E_CONFLICT:
            return "conflict: something else already holds this";
        case WOTBMOD_V3_E_CANCELLED:
            return "cancelled";
        case WOTBMOD_V3_E_BUFFER_TOO_SMALL:
            return "value did not fit the buffer";
        case WOTBMOD_V3_E_LIMIT_REACHED:
            return "limit reached: the client will not allocate another one";
        case WOTBMOD_V3_E_BUSY:
            return "busy: the client is not ready for this call yet";
        case WOTBMOD_V3_E_IO:
            return "input or output failed";
        case WOTBMOD_V3_E_PARSE:
            return "could not be parsed";
        case WOTBMOD_V3_E_HASH_MISMATCH:
            return "hash mismatch: the content is not what its manifest says";
        case WOTBMOD_V3_E_SIGNATURE_INVALID:
            return "the signature is not valid";
        case WOTBMOD_V3_E_DEPENDENCY_MISSING:
            return "a mod or interface this depends on is not loaded";
        case WOTBMOD_V3_E_INCOMPATIBLE:
            return "incompatible with this client or with another loaded mod";
        case WOTBMOD_V3_E_CALLBACK_FAULT:
            return "a callback faulted and was disconnected";
        case WOTBMOD_V3_E_PLATFORM:
            return "the platform refused the request";
        case WOTBMOD_V3_E_TIMEOUT:
            return "timed out";
        default:
            break;
    }
    // Only reachable for a value outside the enum the ABI declares - a client
    // newer than this header, say. Still says more than a bare number would,
    // and still tells a bug report which number to look up.
    static thread_local char unmapped[48] = {};
    _snprintf_s(unmapped, sizeof(unmapped), _TRUNCATE, "unexpected error %d",
                static_cast<int>(result));
    return unmapped;
}

int PushResultWith(lua_State* state, WotbModV3Result result, StackMark mark,
                   const char* context) {
    const int top = lua_gettop(state);
    // A mark from another call frame is a programming error, not something a
    // script can provoke. Clamp rather than trust it: the failure mode of an
    // out-of-range lua_settop is a corrupted stack much later, far from here.
    int base = mark.top;
    if (base < 0) base = 0;
    if (base > top) base = top;

    if (result == WOTBMOD_V3_OK) {
        const int pushed = top - base;
        if (pushed > 0) return pushed;
        lua_pushboolean(state, 1);
        return 1;
    }
    // Restore the stack this binding was handed. See the header for why this
    // is about hygiene rather than about what the script receives.
    lua_settop(state, base);
    lua_pushnil(state);
    // Nothing with a destructor is in scope here, so a longjmp out of these
    // pushes skips nothing. Rule 5.
    if (context && *context) {
        lua_pushfstring(state, "%s: %s", context, ResultMessage(result));
    } else {
        lua_pushstring(state, ResultMessage(result));
    }
    return 2;
}

int PushResult(lua_State* state, WotbModV3Result result, const char* context) {
    return PushResultWith(state, result, MarkStack(state), context);
}

int PushArgumentError(lua_State* state, int index, const char* expected) {
    const char* actual = DescribeValue(state, index);
    lua_pushnil(state);
    lua_pushfstring(state, "argument %d: expected %s, got %s", index,
                    expected ? expected : "something else", actual);
    return 2;
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

bool CheckArgString(lua_State* state, int index, const char** out) {
    if (lua_type(state, index) == LUA_TSTRING) {
        if (out) *out = lua_tostring(state, index);
        return true;
    }
    PushArgumentError(state, index, "a string");
    return false;
}

bool CheckArgBytes(lua_State* state, int index, const char** out_data,
                   size_t* out_size) {
    if (lua_type(state, index) == LUA_TSTRING) {
        size_t length = 0u;
        const char* data = lua_tolstring(state, index, &length);
        if (out_data) *out_data = data;
        if (out_size) *out_size = length;
        return true;
    }
    PushArgumentError(state, index, "a byte string");
    return false;
}

bool CheckArgInteger(lua_State* state, int index, lua_Integer* out) {
    if (lua_type(state, index) == LUA_TNUMBER && lua_isinteger(state, index)) {
        if (out) *out = lua_tointeger(state, index);
        return true;
    }
    PushArgumentError(state, index, "an integer");
    return false;
}

bool CheckArgNumber(lua_State* state, int index, lua_Number* out) {
    if (lua_type(state, index) == LUA_TNUMBER) {
        if (out) *out = lua_tonumber(state, index);
        return true;
    }
    PushArgumentError(state, index, "a number");
    return false;
}

bool CheckArgBoolean(lua_State* state, int index, bool* out) {
    if (lua_type(state, index) == LUA_TBOOLEAN) {
        if (out) *out = lua_toboolean(state, index) != 0;
        return true;
    }
    PushArgumentError(state, index, "a boolean");
    return false;
}

bool CheckArgFunction(lua_State* state, int index) {
    if (lua_type(state, index) == LUA_TFUNCTION) return true;
    PushArgumentError(state, index, "a function");
    return false;
}

bool CheckArgEnum(lua_State* state, int index, const EnumValues& allowed,
                  lua_Integer* out) {
    lua_Integer value = 0;
    // Two steps, never `||`: CheckArgInteger has already pushed its own
    // refusal when the type is wrong, and a second pair pushed on top of it
    // for a value it never got to look at would leave four values where the
    // convention promises two.
    if (!CheckArgInteger(state, index, &value)) return false;
    for (size_t i = 0u; i < allowed.count; ++i) {
        if (allowed.values[i] == value) {
            if (out) *out = value;
            return true;
        }
    }
    PushArgumentError(state, index,
                      allowed.expected ? allowed.expected
                                       : "one of the values this slot defines");
    return false;
}

bool CheckArgFlags(lua_State* state, int index, lua_Integer allowed_mask,
                   const char* expected, lua_Integer* out) {
    lua_Integer value = 0;
    if (!CheckArgInteger(state, index, &value)) return false;
    // A negative mask is refused before the bit test rather than through it:
    // ~allowed_mask would accept -1 as "every bit, including the ones you
    // allowed", and a negative value cast to the ABI's uint32_t flags field
    // is every high bit set.
    if (value < 0 || (value & ~allowed_mask) != 0) {
        PushArgumentError(state, index,
                          expected ? expected : "a mask of this slot's flags");
        return false;
    }
    if (out) *out = value;
    return true;
}

// Absent means "the caller did not give me one", and a Lua caller writing
// f(a, nil, c) means exactly that - so LUA_TNONE and LUA_TNIL are one case
// here, not two.
static bool ArgIsAbsent(lua_State* state, int index) {
    const int type = lua_type(state, index);
    return type == LUA_TNONE || type == LUA_TNIL;
}

bool CheckOptArgString(lua_State* state, int index, const char* fallback,
                       const char** out) {
    if (ArgIsAbsent(state, index)) {
        if (out) *out = fallback;
        return true;
    }
    return CheckArgString(state, index, out);
}

bool CheckOptArgBytes(lua_State* state, int index, const char* fallback,
                      size_t fallback_size, const char** out_data,
                      size_t* out_size) {
    if (ArgIsAbsent(state, index)) {
        if (out_data) *out_data = fallback;
        if (out_size) *out_size = fallback_size;
        return true;
    }
    return CheckArgBytes(state, index, out_data, out_size);
}

bool CheckOptArgInteger(lua_State* state, int index, lua_Integer fallback,
                        lua_Integer* out) {
    if (ArgIsAbsent(state, index)) {
        if (out) *out = fallback;
        return true;
    }
    return CheckArgInteger(state, index, out);
}

bool CheckOptArgNumber(lua_State* state, int index, lua_Number fallback,
                       lua_Number* out) {
    if (ArgIsAbsent(state, index)) {
        if (out) *out = fallback;
        return true;
    }
    return CheckArgNumber(state, index, out);
}

bool CheckOptArgBoolean(lua_State* state, int index, bool fallback, bool* out) {
    if (ArgIsAbsent(state, index)) {
        if (out) *out = fallback;
        return true;
    }
    return CheckArgBoolean(state, index, out);
}

bool CheckOptArgEnum(lua_State* state, int index, const EnumValues& allowed,
                     lua_Integer fallback, lua_Integer* out) {
    if (ArgIsAbsent(state, index)) {
        if (out) *out = fallback;
        return true;
    }
    return CheckArgEnum(state, index, allowed, out);
}

bool CheckOptArgFlags(lua_State* state, int index, lua_Integer allowed_mask,
                      const char* expected, lua_Integer fallback,
                      lua_Integer* out) {
    if (ArgIsAbsent(state, index)) {
        if (out) *out = fallback;
        return true;
    }
    return CheckArgFlags(state, index, allowed_mask, expected, out);
}

// Declared ahead of CheckHandle below only in this .cpp's own reading order;
// the two share nothing but the fact that this one calls the other. See the
// header for why these exist at all rather than being composed at each call
// site.
bool CheckArgHandle(lua_State* state, int index, const char* type_name,
                    const char* expected, WotbModV3Handle* out) {
    if (CheckHandle(state, index, type_name, out)) return true;
    PushArgumentError(state, index, expected);
    return false;
}

bool CheckOptArgHandle(lua_State* state, int index, const char* type_name,
                       const char* expected, WotbModV3Handle fallback,
                       WotbModV3Handle* out) {
    if (ArgIsAbsent(state, index)) {
        if (out) *out = fallback;
        return true;
    }
    // Exactly CheckArgHandle's own body, called rather than repeated: the
    // required and optional forms of every other accessor in this file already
    // stand in that relationship, and a handle is not the one that gets two.
    return CheckArgHandle(state, index, type_name, expected, out);
}

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

void PushHandle(lua_State* state, WotbModV3Handle handle,
                const char* type_name) {
    if (handle == WOTBMOD_V3_INVALID_HANDLE || !type_name) {
        lua_pushnil(state);
        return;
    }
    PushHandleBoxUnconditional(state, handle, type_name);
}

void PushOptionalHandle(lua_State* state, WotbModV3Handle handle,
                        const char* type_name) {
    // The leading true is what keeps rule 2 true for a slot where absence is
    // an answer rather than a failure: the script reads `local ok, camera =
    // ...` and `if not ok` still means only "the call failed".
    lua_pushboolean(state, 1);
    PushHandle(state, handle, type_name);
}

void PushToken(lua_State* state, WotbModV3Token token, const char* type_name) {
    if (!type_name) {
        lua_pushnil(state);
        return;
    }
    // No value-0 check, deliberately - see the header. Every value of a
    // token is a real one; only a missing type name (a caller's own
    // mistake, not something a script can provoke) is refused.
    PushHandleBoxUnconditional(state, static_cast<WotbModV3Handle>(token),
                               type_name);
}

bool CheckHandle(lua_State* state, int index, const char* type_name,
                 WotbModV3Handle* out) {
    if (!type_name) return false;
    const HandleBox* box = AsHandleBox(state, index);
    if (!box) return false;
    if (std::strcmp(box->type_name, type_name) != 0) return false;
    if (out) *out = box->handle;
    return true;
}

const char* HandleTypeName(lua_State* state, int index) {
    const HandleBox* box = AsHandleBox(state, index);
    return box ? box->type_name : nullptr;
}

// ---------------------------------------------------------------------------
// Sized strings and byte buffers
// ---------------------------------------------------------------------------

namespace {

// The two-call dance, shared by FetchSizedString and FetchByteBuffer:
// out_value gets exactly the bytes the client reported, with no
// interpretation of what they mean. That interpretation - stop at the first
// '\0' for text, or take the length as-is for binary - happens in whichever
// of the two callers this returns to; the loop itself (the constants, the
// retry bound, the growth headroom, the noexcept/try-catch boundary) exists
// exactly once, per rule 4.
bool FetchRaw(const std::function<WotbModV3Result(char*, uint32_t*)>& fetch,
             std::string* out_value, std::string* out_error) noexcept {
    if (out_error) out_error->clear();
    if (out_value) out_value->clear();
    if (!out_value) {
        SetError(out_error, "internal error: nowhere to read into");
        return false;
    }
    if (!fetch) {
        SetError(out_error, "internal error: nothing to read from");
        return false;
    }

    // Everything below can allocate, and this runs inside a lua_CFunction
    // where an escaping exception would pass through Lua's C frames and
    // terminate the process rather than be caught by lua_pcall. Rule 5.
    try {
        // Zeroed: a client that answers OK without writing anything would
        // otherwise hand the script whatever was on this stack.
        char inline_buffer[kInlineCapacity] = {};
        std::string heap;             // only used once the value outgrows inline
        char* data = inline_buffer;
        uint32_t capacity = kInlineCapacity;

        for (int attempt = 0; attempt < kMaxFetchAttempts; ++attempt) {
            uint32_t size = capacity;
            const WotbModV3Result result = fetch(data, &size);
            if (result == WOTBMOD_V3_OK) {
                if (size > capacity) size = capacity;   // a host overstating
                out_value->assign(data, size);
                return true;
            }
            if (result != WOTBMOD_V3_E_BUFFER_TOO_SMALL) {
                SetError(out_error, ResultMessage(result));
                return false;
            }
            if (size <= capacity) {
                // It refused a buffer of the very size it then asked for.
                // Retrying would loop on identical arguments.
                SetError(out_error,
                         "the client refused a buffer of the size it asked "
                         "for");
                return false;
            }
            if (size > kMaxFetchedBytes) {
                SetError(out_error,
                         "the value is larger than this host will read");
                return false;
            }
            // Ask for an eighth more than the client said it needed. A live
            // value that grows by one byte between the probe and the read
            // would otherwise burn another of the four attempts, and there
            // are only four.
            uint32_t roomy = size + size / 8u;
            if (roomy < size) roomy = size;                     // overflow
            if (roomy > kMaxFetchedBytes) roomy = kMaxFetchedBytes;
            heap.assign(roomy, '\0');
            data = &heap[0];
            capacity = roomy;
        }
        SetError(out_error, "the value kept growing between reads");
        return false;
    } catch (...) {
        // Losing the value is the outcome either way. What must not happen is
        // this continuing up through Lua's frames.
        SetError(out_error, kOutOfMemory);
        return false;
    }
}

// Phase two of PushSizedString/PushByteBuffer, shared: given bytes already
// copied into either the inline buffer or an owned malloc block - no
// std::string in scope, the caller's own try{} already closed before this
// runs - does the two Lua pushes and, on failure, the context prefix
// PushResultWith's own message uses. Nothing here has a non-trivial
// destructor, so a longjmp out of either push skips nothing. Rule 5.
int PushCopiedBytes(lua_State* state, bool failed, char* payload,
                    size_t payload_size, char* owned, const char* message,
                    const char* context) noexcept {
    if (failed) {
        std::free(owned);
        lua_pushnil(state);
        if (context && *context) {
            lua_pushfstring(state, "%s: %s", context, message);
        } else {
            lua_pushstring(state, message);
        }
        return 2;
    }
    // If this raises LUA_ERRMEM it leaves by longjmp and `owned` leaks. That
    // is a bounded leak in an out-of-memory teardown, and it is what buys the
    // absence of a live std::string here - see the header.
    lua_pushlstring(state, payload, payload_size);
    std::free(owned);
    return 1;
}

}  // namespace

bool FetchSizedString(const SizedStringFetch& fetch, std::string* out_value,
                      std::string* out_error) noexcept {
    if (!FetchRaw(fetch, out_value, out_error)) return false;
    // The ABI's convention is that the size includes the terminator, but not
    // every slot is written that way. Reading it as a C string - stopping at
    // the first '\0' - is right under both, and is exactly the step
    // FetchByteBuffer must not take: see its own header comment.
    if (out_value) {
        out_value->resize(BoundedLength(out_value->data(), out_value->size()));
    }
    return true;
}

bool FetchByteBuffer(const ByteBufferFetch& fetch, std::string* out_value,
                     std::string* out_error) noexcept {
    return FetchRaw(fetch, out_value, out_error);
}

namespace {

// PushSizedString and PushByteBuffer, once. They were character-for-character
// identical apart from which Fetch* they called - rule 4 collapsed the retry
// loop into FetchRaw, and the duplication grew back one level up around it,
// in the buffer-tier choice, the emptying of the callable and the two
// failure messages. Both public names still exist because a call site must
// state which contract it is promising; only the mechanics are shared, and
// truncate_at_nul is the entire difference between the two contracts.
//
// fetch is taken by non-const reference because the emptying below has to
// reach the caller's own object, which outlives this call - see
// PushSizedString's header comment. SizedStringFetch and ByteBufferFetch are
// the same std::function type, so one parameter serves both.
int PushFetched(lua_State* state,
                std::function<WotbModV3Result(char*, uint32_t*)>& fetch,
                bool truncate_at_nul, const char* context) noexcept {
    // Phase one does everything that can allocate or throw, inside a scope
    // that closes before the first Lua call. Phase two touches Lua with
    // nothing in scope that has a destructor. Rule 5.
    char inline_payload[kInlineCapacity] = {};
    char message[kMessageCapacity] = {};
    char* payload = inline_payload;
    char* owned = nullptr;       // set only when the value outgrew the inline
    size_t payload_size = 0u;
    bool failed = false;

    try {
        std::string value;
        std::string error;
        const bool read = truncate_at_nul
                              ? FetchSizedString(fetch, &value, &error)
                              : FetchByteBuffer(fetch, &value, &error);
        if (!read) {
            failed = true;
            CopyMessage(message, sizeof(message), error.c_str());
        } else if (value.size() <= sizeof(inline_payload)) {
            payload_size = value.size();
            std::memcpy(inline_payload, value.data(), payload_size);
        } else {
            owned = static_cast<char*>(std::malloc(value.size()));
            if (owned) {
                payload = owned;
                payload_size = value.size();
                std::memcpy(owned, value.data(), payload_size);
            } else {
                failed = true;
                CopyMessage(message, sizeof(message), kOutOfMemory);
            }
        }
    } catch (...) {
        failed = true;
        CopyMessage(message, sizeof(message), kOutOfMemory);
    }
    // Releases whatever the closure held, so the only object left with a
    // destructor is one with nothing to destroy. Everything below can longjmp.
    // This reaches the caller's temporary, which is the point: the parameter
    // is a reference to it, and it outlives this call. What it cannot reach is
    // that temporary's *construction*, which already happened in the caller's
    // frame - see the header for why that bounds rather than removes the
    // hazard.
    fetch = nullptr;

    // A fetch can fail without managing to build a message. An empty one
    // would read as success at the script.
    if (failed && message[0] == '\0') {
        CopyMessage(message, sizeof(message), "the value could not be read");
    }
    return PushCopiedBytes(state, failed, payload, payload_size, owned,
                           message, context);
}

}  // namespace

int PushSizedString(lua_State* state, SizedStringFetch&& fetch,
                    const char* context) noexcept {
    return PushFetched(state, fetch, true, context);
}

int PushByteBuffer(lua_State* state, ByteBufferFetch&& fetch,
                   const char* context) noexcept {
    return PushFetched(state, fetch, false, context);
}

}  // namespace lua
}  // namespace wotbmod
