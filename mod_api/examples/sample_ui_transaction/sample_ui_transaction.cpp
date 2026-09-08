#include "wotb_mod_api_v3.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {
const WotbModV3Bootstrap* g_bootstrap = nullptr;
const WotbModV3CoreApiV1* g_core = nullptr;
const WotbModV3UiApiV3* g_ui = nullptr;
const WotbModV3HandlesApiV1* g_handles = nullptr;
WotbModV3Handle g_mod = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3UiHandle g_screen = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3UiHandle g_managed = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Token g_event = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Vec2 g_original_position = {};
uint32_t g_state = 0u;
char g_inspection[512] = {};

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
        g_core->log(g_mod, level, "sample.ui_transaction", text);
    }
}

void WOTBMOD_V3_CALL UiEvent(
    WotbModV3Handle,
    const WotbModV3UiEvent*,
    void*) {
    g_state |= 1u << 6;
}

void Cleanup() {
    if (g_ui && g_event != WOTBMOD_V3_INVALID_HANDLE) {
        g_ui->v2.event_unsubscribe(g_mod, g_event);
        g_event = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_ui && g_managed != WOTBMOD_V3_INVALID_HANDLE) {
        if (g_screen != WOTBMOD_V3_INVALID_HANDLE) {
            g_ui->v2.control_remove_child(g_mod, g_screen, g_managed);
        }
        g_ui->v2.control_destroy(g_mod, g_managed);
        g_managed = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_ui && g_screen != WOTBMOD_V3_INVALID_HANDLE &&
        (g_state & (1u << 1)) != 0u) {
        if (g_ui->v2.control_set_position(
                g_mod, g_screen, g_original_position) == WOTBMOD_V3_OK) {
            g_state |= 1u << 4;
        }
    }
    if (g_handles && g_screen != WOTBMOD_V3_INVALID_HANDLE) {
        g_handles->release(g_mod, g_screen);
    }
    g_screen = WOTBMOD_V3_INVALID_HANDLE;
    g_state |= 1u << 5;
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    g_bootstrap = bootstrap;
    g_mod = mod;
    g_state = 0u;
    g_core = Query<WotbModV3CoreApiV1>(
        WOTBMOD_V3_IFACE_CORE, WOTBMOD_V3_CORE_VERSION);
    g_ui = Query<WotbModV3UiApiV3>(
        WOTBMOD_V3_IFACE_UI, WOTBMOD_V3_UI_VERSION_3);
    g_handles = Query<WotbModV3HandlesApiV1>(
        WOTBMOD_V3_IFACE_HANDLES, WOTBMOD_V3_HANDLES_VERSION);
    if (!g_ui || g_ui->get_active_screen(g_mod, &g_screen) != WOTBMOD_V3_OK) {
        Log(WOTBMOD_V3_LOG_ERROR, "active stock UI screen is unavailable");
        return;
    }
    WotbModV3UiControlSnapshot snapshot = {};
    WOTBMOD_V3_INIT_STRUCT(snapshot, WOTBMOD_V3_UI_VERSION_3);
    if (g_ui->control_get_snapshot(g_mod, g_screen, &snapshot) !=
        WOTBMOD_V3_OK) {
        Cleanup();
        return;
    }
    g_original_position = {
        snapshot.geometry.x, snapshot.geometry.y};
    std::string path = "/" + std::string(snapshot.id);
    WotbModV3UiHandle parent = snapshot.parent;
    for (uint32_t depth = 0u;
         parent != WOTBMOD_V3_INVALID_HANDLE && depth < 8u;
         ++depth) {
        WotbModV3UiControlSnapshot ancestor = {};
        WOTBMOD_V3_INIT_STRUCT(ancestor, WOTBMOD_V3_UI_VERSION_3);
        if (g_ui->control_get_snapshot(g_mod, parent, &ancestor) !=
            WOTBMOD_V3_OK) break;
        path = "/" + std::string(ancestor.id) + path;
        parent = ancestor.parent;
    }
    sprintf_s(
        g_inspection,
        "name=%s type=%u path=%s text=NOT_EXPOSED_RC1 geometry=%.1f,%.1f,%.1f,%.1f",
        snapshot.id,
        snapshot.type,
        path.c_str(),
        snapshot.geometry.x,
        snapshot.geometry.y,
        snapshot.geometry.width,
        snapshot.geometry.height);
    g_state |= 1u << 0;
    WotbModV3Vec2 temporary = {
        g_original_position.x + 1.0f,
        g_original_position.y};
    if (g_ui->v2.control_set_position(g_mod, g_screen, temporary) ==
        WOTBMOD_V3_OK) {
        g_state |= 1u << 1;
    }
    WotbModV3UiControlDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_UI_VERSION);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.visible = 1u;
    descriptor.id = "sample.ui_transaction.container";
    descriptor.geometry = {20.0f, 20.0f, 260.0f, 80.0f};
    if (g_ui->v2.control_create(g_mod, &descriptor, &g_managed) ==
            WOTBMOD_V3_OK &&
        g_ui->v2.control_add_child(g_mod, g_screen, g_managed) ==
            WOTBMOD_V3_OK) {
        WotbModV3UiLayoutDescriptor layout = {};
        WOTBMOD_V3_INIT_STRUCT(layout, WOTBMOD_V3_UI_VERSION);
        layout.type = WOTBMOD_V3_UI_LAYOUT_VERTICAL;
        layout.direction = WOTBMOD_V3_UI_DIRECTION_TOP_TO_BOTTOM;
        layout.main_alignment = WOTBMOD_V3_UI_ALIGN_START;
        layout.cross_alignment = WOTBMOD_V3_UI_ALIGN_STRETCH;
        layout.spacing = 4.0f;
        if (g_ui->v2.layout_set(g_mod, g_managed, &layout) ==
            WOTBMOD_V3_OK) {
            g_state |= 1u << 2;
        }
        if (g_ui->v2.event_subscribe(
                g_mod,
                g_managed,
                WOTBMOD_V3_UI_EVENT_CLICK,
                &UiEvent,
                nullptr,
                &g_event) == WOTBMOD_V3_OK) {
            g_state |= 1u << 3;
        }
    }
    Log(WOTBMOD_V3_LOG_INFO, g_inspection);

    // THE TRANSACTION ENDS HERE, NOT AT OnDisable.
    //
    // A mod-created control that carries a UI_EVENT_CLICK subscription is
    // registered as an input-capture rect, and the loader's window hook
    // swallows every click inside it so the stock UI never sees the press.
    // This container is a CONTAINER: the client renders nothing for it. So
    // leaving it alive for the whole session parked an invisible 260x80
    // click-eater at (20,20) - directly on top of the universal back/close
    // button - and the game's top-left corner stopped responding. Measured on
    // 11.19.0.834: the back arrow on the battle-results screen ignored every
    // press, while a control at (259,146) - just outside the rect once the
    // 125% DPI scale is applied - worked normally.
    //
    // The rollback was always part of what this sample advertises ("Stock UI
    // inspection, safe mutation, managed layout, event, rollback"); it was
    // merely deferred to OnDisable, which in a live client means "never". The
    // demonstration is complete the moment the state bits are set, so the
    // transaction is closed immediately and nothing outlives it. OnDisable
    // and OnUnload still call Cleanup(); it is idempotent.
    Cleanup();
}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap*, WotbModV3Handle) {
    Cleanup();
}

void WOTBMOD_V3_CALL OnUnload(
    const WotbModV3Bootstrap*, WotbModV3Handle) {
    Cleanup();
}
}

WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleUi_GetState() { return g_state; }

WOTBMOD_V3_EXPORT const char* WOTBMOD_V3_CALL
WotbSampleUi_GetInspection() { return g_inspection; }

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info || mod == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    strcpy_s(out_info->id, "sample.ui_transaction");
    strcpy_s(out_info->name, "RC1 UI Transaction Sample");
    strcpy_s(out_info->version, "1.0.0-rc1");
    strcpy_s(out_info->author, "WotbMod SDK");
    strcpy_s(out_info->description,
             "Stock UI inspection, safe mutation, managed layout, event, rollback.");
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    return WOTBMOD_V3_OK;
}
