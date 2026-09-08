#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/ui_v3.h"
#include "../src/v3/client_services_backend.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>

namespace {

using wotbmod::v3::ClientHostBackend;
using wotbmod::v3::ClientHostObjectRequest;
using wotbmod::v3::ClientHostObjectResponse;

struct NativeUi {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    std::string name;
    WotbModV3Rect geometry = {};
    uint64_t parent = 0u;
    uint32_t visible = 1u;
    uint32_t enabled = 1u;
    uint32_t interactable = 1u;
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    WotbModV3Color background = {0.10f, 0.12f, 0.16f, 0.92f};
    float opacity = 1.0f;
};

std::unordered_map<uint64_t, NativeUi> g_native;
uint64_t g_nextNative = 1u;
uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
uint32_t g_slotVisits = 0u;
uint32_t g_uiEvents = 0u;
uint64_t g_lastCreatedNative = 0u;

#define CHECK(expression)                                                \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(expression)) {                                             \
            ++g_failures;                                                \
            std::fprintf(                                                \
                stderr,                                                  \
                "check failed at line %d: %s\n",                       \
                __LINE__,                                                \
                #expression);                                            \
        }                                                                \
    } while (0)

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.001f;
}

bool ValidRequest(
    const void* request,
    uint32_t requestSize,
    const ClientHostObjectRequest** output) {
    if (!request || requestSize < sizeof(ClientHostObjectRequest) ||
        !output) {
        return false;
    }
    const ClientHostObjectRequest* typed =
        static_cast<const ClientHostObjectRequest*>(request);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *output = typed;
    return true;
}

bool ValidResponse(
    void* response,
    uint32_t responseSize,
    ClientHostObjectResponse** output) {
    if (!response || responseSize < sizeof(ClientHostObjectResponse) ||
        !output) {
        return false;
    }
    ClientHostObjectResponse* typed =
        static_cast<ClientHostObjectResponse*>(response);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *output = typed;
    return true;
}

NativeUi* Find(
    WotbModV3Handle owner,
    uint64_t object) {
    const auto it = g_native.find(object);
    if (it == g_native.end() || it->second.owner != owner) {
        return nullptr;
    }
    return &it->second;
}

WotbModV3Result WOTBMOD_V3_CALL InvokeUiHost(
    void*,
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t requestSize,
    void* response,
    uint32_t responseSize) {
    if (!operation) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "ui_get_active_screen") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(response, responseSize, &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const uint64_t object = g_nextNative++;
        NativeUi native = {};
        native.owner = mod;
        native.geometry = {0.0f, 0.0f, 1920.0f, 1080.0f};
        g_native.emplace(object, native);
        objectResponse->object = object;
        objectResponse->rect = native.geometry;
        objectResponse->value_u32 = 7u;
        return WOTBMOD_V3_OK;
    }
    const ClientHostObjectRequest* objectRequest = nullptr;
    if (!ValidRequest(request, requestSize, &objectRequest)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "ui_control_create") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(
                response, responseSize, &objectResponse) ||
            !objectRequest->payload ||
            objectRequest->payload_size <
                sizeof(WotbModV3UiControlDescriptor)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const WotbModV3UiControlDescriptor* descriptor =
            static_cast<const WotbModV3UiControlDescriptor*>(
                objectRequest->payload);
        if (descriptor->type !=
            WOTBMOD_V3_UI_CONTROL_CONTAINER) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        const uint64_t object = g_nextNative++;
        NativeUi native = {};
        native.owner = mod;
        native.name = descriptor->id ? descriptor->id : "";
        native.geometry = descriptor->geometry;
        native.visible = descriptor->visible != 0u ? 1u : 0u;
        g_native.emplace(object, native);
        g_lastCreatedNative = object;
        objectResponse->object = object;
        return WOTBMOD_V3_OK;
    }
    NativeUi* native = Find(mod, objectRequest->object);
    if (!native) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    if (std::strcmp(operation, "ui_control_clone") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(response, responseSize, &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const uint64_t object = g_nextNative++;
        NativeUi clone = *native;
        clone.parent = 0u;
        g_native.emplace(object, clone);
        objectResponse->object = object;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_find_by_name") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(response, responseSize, &objectResponse) ||
            !objectRequest->name) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        for (const auto& candidate : g_native) {
            if (candidate.second.owner == mod &&
                candidate.second.name == objectRequest->name &&
                candidate.second.parent == objectRequest->object) {
                objectResponse->object = candidate.first;
                objectResponse->rect = candidate.second.geometry;
                objectResponse->value_u32 =
                    (candidate.second.visible ? 1u : 0u) |
                    (candidate.second.interactable ? 2u : 0u) |
                    (candidate.second.enabled ? 4u : 0u);
                return WOTBMOD_V3_OK;
            }
        }
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    if (std::strcmp(operation, "ui_control_destroy") == 0) {
        g_native.erase(objectRequest->object);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_set_geometry") == 0) {
        native->geometry = {
            objectRequest->vector.x,
            objectRequest->vector.y,
            objectRequest->vector.z,
            objectRequest->vector.w};
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_get_state") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(response, responseSize, &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        objectResponse->rect = native->geometry;
        objectResponse->value_u32 =
            (native->visible != 0u ? 1u : 0u) |
            (native->interactable != 0u ? 2u : 0u) |
            (native->enabled != 0u ? 4u : 0u);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_get_child_count") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(response, responseSize, &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        uint32_t count = 0u;
        for (const auto& candidate : g_native) {
            if (candidate.second.owner == mod &&
                candidate.second.parent == objectRequest->object) {
                ++count;
            }
        }
        objectResponse->value_u32 = count;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_set_visible") == 0) {
        native->visible = objectRequest->flags != 0u ? 1u : 0u;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_set_color") == 0) {
        native->color = {
            objectRequest->vector.x,
            objectRequest->vector.y,
            objectRequest->vector.z,
            objectRequest->vector.w};
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(
            operation,
            "ui_control_set_background_color") == 0) {
        native->background = {
            objectRequest->vector.x,
            objectRequest->vector.y,
            objectRequest->vector.z,
            objectRequest->vector.w};
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_set_opacity") == 0) {
        native->opacity = static_cast<float>(objectRequest->scalar0);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_set_enabled") == 0) {
        native->enabled = objectRequest->flags != 0u ? 1u : 0u;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(
            operation, "ui_control_set_interactable") == 0) {
        native->interactable =
            objectRequest->flags != 0u ? 1u : 0u;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_control_set_parent") == 0) {
        if (objectRequest->related_object != 0u &&
            !Find(mod, objectRequest->related_object)) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        native->parent = objectRequest->related_object;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "ui_slot_attach") == 0) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (std::strcmp(operation, "ui_slot_detach") == 0) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

void Copy(char* output, size_t capacity, const char* value) {
#if defined(_MSC_VER)
    strncpy_s(output, capacity, value, _TRUNCATE);
#else
    std::strncpy(output, value, capacity - 1u);
    output[capacity - 1u] = '\0';
#endif
}

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(info->id, sizeof(info->id), "tests.ui-public-bridge");
    Copy(info->name, sizeof(info->name), "UI public bridge test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

void WOTBMOD_V3_CALL IgnoreUiEvent(
    WotbModV3Handle,
    const WotbModV3UiEvent* event,
    void*) {
    if (event && event->type == WOTBMOD_V3_UI_EVENT_CLICK) {
        ++g_uiEvents;
    }
}

WotbModV3Result WOTBMOD_V3_CALL VisitUiSlot(
    WotbModV3Handle,
    const char*,
    void*) {
    ++g_slotVisits;
    return WOTBMOD_V3_OK;
}

}  // namespace

int main() {
    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_ui_public_tests\\mods";
    options.cache_directory = "build\\v3_ui_public_tests\\cache";
    options.config_directory = "build\\v3_ui_public_tests\\config";
    options.client_version = "ui-public-test";
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);

    ClientHostBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    backend.invoke = &InvokeUiHost;
    wotbmod::v3::SetClientHostBackend(&backend);

    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "ui_public_bridge_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            mod, &TestEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK);

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);
    const void* table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_UI,
            WOTBMOD_V3_UI_VERSION,
            &table) == WOTBMOD_V3_OK);
    const WotbModV3UiApiV2* ui =
        static_cast<const WotbModV3UiApiV2*>(table);
    CHECK(ui != nullptr);
    const void* uiV3Table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_UI,
            WOTBMOD_V3_UI_VERSION_3,
            &uiV3Table) == WOTBMOD_V3_OK);
    const WotbModV3UiApiV3* uiV3 =
        static_cast<const WotbModV3UiApiV3*>(uiV3Table);
    CHECK(uiV3 != nullptr);

    WotbModV3Vec2 viewportSize = {};
    CHECK(
        ui->get_viewport_size(mod, &viewportSize) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);

    WotbModV3UiControlDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_UI_VERSION);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_TEXT;
    descriptor.visible = 1u;
    descriptor.geometry = {1.0f, 2.0f, 100.0f, 50.0f};
    WotbModV3UiHandle unsupported =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->control_create(mod, &descriptor, &unsupported) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(g_native.empty());

    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    WotbModV3UiHandle first = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->control_create(mod, &descriptor, &first) ==
        WOTBMOD_V3_OK);
    CHECK(first != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(g_native.size() == 1u);

    CHECK(
        ui->control_set_position(mod, first, {30.0f, 40.0f}) ==
        WOTBMOD_V3_OK);
    CHECK(
        ui->control_set_size(mod, first, {640.0f, 360.0f}) ==
        WOTBMOD_V3_OK);
    WotbModV3Vec2 position = {};
    WotbModV3Vec2 size = {};
    CHECK(
        ui->control_get_position(mod, first, &position) ==
        WOTBMOD_V3_OK);
    CHECK(ui->control_get_size(mod, first, &size) == WOTBMOD_V3_OK);
    CHECK(Near(position.x, 30.0f) && Near(position.y, 40.0f));
    CHECK(Near(size.x, 640.0f) && Near(size.y, 360.0f));

    const uint64_t nativeChild = g_nextNative++;
    NativeUi childFixture = {};
    childFixture.owner = mod;
    childFixture.name = "OpenButton";
    childFixture.geometry = {50.0f, 60.0f, 180.0f, 70.0f};
    childFixture.parent = g_lastCreatedNative;
    g_native.emplace(nativeChild, childFixture);

    WotbModV3UiHandle foundChild = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->control_find_by_id(mod, first, "OpenButton", &foundChild) ==
        WOTBMOD_V3_OK);
    CHECK(foundChild != WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3UiHandle foundParent = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->control_get_parent(mod, foundChild, &foundParent) ==
        WOTBMOD_V3_OK);
    CHECK(foundParent == first);

    WotbModV3Token childEventToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->event_subscribe(
            mod,
            foundChild,
            WOTBMOD_V3_UI_EVENT_CLICK,
            &IgnoreUiEvent,
            nullptr,
            &childEventToken) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyClientHostUiInput(
            1u, 90.0f, 110.0f, 0.0f, 0.0f, 0u) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyClientHostUiInput(
            1u, 90.0f, 110.0f, 0.0f, 0.0f, 0u) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyClientHostUiInput(
            3u, 90.0f, 110.0f, 0.0f, 0.0f, 0u) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyClientHostUiInput(
            3u, 90.0f, 110.0f, 0.0f, 0.0f, 0u) ==
        WOTBMOD_V3_OK);
    wotbmod::v3::ClientHostFrame childFrame = {};
    childFrame.struct_size = sizeof(childFrame);
    childFrame.api_version = WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    childFrame.frame_index = 1u;
    childFrame.viewport = {0.0f, 0.0f, 1920.0f, 1080.0f};
    wotbmod::v3::SetClientHostUiViewportSize(1536u, 864u);
    wotbmod::v3::PumpClientHostFrame(&childFrame);
    CHECK(wotbmod::v3::ShouldCaptureClientHostUiInput(90.0f, 110.0f));
    CHECK(!wotbmod::v3::ShouldCaptureClientHostUiInput(5.0f, 5.0f));
    viewportSize = {};
    CHECK(
        ui->get_viewport_size(mod, &viewportSize) == WOTBMOD_V3_OK);
    CHECK(
        Near(viewportSize.x, 1536.0f) &&
        Near(viewportSize.y, 864.0f));
    CHECK(g_uiEvents == 1u);
    CHECK(
        ui->event_unsubscribe(mod, childEventToken) == WOTBMOD_V3_OK);
    wotbmod::v3::RefreshClientHostUiCaptureSnapshot();
    CHECK(!wotbmod::v3::ShouldCaptureClientHostUiInput(90.0f, 110.0f));
    childFrame.frame_index = 2u;
    wotbmod::v3::PumpClientHostFrame(&childFrame);
    CHECK(!wotbmod::v3::ShouldCaptureClientHostUiInput(90.0f, 110.0f));
    CHECK(ui->control_destroy(mod, foundChild) == WOTBMOD_V3_OK);
    foundChild = WOTBMOD_V3_INVALID_HANDLE;

    CHECK(ui->control_set_visible(mod, first, 0u) == WOTBMOD_V3_OK);
    CHECK(ui->control_set_enabled(mod, first, 0u) == WOTBMOD_V3_OK);
    CHECK(
        ui->control_set_interactable(mod, first, 0u) ==
        WOTBMOD_V3_OK);

    CHECK(
        ui->control_set_text(mod, first, "not rendered") ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        ui->control_set_anchor(mod, first, {0.5f, 0.5f}) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    WotbModV3UiLayoutDescriptor layout = {};
    WOTBMOD_V3_INIT_STRUCT(layout, WOTBMOD_V3_UI_VERSION);
    CHECK(
        ui->layout_set(mod, first, &layout) == WOTBMOD_V3_OK);
    CHECK(ui->layout_invalidate(mod, first) == WOTBMOD_V3_OK);
    WotbModV3Token eventToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->event_subscribe(
            mod,
            first,
            WOTBMOD_V3_UI_EVENT_CLICK,
            &IgnoreUiEvent,
            nullptr,
            &eventToken) == WOTBMOD_V3_OK);
    CHECK(eventToken != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(ui->control_set_visible(mod, first, 1u) == WOTBMOD_V3_OK);
    CHECK(ui->control_set_enabled(mod, first, 1u) == WOTBMOD_V3_OK);
    CHECK(
        ui->control_set_interactable(mod, first, 1u) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyClientHostUiInput(
            1u, 50.0f, 50.0f, 0.0f, 0.0f, 0u) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyClientHostUiInput(
            3u, 50.0f, 50.0f, 0.0f, 0.0f, 0u) ==
        WOTBMOD_V3_OK);
    wotbmod::v3::ClientHostFrame frame = {};
    frame.struct_size = sizeof(frame);
    frame.api_version = WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    frame.render_backend = WOTBMOD_V3_RENDER_BACKEND_NONE;
    frame.frame_index = 3u;
    frame.delta_seconds = 1.0 / 60.0;
    frame.viewport = {0.0f, 0.0f, 1920.0f, 1080.0f};
    wotbmod::v3::PumpClientHostFrame(&frame);
    CHECK(g_uiEvents == 2u);
    CHECK(ui->event_unsubscribe(mod, eventToken) == WOTBMOD_V3_OK);
    WotbModV3UiStylePatch style = {};
    WOTBMOD_V3_INIT_STRUCT(style, WOTBMOD_V3_UI_VERSION);
    style.fields = WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR |
                   WOTBMOD_V3_UI_STYLE_OPACITY;
    style.background_color = {0.2f, 0.3f, 0.4f, 0.8f};
    style.opacity = 0.75f;
    WotbModV3Handle styleHandle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->style_push(mod, first, &style, &styleHandle) ==
        WOTBMOD_V3_OK);
    CHECK(styleHandle != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        Near(g_native[g_lastCreatedNative].background.r, 0.2f) &&
        Near(g_native[g_lastCreatedNative].background.a, 0.8f) &&
        Near(g_native[g_lastCreatedNative].opacity, 0.75f));
    style.background_color = {0.6f, 0.5f, 0.4f, 1.0f};
    style.opacity = 0.5f;
    CHECK(
        ui->style_update(mod, styleHandle, &style) ==
        WOTBMOD_V3_OK);
    CHECK(
        Near(g_native[g_lastCreatedNative].background.r, 0.6f) &&
        Near(g_native[g_lastCreatedNative].opacity, 0.5f));
    CHECK(ui->style_pop(mod, styleHandle) == WOTBMOD_V3_OK);
    CHECK(
        Near(g_native[g_lastCreatedNative].background.r, 0.10f) &&
        Near(g_native[g_lastCreatedNative].background.a, 0.92f) &&
        Near(g_native[g_lastCreatedNative].opacity, 1.0f));

    WotbModV3UiHandle clone = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(ui->control_clone(mod, first, &clone) == WOTBMOD_V3_OK);
    CHECK(g_native.size() == 2u);
    CHECK(ui->control_add_child(mod, first, clone) == WOTBMOD_V3_OK);
    WotbModV3UiHandle parent = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->control_get_parent(mod, clone, &parent) ==
        WOTBMOD_V3_OK);
    CHECK(parent == first);
    layout.type = WOTBMOD_V3_UI_LAYOUT_HORIZONTAL;
    layout.direction = WOTBMOD_V3_UI_DIRECTION_LEFT_TO_RIGHT;
    layout.main_alignment = WOTBMOD_V3_UI_ALIGN_STRETCH;
    layout.cross_alignment = WOTBMOD_V3_UI_ALIGN_STRETCH;
    CHECK(ui->layout_set(mod, first, &layout) == WOTBMOD_V3_OK);
    CHECK(ui->layout_invalidate(mod, first) == WOTBMOD_V3_OK);
    CHECK(
        ui->control_get_position(mod, clone, &position) ==
        WOTBMOD_V3_OK);
    CHECK(ui->control_get_size(mod, clone, &size) == WOTBMOD_V3_OK);
    CHECK(Near(position.x, 0.0f) && Near(position.y, 0.0f));
    CHECK(Near(size.x, 640.0f) && Near(size.y, 360.0f));
    CHECK(
        ui->control_remove_child(mod, first, clone) ==
        WOTBMOD_V3_OK);

    WotbModV3UiHandle slot = 0x1234u;
    CHECK(
        ui->slot_find(mod, "hangar.top_bar.right", &slot) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(slot == WOTBMOD_V3_INVALID_HANDLE);
    slot = 0x1234u;
    CHECK(
        ui->slot_find(mod, "hangar.unknown", &slot) ==
        WOTBMOD_V3_E_NOT_FOUND);
    CHECK(slot == WOTBMOD_V3_INVALID_HANDLE);
    g_slotVisits = 0u;
    CHECK(
        ui->slot_enumerate(mod, &VisitUiSlot, nullptr) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(g_slotVisits == 0u);
    CHECK(
        ui->slot_attach(mod, slot, clone, 10) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        ui->slot_detach(mod, slot, clone) ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    WotbModV3UiHandle activeScreen = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        uiV3->get_active_screen(mod, &activeScreen) ==
        WOTBMOD_V3_OK);
    WotbModV3UiControlSnapshot snapshot = {};
    WOTBMOD_V3_INIT_STRUCT(snapshot, WOTBMOD_V3_UI_VERSION_3);
    CHECK(
        uiV3->control_get_snapshot(mod, activeScreen, &snapshot) ==
        WOTBMOD_V3_OK);
    CHECK(
        (snapshot.flags & WOTBMOD_V3_UI_SNAPSHOT_GAME_OWNED) != 0u &&
        Near(snapshot.geometry.width, 1920.0f) &&
        Near(snapshot.geometry.height, 1080.0f));
    CHECK(ui->control_destroy(mod, activeScreen) == WOTBMOD_V3_OK);

    CHECK(ui->control_destroy(mod, first) == WOTBMOD_V3_OK);
    CHECK(g_native.size() == 1u);
    CHECK(WotbModV3Runtime_Disable(mod) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK);
    CHECK(g_native.empty());
    WotbModV3Runtime_Shutdown();

    std::printf(
        "v3 public UI bridge checks: %u, failures: %u\n",
        g_checks,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
