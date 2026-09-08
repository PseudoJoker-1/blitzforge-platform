#include "lua_bindings.h"

#include "../../include/wotbmod/interface_ids.h"
#include "../../include/wotbmod/vfs_v1.h"
#include "../../include/wotb_mod_dava_native.h"
#include "lua_permissions.h"
#include "lua_preludes.h"
#include "lua_script.h"
#include "lua_convert.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
}

namespace wotbmod {
namespace lua {
namespace {

constexpr size_t kMaxInputScriptStates = 256u;

const char kDavaClassToken[] = "wotb.dava.class_instance";
const char kDavaMaterialToken[] = "wotb.dava.nmaterial";
const char kDavaTextureToken[] = "wotb.dava.texture";
const char kDavaMeshToken[] = "wotb.dava.mesh";
const char kDavaMeshConsumerToken[] = "wotb.dava.mesh_consumer";
const char kDavaTracerToken[] = "wotb.dava.stock_tracer";

typedef uint64_t(WOTBMOD_V3_CALL* DavaCapabilitiesFn)();
typedef WotbModV3Result(WOTBMOD_V3_CALL* DavaClassRegisteredFn)(
    const char*, uint32_t*);
typedef WotbModV3Result(WOTBMOD_V3_CALL* DavaClassCreateFn)(
    WotbModV3Handle, const WotbModDavaNativeClassRequest*,
    WotbModDavaNativeToken*);
typedef WotbModV3Result(WOTBMOD_V3_CALL* DavaMaterialMutateFn)(
    WotbModV3Handle, WotbModDavaNativeToken,
    const WotbModDavaNativeMaterialMutation*);
typedef WotbModV3Result(WOTBMOD_V3_CALL* DavaMeshHotSwapFn)(
    WotbModV3Handle, WotbModDavaNativeToken, WotbModDavaNativeToken);
typedef WotbModV3Result(WOTBMOD_V3_CALL* DavaTracerCreateFn)(
    WotbModV3Handle, const WotbModDavaNativeTracerRequest*,
    WotbModDavaNativeToken*);
typedef WotbModV3Result(WOTBMOD_V3_CALL* DavaSceneMaterialFn)(
    WotbModV3Handle, const WotbModDavaNativeSceneMaterialRequest*,
    WotbModDavaNativeToken*);
typedef WotbModV3Result(WOTBMOD_V3_CALL* DavaSceneNodesFn)(
    char*, uint32_t*);
typedef WotbModV3Result(WOTBMOD_V3_CALL* CamoVisibilityGetFn)(uint32_t*);
typedef WotbModV3Result(WOTBMOD_V3_CALL* CamoVisibilitySetFn)(
    uint32_t, uint32_t*);

struct DavaLoaderBridge {
    DavaCapabilitiesFn capabilities = nullptr;
    DavaClassRegisteredFn class_is_registered = nullptr;
    DavaClassCreateFn class_create = nullptr;
    DavaMaterialMutateFn material_mutate = nullptr;
    DavaMeshHotSwapFn mesh_hot_swap = nullptr;
    DavaTracerCreateFn tracer_create = nullptr;
    /*
     * Optional, like tracer_create: a loader built before this route existed
     * exports neither, and `Ready()` deliberately does not require them - the
     * rest of wotb.dava must keep working on such a loader. The two bindings
     * answer "the client did not publish this slot" instead.
     */
    DavaSceneMaterialFn scene_material = nullptr;
    DavaSceneNodesFn scene_nodes = nullptr;
    /* Optional like scene_material: the live render-batch route. */
    DavaSceneMaterialFn scene_batch = nullptr;
    /* The native editor's camouflage visibility mask. Optional: an older
     * loader exports neither and wotb.customization stays undefined. */
    CamoVisibilityGetFn camo_visibility_get = nullptr;
    CamoVisibilitySetFn camo_visibility_set = nullptr;
    LuaDavaNativeReleaseFn release = nullptr;
    LuaDavaNativeReleaseAsyncFn release_async = nullptr;

    bool Ready() const noexcept {
        return capabilities && class_is_registered && class_create &&
               material_mutate && release && release_async;
    }
};

const DavaLoaderBridge& LoaderDavaBridge() noexcept {
    static const DavaLoaderBridge bridge = []() noexcept {
        DavaLoaderBridge result;
        HMODULE loader = GetModuleHandleW(L"wotb_mod_loader.dll");
        if (!loader) return result;
        result.capabilities = reinterpret_cast<DavaCapabilitiesFn>(
            GetProcAddress(loader, "WotbModLoader_DavaNativeCapabilities"));
        result.class_is_registered = reinterpret_cast<DavaClassRegisteredFn>(
            GetProcAddress(
                loader, "WotbModLoader_DavaNativeClassIsRegistered"));
        result.class_create = reinterpret_cast<DavaClassCreateFn>(GetProcAddress(
            loader, "WotbModLoader_DavaNativeClassCreate"));
        result.material_mutate = reinterpret_cast<DavaMaterialMutateFn>(
            GetProcAddress(
                loader, "WotbModLoader_DavaNativeMaterialMutate"));
        result.mesh_hot_swap = reinterpret_cast<DavaMeshHotSwapFn>(GetProcAddress(
            loader, "WotbModLoader_DavaNativeMeshHotSwap"));
        result.tracer_create = reinterpret_cast<DavaTracerCreateFn>(GetProcAddress(
            loader, "WotbModLoader_DavaNativeTracerCreate"));
        result.scene_material = reinterpret_cast<DavaSceneMaterialFn>(
            GetProcAddress(
                loader, "WotbModLoader_DavaNativeSceneMaterial"));
        result.scene_nodes = reinterpret_cast<DavaSceneNodesFn>(GetProcAddress(
            loader, "WotbModLoader_DavaNativeSceneNodes"));
        result.scene_batch = reinterpret_cast<DavaSceneMaterialFn>(
            GetProcAddress(loader, "WotbModLoader_DavaNativeSceneBatch"));
        result.camo_visibility_get =
            reinterpret_cast<CamoVisibilityGetFn>(GetProcAddress(
                loader, "WotbModLoader_CamouflageVisibilityGet"));
        result.camo_visibility_set =
            reinterpret_cast<CamoVisibilitySetFn>(GetProcAddress(
                loader, "WotbModLoader_CamouflageVisibilitySet"));
        result.release = reinterpret_cast<LuaDavaNativeReleaseFn>(GetProcAddress(
            loader, "WotbModLoader_DavaNativeRelease"));
        result.release_async =
            reinterpret_cast<LuaDavaNativeReleaseAsyncFn>(GetProcAddress(
                loader, "WotbModLoader_DavaNativeReleaseAsync"));
        return result;
    }();
    return bridge;
}

struct InputScriptState {
    LuaScript* script = nullptr;
    std::array<uint8_t, 256> key_down = {};
    bool cursor_owner = false;
};

SRWLOCK g_inputStateLock = SRWLOCK_INIT;
InputScriptState g_inputStates[kMaxInputScriptStates] = {};
RECT g_savedCursorClip = {};
bool g_savedCursorClipValid = false;
bool g_cursorReleaseApplied = false;
int g_showCursorIncrements = 0;

InputScriptState* FindInputStateLocked(LuaScript* script) noexcept {
    if (!script) return nullptr;
    for (InputScriptState& state : g_inputStates) {
        if (state.script == script) return &state;
    }
    return nullptr;
}

InputScriptState* ReserveInputStateLocked(LuaScript* script) noexcept {
    InputScriptState* existing = FindInputStateLocked(script);
    if (existing) return existing;
    for (InputScriptState& state : g_inputStates) {
        if (state.script) continue;
        state.script = script;
        state.key_down.fill(0u);
        state.cursor_owner = false;
        return &state;
    }
    return nullptr;
}

size_t CursorOwnerCountLocked() noexcept {
    size_t count = 0u;
    for (const InputScriptState& state : g_inputStates) {
        if (state.script && state.cursor_owner) ++count;
    }
    return count;
}

bool GameOwnsForegroundWindow() noexcept {
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD process_id = 0u;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

void ApplyCursorReleaseLocked() noexcept {
    if (CursorOwnerCountLocked() == 0u || !GameOwnsForegroundWindow()) return;
#ifndef WOTBMOD_LUA_HOST_TESTS
    if (!g_cursorReleaseApplied) {
        g_savedCursorClipValid = GetClipCursor(&g_savedCursorClip) != FALSE;
        g_showCursorIncrements = 0;
        int display_count = -1;
        do {
            display_count = ShowCursor(TRUE);
            ++g_showCursorIncrements;
        } while (display_count < 0 && g_showCursorIncrements < 64);
        g_cursorReleaseApplied = true;
    }
    ClipCursor(nullptr);
    ReleaseCapture();
    SetCursor(LoadCursorA(nullptr, IDC_ARROW));
#else
    g_cursorReleaseApplied = true;
#endif
}

void RestoreCursorLocked() noexcept {
    if (!g_cursorReleaseApplied) return;
#ifndef WOTBMOD_LUA_HOST_TESTS
    if (g_savedCursorClipValid) {
        ClipCursor(&g_savedCursorClip);
    }
    while (g_showCursorIncrements > 0) {
        ShowCursor(FALSE);
        --g_showCursorIncrements;
    }
#endif
    g_savedCursorClipValid = false;
    g_cursorReleaseApplied = false;
    g_showCursorIncrements = 0;
}

bool SetCursorOwner(LuaScript* script, bool enabled) noexcept {
    if (!script) return false;
    AcquireSRWLockExclusive(&g_inputStateLock);
    InputScriptState* state = enabled
        ? ReserveInputStateLocked(script)
        : FindInputStateLocked(script);
    if (!state && enabled) {
        ReleaseSRWLockExclusive(&g_inputStateLock);
        return false;
    }
    if (state) state->cursor_owner = enabled;
    if (CursorOwnerCountLocked() == 0u) {
        RestoreCursorLocked();
    } else {
        // The game may re-clip or hide the cursor each frame. Re-applying the
        // reversible parts keeps an open Lua window usable without repeatedly
        // changing Win32's ShowCursor counter.
        ApplyCursorReleaseLocked();
    }
    ReleaseSRWLockExclusive(&g_inputStateLock);
    return true;
}

LuaScript* InputScript(lua_State* state) noexcept {
    return static_cast<LuaScript*>(
        lua_touserdata(state, GuardedUpvalueIndex(1)));
}

int InputHotkeyDown(lua_State* state) noexcept {
    if (lua_type(state, 1) != LUA_TNUMBER || !lua_isinteger(state, 1)) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected an integer virtual-key code");
        return 2;
    }
    const lua_Integer key = lua_tointeger(state, 1);
    if (key <= 0 || key >= 256) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: virtual-key code must be in [1, 255]");
        return 2;
    }
    const bool down = GameOwnsForegroundWindow() &&
        (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    lua_pushboolean(state, down ? 1 : 0);
    return 1;
}

int InputHotkeyPressed(lua_State* state) noexcept {
    if (lua_type(state, 1) != LUA_TNUMBER || !lua_isinteger(state, 1)) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected an integer virtual-key code");
        return 2;
    }
    const lua_Integer key = lua_tointeger(state, 1);
    if (key <= 0 || key >= 256) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: virtual-key code must be in [1, 255]");
        return 2;
    }
    const bool down = GameOwnsForegroundWindow() &&
        (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    bool pressed = false;
    AcquireSRWLockExclusive(&g_inputStateLock);
    InputScriptState* input = ReserveInputStateLocked(InputScript(state));
    if (input) {
        const size_t index = static_cast<size_t>(key);
        pressed = down && input->key_down[index] == 0u;
        input->key_down[index] = down ? 1u : 0u;
    }
    ReleaseSRWLockExclusive(&g_inputStateLock);
    if (!input) {
        lua_pushnil(state);
        lua_pushstring(state, "input script-state limit reached");
        return 2;
    }
    lua_pushboolean(state, pressed ? 1 : 0);
    return 1;
}

int InputSetCursorUnlocked(lua_State* state) noexcept {
    if (lua_type(state, 1) != LUA_TBOOLEAN) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected a boolean");
        return 2;
    }
    if (!SetCursorOwner(InputScript(state), lua_toboolean(state, 1) != 0)) {
        lua_pushnil(state);
        lua_pushstring(state, "input script-state limit reached");
        return 2;
    }
    lua_pushboolean(state, 1);
    return 1;
}

int InputCursorUnlocked(lua_State* state) noexcept {
    AcquireSRWLockShared(&g_inputStateLock);
    const InputScriptState* input = FindInputStateLocked(InputScript(state));
    const bool unlocked = input && input->cursor_owner;
    ReleaseSRWLockShared(&g_inputStateLock);
    lua_pushboolean(state, unlocked ? 1 : 0);
    return 1;
}

void RegisterInputExtensions(lua_State* state, LuaScript* script) {
    if (!state || !script) return;
    PushWotbTable(state);
    lua_getfield(state, -1, "input");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }

    lua_pushlightuserdata(state, script);
    static const luaL_Reg functions[] = {
        {"hotkey_down", &InputHotkeyDown},
        {"hotkey_pressed", &InputHotkeyPressed},
        {"set_cursor_unlocked", &InputSetCursorUnlocked},
        {"cursor_unlocked", &InputCursorUnlocked},
        {nullptr, nullptr},
    };
    SetFuncsGuarded(state, functions, 1, script, "input");
    lua_pushinteger(state, VK_F8);
    lua_setfield(state, -2, "KEY_F8");
    lua_setfield(state, -2, "input");
    lua_pop(state, 1);
}

void ReleaseInputOwnership(LuaScript* script) noexcept {
    if (!script) return;
    AcquireSRWLockExclusive(&g_inputStateLock);
    InputScriptState* input = FindInputStateLocked(script);
    if (input) {
        input->cursor_owner = false;
        input->key_down.fill(0u);
        input->script = nullptr;
    }
    if (CursorOwnerCountLocked() == 0u) RestoreCursorLocked();
    ReleaseSRWLockExclusive(&g_inputStateLock);
}

LuaScript* DavaScript(lua_State* state) noexcept {
    return static_cast<LuaScript*>(
        lua_touserdata(state, GuardedUpvalueIndex(1)));
}

const WotbModV3VfsApiV1* DavaVfs(lua_State* state) noexcept {
    return static_cast<const WotbModV3VfsApiV1*>(
        lua_touserdata(state, GuardedUpvalueIndex(2)));
}

WotbModV3Handle DavaMod(lua_State* state) noexcept {
    return static_cast<WotbModV3Handle>(
        lua_tointeger(state, GuardedUpvalueIndex(3)));
}

const char* DavaResultName(WotbModV3Result result) noexcept {
    switch (result) {
        case WOTBMOD_V3_E_INVALID_ARGUMENT: return "invalid argument";
        case WOTBMOD_V3_E_NOT_SUPPORTED: return "not supported by this client";
        case WOTBMOD_V3_E_NOT_FOUND: return "not found";
        case WOTBMOD_V3_E_PERMISSION_DENIED: return "permission denied";
        case WOTBMOD_V3_E_INVALID_HANDLE: return "invalid or foreign token";
        case WOTBMOD_V3_E_WRONG_THREAD: return "wrong thread";
        case WOTBMOD_V3_E_LIMIT_REACHED: return "resource limit reached";
        case WOTBMOD_V3_E_BUFFER_TOO_SMALL: return "buffer too small";
        case WOTBMOD_V3_E_CALLBACK_FAULT: return "native callback fault";
        case WOTBMOD_V3_E_IO: return "I/O error";
        case WOTBMOD_V3_E_PARSE: return "asset parse error";
        case WOTBMOD_V3_E_PLATFORM: return "native platform error";
        default: return "native operation failed";
    }
}

int PushDavaFailure(
    lua_State* state,
    const char* operation,
    WotbModV3Result result) noexcept {
    char message[256] = {};
    std::snprintf(message, sizeof(message), "%s: %s (result=%d)",
                  operation, DavaResultName(result),
                  static_cast<int>(result));
    lua_pushnil(state);
    lua_pushstring(state, message);
    return 2;
}

const char* DavaTokenType(uint32_t kind) noexcept {
    switch (kind) {
        case WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL:
            return kDavaMaterialToken;
        case WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE:
            return kDavaTextureToken;
        case WOTBMOD_DAVA_NATIVE_OBJECT_MESH:
            return kDavaMeshToken;
        case WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER:
            return kDavaMeshConsumerToken;
        case WOTBMOD_DAVA_NATIVE_OBJECT_TRACER:
            return kDavaTracerToken;
        case WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE:
            return kDavaClassToken;
        default:
            return nullptr;
    }
}

bool ReadDavaToken(
    lua_State* state,
    int index,
    uint32_t expectedKind,
    WotbModDavaNativeToken* outToken) noexcept {
    if (!outToken) return false;
    const char* type = DavaTokenType(expectedKind);
    if (!type) return false;
    WotbModV3Handle value = 0u;
    if (!CheckHandle(state, index, type, &value) || value == 0u) return false;
    *outToken = static_cast<WotbModDavaNativeToken>(value);
    return true;
}

bool ReadAnyDavaToken(
    lua_State* state,
    int index,
    WotbModDavaNativeToken* outToken) noexcept {
    static const uint32_t kinds[] = {
        WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL,
        WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE,
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH,
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER,
        WOTBMOD_DAVA_NATIVE_OBJECT_TRACER,
        WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE,
    };
    for (uint32_t kind : kinds) {
        if (ReadDavaToken(state, index, kind, outToken)) return true;
    }
    return false;
}

bool CopyDavaName(
    lua_State* state,
    int index,
    char* output,
    size_t capacity) noexcept {
    if (!output || capacity == 0u || lua_type(state, index) != LUA_TSTRING) {
        return false;
    }
    size_t length = 0u;
    const char* value = lua_tolstring(state, index, &length);
    if (!value || length == 0u || length >= capacity ||
        std::memchr(value, '\0', length)) {
        return false;
    }
    std::memcpy(output, value, length);
    output[length] = '\0';
    return true;
}

int PushOwnedDavaToken(
    lua_State* state,
    WotbModDavaNativeToken token,
    uint32_t kind,
    const char* operation) noexcept {
    LuaScript* script = DavaScript(state);
    const DavaLoaderBridge& bridge = LoaderDavaBridge();
    if (!script || token == 0u ||
        !script->Ownership().RecordDavaNative(token)) {
        if (bridge.release_async && token != 0u) {
            (void)bridge.release_async(DavaMod(state), token);
        }
        lua_pushnil(state);
        char message[192] = {};
        std::snprintf(message, sizeof(message),
                      "%s: this script is being unloaded", operation);
        lua_pushstring(state, message);
        return 2;
    }
    const char* type = DavaTokenType(kind);
    if (!type) {
        script->Ownership().ForgetDavaNative(token);
        (void)bridge.release_async(DavaMod(state), token);
        lua_pushnil(state);
        lua_pushfstring(state, "%s: unsupported DAVA object kind", operation);
        return 2;
    }
    PushToken(state, static_cast<WotbModV3Token>(token), type);
    return 1;
}

int DavaCapabilities(lua_State* state) noexcept {
    const DavaLoaderBridge& bridge = LoaderDavaBridge();
    lua_pushinteger(state, static_cast<lua_Integer>(
        bridge.capabilities ? bridge.capabilities() : 0u));
    return 1;
}

uint64_t DavaFeatureBit(const char* feature) noexcept {
    if (!feature) return 0u;
    if (std::strcmp(feature, "yaml") == 0) {
        return WOTBMOD_DAVA_NATIVE_CAP_YAML;
    }
    if (std::strcmp(feature, "archive") == 0 ||
        std::strcmp(feature, "resource_archive") == 0) {
        return WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE;
    }
    if (std::strcmp(feature, "material") == 0 ||
        std::strcmp(feature, "nmaterial") == 0 ||
        std::strcmp(feature, "texture") == 0) {
        return WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL;
    }
    if (std::strcmp(feature, "mesh") == 0 ||
        std::strcmp(feature, "mesh_hot_swap") == 0) {
        return WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP;
    }
    if (std::strcmp(feature, "tracer") == 0 ||
        std::strcmp(feature, "stock_tracer") == 0) {
        return WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER;
    }
    if (std::strcmp(feature, "class_factory") == 0 ||
        std::strcmp(feature, "object_factory") == 0) {
        return WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY;
    }
    return 0u;
}

int DavaIsSupported(lua_State* state) noexcept {
    if (lua_type(state, 1) != LUA_TSTRING) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected a DAVA feature name");
        return 2;
    }
    const uint64_t feature = DavaFeatureBit(lua_tostring(state, 1));
    if (feature == 0u) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: unknown DAVA feature name");
        return 2;
    }
    const DavaLoaderBridge& bridge = LoaderDavaBridge();
    const uint64_t capabilities =
        bridge.capabilities ? bridge.capabilities() : 0u;
    lua_pushboolean(state, (capabilities & feature) != 0u ? 1 : 0);
    return 1;
}

int DavaClassIsRegistered(lua_State* state) noexcept {
    char className[WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME] = {};
    if (!CopyDavaName(state, 1, className, sizeof(className))) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected a valid DAVA class name");
        return 2;
    }
    uint32_t registered = 0u;
    const WotbModV3Result result =
        LoaderDavaBridge().class_is_registered(className, &registered);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.class_is_registered", result);
    }
    lua_pushboolean(state, registered ? 1 : 0);
    return 1;
}

int DavaCreateClass(lua_State* state) noexcept {
    WotbModDavaNativeClassRequest request = {};
    request.struct_size = sizeof(request);
    if (!CopyDavaName(
            state, 1, request.class_name, sizeof(request.class_name)) ||
        lua_type(state, 2) != LUA_TNUMBER || !lua_isinteger(state, 2)) {
        lua_pushnil(state);
        lua_pushstring(state,
            "arguments: expected class name, integer object kind, optional payload");
        return 2;
    }
    const lua_Integer kind = lua_tointeger(state, 2);
    if (kind <= 0 || kind > UINT32_MAX) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 2: invalid DAVA object kind");
        return 2;
    }
    request.object_kind = static_cast<uint32_t>(kind);
    if (!lua_isnoneornil(state, 3)) {
        if (lua_type(state, 3) != LUA_TSTRING) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 3: expected a string payload");
            return 2;
        }
        size_t payloadSize = 0u;
        request.payload = lua_tolstring(state, 3, &payloadSize);
        if (!request.payload || payloadSize >= UINT32_MAX) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 3: payload is too large");
            return 2;
        }
        request.payload_size = static_cast<uint32_t>(payloadSize + 1u);
    }
    WotbModDavaNativeToken token = 0u;
    const WotbModV3Result result = LoaderDavaBridge().class_create(
        DavaMod(state), &request, &token);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.class_create", result);
    }
    return PushOwnedDavaToken(
        state, token, request.object_kind, "dava.class_create");
}

int DavaCreateMaterial(lua_State* state) noexcept {
    const char* name = "wotbmod.lua.material";
    size_t nameLength = std::strlen(name);
    if (!lua_isnoneornil(state, 1)) {
        if (lua_type(state, 1) != LUA_TSTRING) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 1: expected an optional material name");
            return 2;
        }
        name = lua_tolstring(state, 1, &nameLength);
        if (!name || nameLength == 0u ||
            nameLength >= WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME ||
            std::memchr(name, '\0', nameLength)) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 1: invalid material name");
            return 2;
        }
    }
    WotbModDavaNativeClassRequest request = {};
    request.struct_size = sizeof(request);
    request.object_kind = WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL;
    strcpy_s(request.class_name, "DAVA::NMaterial");
    request.payload = name;
    request.payload_size = static_cast<uint32_t>(nameLength + 1u);
    WotbModDavaNativeToken token = 0u;
    const WotbModV3Result result = LoaderDavaBridge().class_create(
        DavaMod(state), &request, &token);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.create_material", result);
    }
    return PushOwnedDavaToken(
        state, token, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL,
        "dava.create_material");
}

/*
 * dava.vehicle_part_material([node_name [, batch_index]]) -> material | nil, err
 *
 * The material of a node in the LIVE scene - the one the game owns. This is
 * what makes painting a built-in camouflage possible: every other piece was
 * already published, and the two routes that were supposed to reach a vehicle
 * part both answer an unconditional refusal on this client
 * (`vehicle_get_part_entity` and `set_custom_camouflage`).
 *
 * `node_name` absent or empty means "the first node under the active scene that
 * has a material", which is what a caller uses before it knows what the hangar
 * calls anything. `dava.scene_nodes()` is how it finds out.
 *
 * The token is OWNED, like every other token this table hands out, and the
 * provider retained the material before minting it - so releasing this token
 * gives back what the provider added rather than a reference the mod never
 * took. Release it with `dava.release`.
 */
int DavaVehiclePartMaterial(lua_State* state) noexcept {
    if (!LoaderDavaBridge().scene_material) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "dava.vehicle_part_material: the client did not publish this slot");
        return 2;
    }
    WotbModDavaNativeSceneMaterialRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version = WOTBMOD_V3_ABI_VERSION;
    if (!lua_isnoneornil(state, 1)) {
        size_t length = 0u;
        const char* name = lua_type(state, 1) == LUA_TSTRING
            ? lua_tolstring(state, 1, &length) : nullptr;
        if (!name || length == 0u ||
            length >= sizeof(request.node_name) ||
            std::memchr(name, '\0', length)) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 1: invalid scene node name");
            return 2;
        }
        std::memcpy(request.node_name, name, length);
        request.node_name[length] = '\0';
    }
    if (!lua_isnoneornil(state, 2)) {
        if (!lua_isinteger(state, 2)) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 2: expected a batch index");
            return 2;
        }
        const lua_Integer index = lua_tointeger(state, 2);
        if (index < 0 || index > 0xFFFF) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 2: batch index out of range");
            return 2;
        }
        request.batch_index = static_cast<uint32_t>(index);
    }
    WotbModDavaNativeToken token = 0u;
    const WotbModV3Result result = LoaderDavaBridge().scene_material(
        DavaMod(state), &request, &token);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(
            state, "dava.vehicle_part_material", result);
    }
    return PushOwnedDavaToken(
        state, token, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL,
        "dava.vehicle_part_material");
}

/*
 * dava.vehicle_part_batch([node_name[, batch_index]]) -> mesh-consumer token
 *
 * The RENDER BATCH of a live-scene node, minted as the consumer kind so it
 * feeds straight into `dava.mesh_hot_swap(consumer, replacement_mesh)` - that
 * pair is a geometry change on the tank that is standing in the hangar right
 * now, with no reload behind it. Same discovery rule as vehicle_part_material:
 * an absent name means "the first node that has a render object at all".
 *
 * The token is OWNED and carries a retained batch; resolve-and-release within
 * one operation is the supported usage, holding one across a hangar rebuild is
 * not. Release it with `dava.release`.
 */
int DavaVehiclePartBatch(lua_State* state) noexcept {
    if (!LoaderDavaBridge().scene_batch) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "dava.vehicle_part_batch: the client did not publish this slot");
        return 2;
    }
    WotbModDavaNativeSceneMaterialRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version = WOTBMOD_V3_ABI_VERSION;
    if (!lua_isnoneornil(state, 1)) {
        size_t length = 0u;
        const char* name = lua_type(state, 1) == LUA_TSTRING
            ? lua_tolstring(state, 1, &length) : nullptr;
        if (!name || length == 0u ||
            length >= sizeof(request.node_name) ||
            std::memchr(name, '\0', length)) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 1: invalid scene node name");
            return 2;
        }
        std::memcpy(request.node_name, name, length);
        request.node_name[length] = '\0';
    }
    if (!lua_isnoneornil(state, 2)) {
        if (!lua_isinteger(state, 2)) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 2: expected a batch index");
            return 2;
        }
        const lua_Integer index = lua_tointeger(state, 2);
        if (index < 0 || index > 0xFFFF) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 2: batch index out of range");
            return 2;
        }
        request.batch_index = static_cast<uint32_t>(index);
    }
    WotbModDavaNativeToken token = 0u;
    const WotbModV3Result result = LoaderDavaBridge().scene_batch(
        DavaMod(state), &request, &token);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(
            state, "dava.vehicle_part_batch", result);
    }
    return PushOwnedDavaToken(
        state, token, WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER,
        "dava.vehicle_part_batch");
}

/*
 * dava.scene_nodes() -> "name
name
..." | nil, err
 *
 * Discovery, and the reason it exists is that the names cannot be read out of
 * the client's files: what the hangar calls the tank is a fact about a loaded
 * scene. A mod asks once and is told.
 */
int DavaSceneNodes(lua_State* state) noexcept {
    if (!LoaderDavaBridge().scene_nodes) {
        lua_pushnil(state);
        lua_pushstring(
            state, "dava.scene_nodes: the client did not publish this slot");
        return 2;
    }
    uint32_t size = 0u;
    WotbModV3Result result =
        LoaderDavaBridge().scene_nodes(nullptr, &size);
    if (result != WOTBMOD_V3_E_BUFFER_TOO_SMALL && result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.scene_nodes", result);
    }
    if (size == 0u || size > (1u << 20)) {
        lua_pushnil(state);
        lua_pushstring(state, "dava.scene_nodes: the scene named nothing");
        return 2;
    }
    std::vector<char> buffer(size, '\0');
    result = LoaderDavaBridge().scene_nodes(buffer.data(), &size);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.scene_nodes", result);
    }
    lua_pushstring(state, buffer.data());
    return 1;
}

int DavaCreateTexture(lua_State* state) noexcept {
    if (lua_type(state, 1) != LUA_TSTRING) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected a mod:// or game:// URI");
        return 2;
    }
    const char* uri = lua_tostring(state, 1);
    const WotbModV3VfsApiV1* vfs = DavaVfs(state);
    if (!vfs || !vfs->resolve) {
        return PushDavaFailure(
            state, "dava.create_texture", WOTBMOD_V3_E_NOT_SUPPORTED);
    }
    char physicalPath[WOTBMOD_V3_MAX_PATH * 4u] = {};
    uint32_t physicalSize = static_cast<uint32_t>(sizeof(physicalPath));
    WotbModV3Result result = vfs->resolve(
        DavaMod(state), uri, physicalPath, &physicalSize);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.create_texture resolve", result);
    }
    const size_t pathLength = std::strlen(physicalPath);
    if (pathLength == 0u || pathLength >= sizeof(physicalPath)) {
        return PushDavaFailure(
            state, "dava.create_texture resolve", WOTBMOD_V3_E_PLATFORM);
    }
    WotbModDavaNativeClassRequest request = {};
    request.struct_size = sizeof(request);
    request.object_kind = WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE;
    strcpy_s(request.class_name, "DAVA::Texture");
    request.payload = physicalPath;
    request.payload_size = static_cast<uint32_t>(pathLength + 1u);
    WotbModDavaNativeToken token = 0u;
    result = LoaderDavaBridge().class_create(
        DavaMod(state), &request, &token);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.create_texture", result);
    }
    return PushOwnedDavaToken(
        state, token, WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE,
        "dava.create_texture");
}

int DavaCreatePathClass(
    lua_State* state,
    const char* className,
    uint32_t objectKind,
    const char* operation) noexcept {
    if (lua_type(state, 1) != LUA_TSTRING) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected a mod:// or game:// URI");
        return 2;
    }
    const char* uri = lua_tostring(state, 1);
    const WotbModV3VfsApiV1* vfs = DavaVfs(state);
    if (!vfs || !vfs->resolve) {
        return PushDavaFailure(
            state, operation, WOTBMOD_V3_E_NOT_SUPPORTED);
    }
    char physicalPath[WOTBMOD_V3_MAX_PATH * 4u] = {};
    uint32_t physicalSize = static_cast<uint32_t>(sizeof(physicalPath));
    WotbModV3Result result = vfs->resolve(
        DavaMod(state), uri, physicalPath, &physicalSize);
    if (result != WOTBMOD_V3_OK) {
        char resolveOperation[96] = {};
        std::snprintf(
            resolveOperation,
            sizeof(resolveOperation),
            "%s resolve",
            operation);
        return PushDavaFailure(state, resolveOperation, result);
    }
    const size_t pathLength = std::strlen(physicalPath);
    if (pathLength == 0u || pathLength >= sizeof(physicalPath)) {
        return PushDavaFailure(
            state, operation, WOTBMOD_V3_E_PLATFORM);
    }

    WotbModDavaNativeClassRequest request = {};
    request.struct_size = sizeof(request);
    request.object_kind = objectKind;
    strcpy_s(request.class_name, className);
    request.payload = physicalPath;
    request.payload_size = static_cast<uint32_t>(pathLength + 1u);
    WotbModDavaNativeToken token = 0u;
    result = LoaderDavaBridge().class_create(
        DavaMod(state), &request, &token);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, operation, result);
    }
    return PushOwnedDavaToken(state, token, objectKind, operation);
}

int DavaCreateMesh(lua_State* state) noexcept {
    return DavaCreatePathClass(
        state,
        "DAVA::Mesh",
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH,
        "dava.create_mesh");
}

int DavaCreateMeshConsumer(lua_State* state) noexcept {
    return DavaCreatePathClass(
        state,
        "DAVA::MeshConsumer",
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER,
        "dava.create_mesh_consumer");
}

int DavaMeshHotSwap(lua_State* state) noexcept {
    WotbModDavaNativeToken consumer = 0u;
    WotbModDavaNativeToken replacement = 0u;
    if (!ReadDavaToken(
            state, 1, WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER, &consumer) ||
        !ReadDavaToken(
            state, 2, WOTBMOD_DAVA_NATIVE_OBJECT_MESH, &replacement)) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "arguments 1-2: expected mesh-consumer and mesh tokens");
        return 2;
    }
    const WotbModV3Result result = LoaderDavaBridge().mesh_hot_swap(
        DavaMod(state), consumer, replacement);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.mesh_hot_swap", result);
    }
    lua_pushboolean(state, 1);
    return 1;
}

int RunMaterialMutation(
    lua_State* state,
    WotbModDavaNativeToken material,
    WotbModDavaNativeMaterialMutation* mutation,
    const char* operation) noexcept {
    const WotbModV3Result result = LoaderDavaBridge().material_mutate(
        DavaMod(state), material, mutation);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, operation, result);
    }
    lua_pushboolean(state, 1);
    return 1;
}

bool ReadMaterialPrefix(
    lua_State* state,
    WotbModDavaNativeToken* outMaterial,
    WotbModDavaNativeMaterialMutation* mutation) noexcept {
    if (!ReadDavaToken(
            state, 1, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL, outMaterial) ||
        !mutation) {
        return false;
    }
    mutation->struct_size = sizeof(*mutation);
    return CopyDavaName(state, 2, mutation->name, sizeof(mutation->name));
}

int DavaMaterialSetProperty(lua_State* state) noexcept {
    WotbModDavaNativeToken material = 0u;
    WotbModDavaNativeMaterialMutation mutation = {};
    if (!ReadMaterialPrefix(state, &material, &mutation)) {
        lua_pushnil(state);
        lua_pushstring(state, "arguments 1-2: expected material token and property name");
        return 2;
    }
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_SET_PROPERTY;
    const char* typeName = lua_isnoneornil(state, 4)
        ? nullptr : lua_tostring(state, 4);
    uint32_t width = 0u;
    if (!typeName) {
        if (lua_type(state, 3) == LUA_TBOOLEAN) {
            mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_BOOL;
            width = 1u;
        } else if (lua_type(state, 3) == LUA_TNUMBER) {
            mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT;
            width = 1u;
        } else if (lua_type(state, 3) == LUA_TTABLE) {
            const lua_Unsigned count = lua_rawlen(state, 3);
            mutation.value_type = count == 2u
                ? WOTBMOD_DAVA_NATIVE_VALUE_FLOAT2
                : count == 3u ? WOTBMOD_DAVA_NATIVE_VALUE_FLOAT3
                : count == 4u ? WOTBMOD_DAVA_NATIVE_VALUE_FLOAT4
                              : WOTBMOD_DAVA_NATIVE_VALUE_FLOAT;
            width = count >= 2u && count <= 4u
                ? static_cast<uint32_t>(count) : 1u;
        }
    } else if (std::strcmp(typeName, "float") == 0) {
        mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT; width = 1u;
    } else if (std::strcmp(typeName, "float2") == 0) {
        mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT2; width = 2u;
    } else if (std::strcmp(typeName, "float3") == 0) {
        mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT3; width = 3u;
    } else if (std::strcmp(typeName, "float4") == 0) {
        mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT4; width = 4u;
    } else if (std::strcmp(typeName, "int") == 0) {
        mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_INT; width = 1u;
    } else if (std::strcmp(typeName, "bool") == 0) {
        mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_BOOL; width = 1u;
    }
    if (width == 0u) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 3/4: unsupported material value type");
        return 2;
    }
    mutation.array_size = 1u;
    if (!lua_isnoneornil(state, 5)) {
        if (lua_type(state, 5) != LUA_TNUMBER || !lua_isinteger(state, 5)) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 5: expected an integer array size");
            return 2;
        }
        const lua_Integer arraySize = lua_tointeger(state, 5);
        if (arraySize <= 0 || arraySize > 16) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 5: array size must be in [1, 16]");
            return 2;
        }
        mutation.array_size = static_cast<uint32_t>(arraySize);
    } else if (lua_type(state, 3) == LUA_TTABLE && width == 1u) {
        mutation.array_size = static_cast<uint32_t>(lua_rawlen(state, 3));
    }
    const uint32_t total = width * mutation.array_size;
    if (total == 0u || total > 16u) {
        lua_pushnil(state);
        lua_pushstring(state, "material property contains more than 16 floats");
        return 2;
    }
    if (mutation.value_type == WOTBMOD_DAVA_NATIVE_VALUE_BOOL) {
        if (lua_type(state, 3) != LUA_TBOOLEAN || mutation.array_size != 1u) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 3: expected one boolean");
            return 2;
        }
        mutation.bool_value = lua_toboolean(state, 3) ? 1u : 0u;
    } else if (mutation.value_type == WOTBMOD_DAVA_NATIVE_VALUE_INT) {
        if (lua_type(state, 3) != LUA_TNUMBER ||
            !lua_isinteger(state, 3) || mutation.array_size != 1u) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 3: expected one integer");
            return 2;
        }
        const lua_Integer value = lua_tointeger(state, 3);
        if (value < INT32_MIN || value > INT32_MAX) {
            lua_pushnil(state);
            lua_pushstring(state, "argument 3: integer is outside int32 range");
            return 2;
        }
        mutation.int_value = static_cast<int32_t>(value);
    } else if (lua_type(state, 3) == LUA_TNUMBER && total == 1u) {
        mutation.values[0] = static_cast<float>(lua_tonumber(state, 3));
    } else if (lua_type(state, 3) == LUA_TTABLE &&
               lua_rawlen(state, 3) == total) {
        for (uint32_t index = 0u; index < total; ++index) {
            lua_geti(state, 3, static_cast<lua_Integer>(index + 1u));
            if (lua_type(state, -1) != LUA_TNUMBER) {
                lua_pop(state, 1);
                lua_pushnil(state);
                lua_pushstring(state, "argument 3: property table must contain only numbers");
                return 2;
            }
            mutation.values[index] = static_cast<float>(lua_tonumber(state, -1));
            lua_pop(state, 1);
        }
    } else {
        lua_pushnil(state);
        lua_pushstring(state, "argument 3: value shape does not match property type");
        return 2;
    }
    return RunMaterialMutation(
        state, material, &mutation, "dava.material_set_property");
}

int DavaMaterialNamedMutation(
    lua_State* state,
    uint32_t mutationKind,
    const char* operation) noexcept {
    WotbModDavaNativeToken material = 0u;
    WotbModDavaNativeMaterialMutation mutation = {};
    if (!ReadMaterialPrefix(state, &material, &mutation)) {
        lua_pushnil(state);
        lua_pushstring(state, "arguments: expected material token and name");
        return 2;
    }
    mutation.mutation_kind = mutationKind;
    return RunMaterialMutation(state, material, &mutation, operation);
}

/*
 * The three PRESENCE QUERIES: true, false, or a refusal.
 *
 * They exist so that taking a camouflage off can stop being destructive. A
 * rollback used to remove every name it had set, including ones the GAME had
 * already set, because nothing could tell the two apart - that is what
 * "presence-only restore" (ruling R11) meant. With these, apply records which
 * names pre-existed and rollback leaves exactly those alone.
 *
 * ABSENT IS `false`, NOT AN ERROR, and that distinction is the whole point: a
 * caller asking "is this name here" gets an answer for both cases and a refusal
 * only when nobody could tell. Folding "no" into the error channel would make
 * the common case indistinguishable from a broken material.
 */
int DavaMaterialQuery(
    lua_State* state,
    uint32_t mutationKind,
    const char* operation) noexcept {
    WotbModDavaNativeToken material = 0u;
    WotbModDavaNativeMaterialMutation mutation = {};
    if (!ReadMaterialPrefix(state, &material, &mutation)) {
        lua_pushnil(state);
        lua_pushstring(state, "arguments: expected material token and name");
        return 2;
    }
    mutation.mutation_kind = mutationKind;
    const WotbModV3Result result = LoaderDavaBridge().material_mutate(
        DavaMod(state), material, &mutation);
    if (result == WOTBMOD_V3_OK) {
        lua_pushboolean(state, 1);
        return 1;
    }
    if (result == WOTBMOD_V3_E_NOT_FOUND) {
        lua_pushboolean(state, 0);
        return 1;
    }
    return PushDavaFailure(state, operation, result);
}

int DavaMaterialHasProperty(lua_State* state) noexcept {
    return DavaMaterialQuery(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_PROPERTY,
        "dava.material_has_property");
}

int DavaMaterialHasTexture(lua_State* state) noexcept {
    return DavaMaterialQuery(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_TEXTURE,
        "dava.material_has_texture");
}

int DavaMaterialHasFlag(lua_State* state) noexcept {
    return DavaMaterialQuery(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_FLAG,
        "dava.material_has_flag");
}

int DavaMaterialRemoveProperty(lua_State* state) noexcept {
    return DavaMaterialNamedMutation(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_PROPERTY,
        "dava.material_remove_property");
}

int DavaMaterialSetFlag(lua_State* state) noexcept {
    WotbModDavaNativeToken material = 0u;
    WotbModDavaNativeMaterialMutation mutation = {};
    if (!ReadMaterialPrefix(state, &material, &mutation) ||
        lua_type(state, 3) != LUA_TNUMBER || !lua_isinteger(state, 3)) {
        lua_pushnil(state);
        lua_pushstring(state, "arguments: expected material token, flag name and integer value");
        return 2;
    }
    const lua_Integer value = lua_tointeger(state, 3);
    if (value < INT32_MIN || value > INT32_MAX) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 3: flag value is outside int32 range");
        return 2;
    }
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_SET_FLAG;
    mutation.int_value = static_cast<int32_t>(value);
    return RunMaterialMutation(
        state, material, &mutation, "dava.material_set_flag");
}

int DavaMaterialRemoveFlag(lua_State* state) noexcept {
    return DavaMaterialNamedMutation(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_FLAG,
        "dava.material_remove_flag");
}

int DavaMaterialSetTexture(lua_State* state) noexcept {
    WotbModDavaNativeToken material = 0u;
    WotbModDavaNativeToken texture = 0u;
    WotbModDavaNativeMaterialMutation mutation = {};
    if (!ReadMaterialPrefix(state, &material, &mutation) ||
        !ReadDavaToken(
            state, 3, WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE, &texture)) {
        lua_pushnil(state);
        lua_pushstring(state, "arguments: expected material token, sampler name and texture token");
        return 2;
    }
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE;
    mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_TEXTURE;
    mutation.related_object = texture;
    return RunMaterialMutation(
        state, material, &mutation, "dava.material_set_texture");
}

int DavaMaterialRemoveTexture(lua_State* state) noexcept {
    return DavaMaterialNamedMutation(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_TEXTURE,
        "dava.material_remove_texture");
}

int DavaMaterialSetFx(lua_State* state) noexcept {
    return DavaMaterialNamedMutation(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_SET_FX,
        "dava.material_set_fx");
}

int DavaMaterialSetQuality(lua_State* state) noexcept {
    return DavaMaterialNamedMutation(
        state, WOTBMOD_DAVA_NATIVE_MATERIAL_SET_QUALITY,
        "dava.material_set_quality");
}

int DavaMaterialApply(lua_State* state) noexcept {
    WotbModDavaNativeToken material = 0u;
    WotbModDavaNativeToken consumer = 0u;
    if (!ReadDavaToken(
            state, 1, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL, &material) ||
        !ReadDavaToken(
            state,
            2,
            WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER,
            &consumer)) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "arguments 1-2: expected material and mesh-consumer tokens");
        return 2;
    }
    WotbModDavaNativeMaterialMutation mutation = {};
    mutation.struct_size = sizeof(mutation);
    mutation.mutation_kind =
        WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER;
    mutation.related_object = consumer;
    return RunMaterialMutation(
        state, material, &mutation, "dava.material_apply");
}

bool ReadVec3Field(
    lua_State* state,
    int tableIndex,
    const char* field,
    float output[3]) noexcept {
    tableIndex = lua_absindex(state, tableIndex);
    lua_getfield(state, tableIndex, field);
    if (lua_type(state, -1) != LUA_TTABLE || lua_rawlen(state, -1) != 3u) {
        lua_pop(state, 1);
        return false;
    }
    for (int index = 0; index < 3; ++index) {
        lua_geti(state, -1, index + 1);
        if (lua_type(state, -1) != LUA_TNUMBER) {
            lua_pop(state, 2);
            return false;
        }
        const lua_Number value = lua_tonumber(state, -1);
        lua_pop(state, 1);
        if (!std::isfinite(static_cast<double>(value))) {
            lua_pop(state, 1);
            return false;
        }
        output[index] = static_cast<float>(value);
    }
    lua_pop(state, 1);
    return true;
}

int DavaCreateStockTracer(lua_State* state) noexcept {
    if (lua_type(state, 1) != LUA_TTABLE) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected a stock tracer options table");
        return 2;
    }
    WotbModDavaNativeTracerRequest request = {};
    request.struct_size = sizeof(request);
    request.color[0] = 1.0f;
    request.color[1] = 1.0f;
    request.color[2] = 1.0f;
    request.color[3] = 1.0f;
    request.width = 1.0f;
    request.lifetime_seconds = 1.0f;
    if (!ReadVec3Field(state, 1, "origin", request.origin) ||
        !ReadVec3Field(state, 1, "destination", request.destination)) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "argument 1: origin and destination must be finite {x, y, z} arrays");
        return 2;
    }
    lua_getfield(state, 1, "shell_type");
    if (!lua_isnil(state, -1)) {
        if (!lua_isinteger(state, -1)) {
            lua_pop(state, 1);
            lua_pushnil(state);
            lua_pushstring(state, "argument 1.shell_type: expected an integer in [0, 24]");
            return 2;
        }
        const lua_Integer shellType = lua_tointeger(state, -1);
        if (shellType < 0 || shellType > 24) {
            lua_pop(state, 1);
            lua_pushnil(state);
            lua_pushstring(state, "argument 1.shell_type: expected an integer in [0, 24]");
            return 2;
        }
        request.shell_type = static_cast<uint32_t>(shellType);
    }
    lua_pop(state, 1);

    const DavaLoaderBridge& bridge = LoaderDavaBridge();
    if (!bridge.tracer_create) {
        return PushDavaFailure(
            state, "dava.create_stock_tracer", WOTBMOD_V3_E_NOT_SUPPORTED);
    }
    WotbModDavaNativeToken tracer = 0u;
    const WotbModV3Result result = bridge.tracer_create(
        DavaMod(state), &request, &tracer);
    if (result == WOTBMOD_V3_E_NOT_FOUND) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "dava.create_stock_tracer: available only while a battle is active");
        return 2;
    }
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.create_stock_tracer", result);
    }
    return PushOwnedDavaToken(
        state, tracer, WOTBMOD_DAVA_NATIVE_OBJECT_TRACER,
        "dava.create_stock_tracer");
}

int DavaRelease(lua_State* state) noexcept {
    WotbModDavaNativeToken token = 0u;
    if (!ReadAnyDavaToken(state, 1, &token)) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected a DAVA object token");
        return 2;
    }
    const WotbModV3Result result = LoaderDavaBridge().release_async(
        DavaMod(state), token);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(state, "dava.release", result);
    }
    LuaScript* script = DavaScript(state);
    if (script) script->Ownership().ForgetDavaNative(token);
    lua_pushboolean(state, 1);
    return 1;
}

void PushDavaUpvalues(
    lua_State* state,
    LuaScript* script,
    const WotbModV3VfsApiV1* vfs,
    WotbModV3Handle mod) {
    lua_pushlightuserdata(state, script);
    lua_pushlightuserdata(state, const_cast<WotbModV3VfsApiV1*>(vfs));
    lua_pushinteger(state, static_cast<lua_Integer>(mod));
}

/*
 * wotb.customization.get_camo_visibility_mask() -> mask | nil, err
 * wotb.customization.set_camo_visibility_mask(mask) -> true, previous | nil, err
 *
 * The native editor's allowed-lock-state bitmask. The shipped value is 3:
 * states 0 and 1 are shown, state 2 ("the server never mentioned this name")
 * is dropped before a card exists - which is exactly where every locally
 * authored camouflage died. Setting bit 2 (mask 7) lets them through.
 *
 * This is a data patch behind the loader's fingerprint gate: on any client
 * whose fingerprint does not match, both calls answer an error rather than
 * touching memory. set returns the PREVIOUS mask as its second value, so the
 * caller can restore exactly what was there; only values 0..7 are accepted,
 * because wider bits have no meaning in the field.
 */
int CustomizationGetCamoVisibilityMask(lua_State* state) noexcept {
    if (!LoaderDavaBridge().camo_visibility_get) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "customization.get_camo_visibility_mask: "
            "the client did not publish this slot");
        return 2;
    }
    uint32_t mask = 0u;
    const WotbModV3Result result =
        LoaderDavaBridge().camo_visibility_get(&mask);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(
            state, "customization.get_camo_visibility_mask", result);
    }
    lua_pushinteger(state, static_cast<lua_Integer>(mask));
    return 1;
}

int CustomizationSetCamoVisibilityMask(lua_State* state) noexcept {
    if (!LoaderDavaBridge().camo_visibility_set) {
        lua_pushnil(state);
        lua_pushstring(
            state,
            "customization.set_camo_visibility_mask: "
            "the client did not publish this slot");
        return 2;
    }
    if (!lua_isinteger(state, 1)) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: expected an integer bitmask 0..7");
        return 2;
    }
    const lua_Integer requested = lua_tointeger(state, 1);
    if (requested < 0 || requested > 7) {
        lua_pushnil(state);
        lua_pushstring(state, "argument 1: bitmask out of range 0..7");
        return 2;
    }
    uint32_t previous = 0u;
    const WotbModV3Result result = LoaderDavaBridge().camo_visibility_set(
        static_cast<uint32_t>(requested), &previous);
    if (result != WOTBMOD_V3_OK) {
        return PushDavaFailure(
            state, "customization.set_camo_visibility_mask", result);
    }
    lua_pushboolean(state, 1);
    lua_pushinteger(state, static_cast<lua_Integer>(previous));
    return 2;
}

void RegisterDavaExtensions(
    lua_State* state,
    LuaScript* script,
    const WotbModV3VfsApiV1* vfs,
    WotbModV3Handle mod) {
    if (!state || !script || !LoaderDavaBridge().Ready()) return;
    PushWotbTable(state);
    lua_newtable(state);
    PushDavaUpvalues(state, script, vfs, mod);
    static const luaL_Reg renderFunctions[] = {
        {"capabilities", &DavaCapabilities},
        {"is_supported", &DavaIsSupported},
        {"class_is_registered", &DavaClassIsRegistered},
        {"class_create", &DavaCreateClass},
        {"create_material", &DavaCreateMaterial},
        {"material_set_property", &DavaMaterialSetProperty},
        {"material_remove_property", &DavaMaterialRemoveProperty},
        {"material_set_flag", &DavaMaterialSetFlag},
        {"material_remove_flag", &DavaMaterialRemoveFlag},
        {"material_set_texture", &DavaMaterialSetTexture},
        {"material_remove_texture", &DavaMaterialRemoveTexture},
        {"material_has_property", &DavaMaterialHasProperty},
        {"material_has_texture", &DavaMaterialHasTexture},
        {"material_has_flag", &DavaMaterialHasFlag},
        {"material_set_fx", &DavaMaterialSetFx},
        {"material_set_quality", &DavaMaterialSetQuality},
        {"material_apply", &DavaMaterialApply},
        {"mesh_hot_swap", &DavaMeshHotSwap},
        {"vehicle_part_material", &DavaVehiclePartMaterial},
        {"vehicle_part_batch", &DavaVehiclePartBatch},
        {"scene_nodes", &DavaSceneNodes},
        {"create_stock_tracer", &DavaCreateStockTracer},
        {"release", &DavaRelease},
        {nullptr, nullptr},
    };
    SetFuncsGuarded(
        state, renderFunctions, 3, script, "gameplay.tweak.cosmetic");

    PushDavaUpvalues(state, script, vfs, mod);
    static const luaL_Reg textureFunctions[] = {
        {"create_texture", &DavaCreateTexture},
        {"create_mesh", &DavaCreateMesh},
        {"create_mesh_consumer", &DavaCreateMeshConsumer},
        {nullptr, nullptr},
    };
    const char* texturePermissions[] = {
        "gameplay.tweak.cosmetic", "resources.mod"};
    SetFuncsGuardedAll(
        state, textureFunctions, 3, script, texturePermissions,
        sizeof(texturePermissions) / sizeof(texturePermissions[0]),
        "gameplay.tweak.cosmetic + resources.mod");

    lua_pushinteger(state, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL);
    lua_setfield(state, -2, "OBJECT_NMATERIAL");
    lua_pushinteger(state, WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE);
    lua_setfield(state, -2, "OBJECT_TEXTURE");
    lua_pushinteger(state, WOTBMOD_DAVA_NATIVE_OBJECT_MESH);
    lua_setfield(state, -2, "OBJECT_MESH");
    lua_pushinteger(state, WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER);
    lua_setfield(state, -2, "OBJECT_MESH_CONSUMER");
    lua_pushinteger(state, WOTBMOD_DAVA_NATIVE_OBJECT_TRACER);
    lua_setfield(state, -2, "OBJECT_TRACER");
    lua_pushinteger(state, WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE);
    lua_setfield(state, -2, "OBJECT_CLASS_INSTANCE");
    lua_setfield(state, -2, "dava");

    /*
     * wotb.customization - the native editor's own knobs. Registered beside
     * dava rather than inside it because it is not a DAVA surface: it talks
     * to the client's customization data model. Same guard fence as every
     * reviewed route; the mask is a cosmetic concern and rides the cosmetic
     * permission.
     */
    static const luaL_Reg customizationFunctions[] = {
        {"get_camo_visibility_mask", &CustomizationGetCamoVisibilityMask},
        {"set_camo_visibility_mask", &CustomizationSetCamoVisibilityMask},
        {nullptr, nullptr},
    };
    lua_newtable(state);
    lua_pushlightuserdata(state, script);
    SetFuncsGuarded(
        state,
        customizationFunctions,
        1,
        script,
        "gameplay.tweak.cosmetic");
    lua_setfield(state, -2, "customization");
    lua_pop(state, 1);
}

// Queries one interface off bootstrap, treating every way of not getting it
// (no bootstrap, no query_interface slot, or the query itself failing) as
// the same outcome: nullptr. Every Register* function already has a
// fallback for "this interface is unavailable" — no wotb.storage table, no
// core.log routing for print — so there is nothing here for a caller to
// react to beyond that.
//
// The recorded decision the generated bindings rely on: every
// Register* function below dereferences api's own slots unconditionally
// once api itself is non-null. Generated bindings additionally test each slot
// pointer before invoking it and return an unavailable error. That is safe
// because this function
// is the one gate - a client either answers a versioned query_interface
// call with a complete, ABI-conformant vtable for the version it named, or
// it fails the query and QueryInterfaceOrNull returns nullptr, which every
// Register* function already treats as "leave this table undefined". There
// is no third outcome in this ABI where query_interface succeeds and hands
// back a struct with holes in it - version negotiation happens once, here,
// not per slot. storage and events already relied on this before ui did; it
// is not a new hazard. The generated source keeps this query boundary and a
// per-slot null guard, so a client with an unavailable optional slot cannot be
// dereferenced by Lua.
const void* QueryInterfaceOrNull(const WotbModV3Bootstrap* bootstrap,
                                 WotbModV3Handle mod, const char* name,
                                 uint32_t version) {
    if (!bootstrap || !bootstrap->query_interface) return nullptr;
    const void* iface = nullptr;
    if (bootstrap->query_interface(mod, name, version, &iface) != WOTBMOD_V3_OK) {
        return nullptr;
    }
    return iface;
}

// Convenience-only Lua surface over the frozen entity_public ABI. Keeping
// grouping here means local/ally/enemy views can evolve without adding a C
// struct or exposing a native pointer. The underlying enumerate_visible slot
// remains the security boundary: hidden enemies never enter this table.
const char kPlayersLibrary[] = R"lua(
do
  local players = {}

  local function api(where)
    local value = wotb and wotb.entity_public
    if type(value) ~= "table" or type(value.enumerate_visible) ~= "function" then
      return nil, (where or "players.snapshot") ..
          ": entity_public is unavailable"
    end
    return value
  end

  local function by_public_id(left, right)
    return (left.public_id or 0) < (right.public_id or 0)
  end

  -- Provenance from the data rather than a constant. The loader publishes a
  -- pose only for a vehicle whose appearance chain it has verified, and a
  -- published direction is a unit vector (the ingress refuses anything
  -- else); an unsourced one is the zero vector the frozen snapshot is
  -- initialised with. A direction of length 1, within float error, is
  -- therefore the honest sign of a sourced transform, and position follows
  -- it because both come from the same matrix. (0, 0, 0) is not a direction
  -- and never reports available.
  local function has_pose(direction)
    if type(direction) ~= "table" then return false end
    local x, y, z = direction.x, direction.y, direction.z
    if type(x) ~= "number" or type(y) ~= "number" or type(z) ~= "number" then
      return false
    end
    local length = x * x + y * y + z * z
    return length > 0.98 and length < 1.02
  end

  function players.snapshot()
    local entity_api, api_error = api()
    if not entity_api then return nil, api_error end

    local visible = {}
    local ok, enumerate_error = entity_api.enumerate_visible(function(value)
      if type(value) == "table" and value.type == 1 then
        visible[#visible + 1] = value
      end
    end)
    if not ok then
      return nil, "players.snapshot: " .. tostring(enumerate_error)
    end

    table.sort(visible, by_public_id)
    local local_player = nil
    local local_team = 0
    for _, value in ipairs(visible) do
      if value.local_player ~= 0 then
        local_player = value
        local_team = tonumber(value.team) or 0
        break
      end
    end
    local local_team_inferred = false
    if local_team == 0 then
      local team_counts = {}
      local best_team = 0
      local best_count = 0
      local tied = false
      for _, value in ipairs(visible) do
        local team = tonumber(value.team) or 0
        if team > 0 then
          team_counts[team] = (team_counts[team] or 0) + 1
          if team_counts[team] > best_count then
            best_team = team
            best_count = team_counts[team]
            tied = false
          elseif team ~= best_team and team_counts[team] == best_count then
            tied = true
          end
        end
      end
      -- Hidden enemies never enter enumerate_visible. Before the local-player
      -- hook fires, the only unique multi-vehicle team is therefore our team.
      if best_count >= 2 and not tied then
        local_team = best_team
        local_team_inferred = true
      end
    end

    local our_team = {}
    local enemy_team = {}
    local unknown_team = {}
    for _, value in ipairs(visible) do
      value.is_local = value.local_player ~= 0
      value.is_visible = value.visible_to_player ~= 0
      value.alive = (tonumber(value.health) or 0) > 0
      value.team_available = (tonumber(value.team) or 0) > 0
      value.display_name_available = type(value.display_name) == "string" and
                                     value.display_name ~= ""
      local sourced = has_pose(value.direction)
      value.position_available = sourced
      value.direction_available = sourced
      local maximum = tonumber(value.max_health) or 0
      value.health_percent = maximum > 0 and
          ((tonumber(value.health) or 0) * 100.0 / maximum) or nil

      if value.is_local then
        value.relation = "local_player"
        value.is_ally = true
        value.is_enemy = false
        our_team[#our_team + 1] = value
      elseif local_team > 0 and value.team_available then
        value.is_ally = value.team == local_team
        value.is_enemy = value.team ~= local_team
        value.relation = value.is_enemy and "enemy" or "ally"
        local target = value.is_enemy and enemy_team or our_team
        target[#target + 1] = value
      else
        value.relation = "unknown"
        value.is_ally = false
        value.is_enemy = false
        unknown_team[#unknown_team + 1] = value
      end
    end

    return {
      local_player = local_player,
      our_team = our_team,
      enemy_team = enemy_team,
      unknown_team = unknown_team,
      visible_players = visible,
      relation_available = local_team > 0,
      local_team = local_team,
      local_team_inferred = local_team_inferred,
      team_data_complete = local_team > 0 and #unknown_team == 0,
      enemy_scope = "currently_visible_only",
    }
  end

  local function list(name)
    local snapshot, snapshot_error = players.snapshot()
    if not snapshot then return nil, snapshot_error end
    return snapshot[name]
  end

  function players.local_player()
    local snapshot, snapshot_error = players.snapshot()
    if not snapshot then return nil, snapshot_error end
    if snapshot.local_player == nil then return true, nil end
    return snapshot.local_player
  end

  function players.our_team() return list("our_team") end
  function players.enemy_team() return list("enemy_team") end
  function players.unknown_team() return list("unknown_team") end
  function players.visible() return list("visible_players") end

  function players.find(public_id)
    if type(public_id) ~= "number" then
      return nil, "argument 1: expected an integer public_id"
    end
    local visible, visible_error = players.visible()
    if not visible then return nil, visible_error end
    for _, value in ipairs(visible) do
      if value.public_id == public_id then return value end
    end
    return true, nil
  end

  -- Short names for the same answers. `me` rather than `local`: local is a
  -- reserved word in Lua and could only be spelled players["local"]().
  -- allies() are the teammates other than the local player; our_team() keeps
  -- including it. by_id is find.
  function players.me()
    return players.local_player()
  end

  function players.allies()
    local snapshot, snapshot_error = players.snapshot()
    if not snapshot then return nil, snapshot_error end
    local list = {}
    for _, value in ipairs(snapshot.our_team) do
      if not value.is_local then list[#list + 1] = value end
    end
    return list
  end

  function players.enemies()
    return list("enemy_team")
  end

  players.by_id = players.find

  -- fn(record) for every visible vehicle in public_id order; fn returning
  -- false stops the walk. Answers the number of records visited.
  function players.each_visible(fn)
    if type(fn) ~= "function" then
      return nil, "players.each_visible: argument 1: expected a function"
    end
    local visible, visible_error = players.visible()
    if not visible then return nil, visible_error end
    local visited = 0
    for _, value in ipairs(visible) do
      visited = visited + 1
      if fn(value) == false then break end
    end
    return visited
  end

  -- The roster fields the loader publishes by name only, because the
  -- snapshot ABI is frozen: clan_tag, account_id, kills, vehicle_name
  -- ("nation:tag"), vehicle_display_name (localized). Five ABI calls per
  -- vehicle, which is why they are fetched on request and never inside
  -- snapshot(). A field the client refuses is absent and named in
  -- `unavailable` with the client's own reason; nothing is zero-filled.
  local DETAIL_NAMES = { "clan_tag", "account_id", "kills", "vehicle_name",
                         "vehicle_display_name" }
  -- WotbModV3PublicValueType, spelled here because base-header enums are
  -- deliberately not published as constants.
  local VALUE_BOOL, VALUE_INT64, VALUE_DOUBLE, VALUE_VEC3, VALUE_STRING =
      1, 2, 3, 4, 5

  local function unwrap(value)
    if type(value) ~= "table" or type(value.value) ~= "table" then
      return nil, "malformed public value"
    end
    local kind, inner = value.type, value.value
    if kind == VALUE_BOOL then return inner.boolean ~= 0 end
    if kind == VALUE_INT64 then return inner.integer end
    if kind == VALUE_DOUBLE then return inner.number end
    if kind == VALUE_VEC3 then return inner.vec3 end
    if kind == VALUE_STRING then return inner.string_value end
    return nil, "public value of unknown type " .. tostring(kind)
  end

  function players.details(target)
    local entity_api, api_error = api("players.details")
    if not entity_api then return nil, api_error end
    if type(entity_api.get_public_property) ~= "function" then
      return nil, "players.details: entity_public.get_public_property is " ..
          "unavailable on this client"
    end
    local record = target
    if type(target) == "number" then
      local found, find_error = players.find(target)
      if found == nil then
        return nil, "players.details: " .. tostring(find_error)
      end
      if found == true then
        return nil, "players.details: no visible vehicle has public_id " ..
            tostring(target)
      end
      record = found
    end
    if type(record) ~= "table" or record.handle == nil then
      return nil, "players.details: argument 1: expected a player record " ..
          "or a public_id"
    end
    local details = { public_id = record.public_id, unavailable = {} }
    for _, name in ipairs(DETAIL_NAMES) do
      local value, get_error =
          entity_api.get_public_property(record.handle, name)
      if value == nil then
        details.unavailable[name] = tostring(get_error)
      else
        local plain, unwrap_error = unwrap(value)
        if plain == nil then
          details.unavailable[name] = unwrap_error
        else
          details[name] = plain
        end
      end
    end
    -- The loader answers "frags" as a second spelling of kills; one call is
    -- enough to answer both.
    if details.kills ~= nil then details.frags = details.kills end
    return details
  end

  wotb.players = players
end
)lua";

// Lua-only context helpers over the frozen core.get_context slot.  The
// constants intentionally mirror WotbModV3Context without adding another ABI
// surface, while should_show/apply_visibility give UI authors one fail-closed
// place to enforce battle-only and mod-catalog exclusions.
const char kContextLibrary[] = R"lua(
do
  local context = {
    NONE = 0,
    LOADING = 1,
    HANGAR = 2,
    BATTLE = 4,
    REPLAY = 8,
    TRAINING = 16,
    RESULTS = 32,
    MOD_SCREEN = 64,
    TEXT_INPUT = 128,
    ALL = 255,
  }

  local function checked_integer(value, argument)
    if type(value) ~= "number" or value % 1 ~= 0 or value < 0 then
      return nil, argument .. ": expected a non-negative integer mask"
    end
    return value
  end

  function context.current()
    local core = wotb and wotb.core
    if type(core) ~= "table" or type(core.get_context) ~= "function" then
      return nil, "context.current: core.get_context is unavailable"
    end
    local value, context_error = core.get_context()
    if value == nil then
      return nil, "context.current: " .. tostring(context_error)
    end
    return value
  end

  function context.contains(value, flag)
    local checked_value, value_error = checked_integer(value, "argument 1")
    if checked_value == nil then return nil, value_error end
    local checked_flag, flag_error = checked_integer(flag, "argument 2")
    if checked_flag == nil then return nil, flag_error end
    return (checked_value & checked_flag) ~= 0
  end

  function context.should_show(allowed_mask, blocked_mask, current_mask)
    local allowed, allowed_error = checked_integer(
        allowed_mask == nil and context.ALL or allowed_mask, "argument 1")
    if allowed == nil then return nil, allowed_error end
    local blocked, blocked_error = checked_integer(
        blocked_mask == nil and context.MOD_SCREEN or blocked_mask,
        "argument 2")
    if blocked == nil then return nil, blocked_error end

    local current = current_mask
    if current == nil then
      local current_error
      current, current_error = context.current()
      if current == nil then return nil, current_error end
    else
      local current_error
      current, current_error = checked_integer(current, "argument 3")
      if current == nil then return nil, current_error end
    end

    if current == context.NONE then return false end
    return (current & allowed) ~= 0 and (current & blocked) == 0
  end

  function context.apply_visibility(
      control, allowed_mask, blocked_mask, current_mask)
    if control == nil or type(control.set_visible) ~= "function" then
      return nil, "argument 1: expected a UI control"
    end
    local visible, visibility_error = context.should_show(
        allowed_mask, blocked_mask, current_mask)
    if visible == nil then return nil, visibility_error end
    local ok, set_error = control:set_visible(visible)
    if not ok then return nil, set_error end
    return true, visible
  end

  -- One question each, answered from the current mask: true or false, or
  -- nil plus a message when the mask itself could not be read. Each is one
  -- core.get_context call, so ask once per frame at most.
  local function has(flag, where)
    local current, current_error = context.current()
    if current == nil then return nil, where .. ": " .. current_error end
    return (current & flag) ~= 0
  end

  function context.is_hangar()
    return has(context.HANGAR, "context.is_hangar")
  end
  function context.is_battle()
    return has(context.BATTLE, "context.is_battle")
  end
  function context.is_training()
    return has(context.TRAINING, "context.is_training")
  end
  function context.is_replay()
    return has(context.REPLAY, "context.is_replay")
  end
  function context.is_text_input()
    return has(context.TEXT_INPUT, "context.is_text_input")
  end
  function context.is_mod_screen()
    return has(context.MOD_SCREEN, "context.is_mod_screen")
  end

  wotb.context = context
end
)lua";

// The one place this host enters a Lua VM to load a built-in library. Every
// prelude - the two above and the five in lua_preludes.cpp - comes through
// here, which is what makes the two fixes below worth making once.
//
// F6: this used to call lua_pcall directly. Commit 4cc364d's Directive is
// "keep every Lua VM entry behind LuaScript::ProtectedCall", and a bare
// lua_pcall is not behind it: no instruction budget applies, so an infinite
// loop at the top level of a prelude would hang the game with no way out. The
// call now goes through script->ProtectedCall, which installs the count hook
// exactly as it does for the script's own chunk. RegisterAll runs inside
// LuaScript::Create's `Entry entry(*script)` scope, so the script lock is
// held and protected_call_depth_ is zero - this is a legitimate outermost
// protected call, not a nested one, and the budget it installs is removed
// again before the script's own body runs.
//
// The one written-down exemption: `script` may be null. RegisterAll's
// contract allows it (RegisterStorage and RegisterUi both no-op on a null
// script rather than refusing to be called), and ProtectedCall is a member
// function - there is no script to call it on. That path keeps the bare
// lua_pcall, and it is safe for the reason the Directive exists to guarantee
// elsewhere: with no script there is also no ownership ledger, no event
// subscription and no way for the loaded chunk to be re-entered, so the only
// code that can run is the prelude source in this binary, which contains no
// loops at load time by construction (see lua_preludes.h).
//
// F6's second half: a failure used to be *silent* whenever core_api->log was
// null - a built-in library could fail to load and nothing anywhere would say
// so. OutputDebugStringA is the fallback, in the same format and on the same
// channel print() falls back to (lua_script.cpp SandboxedPrint).
void RegisterLuaLibrary(lua_State* state, const char* source,
                        size_t source_size, const char* chunk_name,
                        const WotbModV3CoreApiV1* core_api,
                        WotbModV3Handle mod, LuaScript* script) {
    if (!state || !source || source_size == 0u) return;
    const int top = lua_gettop(state);
    const char* detail = nullptr;
    int status = luaL_loadbufferx(
        state, source, source_size, chunk_name, "t");
    if (status == LUA_OK) {
        if (script) {
            const LuaProtectedCallResult result =
                script->ProtectedCall(state, 0, 0);
            if (result == LuaProtectedCallResult::kInstructionLimit) {
                status = LUA_ERRRUN;
                // The error object ProtectedCall leaves behind on this path is
                // a light userdata sentinel, not a string, so the message has
                // to come from the host's own stable diagnostic.
                detail = InstructionBudgetError();
            } else if (result != LuaProtectedCallResult::kOk) {
                status = LUA_ERRRUN;
            }
        } else {
            status = lua_pcall(state, 0, 0, 0);
        }
    }
    if (status != LUA_OK) {
        if (!detail) {
            detail = lua_type(state, -1) == LUA_TSTRING
                ? lua_tostring(state, -1)
                : "unknown Lua error";
            if (!detail) detail = "unknown Lua error";
        }
        char line[768] = {};
        std::snprintf(line, sizeof(line),
                      "built-in Lua library %s failed: %s",
                      chunk_name ? chunk_name : "<unnamed>", detail);
        if (core_api && core_api->log) {
            core_api->log(mod, WOTBMOD_V3_LOG_ERROR, "lua", line);
        } else {
            char fallback[832] = {};
            std::snprintf(fallback, sizeof(fallback), "[wotbmod.lua] %s\n",
                          line);
            OutputDebugStringA(fallback);
        }
    }
    lua_settop(state, top);
}

void RegisterPlayersLibrary(lua_State* state,
                            const WotbModV3CoreApiV1* core_api,
                            WotbModV3Handle mod, LuaScript* script) {
    RegisterLuaLibrary(state, kPlayersLibrary, sizeof(kPlayersLibrary) - 1u,
                       "@wotb.players", core_api, mod, script);
}

void RegisterContextLibrary(lua_State* state,
                            const WotbModV3CoreApiV1* core_api,
                            WotbModV3Handle mod, LuaScript* script) {
    RegisterLuaLibrary(state, kContextLibrary, sizeof(kContextLibrary) - 1u,
                       "@wotb.context", core_api, mod, script);
}

// ---------------------------------------------------------------------------
// wotb.mod - the C half: what a script cannot learn about itself from Lua
// ---------------------------------------------------------------------------
//
// id() is the script id LuaScript::Create was given - the same string print()
// carries as an upvalue - and permissions()/has_permission() read the
// script's effective ScriptPermissions, which is already the manifest's
// request intersected with the measured host ceiling, so "may this script
// call it" is one AllowsBit. No permission of its own: a script may always
// ask what it is. The Lua half (capability, info, on_disable) is the prelude
// kModLibrary, which extends this table rather than replacing it.

int ModId(lua_State* state) {
    lua_pushvalue(state, lua_upvalueindex(1));
    if (lua_isnil(state, -1)) {
        lua_pushliteral(state, "mod.id: this state was built without a script identity");
        return 2;
    }
    return 1;
}

const ScriptPermissions* ModPermissions(lua_State* state) {
    return static_cast<const ScriptPermissions*>(
        lua_touserdata(state, lua_upvalueindex(1)));
}

int ModHasPermission(lua_State* state) {
    const char* name = nullptr;
    if (!CheckArgString(state, 1, &name)) return 2;
    const ScriptPermissions* permissions = ModPermissions(state);
    lua_pushboolean(state, permissions != nullptr && permissions->Allows(name));
    return 1;
}

// The names this script holds, in the host's vocabulary order. A fresh table
// per call: the answer is a value the script may keep or mutate.
int ModPermissionList(lua_State* state) {
    const ScriptPermissions* permissions = ModPermissions(state);
    const size_t count = KnownPermissionCount();
    lua_createtable(state, static_cast<int>(count), 0);
    lua_Integer next = 0;
    for (size_t index = 0u; index < count; ++index) {
        if (!permissions || !permissions->AllowsBit(KnownPermissionBit(index))) {
            continue;
        }
        lua_pushstring(state, KnownPermissionName(index));
        lua_rawseti(state, -2, ++next);
    }
    return 1;
}


// The convenience modules added on top of players/context. Their sources
// live in lua_preludes.cpp; the loader stays here so that RegisterLuaLibrary
// above remains the single VM entry point the F6 fix guards.
void RegisterConveniencePreludes(lua_State* state,
                                 const WotbModV3CoreApiV1* core_api,
                                 WotbModV3Handle mod, LuaScript* script) {
    size_t count = 0u;
    const LuaPreludeSource* preludes = ConveniencePreludes(&count);
    for (size_t index = 0u; index < count; ++index) {
        const LuaPreludeSource& prelude = preludes[index];
        RegisterLuaLibrary(state, prelude.source, prelude.size, prelude.name,
                           core_api, mod, script);
    }
}

}  // namespace

void RegisterModLibrary(lua_State* state, const char* id,
                        const ScriptPermissions* permissions) {
    PushWotbTable(state);                                   // [wotb]
    lua_createtable(state, 0, 8);                           // [wotb, mod]
    if (id) {
        lua_pushstring(state, id);
    } else {
        lua_pushnil(state);
    }
    lua_pushcclosure(state, &ModId, 1);
    lua_setfield(state, -2, "id");
    lua_pushlightuserdata(state, const_cast<ScriptPermissions*>(permissions));
    lua_pushcclosure(state, &ModHasPermission, 1);
    lua_setfield(state, -2, "has_permission");
    lua_pushlightuserdata(state, const_cast<ScriptPermissions*>(permissions));
    lua_pushcclosure(state, &ModPermissionList, 1);
    lua_setfield(state, -2, "permissions");
    lua_setfield(state, -2, "mod");                         // [wotb]
    lua_pop(state, 1);                                      // []
}

const WotbModV3CoreApiV1* QueryCoreApi(const WotbModV3Bootstrap* bootstrap,
                                       WotbModV3Handle mod) {
    return static_cast<const WotbModV3CoreApiV1*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_CORE,
                             WOTBMOD_V3_CORE_VERSION));
}

void PushWotbTable(lua_State* state) {
    lua_getglobal(state, "wotb");                     // [wotb?]
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);                             // []
        lua_newtable(state);                           // [wotb]
        lua_pushvalue(state, -1);                       // [wotb, wotb]
        lua_setglobal(state, "wotb");                   // [wotb]
    }
}

void RegisterAll(lua_State* state, const WotbModV3Bootstrap* bootstrap,
                 WotbModV3Handle mod, LuaScript* script,
                 const WotbModV3CoreApiV1** out_core_api) {
    const auto* core_api = static_cast<const WotbModV3CoreApiV1*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_CORE,
                             WOTBMOD_V3_CORE_VERSION));
    if (out_core_api) *out_core_api = core_api;

    const auto* storage_api = static_cast<const WotbModV3StorageApiV1*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_STORAGE,
                             WOTBMOD_V3_STORAGE_VERSION));
    const auto* events_api = static_cast<const WotbModV3EventsApiV1*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_EVENTS,
                             WOTBMOD_V3_EVENTS_VERSION));
    const auto* ges_api = static_cast<const WotbModV3GesApiV1*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_GES,
                             WOTBMOD_V3_GES_VERSION));
    const auto* ui_api = static_cast<const WotbModV3UiApiV2*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_UI,
                             WOTBMOD_V3_UI_VERSION));
    const auto* handles_api = static_cast<const WotbModV3HandlesApiV1*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_HANDLES,
                             WOTBMOD_V3_HANDLES_VERSION));
    const auto* vfs_api = static_cast<const WotbModV3VfsApiV1*>(
        QueryInterfaceOrNull(bootstrap, mod, WOTBMOD_V3_IFACE_VFS,
                             WOTBMOD_V3_VFS_VERSION));
    const DavaLoaderBridge& dava_bridge = LoaderDavaBridge();

    // Armed before a single slot is installed, and therefore before any script
    // code can run: a binding that records a resource into an unbound registry
    // would record something nothing knows how to revoke. Every query above
    // has to happen first for that reason - this is why the three are queried
    // together here rather than each immediately before its own Register*
    // call, which is how this function used to read.
    if (script) {
        script->Ownership().Bind(
            events_api, ui_api, storage_api, handles_api,
            dava_bridge.release, dava_bridge.release_async, mod);
    }

    RegisterStorage(state, storage_api, mod, script);
    RegisterEvents(state, events_api, core_api, mod, script);
    RegisterGes(state, ges_api, events_api, core_api, mod, script);
    RegisterUi(state, ui_api, mod, script);
    RegisterPackages(state, core_api, mod, script);
    RegisterGeneratedBindings(state, bootstrap, mod, script);
    RegisterInputExtensions(state, script);
    RegisterDavaExtensions(state, script, vfs_api, mod);

    // Generated constants after the three hand-written interfaces, not before,
    // and this line was written the other way round first. The instruction
    // was "install them first so a reviewed hand-written spelling of the same
    // name wins", which is the right *goal* and the wrong *mechanism*, because
    // the three hand-written Register* functions do not merge into wotb.<name>
    // - they build a fresh table with lua_newtable and assign it wholesale
    // (lua_bind_storage.cpp:351/378, lua_bind_events.cpp:1375/1401,
    // lua_bind_ui.cpp:997/1020). Anything installed on wotb.storage,
    // wotb.events or wotb.ui before them is therefore discarded without a
    // word. wotb.storage.PATH_* would vanish outright: all 14 storage slots
    // are hand-written, so this function is the only thing that ever puts a
    // constant on that table.
    //
    // The goal is met a different way, and by the generator rather than by
    // this ordering: a constant that a reviewed hand-written surface already
    // publishes is *excluded* from the generated set - WOTBMOD_V3_CONTEXT_*
    // is on that list precisely because wotb.context already spells it - and
    // the model test checks the exclusion list and the no-duplicate-name rule
    // (tests/test_lua_api_model.py). OpenConstantTable merges rather than
    // replacing, which is what makes running last safe.
    //
    // Before the preludes, so a prelude could read a constant at load time if
    // one ever needed to. None do today; see lua_preludes.h.
    RegisterGeneratedConstants(state);

    // Preludes last. Every one of them resolves what it stands on at *call*
    // time rather than at load time - the C tables (wotb.core, wotb.events,
    // wotb.storage, wotb.entity_public) and the three prelude-to-prelude
    // dependencies alike, since wotb.config reads wotb.json, wotb.timer reads
    // wotb.log and wotb.panel reads wotb.context only from inside a call. So
    // relative order among these nine - the two below plus the seven
    // ConveniencePreludes returns - is not load-bearing and none of them would
    // break if it changed; what is load-bearing is that a prelude assigns
    // wotb.<name> and a hand-written Register* assigns wotb.<name> wholesale,
    // so a prelude running before its C neighbour would have its table
    // discarded. Last is the position where that cannot happen.
    RegisterContextLibrary(state, core_api, mod, script);
    RegisterPlayersLibrary(state, core_api, mod, script);
    // The C half of wotb.mod before the preludes, because one of them
    // (kModLibrary) extends this table and must find it rather than make it.
    RegisterModLibrary(state, script ? script->Id().c_str() : nullptr,
                       script ? &script->Permissions() : nullptr);
    RegisterConveniencePreludes(state, core_api, mod, script);
}

size_t InputCursorOwnerCount() {
    AcquireSRWLockShared(&g_inputStateLock);
    const size_t count = CursorOwnerCountLocked();
    ReleaseSRWLockShared(&g_inputStateLock);
    return count;
}

void ReleaseScriptBindings(LuaScript* script) {
    // One line per interface that can still be holding a reference to this
    // script when it is destroyed. Storage holds none — its closures capture
    // the interface pointer and the mod handle, both of which outlive every
    // script — so events is the only entry here, and the only one there can
    // be until an interface with a callback of its own is bound.
    //
    // What this is *not*: the place a script's resources are released. An open
    // transaction and a live control are not references to this script, and
    // taking them back does not need this script to exist; they belong to the
    // ownership registry, which calls this as its own first step. See
    // lua_ownership.h.
    ReleaseInputOwnership(script);
    ReleaseGeneratedCallbacks(script);
    ReleaseEventSubscriptions(script);
}

}  // namespace lua
}  // namespace wotbmod
