#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../include/wotb_mod_api_v3.h"

#include <cstdio>
#include <cstring>

namespace {
uint32_t g_checks = 0u;
uint32_t g_failures = 0u;

#define CHECK(condition)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(condition)) {                                                \
            ++g_failures;                                                  \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n",          \
                         __FILE__, __LINE__, #condition);                  \
        }                                                                  \
    } while (0)

WotbModV3CoreApiV1 g_core = {};
WotbModV3HandlesApiV1 g_handles = {};
WotbModV3UiApiV3 g_ui = {};
WotbModV3ResourcesApiV1 g_resources = {};
WotbModV3VehicleVisualApiV2 g_vehicle = {};
WotbModV3EventsApiV1 g_events = {};
WotbModV3RenderApiV1 g_render = {};
WotbModV3CameraApiV1 g_camera = {};

uint32_t g_release_count = 0u;
uint32_t g_ui_create_count = 0u;
uint32_t g_ui_destroy_count = 0u;
uint32_t g_ui_add_count = 0u;
uint32_t g_ui_remove_count = 0u;
uint32_t g_ui_position_count = 0u;
uint32_t g_ui_layout_count = 0u;
uint32_t g_ui_unsubscribe_count = 0u;
WotbModV3UiEventCallback g_ui_callback = nullptr;

uint32_t g_resource_release_count = 0u;
uint32_t g_skin_register_count = 0u;
uint32_t g_skin_apply_count = 0u;
uint32_t g_skin_rollback_count = 0u;
uint32_t g_skin_release_count = 0u;

uint32_t g_event_unsubscribe_count = 0u;
WotbModV3EventCallback g_vehicle_event_callback = nullptr;
WotbModV3EventCallback g_render_event_callback = nullptr;

uint32_t g_render_unregister_count = 0u;
uint32_t g_draw_text_count = 0u;
WotbModV3RenderCallback g_render_callback_fn = nullptr;
WotbModV3CameraModifierCallback g_camera_modifier_fn = nullptr;
uint32_t g_camera_remove_count = 0u;
uint32_t g_backend = WOTBMOD_V3_RENDER_BACKEND_D3D11;
uint64_t g_frame = 41u;
double g_delta = 1.0 / 60.0;
WotbModV3Rect g_viewport = {0.0f, 0.0f, 1920.0f, 1080.0f};

WotbModV3Result WOTBMOD_V3_CALL OkLog(
    WotbModV3Handle, uint32_t, const char*, const char*) {
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ReleaseHandle(
    WotbModV3Handle, WotbModV3Handle) {
    ++g_release_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiGetActive(
    WotbModV3Handle, WotbModV3UiHandle* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = 100u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSnapshot(
    WotbModV3Handle,
    WotbModV3UiHandle control,
    WotbModV3UiControlSnapshot* out) {
    if (!out || control != 100u) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    WOTBMOD_V3_INIT_STRUCT(*out, WOTBMOD_V3_UI_VERSION_3);
    out->control = control;
    out->parent = WOTBMOD_V3_INVALID_HANDLE;
    out->type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    out->flags = WOTBMOD_V3_UI_SNAPSHOT_VISIBLE |
                 WOTBMOD_V3_UI_SNAPSHOT_GAME_OWNED;
    out->geometry = {3.0f, 4.0f, 1280.0f, 720.0f};
    strcpy_s(out->id, "HangarRoot");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetPosition(
    WotbModV3Handle, WotbModV3UiHandle, WotbModV3Vec2) {
    ++g_ui_position_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiCreate(
    WotbModV3Handle,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* out) {
    CHECK(descriptor != nullptr);
    CHECK(descriptor && descriptor->type == WOTBMOD_V3_UI_CONTROL_CONTAINER);
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    ++g_ui_create_count;
    *out = 101u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiDestroy(
    WotbModV3Handle, WotbModV3UiHandle) {
    ++g_ui_destroy_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiAdd(
    WotbModV3Handle, WotbModV3UiHandle, WotbModV3UiHandle) {
    ++g_ui_add_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiRemove(
    WotbModV3Handle, WotbModV3UiHandle, WotbModV3UiHandle) {
    ++g_ui_remove_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiLayout(
    WotbModV3Handle,
    WotbModV3UiHandle,
    const WotbModV3UiLayoutDescriptor* layout) {
    CHECK(layout && layout->type == WOTBMOD_V3_UI_LAYOUT_VERTICAL);
    ++g_ui_layout_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSubscribe(
    WotbModV3Handle,
    WotbModV3UiHandle,
    uint32_t type,
    WotbModV3UiEventCallback callback,
    void*,
    WotbModV3Token* out) {
    CHECK(type == WOTBMOD_V3_UI_EVENT_CLICK);
    if (!callback || !out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    g_ui_callback = callback;
    *out = 102u;
    // THE CLICK ARRIVES WHILE THE SUBSCRIPTION IS LIVE.
    //
    // It used to be delivered from the test body after OnEnable had returned,
    // which only worked because the sample left its subscription (and the
    // input-capture rect behind it) alive for the whole session - the very
    // defect that blinded the client's top-left corner. Now that the sample
    // closes its transaction before returning, the only honest moment to
    // dispatch a click is while the subscription actually exists.
    WotbModV3UiEvent event = {};
    callback(1u, &event, nullptr);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiUnsubscribe(
    WotbModV3Handle, WotbModV3Token) {
    ++g_ui_unsubscribe_count;
    g_ui_callback = nullptr;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ResourceLoad(
    WotbModV3Handle,
    const WotbModV3ResourceLoadDesc* desc,
    WotbModV3ResourceHandle* out) {
    CHECK(desc && std::strcmp(desc->uri, "mod://skin/skin.json") == 0);
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = 201u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ResourceRelease(
    WotbModV3Handle, WotbModV3ResourceHandle) {
    ++g_resource_release_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetLocal(
    WotbModV3Handle, WotbModV3EntityHandle* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = 301u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SkinRegister(
    WotbModV3Handle,
    const WotbModV3VehicleSkinPack* pack,
    WotbModV3Handle* out) {
    CHECK(pack != nullptr);
    CHECK(pack && pack->asset_count == 5u);
    CHECK(pack && std::strcmp(pack->vehicle_name, "R110_Object_260") == 0);
    CHECK(pack && pack->assets[0].lod == 0 && pack->assets[1].lod == 1);
    CHECK(pack && std::strncmp(pack->assets[0].stock_game_uri, "game://", 7) == 0);
    CHECK(pack && std::strncmp(pack->assets[0].replacement_uri, "mod://", 6) == 0);
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    ++g_skin_register_count;
    *out = 302u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SkinApply(
    WotbModV3Handle, WotbModV3Handle, WotbModV3EntityHandle) {
    ++g_skin_apply_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SkinRollback(
    WotbModV3Handle, WotbModV3Handle) {
    ++g_skin_rollback_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SkinState(
    WotbModV3Handle,
    WotbModV3Handle,
    WotbModV3VehicleSkinState* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof(*out));
    WOTBMOD_V3_INIT_STRUCT(*out, WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    out->applied = 1u;
    out->asset_count = 5u;
    out->mounted_asset_count = 5u;
    out->requires_model_reload = 1u;
    out->vehicle = 301u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SkinRelease(
    WotbModV3Handle, WotbModV3Handle) {
    ++g_skin_release_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventSubscribe(
    WotbModV3Handle,
    const WotbModV3EventSubscriptionInfo* info,
    WotbModV3EventCallback callback,
    void*,
    WotbModV3EventToken* out) {
    if (!info || !callback || !out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (std::strcmp(info->topic_pattern, WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED) == 0) {
        g_vehicle_event_callback = callback;
        *out = 401u;
    } else if (std::strcmp(info->topic_pattern, "wotbmod.render.*") == 0) {
        g_render_event_callback = callback;
        *out = 402u;
    } else {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventUnsubscribe(
    WotbModV3Handle, WotbModV3EventToken token) {
    ++g_event_unsubscribe_count;
    if (token == 401u) g_vehicle_event_callback = nullptr;
    if (token == 402u) g_render_event_callback = nullptr;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderRegister(
    WotbModV3Handle,
    uint32_t phase,
    int32_t,
    WotbModV3RenderCallback callback,
    void*,
    WotbModV3Token* out) {
    CHECK(phase == WOTBMOD_V3_RENDER_PHASE_AFTER_UI);
    if (!callback || !out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    g_render_callback_fn = callback;
    *out = 601u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderUnregister(
    WotbModV3Handle, WotbModV3Token) {
    ++g_render_unregister_count;
    g_render_callback_fn = nullptr;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetBackend(
    WotbModV3Handle, uint32_t* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = g_backend;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetViewport(
    WotbModV3Handle, WotbModV3Rect* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = g_viewport;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetFrame(
    WotbModV3Handle, uint64_t* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = g_frame;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetDelta(
    WotbModV3Handle, double* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = g_delta;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderDrawText(
    WotbModV3Handle, const WotbModV3DrawText* draw) {
    CHECK(draw && draw->text && std::strstr(draw->text, "backend=") != nullptr);
    ++g_draw_text_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetActive(
    WotbModV3Handle, WotbModV3CameraHandle* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = 501u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetMode(
    WotbModV3Handle, WotbModV3CameraHandle, uint32_t* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = WOTBMOD_V3_CAMERA_MODE_SNIPER;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraAddModifier(
    WotbModV3Handle,
    uint32_t phase,
    int32_t,
    WotbModV3CameraModifierCallback callback,
    void*,
    WotbModV3Token* out) {
    CHECK(phase == WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME);
    if (!callback || !out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    g_camera_modifier_fn = callback;
    *out = 502u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraRemoveModifier(
    WotbModV3Handle, WotbModV3Token) {
    ++g_camera_remove_count;
    g_camera_modifier_fn = nullptr;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL QueryInterface(
    WotbModV3Handle,
    const char* name,
    uint32_t,
    const void** out) {
    if (!name || !out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out = nullptr;
    if (std::strcmp(name, WOTBMOD_V3_IFACE_CORE) == 0) *out = &g_core;
    else if (std::strcmp(name, WOTBMOD_V3_IFACE_HANDLES) == 0) *out = &g_handles;
    else if (std::strcmp(name, WOTBMOD_V3_IFACE_UI) == 0) *out = &g_ui;
    else if (std::strcmp(name, WOTBMOD_V3_IFACE_RESOURCES) == 0) *out = &g_resources;
    else if (std::strcmp(name, WOTBMOD_V3_IFACE_VEHICLE_VISUAL) == 0) *out = &g_vehicle;
    else if (std::strcmp(name, WOTBMOD_V3_IFACE_EVENTS) == 0) *out = &g_events;
    else if (std::strcmp(name, WOTBMOD_V3_IFACE_RENDER) == 0) *out = &g_render;
    else if (std::strcmp(name, WOTBMOD_V3_IFACE_CAMERA) == 0) *out = &g_camera;
    return *out ? WOTBMOD_V3_OK : WOTBMOD_V3_E_NOT_SUPPORTED;
}

void InitTables() {
    WOTBMOD_V3_INIT_STRUCT(g_core, WOTBMOD_V3_CORE_VERSION);
    g_core.log = &OkLog;
    WOTBMOD_V3_INIT_STRUCT(g_handles, WOTBMOD_V3_HANDLES_VERSION);
    g_handles.release = &ReleaseHandle;
    g_ui.v2.struct_size = sizeof(g_ui);
    g_ui.v2.api_version = WOTBMOD_V3_UI_VERSION_3;
    g_ui.v2.control_create = &UiCreate;
    g_ui.v2.control_destroy = &UiDestroy;
    g_ui.v2.control_add_child = &UiAdd;
    g_ui.v2.control_remove_child = &UiRemove;
    g_ui.v2.control_set_position = &UiSetPosition;
    g_ui.v2.layout_set = &UiLayout;
    g_ui.v2.event_subscribe = &UiSubscribe;
    g_ui.v2.event_unsubscribe = &UiUnsubscribe;
    g_ui.get_active_screen = &UiGetActive;
    g_ui.control_get_snapshot = &UiSnapshot;
    WOTBMOD_V3_INIT_STRUCT(g_resources, WOTBMOD_V3_RESOURCES_VERSION);
    g_resources.load = &ResourceLoad;
    g_resources.release = &ResourceRelease;
    g_vehicle.v1.struct_size = sizeof(g_vehicle);
    g_vehicle.v1.api_version = WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2;
    g_vehicle.v1.get_local_player_vehicle = &VehicleGetLocal;
    g_vehicle.skin_pack_register = &SkinRegister;
    g_vehicle.skin_pack_apply = &SkinApply;
    g_vehicle.skin_pack_rollback = &SkinRollback;
    g_vehicle.skin_pack_get_state = &SkinState;
    g_vehicle.skin_pack_release = &SkinRelease;
    WOTBMOD_V3_INIT_STRUCT(g_events, WOTBMOD_V3_EVENTS_VERSION);
    g_events.subscribe = &EventSubscribe;
    g_events.unsubscribe = &EventUnsubscribe;
    WOTBMOD_V3_INIT_STRUCT(g_render, WOTBMOD_V3_RENDER_VERSION);
    g_render.register_callback = &RenderRegister;
    g_render.unregister_callback = &RenderUnregister;
    g_render.get_backend = &RenderGetBackend;
    g_render.get_viewport = &RenderGetViewport;
    g_render.get_frame_index = &RenderGetFrame;
    g_render.get_delta_time = &RenderGetDelta;
    g_render.draw_text = &RenderDrawText;
    WOTBMOD_V3_INIT_STRUCT(g_camera, WOTBMOD_V3_CAMERA_VERSION);
    g_camera.get_active = &CameraGetActive;
    g_camera.get_mode = &CameraGetMode;
    g_camera.add_modifier = &CameraAddModifier;
    g_camera.remove_modifier = &CameraRemoveModifier;
}

template <typename T>
T Export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

struct LoadedSample {
    HMODULE module;
    WotbModV3Info info;
};

LoadedSample LoadSample(const char* path, const char* expected_id) {
    LoadedSample sample = {};
    sample.module = LoadLibraryA(path);
    CHECK(sample.module != nullptr);
    if (!sample.module) return sample;
    const auto entry = Export<WotbModLoadV3Fn>(sample.module, WOTBMOD_V3_ENTRY_NAME);
    CHECK(entry != nullptr);
    WotbModV3Bootstrap bootstrap = {};
    WOTBMOD_V3_INIT_STRUCT(bootstrap, WOTBMOD_V3_ABI_VERSION);
    bootstrap.sdk_version = WOTBMOD_V3_SDK_VERSION;
    bootstrap.bootstrap_version = WOTBMOD_V3_BOOTSTRAP_VERSION;
    bootstrap.query_interface = &QueryInterface;
    CHECK(entry && entry(&bootstrap, 1u, &sample.info) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(sample.info.id, expected_id) == 0);
    CHECK(sample.info.on_enable && sample.info.on_disable && sample.info.on_unload);
    if (sample.info.on_enable) sample.info.on_enable(&bootstrap, 1u);
    return sample;
}

void FinishSample(LoadedSample* sample) {
    if (!sample || !sample->module) return;
    WotbModV3Bootstrap bootstrap = {};
    WOTBMOD_V3_INIT_STRUCT(bootstrap, WOTBMOD_V3_ABI_VERSION);
    bootstrap.query_interface = &QueryInterface;
    sample->info.on_disable(&bootstrap, 1u);
    sample->info.on_unload(&bootstrap, 1u);
    FreeLibrary(sample->module);
    sample->module = nullptr;
}

void TestUi(const char* path) {
    const uint32_t release_before = g_release_count;
    LoadedSample sample = LoadSample(path, "sample.ui_transaction");
    using StateFn = uint32_t(WOTBMOD_V3_CALL*)();
    using TextFn = const char*(WOTBMOD_V3_CALL*)();
    const auto state = Export<StateFn>(sample.module, "WotbSampleUi_GetState");
    const auto inspection = Export<TextFn>(sample.module, "WotbSampleUi_GetInspection");
    CHECK(state && (state() & 0x0fu) == 0x0fu);
    CHECK(inspection && std::strstr(inspection(), "path=/HangarRoot") != nullptr);
    CHECK(inspection && std::strstr(inspection(), "text=NOT_EXPOSED_RC1") != nullptr);
    // The click dispatched from UiSubscribe reached the handler...
    CHECK(state && (state() & (1u << 6)) != 0u);
    // ...and by the time OnEnable returned the subscription was gone, so no
    // input-capture rect outlives the transaction. This is the assertion that
    // would have caught the dead top-left corner on 11.19.0.834.
    CHECK(g_ui_callback == nullptr);
    CHECK(g_ui_unsubscribe_count == 1u);
    CHECK(g_ui_destroy_count == 1u);
    FinishSample(&sample);
    CHECK(g_ui_create_count == 1u && g_ui_add_count == 1u);
    CHECK(g_ui_layout_count == 1u && g_ui_unsubscribe_count == 1u);
    CHECK(g_ui_remove_count == 1u && g_ui_destroy_count == 1u);
    CHECK(g_ui_position_count == 2u);
    CHECK(g_release_count == release_before + 1u);
}

void TestVehicle(const char* path) {
    const uint32_t release_before = g_release_count;
    LoadedSample sample = LoadSample(path, "sample.vehicle_cosmetic");
    using StateFn = uint32_t(WOTBMOD_V3_CALL*)();
    const auto state = Export<StateFn>(sample.module, "WotbSampleVehicle_GetState");
    const auto reapply = Export<StateFn>(sample.module, "WotbSampleVehicle_GetReapplyCount");
    const auto reload = Export<StateFn>(sample.module, "WotbSampleVehicle_RequiresReload");
    CHECK(state && (state() & 0x1fu) == 0x1fu);
    CHECK(reapply && reapply() == 1u);
    CHECK(reload && reload() == 1u);
    CHECK(g_vehicle_event_callback != nullptr);
    if (g_vehicle_event_callback) {
        WotbModV3Event event = {};
        g_vehicle_event_callback(1u, &event, nullptr);
    }
    CHECK(reapply && reapply() == 2u);
    CHECK(state && (state() & (1u << 6)) != 0u);
    FinishSample(&sample);
    CHECK(g_skin_register_count == 1u && g_skin_apply_count == 2u);
    CHECK(g_skin_rollback_count == 2u && g_skin_release_count == 1u);
    CHECK(g_resource_release_count == 1u);
    CHECK(g_event_unsubscribe_count >= 1u);
    CHECK(g_release_count == release_before + 2u);
}

void TestCamera(const char* path) {
    const uint32_t release_before = g_release_count;
    LoadedSample sample = LoadSample(path, "sample.camera_render");
    using StateFn = uint32_t(WOTBMOD_V3_CALL*)();
    using FrameFn = uint64_t(WOTBMOD_V3_CALL*)();
    const auto state = Export<StateFn>(sample.module, "WotbSampleCamera_GetState");
    const auto lifecycle = Export<StateFn>(sample.module, "WotbSampleCamera_GetLifecycleCount");
    const auto mode = Export<StateFn>(sample.module, "WotbSampleCamera_GetLastMode");
    const auto backend = Export<StateFn>(sample.module, "WotbSampleCamera_GetLastBackend");
    const auto frame = Export<FrameFn>(sample.module, "WotbSampleCamera_GetLastFrame");
    CHECK(state && (state() & 1u) != 0u);
    CHECK(g_camera_modifier_fn && g_render_callback_fn && g_render_event_callback);
    if (g_camera_modifier_fn) {
        WotbModV3CameraState camera_state = {};
        WOTBMOD_V3_INIT_STRUCT(camera_state, WOTBMOD_V3_CAMERA_VERSION);
        camera_state.fov_degrees = -1.0f;
        g_camera_modifier_fn(1u, &camera_state, nullptr);
        CHECK(camera_state.fov_degrees == 60.0f);
    }
    WotbModV3RenderFrameInfo frame_info = {};
    WOTBMOD_V3_INIT_STRUCT(frame_info, WOTBMOD_V3_RENDER_VERSION);
    if (g_render_callback_fn) g_render_callback_fn(1u, &frame_info, nullptr);
    g_backend = WOTBMOD_V3_RENDER_BACKEND_D3D12;
    g_viewport = {0.0f, 0.0f, 1280.0f, 720.0f};
    g_frame = 42u;
    if (g_render_event_callback) {
        WotbModV3Event event = {};
        g_render_event_callback(1u, &event, nullptr);
    }
    if (g_render_callback_fn) g_render_callback_fn(1u, &frame_info, nullptr);
    CHECK(g_draw_text_count == 2u);
    CHECK(state && (state() & 0x3fu) == 0x3fu);
    CHECK(lifecycle && lifecycle() == 1u);
    CHECK(mode && mode() == WOTBMOD_V3_CAMERA_MODE_SNIPER);
    CHECK(backend && backend() == WOTBMOD_V3_RENDER_BACKEND_D3D12);
    CHECK(frame && frame() == 42u);
    FinishSample(&sample);
    CHECK(g_render_unregister_count == 1u && g_camera_remove_count == 1u);
    CHECK(g_release_count == release_before + 1u);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: rc1_samples_host_tests <ui.dll> <vehicle.dll> <camera.dll>\n");
        return 2;
    }
    InitTables();
    TestUi(argv[1]);
    TestVehicle(argv[2]);
    TestCamera(argv[3]);
    if (g_failures) {
        std::fprintf(stderr, "RC1 SAMPLES FAILED: %u/%u\n", g_failures, g_checks);
        return 1;
    }
    std::printf("RC1 SAMPLES OK: checks=%u packages=3 cleanup=PASS\n", g_checks);
    return 0;
}
