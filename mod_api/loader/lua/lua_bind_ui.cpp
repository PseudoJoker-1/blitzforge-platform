#include "lua_bindings.h"

#include "lua_convert.h"
#include "lua_permissions.h"
#include "lua_script.h"

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

#include <cstdint>
#include <cstring>

// wotb.ui - 18 hand-written WotbModV3UiApiV2 slots: the tree
// (create/destroy/add_child/remove_child/set_id/find_by_id/is_alive), text
// and visibility, geometry (position/size/anchor/pivot) and named slots
// (slot_find/slot_attach/slot_detach). Clone, parent queries, layout, style,
// dialogs, typed-control constructors and UI event subscriptions are added by
// the generated binding pass after this table.
//
// Nothing in this hand-written subset calls back into Lua: all 18 are
// synchronous requests. The generated event_subscribe slot owns the callback
// lifecycle separately and extends this same wotb.ui table.

namespace wotbmod {
namespace lua {
namespace {

// The one thing every one of the 18 closures needs: the interface pointer
// this script was bound against, and the mod handle to pass with every
// call. Same shape as StorageContext - one userdata upvalue, shared by
// luaL_setfuncs across every closure in kUiFuncs below.
struct UiContext {
    const WotbModV3UiApiV2* api;
    WotbModV3Handle mod;
    // Only control_create and control_destroy touch this: a control is the one
    // thing in this file that outlives the call that made it, so it is the one
    // thing the script's ownership ledger has to know about. A slot handle
    // from slot_find is deliberately not tracked - slot_find resolves an
    // extension point the client already owns, it does not mint one, and
    // destroying it is not this host's to do. An attachment made by
    // slot_attach is not tracked either: it is a property of the control, and
    // it goes when the control does.
    LuaScript* script;
};

// GuardedUpvalueIndex, not lua_upvalueindex: every slot in this file is
// installed behind the shared permission guard, which owns upvalue 1. See
// lua_permissions.h.
UiContext* Context(lua_State* state) noexcept {
    return static_cast<UiContext*>(
        lua_touserdata(state, GuardedUpvalueIndex(1)));
}

// The control argument every slot but slot_find/slot_attach/slot_detach
// takes. Hand-written creates use wotb.control; generated traversal and V3
// getters use wotb.generated_handle. Both boxes carry the same ABI handle and
// must compose, while slots/tokens and arbitrary userdata remain rejected.
//
// The comment that used to sit here cited a note in lua_convert.h saying a
// handle-typed argument composes PushArgumentError at its own call site
// rather than growing a CheckArg* member. That note no longer exists and the
// member does - CheckArgHandle. All this wrapper still owns is the
// WotbModV3UiHandle cast and the phrase.
bool CheckArgControl(lua_State* state, int index,
                     WotbModV3UiHandle* out) noexcept {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const char* type = HandleTypeName(state, index);
    const bool valid = type &&
        (std::strcmp(type, kHandleUiControl) == 0 ||
         std::strcmp(type, "wotb.generated_handle") == 0) &&
        CheckHandle(state, index, type, &handle);
    if (!valid) {
        PushArgumentError(state, index, "a wotb.control handle");
        return false;
    }
    if (out) *out = static_cast<WotbModV3UiHandle>(handle);
    return true;
}

// The slot argument slot_attach and slot_detach take - its own handle type,
// never interchangeable with a control's, which is the entire reason a
// handle carries a type name rather than crossing as a number (rule 3).
bool CheckArgSlot(lua_State* state, int index,
                  WotbModV3UiHandle* out) noexcept {
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgHandle(state, index, kHandleUiSlot, "a wotb.slot handle",
                        &handle)) {
        return false;
    }
    if (out) *out = static_cast<WotbModV3UiHandle>(handle);
    return true;
}

// The 11 WotbModV3UiControlType values control_create's optional first
// argument accepts, and nothing else - CheckArgEnum's value-membership
// test, not merely CheckArgInteger's type test, so control_create(99) is
// refused the same way storage's get_path(0) used to be before rule 6 grew
// CheckArgEnum for exactly this gap.
constexpr lua_Integer kControlTypeValues[] = {
    WOTBMOD_V3_UI_CONTROL_CONTAINER,   WOTBMOD_V3_UI_CONTROL_TEXT,
    WOTBMOD_V3_UI_CONTROL_IMAGE,       WOTBMOD_V3_UI_CONTROL_BUTTON,
    WOTBMOD_V3_UI_CONTROL_CHECKBOX,    WOTBMOD_V3_UI_CONTROL_SLIDER,
    WOTBMOD_V3_UI_CONTROL_DROPDOWN,    WOTBMOD_V3_UI_CONTROL_TEXT_INPUT,
    WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW, WOTBMOD_V3_UI_CONTROL_LIST,
    WOTBMOD_V3_UI_CONTROL_TABS};

const EnumValues kControlType = Enum(
    kControlTypeValues,
    "CONTROL_CONTAINER, CONTROL_TEXT, CONTROL_IMAGE, CONTROL_BUTTON, "
    "CONTROL_CHECKBOX, CONTROL_SLIDER, CONTROL_DROPDOWN, "
    "CONTROL_TEXT_INPUT, CONTROL_SCROLL_VIEW, CONTROL_LIST or CONTROL_TABS");

int SpecError(lua_State* state, const char* field, const char* expected) {
    lua_pushnil(state);
    lua_pushfstring(
        state,
        "ui.create: field '%s' must be %s",
        field,
        expected);
    return 2;
}

void RawGetSpecField(lua_State* state, const char* field) {
    lua_pushstring(state, field);
    lua_rawget(state, 1);
}

bool ReadSpecString(
    lua_State* state,
    const char* field,
    const char** output,
    bool* present) {
    RawGetSpecField(state, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (present) *present = false;
        return true;
    }
    if (lua_type(state, -1) != LUA_TSTRING) {
        lua_pop(state, 1);
        SpecError(state, field, "a string");
        return false;
    }
    if (output) *output = lua_tostring(state, -1);
    if (present) *present = true;
    lua_pop(state, 1);
    return true;
}

bool ReadSpecNumber(
    lua_State* state,
    const char* field,
    float* output,
    bool* present) {
    RawGetSpecField(state, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (present) *present = false;
        return true;
    }
    if (lua_type(state, -1) != LUA_TNUMBER) {
        lua_pop(state, 1);
        SpecError(state, field, "a number");
        return false;
    }
    if (output) *output = static_cast<float>(lua_tonumber(state, -1));
    if (present) *present = true;
    lua_pop(state, 1);
    return true;
}

bool ReadSpecInteger(
    lua_State* state,
    const char* field,
    uint32_t* output,
    bool* present) {
    RawGetSpecField(state, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (present) *present = false;
        return true;
    }
    if (!lua_isinteger(state, -1)) {
        lua_pop(state, 1);
        SpecError(state, field, "an integer");
        return false;
    }
    const lua_Integer value = lua_tointeger(state, -1);
    if (value < 0 || value > UINT32_MAX) {
        lua_pop(state, 1);
        SpecError(state, field, "a non-negative 32-bit integer");
        return false;
    }
    if (output) *output = static_cast<uint32_t>(value);
    if (present) *present = true;
    lua_pop(state, 1);
    return true;
}

bool ReadSpecBoolean(
    lua_State* state,
    const char* field,
    uint32_t* output,
    bool* present) {
    RawGetSpecField(state, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (present) *present = false;
        return true;
    }
    if (lua_type(state, -1) != LUA_TBOOLEAN) {
        lua_pop(state, 1);
        SpecError(state, field, "a boolean");
        return false;
    }
    if (output) *output = lua_toboolean(state, -1) ? 1u : 0u;
    if (present) *present = true;
    lua_pop(state, 1);
    return true;
}

bool ReadSpecColor(
    lua_State* state,
    const char* field,
    WotbModV3Color* output,
    bool* present) {
    RawGetSpecField(state, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (present) *present = false;
        return true;
    }
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        SpecError(state, field, "a color table");
        return false;
    }
    const int color_index = lua_gettop(state);
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    const char* channels[] = {"r", "g", "b", "a"};
    float* values[] = {&color.r, &color.g, &color.b, &color.a};
    for (size_t index = 0u; index < 4u; ++index) {
        lua_pushstring(state, channels[index]);
        lua_rawget(state, color_index);
        if (!lua_isnil(state, -1)) {
            if (lua_type(state, -1) != LUA_TNUMBER) {
                lua_pop(state, 2);
                SpecError(state, field, "a color table with numeric r/g/b/a");
                return false;
            }
            *values[index] = static_cast<float>(lua_tonumber(state, -1));
        }
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    if (output) *output = color;
    if (present) *present = true;
    return true;
}

bool ReadSpecControl(
    lua_State* state,
    const char* field,
    WotbModV3UiHandle* output,
    bool* present) {
    RawGetSpecField(state, field);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        if (present) *present = false;
        return true;
    }
    const char* type = HandleTypeName(state, -1);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const bool valid = type &&
        (std::strcmp(type, kHandleUiControl) == 0 ||
         std::strcmp(type, "wotb.generated_handle") == 0) &&
        CheckHandle(state, -1, type, &handle);
    lua_pop(state, 1);
    if (!valid) {
        SpecError(state, field, "a UI control handle");
        return false;
    }
    if (output) *output = static_cast<WotbModV3UiHandle>(handle);
    if (present) *present = true;
    return true;
}

WotbModV3Result ApplyOptionalUiProperties(
    UiContext* ctx,
    WotbModV3UiHandle control,
    const char* font,
    bool has_font,
    float font_size,
    bool has_font_size,
    WotbModV3Color color,
    bool has_color,
    float opacity,
    bool has_opacity,
    uint32_t alignment,
    bool has_alignment,
    uint32_t text_wrap,
    bool has_text_wrap,
    uint32_t rich_text,
    bool has_rich_text,
    uint32_t enabled,
    bool has_enabled,
    uint32_t interactable,
    bool has_interactable) {
#define APPLY_OPTIONAL(condition, slot, ...)                              \
    do {                                                                  \
        if (condition) {                                                   \
            if (!ctx->api->slot) return WOTBMOD_V3_E_NOT_SUPPORTED;       \
            const WotbModV3Result applied =                               \
                ctx->api->slot(ctx->mod, control, __VA_ARGS__);            \
            if (applied != WOTBMOD_V3_OK) return applied;                  \
        }                                                                 \
    } while (false)
    APPLY_OPTIONAL(has_font, control_set_font, font);
    APPLY_OPTIONAL(has_font_size, control_set_font_size, font_size);
    APPLY_OPTIONAL(has_color, control_set_color, color);
    APPLY_OPTIONAL(has_opacity, control_set_opacity, opacity);
    APPLY_OPTIONAL(has_alignment, control_set_text_alignment, alignment);
    APPLY_OPTIONAL(has_text_wrap, control_set_text_wrap, text_wrap);
    APPLY_OPTIONAL(has_rich_text, control_set_rich_text, rich_text);
    APPLY_OPTIONAL(has_enabled, control_set_enabled, enabled);
    APPLY_OPTIONAL(
        has_interactable,
        control_set_interactable,
        interactable);
#undef APPLY_OPTIONAL
    return WOTBMOD_V3_OK;
}

// ---------------------------------------------------------------------------
// control_create, control_destroy: minting and releasing the handle every
// other slot in this file takes back.
// ---------------------------------------------------------------------------

// wotb.ui.control_create([type [, id [, template_uri]]])
//
// id and template_uri are the narrow exception to the setter-first shape:
// a DAVA YAML package needs its top-level object name and trusted VFS URI
// before the native control exists. Plain controls keep using the one-argument
// form, while text and visibility remain setter operations. The ABI's
// WotbModV3UiControlDescriptor.geometry is left zeroed the same way -
// control_set_position/control_set_size are bound too, and lua_convert.h
// has no CheckRect counterpart to read one through in the first place (see
// the geometry section below for why a WotbModV3Rect argument would need
// its own accessor rather than reusing anything Vec2-shaped).
int UiControlCreate(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    lua_Integer type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    if (!CheckOptArgEnum(state, 1, kControlType, type, &type)) return 2;
    const char* id = nullptr;
    if (!CheckOptArgString(state, 2, nullptr, &id)) return 2;
    const char* template_uri = nullptr;
    if (!CheckOptArgString(state, 3, nullptr, &template_uri)) return 2;

    WotbModV3UiControlDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_UI_VERSION);
    descriptor.type = static_cast<uint32_t>(type);
    descriptor.visible = 1u;
    descriptor.id = id;
    descriptor.texture_uri = template_uri;

    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->control_create(ctx->mod, &descriptor, &control);
    // Onto the ledger before the handle is ever handed to the script, so that
    // a control is revocable from the moment it exists rather than from the
    // moment the script does something with it. A refused record means this
    // script is being destroyed (see lua_ownership.h): the control is
    // destroyed here and now rather than handed to a script whose teardown
    // pass has already gone by, because a control nothing will ever destroy is
    // a panel left on a player's screen with no mod behind it.
    if (result == WOTBMOD_V3_OK &&
        !ctx->script->Ownership().RecordControl(
            static_cast<WotbModV3Handle>(control))) {
        ctx->api->control_destroy(ctx->mod, control);
        lua_pushnil(state);
        lua_pushliteral(state,
                        "ui.control_create: this script is being unloaded");
        return 2;
    }
    // Required, not optional: a successful create always hands back a live
    // control, exactly as storage's begin_transaction does for its token.
    PushHandle(state, static_cast<WotbModV3Handle>(control), kHandleUiControl);
    return PushResultWith(state, result, mark, "ui.control_create");
}

// wotb.ui.create({ type=..., text=..., x=..., y=..., width=..., height=... })
//
// This is the author-facing constructor. It writes the complete native
// descriptor before the first ABI crossing, then applies optional properties
// through the same frozen slots exposed separately on wotb.ui. No YAML,
// native pointer, or generated binding internals are visible to Lua.
int UiCreate(lua_State* state) noexcept {
    if (lua_type(state, 1) != LUA_TTABLE) {
        return SpecError(state, "argument", "a table");
    }
    UiContext* ctx = Context(state);
    WotbModV3UiControlDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_UI_VERSION);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.visible = 1u;

    bool present = false;
    if (!ReadSpecInteger(
            state,
            "type",
            &descriptor.type,
            &present)) return 2;
    if (descriptor.type > WOTBMOD_V3_UI_CONTROL_TABS) {
        return SpecError(state, "type", "a valid wotb.ui.CONTROL_* value");
    }
    if (!ReadSpecBoolean(
            state,
            "visible",
            &descriptor.visible,
            &present)) return 2;
    if (!ReadSpecString(state, "id", &descriptor.id, &present)) return 2;
    if (!ReadSpecString(state, "text", &descriptor.text, &present)) return 2;
    if (!ReadSpecString(
            state,
            "texture",
            &descriptor.texture_uri,
            &present)) return 2;
    if (!ReadSpecNumber(
            state,
            "x",
            &descriptor.geometry.x,
            &present)) return 2;
    if (!ReadSpecNumber(
            state,
            "y",
            &descriptor.geometry.y,
            &present)) return 2;
    if (!ReadSpecNumber(
            state,
            "width",
            &descriptor.geometry.width,
            &present)) return 2;
    if (!ReadSpecNumber(
            state,
            "height",
            &descriptor.geometry.height,
            &present)) return 2;

    const char* font = nullptr;
    bool has_font = false;
    float font_size = 0.0f;
    bool has_font_size = false;
    WotbModV3Color color = {};
    bool has_color = false;
    WotbModV3Color background_color = {};
    bool has_background_color = false;
    float opacity = 0.0f;
    bool has_opacity = false;
    uint32_t alignment = 0u;
    bool has_alignment = false;
    uint32_t text_wrap = 0u;
    bool has_text_wrap = false;
    uint32_t rich_text = 0u;
    bool has_rich_text = false;
    uint32_t enabled = 1u;
    bool has_enabled = false;
    uint32_t interactable = 1u;
    bool has_interactable = false;
    WotbModV3UiHandle parent = WOTBMOD_V3_INVALID_HANDLE;
    bool has_parent = false;

    if (!ReadSpecString(state, "font", &font, &has_font) ||
        !ReadSpecNumber(
            state,
            "font_size",
            &font_size,
            &has_font_size) ||
        !ReadSpecColor(state, "color", &color, &has_color) ||
        !ReadSpecColor(
            state,
            "background_color",
            &background_color,
            &has_background_color) ||
        !ReadSpecNumber(
            state,
            "opacity",
            &opacity,
            &has_opacity) ||
        !ReadSpecInteger(
            state,
            "alignment",
            &alignment,
            &has_alignment) ||
        !ReadSpecBoolean(
            state,
            "text_wrap",
            &text_wrap,
            &has_text_wrap) ||
        !ReadSpecBoolean(
            state,
            "rich_text",
            &rich_text,
            &has_rich_text) ||
        !ReadSpecBoolean(
            state,
            "enabled",
            &enabled,
            &has_enabled) ||
        !ReadSpecBoolean(
            state,
            "interactable",
            &interactable,
            &has_interactable) ||
        !ReadSpecControl(state, "parent", &parent, &has_parent)) {
        return 2;
    }
    if (has_alignment &&
        alignment > WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY) {
        return SpecError(state, "alignment", "a valid wotb.ui.ALIGN_* value");
    }

    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result =
        ctx->api->control_create(ctx->mod, &descriptor, &control);
    if (result != WOTBMOD_V3_OK) {
        return PushResult(state, result, "ui.create");
    }
    if (!ctx->script->Ownership().RecordControl(
            static_cast<WotbModV3Handle>(control))) {
        ctx->api->control_destroy(ctx->mod, control);
        lua_pushnil(state);
        lua_pushliteral(state, "ui.create: this script is being unloaded");
        return 2;
    }

    result = ApplyOptionalUiProperties(
        ctx,
        control,
        font,
        has_font,
        font_size,
        has_font_size,
        color,
        has_color,
        opacity,
        has_opacity,
        alignment,
        has_alignment,
        text_wrap,
        has_text_wrap,
        rich_text,
        has_rich_text,
        enabled,
        has_enabled,
        interactable,
        has_interactable);
    WotbModV3Handle style_handle = WOTBMOD_V3_INVALID_HANDLE;
    if (result == WOTBMOD_V3_OK && has_background_color) {
        if (!ctx->api->style_push || !ctx->api->style_pop) {
            result = WOTBMOD_V3_E_NOT_SUPPORTED;
        } else {
            WotbModV3UiStylePatch style = {};
            WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION);
            style.fields = WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR;
            style.background_color = background_color;
            result = ctx->api->style_push(
                ctx->mod,
                control,
                &style,
                &style_handle);
        }
    }
    if (result == WOTBMOD_V3_OK && has_parent) {
        result = ctx->api->control_add_child
            ? ctx->api->control_add_child(ctx->mod, parent, control)
            : WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (result != WOTBMOD_V3_OK) {
        if (style_handle != WOTBMOD_V3_INVALID_HANDLE) {
            ctx->api->style_pop(ctx->mod, style_handle);
        }
        ctx->api->control_destroy(ctx->mod, control);
        ctx->script->Ownership().ForgetControl(
            static_cast<WotbModV3Handle>(control));
        return PushResult(state, result, "ui.create");
    }

    PushHandle(
        state,
        static_cast<WotbModV3Handle>(control),
        kHandleUiControl);
    return 1;
}

int UiControlDestroy(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    const WotbModV3Result result = ctx->api->control_destroy(ctx->mod, control);
    // Only on success, for the same reason storage's commit forgets only on
    // success: a destroy the client refused - a permission the mod no longer
    // has, a control it never owned - leaves the control exactly where it was,
    // and a host that forgot it then would have no way to take it back at
    // teardown. Forgetting a handle the ledger does not hold is harmless, so
    // this needs no "did we create it" test of its own.
    if (result == WOTBMOD_V3_OK) {
        ctx->script->Ownership().ForgetControl(
            static_cast<WotbModV3Handle>(control));
        // Controls returned by generated V2/V3 helpers (clone, traversal,
        // buttons/dialogs) live in the generic handle ledger. The hand-written
        // create path lives in controls_; forgetting from both is harmless and
        // prevents either family from being released twice.
        ctx->script->Ownership().ForgetHandle(
            static_cast<WotbModV3Handle>(control));
    }
    return PushResult(state, result, "ui.control_destroy");
}

// ---------------------------------------------------------------------------
// The tree: add_child, remove_child, set_id, find_by_id, is_alive.
// ---------------------------------------------------------------------------

int UiControlAddChild(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle parent = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &parent)) return 2;
    WotbModV3UiHandle child = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 2, &child)) return 2;
    return PushResult(
        state, ctx->api->control_add_child(ctx->mod, parent, child),
        "ui.control_add_child");
}

int UiControlRemoveChild(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle parent = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &parent)) return 2;
    WotbModV3UiHandle child = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 2, &child)) return 2;
    return PushResult(
        state, ctx->api->control_remove_child(ctx->mod, parent, child),
        "ui.control_remove_child");
}

int UiControlSetId(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    const char* id = nullptr;
    if (!CheckArgString(state, 2, &id)) return 2;
    return PushResult(state, ctx->api->control_set_id(ctx->mod, control, id),
                      "ui.control_set_id");
}

// wotb.ui.control_find_by_id(root, id)
//
// Argument order mirrors the ABI's own (mod, root, id, out_control)
// verbatim, root before id, rather than reordering for the ergonomics of
// "root is usually omitted, so put id first". A reorder here would be a
// manual, per-slot exception in what is meant to become a table-driven
// generator over several hundred slots - every other bound slot in this
// file, and every slot storage/events bound before it, keeps strict ABI
// parameter order, and a generator emitting hundreds of these needs one
// rule with no exceptions more than any one call site needs to read well.
// The cost is real but small: a caller after the whole tree passes an
// explicit `nil` - wotb.ui.control_find_by_id(nil, 'my_id') - rather than
// omitting a trailing argument.
//
// root goes through CheckOptArgHandle, not a hand-composed
// `lua_isnoneornil(...) ? ... : CheckArgControl(...)`: absent or nil means
// "search this mod's own top-level tree", the same WOTBMOD_V3_INVALID_HANDLE
// the ABI itself treats as "no root" (client_services.cpp's UiControlFindById
// takes the CheckUiOwnPermission branch rather than resolving a handle when
// root == WOTBMOD_V3_INVALID_HANDLE) - and a *present* root is validated and
// scoped by the client exactly as any other control handle is, so a
// destroyed or wrong-typed root is refused rather than silently treated as
// "no root" or silently searching the whole tree.
//
// The return itself is the required-handle shape, not PushOptionalHandle.
// This ABI's own production implementation answers "no such id" with
// WOTBMOD_V3_E_NOT_FOUND, never WOTBMOD_V3_OK plus WOTBMOD_V3_INVALID_HANDLE
// - there is no "OK, and here is nothing" path this slot can take. A failing
// result already produces `nil, message` through PushResultWith, which is
// rule 1's own shape for "the script did not get what it asked for";
// wrapping that in `true, nil` on top would claim the call succeeded when
// the ABI just said it did not. slot_find below is the other named
// candidate in the brief, and reads the same way for the same reason - see
// its own comment for the header text that settles it there.
int UiControlFindById(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle root = WOTBMOD_V3_INVALID_HANDLE;
    if (!lua_isnoneornil(state, 1) &&
        !CheckArgControl(state, 1, &root)) {
        return 2;
    }
    const char* id = nullptr;
    if (!CheckArgString(state, 2, &id)) return 2;

    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->control_find_by_id(ctx->mod, root, id, &control);
    PushHandle(state, static_cast<WotbModV3Handle>(control), kHandleUiControl);
    return PushResultWith(state, result, mark, "ui.control_find_by_id");
}

// wotb.ui.control_is_alive(control) -> true, is_alive
//
// The contains-style shape (see lua_bind_storage.cpp's StorageContains
// comment for the reasoning this shares in full): control_is_alive's own
// contract, mirrored in client_services.cpp's UiControlIsAlive, is to
// answer WOTBMOD_V3_OK with out_alive telling the story even for a handle
// that was never created here or was destroyed long ago - asking "is this
// thing alive" must not itself fail just because the answer is no. `true,
// false` is what keeps rule 2's "the first value is falsy only when the
// script did not get what it asked for" literally true here: the script did
// get an answer, and the answer happens to be no.
int UiControlIsAlive(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    uint32_t out_alive = 0u;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->control_is_alive(ctx->mod, control, &out_alive);
    lua_pushboolean(state, 1);
    lua_pushboolean(state, out_alive != 0u);
    return PushResultWith(state, result, mark, "ui.control_is_alive");
}

// ---------------------------------------------------------------------------
// Text and visibility.
// ---------------------------------------------------------------------------

int UiControlSetText(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    const char* text = nullptr;
    if (!CheckArgString(state, 2, &text)) return 2;
    return PushResult(
        state, ctx->api->control_set_text(ctx->mod, control, text),
        "ui.control_set_text");
}

int UiControlSetVisible(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    bool visible = false;
    if (!CheckArgBoolean(state, 2, &visible)) return 2;
    return PushResult(
        state,
        ctx->api->control_set_visible(ctx->mod, control, visible ? 1u : 0u),
        "ui.control_set_visible");
}

// ---------------------------------------------------------------------------
// Geometry: position, size, anchor, pivot. Two raw numbers each way - the
// brief's own step-1 test spells control_set_size(c, 200, 80) and `local w,
// h = control_get_size(c)`, and keeping set and get symmetric (x, y in; x, y
// out) is what makes a round trip readable at the call site. This is now the
// only shape lua_convert.h documents for a Vec2 (see its own "Vectors"
// section): a Vec2 argument is CheckArgNumber twice, a Vec2 return is two
// pushed values, and there is no table-shaped alternative left to choose
// between - the table-shaped PushVec2/CheckVec2 that used to live there were
// deleted once all three shipped bindings turned out to agree on this shape
// and neither was ever called.
// ---------------------------------------------------------------------------

int UiControlSetPosition(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    lua_Number x = 0.0;
    if (!CheckArgNumber(state, 2, &x)) return 2;
    lua_Number y = 0.0;
    if (!CheckArgNumber(state, 3, &y)) return 2;
    WotbModV3Vec2 position = {static_cast<float>(x), static_cast<float>(y)};
    return PushResult(
        state, ctx->api->control_set_position(ctx->mod, control, position),
        "ui.control_set_position");
}

int UiControlGetPosition(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    WotbModV3Vec2 position = {};
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->control_get_position(ctx->mod, control, &position);
    lua_pushnumber(state, static_cast<lua_Number>(position.x));
    lua_pushnumber(state, static_cast<lua_Number>(position.y));
    return PushResultWith(state, result, mark, "ui.control_get_position");
}

int UiControlSetSize(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    lua_Number width = 0.0;
    if (!CheckArgNumber(state, 2, &width)) return 2;
    lua_Number height = 0.0;
    if (!CheckArgNumber(state, 3, &height)) return 2;
    WotbModV3Vec2 size = {static_cast<float>(width),
                          static_cast<float>(height)};
    return PushResult(
        state, ctx->api->control_set_size(ctx->mod, control, size),
        "ui.control_set_size");
}

int UiControlGetSize(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    WotbModV3Vec2 size = {};
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->control_get_size(ctx->mod, control, &size);
    lua_pushnumber(state, static_cast<lua_Number>(size.x));
    lua_pushnumber(state, static_cast<lua_Number>(size.y));
    return PushResultWith(state, result, mark, "ui.control_get_size");
}

int UiControlSetAnchor(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    lua_Number x = 0.0;
    if (!CheckArgNumber(state, 2, &x)) return 2;
    lua_Number y = 0.0;
    if (!CheckArgNumber(state, 3, &y)) return 2;
    WotbModV3Vec2 anchor = {static_cast<float>(x), static_cast<float>(y)};
    return PushResult(
        state, ctx->api->control_set_anchor(ctx->mod, control, anchor),
        "ui.control_set_anchor");
}

int UiControlSetPivot(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 1, &control)) return 2;
    lua_Number x = 0.0;
    if (!CheckArgNumber(state, 2, &x)) return 2;
    lua_Number y = 0.0;
    if (!CheckArgNumber(state, 3, &y)) return 2;
    WotbModV3Vec2 pivot = {static_cast<float>(x), static_cast<float>(y)};
    return PushResult(
        state, ctx->api->control_set_pivot(ctx->mod, control, pivot),
        "ui.control_set_pivot");
}

// ---------------------------------------------------------------------------
// Named slots: slot_find, slot_attach, slot_detach.
// ---------------------------------------------------------------------------

// wotb.ui.slot_find(slot_id)
//
// Required-handle shape - see UiControlFindById's own comment above for the
// reasoning this shares. ui_v2.h's own header comment settles this slot
// specifically: "When unavailable, slot_find returns
// WOTBMOD_V3_E_NOT_SUPPORTED and writes WOTBMOD_V3_INVALID_HANDLE" - a
// documented *failure* result, not WOTBMOD_V3_OK with nothing found. An
// unknown slot id fails the same way, with WOTBMOD_V3_E_NOT_FOUND
// (docs/API_V3_RU.md's text for this exact slot). Either way PushResultWith
// already turns it into `nil, message`; PushOptionalHandle here would turn
// a documented failure into a reported success with a nil payload, which is
// exactly the misreading rule 2's own comment warns against.
int UiSlotFind(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    const char* slot_id = nullptr;
    if (!CheckArgString(state, 1, &slot_id)) return 2;

    WotbModV3UiHandle slot = WOTBMOD_V3_INVALID_HANDLE;
    const StackMark mark = MarkStack(state);
    const WotbModV3Result result =
        ctx->api->slot_find(ctx->mod, slot_id, &slot);
    PushHandle(state, static_cast<WotbModV3Handle>(slot), kHandleUiSlot);
    return PushResultWith(state, result, mark, "ui.slot_find");
}

int UiSlotAttach(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle slot = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgSlot(state, 1, &slot)) return 2;
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 2, &control)) return 2;
    lua_Integer priority = 0;
    if (!CheckArgInteger(state, 3, &priority)) return 2;
    return PushResult(
        state,
        ctx->api->slot_attach(ctx->mod, slot, control,
                              static_cast<int32_t>(priority)),
        "ui.slot_attach");
}

int UiSlotDetach(lua_State* state) noexcept {
    UiContext* ctx = Context(state);
    WotbModV3UiHandle slot = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgSlot(state, 1, &slot)) return 2;
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    if (!CheckArgControl(state, 2, &control)) return 2;
    return PushResult(state, ctx->api->slot_detach(ctx->mod, slot, control),
                      "ui.slot_detach");
}

const luaL_Reg kUiFuncs[] = {
    {"create", &UiCreate},
    {"control_create", &UiControlCreate},
    {"control_destroy", &UiControlDestroy},
    {"control_add_child", &UiControlAddChild},
    {"control_remove_child", &UiControlRemoveChild},
    {"control_set_id", &UiControlSetId},
    {"control_find_by_id", &UiControlFindById},
    {"control_is_alive", &UiControlIsAlive},
    {"control_set_text", &UiControlSetText},
    {"control_set_visible", &UiControlSetVisible},
    {"control_set_position", &UiControlSetPosition},
    {"control_get_position", &UiControlGetPosition},
    {"control_set_size", &UiControlSetSize},
    {"control_get_size", &UiControlGetSize},
    {"control_set_anchor", &UiControlSetAnchor},
    {"control_set_pivot", &UiControlSetPivot},
    {"slot_find", &UiSlotFind},
    {"slot_attach", &UiSlotAttach},
    {"slot_detach", &UiSlotDetach},
    {nullptr, nullptr},
};

struct NamedConstant {
    const char* name;
    lua_Integer value;
};

const NamedConstant kUiConstants[] = {
    {"CONTROL_CONTAINER", WOTBMOD_V3_UI_CONTROL_CONTAINER},
    {"CONTROL_TEXT", WOTBMOD_V3_UI_CONTROL_TEXT},
    {"CONTROL_IMAGE", WOTBMOD_V3_UI_CONTROL_IMAGE},
    {"CONTROL_BUTTON", WOTBMOD_V3_UI_CONTROL_BUTTON},
    {"CONTROL_CHECKBOX", WOTBMOD_V3_UI_CONTROL_CHECKBOX},
    {"CONTROL_SLIDER", WOTBMOD_V3_UI_CONTROL_SLIDER},
    {"CONTROL_DROPDOWN", WOTBMOD_V3_UI_CONTROL_DROPDOWN},
    {"CONTROL_TEXT_INPUT", WOTBMOD_V3_UI_CONTROL_TEXT_INPUT},
    {"CONTROL_SCROLL_VIEW", WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW},
    {"CONTROL_LIST", WOTBMOD_V3_UI_CONTROL_LIST},
    {"CONTROL_TABS", WOTBMOD_V3_UI_CONTROL_TABS},
    {"ALIGN_LEFT", WOTBMOD_V3_UI_TEXT_ALIGN_LEFT},
    {"ALIGN_CENTER", WOTBMOD_V3_UI_TEXT_ALIGN_CENTER},
    {"ALIGN_RIGHT", WOTBMOD_V3_UI_TEXT_ALIGN_RIGHT},
    {"ALIGN_JUSTIFY", WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY},
    {"STYLE_COLOR", WOTBMOD_V3_UI_STYLE_COLOR},
    {"STYLE_BACKGROUND_COLOR", WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR},
    {"STYLE_OPACITY", WOTBMOD_V3_UI_STYLE_OPACITY},
    {"STYLE_FONT", WOTBMOD_V3_UI_STYLE_FONT},
    {"STYLE_FONT_SIZE", WOTBMOD_V3_UI_STYLE_FONT_SIZE},
    {"STYLE_TEXTURE", WOTBMOD_V3_UI_STYLE_TEXTURE},
    {"STYLE_Z_ORDER", WOTBMOD_V3_UI_STYLE_Z_ORDER},
    {"EVENT_CLICK", WOTBMOD_V3_UI_EVENT_CLICK},
    {"EVENT_DOUBLE_CLICK", WOTBMOD_V3_UI_EVENT_DOUBLE_CLICK},
    {"EVENT_VALUE_CHANGED", WOTBMOD_V3_UI_EVENT_VALUE_CHANGED},
    {"EVENT_TEXT_CHANGED", WOTBMOD_V3_UI_EVENT_TEXT_CHANGED},
    {"EVENT_FOCUS_GAINED", WOTBMOD_V3_UI_EVENT_FOCUS_GAINED},
    {"EVENT_FOCUS_LOST", WOTBMOD_V3_UI_EVENT_FOCUS_LOST},
    {"EVENT_POINTER_ENTER", WOTBMOD_V3_UI_EVENT_POINTER_ENTER},
    {"EVENT_POINTER_LEAVE", WOTBMOD_V3_UI_EVENT_POINTER_LEAVE},
    {"EVENT_POINTER_DOWN", WOTBMOD_V3_UI_EVENT_POINTER_DOWN},
    {"EVENT_POINTER_UP", WOTBMOD_V3_UI_EVENT_POINTER_UP},
    {"EVENT_DRAG_START", WOTBMOD_V3_UI_EVENT_DRAG_START},
    {"EVENT_DRAG", WOTBMOD_V3_UI_EVENT_DRAG},
    {"EVENT_DRAG_END", WOTBMOD_V3_UI_EVENT_DRAG_END},
    {"EVENT_SCROLL", WOTBMOD_V3_UI_EVENT_SCROLL},
    {"EVENT_SUBMIT", WOTBMOD_V3_UI_EVENT_SUBMIT},
    {"EVENT_CANCEL", WOTBMOD_V3_UI_EVENT_CANCEL},
};

// This hand-written subset shares the same conservative family fence as the
// generated UI slots. The runtime only sees the host's aggregate mod handle;
// requiring all independent UI grants here prevents one script from borrowing
// a different UI grant held by the host on its behalf.
const char* const kUiPermissions[] = {
    kPermissionNameUiModifyGame,
    "ui.create",
    "ui.modify.own",
    "battle.ui",
};

}  // namespace

void RegisterUi(lua_State* state, const WotbModV3UiApiV2* api,
                WotbModV3Handle mod, LuaScript* script) {
    if (!state || !api || !script) return;

    PushWotbTable(state);                              // [wotb]
    lua_newtable(state);                               // [wotb, ui]

    UiContext* ctx = static_cast<UiContext*>(
        lua_newuserdatauv(state, sizeof(UiContext), 0));
    ctx->api = api;
    ctx->mod = mod;
    ctx->script = script;                               // [wotb, ui, ctx]

    // Guarded, exactly as storage and events are: every one of these 18 slots
    // refuses before the ABI unless the script holds the whole UI family set.
    // The stable diagnostic keeps naming ui.modify.game. The table, names and
    // constants exist either way, so permissions change answers, never API
    // shape. The generated pass subsequently extends this same table.
    SetFuncsGuardedAll(
        state, kUiFuncs, 1, script, kUiPermissions,
        sizeof(kUiPermissions) / sizeof(kUiPermissions[0]),
        kPermissionNameUiModifyGame);                   // [wotb, ui]

    for (const NamedConstant& constant : kUiConstants) {
        lua_pushinteger(state, constant.value);
        lua_setfield(state, -2, constant.name);
    }

    lua_setfield(state, -2, "ui");                      // [wotb]
    lua_pop(state, 1);                                  // []
}

}  // namespace lua
}  // namespace wotbmod
