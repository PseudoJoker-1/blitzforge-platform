#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <new>
#include <string>
#include <sstream>
#include <vector>

#include "v3_native_bindings.h"
#include "v3_hook_observers.h"
namespace hobs = wotbmod::loader::observers;
#include "v3_camera_effects.h"
#include "v3_native_camera_layout.h"
#include "v3_native_camera_state.h"
#include "v3_native_client_services.h"
#include "v3_managed_renderer.h"
#include "../include/wotbmod/camera_v1.h"
#include "../include/wotbmod/bigworld_rpc_v1.h"
#include "../include/wotbmod/client_v1.h"
#include "../include/wotbmod/entity_public_v1.h"
#include "../include/wotbmod/input_v1.h"
#include "../include/wotbmod/projectile_v2.h"
#include "../include/wotbmod/render_v1.h"
#include "../src/v3/client_services_backend.h"
#include "../src/v3/data_services_backend.h"
#include "../../proxy_dll/third_party/minhook/include/MinHook.h"
#include "anchor_rvas.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

/*
 * The runtime's MAIN async ingress, declared here rather than by including
 * src/v3/wotb_mod_v3_internal.h: that header is the runtime's private
 * interface and the loader is the only host allowed to drive these two, so
 * one narrow declaration is a smaller coupling than pulling the whole header
 * into the loader. Signature drift is caught by the linker rather than
 * silently accepted, because C++ mangles the parameter types into the symbol.
 *
 * This must stay OUTSIDE the anonymous namespace below. Declaring it inside
 * would create `(anonymous)::wotbmod`, which shadows the real `::wotbmod` for
 * the rest of the file and makes every existing wotbmod::v3:: reference
 * ambiguous - the whole translation unit stops compiling.
 *
 * Both functions are documented at wotb_mod_v3_internal.h:208-214: MAIN
 * ingress stays OFF until a per-frame main-thread hook exists and declares
 * itself online, and while it is off dispatch_to_main_thread refuses honestly
 * instead of queueing work that would never run. That hook did not exist
 * until now - re_anchors.md carried it as an open TODO - and this file is it.
 */
namespace wotbmod {
namespace v3 {
void SetMainIngressOnline(bool online);
uint32_t PumpMainThread(uint32_t max_callbacks);
WotbModV3Result InvokeMainThreadBlocking(
    WotbModDavaMainThreadCallFn callback,
    void* user_data,
    uint32_t timeout_ms);
}  // namespace v3
}  // namespace wotbmod

namespace {

/* Filled by WotbModV3NativeBindings_SetGesProvider; empty until the loader
 * has something to offer, which leaves the ges group absent. */
struct GesProvider {
    wotbmod::v3::ClientHostGesListTypesFn list_types;
    wotbmod::v3::ClientHostGesObserveFn observe;
    wotbmod::v3::ClientHostGesPublishFn publish;
};
GesProvider g_ges_provider = {};

/* Filled by WotbModV3NativeBindings_SetSessionClusterProvider. */
struct SessionClusterProvider {
    wotbmod::v3::ClientHostSessionClusterEnumerateFn enumerate;
    wotbmod::v3::ClientHostSessionClusterGetCurrentFn get_current;
    wotbmod::v3::ClientHostSessionClusterChangeFn change;
    wotbmod::v3::ClientHostSessionClusterSetManualFn set_manual;
};
SessionClusterProvider g_session_cluster_provider = {};

}  // namespace

extern "C" void WOTBMOD_CALL WotbModV3NativeBindings_SetSessionClusterProvider(
    WotbModV3Result (*enumerate)(void*, WotbModV3ClusterInfo*, uint32_t*),
    WotbModV3Result (*get_current)(void*, WotbModV3ClusterInfo*),
    WotbModV3Result (*change)(void*, int32_t),
    WotbModV3Result (*set_manual)(void*, uint32_t)) {
    g_session_cluster_provider.enumerate = enumerate;
    g_session_cluster_provider.get_current = get_current;
    g_session_cluster_provider.change = change;
    g_session_cluster_provider.set_manual = set_manual;
}

namespace {
}  // namespace

extern "C" void WOTBMOD_CALL WotbModV3NativeBindings_SetGesProvider(
    WotbModV3Result (*list_types)(void*, WotbModV3NativeGesVisitFn, void*),
    WotbModV3Result (*observe)(void*, const char*, uint32_t),
    WotbModV3Result (*publish)(void*, const char*, const void*, uint32_t, uint32_t)) {
    g_ges_provider.list_types = list_types;
    g_ges_provider.observe = observe;
    g_ges_provider.publish = publish;
}

namespace {

using wotbmod::v3::ClientHostObjectRequest;
using wotbmod::v3::ClientHostObjectResponse;

const uint32_t kBindingPackVersion = 112000887u;
const char kClientBuild[] = "11.20.0.887";
const uint32_t kDavaMainThreadCallTimeoutMs = 2000u;

const uint32_t kNativeTextureMagic = 0x3358544Eu; /* NTX3 */
const uint32_t kNativeMaterialMagic = 0x33544D4Eu; /* NMT3 */
const uint32_t kMaxTrackedProjectiles = 512u;
const uint32_t kMaxTrackedEntities = 512u;
const uint32_t kMaxRpcSubscriptions = 1024u;

const uint8_t kExpectedExecutableSha256[32] = {
    0x48u, 0x13u, 0x54u, 0x4Du, 0x3Du, 0x6Bu, 0x9Fu, 0x45u,
    0xA3u, 0x57u, 0xE8u, 0x7Fu, 0x5Au, 0x14u, 0xD0u, 0x65u,
    0xBDu, 0x10u, 0x8Cu, 0x4Au, 0xCDu, 0x32u, 0x4Au, 0xC1u,
    0x50u, 0x6Cu, 0xB6u, 0xD1u, 0xADu, 0x6Fu, 0xF0u, 0xAFu};

struct Vec3Native {
    float x;
    float y;
    float z;
};

struct NativeTexture {
    uint32_t magic = kNativeTextureMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t format = 0u;
};

struct NativeMaterial {
    uint32_t magic = kNativeMaterialMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    bool blend = true;
    bool depth = false;
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    NativeTexture* texture = nullptr;
};

struct ProjectileRecord {
    uint64_t token = 0u;
    void* strategy = nullptr;
    void* owner_tank = nullptr;
    WotbModV3Vec3 origin = {};
    WotbModV3Vec3 direction = {};
    WotbModV3ProjectileHandle public_handle =
        WOTBMOD_V3_INVALID_HANDLE;
    uint32_t public_entity_id = 0u;
    uint32_t secondary_public_entity_id = 0u;
    uint32_t owner_scope = WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
    uint32_t shell_type = 0u;
    uint32_t native_shot_id = 0u;
    uint32_t native_flags = 0u;
    uint32_t stock_shot_code = 0u;
    uint32_t lifecycle_state = WOTBMOD_V3_PROJECTILE_STATE_CREATED;
    uint32_t source = WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT;
    uint64_t valid_fields = 0u;
    uint64_t timestamp_microseconds = 0u;
    WotbModV3Vec3 impact_position = {};
    uint64_t created_tick_milliseconds = 0u;
    bool published = false;
    bool alive = false;
    /*
     * Visible-tracer bookkeeping. tracer_published records that a
     * visible_tracer_created was announced for this projectile, so the removal
     * paths know they owe a matching visible_tracer_destroyed - and know not to
     * announce a retirement for a tracer that was never announced. The geometry
     * is kept because ShowTracer is the only place it is ever known: the record
     * itself carries no origin for a stock shot, so the destroyed event would
     * otherwise have to invent a position.
     */
    bool tracer_published = false;
    WotbModV3Vec3 tracer_position = {};
    WotbModV3Vec3 tracer_direction = {};
    uint32_t tracer_shell_type = 0u;
};

struct EntityRecord {
    uint64_t token = 0u;
    void* entity = nullptr;
    void* game_logic = nullptr;
    uint32_t public_id = 0u;
    WotbModV3EntityHandle public_handle =
        WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3PublicEntitySnapshot snapshot = {};
    bool published = false;
    bool alive = false;
};

struct RpcSubscriptionRecord {
    uint64_t token = 0u;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
};

struct HookSlot {
    void* target = nullptr;
    void* original = nullptr;
};

struct BindingValidationRecord {
    std::string id;
    std::string kind;
    uint32_t rva = 0u;
    bool required = true;
    bool checked = false;
    bool bound = false;
};

struct RendererState {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint64_t frame_index = 0u;
    double delta_seconds = 0.0;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* texture_shader = nullptr;
    ID3D11PixelShader* color_shader = nullptr;
    ID3D11InputLayout* input_layout = nullptr;
    ID3D11Buffer* vertex_buffer = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11BlendState* blend_state = nullptr;
    ID3D11DepthStencilState* depth_off = nullptr;
    ID3D11RasterizerState* rasterizer = nullptr;
    bool initialized = false;
};

struct NativeState {
    HMODULE game_module = nullptr;
    uint8_t* game_base = nullptr;
    size_t image_size = 0u;
    WotbModV3NativeBindingsLog log = nullptr;
    void* log_user_data = nullptr;
    WotbModRuntimeResourceBackend resource = {};
    WotbModRuntimeAudioBackend audio = {};
    WotbModRuntimeSoundBackend sound = {};
    WotbModRuntimeGameplayBackend gameplay = {};
    void* (WOTBMOD_CALL* resource_identity)(void*, void*) = nullptr;
    void* resource_identity_user_data = nullptr;
    WotbModResult (WOTBMOD_CALL* resource_bring_ui_to_front)(
        void*, void*) = nullptr;
    void* resource_bring_ui_to_front_user_data = nullptr;
    WotbModV3NativeAvatarObserved avatar_observed = nullptr;
    void* avatar_observed_user_data = nullptr;
    uint32_t compatibility =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN;
    PVOID volatile camera = nullptr;
    PVOID volatile client_owner = nullptr;
    PVOID volatile tracer_manager = nullptr;
    bool stock_tracer_ready = false;
    /*
     * Visible-tracer producer. tracer_producer_ready means the ShowTracer hook
     * actually installed on this build, which is what the public
     * WOTBMOD_V3_EVENT_SOURCE_VISIBLE_TRACER bit reports; without it no tracer
     * event can ever be published and the subscription must stay closed.
     *
     * self_issued_tracer suppresses attribution while
     * WotbModV3NativeBindings_CreateStockTracer drives ShowTracer itself: a
     * tracer a mod asked us to draw belongs to no projectile, and attributing it
     * to whatever shot happened to be in flight would be a fabrication.
     */
    bool tracer_producer_ready = false;
    volatile LONG self_issued_tracer = 0;
    volatile LONG tracer_attributed = 0;
    volatile LONG tracer_unattributed = 0;
    /*
     * wotbmod.camera.state cache.
     *
     * The CameraController is only ever reachable through a live
     * SwitchState this-pointer: re_anchors.md refutes the old
     * GameScene+0x128 route, and the owner class of the AvatarContext +0x2C
     * slot that publishes it is UNKNOWN. So the detour captures the pointer,
     * the vtable it had at capture time (a liveness witness that costs one
     * dword), the thread that mutated it, and the raw +0x5C value.
     *
     * `camera_animation_state` and `camera_view_mode` are the only things the
     * public slot ever reads. Negative means "not resolved"; the two halves
     * are independent and neither is ever synthesised from the other.
     */
    PVOID volatile camera_controller = nullptr;
    PVOID volatile camera_controller_vtable = nullptr;
    volatile LONG camera_controller_thread = 0;
    volatile LONG camera_animation_state = -1;
    volatile LONG camera_view_mode = -1;
    /* Proven main thread, published by the UpdateAndDrawWindows detour. */
    volatile LONG main_thread_id = 0;
    volatile LONG64 main_frame_index = 0;
    volatile LONG camera_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
    volatile LONG64 next_projectile_token = 0;
    volatile LONG64 next_entity_token = 0;
    volatile LONG64 next_camera_modifier_token = 0;
    volatile LONG64 next_rpc_subscription_token = 0;
    volatile LONG64 next_rpc_sequence = 0;
    volatile LONG next_stock_tracer_id = 0;
    volatile LONG gameplay_bridge_sources = 0;
    volatile LONG installed_source_mask = 0;
    ProjectileRecord projectiles[kMaxTrackedProjectiles] = {};
    EntityRecord entities[kMaxTrackedEntities] = {};
    SRWLOCK projectile_lock = SRWLOCK_INIT;
    SRWLOCK entity_lock = SRWLOCK_INIT;
    HookSlot hooks[12] = {};
    uint32_t hook_count = 0u;
    std::mutex render_mutex;
    RendererState renderer;
    std::mutex object_mutex;
    std::vector<NativeTexture*> textures;
    std::vector<NativeMaterial*> materials;
    std::mutex rpc_mutex;
    std::vector<RpcSubscriptionRecord> rpc_subscriptions;
    std::mutex input_window_mutex;
    HWND input_window = nullptr;
    WNDPROC original_window_proc = nullptr;
    LONG last_mouse_x = 0;
    LONG last_mouse_y = 0;
    bool has_last_mouse_position = false;
    bool ui_pointer_captured = false;
    float ui_pointer_x = 0.0f;
    float ui_pointer_y = 0.0f;
    float ui_pointer_scale = 1.0f;
    wotbmod::loader::CameraEffectStore camera_effects;
    bool reviewed_hook_symbols_ready = false;
    std::string binding_report_path;
    std::string executable_sha256;
    std::vector<BindingValidationRecord> binding_validation;
    bool created = false;
};

NativeState g_state;

typedef void*(__thiscall* GameCameraCtorFn)(
    void*, uint8_t, uint32_t, uint32_t);
typedef void*(__thiscall* GameCameraDtorFn)(void*, uint8_t);
typedef void(__thiscall* ClientInitializeFn)(void*);
typedef void(__thiscall* LeaveToHangarFn)(void*, void*);
typedef void*(__thiscall* BwEntityCtorFn)(void*, int32_t);
typedef void(__thiscall* BwEntityDtorFn)(void*);
typedef void*(__thiscall* TracerManagerCtorFn)(void*, void*, void*);
typedef void*(__thiscall* TracerManagerDtorFn)(void*, uint32_t);
typedef void(__thiscall* TracerManagerShowTracerFn)(
    void*,
    const float*, const uint32_t*,
    const float*, const uint32_t*,
    const uint8_t*, const float*, uint32_t);
typedef int(__thiscall* CameraSetFovFn)(void*, float);
typedef bool(__thiscall* CameraModeChangedFn)(
    void*, const int32_t*);
/*
 * CameraController::SwitchState, __thiscall(int newState), retn 4.
 *
 * Declared as returning a dword, NOT void, and the detour passes the original's
 * eax back untouched. This is not pedantry - typing it void shipped a crash.
 *
 * The function's tail is `mov [esi+5Ch], edi ; call StartNewState ; retn 4`,
 * so whatever StartNewState leaves in eax IS this function's return value. A
 * void detour is free to clobber eax with its own work before returning, and
 * then any caller that consumes the value gets whatever our last store left
 * there. The observed result was an access violation inside game code, minutes
 * after the fact, during scene teardown - never inside the loader, which is
 * what made it look like anything but a calling-convention bug.
 *
 * Proven by bisection against the running client (WOTBMOD_DISABLE_HOOKS), all
 * runs confirmed to reach battle teardown by their DAVA-release line counts:
 * every configuration with this hook installed crashed, every configuration
 * without it completed teardown cleanly, including one with the whole
 * per-frame ingress still active.
 *
 * The general rule this cost us: a detour must preserve the return register
 * unless the function is PROVEN to return nothing. "The disassembly showed no
 * obvious use of eax" is not that proof. Passing the value through is free and
 * makes the detour transparent whether or not the value means anything.
 */
typedef uint32_t(__thiscall* CameraSwitchStateFn)(void*, int32_t);
/*
 * DAVA::Private::EngineBackend::UpdateAndDrawWindows,
 * __thiscall(float frameDelta, bool skipUpdate), retn 8. `skipUpdate` occupies
 * a full stack dword; it is carried as uint32_t and passed back untouched so
 * the detour cannot change the engine's own decision.
 */
/*
 * Return-transparent for the same reason as CameraSwitchStateFn above: the
 * evidence that this returns nothing is "the disassembly showed no obvious use
 * of eax", which is exactly the evidence that turned out to be wrong for
 * SwitchState. Passing eax through costs nothing and removes the whole class.
 */
typedef uint32_t(__thiscall* EngineUpdateAndDrawWindowsFn)(
    void*, float, uint32_t);
/*
 * DAVA::SoundSystem::CreateSoundEvent, vtable slot +0x0C.
 * __thiscall(const FastName* name, const int32_t* group), retn 8. The
 * FastName is ONE dword holding an interned const char*.
 */
typedef void*(__thiscall* SoundCreateEventFn)(
    void*, const void*, const int32_t*);
/* DAVA::FastName::FastName(const char*), __thiscall, one-dword object. */
typedef void*(__thiscall* FastNameCtorFn)(void*, const char*);
/*
 * sub_C474F0: the engine's own SoundEventStub factory. Takes no arguments,
 * does not touch `this`, plain `retn` - so it is __cdecl with zero arguments.
 */
typedef void*(__cdecl* SoundEventStubFactoryFn)(void);

GameCameraCtorFn g_original_camera_ctor = nullptr;
GameCameraDtorFn g_original_camera_dtor = nullptr;
ClientInitializeFn g_original_client_initialize = nullptr;
BwEntityCtorFn g_original_entity_ctor = nullptr;
BwEntityDtorFn g_original_entity_dtor = nullptr;
TracerManagerCtorFn g_original_tracer_ctor = nullptr;
TracerManagerDtorFn g_original_tracer_dtor = nullptr;
TracerManagerShowTracerFn g_original_tracer_show = nullptr;
CameraModeChangedFn g_original_camera_mode_changed = nullptr;
CameraSwitchStateFn g_original_camera_switch_state = nullptr;
EngineUpdateAndDrawWindowsFn g_original_update_and_draw_windows = nullptr;

/*
 * In-flight detour count.
 *
 * MH_RemoveHook returns the trampoline to MinHook's pool, and MinHook's
 * thread freeze only relocates instruction pointers parked at the patch site
 * - it does not know about a thread already executing inside a detour body,
 * which is exactly the thread that is about to call g_original_*. Shutdown
 * therefore disables every hook first (no new entries), waits for this
 * counter to drain, clears the g_original_* pointers, and only then removes
 * the hooks and frees the trampolines.
 *
 * Shutdown is reachable while the game is running - the loader calls it when
 * runtime initialization fails - so this is a live path, not just teardown.
 */
volatile LONG g_detour_active = 0;

struct DetourGuard {
    DetourGuard() { InterlockedIncrement(&g_detour_active); }
    ~DetourGuard() { InterlockedDecrement(&g_detour_active); }
    DetourGuard(const DetourGuard&) = delete;
    DetourGuard& operator=(const DetourGuard&) = delete;
};

void Log(const char* message) {
    if (g_state.log) {
        g_state.log(message ? message : "", g_state.log_user_data);
    }
}

uint32_t CurrentInputModifiers(uint32_t keyboard_code) {
    uint32_t modifiers = WOTBMOD_V3_INPUT_MOD_NONE;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= WOTBMOD_V3_INPUT_MOD_SHIFT;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= WOTBMOD_V3_INPUT_MOD_CONTROL;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= WOTBMOD_V3_INPUT_MOD_ALT;
    }
    if ((GetKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetKeyState(VK_RWIN) & 0x8000) != 0) {
        modifiers |= WOTBMOD_V3_INPUT_MOD_META;
    }
    if (keyboard_code == VK_SHIFT ||
        keyboard_code == VK_LSHIFT ||
        keyboard_code == VK_RSHIFT) {
        modifiers &= ~WOTBMOD_V3_INPUT_MOD_SHIFT;
    } else if (keyboard_code == VK_CONTROL ||
               keyboard_code == VK_LCONTROL ||
               keyboard_code == VK_RCONTROL) {
        modifiers &= ~WOTBMOD_V3_INPUT_MOD_CONTROL;
    } else if (keyboard_code == VK_MENU ||
               keyboard_code == VK_LMENU ||
               keyboard_code == VK_RMENU) {
        modifiers &= ~WOTBMOD_V3_INPUT_MOD_ALT;
    } else if (keyboard_code == VK_LWIN ||
               keyboard_code == VK_RWIN) {
        modifiers &= ~WOTBMOD_V3_INPUT_MOD_META;
    }
    return modifiers;
}

void PublishNativeButton(
    uint32_t device,
    uint32_t code,
    uint32_t modifiers,
    bool down) {
    wotbmod::v3::NotifyNativeInput(
        wotbmod::v3::NATIVE_INPUT_EVENT_BUTTON,
        device,
        code,
        modifiers,
        down ? 1.0f : 0.0f,
        down ? 1u : 0u);
}

void PublishNativeAxis(
    uint32_t code,
    uint32_t modifiers,
    float value) {
    if (value == 0.0f || !std::isfinite(value)) return;
    wotbmod::v3::NotifyNativeInput(
        wotbmod::v3::NATIVE_INPUT_EVENT_AXIS,
        WOTBMOD_V3_INPUT_DEVICE_MOUSE,
        code,
        modifiers,
        value,
        1u);
}

float UiLogicalScaleForWindow(HWND window) {
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    static const GetDpiForWindowFn get_dpi_for_window =
        reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(
            GetModuleHandleW(L"user32.dll"),
            "GetDpiForWindow"));
    const UINT dpi = get_dpi_for_window && window
        ? get_dpi_for_window(window)
        : USER_DEFAULT_SCREEN_DPI;
    return dpi > 0u
        ? static_cast<float>(USER_DEFAULT_SCREEN_DPI) /
              static_cast<float>(dpi)
        : 1.0f;
}

bool UiPointerFromMessage(
    HWND window,
    LPARAM lparam,
    bool screen_coordinates,
    float scale,
    float* out_x,
    float* out_y) {
    if (!window || !out_x || !out_y) return false;
    POINT point = {
        static_cast<short>(LOWORD(lparam)),
        static_cast<short>(HIWORD(lparam))};
    if (screen_coordinates && !ScreenToClient(window, &point)) {
        return false;
    }
    *out_x = static_cast<float>(point.x) * scale;
    *out_y = static_cast<float>(point.y) * scale;
    return std::isfinite(*out_x) && std::isfinite(*out_y);
}

bool ResolveUiCapturePosition(
    HWND window,
    LPARAM lparam,
    bool screen_coordinates,
    float* out_x,
    float* out_y,
    float* out_scale) {
    if (!out_x || !out_y || !out_scale) return false;
    const float logical_scale = UiLogicalScaleForWindow(window);
    if (UiPointerFromMessage(
            window,
            lparam,
            screen_coordinates,
            logical_scale,
            out_x,
            out_y) &&
        wotbmod::v3::ShouldCaptureClientHostUiInput(*out_x, *out_y)) {
        *out_scale = logical_scale;
        return true;
    }
    return false;
}

bool ResolveUiCaptureClientPoint(
    HWND window,
    float raw_x,
    float raw_y,
    float* out_x,
    float* out_y,
    float* out_scale) {
    if (!window || !out_x || !out_y || !out_scale ||
        !std::isfinite(raw_x) || !std::isfinite(raw_y)) {
        return false;
    }
    const float logical_scale = UiLogicalScaleForWindow(window);
    *out_x = raw_x * logical_scale;
    *out_y = raw_y * logical_scale;
    if (wotbmod::v3::ShouldCaptureClientHostUiInput(*out_x, *out_y)) {
        *out_scale = logical_scale;
        return true;
    }
    return false;
}

bool ReadUiClientCursor(HWND window, float* out_x, float* out_y) {
    if (!window || !out_x || !out_y) return false;
    POINT point = {};
    if (!GetCursorPos(&point) || !ScreenToClient(window, &point)) {
        return false;
    }
    *out_x = static_cast<float>(point.x);
    *out_y = static_cast<float>(point.y);
    return true;
}

bool ReadUiPointerCapture(
    float* out_x,
    float* out_y,
    float* out_scale) {
    std::lock_guard<std::mutex> lock(g_state.input_window_mutex);
    if (out_x) *out_x = g_state.ui_pointer_x;
    if (out_y) *out_y = g_state.ui_pointer_y;
    if (out_scale) *out_scale = g_state.ui_pointer_scale;
    return g_state.ui_pointer_captured;
}

void StoreUiPointerCapture(bool captured, float x, float y, float scale) {
    std::lock_guard<std::mutex> lock(g_state.input_window_mutex);
    g_state.ui_pointer_captured = captured;
    g_state.ui_pointer_x = x;
    g_state.ui_pointer_y = y;
    g_state.ui_pointer_scale = scale;
}

void PublishClientUiPointer(
    uint32_t phase,
    float x,
    float y,
    float delta_x = 0.0f,
    float delta_y = 0.0f) {
    WotbModV3NativeBindings_ObserveUiInput(
        phase,
        x,
        y,
        delta_x,
        delta_y,
        CurrentInputModifiers(0u));
}

bool HandleRawClientUiPointer(HWND window, LPARAM lparam) {
    RAWINPUT input = {};
    UINT input_size = sizeof(input);
    const UINT read = GetRawInputData(
        reinterpret_cast<HRAWINPUT>(lparam),
        RID_INPUT,
        &input,
        &input_size,
        sizeof(RAWINPUTHEADER));
    if (read == static_cast<UINT>(-1) ||
        read < sizeof(RAWINPUTHEADER) ||
        input.header.dwType != RIM_TYPEMOUSE) {
        return false;
    }

    float raw_x = 0.0f;
    float raw_y = 0.0f;
    if (!ReadUiClientCursor(window, &raw_x, &raw_y)) return false;

    const USHORT buttons = input.data.mouse.usButtonFlags;
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
    float previous_x = 0.0f;
    float previous_y = 0.0f;
    const bool pointer_captured =
        ReadUiPointerCapture(&previous_x, &previous_y, &scale);

    if ((buttons & RI_MOUSE_LEFT_BUTTON_DOWN) != 0u) {
        const bool ui_capture = ResolveUiCaptureClientPoint(
            window, raw_x, raw_y, &x, &y, &scale);
        if (!ui_capture) return false;
        StoreUiPointerCapture(true, x, y, scale);
        PublishClientUiPointer(1u, x, y);
        return true;
    }

    if ((buttons & RI_MOUSE_LEFT_BUTTON_UP) != 0u) {
        if (!pointer_captured) return false;
        x = raw_x * scale;
        y = raw_y * scale;
        StoreUiPointerCapture(false, x, y, scale);
        PublishClientUiPointer(3u, x, y);
        return true;
    }

    if ((buttons & RI_MOUSE_WHEEL) != 0u) {
        if (!pointer_captured &&
            !ResolveUiCaptureClientPoint(
                window, raw_x, raw_y, &x, &y, &scale)) {
            return false;
        }
        if (pointer_captured) {
            x = raw_x * scale;
            y = raw_y * scale;
        }
        const float wheel_delta = static_cast<float>(
            static_cast<short>(input.data.mouse.usButtonData)) /
            static_cast<float>(WHEEL_DELTA);
        PublishClientUiPointer(5u, x, y, 0.0f, wheel_delta);
        return true;
    }

    if (pointer_captured) {
        x = raw_x * scale;
        y = raw_y * scale;
        StoreUiPointerCapture(true, x, y, scale);
        PublishClientUiPointer(
            2u, x, y, x - previous_x, y - previous_y);
        return true;
    }

    if (input.data.mouse.lLastX != 0 || input.data.mouse.lLastY != 0) {
        if (!ResolveUiCaptureClientPoint(
                window, raw_x, raw_y, &x, &y, &scale)) {
            return false;
        }
        PublishClientUiPointer(
            4u,
            x,
            y,
            static_cast<float>(input.data.mouse.lLastX) * scale,
            static_cast<float>(input.data.mouse.lLastY) * scale);
        return true;
    }
    return false;
}

LRESULT CALLBACK NativeInputWindowProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam) {
    switch (message) {
        case WM_INPUT:
            if (HandleRawClientUiPointer(window, lparam)) {
                // Foreground WM_INPUT requires DefWindowProc cleanup even
                // when the game handler is intentionally bypassed.
                return DefWindowProcA(window, message, wparam, lparam);
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            const uint32_t code = static_cast<uint32_t>(wparam);
            const bool down = message == WM_KEYDOWN ||
                              message == WM_SYSKEYDOWN;
            PublishNativeButton(
                WOTBMOD_V3_INPUT_DEVICE_KEYBOARD,
                code,
                CurrentInputModifiers(code),
                down);
            break;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP: {
            const bool down = message == WM_LBUTTONDOWN;
            PublishNativeButton(
                WOTBMOD_V3_INPUT_DEVICE_MOUSE,
                WOTBMOD_V3_INPUT_MOUSE_LEFT,
                CurrentInputModifiers(0u),
                down);
            float x = 0.0f;
            float y = 0.0f;
            if (down) {
                float scale = 1.0f;
                const bool ui_capture = ResolveUiCapturePosition(
                        window,
                        lparam,
                        false,
                        &x,
                        &y,
                        &scale);
                if (!ui_capture) {
                    break;
                }
                StoreUiPointerCapture(true, x, y, scale);
                PublishClientUiPointer(1u, x, y);
                return 0;
            }
            float scale = 1.0f;
            const bool ui_capture =
                ReadUiPointerCapture(nullptr, nullptr, &scale);
            if (ui_capture &&
                UiPointerFromMessage(
                    window, lparam, false, scale, &x, &y)) {
                StoreUiPointerCapture(false, x, y, scale);
                PublishClientUiPointer(3u, x, y);
                if (GetCapture() == window) ReleaseCapture();
                return 0;
            }
            break;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            PublishNativeButton(
                WOTBMOD_V3_INPUT_DEVICE_MOUSE,
                WOTBMOD_V3_INPUT_MOUSE_RIGHT,
                CurrentInputModifiers(0u),
                message == WM_RBUTTONDOWN);
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            PublishNativeButton(
                WOTBMOD_V3_INPUT_DEVICE_MOUSE,
                WOTBMOD_V3_INPUT_MOUSE_MIDDLE,
                CurrentInputModifiers(0u),
                message == WM_MBUTTONDOWN);
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            const uint32_t code = HIWORD(wparam) == XBUTTON1
                ? WOTBMOD_V3_INPUT_MOUSE_X1
                : WOTBMOD_V3_INPUT_MOUSE_X2;
            PublishNativeButton(
                WOTBMOD_V3_INPUT_DEVICE_MOUSE,
                code,
                CurrentInputModifiers(0u),
                message == WM_XBUTTONDOWN);
            break;
        }
        case WM_MOUSEWHEEL: {
            const float wheel_delta = static_cast<float>(
                static_cast<short>(HIWORD(wparam))) /
                static_cast<float>(WHEEL_DELTA);
            PublishNativeAxis(
                WOTBMOD_V3_INPUT_MOUSE_WHEEL,
                CurrentInputModifiers(0u),
                wheel_delta);
            float x = 0.0f;
            float y = 0.0f;
            float scale = 1.0f;
            if (ResolveUiCapturePosition(
                    window, lparam, true, &x, &y, &scale)) {
                PublishClientUiPointer(5u, x, y, 0.0f, wheel_delta);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            const LONG x = static_cast<short>(LOWORD(lparam));
            const LONG y = static_cast<short>(HIWORD(lparam));
            LONG dx = 0;
            LONG dy = 0;
            {
                std::lock_guard<std::mutex> lock(
                    g_state.input_window_mutex);
                if (g_state.has_last_mouse_position) {
                    dx = x - g_state.last_mouse_x;
                    dy = y - g_state.last_mouse_y;
                }
                g_state.last_mouse_x = x;
                g_state.last_mouse_y = y;
                g_state.has_last_mouse_position = true;
            }
            const uint32_t modifiers = CurrentInputModifiers(0u);
            PublishNativeAxis(
                WOTBMOD_V3_INPUT_MOUSE_MOVE_X,
                modifiers,
                static_cast<float>(dx));
            PublishNativeAxis(
                WOTBMOD_V3_INPUT_MOUSE_MOVE_Y,
                modifiers,
                static_cast<float>(dy));
            float ui_x = 0.0f;
            float ui_y = 0.0f;
            float scale = 1.0f;
            const bool pointer_captured =
                ReadUiPointerCapture(nullptr, nullptr, &scale);
            const bool resolved = pointer_captured
                ? UiPointerFromMessage(
                      window,
                      lparam,
                      false,
                      scale,
                      &ui_x,
                      &ui_y)
                : ResolveUiCapturePosition(
                      window,
                      lparam,
                      false,
                      &ui_x,
                      &ui_y,
                      &scale);
            if (resolved) {
                if (pointer_captured) {
                    StoreUiPointerCapture(true, ui_x, ui_y, scale);
                }
                    PublishClientUiPointer(
                        pointer_captured ? 2u : 4u,
                        ui_x,
                        ui_y,
                        static_cast<float>(dx) * scale,
                        static_cast<float>(dy) * scale);
                    return 0;
            }
            break;
        }
        case WM_ACTIVATEAPP:
            if (wparam == FALSE) {
                wotbmod::v3::ResetNativeInputState();
                float x = 0.0f;
                float y = 0.0f;
                float scale = 1.0f;
                const bool pointer_captured =
                    ReadUiPointerCapture(&x, &y, &scale);
                {
                    std::lock_guard<std::mutex> lock(
                        g_state.input_window_mutex);
                    g_state.has_last_mouse_position = false;
                    g_state.ui_pointer_captured = false;
                }
                if (pointer_captured) {
                    PublishClientUiPointer(6u, x, y);
                }
            }
            break;
        case WM_CAPTURECHANGED: {
            float x = 0.0f;
            float y = 0.0f;
            float scale = 1.0f;
            const bool pointer_captured =
                ReadUiPointerCapture(&x, &y, &scale);
            if (pointer_captured &&
                reinterpret_cast<HWND>(lparam) != window) {
                StoreUiPointerCapture(false, x, y, scale);
                PublishClientUiPointer(6u, x, y);
            }
            break;
        }
        default:
            break;
    }

    WNDPROC original = nullptr;
    {
        std::lock_guard<std::mutex> lock(
            g_state.input_window_mutex);
        original = g_state.original_window_proc;
    }
    return original
        ? CallWindowProcA(original, window, message, wparam, lparam)
        : DefWindowProcA(window, message, wparam, lparam);
}

void RestoreNativeInputWindow() {
    std::lock_guard<std::mutex> lock(g_state.input_window_mutex);
    if (g_state.input_window && g_state.original_window_proc &&
        IsWindow(g_state.input_window)) {
        const WNDPROC current = reinterpret_cast<WNDPROC>(
            GetWindowLongPtrA(g_state.input_window, GWLP_WNDPROC));
        if (current == &NativeInputWindowProc) {
            SetWindowLongPtrA(
                g_state.input_window,
                GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(
                    g_state.original_window_proc));
        }
    }
    g_state.input_window = nullptr;
    g_state.original_window_proc = nullptr;
    g_state.has_last_mouse_position = false;
    g_state.ui_pointer_captured = false;
    g_state.ui_pointer_x = 0.0f;
    g_state.ui_pointer_y = 0.0f;
    g_state.ui_pointer_scale = 1.0f;
    wotbmod::v3::ResetNativeInputState();
}

void PumpCapturedUiPointer(HWND window) {
    float previous_x = 0.0f;
    float previous_y = 0.0f;
    float scale = 1.0f;
    if (!ReadUiPointerCapture(&previous_x, &previous_y, &scale)) {
        return;
    }
    float raw_x = 0.0f;
    float raw_y = 0.0f;
    if (!ReadUiClientCursor(window, &raw_x, &raw_y)) return;
    const float x = raw_x * scale;
    const float y = raw_y * scale;
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0) {
        if (x != previous_x || y != previous_y) {
            StoreUiPointerCapture(true, x, y, scale);
            PublishClientUiPointer(
                2u, x, y, x - previous_x, y - previous_y);
        }
        return;
    }
    StoreUiPointerCapture(false, x, y, scale);
    PublishClientUiPointer(3u, x, y);
}

void UpdateNativeInputWindow(IDXGISwapChain* swap_chain) {
    if (!swap_chain) return;
    DXGI_SWAP_CHAIN_DESC description = {};
    if (FAILED(swap_chain->GetDesc(&description)) ||
        !description.OutputWindow ||
        !IsWindow(description.OutputWindow)) {
        return;
    }
    PumpCapturedUiPointer(description.OutputWindow);
    {
        std::lock_guard<std::mutex> lock(
            g_state.input_window_mutex);
        if (g_state.input_window == description.OutputWindow &&
            g_state.original_window_proc) {
            return;
        }
    }
    RestoreNativeInputWindow();
    std::lock_guard<std::mutex> lock(g_state.input_window_mutex);
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous = SetWindowLongPtrA(
        description.OutputWindow,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&NativeInputWindowProc));
    if (previous == 0 && GetLastError() != ERROR_SUCCESS) {
        Log("native input window hook installation failed");
        return;
    }
    g_state.input_window = description.OutputWindow;
    g_state.original_window_proc =
        reinterpret_cast<WNDPROC>(previous);
    Log("native input ingress attached to the game window");
}

void LogBindingStatus(
    const char* binding_id,
    uint32_t rva,
    bool installed,
    const char* binding_kind,
    bool required = true) {
    BindingValidationRecord record;
    record.id = binding_id ? binding_id : "unknown";
    record.kind = binding_kind ? binding_kind : "unknown";
    record.rva = rva;
    record.required = required;
    record.checked = true;
    record.bound = installed;
    g_state.binding_validation.push_back(std::move(record));
    char message[256] = {};
    sprintf_s(
        message,
        "V3 native binding id=%s kind=%s rva=0x%08X state=%s",
        binding_id ? binding_id : "unknown",
        binding_kind ? binding_kind : "unknown",
        rva,
        installed ? "BOUND" : "FAILED");
    Log(message);
}

void AddUncheckedBinding(
    const char* id,
    const char* kind,
    uint32_t rva,
    bool required = true) {
    BindingValidationRecord record;
    record.id = id ? id : "unknown";
    record.kind = kind ? kind : "unknown";
    record.rva = rva;
    record.required = required;
    g_state.binding_validation.push_back(std::move(record));
}

/*
 * Same validation record and the same log line as LogBindingStatus, but
 * deliberately NOT part of the mod-facing hook-symbol list.
 *
 * That list is what resolve_symbol publishes, i.e. what a mod may aim its own
 * detour at, and none of the anchors routed through here belongs there:
 *
 *   - CameraController::SwitchState is precisely the unsafe camera setter
 *     camera_v2.h refuses to express. It validates nothing and the transition
 *     policy lives at its eight call sites, so handing mods a way to hook it
 *     would give back the exact capability the frozen header removed.
 *   - EngineBackend::UpdateAndDrawWindows is the host's own main-thread
 *     identity proof; a mod detour there could stall or reorder the frame the
 *     scene walk's thread rule depends on.
 *   - the tracer style table is .rdata, not executable at all.
 *   - the SoundEventStub factory is the host's suppression primitive; a mod
 *     hooking it could make every suppressed sound return an object of its
 *     own choosing to engine code that does not null-check.
 *
 * They still appear in binding_pack_validation.json, on both the matching and
 * the mismatched-fingerprint path, because that report is how a support case
 * finds out which parts of the pack bound.
 */
void RecordInternalBinding(
    const char* id,
    const char* kind,
    uint32_t rva,
    bool checked,
    bool bound) {
    BindingValidationRecord record;
    record.id = id ? id : "unknown";
    record.kind = kind ? kind : "unknown";
    record.rva = rva;
    record.required = false;
    record.checked = checked;
    record.bound = bound;
    g_state.binding_validation.push_back(std::move(record));
    if (!checked) return;
    char message[256] = {};
    sprintf_s(
        message,
        "V3 native binding id=%s kind=%s rva=0x%08X state=%s",
        id ? id : "unknown",
        kind ? kind : "unknown",
        rva,
        bound ? "BOUND" : "FAILED");
    Log(message);
}

/* The four declared-interface anchors, in one place so the checked and the
 * unchecked path cannot drift apart. */
void RecordDeclaredInterfaceBindings(
    bool checked,
    bool switch_state,
    bool main_thread_pump,
    bool tracer_table,
    bool sound_stub) {
    RecordInternalBinding(
        "CameraController::SwitchState",
        "hook",
        kCameraControllerSwitchStateRva,
        checked,
        switch_state);
    RecordInternalBinding(
        "EngineBackend::UpdateAndDrawWindows",
        "hook",
        kEngineUpdateAndDrawWindowsRva,
        checked,
        main_thread_pump);
    RecordInternalBinding(
        "TracerManager::ShellStyleTable",
        "identity",
        kTracerShellStyleTableRva,
        checked,
        tracer_table);
    RecordInternalBinding(
        "DAVA::SoundSystem::SoundEventStub",
        "direct-call",
        kSoundEventStubFactoryRva,
        checked,
        sound_stub);
}

void AddAllBindingsUnchecked() {
    AddUncheckedBinding("GameCamera::ctor", "hook", kGameCameraCtorRva);
    AddUncheckedBinding("GameCamera::dtor", "hook", kGameCameraDtorRva);
    AddUncheckedBinding("Client::Initialize", "hook", kClientInitializeRva);
    AddUncheckedBinding("BWEntity::ctor", "hook", kBwEntityCtorRva);
    AddUncheckedBinding("BWEntity::dtor", "hook", kBwEntityDtorRva);
    AddUncheckedBinding("TracerManager::ctor", "hook", kTracerManagerCtorRva);
    AddUncheckedBinding("TracerManager::dtor", "hook", kTracerManagerDtorRva);
    AddUncheckedBinding(
        "TracerManager::ShowTracer",
        "direct-call",
        kTracerManagerShowTracerRva);
    AddUncheckedBinding("CameraModeChanged", "hook", kCameraModeChangedRva);
    AddUncheckedBinding("DAVA::Camera::SetFovY", "direct-call", kGameCameraSetFovRva);
    AddUncheckedBinding("Client::LeaveToHangar", "direct-call", kLeaveToHangarRva);
    AddUncheckedBinding("GameCamera::vtable", "identity", kGameCameraVtableRva, false);
    AddUncheckedBinding("TracerManager::vtable", "identity", kTracerManagerVtableRva, false);
    RecordDeclaredInterfaceBindings(false, false, false, false, false);
}

const char* CompatibilityName(uint32_t compatibility) {
    switch (compatibility) {
        case WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED:
            return "supported";
        case WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED:
            return "degraded";
        case WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH:
            return "hash_mismatch";
        default:
            return "unknown";
    }
}

void WriteBindingValidationReport() {
    if (g_state.binding_report_path.empty()) return;
    uint32_t checked = 0u;
    uint32_t passed = 0u;
    uint32_t mandatory_failed = 0u;
    for (const BindingValidationRecord& record :
         g_state.binding_validation) {
        if (record.checked) ++checked;
        if (record.checked && record.bound) ++passed;
        if (record.required && (!record.checked || !record.bound)) {
            ++mandatory_failed;
        }
    }
    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"binding_pack_version\": " << kBindingPackVersion << ",\n"
         << "  \"client_build\": \"" << kClientBuild << "\",\n"
         << "  \"expected_executable_sha256\": \"4813544d3d6b9f45a357e87f5a14d065bd108c4acd324ac1506cb6d1ad6ff0af\",\n"
         << "  \"actual_executable_sha256\": \""
         << g_state.executable_sha256 << "\",\n"
         << "  \"compatibility\": \""
         << CompatibilityName(g_state.compatibility) << "\",\n"
         << "  \"checked_count\": " << checked << ",\n"
         << "  \"passed_count\": " << passed << ",\n"
         << "  \"mandatory_failed_count\": "
         << mandatory_failed << ",\n"
         << "  \"bindings\": [\n";
    for (size_t index = 0u;
         index < g_state.binding_validation.size(); ++index) {
        const BindingValidationRecord& record =
            g_state.binding_validation[index];
        char rva[16] = {};
        sprintf_s(rva, "0x%08X", record.rva);
        json << "    {\"id\":\"" << record.id
             << "\",\"kind\":\"" << record.kind
             << "\",\"rva\":\"" << rva
             << "\",\"required\":"
             << (record.required ? "true" : "false")
             << ",\"state\":\""
             << (!record.checked ? "not_checked"
                 : record.bound ? "bound" : "failed")
             << "\"}"
             << (index + 1u == g_state.binding_validation.size()
                     ? "\n" : ",\n");
    }
    json << "  ]\n}\n";

    namespace fs = std::filesystem;
    const fs::path report =
        fs::u8path(g_state.binding_report_path);
    std::error_code ec;
    fs::create_directories(report.parent_path(), ec);
    if (ec) {
        Log("V3 binding validation report directory creation failed");
        return;
    }
    const fs::path temporary =
        fs::u8path(g_state.binding_report_path + ".tmp");
    std::ofstream output(
        temporary, std::ios::binary | std::ios::trunc);
    const std::string bytes = json.str();
    output.write(bytes.data(),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output || !MoveFileExW(
            temporary.c_str(), report.c_str(),
            MOVEFILE_REPLACE_EXISTING |
                MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        Log("V3 binding validation report atomic write failed");
        return;
    }
    Log("V3 binding validation report updated");
}

template <typename T>
void ReleaseCom(T*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

bool SafeRead(const void* address, void* output, size_t size) {
    if (!address || !output || size == 0u) return false;
    __try {
        std::memcpy(output, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWrite(void* address, const void* input, size_t size) {
    if (!address || !input || size == 0u) return false;
    __try {
        std::memcpy(address, input, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsExecutableAddress(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsReadableAddress(const void* address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0u) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_READONLY ||
           protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool ResolveImage(HMODULE module, uint8_t** out_base, size_t* out_size) {
    if (!module || !out_base || !out_size) return false;
    uint8_t* base = reinterpret_cast<uint8_t*>(module);
    __try {
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(
                base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.SizeOfImage == 0u) {
            return false;
        }
        *out_base = base;
        *out_size = nt->OptionalHeader.SizeOfImage;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* AddressFromRva(uint32_t rva) {
    if (!g_state.game_base || rva >= g_state.image_size) return nullptr;
    return g_state.game_base + rva;
}

bool MatchesBytes(
    const void* target,
    const uint8_t* expected,
    size_t expected_size) {
    uint8_t actual[32] = {};
    if (!expected || expected_size == 0u ||
        expected_size > sizeof(actual) ||
        !SafeRead(target, actual, expected_size)) {
        return false;
    }
    return std::memcmp(actual, expected, expected_size) == 0;
}

bool HashFileSha256(const char* path, uint8_t output[32]) {
    if (!path || !path[0] || !output) return false;
    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    PUCHAR object = nullptr;
    DWORD object_size = 0u;
    DWORD result_size = 0u;
    bool success = false;
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0u) < 0 ||
        BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&object_size),
            sizeof(object_size),
            &result_size,
            0u) < 0) {
        goto cleanup;
    }
    object = static_cast<PUCHAR>(
        HeapAlloc(GetProcessHeap(), 0u, object_size));
    if (!object ||
        BCryptCreateHash(
            algorithm,
            &hash,
            object,
            object_size,
            nullptr,
            0u,
            0u) < 0) {
        goto cleanup;
    }
    for (;;) {
        uint8_t buffer[64u * 1024u] = {};
        DWORD read = 0u;
        if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) {
            goto cleanup;
        }
        if (read == 0u) break;
        if (BCryptHashData(hash, buffer, read, 0u) < 0) {
            goto cleanup;
        }
    }
    success =
        BCryptFinishHash(hash, output, 32u, 0u) >= 0;

cleanup:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0u);
    if (object) HeapFree(GetProcessHeap(), 0u, object);
    CloseHandle(file);
    return success;
}

/*
 * Per-hook kill switch, read once from WOTBMOD_DISABLE_HOOKS.
 *
 * Two reasons this exists rather than being a debug-only #if. First, bisecting
 * a fault that only appears in a running game is otherwise a rebuild per
 * hypothesis, and a rebuild is minutes -- this turns a day of guessing into
 * three launches. Second, and more importantly, it is a safety valve for a
 * player: every hook here is gated on an exact client fingerprint, but a
 * fingerprint match does not prove a hook is harmless on somebody else's
 * driver, overlay or security software. If one of them turns out to be, the
 * answer should be an environment variable, not "wait for the next build".
 *
 * Value is a comma or semicolon separated list of hook names, matched
 * case-insensitively. "all" disables every name below. Unknown names are
 * ignored deliberately: a typo must not silently disable something else, and
 * a name retired in a later build must not turn into a hard error on a
 * machine whose environment still sets it.
 */
bool HookDisabled(const char* name) {
    static char buffer[512] = {};
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        const DWORD length = GetEnvironmentVariableA(
            "WOTBMOD_DISABLE_HOOKS", buffer, sizeof(buffer));
        if (length == 0u || length >= sizeof(buffer)) buffer[0] = '\0';
    }
    if (buffer[0] == '\0' || !name) return false;
    const size_t name_length = std::strlen(name);
    const char* cursor = buffer;
    while (*cursor != '\0') {
        while (*cursor == ',' || *cursor == ';' || *cursor == ' ') ++cursor;
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != ',' && *cursor != ';' &&
               *cursor != ' ') {
            ++cursor;
        }
        const size_t length = static_cast<size_t>(cursor - start);
        if (length == 3u && _strnicmp(start, "all", 3u) == 0) return true;
        if (length == name_length &&
            _strnicmp(start, name, name_length) == 0) {
            return true;
        }
    }
    return false;
}

bool InstallHook(
    uint32_t rva,
    const uint8_t* expected,
    size_t expected_size,
    void* detour,
    void** original,
    const char* label) {
    if (g_state.hook_count >=
        sizeof(g_state.hooks) / sizeof(g_state.hooks[0])) {
        return false;
    }
    void* target = AddressFromRva(rva);
    if (!MatchesBytes(target, expected, expected_size)) {
        Log(label);
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK ||
        MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        if (original) *original = nullptr;
        Log(label);
        return false;
    }
    HookSlot& slot = g_state.hooks[g_state.hook_count++];
    slot.target = target;
    slot.original = original ? *original : nullptr;
    return true;
}

WotbModV3Vec3 Normalize(WotbModV3Vec3 value) {
    const float length = std::sqrt(
        value.x * value.x + value.y * value.y + value.z * value.z);
    if (!(length > 0.000001f) || !std::isfinite(length)) {
        return {0.0f, 0.0f, 1.0f};
    }
    value.x /= length;
    value.y /= length;
    value.z /= length;
    return value;
}

WotbModV3Vec3 Cross(
    const WotbModV3Vec3& left,
    const WotbModV3Vec3& right) {
    return {
        left.y * right.z - left.z * right.y,
        left.z * right.x - left.x * right.z,
        left.x * right.y - left.y * right.x};
}

float Dot(
    const WotbModV3Vec3& left,
    const WotbModV3Vec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

WotbModV3Vec3 Rotate(
    const WotbModV3Vec4& quaternion,
    const WotbModV3Vec3& vector) {
    const WotbModV3Vec3 q = {
        quaternion.x, quaternion.y, quaternion.z};
    const WotbModV3Vec3 uv = Cross(q, vector);
    const WotbModV3Vec3 uuv = Cross(q, uv);
    return {
        vector.x + 2.0f * (quaternion.w * uv.x + uuv.x),
        vector.y + 2.0f * (quaternion.w * uv.y + uuv.y),
        vector.z + 2.0f * (quaternion.w * uv.z + uuv.z)};
}

WotbModV3Vec4 QuaternionFromBasis(
    WotbModV3Vec3 right,
    WotbModV3Vec3 up,
    WotbModV3Vec3 forward) {
    right = Normalize(right);
    up = Normalize(up);
    forward = Normalize(forward);
    const float m00 = right.x;
    const float m01 = up.x;
    const float m02 = forward.x;
    const float m10 = right.y;
    const float m11 = up.y;
    const float m12 = forward.y;
    const float m20 = right.z;
    const float m21 = up.z;
    const float m22 = forward.z;
    WotbModV3Vec4 result = {};
    const float trace = m00 + m11 + m22;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        result.w = 0.25f * s;
        result.x = (m21 - m12) / s;
        result.y = (m02 - m20) / s;
        result.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        result.w = (m21 - m12) / s;
        result.x = 0.25f * s;
        result.y = (m01 + m10) / s;
        result.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        result.w = (m02 - m20) / s;
        result.x = (m01 + m10) / s;
        result.y = 0.25f * s;
        result.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        result.w = (m10 - m01) / s;
        result.x = (m02 + m20) / s;
        result.y = (m12 + m21) / s;
        result.z = 0.25f * s;
    }
    const float length = std::sqrt(
        result.x * result.x + result.y * result.y +
        result.z * result.z + result.w * result.w);
    if (!(length > 0.000001f)) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    result.x /= length;
    result.y /= length;
    result.z /= length;
    result.w /= length;
    return result;
}

bool ValidateCamera(void* camera) {
    if (!camera) return false;
    void* vtable = nullptr;
    return SafeRead(camera, &vtable, sizeof(vtable)) &&
           vtable == AddressFromRva(kGameCameraVtableRva);
}

bool ReadCameraTransform(
    void* camera,
    WotbModV3Transform* out_transform) {
    if (!ValidateCamera(camera) || !out_transform) return false;
    WotbModV3Vec3 position = {};
    WotbModV3Vec3 target = {};
    WotbModV3Vec3 up = {};
    const uint8_t* bytes = static_cast<const uint8_t*>(camera);
    if (!SafeRead(bytes + 0x38u, &position, sizeof(position)) ||
        !SafeRead(bytes + 0x44u, &target, sizeof(target)) ||
        !SafeRead(bytes + 0x50u, &up, sizeof(up))) {
        return false;
    }
    const WotbModV3Vec3 forward = Normalize(
        {target.x - position.x,
         target.y - position.y,
         target.z - position.z});
    WotbModV3Vec3 right = Normalize(Cross(up, forward));
    up = Normalize(Cross(forward, right));
    out_transform->struct_size = sizeof(*out_transform);
    out_transform->api_version = WOTBMOD_V3_ABI_VERSION;
    out_transform->position = position;
    out_transform->rotation =
        QuaternionFromBasis(right, up, forward);
    out_transform->scale = {1.0f, 1.0f, 1.0f};
    return true;
}

bool WriteCameraTransform(
    void* camera,
    const WotbModV3Transform& transform) {
    if (!ValidateCamera(camera)) return false;
    const WotbModV3Vec3 forward =
        Normalize(Rotate(transform.rotation, {0.0f, 0.0f, 1.0f}));
    const WotbModV3Vec3 up =
        Normalize(Rotate(transform.rotation, {0.0f, 1.0f, 0.0f}));
    uint8_t* bytes = static_cast<uint8_t*>(camera);
    WotbModV3Vec3 current_position = {};
    WotbModV3Vec3 current_target = {};
    if (!SafeRead(bytes + 0x38u, &current_position, sizeof(current_position)) ||
        !SafeRead(bytes + 0x44u, &current_target, sizeof(current_target))) {
        return false;
    }
    const WotbModV3Vec3 target =
        wotbmod::v3::native_camera::TargetWithPreservedDistance(
            transform.position,
            forward,
            current_position,
            current_target);
    uint16_t dirty = 0u;
    if (!SafeRead(bytes + 0x24Cu, &dirty, sizeof(dirty))) {
        return false;
    }
    dirty = static_cast<uint16_t>(dirty | 1u);
    return SafeWrite(
               bytes + 0x38u,
               &transform.position,
               sizeof(transform.position)) &&
           SafeWrite(bytes + 0x44u, &target, sizeof(target)) &&
           SafeWrite(bytes + 0x50u, &up, sizeof(up)) &&
           SafeWrite(bytes + 0x24Cu, &dirty, sizeof(dirty));
}

bool SetCameraFovSafe(void* camera, float degrees) {
    CameraSetFovFn setter =
        reinterpret_cast<CameraSetFovFn>(
            AddressFromRva(kGameCameraSetFovRva));
    if (!camera || !setter ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(setter))) {
        return false;
    }
    __try {
        setter(camera, degrees);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CameraWorldToScreen(
    void* camera,
    const WotbModV3Vec3& world,
    WotbModV3Vec3* out_screen) {
    if (!ValidateCamera(camera) || !out_screen) return false;
    float matrix[16] = {};
    if (!SafeRead(
            static_cast<const uint8_t*>(camera) + 0x140u,
            matrix,
            sizeof(matrix))) {
        return false;
    }
    const float x =
        world.x * matrix[0] + world.y * matrix[4] +
        world.z * matrix[8] + matrix[12];
    const float y =
        world.x * matrix[1] + world.y * matrix[5] +
        world.z * matrix[9] + matrix[13];
    const float z =
        world.x * matrix[2] + world.y * matrix[6] +
        world.z * matrix[10] + matrix[14];
    const float w =
        world.x * matrix[3] + world.y * matrix[7] +
        world.z * matrix[11] + matrix[15];
    if (!std::isfinite(w) || std::fabs(w) < 0.000001f ||
        g_state.renderer.width == 0u ||
        g_state.renderer.height == 0u) {
        return false;
    }
    const float inverse_w = 1.0f / w;
    out_screen->x =
        (x * inverse_w + 1.0f) * 0.5f *
        static_cast<float>(g_state.renderer.width);
    out_screen->y =
        (1.0f - y * inverse_w) * 0.5f *
        static_cast<float>(g_state.renderer.height);
    out_screen->z = z * inverse_w;
    return std::isfinite(out_screen->x) &&
           std::isfinite(out_screen->y) &&
           std::isfinite(out_screen->z);
}

bool CameraScreenToWorld(
    void* camera,
    const WotbModV3Vec3& screen,
    WotbModV3Vec3* out_world) {
    if (!ValidateCamera(camera) || !out_world ||
        g_state.renderer.width == 0u ||
        g_state.renderer.height == 0u) {
        return false;
    }
    float matrix[16] = {};
    if (!SafeRead(
            static_cast<const uint8_t*>(camera) + 0x200u,
            matrix,
            sizeof(matrix))) {
        return false;
    }
    const float x =
        screen.x / static_cast<float>(g_state.renderer.width) *
            2.0f -
        1.0f;
    const float y =
        1.0f -
        screen.y / static_cast<float>(g_state.renderer.height) *
            2.0f;
    const float z = screen.z;
    const float wx =
        x * matrix[0] + y * matrix[4] + z * matrix[8] + matrix[12];
    const float wy =
        x * matrix[1] + y * matrix[5] + z * matrix[9] + matrix[13];
    const float wz =
        x * matrix[2] + y * matrix[6] + z * matrix[10] + matrix[14];
    const float ww =
        x * matrix[3] + y * matrix[7] + z * matrix[11] + matrix[15];
    if (!std::isfinite(ww) || std::fabs(ww) < 0.000001f) {
        return false;
    }
    const float inverse_w = 1.0f / ww;
    *out_world = {wx * inverse_w, wy * inverse_w, wz * inverse_w};
    return std::isfinite(out_world->x) &&
           std::isfinite(out_world->y) &&
           std::isfinite(out_world->z);
}

void* __fastcall CameraCtorDetour(
    void* self,
    void*,
    uint8_t kind,
    uint32_t first,
    uint32_t second) {
    DetourGuard guard;
    void* const camera_target = AddressFromRva(kGameCameraCtorRva);
    if (g_original_camera_ctor) {
        hobs::Before<GameCameraCtorFn>(camera_target, self, kind, first, second);
    }
    void* result = g_original_camera_ctor
        ? g_original_camera_ctor(self, kind, first, second)
        : self;
    if (g_original_camera_ctor) {
        hobs::After<GameCameraCtorFn>(camera_target, self, kind, first, second);
    }
    if (ValidateCamera(result)) {
        InterlockedExchangePointer(&g_state.camera, result);
    }
    return result;
}

void* __fastcall CameraDtorDetour(
    void* self,
    void*,
    uint8_t flags) {
    DetourGuard guard;
    InterlockedCompareExchangePointer(
        &g_state.camera, nullptr, self);
    if (!g_original_camera_dtor) return self;
    void* const dtor_target = AddressFromRva(kGameCameraDtorRva);
    hobs::Before<GameCameraDtorFn>(dtor_target, self, flags);
    void* const result = g_original_camera_dtor(self, flags);
    hobs::After<GameCameraDtorFn>(dtor_target, self, flags);
    return result;
}

void __fastcall ClientInitializeDetour(void* self, void*) {
    DetourGuard guard;
    if (g_original_client_initialize) {
        void* const target = AddressFromRva(kClientInitializeRva);
        hobs::Before<ClientInitializeFn>(target, self);
        g_original_client_initialize(self);
        hobs::After<ClientInitializeFn>(target, self);
    }
    if (self) {
        InterlockedExchangePointer(&g_state.client_owner, self);
    }
}

EntityRecord* FindEntityRecordByPointerLocked(void* entity);

/*
 * CameraModeChanged@Avatar@GES payload field0.
 *
 * CORRECTED 2026-08-16 - this mapping shipped INVERTED and is now derived
 * from the image rather than from the older note:
 *
 *   CameraController::OnCameraModeChanged (0x015B5200) branches on
 *   `cmp dword ptr [arg], 0`. The field0 == 0 fall-through pushes the
 *   {pointer, length} pair at 0x03690224/0x03690228, and 0x03690224 holds
 *   0x0369022C = "STATE_view_play_mode_sniper" with length 27; the non-zero
 *   branch pushes the pair for "STATE_view_play_mode_arcade".
 *   The Avatar-side consumer 0x0156ED10 is sixteen bytes of code that reduce
 *   to `this->+0x60 = (field0 == 0)` - an isSniper flag, not isArcade.
 *   Independently, GameCamera+0x320 == 0 selects
 *   GameCamera::GetPivotForSniperMode at the branch 0x015C460B.
 *
 * So field0 == 0 is SNIPER. Anything other than 0 or 1 is refused rather
 * than folded into one of the two, because a third value would mean this
 * reading is incomplete.
 */
bool NativeCameraModeToPublic(
    int32_t native_mode,
    uint32_t* out_mode) {
    if (!out_mode) return false;
    if (native_mode == 0) {
        *out_mode = WOTBMOD_V3_CAMERA_MODE_SNIPER;
        return true;
    }
    if (native_mode == 1) {
        *out_mode = WOTBMOD_V3_CAMERA_MODE_ARCADE;
        return true;
    }
    return false;
}

bool __fastcall CameraModeChangedDetour(
    void* self,
    void*,
    const int32_t* native_mode_event) {
    DetourGuard guard;
    void* const mode_target = AddressFromRva(kCameraModeChangedRva);
    if (g_original_camera_mode_changed) {
        hobs::Before<CameraModeChangedFn>(mode_target, self, native_mode_event);
    }
    const bool result = g_original_camera_mode_changed
        ? g_original_camera_mode_changed(self, native_mode_event)
        : false;
    if (g_original_camera_mode_changed) {
        hobs::After<CameraModeChangedFn>(mode_target, self, native_mode_event);
    }
    int32_t native_mode = -1;
    uint32_t public_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
    if (!SafeRead(
            native_mode_event,
            &native_mode,
            sizeof(native_mode)) ||
        !NativeCameraModeToPublic(native_mode, &public_mode)) {
        return result;
    }
    /*
     * The declared camera.state view index, published from the event instead
     * of from a cached CameraController pointer. native_mode is the same
     * 2-valued quantity as GameCamera+0x320 and carries the same meaning:
     * 0 is SNIPER, which matches WOTBMOD_V3_CAMERA_VIEW_SNIPER = 0 and
     * WOTBMOD_V3_CAMERA_VIEW_ARCADE = 1. Anything else is refused rather than
     * folded into one of the two, so a third value in a future build reports
     * unavailable instead of silently reading as arcade.
     */
    if (native_mode == 0 || native_mode == 1) {
        InterlockedExchange(
            &g_state.camera_view_mode, static_cast<LONG>(native_mode));
    } else {
        InterlockedExchange(&g_state.camera_view_mode, -1);
    }
    /*
     * Retry on duplicate mode notifications too: in short replays the first
     * camera event can precede Vehicle::onEnterWorld. The callback is private
     * to the loader and performs the fingerprint-gated Avatar resolution.
     */
    if (g_state.created && g_state.avatar_observed) {
        g_state.avatar_observed(
            self, g_state.avatar_observed_user_data);
    }
    const LONG previous = InterlockedExchange(
        &g_state.camera_mode,
        static_cast<LONG>(public_mode));
    if (previous == static_cast<LONG>(public_mode)) {
        return result;
    }

    WotbModRuntimeClientEvent event = {};
    event.struct_size = sizeof(event);
    event.type = WOTBMOD_EVENT_CAMERA_MODE_CHANGED;
    event.resource_type = WOTBMOD_RESOURCE_GENERIC;
    event.payload.camera.previous_mode =
        previous >= WOTBMOD_V3_CAMERA_MODE_UNKNOWN &&
                previous <= WOTBMOD_V3_CAMERA_MODE_CINEMATIC
            ? static_cast<uint32_t>(previous)
            : WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
    event.payload.camera.mode = public_mode;
    event.payload.camera.native_mode = native_mode;
    event.payload.camera.flags = 0u;
    event.payload_size = sizeof(event.payload.camera);
    WotbModRuntime_NotifyClientEvent(&event);
    return result;
}

/* =========================================================================
 * wotbmod.camera.state -- READ ONLY, two independent halves.
 *
 * CameraController+0x5C is written in exactly two places on this build: the
 * constructor (0x01598D90, `mov dword ptr [edi+5Ch], 5`) and the tail of
 * SwitchState (`mov [esi+5Ch], edi ; call StartNewState`). Detouring
 * SwitchState therefore sees every transition AND is the only way to obtain a
 * CameraController pointer at all - the old GameScene+0x128 route is refuted
 * and the owner class of the AvatarContext +0x2C slot is unknown.
 *
 * There is no setter and none may be added. SwitchState validates nothing; the
 * transition policy lives at its eight call sites, so a direct call can tear
 * the camera out of PostMortemAnimationController while that controller still
 * owns the death animation.
 * ========================================================================= */

const uint32_t kCameraControllerStateOffset = 0x5Cu;
const uint32_t kCameraControllerGameCameraOffset = 0x28u;
const uint32_t kGameCameraViewModeOffset = 0x320u;

uint32_t __fastcall CameraSwitchStateDetour(
    void* self,
    void*,
    int32_t new_state) {
    DetourGuard guard;
    uint32_t original_result = 0u;
    if (g_original_camera_switch_state) {
        original_result =
            g_original_camera_switch_state(self, new_state);
    }
    if (!self) return original_result;
    /*
     * Publish the FIELD, not the argument. SwitchState early-returns when the
     * new state equals the current one, and a future build could reject a
     * transition; reading [this+0x5C] back after the original ran reports
     * what the engine actually holds instead of what it was asked for.
     */
    void* vtable = nullptr;
    int32_t state = 0;
    if (!SafeRead(self, &vtable, sizeof(vtable)) ||
        !IsReadableAddress(vtable) ||
        !SafeRead(
            static_cast<uint8_t*>(self) + kCameraControllerStateOffset,
            &state,
            sizeof(state))) {
        return original_result;
    }
    InterlockedExchangePointer(
        &g_state.camera_controller_vtable, vtable);
    InterlockedExchangePointer(&g_state.camera_controller, self);
    InterlockedExchange(
        &g_state.camera_controller_thread,
        static_cast<LONG>(GetCurrentThreadId()));
    /*
     * The raw value, unmapped. camera_v2.h wants "we read it and cannot name
     * it" to stay distinguishable from "we could not read it", and the
     * runtime is what normalises an unnamed value to _UNKNOWN with the
     * validity flag left set.
     */
    InterlockedExchange(
        &g_state.camera_animation_state, static_cast<LONG>(state));
    return original_result;
}

/*
 * The arcade/sniper half is published by the CameraModeChanged detour, NOT by
 * dereferencing a cached CameraController on the frame tick.
 *
 * It used to be the latter, and that shipped a crash. A cached this-pointer
 * was re-read every frame behind what looked like a thorough set of
 * fail-closed guards: same thread as the mutating detour, vtable unchanged
 * since capture, +0x28 holding an object with GameCamera's exact vtable,
 * +0x320 within its 2-value range, every access through SEH-guarded reads.
 * It still faulted, reproducibly, ~94 seconds in, as a battle tore down and
 * the engine released its scene: exit code 0xC0000005 inside game code, with
 * the loader's own reads never faulting. Guards on a pointer the engine owns
 * and frees without telling us cannot establish that it is still alive - at
 * best they narrow the window, and a freed allocation that has been recycled
 * can satisfy every one of them.
 *
 * Bisected against the running client with WOTBMOD_DISABLE_HOOKS: no loader
 * 342s clean, previous loader 121s clean, this loader 94s crash, this loader
 * with only the per-frame dereference disabled 157s clean.
 *
 * The replacement needs no pointer at all. CameraModeChanged already receives
 * the value: its event argument's first dword is the mode, the same 2-valued
 * quantity as GameCamera+0x320, and the detour already reads it to map the
 * public camera mode. Publishing the view index from there is strictly more
 * information for strictly less risk.
 *
 * The cost, stated honestly: the view mode is now observed only when the
 * engine announces a change, so before the first announcement it reports
 * unavailable rather than a value. camera_v2.h has a validity flag for
 * exactly this, and "not observed yet" is the truthful answer - the previous
 * design's alternative was a value read out of memory the engine may already
 * have freed.
 */

bool DeclaredCameraStateAvailable() {
    return g_state.created &&
        g_state.compatibility !=
            WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH &&
        g_original_camera_switch_state != nullptr;
}

WotbModV3Result DeclaredCameraReadObservedState(
    void*,
    WotbModV3CameraObservedState* out_state) {
    if (!out_state) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!DeclaredCameraStateAvailable()) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const LONG animation = InterlockedCompareExchange(
        &g_state.camera_animation_state, -1, -1);
    const LONG view = InterlockedCompareExchange(
        &g_state.camera_view_mode, -1, -1);
    out_state->struct_size = sizeof(*out_state);
    out_state->api_version = WOTBMOD_V3_CAMERA_VERSION_2;
    out_state->animation_state =
        animation >= 0 ? static_cast<uint32_t>(animation) : 0u;
    out_state->animation_state_valid = animation >= 0 ? 1u : 0u;
    out_state->view_mode = view >= 0 ? static_cast<uint32_t>(view) : 0u;
    out_state->view_mode_valid = view >= 0 ? 1u : 0u;
    if (animation < 0 && view < 0) {
        /* Neither half resolvable; the runtime turns this into the
         * NOT_SUPPORTED camera_v2.h mandates. */
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    return WOTBMOD_V3_OK;
}

/* =========================================================================
 * wotbmod.tracer -- READ ONLY query of a 25-entry .rdata pointer table.
 *
 * No hook, no live object, no lifetime. The table is immutable and the bound
 * is the image's own (`cmp al, 19h / jnb error` immediately before
 * `mov edx, off_3FD6168[eax*4]` in the resolver at 0x0155A040), so the whole
 * surface is a bounds check and a bounded string copy.
 * ========================================================================= */

struct TracerStyleName {
    const char* name;
    uint32_t style_id;
};

const TracerStyleName kTracerStyleNames[] = {
    {"ARMOR_PIERCING", WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING},
    {"ARMOR_PIERCING_CR", WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING_CR},
    {"HIGH_EXPLOSIVE", WOTBMOD_V3_TRACER_STYLE_HIGH_EXPLOSIVE},
    {"HOLLOW_CHARGE", WOTBMOD_V3_TRACER_STYLE_HOLLOW_CHARGE},
    {"ANTI_TANK_GUIDED_MISSILE",
     WOTBMOD_V3_TRACER_STYLE_ANTI_TANK_GUIDED_MISSILE},
    {"RAILGUN", WOTBMOD_V3_TRACER_STYLE_RAILGUN},
    {"IMPROVED_DETECTION", WOTBMOD_V3_TRACER_STYLE_IMPROVED_DETECTION},
    {"STT_TRACER", WOTBMOD_V3_TRACER_STYLE_STT_TRACER}};

/*
 * Entries proven readable at binding time. Zero disables the interface: the
 * count is what the running client actually has, never the header constant.
 */
uint32_t g_tracer_shell_type_count = 0u;

bool ReadTracerStyleName(
    uint32_t shell_type,
    char* output,
    size_t capacity) {
    if (!output || capacity == 0u) return false;
    output[0] = '\0';
    if (shell_type >= WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT) return false;
    const void* table = AddressFromRva(kTracerShellStyleTableRva);
    if (!table) return false;
    const char* entry = nullptr;
    if (!SafeRead(
            static_cast<const uint8_t*>(table) +
                shell_type * sizeof(const char*),
            &entry,
            sizeof(entry)) ||
        !entry || !IsReadableAddress(entry)) {
        return false;
    }
    char buffer[WOTBMOD_V3_TRACER_STYLE_NAME_SIZE] = {};
    if (!SafeRead(entry, buffer, sizeof(buffer) - 1u)) return false;
    buffer[sizeof(buffer) - 1u] = '\0';
    const size_t length = std::strlen(buffer);
    if (length == 0u || length >= capacity) return false;
    std::memcpy(output, buffer, length + 1u);
    return true;
}

uint32_t ValidateTracerStyleTable() {
    char name[WOTBMOD_V3_TRACER_STYLE_NAME_SIZE] = {};
    for (uint32_t shell_type = 0u;
         shell_type < WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT;
         ++shell_type) {
        if (!ReadTracerStyleName(shell_type, name, sizeof(name))) {
            return 0u;
        }
    }
    return WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT;
}

WotbModV3Result DeclaredTracerGetShellTypeCount(
    void*,
    uint32_t* out_count) {
    if (!out_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (g_tracer_shell_type_count == 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    *out_count = g_tracer_shell_type_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result DeclaredTracerGetStyle(
    void*,
    uint32_t shell_type,
    WotbModV3TracerStyle* out_style) {
    if (!out_style) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (g_tracer_shell_type_count == 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    /* Never a clamp and never a default style. */
    if (shell_type >= g_tracer_shell_type_count) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    char name[WOTBMOD_V3_TRACER_STYLE_NAME_SIZE] = {};
    if (!ReadTracerStyleName(shell_type, name, sizeof(name))) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    out_style->struct_size = sizeof(*out_style);
    out_style->api_version = WOTBMOD_V3_TRACER_VERSION;
    out_style->shell_type = shell_type;
    out_style->style_id = WOTBMOD_V3_TRACER_STYLE_UNKNOWN;
    out_style->reserved = 0u;
    /*
     * The COLOUR IS NOT REPORTED, and this is a finding rather than an
     * omission.
     *
     * re_anchors.md marks record+0x34 as INFERRED. It is now confirmed as an
     * offset - the configuration path writes `movups [esi+34h], xmm0` twice -
     * but confirming the offset disproves the value being usable here:
     *
     *   1. the constant written first, xmmword_0368EFD0, is
     *      {1.0, 1.0, 1.0, 0.15} and is a FALLBACK, not the stock style;
     *   2. immediately after, the function looks the style name up in a
     *      runtime configuration map at [this+0x38] and, when the entry
     *      exists, reads its "Color" key and overwrites +0x34 with it.
     *
     * So the real per-shell colour lives in a live keyed record reached
     * through a `this` whose ownership and lifetime re_anchors.md explicitly
     * says are unvalidated, and reading it would mean calling two unproven
     * engine helpers with constructed DAVA strings. Publishing the .rdata
     * fallback as if it were the stock colour would be exactly the
     * "plausible-looking backend on unproven ownership" the doctrine
     * forbids, so COLOR_VALID stays clear and the colour stays zeroed - the
     * flag exists for precisely this case.
     */
    out_style->flags = WOTBMOD_V3_TRACER_STYLE_NAME_VALID;
    out_style->color = {0.0f, 0.0f, 0.0f, 0.0f};
    std::memset(out_style->style_name, 0, sizeof(out_style->style_name));
    const size_t length = std::strlen(name);
    if (length >= sizeof(out_style->style_name)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    std::memcpy(out_style->style_name, name, length + 1u);
    for (const TracerStyleName& known : kTracerStyleNames) {
        if (std::strcmp(known.name, name) == 0) {
            out_style->style_id = known.style_id;
            break;
        }
    }
    /*
     * A name the eight-way table does not know keeps style_id UNKNOWN with
     * the real string attached: tracer_v1.h makes the NAME authoritative so a
     * ninth style in a future build degrades instead of impersonating an
     * existing one.
     */
    return WOTBMOD_V3_OK;
}

/* =========================================================================
 * wotbmod.audio.intercept -- a VTABLE SLOT detour on CreateSoundEvent.
 *
 * The slot, not the function body: slot +0x0C of DAVA::SoundSystemProxy
 * (0x0360C894) currently holds 0x00C4F050, a twelve-byte tail-jump thunk that
 * MinHook could not safely trampoline, and slot +0x0C of
 * DAVA::WwiseHybridSoundSystem (0x0381A070) holds 0x024F3B70. Both are
 * patched, and each patch is refused unless the slot still contains exactly
 * the address this pack was generated for.
 *
 * Both are patched because the live singleton is normally the proxy but the
 * hybrid is reachable directly, and the proxy's thunk dispatches through the
 * hybrid's own slot - so one public call can enter twice. The thread-local
 * depth guard makes the outer entry the only one that dispatches; the inner
 * one passes straight through. The engine's own re-entry at 0x024F3BA2 goes
 * through a third vtable (WwiseSoundSystem) that is deliberately not patched.
 *
 * THE MATCH SET IS IMMORTAL AND APPEND-ONLY. Slots are filled before the
 * count is published with a release store, and nothing is ever removed,
 * rewritten or freed. The detour runs on an ARBITRARY thread inside the
 * engine's creation path - the engine takes its own mutex at
 * WwiseHybridSoundSystem+0x40 INSIDE the factory, which is the engine
 * declaring that concurrent creation is expected - so a detour that took a
 * lock could be made to wait by any other thread, and a mutable array would
 * need reclamation x86 gives no cheap primitive for. An acquire load of the
 * count plus an index into slots written before it is wait-free and has no
 * reclamation problem at all. The price of never shrinking is one strcmp and
 * one dispatch that answers NOT_FOUND: the sound plays exactly as stock.
 * ========================================================================= */

const uint32_t kMaxSoundInterceptNames = 64u;

struct SoundInterceptState {
    /* Guards APPENDS only. The detour never takes it. */
    SRWLOCK publish_lock;
    char names[kMaxSoundInterceptNames][WOTBMOD_V3_MAX_NAME];
    volatile LONG published_count;
    volatile LONG armed;
    volatile LONG in_flight;
    /* Set only after the engine stub factory was proven on this build. */
    volatile LONG suppression_ready;
    void** proxy_slot;
    void** hybrid_slot;
    SoundCreateEventFn proxy_original;
    SoundCreateEventFn hybrid_original;
};

SoundInterceptState g_sound_intercept = {
    SRWLOCK_INIT, {}, 0, 0, 0, 0, nullptr, nullptr, nullptr, nullptr};

/*
 * Re-entry guard. audio_v3.h is shaped so a thread-local bool is the whole
 * implementation: a callback cannot create a sound through this interface, so
 * the only re-entry left is the engine's own.
 */
thread_local uint32_t g_sound_intercept_depth = 0u;

bool ReadInternedFastName(
    const void* fast_name,
    char* output,
    size_t capacity) {
    if (!fast_name || !output || capacity == 0u) return false;
    output[0] = '\0';
    /*
     * DAVA FastName is ONE dword holding an interned const char*, so matching
     * is a dword load plus strcmp - no allocation, no FastNameDB lock and no
     * FastName construction on the hot path.
     */
    const char* interned = nullptr;
    if (!SafeRead(fast_name, &interned, sizeof(interned)) || !interned) {
        return false;
    }
    __try {
        for (size_t index = 0u; index + 1u < capacity; ++index) {
            const char value = interned[index];
            output[index] = value;
            if (value == '\0') return true;
        }
        /* Longer than any registrable name: it cannot match, so refuse. */
        output[0] = '\0';
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

bool SoundInterceptNameWanted(const char* name) {
    if (!name || !name[0]) return false;
    /* Acquire load: every slot below `count` was written before it. */
    const LONG count = InterlockedCompareExchange(
        &g_sound_intercept.published_count, 0, 0);
    for (LONG index = 0; index < count; ++index) {
        if (std::strcmp(g_sound_intercept.names[index], name) == 0) {
            return true;
        }
    }
    return false;
}

void* CallOriginalCreateSoundEvent(
    SoundCreateEventFn original,
    void* self,
    const void* fast_name,
    const int32_t* group) {
    if (!original) return nullptr;
    __try {
        return original(self, fast_name, group);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

/*
 * The engine's OWN null object, never nullptr: at least six of the eight real
 * call sites store the result straight into a field without a null check.
 * Returns null only if the factory itself failed, and the caller then falls
 * back to PASS_THROUGH - a sound that plays is always better than a store to
 * null.
 */
void* CreateEngineSoundEventStub() {
    if (InterlockedCompareExchange(
            &g_sound_intercept.suppression_ready, 0, 0) == 0) {
        return nullptr;
    }
    SoundEventStubFactoryFn factory =
        reinterpret_cast<SoundEventStubFactoryFn>(
            AddressFromRva(kSoundEventStubFactoryRva));
    if (!factory) return nullptr;
    void* stub = nullptr;
    __try {
        stub = factory();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!stub) return nullptr;
    void* vtable = nullptr;
    if (!SafeRead(stub, &vtable, sizeof(vtable)) ||
        vtable != AddressFromRva(kSoundEventStubVtableRva)) {
        /*
         * Cannot happen behind the fingerprint gate. If it ever does, stop
         * suppressing for the rest of the process rather than hand a caller
         * an object of unknown type; the one already-allocated object is
         * deliberately leaked instead of destroyed through a vtable we just
         * failed to recognise.
         */
        InterlockedExchange(&g_sound_intercept.suppression_ready, 0);
        return nullptr;
    }
    return stub;
}

/*
 * Interns `name` and yields the ONE dword a DAVA FastName is. Kept separate
 * from the creation call so a fault while interning never leaves the engine
 * half-way through building an event.
 *
 * The intern table's own lock is taken inside the ctor, and the sound-creation
 * mutex is taken inside the factory rather than around CreateSoundEvent, so
 * neither self-deadlocks from here. The ctor itself is the same address
 * WotbModDavaSound already exercises live on this fingerprint.
 */
bool ConstructInternedFastName(
    const char* name,
    void** out_fast_name) {
    if (!out_fast_name) return false;
    *out_fast_name = nullptr;
    FastNameCtorFn fast_name_ctor =
        reinterpret_cast<FastNameCtorFn>(
            AddressFromRva(kDefaultFastNameCtorRva));
    if (!name || !name[0] || !fast_name_ctor ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(fast_name_ctor))) {
        return false;
    }
    void* interned = nullptr;
    __try {
        fast_name_ctor(&interned, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!interned) return false;
    *out_fast_name = interned;
    return true;
}

/*
 * The runtime already wraps each mod callback in its own SEH handler, so a
 * faulting mod cannot unwind into the detour. This catch covers the rest of
 * the dispatch path - a std::bad_alloc from the subscription snapshot, say -
 * because an exception escaping into engine code is not something the engine
 * can be expected to survive, and the honest fallback is PASS_THROUGH.
 */
WotbModV3Result SafeDispatchSoundIntercept(
    const char* event_name,
    WotbModV3SoundInterceptResponse* response) {
    try {
        return wotbmod::v3::DispatchClientHostSoundIntercept(
            event_name, response);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

/*
 * One entry of the detour. Owns no unwindable object: under /EHsc MSVC runs
 * no destructor while unwinding to an __except, so every guarded region above
 * is its own frame and this function only sequences them.
 */
void* DispatchNativeCreateSoundEvent(
    SoundCreateEventFn original,
    void* self,
    const void* fast_name,
    const int32_t* group) {
    InterlockedIncrement(&g_sound_intercept.in_flight);
    void* result = nullptr;
    char name[WOTBMOD_V3_MAX_NAME] = {};
    const bool outermost = g_sound_intercept_depth == 0u;
    if (!outermost ||
        !fast_name ||
        !ReadInternedFastName(fast_name, name, sizeof(name)) ||
        !SoundInterceptNameWanted(name)) {
        result = CallOriginalCreateSoundEvent(
            original, self, fast_name, group);
        InterlockedDecrement(&g_sound_intercept.in_flight);
        return result;
    }

    ++g_sound_intercept_depth;
    WotbModV3SoundInterceptResponse response = {};
    response.struct_size = sizeof(response);
    response.api_version = WOTBMOD_V3_AUDIO_VERSION_3;
    response.decision = WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH;
    const WotbModV3Result dispatched =
        SafeDispatchSoundIntercept(name, &response);
    uint32_t decision = WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH;
    if (dispatched == WOTBMOD_V3_OK) {
        /*
         * EVERY non-OK return means PASS_THROUGH, and suppression is never
         * derived from a failure.
         */
        decision = response.decision;
    }
    if (decision == WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS) {
        result = CreateEngineSoundEventStub();
    } else if (decision == WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE) {
        /* The runtime guarantees a non-empty terminated name here. */
        response.substitute_event_name[
            sizeof(response.substitute_event_name) - 1u] = '\0';
        void* substitute = nullptr;
        if (ConstructInternedFastName(
                response.substitute_event_name, &substitute)) {
            result = CallOriginalCreateSoundEvent(
                original, self, &substitute, group);
            if (!result) {
                /*
                 * The substituted creation faulted - the engine never returns
                 * null on its own, which is why six of its eight call sites do
                 * not check. Re-entering the creation path a second time on a
                 * path that just faulted is worse than handing back the
                 * engine's own null object, so try the stub first.
                 */
                result = CreateEngineSoundEventStub();
            }
        }
    }
    if (!result) {
        result = CallOriginalCreateSoundEvent(
            original, self, fast_name, group);
    }
    --g_sound_intercept_depth;
    InterlockedDecrement(&g_sound_intercept.in_flight);
    return result;
}

void* __fastcall SoundProxyCreateEventDetour(
    void* self,
    void*,
    const void* fast_name,
    const int32_t* group) {
    DetourGuard guard;
    return DispatchNativeCreateSoundEvent(
        g_sound_intercept.proxy_original, self, fast_name, group);
}

void* __fastcall SoundHybridCreateEventDetour(
    void* self,
    void*,
    const void* fast_name,
    const int32_t* group) {
    DetourGuard guard;
    return DispatchNativeCreateSoundEvent(
        g_sound_intercept.hybrid_original, self, fast_name, group);
}

bool ExchangeVtableSlot(
    void** slot,
    void* expected,
    void* replacement,
    void** out_previous) {
    if (!slot) return false;
    void* current = nullptr;
    if (!SafeRead(slot, &current, sizeof(current)) ||
        current != expected) {
        return false;
    }
    DWORD previous_protection = 0u;
    if (!VirtualProtect(
            slot,
            sizeof(void*),
            PAGE_READWRITE,
            &previous_protection)) {
        return false;
    }
    void* replaced = InterlockedExchangePointer(slot, replacement);
    DWORD restored = 0u;
    VirtualProtect(
        slot, sizeof(void*), previous_protection, &restored);
    if (out_previous) *out_previous = replaced;
    return true;
}

bool BindSoundSystemSlot(
    uint32_t vtable_rva,
    uint32_t expected_impl_rva,
    const uint8_t* expected_prologue,
    size_t prologue_size,
    void*** out_slot,
    void** out_expected) {
    *out_slot = nullptr;
    *out_expected = nullptr;
    uint8_t* vtable = static_cast<uint8_t*>(AddressFromRva(vtable_rva));
    void* expected = AddressFromRva(expected_impl_rva);
    if (!vtable || !expected || !IsReadableAddress(vtable) ||
        !IsExecutableAddress(expected) ||
        !MatchesBytes(expected, expected_prologue, prologue_size)) {
        return false;
    }
    void** slot = reinterpret_cast<void**>(vtable + 0x0Cu);
    void* current = nullptr;
    /* The slot must still hold exactly what this pack was generated for. */
    if (!SafeRead(slot, &current, sizeof(current)) ||
        current != expected) {
        return false;
    }
    *out_slot = slot;
    *out_expected = expected;
    return true;
}

/*
 * One-shot proof that suppression can actually be performed on this build,
 * run at arm time rather than assumed: build the stub, check it carries the
 * SoundEventStub vtable, and LEAK it deliberately. Releasing it would mean
 * driving a destructor through a vtable purely to reclaim 36 bytes at
 * startup, which is a worse trade than the leak.
 */
bool ProveSoundEventStubFactory() {
    const uint8_t stub_prologue[] = {
        0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu, 0x68u};
    void* factory_address =
        AddressFromRva(kSoundEventStubFactoryRva);
    if (!factory_address || !IsExecutableAddress(factory_address) ||
        !MatchesBytes(
            factory_address, stub_prologue, sizeof(stub_prologue))) {
        return false;
    }
    SoundEventStubFactoryFn factory =
        reinterpret_cast<SoundEventStubFactoryFn>(factory_address);
    void* stub = nullptr;
    __try {
        stub = factory();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!stub) return false;
    void* vtable = nullptr;
    return SafeRead(stub, &vtable, sizeof(vtable)) &&
        vtable == AddressFromRva(kSoundEventStubVtableRva);
}

/*
 * Read-only rehearsal of the arm-time checks, used to decide whether the
 * interception group is offered at all. Without it the interface would be
 * advertised DEGRADED and then refuse the first registration, which is the
 * "one status describing two things" lie the five interfaces were split up to
 * avoid.
 */
bool SoundInterceptSlotsBindable() {
    const uint8_t proxy_prologue[] = {
        0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x49u, 0x20u,
        0x8Bu, 0x01u, 0x5Du, 0xFFu, 0x60u, 0x0Cu};
    const uint8_t hybrid_prologue[] = {
        0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu, 0x68u};
    void** slot = nullptr;
    void* expected = nullptr;
    if (BindSoundSystemSlot(
            kDefaultSoundSystemProxyVtableRva,
            kSoundSystemProxyCreateEventRva,
            proxy_prologue,
            sizeof(proxy_prologue),
            &slot,
            &expected)) {
        return true;
    }
    return BindSoundSystemSlot(
        kDefaultSoundSystemVtableRva,
        kSoundSystemHybridCreateEventRva,
        hybrid_prologue,
        sizeof(hybrid_prologue),
        &slot,
        &expected);
}

/* Caller holds publish_lock. */
WotbModV3Result ArmSoundInterceptLocked() {
    if (InterlockedCompareExchange(&g_sound_intercept.armed, 0, 0) != 0) {
        return WOTBMOD_V3_OK;
    }
    if (!g_state.created ||
        g_state.compatibility ==
            WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const uint8_t proxy_prologue[] = {
        0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x49u, 0x20u,
        0x8Bu, 0x01u, 0x5Du, 0xFFu, 0x60u, 0x0Cu};
    const uint8_t hybrid_prologue[] = {
        0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu, 0x68u};
    void** proxy_slot = nullptr;
    void* proxy_expected = nullptr;
    void** hybrid_slot = nullptr;
    void* hybrid_expected = nullptr;
    const bool proxy_ready = BindSoundSystemSlot(
        kDefaultSoundSystemProxyVtableRva,
        kSoundSystemProxyCreateEventRva,
        proxy_prologue,
        sizeof(proxy_prologue),
        &proxy_slot,
        &proxy_expected);
    const bool hybrid_ready = BindSoundSystemSlot(
        kDefaultSoundSystemVtableRva,
        kSoundSystemHybridCreateEventRva,
        hybrid_prologue,
        sizeof(hybrid_prologue),
        &hybrid_slot,
        &hybrid_expected);
    if (!proxy_ready && !hybrid_ready) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    /*
     * Originals first, then the slots. If the store below is observed by
     * another thread the instant it lands, the detour it reaches must already
     * know what to call.
     */
    if (proxy_ready) {
        g_sound_intercept.proxy_original =
            reinterpret_cast<SoundCreateEventFn>(proxy_expected);
    }
    if (hybrid_ready) {
        g_sound_intercept.hybrid_original =
            reinterpret_cast<SoundCreateEventFn>(hybrid_expected);
    }
    InterlockedExchange(
        &g_sound_intercept.suppression_ready,
        ProveSoundEventStubFactory() ? 1 : 0);

    bool patched = false;
    if (proxy_ready &&
        ExchangeVtableSlot(
            proxy_slot,
            proxy_expected,
            reinterpret_cast<void*>(&SoundProxyCreateEventDetour),
            nullptr)) {
        g_sound_intercept.proxy_slot = proxy_slot;
        patched = true;
    }
    if (hybrid_ready &&
        ExchangeVtableSlot(
            hybrid_slot,
            hybrid_expected,
            reinterpret_cast<void*>(&SoundHybridCreateEventDetour),
            nullptr)) {
        g_sound_intercept.hybrid_slot = hybrid_slot;
        patched = true;
    }
    if (!patched) return WOTBMOD_V3_E_NOT_SUPPORTED;
    InterlockedExchange(&g_sound_intercept.armed, 1);
    Log(InterlockedCompareExchange(
            &g_sound_intercept.suppression_ready, 0, 0) != 0
            ? "sound interception armed (suppression proven)"
            : "sound interception armed; SUPPRESS degrades to PASS_THROUGH "
              "because the engine stub factory could not be proven");
    return WOTBMOD_V3_OK;
}

WotbModV3Result DeclaredSoundInterceptPublishName(
    void*,
    const char* event_name) {
    if (!event_name || !event_name[0]) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strlen(event_name) >= WOTBMOD_V3_MAX_NAME) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&g_sound_intercept.publish_lock);
    WotbModV3Result result = WOTBMOD_V3_OK;
    const LONG count = InterlockedCompareExchange(
        &g_sound_intercept.published_count, 0, 0);
    bool already_published = false;
    for (LONG index = 0; index < count; ++index) {
        if (std::strcmp(g_sound_intercept.names[index], event_name) == 0) {
            already_published = true;
            break;
        }
    }
    if (!already_published) {
        if (count >= static_cast<LONG>(kMaxSoundInterceptNames)) {
            result = WOTBMOD_V3_E_LIMIT_REACHED;
        } else {
            /*
             * Fill the slot, THEN publish the count. The detour reads the
             * count with an acquire load and only ever indexes below it, so
             * it can never observe a half-written name.
             */
            strcpy_s(
                g_sound_intercept.names[count],
                sizeof(g_sound_intercept.names[count]),
                event_name);
            InterlockedExchange(
                &g_sound_intercept.published_count, count + 1);
        }
    }
    if (result == WOTBMOD_V3_OK) {
        /* Name first, arm second - never the other way round. */
        result = ArmSoundInterceptLocked();
    }
    ReleaseSRWLockExclusive(&g_sound_intercept.publish_lock);
    return result;
}

WotbModV3Result DeclaredSoundInterceptDisarm(void*) {
    AcquireSRWLockExclusive(&g_sound_intercept.publish_lock);
    if (g_sound_intercept.proxy_slot) {
        ExchangeVtableSlot(
            g_sound_intercept.proxy_slot,
            reinterpret_cast<void*>(&SoundProxyCreateEventDetour),
            reinterpret_cast<void*>(g_sound_intercept.proxy_original),
            nullptr);
        g_sound_intercept.proxy_slot = nullptr;
    }
    if (g_sound_intercept.hybrid_slot) {
        ExchangeVtableSlot(
            g_sound_intercept.hybrid_slot,
            reinterpret_cast<void*>(&SoundHybridCreateEventDetour),
            reinterpret_cast<void*>(g_sound_intercept.hybrid_original),
            nullptr);
        g_sound_intercept.hybrid_slot = nullptr;
    }
    InterlockedExchange(&g_sound_intercept.armed, 0);
    ReleaseSRWLockExclusive(&g_sound_intercept.publish_lock);
    /*
     * The contract is that this returns only once no detour invocation is
     * still running anywhere. The slots are unpatched by now so the counter
     * only decreases; the bound exists purely so a thread parked inside game
     * code cannot hang teardown forever, and overrunning it is reported
     * rather than silently accepted.
     */
    for (uint32_t spin = 0u;
         InterlockedCompareExchange(
             &g_sound_intercept.in_flight, 0, 0) != 0 && spin < 10000u;
         ++spin) {
        Sleep(1);
    }
    if (InterlockedCompareExchange(
            &g_sound_intercept.in_flight, 0, 0) != 0) {
        Log("sound interception disarm timed out with a detour still in "
            "flight");
        return WOTBMOD_V3_E_BUSY;
    }
    /*
     * The match set is deliberately NOT cleared. Nothing reads it once the
     * detour is gone, and clearing it would turn an immortal array into a
     * mutable one for no gain.
     */
    return WOTBMOD_V3_OK;
}

/* =========================================================================
 * MAIN-THREAD FRAME INGRESS.
 *
 * DAVA::Private::EngineBackend::UpdateAndDrawWindows. The thread identity is
 * PROVEN, not assumed: the function's only caller is 0x00ABF653 inside
 * 0x00ABF320, and that function is the Win32 game loop - PeekMessageW at
 * 0x00ABF4F0/0x00ABF535, TranslateMessage at 0x00ABF516, DispatchMessageW at
 * 0x00ABF520. A thread that pumps a window's message queue is that window's
 * thread. It runs once per frame unconditionally, including on UI-only
 * screens where no 3D scene exists.
 *
 * Scene::Draw is NOT a substitute as a general pump: it does not tick during
 * login, loading or UI-only screens; it is a virtual slot invoked per drawn
 * UI3DView, so its rate is neither one-per-frame nor guaranteed non-zero; and
 * it runs inside a draw pass. It stays what it already is - the active-scene
 * tracker.
 *
 * The existing Present-side pump is deliberately left where it is. It drives
 * UI retirement on a frame epoch that other code already reasons about, and
 * this ingress answers a different question: which thread is allowed to touch
 * unlocked engine containers, and when is it safe to sample a cached
 * CameraController.
 * ========================================================================= */

/*
 * The per-pump callback ceiling. A frame is a hard real-time budget shared
 * with the game, so the queue is drained a bounded slice at a time and the
 * remainder waits for the next frame rather than extending this one. 64 is
 * the figure re_anchors.md specified for this ingress.
 */
constexpr uint32_t kMainPumpCallbacksPerFrame = 64u;

void PumpNativeMainThreadFrame() {
    const uint64_t frame = static_cast<uint64_t>(
        InterlockedIncrement64(&g_state.main_frame_index));
    const DWORD thread_id = GetCurrentThreadId();
    InterlockedExchange(
        &g_state.main_thread_id, static_cast<LONG>(thread_id));
    wotbmod::loader::NotifyV3NativeClientServicesMainThread(
        static_cast<uint32_t>(thread_id), frame);
    /*
     * The arcade/sniper half is sampled here rather than in the public slot
     * because the public slot is callable from any thread and a
     * CameraController may only be dereferenced on the thread that mutates
     * it. What the mod reads is always a cached POD.
     */
    /*
     * Drain the runtime's MAIN queue last, after the loader's own per-frame
     * state has been refreshed, so a mod callback that asks for the camera or
     * walks the scene sees this frame rather than the previous one.
     *
     * PumpMainThread sets the calling thread's role to MAIN for the duration
     * and restores it afterwards, which is what finally makes a main-thread
     * scene walk reachable: mod callbacks otherwise run from DispatchFrame on
     * the Present thread with the role set to RENDER, and the walk refuses
     * anywhere but MAIN. That refusal is not pedantry - AddNode/RemoveNode
     * memmove the child vector with no lock and Release the child immediately
     * after compacting, so a walk on any other thread can read a pointer that
     * is freed one instruction later.
     *
     * It is called unconditionally: the pump itself is a no-op while ingress
     * is offline, and it guards its own re-entry.
     */
    if (!HookDisabled("main_pump")) {
        wotbmod::v3::PumpMainThread(kMainPumpCallbacksPerFrame);
    }
}

/* Frame profile accumulators read by the loader's hitch report; defined here
 * because this detour is the only writer and the gameplay bridge test links
 * this file without the loader. */
extern "C" {
volatile LONGLONG g_wotbProfEngineTicks = 0;
volatile LONGLONG g_wotbProfPumpTicks = 0;
volatile LONGLONG g_wotbProfEngineCalls = 0;
}

uint32_t __fastcall EngineUpdateAndDrawWindowsDetour(
    void* self,
    void*,
    float frame_delta,
    uint32_t skip_update) {
    DetourGuard guard;
    uint32_t original_result = 0u;
    LARGE_INTEGER t0 = {}, t1 = {}, t2 = {};
    QueryPerformanceCounter(&t0);
    if (g_original_update_and_draw_windows) {
        original_result = g_original_update_and_draw_windows(
            self, frame_delta, skip_update);
    }
    QueryPerformanceCounter(&t1);
    InterlockedExchangeAdd64(&g_wotbProfEngineTicks, t1.QuadPart - t0.QuadPart);
    InterlockedIncrement64(&g_wotbProfEngineCalls);
    /*
     * After the engine's own update and draw: the frame's scene mutations
     * have happened, so a cached pointer sampled here is as fresh as it can
     * be. Nothing in the pump can change what the engine just did.
     */
    PumpNativeMainThreadFrame();
    QueryPerformanceCounter(&t2);
    InterlockedExchangeAdd64(&g_wotbProfPumpTicks, t2.QuadPart - t1.QuadPart);
    return original_result;
}

EntityRecord* FindEntityRecordByPublicIdLocked(uint32_t public_id) {
    if (public_id == 0u) return nullptr;
    for (uint32_t index = 0u; index < kMaxTrackedEntities; ++index) {
        EntityRecord& record = g_state.entities[index];
        if (record.alive && record.public_id == public_id) {
            return &record;
        }
    }
    return nullptr;
}

EntityRecord* FindEntityRecordByPointerLocked(void* entity) {
    if (!entity) return nullptr;
    for (uint32_t index = 0u; index < kMaxTrackedEntities; ++index) {
        EntityRecord& record = g_state.entities[index];
        if (record.alive &&
            (record.entity == entity ||
             record.game_logic == entity)) {
            return &record;
        }
    }
    return nullptr;
}

EntityRecord* ReservePublicEntityRecordLocked(
    void* entity,
    void* game_logic,
    uint32_t public_id) {
    EntityRecord* existing =
        FindEntityRecordByPublicIdLocked(public_id);
    if (existing) {
        if (entity) existing->entity = entity;
        if (game_logic) existing->game_logic = game_logic;
        return existing;
    }
    EntityRecord* free_record = nullptr;
    for (uint32_t index = 0u; index < kMaxTrackedEntities; ++index) {
        if (!g_state.entities[index].alive) {
            free_record = &g_state.entities[index];
            break;
        }
    }
    if (!free_record) return nullptr;
    const uint64_t token = static_cast<uint64_t>(
        InterlockedIncrement64(&g_state.next_entity_token));
    *free_record = {};
    free_record->token = token ? token : 1u;
    free_record->entity = entity;
    free_record->game_logic = game_logic;
    free_record->public_id = public_id;
    free_record->alive = true;
    return free_record;
}

void* __fastcall BwEntityCtorDetour(
    void* self,
    void*,
    int32_t entity_id) {
    DetourGuard guard;
    void* const entity_target = AddressFromRva(kBwEntityCtorRva);
    if (g_original_entity_ctor) {
        hobs::Before<BwEntityCtorFn>(entity_target, self, entity_id);
    }
    void* result = g_original_entity_ctor
        ? g_original_entity_ctor(self, entity_id)
        : self;
    if (g_original_entity_ctor) {
        hobs::After<BwEntityCtorFn>(entity_target, self, entity_id);
    }
    /*
     * Construction is not proof that an entity is local or visible. Raw
     * BWEntity instances are intentionally not entered into the public
     * registry here.
     */
    return result;
}

void __fastcall BwEntityDtorDetour(void* self, void*) {
    DetourGuard guard;
    uint32_t public_id = 0u;
    AcquireSRWLockShared(&g_state.entity_lock);
    const EntityRecord* record =
        FindEntityRecordByPointerLocked(self);
    if (record && record->published) {
        public_id = record->public_id;
    }
    ReleaseSRWLockShared(&g_state.entity_lock);
    if (public_id != 0u) {
        WotbModV3NativeBindings_RemovePublicVehicle(
            public_id,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_NATIVE_DESTROYED);
    }
    if (g_original_entity_dtor) {
        void* const target = AddressFromRva(kBwEntityDtorRva);
        hobs::Before<BwEntityDtorFn>(target, self);
        g_original_entity_dtor(self);
        hobs::After<BwEntityDtorFn>(target, self);
    }
}

void* __fastcall TracerManagerCtorDetour(
    void* self,
    void*,
    void* first,
    void* second) {
    DetourGuard guard;
    void* const tracer_target = AddressFromRva(kTracerManagerCtorRva);
    if (g_original_tracer_ctor) {
        hobs::Before<TracerManagerCtorFn>(tracer_target, self, first, second);
    }
    void* result = g_original_tracer_ctor
        ? g_original_tracer_ctor(self, first, second)
        : self;
    if (g_original_tracer_ctor) {
        hobs::After<TracerManagerCtorFn>(tracer_target, self, first, second);
    }
    void* vtable = nullptr;
    if (result &&
        SafeRead(result, &vtable, sizeof(vtable)) &&
        vtable == AddressFromRva(kTracerManagerVtableRva)) {
        InterlockedExchangePointer(
            &g_state.tracer_manager, result);
    }
    return result;
}

void* __fastcall TracerManagerDtorDetour(
    void* self,
    void*,
    uint32_t deleteFlags) {
    DetourGuard guard;
    InterlockedCompareExchangePointer(
        &g_state.tracer_manager, nullptr, self);
    if (!g_original_tracer_dtor) return self;
    void* const dtor_target = AddressFromRva(kTracerManagerDtorRva);
    hobs::Before<TracerManagerDtorFn>(dtor_target, self, deleteFlags);
    void* const result = g_original_tracer_dtor(self, deleteFlags);
    hobs::After<TracerManagerDtorFn>(dtor_target, self, deleteFlags);
    return result;
}

/*
 * WHY A TRACER IS NOT SIMPLY MATCHED TO THE SHOT THAT CAUSED IT.
 *
 * There is no identity on this build that links the two. Measured, not assumed:
 *
 *   - ShowTracer's two uint32 arguments are the tracer's own source/destination
 *     ids in the manager's numbering. They are not entity ids -
 *     CreateStockTracer mints them from a private counter and the client accepts
 *     them, so they carry no entity identity at all.
 *   - A stock-shot ProjectileRecord carries public_entity_id and owner_scope but
 *     no geometry: record->origin is written only by ObserveImpact (the impact
 *     point), never by ObserveShot. So the tracer's origin cannot be compared
 *     against the shot.
 *   - Entity snapshots would give a muzzle to compare against, but
 *     snapshot.position is never written anywhere in this file; every public
 *     entity reports the origin of the world.
 *
 * That leaves causal adjacency: the shot RPC reaches ObserveShot and the visual
 * for that same shot reaches ShowTracer moments later. That is a real causal
 * chain, but it is not an identity, so it is used only when it cannot be
 * ambiguous: exactly ONE live, published, not-yet-traced projectile may have
 * been created inside the window. Two candidates means two vehicles fired at
 * once and either answer would be a guess, so nothing is published and the
 * refusal is counted. A published owner_scope is a claim about who shot at
 * whom; it is better to publish no tracer than the wrong shooter.
 */
const uint64_t kTracerAttributionWindowMs = 60u;

ProjectileRecord* AttributeTracerLocked(uint64_t now_milliseconds) {
    ProjectileRecord* match = nullptr;
    for (uint32_t index = 0u;
         index < kMaxTrackedProjectiles;
         ++index) {
        ProjectileRecord& record = g_state.projectiles[index];
        if (!record.alive || !record.published ||
            record.tracer_published ||
            record.public_handle == WOTBMOD_V3_INVALID_HANDLE ||
            record.owner_scope ==
                WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN ||
            now_milliseconds < record.created_tick_milliseconds ||
            now_milliseconds - record.created_tick_milliseconds >
                kTracerAttributionWindowMs) {
            continue;
        }
        if (match) return nullptr;  // ambiguous: refuse rather than guess
        match = &record;
    }
    return match;
}

WotbModV3Result PublishVisibleTracer(
    const char* topic,
    WotbModV3ProjectileHandle projectile,
    uint32_t owner_scope,
    uint32_t shell_type,
    const WotbModV3Vec3& position,
    const WotbModV3Vec3& direction) {
    WotbModV3VisibleTracerEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_PROJECTILE_VERSION;
    event.projectile = projectile;
    event.owner_scope = owner_scope;
    event.shell_type = shell_type;
    event.visible_position = position;
    event.visible_direction = direction;
    return wotbmod::v3::PublishClientHostEvent(
        topic, &event, sizeof(event));
}

void __fastcall TracerManagerShowTracerDetour(
    void* self,
    void*,
    const float* origin,
    const uint32_t* sourceId,
    const float* destination,
    const uint32_t* destinationId,
    const uint8_t* shellType,
    const float* parameter,
    uint32_t count) {
    DetourGuard guard;
    void* const show_target = AddressFromRva(kTracerManagerShowTracerRva);
    if (g_original_tracer_show) {
        hobs::Before<TracerManagerShowTracerFn>(
            show_target, self, origin, sourceId, destination, destinationId, shellType, parameter, count);
    }
    if (g_original_tracer_show) {
        g_original_tracer_show(
            self,
            origin,
            sourceId,
            destination,
            destinationId,
            shellType,
            parameter,
            count);
    }
    if (g_original_tracer_show) {
        hobs::After<TracerManagerShowTracerFn>(
            show_target, self, origin, sourceId, destination, destinationId, shellType, parameter, count);
    }
    /*
     * A tracer this loader asked the client to draw belongs to no shot. Skip it
     * outright rather than let it consume the one attribution slot a real shot
     * would have used.
     */
    if (InterlockedCompareExchange(
            &g_state.self_issued_tracer, 0, 0) != 0) {
        return;
    }
    float origin_values[3] = {};
    float destination_values[3] = {};
    uint8_t shell_value = 0u;
    if (!origin || !destination ||
        !SafeRead(origin, origin_values, sizeof(origin_values)) ||
        !SafeRead(
            destination,
            destination_values,
            sizeof(destination_values))) {
        return;
    }
    if (shellType) {
        SafeRead(shellType, &shell_value, sizeof(shell_value));
    }
    const WotbModV3Vec3 position = {
        origin_values[0], origin_values[1], origin_values[2]};
    const WotbModV3Vec3 target = {
        destination_values[0],
        destination_values[1],
        destination_values[2]};
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || !std::isfinite(target.x) ||
        !std::isfinite(target.y) || !std::isfinite(target.z)) {
        return;
    }
    const WotbModV3Vec3 direction = Normalize(
        {target.x - position.x,
         target.y - position.y,
         target.z - position.z});

    const uint64_t now = GetTickCount64();
    WotbModV3ProjectileHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t owner_scope = WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    ProjectileRecord* record = AttributeTracerLocked(now);
    if (record) {
        record->tracer_published = true;
        record->tracer_position = position;
        record->tracer_direction = direction;
        record->tracer_shell_type = shell_value;
        handle = record->public_handle;
        owner_scope = record->owner_scope;
    }
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    if (handle == WOTBMOD_V3_INVALID_HANDLE) {
        InterlockedIncrement(&g_state.tracer_unattributed);
        return;
    }

    const WotbModV3Result result = PublishVisibleTracer(
        WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED,
        handle,
        owner_scope,
        shell_value,
        position,
        direction);
    if (result == WOTBMOD_V3_OK) {
        InterlockedIncrement(&g_state.tracer_attributed);
        return;
    }
    /*
     * The registry refused the event, so nothing was announced and nothing is
     * owed a retirement. Clear the flag again or the removal path would publish
     * a destroyed for a tracer no mod ever saw created.
     */
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    for (uint32_t index = 0u;
         index < kMaxTrackedProjectiles;
         ++index) {
        ProjectileRecord& candidate = g_state.projectiles[index];
        if (candidate.alive &&
            candidate.public_handle == handle) {
            candidate.tracer_published = false;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    static LONG reported_publish[24] = {};
    const int slot = (result >= 0 && result < 24) ? result : 23;
    if (InterlockedExchange(&reported_publish[slot], 1) == 0) {
        char message[160] = {};
        sprintf_s(
            message,
            "[v3] tracer ingress: visible_tracer_created -> %d (scope %u)",
            static_cast<int>(result),
            owner_scope);
        Log(message);
    }
}

bool IsFixedTextValid(const char* text, size_t capacity) {
    if (!text || capacity <= 1u) return false;
    __try {
        return std::memchr(text, '\0', capacity) != nullptr &&
               text[0] != '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t UnixTimestampMicroseconds() {
    FILETIME file_time = {};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER ticks = {};
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    const uint64_t kWindowsToUnixTicks =
        UINT64_C(116444736000000000);
    return ticks.QuadPart > kWindowsToUnixTicks
               ? (ticks.QuadPart - kWindowsToUnixTicks) / 10u
               : 0u;
}

bool IsPublicEntityRegistered(uint32_t public_id) {
    bool registered = false;
    AcquireSRWLockShared(&g_state.entity_lock);
    const EntityRecord* record =
        FindEntityRecordByPublicIdLocked(public_id);
    registered = record && record->published &&
                 record->public_handle != WOTBMOD_V3_INVALID_HANDLE &&
                 (record->snapshot.local_player != 0u ||
                  record->snapshot.visible_to_player != 0u);
    ReleaseSRWLockShared(&g_state.entity_lock);
    return registered;
}

bool ResolveProjectileOwnerScope(
    uint32_t public_id,
    uint32_t* out_scope) {
    if (!public_id || !out_scope) return false;
    bool resolved = false;
    AcquireSRWLockShared(&g_state.entity_lock);
    const EntityRecord* subject =
        FindEntityRecordByPublicIdLocked(public_id);
    if (subject && subject->published &&
        (subject->snapshot.local_player != 0u ||
         subject->snapshot.visible_to_player != 0u)) {
        if (subject->snapshot.local_player != 0u) {
            *out_scope =
                WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER;
            resolved = true;
        } else if (
            (subject->snapshot.team != 0u) &&
            (subject->snapshot.visible_to_player != 0u)) {
            uint32_t local_team = 0u;
            for (uint32_t index = 0u;
                 index < kMaxTrackedEntities;
                 ++index) {
                const EntityRecord& candidate =
                    g_state.entities[index];
                if (candidate.alive && candidate.published &&
                    candidate.snapshot.local_player != 0u) {
                    local_team = candidate.snapshot.team;
                    break;
                }
            }
            if (local_team != 0u) {
                *out_scope = subject->snapshot.team == local_team
                    ? WOTBMOD_V3_PROJECTILE_OWNER_ALLY_VISIBLE
                    : WOTBMOD_V3_PROJECTILE_OWNER_ENEMY_VISIBLE;
                resolved = true;
            }
        }
    }
    ReleaseSRWLockShared(&g_state.entity_lock);
    return resolved;
}

wotbmod::v3::ClientHostProjectile ToClientHostProjectile(
    const ProjectileRecord& record) {
    wotbmod::v3::ClientHostProjectile projectile = {};
    projectile.struct_size = sizeof(projectile);
    projectile.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    projectile.native_token = record.token;
    projectile.owner_scope = record.owner_scope;
    projectile.shell_type = record.shell_type;
    projectile.visible_position = record.origin;
    projectile.visible_direction = record.direction;
    projectile.sequence_id = record.token;
    projectile.valid_fields = record.valid_fields;
    projectile.timestamp_microseconds =
        record.timestamp_microseconds;
    projectile.lifecycle_state = record.lifecycle_state;
    projectile.source = record.source;
    projectile.native_shot_id = record.native_shot_id;
    projectile.primary_entity_id = record.public_entity_id;
    projectile.secondary_entity_id =
        record.secondary_public_entity_id;
    projectile.native_flags = record.native_flags;
    projectile.stock_shot_code = record.stock_shot_code;
    projectile.origin = record.origin;
    projectile.impact_position = record.impact_position;
    return projectile;
}

ProjectileRecord* ReserveProjectileRecordLocked() {
    for (uint32_t index = 0u;
         index < kMaxTrackedProjectiles;
         ++index) {
        if (!g_state.projectiles[index].alive) {
            ProjectileRecord& record =
                g_state.projectiles[index];
            record = {};
            const uint64_t token = static_cast<uint64_t>(
                InterlockedIncrement64(
                    &g_state.next_projectile_token));
            record.token = token ? token : 1u;
            record.alive = true;
            record.created_tick_milliseconds = GetTickCount64();
            record.timestamp_microseconds =
                UnixTimestampMicroseconds();
            return &record;
        }
    }
    return nullptr;
}

WotbModV3Result PublishNewProjectile(
    ProjectileRecord* record,
    bool publish_in_flight) {
    if (!record || !record->alive ||
        record->owner_scope == WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const uint64_t token = record->token;
    const wotbmod::v3::ClientHostProjectile created =
        ToClientHostProjectile(*record);
    WotbModV3ProjectileHandle handle =
        WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result =
        wotbmod::v3::RegisterClientHostProjectile(
            &created, &handle);
    if (result != WOTBMOD_V3_OK) return result;
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    if (!record->alive || record->token != token) {
        ReleaseSRWLockExclusive(&g_state.projectile_lock);
        wotbmod::v3::RemoveClientHostProjectile(
            handle,
            WOTBMOD_V3_PROJECTILE_DESTROY_NATIVE);
        return WOTBMOD_V3_E_CONFLICT;
    }
    record->public_handle = handle;
    record->published = true;
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    result = wotbmod::v3::PublishClientHostProjectileLifecycle(
        handle, 0u, 0u);
    if (result != WOTBMOD_V3_OK) return result;
    if (!publish_in_flight) return WOTBMOD_V3_OK;
    wotbmod::v3::ClientHostProjectile updated = {};
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    if (!record->alive || record->token != token) {
        ReleaseSRWLockExclusive(&g_state.projectile_lock);
        return WOTBMOD_V3_E_CONFLICT;
    }
    record->lifecycle_state =
        WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT;
    updated = ToClientHostProjectile(*record);
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    result = wotbmod::v3::UpdateClientHostProjectile(
        handle, &updated);
    if (result == WOTBMOD_V3_OK) {
        result = wotbmod::v3::PublishClientHostProjectileLifecycle(
            handle,
            WOTBMOD_V3_PROJECTILE_STATE_CREATED,
            0u);
    }
    return result;
}

/*
 * A projectile that announced a visible tracer owes a matching retirement, and
 * the retirement has to go out BEFORE the registry drops the handle:
 * PublishClientHostEvent resolves tracer->projectile through GetHostProjectile,
 * so an event published after RemoveClientHostProjectile would be rejected as an
 * invalid handle and the tracer would simply never be retired. The geometry
 * comes off the record because ShowTracer was the only place it was ever known.
 */
struct RetiredProjectile {
    WotbModV3ProjectileHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    bool tracer_published = false;
    uint32_t owner_scope = WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
    uint32_t tracer_shell_type = 0u;
    WotbModV3Vec3 tracer_position = {};
    WotbModV3Vec3 tracer_direction = {};
};

RetiredProjectile CaptureRetirementLocked(
    const ProjectileRecord& record) {
    RetiredProjectile retired;
    retired.handle = record.public_handle;
    retired.tracer_published = record.tracer_published;
    retired.owner_scope = record.owner_scope;
    retired.tracer_shell_type = record.tracer_shell_type;
    retired.tracer_position = record.tracer_position;
    retired.tracer_direction = record.tracer_direction;
    return retired;
}

void RetireProjectiles(
    const RetiredProjectile* retired,
    uint32_t count,
    uint32_t reason) {
    for (uint32_t index = 0u; index < count; ++index) {
        const RetiredProjectile& entry = retired[index];
        if (entry.tracer_published) {
            PublishVisibleTracer(
                WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED,
                entry.handle,
                entry.owner_scope,
                entry.tracer_shell_type,
                entry.tracer_position,
                entry.tracer_direction);
        }
        wotbmod::v3::RemoveClientHostProjectile(
            entry.handle, reason);
    }
}

void ExpirePublicProjectiles(uint64_t now_milliseconds) {
    RetiredProjectile expired[kMaxTrackedProjectiles] = {};
    uint32_t count = 0u;
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    for (uint32_t index = 0u;
         index < kMaxTrackedProjectiles;
         ++index) {
        ProjectileRecord& record = g_state.projectiles[index];
        if (!record.alive || !record.published ||
            record.public_handle == WOTBMOD_V3_INVALID_HANDLE ||
            now_milliseconds < record.created_tick_milliseconds ||
            now_milliseconds - record.created_tick_milliseconds <
                (record.lifecycle_state ==
                         WOTBMOD_V3_PROJECTILE_STATE_IMPACTED
                     ? 2000u
                     : 10000u)) {
            continue;
        }
        expired[count++] = CaptureRetirementLocked(record);
        record = {};
    }
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    RetireProjectiles(
        expired, count, WOTBMOD_V3_PROJECTILE_DESTROY_TIMEOUT);
}

void RemoveProjectilesForPublicEntity(uint32_t public_entity_id) {
    if (public_entity_id == 0u) return;
    RetiredProjectile removed[kMaxTrackedProjectiles] = {};
    uint32_t count = 0u;
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    for (uint32_t index = 0u;
         index < kMaxTrackedProjectiles;
         ++index) {
        ProjectileRecord& record = g_state.projectiles[index];
        if (!record.alive ||
            record.public_entity_id != public_entity_id) {
            continue;
        }
        if (record.published &&
            record.public_handle != WOTBMOD_V3_INVALID_HANDLE) {
            removed[count++] = CaptureRetirementLocked(record);
        }
        record = {};
    }
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    RetireProjectiles(
        removed,
        count,
        WOTBMOD_V3_PROJECTILE_DESTROY_ENTITY_REMOVED);
}

WotbModV3Result RegisterRpcSubscription(
    WotbModV3Handle owner,
    const wotbmod::v3::ClientHostRpcObserverRequest* request,
    ClientHostObjectResponse* response) {
    if (!request || !response ||
        request->struct_size < sizeof(*request) ||
        request->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
        request->mod != owner ||
        request->direction_mask == 0u ||
        (request->direction_mask &
         ~(WOTBMOD_V3_RPC_INCOMING |
           WOTBMOD_V3_RPC_OUTGOING)) != 0u ||
        (request->method_filter &&
         !IsFixedTextValid(
             request->method_filter,
             WOTBMOD_V3_MAX_NAME))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if ((request->direction_mask & WOTBMOD_V3_RPC_OUTGOING) != 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (InterlockedCompareExchange(
            &g_state.gameplay_bridge_sources, 0, 0) == 0) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const uint64_t token = static_cast<uint64_t>(
        InterlockedIncrement64(
            &g_state.next_rpc_subscription_token));
    RpcSubscriptionRecord subscription = {};
    subscription.token = token ? token : 1u;
    subscription.owner = owner;
    {
        std::lock_guard<std::mutex> lock(g_state.rpc_mutex);
        if (g_state.rpc_subscriptions.size() >=
            kMaxRpcSubscriptions) {
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        try {
            g_state.rpc_subscriptions.push_back(subscription);
        } catch (...) {
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
    }
    response->object = subscription.token;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UnregisterRpcSubscription(
    WotbModV3Handle owner,
    uint64_t token) {
    if (owner == WOTBMOD_V3_INVALID_HANDLE || token == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_state.rpc_mutex);
    for (auto current = g_state.rpc_subscriptions.begin();
         current != g_state.rpc_subscriptions.end();
         ++current) {
        if (current->token != token) continue;
        if (current->owner != owner) {
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        }
        g_state.rpc_subscriptions.erase(current);
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_INVALID_HANDLE;
}

DXGI_FORMAT TextureFormat(uint32_t format) {
    switch (format) {
    case WOTBMOD_V3_TEXTURE_RGBA8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case WOTBMOD_V3_TEXTURE_BGRA8_UNORM:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case WOTBMOD_V3_TEXTURE_R8_UNORM:
        return DXGI_FORMAT_R8_UNORM;
    case WOTBMOD_V3_TEXTURE_RGBA16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

uint32_t BytesPerPixel(uint32_t format) {
    switch (format) {
    case WOTBMOD_V3_TEXTURE_RGBA8_UNORM:
    case WOTBMOD_V3_TEXTURE_BGRA8_UNORM:
        return 4u;
    case WOTBMOD_V3_TEXTURE_R8_UNORM:
        return 1u;
    case WOTBMOD_V3_TEXTURE_RGBA16_FLOAT:
        return 8u;
    default:
        return 0u;
    }
}

WotbModV3Result CreateTexture(
    WotbModV3Handle owner,
    const WotbModV3TextureDescriptor* descriptor,
    ClientHostObjectResponse* response) {
    if (!descriptor || !response) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> render_lock(g_state.render_mutex);
    ID3D11Device* device = g_state.renderer.device;
    if (!device) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const DXGI_FORMAT format = TextureFormat(descriptor->format);
    if (format == DXGI_FORMAT_UNKNOWN) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    NativeTexture* native = new (std::nothrow) NativeTexture();
    if (!native) return WOTBMOD_V3_E_LIMIT_REACHED;
    native->owner = owner;
    native->width = descriptor->width;
    native->height = descriptor->height;
    native->format = descriptor->format;
    D3D11_TEXTURE2D_DESC texture_desc = {};
    texture_desc.Width = descriptor->width;
    texture_desc.Height = descriptor->height;
    texture_desc.MipLevels = 1u;
    texture_desc.ArraySize = 1u;
    texture_desc.Format = format;
    texture_desc.SampleDesc.Count = 1u;
    texture_desc.Usage = descriptor->dynamic
        ? D3D11_USAGE_DYNAMIC
        : D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = descriptor->dynamic
        ? D3D11_CPU_ACCESS_WRITE
        : 0u;
    D3D11_SUBRESOURCE_DATA initial = {};
    initial.pSysMem = descriptor->initial_data;
    initial.SysMemPitch = descriptor->row_pitch
        ? descriptor->row_pitch
        : descriptor->width * BytesPerPixel(descriptor->format);
    const HRESULT create_result = device->CreateTexture2D(
        &texture_desc,
        descriptor->initial_data ? &initial : nullptr,
        &native->texture);
    if (FAILED(create_result) ||
        FAILED(device->CreateShaderResourceView(
            native->texture, nullptr, &native->view))) {
        ReleaseCom(native->view);
        ReleaseCom(native->texture);
        delete native;
        return WOTBMOD_V3_E_PLATFORM;
    }
    {
        std::lock_guard<std::mutex> object_lock(g_state.object_mutex);
        g_state.textures.push_back(native);
    }
    response->object =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(native));
    return WOTBMOD_V3_OK;
}

NativeTexture* GetTexture(uint64_t token, WotbModV3Handle owner) {
    NativeTexture* texture = reinterpret_cast<NativeTexture*>(
        static_cast<uintptr_t>(token));
    if (!texture) return nullptr;
    std::lock_guard<std::mutex> lock(g_state.object_mutex);
    const auto found = std::find(
        g_state.textures.begin(), g_state.textures.end(), texture);
    return found != g_state.textures.end() &&
                   texture->magic == kNativeTextureMagic &&
                   texture->owner == owner
        ? texture
        : nullptr;
}

NativeMaterial* GetMaterial(uint64_t token, WotbModV3Handle owner) {
    NativeMaterial* material = reinterpret_cast<NativeMaterial*>(
        static_cast<uintptr_t>(token));
    if (!material) return nullptr;
    std::lock_guard<std::mutex> lock(g_state.object_mutex);
    const auto found = std::find(
        g_state.materials.begin(), g_state.materials.end(), material);
    return found != g_state.materials.end() &&
                   material->magic == kNativeMaterialMagic &&
                   material->owner == owner
        ? material
        : nullptr;
}

WotbModV3Result DestroyTexture(
    WotbModV3Handle owner,
    uint64_t token) {
    NativeTexture* texture = reinterpret_cast<NativeTexture*>(
        static_cast<uintptr_t>(token));
    if (!texture) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(g_state.object_mutex);
        const auto found = std::find(
            g_state.textures.begin(), g_state.textures.end(), texture);
        if (found == g_state.textures.end() ||
            texture->magic != kNativeTextureMagic ||
            texture->owner != owner) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        g_state.textures.erase(found);
    }
    texture->magic = 0u;
    ReleaseCom(texture->view);
    ReleaseCom(texture->texture);
    delete texture;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UpdateTexture(
    WotbModV3Handle owner,
    const ClientHostObjectRequest& request) {
    NativeTexture* texture = GetTexture(request.object, owner);
    if (!texture || !request.payload || request.payload_size == 0u) {
        return texture
            ? WOTBMOD_V3_E_INVALID_ARGUMENT
            : WOTBMOD_V3_E_INVALID_HANDLE;
    }
    const uint32_t source_pitch =
        static_cast<uint32_t>(request.scalar0);
    const uint32_t x = static_cast<uint32_t>(request.vector.x);
    const uint32_t y = static_cast<uint32_t>(request.vector.y);
    const uint32_t width = static_cast<uint32_t>(request.vector.z);
    const uint32_t height = static_cast<uint32_t>(request.vector.w);
    if (width == 0u || height == 0u ||
        x > texture->width || width > texture->width - x ||
        y > texture->height || height > texture->height - y) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> render_lock(g_state.render_mutex);
    ID3D11DeviceContext* context = g_state.renderer.context;
    if (!context) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const D3D11_BOX box = {
        x, y, 0u, x + width, y + height, 1u};
    context->UpdateSubresource(
        texture->texture,
        0u,
        request.selector ? &box : nullptr,
        request.payload,
        source_pitch,
        0u);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CreateMaterial(
    WotbModV3Handle owner,
    const WotbModV3MaterialDescriptor* descriptor,
    ClientHostObjectResponse* response) {
    if (!descriptor || !response) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    NativeMaterial* material = new (std::nothrow) NativeMaterial();
    if (!material) return WOTBMOD_V3_E_LIMIT_REACHED;
    material->owner = owner;
    material->blend = descriptor->blend_enabled != 0u;
    material->depth = descriptor->depth_test_enabled != 0u;
    {
        std::lock_guard<std::mutex> lock(g_state.object_mutex);
        g_state.materials.push_back(material);
    }
    response->object =
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(material));
    return WOTBMOD_V3_OK;
}

WotbModV3Result SetMaterialParameter(
    WotbModV3Handle owner,
    const ClientHostObjectRequest& request) {
    NativeMaterial* material = GetMaterial(request.object, owner);
    if (!material) return WOTBMOD_V3_E_INVALID_HANDLE;
    const WotbModV3RenderParameter* parameter =
        static_cast<const WotbModV3RenderParameter*>(request.payload);
    if (!parameter ||
        request.payload_size < sizeof(*parameter)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (parameter->type == WOTBMOD_V3_RENDER_PARAMETER_COLOR) {
        material->color = parameter->value.color;
        return WOTBMOD_V3_OK;
    }
    if (parameter->type == WOTBMOD_V3_RENDER_PARAMETER_VEC4) {
        material->color = {
            parameter->value.vec4.x,
            parameter->value.vec4.y,
            parameter->value.vec4.z,
            parameter->value.vec4.w};
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result DestroyMaterial(
    WotbModV3Handle owner,
    uint64_t token) {
    NativeMaterial* material = reinterpret_cast<NativeMaterial*>(
        static_cast<uintptr_t>(token));
    if (!material) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    {
        std::lock_guard<std::mutex> lock(g_state.object_mutex);
        const auto found = std::find(
            g_state.materials.begin(), g_state.materials.end(), material);
        if (found == g_state.materials.end() ||
            material->magic != kNativeMaterialMagic ||
            material->owner != owner) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        g_state.materials.erase(found);
    }
    material->magic = 0u;
    delete material;
    return WOTBMOD_V3_OK;
}

bool ValidHostRequest(
    const void* request,
    uint32_t request_size,
    const ClientHostObjectRequest** out_request) {
    if (!request || request_size < sizeof(ClientHostObjectRequest) ||
        !out_request) {
        return false;
    }
    const ClientHostObjectRequest* typed =
        static_cast<const ClientHostObjectRequest*>(request);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *out_request = typed;
    return true;
}

bool ValidHostResponse(
    void* response,
    uint32_t response_size,
    ClientHostObjectResponse** out_response) {
    if (!response ||
        response_size < sizeof(ClientHostObjectResponse) ||
        !out_response) {
        return false;
    }
    ClientHostObjectResponse* typed =
        static_cast<ClientHostObjectResponse*>(response);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *out_response = typed;
    return true;
}

WotbModV3Result InvokeCamera(
    WotbModV3Handle mod,
    const char* operation,
    const ClientHostObjectRequest* request,
    ClientHostObjectResponse* response) {
    void* camera = InterlockedCompareExchangePointer(
        &g_state.camera, nullptr, nullptr);
    if (!ValidateCamera(camera)) {
        InterlockedCompareExchangePointer(
            &g_state.camera, nullptr, camera);
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    if (std::strcmp(operation, "camera_get_active") == 0) {
        if (!response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
        response->object =
            static_cast<uint64_t>(reinterpret_cast<uintptr_t>(camera));
        return WOTBMOD_V3_OK;
    }
    if (!request ||
        (request->object != 0u &&
         request->object != static_cast<uint64_t>(
             reinterpret_cast<uintptr_t>(camera)))) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    if (std::strcmp(operation, "camera_get_mode") == 0) {
        if (!response) return WOTBMOD_V3_E_INVALID_ARGUMENT;
        const LONG mode = InterlockedCompareExchange(
            &g_state.camera_mode,
            WOTBMOD_V3_CAMERA_MODE_UNKNOWN,
            WOTBMOD_V3_CAMERA_MODE_UNKNOWN);
        if (mode == WOTBMOD_V3_CAMERA_MODE_UNKNOWN) {
            return WOTBMOD_V3_E_NOT_FOUND;
        }
        response->value_u32 = static_cast<uint32_t>(mode);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_get_transform") == 0) {
        return response &&
                       ReadCameraTransform(camera, &response->transform)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_PLATFORM;
    }
    if (std::strcmp(operation, "camera_set_transform") == 0) {
        return WriteCameraTransform(camera, request->transform)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_PLATFORM;
    }
    if (std::strcmp(operation, "camera_get_fov") == 0) {
        float fov = 0.0f;
        if (!response ||
            !SafeRead(
                static_cast<uint8_t*>(camera) + 0x268u,
                &fov,
                sizeof(fov))) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        response->value_f64 = static_cast<double>(fov);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_set_fov") == 0) {
        CameraSetFovFn setter =
            reinterpret_cast<CameraSetFovFn>(
                AddressFromRva(kGameCameraSetFovRva));
        if (!setter || request->scalar0 < 1.0 ||
            request->scalar0 > 140.0) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        __try {
            setter(camera, static_cast<float>(request->scalar0));
            return WOTBMOD_V3_OK;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return WOTBMOD_V3_E_PLATFORM;
        }
    }
    if (std::strcmp(operation, "camera_get_near_plane") == 0 ||
        std::strcmp(operation, "camera_get_far_plane") == 0) {
        float value = 0.0f;
        const size_t offset =
            wotbmod::v3::native_layout::CameraProjectionPlaneOffset(
                operation[11] == 'n');
        if (!response ||
            !SafeRead(
                static_cast<uint8_t*>(camera) + offset,
                &value,
                sizeof(value))) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        response->value_f64 = static_cast<double>(value);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_world_to_screen") == 0) {
        WotbModV3Vec3 output = {};
        const WotbModV3Vec3 input = {
            request->vector.x,
            request->vector.y,
            request->vector.z};
        if (!response ||
            !CameraWorldToScreen(camera, input, &output)) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        response->vector = {
            output.x, output.y, output.z, 1.0f};
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_screen_to_world") == 0) {
        WotbModV3Vec3 output = {};
        const WotbModV3Vec3 input = {
            request->vector.x,
            request->vector.y,
            request->vector.z};
        if (!response ||
            !CameraScreenToWorld(camera, input, &output)) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        response->vector = {
            output.x, output.y, output.z, 1.0f};
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_transition_to") == 0) {
        if (!response || !request->payload ||
            request->payload_size <
                sizeof(WotbModV3CameraTransition)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        uint64_t controller = 0u;
        const WotbModV3Result result =
            g_state.camera_effects.QueueTransition(
                mod,
                static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(camera)),
                *static_cast<const WotbModV3CameraTransition*>(
                    request->payload),
                &controller);
        if (result == WOTBMOD_V3_OK) {
            response->object = controller;
        }
        return result;
    }
    if (std::strcmp(operation, "camera_add_shake") == 0) {
        if (!response || !request->payload ||
            request->payload_size <
                sizeof(WotbModV3CameraShake)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        uint64_t controller = 0u;
        const WotbModV3Result result =
            g_state.camera_effects.QueueShake(
                mod,
                static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(camera)),
                *static_cast<const WotbModV3CameraShake*>(
                    request->payload),
                &controller);
        if (result == WOTBMOD_V3_OK) {
            response->object = controller;
        }
        return result;
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result WOTBMOD_V3_CALL NativeInvoke(
    void*,
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size) {
    if (!operation || !operation[0]) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    const WotbModV3Result service_result =
        wotbmod::loader::InvokeV3NativeClientServices(
            mod,
            operation,
            request,
            request_size,
            response,
            response_size);
    if (service_result != WOTBMOD_V3_E_NOT_SUPPORTED) {
        return service_result;
    }

    const bool fixed_binding_operation =
        std::strncmp(operation, "camera_", 7u) == 0 ||
        std::strncmp(operation, "entity_", 7u) == 0 ||
        std::strncmp(operation, "rpc_", 4u) == 0 ||
        std::strncmp(operation, "projectile_", 11u) == 0 ||
        std::strncmp(operation, "tracer_", 7u) == 0 ||
        std::strcmp(operation, "client_leave_to_hangar") == 0;
    if (fixed_binding_operation &&
        g_state.compatibility ==
            WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }

    if (std::strcmp(operation, "camera_add_modifier") == 0) {
        if (!request ||
            request_size <
                sizeof(wotbmod::v3::ClientHostCameraModifierRequest) ||
            !response ||
            response_size < sizeof(ClientHostObjectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const wotbmod::v3::ClientHostCameraModifierRequest* modifier =
            static_cast<
                const wotbmod::v3::ClientHostCameraModifierRequest*>(
                request);
        ClientHostObjectResponse* modifier_response =
            static_cast<ClientHostObjectResponse*>(response);
        if (modifier->struct_size < sizeof(*modifier) ||
            modifier->api_version !=
                WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
            modifier->phase !=
                WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME ||
            modifier_response->struct_size <
                sizeof(*modifier_response) ||
            modifier_response->api_version !=
                WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
            return modifier->phase ==
                           WOTBMOD_V3_CAMERA_MODIFIER_BEFORE_GAME
                ? WOTBMOD_V3_E_NOT_SUPPORTED
                : WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const uint64_t token = static_cast<uint64_t>(
            InterlockedIncrement64(
                &g_state.next_camera_modifier_token));
        modifier_response->object = token ? token : 1u;
        return WOTBMOD_V3_OK;
    }

    if (std::strcmp(operation, "rpc_subscribe_observed") == 0) {
        if (!request ||
            request_size <
                sizeof(
                    wotbmod::v3::ClientHostRpcObserverRequest) ||
            !response ||
            response_size < sizeof(ClientHostObjectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ClientHostObjectResponse* rpc_response =
            static_cast<ClientHostObjectResponse*>(response);
        if (rpc_response->struct_size < sizeof(*rpc_response) ||
            rpc_response->api_version !=
                WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return RegisterRpcSubscription(
            mod,
            static_cast<
                const wotbmod::v3::ClientHostRpcObserverRequest*>(
                request),
            rpc_response);
    }

    if (std::strcmp(operation, "entity_enumerate_visible") == 0) {
        if (!request ||
            request_size <
                sizeof(wotbmod::v3::ClientHostEntityVisitorRequest)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const wotbmod::v3::ClientHostEntityVisitorRequest* enumeration =
            static_cast<
                const wotbmod::v3::ClientHostEntityVisitorRequest*>(
                request);
        if (enumeration->struct_size < sizeof(*enumeration) ||
            enumeration->api_version !=
                WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
            !enumeration->visitor) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        // client_services already visits every registered visible snapshot
        // before invoking this fallback. Reaching the native backend therefore
        // means the authoritative registry is valid and currently empty.
        return WOTBMOD_V3_OK;
    }

    const ClientHostObjectRequest* typed_request = nullptr;
    ClientHostObjectResponse* typed_response = nullptr;
    if (request &&
        !ValidHostRequest(request, request_size, &typed_request)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (response &&
        !ValidHostResponse(
            response, response_size, &typed_response)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    if (std::strncmp(operation, "render_", 7u) == 0) {
        return wotbmod::loader::ManagedRendererInvoke(
            mod,
            operation,
            typed_request,
            typed_response);
    }

    if (std::strncmp(operation, "camera_", 7u) == 0) {
        if (std::strcmp(operation, "camera_remove_modifier") == 0) {
            return typed_request && typed_request->object != 0u
                ? WOTBMOD_V3_OK
                : WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        if (std::strcmp(operation, "camera_cancel_effects") == 0) {
            return typed_request
                ? g_state.camera_effects.Cancel(
                      mod, typed_request->object)
                : WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return InvokeCamera(
            mod, operation, typed_request, typed_response);
    }
    if (std::strcmp(operation, "rpc_unsubscribe_observed") == 0) {
        return typed_request
            ? UnregisterRpcSubscription(
                  mod, typed_request->object)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "client_leave_to_hangar") == 0) {
        void* owner = InterlockedCompareExchangePointer(
            &g_state.client_owner, nullptr, nullptr);
        LeaveToHangarFn leave =
            reinterpret_cast<LeaveToHangarFn>(
                AddressFromRva(kLeaveToHangarRva));
        void* controller = nullptr;
        void* vtable = nullptr;
        void* method = nullptr;
        if (!owner || !leave ||
            !SafeRead(
                static_cast<uint8_t*>(owner) + 0x120u,
                &controller,
                sizeof(controller)) ||
            !controller ||
            !SafeRead(controller, &vtable, sizeof(vtable)) ||
            !vtable ||
            !SafeRead(
                static_cast<uint8_t*>(vtable) + 0x16Cu,
                &method,
                sizeof(method)) ||
            !IsExecutableAddress(method)) {
            return WOTBMOD_V3_E_NOT_FOUND;
        }
        __try {
            leave(owner, nullptr);
            return WOTBMOD_V3_OK;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return WOTBMOD_V3_E_PLATFORM;
        }
    }
    if (std::strcmp(operation, "render_create_texture") == 0) {
        if (!typed_request || !typed_response ||
            !typed_request->payload ||
            typed_request->payload_size <
                sizeof(WotbModV3TextureDescriptor)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return CreateTexture(
            mod,
            static_cast<const WotbModV3TextureDescriptor*>(
                typed_request->payload),
            typed_response);
    }
    if (std::strcmp(operation, "render_update_texture") == 0) {
        return typed_request
            ? UpdateTexture(mod, *typed_request)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "render_destroy_texture") == 0) {
        return typed_request
            ? DestroyTexture(mod, typed_request->object)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "render_create_material") == 0) {
        if (!typed_request || !typed_response ||
            !typed_request->payload ||
            typed_request->payload_size <
                sizeof(WotbModV3MaterialDescriptor)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return CreateMaterial(
            mod,
            static_cast<const WotbModV3MaterialDescriptor*>(
                typed_request->payload),
            typed_response);
    }
    if (std::strcmp(
            operation, "render_set_material_parameter") == 0) {
        return typed_request
            ? SetMaterialParameter(mod, *typed_request)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "render_destroy_material") == 0) {
        return typed_request
            ? DestroyMaterial(mod, typed_request->object)
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result PublishEntityLifecycle(
    const char* topic,
    uint32_t reason,
    const WotbModV3PublicEntitySnapshot& snapshot) {
    WotbModV3PublicEntityLifecycleEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_ENTITY_PUBLIC_VERSION;
    event.reason = reason;
    event.snapshot = snapshot;
    return wotbmod::v3::PublishClientHostEvent(
        topic, &event, sizeof(event));
}

void DestroyRendererLocked() {
    ReleaseCom(g_state.renderer.rasterizer);
    ReleaseCom(g_state.renderer.depth_off);
    ReleaseCom(g_state.renderer.blend_state);
    ReleaseCom(g_state.renderer.sampler);
    ReleaseCom(g_state.renderer.vertex_buffer);
    ReleaseCom(g_state.renderer.input_layout);
    ReleaseCom(g_state.renderer.color_shader);
    ReleaseCom(g_state.renderer.texture_shader);
    ReleaseCom(g_state.renderer.vertex_shader);
    g_state.renderer.initialized = false;
}

/*
 * Client fingerprint.
 *
 * Every fixed-RVA binding in this loader is generated for one exact client
 * build, so the digest has to be known before the first detour is installed,
 * not after. The loader calls VerifyClientFingerprint() up front and gates
 * the DAVA sound/resource backends, the Scene trackers and the file resolver
 * on it; Create() then reuses this cached answer instead of hashing the
 * ~70 MB executable a second time.
 *
 * The cache is never invalidated: the running executable cannot change under
 * us, so one digest per process is both correct and the cheapest option.
 */
struct FingerprintState {
    bool computed = false;
    bool matches = false;
    std::string hex;
};

FingerprintState g_fingerprint;

/*
 * OK only on an exact match. E_IO and E_PLATFORM mean the digest could not be
 * established at all, which every caller treats exactly like a mismatch - an
 * executable we cannot hash never enables a native backend.
 */
WotbModV3Result ComputeClientFingerprint(
    HMODULE game_module,
    const char* game_executable_path,
    WotbModV3NativeBindingsLog log,
    void* log_user_data) {
    if (g_fingerprint.computed) {
        return g_fingerprint.matches
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    char executable_path[MAX_PATH] = {};
    const char* path = game_executable_path;
    if (!path || !path[0]) {
        const DWORD length = GetModuleFileNameA(
            game_module ? game_module : GetModuleHandleA(nullptr),
            executable_path,
            static_cast<DWORD>(sizeof(executable_path)));
        if (length == 0u || length >= sizeof(executable_path)) {
            if (log) {
                log("V3 binding pack disabled: executable path unavailable",
                    log_user_data);
            }
            return WOTBMOD_V3_E_PLATFORM;
        }
        path = executable_path;
    }
    uint8_t digest[32] = {};
    if (!HashFileSha256(path, digest)) {
        if (log) {
            log("V3 binding pack disabled: wotblitz.exe SHA-256 unreadable",
                log_user_data);
        }
        return WOTBMOD_V3_E_IO;
    }
    static const char kDigestHex[] = "0123456789abcdef";
    g_fingerprint.hex.resize(64u);
    for (size_t index = 0u; index < sizeof(digest); ++index) {
        g_fingerprint.hex[index * 2u] =
            kDigestHex[(digest[index] >> 4u) & 0x0fu];
        g_fingerprint.hex[index * 2u + 1u] =
            kDigestHex[digest[index] & 0x0fu];
    }
    g_fingerprint.matches =
        std::memcmp(
            digest,
            kExpectedExecutableSha256,
            sizeof(digest)) == 0;
    g_fingerprint.computed = true;
    if (!g_fingerprint.matches && log) {
        log("V3 binding pack disabled: wotblitz.exe SHA-256 mismatch",
            log_user_data);
    }
    return g_fingerprint.matches
        ? WOTBMOD_V3_OK
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

}  // namespace

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_VerifyClientFingerprint(
    HMODULE game_module,
    const char* game_executable_path,
    WotbModV3NativeBindingsLog log,
    void* log_user_data) {
    return ComputeClientFingerprint(
        game_module,
        game_executable_path,
        log,
        log_user_data);
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_CreateStockTracer(
    void*,
    const WotbModDavaNativeTracerRequest* request) {
    if (!request || request->struct_size < sizeof(*request) ||
        request->flags != 0u || request->reserved != 0u ||
        request->shell_type >= g_tracer_shell_type_count ||
        !std::isfinite(request->width) || request->width <= 0.0f ||
        !std::isfinite(request->lifetime_seconds) ||
        request->lifetime_seconds <= 0.0f) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (float value : request->origin) {
        if (!std::isfinite(value)) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (float value : request->destination) {
        if (!std::isfinite(value)) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (float value : request->color) {
        if (!std::isfinite(value)) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const float dx = request->destination[0] - request->origin[0];
    const float dy = request->destination[1] - request->origin[1];
    const float dz = request->destination[2] - request->origin[2];
    if (!(dx * dx + dy * dy + dz * dz > 0.000001f)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_state.created || !g_state.stock_tracer_ready) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const LONG mainThread = InterlockedCompareExchange(
        &g_state.main_thread_id, 0, 0);
    if (mainThread == 0 ||
        static_cast<DWORD>(mainThread) != GetCurrentThreadId()) {
        return WOTBMOD_V3_E_WRONG_THREAD;
    }

    void* manager = InterlockedCompareExchangePointer(
        &g_state.tracer_manager, nullptr, nullptr);
    if (!manager) return WOTBMOD_V3_E_NOT_FOUND;
    void* vtable = nullptr;
    void* poolBegin = nullptr;
    void* poolEnd = nullptr;
    if (!SafeRead(manager, &vtable, sizeof(vtable)) ||
        vtable != AddressFromRva(kTracerManagerVtableRva) ||
        !SafeRead(
            static_cast<const uint8_t*>(manager) + 0x10u,
            &poolBegin,
            sizeof(poolBegin)) ||
        !SafeRead(
            static_cast<const uint8_t*>(manager) + 0x14u,
            &poolEnd,
            sizeof(poolEnd)) ||
        !poolBegin || !poolEnd ||
        reinterpret_cast<uintptr_t>(poolEnd) <=
            reinterpret_cast<uintptr_t>(poolBegin)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }

    TracerManagerShowTracerFn showTracer =
        reinterpret_cast<TracerManagerShowTracerFn>(
            AddressFromRva(kTracerManagerShowTracerRva));
    if (!IsExecutableAddress(reinterpret_cast<const void*>(showTracer))) {
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    uint32_t sourceId = static_cast<uint32_t>(InterlockedIncrement(
        &g_state.next_stock_tracer_id));
    if (sourceId == 0u) {
        sourceId = static_cast<uint32_t>(InterlockedIncrement(
            &g_state.next_stock_tracer_id));
    }
    const uint32_t destinationId = sourceId ^ 0x80000000u;
    const uint8_t shellType = static_cast<uint8_t>(request->shell_type);
    const float stockParameter = 0.0f;
    /*
     * Drawing through the original bypasses our own detour, and the flag covers
     * the case where the hook is not installed and showTracer is still the raw
     * address. Either way a tracer the caller asked for must not be attributed
     * to somebody's shot.
     */
    if (g_original_tracer_show) {
        showTracer = g_original_tracer_show;
    }
    InterlockedIncrement(&g_state.self_issued_tracer);
    __try {
        showTracer(
            manager,
            request->origin,
            &sourceId,
            request->destination,
            &destinationId,
            &shellType,
            &stockParameter,
            1u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedDecrement(&g_state.self_issued_tracer);
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
    InterlockedDecrement(&g_state.self_issued_tracer);
    return WOTBMOD_V3_OK;
}

extern "C" uint32_t WOTBMOD_CALL
WotbModV3NativeBindings_IsMainThread(void*) {
    const LONG mainThread = InterlockedCompareExchange(
        &g_state.main_thread_id, 0, 0);
    return mainThread != 0 &&
            static_cast<DWORD>(mainThread) == GetCurrentThreadId()
        ? 1u
        : 0u;
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_InvokeMainThread(
    void*,
    WotbModDavaMainThreadCallFn call,
    void* callData) {
    if (!call) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (WotbModV3NativeBindings_IsMainThread(nullptr) != 0u) {
        return call(callData);
    }
    return wotbmod::v3::InvokeMainThreadBlocking(
        call, callData, kDavaMainThreadCallTimeoutMs);
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_Create(
    const WotbModV3NativeBindingsOptions* options,
    WotbModRuntimeV3ClientBackend* out_backend) {
    if (!options ||
        options->struct_size < sizeof(*options) ||
        !out_backend || g_state.created) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    g_state.game_module = options->game_module
        ? options->game_module
        : GetModuleHandleA(nullptr);
    g_state.log = options->log;
    g_state.log_user_data = options->log_user_data;
    g_state.avatar_observed = options->avatar_observed;
    g_state.avatar_observed_user_data =
        options->avatar_observed_user_data;
    g_state.resource_identity = options->resource_identity;
    g_state.resource_identity_user_data =
        options->resource_identity_user_data;
    g_state.resource_bring_ui_to_front =
        options->resource_bring_ui_to_front;
    g_state.resource_bring_ui_to_front_user_data =
        options->resource_bring_ui_to_front_user_data;
    g_state.binding_report_path =
        options->binding_validation_report_path
        ? options->binding_validation_report_path
        : "";
    g_state.binding_validation.clear();
    g_state.executable_sha256.clear();
    if (!ResolveImage(
            g_state.game_module,
            &g_state.game_base,
            &g_state.image_size)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (options->resource_backend) {
        const size_t size = std::min(
            static_cast<size_t>(
                options->resource_backend->struct_size),
            sizeof(g_state.resource));
        std::memcpy(
            &g_state.resource, options->resource_backend, size);
    }
    if (options->audio_backend) {
        const size_t size = std::min(
            static_cast<size_t>(
                options->audio_backend->struct_size),
            sizeof(g_state.audio));
        std::memcpy(&g_state.audio, options->audio_backend, size);
    }
    if (options->sound_backend) {
        const size_t size = std::min(
            static_cast<size_t>(
                options->sound_backend->struct_size),
            sizeof(g_state.sound));
        std::memcpy(&g_state.sound, options->sound_backend, size);
    }
    if (options->gameplay_backend) {
        const size_t size = std::min(
            static_cast<size_t>(
                options->gameplay_backend->struct_size),
            sizeof(g_state.gameplay));
        std::memcpy(
            &g_state.gameplay, options->gameplay_backend, size);
    }

    /*
     * Normally already computed and cached by the loader's pre-hook gate;
     * recomputed here only when Create() is called directly (host tests).
     * Keeping the check inside Create() as well means the pack stays
     * fail-closed even for a caller that skipped the gate.
     */
    const WotbModV3Result fingerprint = ComputeClientFingerprint(
        g_state.game_module,
        options->game_executable_path,
        options->log,
        options->log_user_data);
    if (fingerprint == WOTBMOD_V3_E_PLATFORM) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (fingerprint == WOTBMOD_V3_E_IO) {
        return WOTBMOD_V3_E_IO;
    }
    g_state.executable_sha256 = g_fingerprint.hex;
    if (fingerprint != WOTBMOD_V3_OK) {
        g_state.compatibility =
            WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH;
        AddAllBindingsUnchecked();
        Log("V3 binding pack disabled: wotblitz.exe SHA-256 mismatch");
    } else {
        const uint8_t standard_prologue[] = {
            0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu};
        const bool camera_ctor = InstallHook(
            kGameCameraCtorRva,
            standard_prologue,
            sizeof(standard_prologue),
            reinterpret_cast<void*>(&CameraCtorDetour),
            reinterpret_cast<void**>(&g_original_camera_ctor),
            "GameCamera ctor binding signature mismatch");
        if (camera_ctor) {
            hobs::RegisterTarget(AddressFromRva(kGameCameraCtorRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "GameCamera::ctor", kGameCameraCtorRva,
            camera_ctor, "hook");
        const uint8_t camera_dtor_prologue[] = {
            0x55u, 0x8Bu, 0xECu, 0x56u, 0x8Bu, 0xF1u};
        const bool camera_dtor = InstallHook(
            kGameCameraDtorRva,
            camera_dtor_prologue,
            sizeof(camera_dtor_prologue),
            reinterpret_cast<void*>(&CameraDtorDetour),
            reinterpret_cast<void**>(&g_original_camera_dtor),
            "GameCamera dtor binding signature mismatch");
        if (camera_dtor) {
            hobs::RegisterTarget(AddressFromRva(kGameCameraDtorRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "GameCamera::dtor", kGameCameraDtorRva,
            camera_dtor, "hook");
        const bool client_owner = InstallHook(
            kClientInitializeRva,
            standard_prologue,
            sizeof(standard_prologue),
            reinterpret_cast<void*>(&ClientInitializeDetour),
            reinterpret_cast<void**>(&g_original_client_initialize),
            "client action owner binding signature mismatch");
        if (client_owner) {
            hobs::RegisterTarget(AddressFromRva(kClientInitializeRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "Client::Initialize", kClientInitializeRva,
            client_owner, "hook");
        const bool entity_ctor = InstallHook(
            kBwEntityCtorRva,
            standard_prologue,
            sizeof(standard_prologue),
            reinterpret_cast<void*>(&BwEntityCtorDetour),
            reinterpret_cast<void**>(&g_original_entity_ctor),
            "BWEntity ctor binding signature mismatch");
        if (entity_ctor) {
            hobs::RegisterTarget(AddressFromRva(kBwEntityCtorRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "BWEntity::ctor", kBwEntityCtorRva,
            entity_ctor, "hook");
        const bool entity_dtor = InstallHook(
            kBwEntityDtorRva,
            standard_prologue,
            sizeof(standard_prologue),
            reinterpret_cast<void*>(&BwEntityDtorDetour),
            reinterpret_cast<void**>(&g_original_entity_dtor),
            "BWEntity dtor binding signature mismatch");
        if (entity_dtor) {
            hobs::RegisterTarget(AddressFromRva(kBwEntityDtorRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "BWEntity::dtor", kBwEntityDtorRva,
            entity_dtor, "hook");
        const bool tracer = InstallHook(
            kTracerManagerCtorRva,
            standard_prologue,
            sizeof(standard_prologue),
            reinterpret_cast<void*>(&TracerManagerCtorDetour),
            reinterpret_cast<void**>(&g_original_tracer_ctor),
            "TracerManager binding signature mismatch");
        if (tracer) {
            hobs::RegisterTarget(AddressFromRva(kTracerManagerCtorRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "TracerManager::ctor", kTracerManagerCtorRva,
            tracer, "hook");
        const bool tracerDtor = InstallHook(
            kTracerManagerDtorRva,
            camera_dtor_prologue,
            sizeof(camera_dtor_prologue),
            reinterpret_cast<void*>(&TracerManagerDtorDetour),
            reinterpret_cast<void**>(&g_original_tracer_dtor),
            "TracerManager dtor binding signature mismatch");
        if (tracerDtor) {
            hobs::RegisterTarget(AddressFromRva(kTracerManagerDtorRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "TracerManager::dtor", kTracerManagerDtorRva,
            tracerDtor, "hook");
        /*
         * ShowTracer is both called (CreateStockTracer drives it) and hooked
         * (it is the only place a visible tracer is ever announced). InstallHook
         * re-checks the prologue itself, so the byte guard that used to be the
         * whole binding is now the gate in front of the hook.
         */
        const bool tracerShow = InstallHook(
            kTracerManagerShowTracerRva,
            standard_prologue,
            sizeof(standard_prologue),
            reinterpret_cast<void*>(&TracerManagerShowTracerDetour),
            reinterpret_cast<void**>(&g_original_tracer_show),
            "TracerManager ShowTracer binding signature mismatch");
        if (tracerShow) {
            hobs::RegisterTarget(AddressFromRva(kTracerManagerShowTracerRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "TracerManager::ShowTracer",
            kTracerManagerShowTracerRva,
            tracerShow,
            "hook");
        g_state.tracer_producer_ready = tracerShow;
        const uint8_t camera_mode_prologue[] = {
            0x55u, 0x8Bu, 0xECu, 0x8Bu, 0x45u, 0x08u};
        const bool camera_mode = InstallHook(
            kCameraModeChangedRva,
            camera_mode_prologue,
            sizeof(camera_mode_prologue),
            reinterpret_cast<void*>(&CameraModeChangedDetour),
            reinterpret_cast<void**>(&g_original_camera_mode_changed),
            "CameraModeChanged binding signature mismatch");
        if (camera_mode) {
            hobs::RegisterTarget(AddressFromRva(kCameraModeChangedRva), hobs::kModeObserve | hobs::kModeAfter);
        }
        LogBindingStatus(
            "CameraModeChanged", kCameraModeChangedRva,
            camera_mode, "hook");
        /*
         * The five interfaces declared 2026-08-16. Every one of them is
         * OPTIONAL: a failure here leaves that interface UNAVAILABLE and
         * changes nothing about the compatibility verdict below, so a new
         * surface can never downgrade a client that was SUPPORTED before it
         * existed.
         */
        const bool switch_state =
            !HookDisabled("camera_switch_state") &&
            InstallHook(
                kCameraControllerSwitchStateRva,
                standard_prologue,
                sizeof(standard_prologue),
                reinterpret_cast<void*>(&CameraSwitchStateDetour),
                reinterpret_cast<void**>(
                    &g_original_camera_switch_state),
                "CameraController::SwitchState binding signature mismatch");
        const bool main_thread_pump =
            !HookDisabled("frame_pump") &&
            InstallHook(
                kEngineUpdateAndDrawWindowsRva,
                standard_prologue,
                sizeof(standard_prologue),
                reinterpret_cast<void*>(
                    &EngineUpdateAndDrawWindowsDetour),
                reinterpret_cast<void**>(
                    &g_original_update_and_draw_windows),
                "EngineBackend::UpdateAndDrawWindows binding signature "
                "mismatch");
        /*
         * MAIN async ingress comes online only if the per-frame hook actually
         * installed. Declaring it online without the hook would let
         * dispatch_to_main_thread accept work that nothing would ever run -
         * the exact silent-stall the runtime's offline default exists to
         * prevent - so this is gated on the install result, not attempted.
         */
        wotbmod::v3::SetMainIngressOnline(main_thread_pump);
        g_tracer_shell_type_count = ValidateTracerStyleTable();
        g_state.stock_tracer_ready =
            tracer && tracerDtor && tracerShow &&
            g_tracer_shell_type_count != 0u;
        const uint8_t sound_stub_prologue[] = {
            0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu, 0x68u};
        RecordDeclaredInterfaceBindings(
            true,
            switch_state,
            main_thread_pump,
            g_tracer_shell_type_count != 0u,
            MatchesBytes(
                AddressFromRva(kSoundEventStubFactoryRva),
                sound_stub_prologue,
                sizeof(sound_stub_prologue)));
        uint32_t installed_source_mask = 0u;
        if (camera_ctor) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CAMERA_CTOR;
        }
        if (camera_dtor) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CAMERA_DTOR;
        }
        if (client_owner) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CLIENT_INITIALIZE;
        }
        if (entity_ctor) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_ENTITY_CTOR;
        }
        if (entity_dtor) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_ENTITY_DTOR;
        }
        if (tracer) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_TRACER_MANAGER_CTOR;
        }
        if (tracerShow) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_TRACER_SHOW;
        }
        if (camera_mode) {
            installed_source_mask |=
                WOTBMOD_V3_NATIVE_INSTALLED_SOURCE_CAMERA_MODE;
        }
        InterlockedExchange(
            &g_state.installed_source_mask,
            static_cast<LONG>(installed_source_mask));
        const uint8_t set_fov_prologue[] = {
            0x55u, 0x8Bu, 0xECu, 0xF3u, 0x0Fu, 0x10u};
        const uint8_t leave_prologue[] = {
            0x57u, 0x8Bu, 0xF9u, 0xB8u};
        const bool set_fov = MatchesBytes(
            AddressFromRva(kGameCameraSetFovRva),
            set_fov_prologue,
            sizeof(set_fov_prologue));
        const bool leave_to_hangar = MatchesBytes(
            AddressFromRva(kLeaveToHangarRva),
            leave_prologue,
            sizeof(leave_prologue));
        LogBindingStatus(
            "DAVA::Camera::SetFovY", kGameCameraSetFovRva,
            set_fov, "direct-call");
        LogBindingStatus(
            "Client::LeaveToHangar", kLeaveToHangarRva,
            leave_to_hangar, "direct-call");
        LogBindingStatus(
            "GameCamera::vtable", kGameCameraVtableRva,
            IsReadableAddress(AddressFromRva(kGameCameraVtableRva)),
            "identity", false);
        LogBindingStatus(
            "TracerManager::vtable", kTracerManagerVtableRva,
            IsReadableAddress(AddressFromRva(kTracerManagerVtableRva)),
            "identity", false);
        const bool direct_functions = set_fov && leave_to_hangar;
        g_state.reviewed_hook_symbols_ready = direct_functions;
        g_state.compatibility =
            camera_ctor && camera_dtor && client_owner &&
                    entity_ctor && entity_dtor && tracer && tracerDtor &&
                    camera_mode && direct_functions
                ? WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED
                : WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED;
    }
    WriteBindingValidationReport();
    g_state.created = true;
    wotbmod::loader::V3NativeClientServicesOptions
        client_services_options = {};
    client_services_options.struct_size =
        sizeof(client_services_options);
    client_services_options.resource_backend = &g_state.resource;
    client_services_options.audio_backend = &g_state.audio;
    client_services_options.sound_backend = &g_state.sound;
    client_services_options.resource_identity =
        g_state.resource_identity;
    client_services_options.resource_identity_user_data =
        g_state.resource_identity_user_data;
    client_services_options.resource_bring_ui_to_front =
        g_state.resource_bring_ui_to_front;
    client_services_options.resource_bring_ui_to_front_user_data =
        g_state.resource_bring_ui_to_front_user_data;
    client_services_options.game_base = g_state.game_base;
    client_services_options.game_image_size = g_state.image_size;
    client_services_options.compatibility_state = g_state.compatibility;
    const WotbModV3Result client_services_result =
        wotbmod::loader::InitializeV3NativeClientServices(
            &client_services_options);
    if (client_services_result != WOTBMOD_V3_OK) {
        WotbModV3NativeBindings_Shutdown();
        return client_services_result;
    }
    const WotbModV3Result renderer_result =
        wotbmod::loader::ManagedRendererCreate();
    if (renderer_result != WOTBMOD_V3_OK) {
        WotbModV3NativeBindings_Shutdown();
        return renderer_result;
    }

    std::memset(out_backend, 0, sizeof(*out_backend));
    out_backend->struct_size = sizeof(*out_backend);
    out_backend->api_version =
        WOTBMOD_RUNTIME_V3_CLIENT_BACKEND_VERSION;
    out_backend->binding_pack_version = kBindingPackVersion;
    out_backend->compatibility_state = g_state.compatibility;
    out_backend->invoke = &NativeInvoke;
    std::memcpy(
        out_backend->client_build,
        kClientBuild,
        sizeof(kClientBuild));
    std::memcpy(
        out_backend->client_executable_sha256,
        g_state.executable_sha256.c_str(), 65u);
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_InstallDeclaredBackend(void) {
    if (!g_state.created) return WOTBMOD_V3_E_NOT_SUPPORTED;
    /*
     * The exact-fingerprint gate, restated on this side so the table is never
     * even offered on a build it was not generated for. The runtime refuses
     * it again - there is no per-slot override and no degraded install.
     */
    if (g_state.compatibility !=
            WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED &&
        g_state.compatibility !=
            WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED) {
        Log("declared client backend withheld: client fingerprint not "
            "proven");
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    static wotbmod::v3::ClientHostDeclaredBackend backend = {};
    backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version =
        WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
    backend.binding_pack_version = kBindingPackVersion;
    backend.compatibility_state = g_state.compatibility;
    backend.user_data = nullptr;

    /*
     * Groups are all-or-nothing and are offered only when the evidence for
     * them exists. A group left null keeps its interface UNAVAILABLE with the
     * frozen reason, which is the honest answer and a correct outcome.
     */

    /* ui.read: pure mirror read-back, no engine access, always available. */
    backend.ui_read_string = &wotbmod::loader::V3NativeUiReadString;
    backend.ui_read_style = &wotbmod::loader::V3NativeUiReadStyle;

    /* camera.state: needs the SwitchState capture; nothing else can reach a
     * CameraController on this build. */
    if (g_original_camera_switch_state) {
        backend.camera_read_observed_state =
            &DeclaredCameraReadObservedState;
    }

    /* audio.intercept: offered only when at least one CreateSoundEvent slot
     * still holds exactly the address this pack was generated for. */
    if (SoundInterceptSlotsBindable()) {
        backend.audio_intercept_publish_name =
            &DeclaredSoundInterceptPublishName;
        backend.audio_intercept_disarm = &DeclaredSoundInterceptDisarm;
    }

    /* scene.enumerate: needs the proven main-thread ingress. Without it the
     * walk would have to guess which thread it is on, and guessing there is
     * a use-after-free rather than a wrong answer. */
    if (g_original_update_and_draw_windows) {
        backend.scene_get_walk_limits =
            &wotbmod::loader::V3NativeSceneWalkLimits;
        backend.scene_walk_active =
            &wotbmod::loader::V3NativeSceneWalkActive;
    }

    /* tracer: the whole surface is one static table. */
    if (g_tracer_shell_type_count != 0u) {
        backend.tracer_get_shell_type_count =
            &DeclaredTracerGetShellTypeCount;
        backend.tracer_get_style = &DeclaredTracerGetStyle;
    }

    /* ges: engine-native listener registration; slots only when the loader
     * indexed the type table and captured the bus hook. */
    if (g_ges_provider.list_types && g_ges_provider.observe &&
        g_ges_provider.publish) {
        backend.ges_list_types = g_ges_provider.list_types;
        backend.ges_observe = g_ges_provider.observe;
        backend.ges_publish = g_ges_provider.publish;
    }

    /* session.cluster: the loader's OnHostChosen capture; all four or none. */
    if (g_session_cluster_provider.enumerate && g_session_cluster_provider.get_current &&
        g_session_cluster_provider.change && g_session_cluster_provider.set_manual) {
        backend.session_cluster_enumerate = g_session_cluster_provider.enumerate;
        backend.session_cluster_get_current = g_session_cluster_provider.get_current;
        backend.session_cluster_change = g_session_cluster_provider.change;
        backend.session_cluster_set_manual = g_session_cluster_provider.set_manual;
    }
    wotbmod::v3::SetClientHostDeclaredBackend(&backend);
    char status[224] = {};
    sprintf_s(
        status,
        "declared client backend installed ui.read=%d camera.state=%d "
        "audio.intercept=%d scene.enumerate=%d tracer=%d(%u) "
        "tracer.producer=%d ges=%d session=%d",
        backend.ui_read_string ? 1 : 0,
        backend.camera_read_observed_state ? 1 : 0,
        backend.audio_intercept_publish_name ? 1 : 0,
        backend.scene_walk_active ? 1 : 0,
        backend.tracer_get_style ? 1 : 0,
        g_tracer_shell_type_count,
        g_state.tracer_producer_ready ? 1 : 0,
        backend.ges_observe ? 1 : 0,
        backend.session_cluster_change ? 1 : 0);
    Log(status);
    return WOTBMOD_V3_OK;
}

extern "C" uint32_t WOTBMOD_CALL
WotbModV3NativeBindings_GetInstalledSourceMask(void) {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(
            &g_state.installed_source_mask, 0, 0));
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ResolveHookSymbol(
    const char* symbol,
    void** out_target) {
    if (!symbol || !symbol[0] || !out_target) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_target = nullptr;
    if (!g_state.created ||
        !g_state.reviewed_hook_symbols_ready) {
        return g_state.compatibility ==
                       WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH
                   ? WOTBMOD_V3_E_CLIENT_MISMATCH
                   : WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    struct HookSymbol {
        const char* name;
        uint32_t rva;
    };
    // Only reviewed targets with a name re_anchors.md actually documents.
    // Data anchors (vtables, singletons) are absent because they are not
    // executable; CRT boilerplate is absent because a hook on operator new
    // fires for the entire process; six anchors the notes describe only by
    // role are withheld until their real names are established.
    // Enforced by _mod_tools/test_hook_symbol_table.py.
    static const HookSymbol kHookSymbols[] = {
        // -- battle vehicle --------------------------------------------
        {"Vehicle::onEnterWorld", kOnEnterWorldRva},
        {"Vehicle::onLeaveWorld", kOnLeaveWorldRva},
        {"Vehicle::showShooting", kShowShootingRva},
        {"Vehicle::set_health", kSetHealthRva},
        {"Avatar::updateVehicleHealth", kUpdateVehicleHealthRva},
        {"ReloadTimer::setState", kReloadSetStateRva},
        {"GameSceneController::OnVehicleHitDamage", kVehicleHitDamageRva},
        {"UIShellSelectorControl::OnCurrentAmmoChanged", kAmmoChangedRva},
        // -- client / camera -------------------------------------------
        {"Client::Initialize", kClientInitializeRva},
        {"Client::LeaveToHangar", kLeaveToHangarRva},
        {"Camera::setFOV", kGameCameraSetFovRva},
        {"DAVA::Camera::SetFovY", kGameCameraSetFovRva},
        {"GameCamera::GameCamera", kGameCameraCtorRva},
        {"GameCamera::ctor", kGameCameraCtorRva},
        {"GameCamera::~GameCamera", kGameCameraDtorRva},
        {"GameCamera::dtor", kGameCameraDtorRva},
        {"GES::Avatar::CameraModeChanged", kCameraModeChangedRva},
        {"CameraModeChanged", kCameraModeChangedRva},
        // -- entity / tracers ------------------------------------------
        {"BWEntity::BWEntity", kBwEntityCtorRva},
        {"BWEntity::ctor", kBwEntityCtorRva},
        {"BWEntity::~BWEntity", kBwEntityDtorRva},
        {"BWEntity::dtor", kBwEntityDtorRva},
        {"TracerManager::TracerManager", kTracerManagerCtorRva},
        {"TracerManager::ctor", kTracerManagerCtorRva},
        {"TracerManager::~TracerManager", kTracerManagerDtorRva},
        {"TracerManager::dtor", kTracerManagerDtorRva},
        {"TracerManager::ShowTracer", kTracerManagerShowTracerRva},
        // -- DAVA scene ------------------------------------------------
        {"DAVA::Scene::Scene", kDefaultSceneCtorRva},
        {"DAVA::Scene::Activate", kDefaultSceneActivateRva},
        {"DAVA::Scene::Deactivate", kDefaultSceneDeactivateRva},
        {"DAVA::Scene::Draw", kDefaultSceneDrawRva},
        {"DAVA::Entity::Entity", kDefaultEntityCtorRva},
        {"DAVA::EntityCache::LoadEntityUnsafe", kDefaultSceneLoadEntityRva},
        {"DAVA::TransformComponent::SetLocalTransform",
         kDefaultTransformSetLocalTransformRva},
        // -- DAVA UI ---------------------------------------------------
        {"DAVA::UIControl::UIControl", kDefaultUiControlCtorRva},
        {"DAVA::UIControl::SystemInput", kUiControlSystemInputRva},
        {"DAVA::UIControlSystem::GetScreen",
         kDefaultUiControlSystemGetScreenRva},
        {"DAVA::UIPackage::ExtractControl", kDefaultUiExtractControlRva},
        {"DAVA::UIPackageLoader::LoadPackage", kDefaultUiLoadPackageRva},
        {"DAVA::UIPackageLoader::UIPackageLoader",
         kDefaultUiPackageLoaderCtorRva},
        {"DAVA::UIPackageLoader::~UIPackageLoader",
         kDefaultUiPackageLoaderDtorRva},
        {"DAVA::DefaultUIPackageBuilder::DefaultUIPackageBuilder",
         kDefaultUiPackageBuilderCtorRva},
        {"DAVA::DefaultUIPackageBuilder::~DefaultUIPackageBuilder",
         kDefaultUiPackageBuilderDtorRva},
        // -- DAVA engine / io ------------------------------------------
        {"DAVA::EngineContext::GetInstance", kDefaultGetEngineContextRva},
        {"DAVA::File::Create", kDavaFileCreateRva},
    };

    uint32_t rva = 0u;
    for (const HookSymbol& entry : kHookSymbols) {
        if (std::strcmp(symbol, entry.name) == 0) {
            rva = entry.rva;
            break;
        }
    }
    if (rva == 0u) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    void* target = AddressFromRva(rva);
    if (!IsExecutableAddress(target)) {
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    *out_target = target;
    return WOTBMOD_V3_OK;
}

extern "C" void WOTBMOD_CALL
WotbModV3NativeBindings_UpdateFrame(
    void* swap_chain,
    void* device,
    void* device_context,
    uint32_t width,
    uint32_t height,
    uint64_t frame_index,
    double delta_seconds) {
    if (!g_state.created) return;
    UpdateNativeInputWindow(
        static_cast<IDXGISwapChain*>(swap_chain));
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    {
        std::lock_guard<std::mutex> lock(g_state.render_mutex);
        ID3D11Device* typed_device =
            static_cast<ID3D11Device*>(device);
        ID3D11DeviceContext* typed_context =
            static_cast<ID3D11DeviceContext*>(device_context);
        if (g_state.renderer.device != typed_device) {
            DestroyRendererLocked();
        }
        g_state.renderer.swap_chain =
            static_cast<IDXGISwapChain*>(swap_chain);
        g_state.renderer.device = typed_device;
        g_state.renderer.context = typed_context;
        g_state.renderer.width = width;
        g_state.renderer.height = height;
        g_state.renderer.frame_index = frame_index;
        g_state.renderer.delta_seconds = delta_seconds;
    }
    wotbmod::loader::ManagedRendererUpdateFrame(
        swap_chain,
        device,
        device_context,
        width,
        height,
        frame_index,
        delta_seconds);
    ExpirePublicProjectiles(GetTickCount64());

    void* camera = InterlockedCompareExchangePointer(
        &g_state.camera, nullptr, nullptr);
    if (g_state.compatibility !=
            WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH &&
        ValidateCamera(camera)) {
        WotbModV3CameraState camera_state = {};
        camera_state.struct_size = sizeof(camera_state);
        camera_state.api_version = WOTBMOD_V3_CAMERA_VERSION;
        camera_state.mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
        float fov = 0.0f;
        float near_plane = 0.0f;
        float far_plane = 0.0f;
        if (ReadCameraTransform(camera, &camera_state.transform) &&
            SafeRead(
                static_cast<uint8_t*>(camera) + 0x268u,
                &fov,
                sizeof(fov)) &&
            SafeRead(
                static_cast<uint8_t*>(camera) +
                    wotbmod::v3::native_layout::
                        kCameraNearPlaneOffset,
                &near_plane,
                sizeof(near_plane)) &&
            SafeRead(
                static_cast<uint8_t*>(camera) +
                    wotbmod::v3::native_layout::
                        kCameraFarPlaneOffset,
                &far_plane,
                sizeof(far_plane)) &&
            std::isfinite(fov) && fov >= 1.0f && fov <= 179.0f &&
            std::isfinite(near_plane) && near_plane >= 0.0f &&
            std::isfinite(far_plane) && far_plane > near_plane) {
            camera_state.fov_degrees = fov;
            camera_state.near_plane = near_plane;
            camera_state.far_plane = far_plane;
            const WotbModV3CameraState stock_camera_state = camera_state;
            g_state.camera_effects.Apply(
                static_cast<uint64_t>(
                    reinterpret_cast<uintptr_t>(camera)),
                delta_seconds,
                &camera_state);
            wotbmod::v3::ApplyClientHostCameraModifiers(
                WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME,
                &camera_state);
            const wotbmod::v3::native_camera::CameraWriteMask writes =
                wotbmod::v3::native_camera::CameraWritesFor(
                    stock_camera_state,
                    camera_state);
            if (writes.transform &&
                !WriteCameraTransform(camera, camera_state.transform)) {
                Log("camera modifier transform update faulted");
            }
            if (writes.fov &&
                !SetCameraFovSafe(camera, camera_state.fov_degrees)) {
                Log("camera modifier FOV update faulted");
            }
            if (writes.near_plane &&
                !SafeWrite(
                    static_cast<uint8_t*>(camera) +
                        wotbmod::v3::native_layout::
                            kCameraNearPlaneOffset,
                    &camera_state.near_plane,
                    sizeof(camera_state.near_plane))) {
                Log("camera modifier near-plane update faulted");
            }
            if (writes.far_plane &&
                !SafeWrite(
                    static_cast<uint8_t*>(camera) +
                        wotbmod::v3::native_layout::
                            kCameraFarPlaneOffset,
                    &camera_state.far_plane,
                    sizeof(camera_state.far_plane))) {
                Log("camera modifier far-plane update faulted");
            }
        }
    }
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_UpsertPublicVehicle(
    const WotbModV3NativePublicVehicle* vehicle,
    uint32_t reason) {
    const uint32_t allowed_flags =
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE |
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL |
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_DESTROYED |
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY;
    constexpr uint64_t kRequiredFields =
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VISIBILITY |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_LOCAL;
    constexpr uint64_t kCurrentVehicleFields =
        kRequiredFields |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE;
    constexpr uint64_t kVehicleIngressFields =
        kCurrentVehicleFields |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DISPLAY_NAME |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_POSITION |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DIRECTION |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_CLAN_TAG |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ACCOUNT_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_KILLS |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_NAME |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME;
    const auto finite3 = [](const float* v) {
        return std::isfinite(v[0]) && std::isfinite(v[1]) &&
               std::isfinite(v[2]);
    };
    const auto unit3 = [&finite3](const float* v) {
        if (!finite3(v)) return false;
        const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        return len2 > 0.81f && len2 < 1.21f;
    };
    const uint64_t valid_fields =
        vehicle && vehicle->valid_fields != 0u
            ? vehicle->valid_fields
            : kCurrentVehicleFields;
    if (!vehicle ||
        vehicle->struct_size < sizeof(*vehicle) ||
        vehicle->public_id == 0u ||
        (valid_fields & ~kVehicleIngressFields) != 0u ||
        (valid_fields & kRequiredFields) != kRequiredFields ||
        (vehicle->flags & ~allowed_flags) != 0u ||
        !WotbModV3NativeBindings_IsPublicVehicleFlags(
            vehicle->flags) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH) != 0u &&
         vehicle->health < 0) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH) != 0u &&
         vehicle->max_health < 0) ||
        ((valid_fields &
          (WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
           WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH)) ==
             (WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
              WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH) &&
         vehicle->health > vehicle->max_health) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DISPLAY_NAME) != 0u &&
         strnlen_s(
             vehicle->display_name,
             sizeof(vehicle->display_name)) ==
             sizeof(vehicle->display_name)) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_POSITION) != 0u &&
         !finite3(vehicle->position)) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DIRECTION) != 0u &&
         !unit3(vehicle->direction)) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_CLAN_TAG) != 0u &&
         strnlen_s(vehicle->clan_tag, sizeof(vehicle->clan_tag)) ==
             sizeof(vehicle->clan_tag)) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_NAME) != 0u &&
         strnlen_s(vehicle->vehicle_name, sizeof(vehicle->vehicle_name)) ==
             sizeof(vehicle->vehicle_name)) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_KILLS) != 0u &&
         vehicle->kills < 0) ||
        ((valid_fields &
          WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME) != 0u &&
         strnlen_s(vehicle->vehicle_display_name,
                   sizeof(vehicle->vehicle_display_name)) ==
             sizeof(vehicle->vehicle_display_name)) ||
        reason < WOTBMOD_V3_PUBLIC_ENTITY_REASON_LOCAL_PLAYER ||
        reason > WOTBMOD_V3_PUBLIC_ENTITY_REASON_SHUTDOWN) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (g_state.compatibility ==
        WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }

    uint64_t native_token = 0u;
    WotbModV3EntityHandle public_handle =
        WOTBMOD_V3_INVALID_HANDLE;
    bool was_published = false;
    AcquireSRWLockExclusive(&g_state.entity_lock);
    EntityRecord* record = ReservePublicEntityRecordLocked(
        vehicle->native_entity,
        vehicle->native_game_logic,
        vehicle->public_id);
    if (record) {
        native_token = record->token;
        public_handle = record->public_handle;
        was_published = record->published;
    }
    ReleaseSRWLockExclusive(&g_state.entity_lock);
    if (!record) return WOTBMOD_V3_E_LIMIT_REACHED;

    wotbmod::v3::ClientHostPublicEntity host_entity = {};
    host_entity.struct_size = sizeof(host_entity);
    host_entity.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    host_entity.native_token = native_token;
    host_entity.valid_fields = valid_fields;
    WotbModV3PublicEntitySnapshot& snapshot =
        host_entity.snapshot;
    snapshot.struct_size = sizeof(snapshot);
    snapshot.api_version = WOTBMOD_V3_ENTITY_PUBLIC_VERSION;
    snapshot.public_id = vehicle->public_id;
    snapshot.type = WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE;
    snapshot.visible_to_player = 1u;
    snapshot.local_player =
        (vehicle->flags &
         WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL) != 0u
            ? 1u
            : 0u;
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM) != 0u) {
        snapshot.team = vehicle->team;
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH) != 0u) {
        snapshot.health = vehicle->health;
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH) != 0u) {
        snapshot.max_health = vehicle->max_health;
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE) != 0u) {
        strcpy_s(
            snapshot.public_type,
            sizeof(snapshot.public_type),
            "vehicle");
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DISPLAY_NAME) != 0u) {
        strcpy_s(
            snapshot.display_name,
            sizeof(snapshot.display_name),
            vehicle->display_name);
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_POSITION) != 0u) {
        snapshot.position.x = vehicle->position[0];
        snapshot.position.y = vehicle->position[1];
        snapshot.position.z = vehicle->position[2];
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DIRECTION) != 0u) {
        snapshot.direction.x = vehicle->direction[0];
        snapshot.direction.y = vehicle->direction[1];
        snapshot.direction.z = vehicle->direction[2];
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ACCOUNT_ID) != 0u) {
        host_entity.extras.account_id = vehicle->account_id;
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_KILLS) != 0u) {
        host_entity.extras.kills = vehicle->kills;
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_CLAN_TAG) != 0u) {
        strcpy_s(
            host_entity.extras.clan_tag,
            sizeof(host_entity.extras.clan_tag),
            vehicle->clan_tag);
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_NAME) != 0u) {
        strcpy_s(
            host_entity.extras.vehicle_name,
            sizeof(host_entity.extras.vehicle_name),
            vehicle->vehicle_name);
    }
    if ((valid_fields &
         WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME) != 0u) {
        strcpy_s(
            host_entity.extras.vehicle_display_name,
            sizeof(host_entity.extras.vehicle_display_name),
            vehicle->vehicle_display_name);
    }

    WotbModV3Result registry_result = WOTBMOD_V3_OK;
    if (was_published) {
        snapshot.handle = public_handle;
        registry_result =
            wotbmod::v3::UpdateClientHostPublicEntity(
                public_handle, &host_entity);
    } else {
        registry_result =
            wotbmod::v3::RegisterClientHostPublicEntity(
                &host_entity, &public_handle);
        snapshot.handle = public_handle;
    }
    if (registry_result != WOTBMOD_V3_OK) {
        if (!was_published) {
            AcquireSRWLockExclusive(&g_state.entity_lock);
            record = FindEntityRecordByPublicIdLocked(
                vehicle->public_id);
            if (record && !record->published) *record = {};
            ReleaseSRWLockExclusive(&g_state.entity_lock);
        }
        return registry_result;
    }

    AcquireSRWLockExclusive(&g_state.entity_lock);
    record = FindEntityRecordByPublicIdLocked(vehicle->public_id);
    if (!record || record->token != native_token) {
        ReleaseSRWLockExclusive(&g_state.entity_lock);
        wotbmod::v3::RemoveClientHostPublicEntity(public_handle);
        return WOTBMOD_V3_E_CONFLICT;
    }
    record->entity = vehicle->native_entity
                         ? vehicle->native_entity
                         : record->entity;
    record->game_logic = vehicle->native_game_logic
                             ? vehicle->native_game_logic
                             : record->game_logic;
    record->public_handle = public_handle;
    record->snapshot = snapshot;
    record->published = true;
    ReleaseSRWLockExclusive(&g_state.entity_lock);

    const char* topic = was_published
                            ? WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED
                            : WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED;
    const WotbModV3Result event_result =
        PublishEntityLifecycle(topic, reason, snapshot);
    return event_result;
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveShot(
    uint32_t primary_public_entity_id,
    uint32_t shot_code) {
    // EVERY REFUSAL IS NAMED, ONCE.
    //
    // This function is the only way a fired shell becomes a
    // `wotbmod.projectile.*` event, and on 11.19.0.834 it produces nothing: the
    // same detour that reaches it also emits an RPC observation and a
    // `gameplay.shot_fired` for the same entity id, and those both arrive. It
    // cannot be diagnosed offline either - the first gate below is fail-closed
    // behind state only a matching client can arm, so no test binary gets past
    // it. Without a line in the log, the only symptom is silence.
    //
    // Once per reason per session: a refusal that repeats on every shot would
    // bury the log it is meant to explain.
    static LONG reported_not_supported = 0;
    static LONG reported_mismatch = 0;
    static LONG reported_unresolved = 0;
    if (primary_public_entity_id == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_state.created ||
        InterlockedCompareExchange(
            &g_state.gameplay_bridge_sources, 0, 0) == 0) {
        if (InterlockedExchange(&reported_not_supported, 1) == 0) {
            char message[160] = {};
            sprintf_s(
                message,
                "[v3] shot ingress disabled: created=%d sources=%ld",
                g_state.created ? 1 : 0,
                static_cast<long>(InterlockedCompareExchange(
                    &g_state.gameplay_bridge_sources, 0, 0)));
            Log(message);
        }
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (g_state.compatibility ==
        WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        if (InterlockedExchange(&reported_mismatch, 1) == 0) {
            Log("[v3] shot ingress disabled: client fingerprint mismatch");
        }
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    uint32_t scope = WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
    if (!ResolveProjectileOwnerScope(
            primary_public_entity_id, &scope)) {
        if (InterlockedExchange(&reported_unresolved, 1) == 0) {
            char message[160] = {};
            sprintf_s(
                message,
                "[v3] shot ingress: owner scope unresolved for entity %u",
                primary_public_entity_id);
            Log(message);
        }
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    ProjectileRecord* record = nullptr;
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    record = ReserveProjectileRecordLocked();
    if (record) {
        record->public_entity_id = primary_public_entity_id;
        record->owner_scope = scope;
        record->source =
            WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT;
        record->lifecycle_state =
            WOTBMOD_V3_PROJECTILE_STATE_CREATED;
        record->stock_shot_code = shot_code;
        record->valid_fields =
            WOTBMOD_V3_PROJECTILE_FIELD_PRIMARY_ENTITY |
            WOTBMOD_V3_PROJECTILE_FIELD_STOCK_SHOT_CODE;
    }
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    if (!record) {
        static LONG reported_limit = 0;
        if (InterlockedExchange(&reported_limit, 1) == 0) {
            Log("[v3] shot ingress: projectile table is full");
        }
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    const WotbModV3Result result =
        PublishNewProjectile(record, true);
    // THE TAIL IS LOGGED TOO, INCLUDING SUCCESS.
    //
    // The first live run with only the gates instrumented produced no line at
    // all while still emitting nothing: the gates passed and the failure was
    // past them. Reporting every distinct outcome once removes the guessing -
    // a silent success here would mean the loss is further downstream again,
    // in the client-services registry rather than in this file.
    static LONG reported_publish[24] = {};
    const int slot = (result >= 0 && result < 24) ? result : 23;
    if (InterlockedExchange(&reported_publish[slot], 1) == 0) {
        char message[160] = {};
        sprintf_s(
            message,
            "[v3] shot ingress: PublishNewProjectile -> %d (entity %u, scope %u)",
            static_cast<int>(result),
            primary_public_entity_id,
            scope);
        Log(message);
    }
    if (result != WOTBMOD_V3_OK) {
        AcquireSRWLockExclusive(&g_state.projectile_lock);
        if (record->alive && !record->published) *record = {};
        ReleaseSRWLockExclusive(&g_state.projectile_lock);
    }
    return result;
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveImpact(
    uint32_t primary_public_entity_id,
    uint32_t secondary_public_entity_id,
    const WotbModV3Vec3* position,
    uint32_t shell_type,
    uint32_t native_flags,
    uint32_t native_shot_id) {
    if ((!primary_public_entity_id && !secondary_public_entity_id) ||
        !position || !std::isfinite(position->x) ||
        !std::isfinite(position->y) ||
        !std::isfinite(position->z)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_state.created ||
        InterlockedCompareExchange(
            &g_state.gameplay_bridge_sources, 0, 0) == 0) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (g_state.compatibility ==
        WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    uint32_t scope = WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
    if (!ResolveProjectileOwnerScope(
            primary_public_entity_id, &scope) &&
        !ResolveProjectileOwnerScope(
            secondary_public_entity_id, &scope)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    ProjectileRecord* record = nullptr;
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    record = ReserveProjectileRecordLocked();
    if (record) {
        record->public_entity_id = primary_public_entity_id;
        record->secondary_public_entity_id =
            secondary_public_entity_id;
        record->owner_scope = scope;
        record->shell_type = shell_type;
        record->native_shot_id = native_shot_id;
        record->native_flags = native_flags;
        record->source =
            WOTBMOD_V3_PROJECTILE_SOURCE_NATIVE_IMPACT;
        record->lifecycle_state =
            WOTBMOD_V3_PROJECTILE_STATE_CREATED;
        record->origin = *position;
        record->impact_position = *position;
        record->valid_fields =
            WOTBMOD_V3_PROJECTILE_FIELD_IMPACT_POSITION |
            WOTBMOD_V3_PROJECTILE_FIELD_POSITION |
            WOTBMOD_V3_PROJECTILE_FIELD_SHELL_TYPE;
        if (primary_public_entity_id) {
            record->valid_fields |=
                WOTBMOD_V3_PROJECTILE_FIELD_PRIMARY_ENTITY;
        }
        if (secondary_public_entity_id) {
            record->valid_fields |=
                WOTBMOD_V3_PROJECTILE_FIELD_SECONDARY_ENTITY;
        }
        if (native_shot_id) {
            record->valid_fields |=
                WOTBMOD_V3_PROJECTILE_FIELD_NATIVE_SHOT_ID;
        }
    }
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    if (!record) return WOTBMOD_V3_E_LIMIT_REACHED;
    const uint64_t token = record->token;
    WotbModV3Result result = PublishNewProjectile(record, false);
    if (result == WOTBMOD_V3_OK) {
        wotbmod::v3::ClientHostProjectile impacted = {};
        WotbModV3ProjectileHandle handle =
            WOTBMOD_V3_INVALID_HANDLE;
        AcquireSRWLockExclusive(&g_state.projectile_lock);
        if (!record->alive || record->token != token ||
            !record->published) {
            ReleaseSRWLockExclusive(&g_state.projectile_lock);
            return WOTBMOD_V3_E_CONFLICT;
        }
        record->lifecycle_state =
            WOTBMOD_V3_PROJECTILE_STATE_IMPACTED;
        impacted = ToClientHostProjectile(*record);
        handle = record->public_handle;
        ReleaseSRWLockExclusive(&g_state.projectile_lock);
        result = wotbmod::v3::UpdateClientHostProjectile(
            handle, &impacted);
        if (result == WOTBMOD_V3_OK) {
            result =
                wotbmod::v3::PublishClientHostProjectileLifecycle(
                    handle,
                    WOTBMOD_V3_PROJECTILE_STATE_CREATED,
                    0u);
        }
    }
    if (result != WOTBMOD_V3_OK) {
        AcquireSRWLockExclusive(&g_state.projectile_lock);
        if (record->alive && record->token == token &&
            !record->published) {
            *record = {};
        }
        ReleaseSRWLockExclusive(&g_state.projectile_lock);
    }
    return result;
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_RemovePublicVehicle(
    uint32_t public_id,
    uint32_t reason) {
    if (public_id == 0u ||
        reason < WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN ||
        reason > WOTBMOD_V3_PUBLIC_ENTITY_REASON_SHUTDOWN) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    WotbModV3EntityHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t native_token = 0u;
    WotbModV3PublicEntitySnapshot snapshot = {};
    AcquireSRWLockShared(&g_state.entity_lock);
    const EntityRecord* record =
        FindEntityRecordByPublicIdLocked(public_id);
    if (record && record->published) {
        handle = record->public_handle;
        native_token = record->token;
        snapshot = record->snapshot;
    }
    ReleaseSRWLockShared(&g_state.entity_lock);
    if (handle == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    RemoveProjectilesForPublicEntity(public_id);

    const WotbModV3Result registry_result =
        wotbmod::v3::RemoveClientHostPublicEntity(handle);
    if (registry_result != WOTBMOD_V3_OK &&
        registry_result != WOTBMOD_V3_E_INVALID_HANDLE) {
        return registry_result;
    }
    AcquireSRWLockExclusive(&g_state.entity_lock);
    EntityRecord* current =
        FindEntityRecordByPublicIdLocked(public_id);
    if (current && current->token == native_token) {
        *current = {};
    }
    ReleaseSRWLockExclusive(&g_state.entity_lock);

    snapshot.visible_to_player = 0u;
    return PublishEntityLifecycle(
        WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED,
        reason,
        snapshot);
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveRpcMetadata(
    uint32_t direction,
    uint32_t public_entity_id,
    const char* entity_type,
    const char* method_name) {
    if ((direction != WOTBMOD_V3_RPC_INCOMING &&
         direction != WOTBMOD_V3_RPC_OUTGOING) ||
        public_entity_id == 0u ||
        !IsFixedTextValid(entity_type, WOTBMOD_V3_MAX_NAME) ||
        !IsFixedTextValid(method_name, WOTBMOD_V3_MAX_NAME)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (direction == WOTBMOD_V3_RPC_OUTGOING) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (InterlockedCompareExchange(
            &g_state.gameplay_bridge_sources, 0, 0) == 0) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (g_state.compatibility ==
        WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    if (!IsPublicEntityRegistered(public_entity_id)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }

    WotbModV3ObservedRpc rpc = {};
    rpc.struct_size = sizeof(rpc);
    rpc.api_version = WOTBMOD_V3_BIGWORLD_RPC_VERSION;
    rpc.direction = direction;
    rpc.public_entity_id = public_entity_id;
    rpc.sequence = static_cast<uint64_t>(
        InterlockedIncrement64(&g_state.next_rpc_sequence));
    rpc.timestamp_microseconds = UnixTimestampMicroseconds();
    strncpy_s(
        rpc.entity_type,
        sizeof(rpc.entity_type),
        entity_type,
        _TRUNCATE);
    strncpy_s(
        rpc.method_name,
        sizeof(rpc.method_name),
        method_name,
        _TRUNCATE);

    const WotbModV3Result callback_result =
        wotbmod::v3::NotifyClientHostObservedRpc(&rpc);
    const WotbModV3Result event_result =
        wotbmod::v3::PublishClientHostEvent(
            WOTBMOD_V3_EVENT_RPC_OBSERVED,
            &rpc,
            sizeof(rpc));
    return callback_result != WOTBMOD_V3_OK
               ? callback_result
               : event_result;
}

extern "C" void WOTBMOD_CALL
WotbModV3NativeBindings_SetGameplayBridgeSources(
    uint32_t installed_hook_mask) {
    if (g_state.compatibility ==
        WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH) {
        installed_hook_mask = 0u;
    }
    InterlockedExchange(
        &g_state.gameplay_bridge_sources,
        static_cast<LONG>(installed_hook_mask));
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModV3NativeBindings_ObserveUiInput(
    uint32_t phase,
    float screen_x,
    float screen_y,
    float delta_x,
    float delta_y,
    uint32_t modifiers) {
    wotbmod::loader::NotifyV3NativeClientServicesUiInputPhase(phase);
    return wotbmod::v3::NotifyClientHostUiInput(
        phase,
        screen_x,
        screen_y,
        delta_x,
        delta_y,
        modifiers);
}

extern "C" void WOTBMOD_CALL
WotbModV3NativeBindings_ResetPublicGameplayState(
    uint32_t reason) {
    uint32_t public_ids[kMaxTrackedEntities] = {};
    uint32_t count = 0u;
    AcquireSRWLockShared(&g_state.entity_lock);
    for (uint32_t index = 0u;
         index < kMaxTrackedEntities &&
         count < kMaxTrackedEntities;
         ++index) {
        const EntityRecord& record = g_state.entities[index];
        if (record.alive && record.published) {
            public_ids[count++] = record.public_id;
        }
    }
    ReleaseSRWLockShared(&g_state.entity_lock);
    for (uint32_t index = 0u; index < count; ++index) {
        WotbModV3NativeBindings_RemovePublicVehicle(
            public_ids[index], reason);
    }

    RetiredProjectile retired_projectiles[
        kMaxTrackedProjectiles] = {};
    uint32_t projectile_count = 0u;
    AcquireSRWLockExclusive(&g_state.projectile_lock);
    for (uint32_t index = 0u;
         index < kMaxTrackedProjectiles;
         ++index) {
        const ProjectileRecord& record =
            g_state.projectiles[index];
        if (record.alive && record.published &&
            record.public_handle != WOTBMOD_V3_INVALID_HANDLE) {
            retired_projectiles[projectile_count++] =
                CaptureRetirementLocked(record);
        }
        g_state.projectiles[index] = {};
    }
    ReleaseSRWLockExclusive(&g_state.projectile_lock);
    RetireProjectiles(
        retired_projectiles,
        projectile_count,
        reason == WOTBMOD_V3_PUBLIC_ENTITY_REASON_SHUTDOWN
            ? WOTBMOD_V3_PROJECTILE_DESTROY_SHUTDOWN
            : WOTBMOD_V3_PROJECTILE_DESTROY_NATIVE);
}

extern "C" void WOTBMOD_CALL
WotbModV3NativeBindings_Shutdown(void) {
    if (!g_state.created) return;
    /*
     * First, before anything else is torn down: removing the declared table
     * quiesces every in-flight backend call and then calls our disarm, which
     * restores the two CreateSoundEvent vtable slots and returns only after
     * the last detour invocation has left. Doing it after the MinHook
     * teardown below would leave a patched slot pointing at a detour whose
     * originals have already been cleared.
     */
    wotbmod::v3::SetClientHostDeclaredBackend(nullptr);
    InterlockedExchange(&g_state.main_thread_id, 0);
    InterlockedExchange(&g_state.camera_view_mode, -1);
    InterlockedExchange(&g_state.camera_animation_state, -1);
    InterlockedExchange(&g_state.camera_controller_thread, 0);
    InterlockedExchangePointer(&g_state.camera_controller, nullptr);
    InterlockedExchangePointer(
        &g_state.camera_controller_vtable, nullptr);
    /*
     * Take MAIN ingress offline FIRST, before any hook is disabled. After
     * this store dispatch_to_main_thread refuses instead of queueing, so no
     * new work can accumulate for a pump that is about to stop running.
     * Anything already queued is simply never drained, which is the same
     * outcome as the pre-hook default and is why the runtime's honest refusal
     * exists. Doing this after the disable would leave a window in which a
     * mod queues work that nothing will ever run.
     */
    wotbmod::v3::SetMainIngressOnline(false);
    RestoreNativeInputWindow();
    WotbModV3NativeBindings_ResetPublicGameplayState(
        WOTBMOD_V3_PUBLIC_ENTITY_REASON_SHUTDOWN);
    InterlockedExchange(&g_state.gameplay_bridge_sources, 0);
    InterlockedExchange(&g_state.installed_source_mask, 0);
    {
        std::lock_guard<std::mutex> lock(g_state.rpc_mutex);
        g_state.rpc_subscriptions.clear();
    }
    wotbmod::loader::ManagedRendererShutdown();
    wotbmod::loader::ShutdownV3NativeClientServices();
    /*
     * Ordered teardown. Swapping any two of these steps reintroduces a
     * use-after-free on the trampolines:
     *   1. disable  - no thread can newly enter a detour
     *   2. drain    - threads already inside one finish
     *   3. clear    - g_original_* stop pointing at trampoline memory
     *   4. remove   - trampolines are returned to MinHook's pool
     */
    for (uint32_t index = g_state.hook_count; index > 0u; --index) {
        HookSlot& slot = g_state.hooks[index - 1u];
        if (slot.target) {
            MH_DisableHook(slot.target);
        }
    }
    /*
     * The call sites are unpatched by now, so the counter only decreases
     * here. The bound exists purely so a detour blocked inside game code can
     * never hang shutdown forever.
     */
    for (uint32_t spin = 0u;
         InterlockedCompareExchange(&g_detour_active, 0, 0) != 0 &&
             spin < 2000u;
         ++spin) {
        Sleep(1);
    }
    g_original_camera_ctor = nullptr;
    g_original_camera_dtor = nullptr;
    g_original_client_initialize = nullptr;
    g_original_entity_ctor = nullptr;
    g_original_entity_dtor = nullptr;
    g_original_tracer_ctor = nullptr;
    g_original_tracer_dtor = nullptr;
    g_original_tracer_show = nullptr;
    g_original_camera_mode_changed = nullptr;
    g_original_camera_switch_state = nullptr;
    g_original_update_and_draw_windows = nullptr;
    /*
     * If something is still in flight after the bound, leak the trampolines
     * instead of freeing them: a few hundred bytes of leak is strictly better
     * than releasing memory a live thread is about to execute.
     */
    const bool quiesced =
        InterlockedCompareExchange(&g_detour_active, 0, 0) == 0;
    for (uint32_t index = g_state.hook_count; index > 0u; --index) {
        HookSlot& slot = g_state.hooks[index - 1u];
        if (slot.target && quiesced) {
            MH_RemoveHook(slot.target);
        }
        slot = {};
    }
    if (!quiesced) {
        Log("native detour still in flight at shutdown; trampolines "
            "leaked instead of freed");
    }
    g_state.hook_count = 0u;
    {
        std::lock_guard<std::mutex> lock(g_state.object_mutex);
        for (NativeMaterial* material : g_state.materials) {
            if (!material) continue;
            material->magic = 0u;
            delete material;
        }
        g_state.materials.clear();
        for (NativeTexture* texture : g_state.textures) {
            if (!texture) continue;
            texture->magic = 0u;
            ReleaseCom(texture->view);
            ReleaseCom(texture->texture);
            delete texture;
        }
        g_state.textures.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_state.render_mutex);
        DestroyRendererLocked();
        g_state.renderer = {};
    }
    InterlockedExchangePointer(&g_state.camera, nullptr);
    InterlockedExchangePointer(&g_state.client_owner, nullptr);
    InterlockedExchangePointer(&g_state.tracer_manager, nullptr);
    g_state.stock_tracer_ready = false;
    g_state.tracer_producer_ready = false;
    InterlockedExchange(&g_state.self_issued_tracer, 0);
    InterlockedExchange(&g_state.tracer_attributed, 0);
    InterlockedExchange(&g_state.tracer_unattributed, 0);
    InterlockedExchange(
        &g_state.camera_mode,
        WOTBMOD_V3_CAMERA_MODE_UNKNOWN);
    g_state.camera_effects.Clear();
    g_state.avatar_observed = nullptr;
    g_state.avatar_observed_user_data = nullptr;
    InterlockedExchange64(&g_state.next_rpc_subscription_token, 0);
    InterlockedExchange64(&g_state.next_rpc_sequence, 0);
    InterlockedExchange(&g_state.next_stock_tracer_id, 0);
    g_state.created = false;
    g_state.game_module = nullptr;
    g_state.game_base = nullptr;
    g_state.image_size = 0u;
    g_state.reviewed_hook_symbols_ready = false;
    g_state.binding_report_path.clear();
    g_state.executable_sha256.clear();
    g_state.binding_validation.clear();
}
