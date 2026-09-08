#include "wotb_mod_api_v3.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
const WotbModV3Bootstrap* g_bootstrap = nullptr;
const WotbModV3CoreApiV1* g_core = nullptr;
const WotbModV3RenderApiV1* g_render = nullptr;
const WotbModV3CameraApiV1* g_camera = nullptr;
const WotbModV3EventsApiV1* g_events = nullptr;
const WotbModV3HandlesApiV1* g_handles = nullptr;
WotbModV3Handle g_mod = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3CameraHandle g_active_camera = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Token g_render_callback = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Token g_camera_modifier = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3EventToken g_render_lifecycle = WOTBMOD_V3_INVALID_HANDLE;
uint32_t g_state = 0u;
uint32_t g_lifecycle_events = 0u;
uint32_t g_last_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
uint32_t g_last_backend = WOTBMOD_V3_RENDER_BACKEND_NONE;
uint64_t g_last_frame = 0u;
WotbModV3Rect g_last_viewport = {};

template <typename T>
const T* Query(const char* name, uint32_t version) {
    const void* table = nullptr;
    return g_bootstrap && g_bootstrap->query_interface &&
           g_bootstrap->query_interface(g_mod, name, version, &table) ==
               WOTBMOD_V3_OK
        ? static_cast<const T*>(table)
        : nullptr;
}

void Log(uint32_t level, const char* text) {
    if (g_core && g_core->log) {
        g_core->log(g_mod, level, "sample.camera_render", text);
    }
}

void WOTBMOD_V3_CALL CameraModifier(
    WotbModV3Handle,
    WotbModV3CameraState* state,
    void*) {
    if (!state) return;
    // AFTER_GAME policy: preserve valid game output and repair only invalid FOV.
    if (!std::isfinite(state->fov_degrees) || state->fov_degrees < 1.0f ||
        state->fov_degrees > 179.0f) {
        state->fov_degrees = 60.0f;
    }
    g_state |= 1u << 4;
}

void WOTBMOD_V3_CALL RenderFrame(
    WotbModV3Handle,
    const WotbModV3RenderFrameInfo* frame,
    void*) {
    if (!g_render || !frame) return;
    uint32_t backend = WOTBMOD_V3_RENDER_BACKEND_NONE;
    WotbModV3Rect viewport = {};
    uint64_t frame_index = 0u;
    double delta_seconds = 0.0;
    if (g_render->get_backend(g_mod, &backend) == WOTBMOD_V3_OK &&
        g_render->get_viewport(g_mod, &viewport) == WOTBMOD_V3_OK &&
        g_render->get_frame_index(g_mod, &frame_index) == WOTBMOD_V3_OK &&
        g_render->get_delta_time(g_mod, &delta_seconds) == WOTBMOD_V3_OK) {
        g_last_backend = backend;
        g_last_viewport = viewport;
        g_last_frame = frame_index;
        g_state |= 1u << 1;
    }
    if (g_camera && g_active_camera != WOTBMOD_V3_INVALID_HANDLE) {
        uint32_t mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
        if (g_camera->get_mode(g_mod, g_active_camera, &mode) ==
            WOTBMOD_V3_OK) {
            g_last_mode = mode;
            g_state |= 1u << 2;
        }
    }
    char line[192] = {};
    sprintf_s(
        line,
        "RC1 camera/render backend=%u viewport=%.0fx%.0f frame=%llu dt=%.3f mode=%u",
        backend,
        viewport.width,
        viewport.height,
        static_cast<unsigned long long>(frame_index),
        delta_seconds,
        g_last_mode);
    WotbModV3DrawText draw = {};
    WOTBMOD_V3_INIT_STRUCT(draw, WOTBMOD_V3_RENDER_VERSION);
    draw.text = line;
    draw.font_uri = "system://default";
    draw.position = {16.0f, 16.0f};
    draw.color = {0.92f, 0.96f, 1.0f, 0.9f};
    draw.font_size = 15.0f;
    draw.max_width = viewport.width > 32.0f ? viewport.width - 32.0f : 0.0f;
    if (g_render->draw_text(g_mod, &draw) == WOTBMOD_V3_OK) {
        g_state |= 1u << 3;
    }
}

void WOTBMOD_V3_CALL RenderLifecycle(
    WotbModV3Handle,
    WotbModV3Event*,
    void*) {
    ++g_lifecycle_events;
    // Managed render resources/callback ownership remains with the runtime.
    g_state |= 1u << 5;
}

void Cleanup() {
    if (g_events && g_render_lifecycle != WOTBMOD_V3_INVALID_HANDLE) {
        g_events->unsubscribe(g_mod, g_render_lifecycle);
        g_render_lifecycle = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_render && g_render_callback != WOTBMOD_V3_INVALID_HANDLE) {
        g_render->unregister_callback(g_mod, g_render_callback);
        g_render_callback = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_camera && g_camera_modifier != WOTBMOD_V3_INVALID_HANDLE) {
        g_camera->remove_modifier(g_mod, g_camera_modifier);
        g_camera_modifier = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_handles && g_active_camera != WOTBMOD_V3_INVALID_HANDLE) {
        g_handles->release(g_mod, g_active_camera);
        g_active_camera = WOTBMOD_V3_INVALID_HANDLE;
    }
    g_state |= 1u << 6;
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    g_bootstrap = bootstrap;
    g_mod = mod;
    g_state = 0u;
    g_lifecycle_events = 0u;
    g_last_mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
    g_last_backend = WOTBMOD_V3_RENDER_BACKEND_NONE;
    g_last_frame = 0u;
    g_last_viewport = {};
    g_core = Query<WotbModV3CoreApiV1>(
        WOTBMOD_V3_IFACE_CORE, WOTBMOD_V3_CORE_VERSION);
    g_render = Query<WotbModV3RenderApiV1>(
        WOTBMOD_V3_IFACE_RENDER, WOTBMOD_V3_RENDER_VERSION);
    g_camera = Query<WotbModV3CameraApiV1>(
        WOTBMOD_V3_IFACE_CAMERA, WOTBMOD_V3_CAMERA_VERSION);
    g_events = Query<WotbModV3EventsApiV1>(
        WOTBMOD_V3_IFACE_EVENTS, WOTBMOD_V3_EVENTS_VERSION);
    g_handles = Query<WotbModV3HandlesApiV1>(
        WOTBMOD_V3_IFACE_HANDLES, WOTBMOD_V3_HANDLES_VERSION);
    if (!g_render || !g_camera) {
        Log(WOTBMOD_V3_LOG_ERROR, "render/camera interface unavailable");
        return;
    }
    if (g_camera->get_active(g_mod, &g_active_camera) == WOTBMOD_V3_OK) {
        g_state |= 1u << 0;
    }
    g_camera->add_modifier(
        g_mod,
        WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME,
        -100,
        &CameraModifier,
        nullptr,
        &g_camera_modifier);
    g_render->register_callback(
        g_mod,
        WOTBMOD_V3_RENDER_PHASE_AFTER_UI,
        0,
        &RenderFrame,
        nullptr,
        &g_render_callback);
    if (g_events) {
        WotbModV3EventSubscriptionInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_EVENTS_VERSION);
        info.topic_pattern = "wotbmod.render.*";
        info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
        info.receive_system_events = 1u;
        g_events->subscribe(
            g_mod,
            &info,
            &RenderLifecycle,
            nullptr,
            &g_render_lifecycle);
    }
    Log(WOTBMOD_V3_LOG_INFO,
        "managed overlay and AFTER_GAME modifier registered");
}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap*, WotbModV3Handle) { Cleanup(); }
void WOTBMOD_V3_CALL OnUnload(
    const WotbModV3Bootstrap*, WotbModV3Handle) { Cleanup(); }
}

WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleCamera_GetState() { return g_state; }
WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleCamera_GetLifecycleCount() { return g_lifecycle_events; }
WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleCamera_GetLastMode() { return g_last_mode; }
WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleCamera_GetLastBackend() { return g_last_backend; }
WOTBMOD_V3_EXPORT uint64_t WOTBMOD_V3_CALL
WotbSampleCamera_GetLastFrame() { return g_last_frame; }

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info || mod == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    strcpy_s(out_info->id, "sample.camera_render");
    strcpy_s(out_info->name, "RC1 Camera Render Sample");
    strcpy_s(out_info->version, "1.0.0-rc1");
    strcpy_s(out_info->author, "WotbMod SDK");
    strcpy_s(out_info->description,
             "Managed overlay, render telemetry and safe camera modifier.");
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    return WOTBMOD_V3_OK;
}
