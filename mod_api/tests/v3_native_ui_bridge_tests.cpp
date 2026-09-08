#include "../loader/v3_native_client_services.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <new>
#include <string>
#include <vector>

#include "../include/wotbmod/ui_v2.h"

namespace {

struct FakeUi {
    uint32_t references = 1u;
    WotbModUiControlGeometry geometry = {};
    uint32_t visible = 1u;
    uint32_t input_enabled = 1u;
    uint32_t disabled = 0u;
    FakeUi* parent = nullptr;
    std::vector<FakeUi*> children;
    WotbModSceneTransform transform = {};
    bool scene = false;
};

uint32_t g_allocations = 0u;
uint32_t g_failures = 0u;
uint32_t g_passes = 0u;
FakeUi* g_last_scene_entity = nullptr;
FakeUi* g_last_active_scene = nullptr;
uint32_t g_resolved_loads = 0u;
WotbModResourceType g_last_resolved_type = WOTBMOD_RESOURCE_GENERIC;
std::string g_last_resolved_path;
std::string g_last_resolved_object;
std::string g_last_resolved_contents;
uint32_t g_clone_calls = 0u;
uint32_t g_add_child_calls = 0u;
uint32_t g_remove_child_calls = 0u;
uint32_t g_bring_to_front_calls = 0u;
bool g_destroy_during_resolved_load = false;
bool g_destroy_during_add_child = false;
bool g_shutdown_during_set_visible = false;
bool g_shutdown_during_release = false;
bool g_shutdown_released_other_ui_inside_callback = false;
uint32_t g_expected_allocations_during_shutdown_release = 0u;
WotbModV3Handle g_reentrant_destroy_owner = WOTBMOD_V3_INVALID_HANDLE;
uint64_t g_reentrant_destroy_object = 0u;
WotbModV3Result g_reentrant_destroy_result = WOTBMOD_V3_E_PLATFORM;
FakeUi* g_last_allocated_ui = nullptr;
FakeUi* g_reentrant_release_target = nullptr;
bool g_reentrant_target_released = false;
bool g_release_observed_inside_callback = false;

#define CHECK(expression)                                                \
    do {                                                                 \
        if (!(expression)) {                                             \
            std::fprintf(                                                \
                stderr,                                                  \
                "check failed at line %d: %s\n",                       \
                __LINE__,                                                \
                #expression);                                            \
            ++g_failures;                                                \
        } else {                                                         \
            ++g_passes;                                                  \
        }                                                                \
    } while (0)

void PumpNativeFrames(uint32_t count) {
    for (uint32_t frame = 0u; frame < count; ++frame) {
        wotbmod::loader::PumpV3NativeClientServicesFrame();
    }
}

FakeUi* AllocateUi(
    const WotbModUiControlGeometry* geometry = nullptr) {
    FakeUi* ui = new (std::nothrow) FakeUi();
    if (!ui) return nullptr;
    ui->geometry.struct_size = sizeof(ui->geometry);
    if (geometry) ui->geometry = *geometry;
    g_last_allocated_ui = ui;
    ++g_allocations;
    return ui;
}

WotbModResult WOTBMOD_CALL FakeRelease(
    void*,
    void* native_resource) {
    FakeUi* ui = static_cast<FakeUi*>(native_resource);
    if (!ui) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (g_shutdown_during_release) {
        g_shutdown_during_release = false;
        wotbmod::loader::ShutdownV3NativeClientServices();
        g_shutdown_released_other_ui_inside_callback =
            g_allocations !=
            g_expected_allocations_during_shutdown_release;
    }
    if (ui == g_reentrant_release_target) {
        g_reentrant_target_released = true;
    }
    if (ui->references > 1u) {
        --ui->references;
        return WOTBMOD_OK;
    }
    if (ui->parent) {
        std::vector<FakeUi*>& children = ui->parent->children;
        children.erase(
            std::remove(children.begin(), children.end(), ui),
            children.end());
    }
    for (FakeUi* child : ui->children) {
        if (child && child->parent == ui) child->parent = nullptr;
    }
    delete ui;
    --g_allocations;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeUiGetParent(
    void*,
    void* native_resource,
    void** output) {
    FakeUi* ui = static_cast<FakeUi*>(native_resource);
    if (!ui || !output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *output = nullptr;
    if (!ui->parent) return WOTBMOD_ERROR_NOT_FOUND;
    ++ui->parent->references;
    *output = ui->parent;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeUiGetChildCount(
    void*,
    void* native_resource,
    uint32_t* output) {
    FakeUi* ui = static_cast<FakeUi*>(native_resource);
    if (!ui || !output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *output = static_cast<uint32_t>(ui->children.size());
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeUiGetChildAt(
    void*,
    void* native_resource,
    uint32_t index,
    void** output) {
    FakeUi* ui = static_cast<FakeUi*>(native_resource);
    if (!ui || !output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *output = nullptr;
    if (index >= ui->children.size()) return WOTBMOD_ERROR_NOT_FOUND;
    FakeUi* child = ui->children[index];
    ++child->references;
    *output = child;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeBringUiToFront(
    void*,
    void* native_resource) {
    FakeUi* child = static_cast<FakeUi*>(native_resource);
    if (!child || !child->parent) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    FakeUi* parent = child->parent;
    parent->children.erase(
        std::remove(
            parent->children.begin(),
            parent->children.end(),
            child),
        parent->children.end());
    parent->children.push_back(child);
    ++g_bring_to_front_calls;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeUiCreate(
    void*,
    const WotbModUiControlGeometry* geometry,
    void** output) {
    if (!geometry || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *output = AllocateUi(geometry);
    return *output ? WOTBMOD_OK : WOTBMOD_ERROR_LIMIT_REACHED;
}

WotbModResult WOTBMOD_CALL FakeLoadResolved(
    void*,
    WotbModResourceType type,
    const char* resolved_path,
    const char* object_name,
    uint32_t,
    void** output) {
    if (!resolved_path || !object_name || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (g_destroy_during_resolved_load) {
        g_destroy_during_resolved_load = false;
        wotbmod::v3::ClientHostObjectRequest request = {};
        request.struct_size = sizeof(request);
        request.api_version =
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
        request.object = g_reentrant_destroy_object;
        g_reentrant_destroy_result =
            wotbmod::loader::InvokeV3NativeClientServices(
                g_reentrant_destroy_owner,
                "ui_control_destroy",
                &request,
                sizeof(request),
                nullptr,
                0u);
        if (g_reentrant_target_released) {
            g_release_observed_inside_callback = true;
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }
    WotbModUiControlGeometry geometry = {};
    geometry.struct_size = sizeof(geometry);
    geometry.width = 340.0f;
    geometry.height = 160.0f;
    *output = AllocateUi(&geometry);
    if (!*output) return WOTBMOD_ERROR_LIMIT_REACHED;
    ++g_resolved_loads;
    g_last_resolved_type = type;
    g_last_resolved_path = resolved_path;
    g_last_resolved_object = object_name;
    std::ifstream generated(resolved_path, std::ios::binary);
    g_last_resolved_contents.assign(
        std::istreambuf_iterator<char>(generated),
        std::istreambuf_iterator<char>());
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeGetScreen(
    void*,
    void** output) {
    if (!output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    WotbModUiControlGeometry geometry = {};
    geometry.struct_size = sizeof(geometry);
    geometry.width = 1920.0f;
    geometry.height = 1080.0f;
    *output = AllocateUi(&geometry);
    return *output ? WOTBMOD_OK : WOTBMOD_ERROR_LIMIT_REACHED;
}

WotbModResult WOTBMOD_CALL FakeClone(
    void*,
    void* native_resource,
    void** output) {
    if (!native_resource || !output) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    FakeUi* source = static_cast<FakeUi*>(native_resource);
    FakeUi* clone = AllocateUi(&source->geometry);
    if (!clone) return WOTBMOD_ERROR_LIMIT_REACHED;
    ++g_clone_calls;
    clone->visible = source->visible;
    clone->input_enabled = source->input_enabled;
    clone->disabled = source->disabled;
    *output = clone;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeSetGeometry(
    void*,
    void* native_resource,
    const WotbModUiControlGeometry* geometry) {
    if (!native_resource || !geometry) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    static_cast<FakeUi*>(native_resource)->geometry = *geometry;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeSetVisible(
    void*,
    void* native_resource,
    int32_t visible) {
    if (!native_resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (g_shutdown_during_set_visible) {
        g_shutdown_during_set_visible = false;
        wotbmod::loader::ShutdownV3NativeClientServices();
    }
    static_cast<FakeUi*>(native_resource)->visible =
        visible != 0 ? 1u : 0u;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeAddChild(
    void*,
    void* parent_resource,
    void* child_resource) {
    FakeUi* parent = static_cast<FakeUi*>(parent_resource);
    FakeUi* child = static_cast<FakeUi*>(child_resource);
    if (!parent || !child || parent == child) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    ++g_add_child_calls;
    if (g_destroy_during_add_child) {
        g_destroy_during_add_child = false;
        wotbmod::v3::ClientHostObjectRequest request = {};
        request.struct_size = sizeof(request);
        request.api_version =
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
        request.object = g_reentrant_destroy_object;
        g_reentrant_destroy_result =
            wotbmod::loader::InvokeV3NativeClientServices(
                g_reentrant_destroy_owner,
                "ui_control_destroy",
                &request,
                sizeof(request),
                nullptr,
                0u);
        if (g_reentrant_target_released) {
            g_release_observed_inside_callback = true;
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }
    if (child->parent == parent) {
        parent->children.erase(
            std::remove(
                parent->children.begin(),
                parent->children.end(),
                child),
            parent->children.end());
        parent->children.push_back(child);
        return WOTBMOD_OK;
    }
    if (child->parent) return WOTBMOD_ERROR_ALREADY_EXISTS;
    child->parent = parent;
    parent->children.push_back(child);
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeRemoveChild(
    void*,
    void* parent_resource,
    void* child_resource) {
    FakeUi* parent = static_cast<FakeUi*>(parent_resource);
    FakeUi* child = static_cast<FakeUi*>(child_resource);
    if (!parent || !child || child->parent != parent) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    ++g_remove_child_calls;
    parent->children.erase(
        std::remove(
            parent->children.begin(),
            parent->children.end(),
            child),
        parent->children.end());
    child->parent = nullptr;
    return WOTBMOD_OK;
}

WotbModResult g_live_text_result = WOTBMOD_OK;

WotbModResult WOTBMOD_CALL FakeGetText(
    void*,
    void* native_resource,
    char* buffer,
    uint32_t* inout_size) {
    if (!native_resource || !inout_size) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (g_live_text_result != WOTBMOD_OK) {
        *inout_size = 0u;
        return g_live_text_result;
    }
    static const char kText[] = "Live text 42";
    const uint32_t length = static_cast<uint32_t>(sizeof(kText) - 1u);
    if (!buffer || *inout_size <= length) {
        *inout_size = length + 1u;
        return WOTBMOD_ERROR_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, kText, sizeof(kText));
    *inout_size = length;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeGetState(
    void*,
    void* native_resource,
    WotbModUiControlState* output) {
    FakeUi* ui = static_cast<FakeUi*>(native_resource);
    if (!ui || !output ||
        output->struct_size < sizeof(*output)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    output->geometry = ui->geometry;
    output->flags =
        (ui->visible ? WOTBMOD_UI_CONTROL_VISIBLE : 0u) |
        (ui->input_enabled
             ? WOTBMOD_UI_CONTROL_INPUT_ENABLED
             : 0u) |
        (ui->disabled ? WOTBMOD_UI_CONTROL_DISABLED : 0u);
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeSetInputEnabled(
    void*,
    void* native_resource,
    int32_t value,
    int32_t) {
    if (!native_resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    static_cast<FakeUi*>(native_resource)->input_enabled =
        value != 0 ? 1u : 0u;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeSetDisabled(
    void*,
    void* native_resource,
    int32_t value,
    int32_t) {
    if (!native_resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    static_cast<FakeUi*>(native_resource)->disabled =
        value != 0 ? 1u : 0u;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeSceneCreate(
    void*,
    void** output) {
    if (!output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    FakeUi* scene = AllocateUi();
    if (!scene) return WOTBMOD_ERROR_LIMIT_REACHED;
    scene->scene = true;
    g_last_scene_entity = scene;
    *output = scene;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeSceneGetActive(
    void*,
    void** output) {
    if (!output) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    FakeUi* scene = AllocateUi();
    if (!scene) return WOTBMOD_ERROR_LIMIT_REACHED;
    scene->scene = true;
    g_last_active_scene = scene;
    *output = scene;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL FakeSceneSetTransform(
    void*,
    void* native_resource,
    const WotbModSceneTransform* transform) {
    FakeUi* scene = static_cast<FakeUi*>(native_resource);
    if (!scene || !scene->scene || !transform) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    scene->transform = *transform;
    return WOTBMOD_OK;
}

wotbmod::v3::ClientHostObjectRequest Request() {
    wotbmod::v3::ClientHostObjectRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return request;
}

wotbmod::v3::ClientHostObjectResponse Response() {
    wotbmod::v3::ClientHostObjectResponse response = {};
    response.struct_size = sizeof(response);
    response.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return response;
}

WotbModV3Result Invoke(
    WotbModV3Handle mod,
    const char* operation,
    const wotbmod::v3::ClientHostObjectRequest* request,
    wotbmod::v3::ClientHostObjectResponse* response = nullptr) {
    return wotbmod::loader::InvokeV3NativeClientServices(
        mod,
        operation,
        request,
        request ? sizeof(*request) : 0u,
        response,
        response ? sizeof(*response) : 0u);
}

}  // namespace

int main() {
    WotbModRuntimeResourceBackend resource = {};
    resource.struct_size = sizeof(resource);
    resource.release = &FakeRelease;
    resource.ui_set_geometry = &FakeSetGeometry;
    resource.ui_set_visible = &FakeSetVisible;
    resource.ui_add_child = &FakeAddChild;
    resource.ui_remove_child = &FakeRemoveChild;
    resource.ui_create = &FakeUiCreate;
    resource.ui_get_active_screen = &FakeGetScreen;
    resource.ui_get_parent = &FakeUiGetParent;
    resource.ui_get_child_count = &FakeUiGetChildCount;
    resource.ui_get_child_at = &FakeUiGetChildAt;
    resource.clone = &FakeClone;
    resource.ui_get_state = &FakeGetState;
    resource.ui_get_text = &FakeGetText;
    resource.ui_set_input_enabled = &FakeSetInputEnabled;
    resource.ui_set_disabled = &FakeSetDisabled;
    resource.load_resolved = &FakeLoadResolved;
    resource.scene_entity_create = &FakeSceneCreate;
    resource.scene_get_active = &FakeSceneGetActive;
    resource.scene_set_transform = &FakeSceneSetTransform;
    resource.scene_add_child = &FakeAddChild;
    resource.scene_remove_child = &FakeRemoveChild;

    wotbmod::loader::V3NativeClientServicesOptions options = {};
    options.struct_size = sizeof(options);
    options.resource_backend = &resource;
    options.resource_bring_ui_to_front = &FakeBringUiToFront;
    CHECK(
        wotbmod::loader::InitializeV3NativeClientServices(
            &options) == WOTBMOD_V3_OK);

    const WotbModV3Handle owner = 0x101u;
    WotbModV3UiControlDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_UI_VERSION);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.visible = 1u;
    descriptor.geometry = {10.0f, 20.0f, 300.0f, 180.0f};

    wotbmod::v3::ClientHostObjectRequest request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse first = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &first) ==
        WOTBMOD_V3_OK);
    CHECK(first.object != 0u);
    CHECK(g_allocations == 1u);

    // Live engine text rides the ui_read_string slot under its own field:
    // the provider's string with the LIVE_TEXT _SET bit, an empty value with
    // the bit clear when the control has no text component, NOT_SUPPORTED
    // when the provider has no such slot. The mirror fields are untouched.
    {
        wotbmod::v3::ClientHostUiReadRequest read = {};
        read.struct_size = sizeof(read);
        read.api_version = WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
        read.object = first.object;
        read.mod = owner;
        read.field = wotbmod::v3::CLIENT_HOST_UI_READ_FIELD_LIVE_TEXT;
        wotbmod::v3::ClientHostUiReadString value = {};
        value.struct_size = sizeof(value);
        value.api_version = WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
        CHECK(
            wotbmod::loader::V3NativeUiReadString(nullptr, &read, &value) ==
            WOTBMOD_V3_OK);
        CHECK(value.flags == WOTBMOD_V3_UI_READ_LIVE_TEXT_SET);
        CHECK(value.length == 12u);
        CHECK(std::strcmp(value.value, "Live text 42") == 0);
        g_live_text_result = WOTBMOD_ERROR_NOT_FOUND;
        value = {};
        value.struct_size = sizeof(value);
        value.api_version = WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
        CHECK(
            wotbmod::loader::V3NativeUiReadString(nullptr, &read, &value) ==
            WOTBMOD_V3_OK);
        CHECK(value.flags == 0u && value.length == 0u && value.value[0] == '\0');
        g_live_text_result = WOTBMOD_OK;
        read.field = wotbmod::v3::CLIENT_HOST_UI_READ_FIELD_TEXT;
        value = {};
        value.struct_size = sizeof(value);
        value.api_version = WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
        CHECK(
            wotbmod::loader::V3NativeUiReadString(nullptr, &read, &value) ==
            WOTBMOD_V3_OK);
        CHECK((value.flags & WOTBMOD_V3_UI_READ_LIVE_TEXT_SET) == 0u);
    }

    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.text = nullptr;
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse direct_parent = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &direct_parent) ==
        WOTBMOD_V3_OK);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_BUTTON;
    descriptor.text = "Destroy inside set_parent";
    wotbmod::v3::ClientHostObjectResponse direct_child = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &direct_child) ==
        WOTBMOD_V3_OK);
    FakeUi* const direct_child_resource = g_last_allocated_ui;
    g_reentrant_destroy_owner = owner;
    g_reentrant_destroy_object = direct_child.object;
    g_reentrant_destroy_result = WOTBMOD_V3_E_PLATFORM;
    g_reentrant_release_target = direct_child_resource;
    g_reentrant_target_released = false;
    g_release_observed_inside_callback = false;
    g_destroy_during_add_child = true;
    request = Request();
    request.object = direct_child.object;
    request.related_object = direct_parent.object;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_E_OBJECT_DESTROYED);
    CHECK(g_reentrant_destroy_result == WOTBMOD_V3_OK);
    CHECK(!g_release_observed_inside_callback);
    CHECK(g_reentrant_target_released);
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    g_reentrant_release_target = nullptr;
    CHECK(g_allocations == 2u);
    request = Request();
    request.object = direct_parent.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 1u);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_TEXT;
    descriptor.text = "Destroy inside clone";
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse clone_source = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &clone_source) ==
        WOTBMOD_V3_OK);
    FakeUi* const clone_source_resource = g_last_allocated_ui;
    g_reentrant_destroy_owner = owner;
    g_reentrant_destroy_object = clone_source.object;
    g_reentrant_destroy_result = WOTBMOD_V3_E_PLATFORM;
    g_reentrant_release_target = clone_source_resource;
    g_reentrant_target_released = false;
    g_release_observed_inside_callback = false;
    g_destroy_during_resolved_load = true;
    request = Request();
    request.object = clone_source.object;
    wotbmod::v3::ClientHostObjectResponse destroyed_clone = Response();
    CHECK(
        Invoke(owner, "ui_control_clone", &request, &destroyed_clone) ==
        WOTBMOD_V3_E_OBJECT_DESTROYED);
    CHECK(destroyed_clone.object == 0u);
    CHECK(g_reentrant_destroy_result == WOTBMOD_V3_OK);
    CHECK(!g_release_observed_inside_callback);
    CHECK(g_reentrant_target_released);
    g_reentrant_release_target = nullptr;
    CHECK(g_allocations == 1u);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.text = nullptr;
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse leased_parent = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &leased_parent) ==
        WOTBMOD_V3_OK);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_BUTTON;
    descriptor.text = "Leased child";
    wotbmod::v3::ClientHostObjectResponse leased_child = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &leased_child) ==
        WOTBMOD_V3_OK);
    FakeUi* const leased_child_resource = g_last_allocated_ui;
    request = Request();
    request.object = leased_child.object;
    request.related_object = leased_parent.object;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_OK);
    request = Request();
    request.object = leased_parent.object;
    request.vector = {0.31f, 0.42f, 0.53f, 0.94f};
    CHECK(
        Invoke(owner, "ui_control_set_background_color", &request) ==
        WOTBMOD_V3_OK);
    g_reentrant_destroy_owner = owner;
    g_reentrant_destroy_object = leased_child.object;
    g_reentrant_destroy_result = WOTBMOD_V3_E_PLATFORM;
    g_reentrant_release_target = leased_child_resource;
    g_reentrant_target_released = false;
    g_destroy_during_add_child = true;
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_reentrant_destroy_result == WOTBMOD_V3_OK);
    /* The child was attached, so the destroy completes for the mod but its
     * native object waits out the retirement window instead of being freed
     * underneath the add_child callback that is still running. */
    CHECK(!g_reentrant_target_released);
    request = Request();
    request.object = leased_child.object;
    request.name = "Already destroyed";
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(g_allocations == 4u);
    PumpNativeFrames(4u);
    CHECK(g_reentrant_target_released);
    g_reentrant_release_target = nullptr;
    CHECK(g_allocations == 2u);
    request = Request();
    request.object = leased_parent.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_allocations == 1u);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_TEXT;
    descriptor.text = "Dynamic label";
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse text_control = Response();
    CHECK(
        Invoke(
            owner,
            "ui_control_create",
            &request,
            &text_control) == WOTBMOD_V3_OK);
    CHECK(text_control.object != 0u);
    CHECK(g_allocations == 2u);
    CHECK(g_last_resolved_object == "WotbModRuntimeControl");
    CHECK(
        g_last_resolved_contents.find("class: \"UIStaticText\"") !=
        std::string::npos);
    CHECK(
        g_last_resolved_contents.find("text: \"Dynamic label\"") !=
        std::string::npos);
    CHECK(
        g_last_resolved_contents.find("UITextComponent:") !=
        std::string::npos);

    request = Request();
    request.object = text_control.object;
    request.name = "Runtime text: 40%";
    const uint32_t loads_before_text = g_resolved_loads;
    wotbmod::loader::NotifyV3NativeClientServicesUiInputPhase(1u);
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_resolved_loads == loads_before_text);

    for (uint32_t drag = 41u; drag <= 75u; ++drag) {
        const std::string text =
            "Runtime text: " + std::to_string(drag) + "%";
        request.name = text.c_str();
        wotbmod::loader::NotifyV3NativeClientServicesUiInputPhase(2u);
        CHECK(
            Invoke(owner, "ui_control_set_text", &request) ==
            WOTBMOD_V3_OK);
        wotbmod::loader::PumpV3NativeClientServicesFrame();
    }
    CHECK(g_resolved_loads == loads_before_text);
    CHECK(g_allocations == 2u);

    request.vector = {0.25f, 0.50f, 0.75f, 1.0f};
    CHECK(
        Invoke(owner, "ui_control_set_background_color", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_resolved_loads == loads_before_text);

    request.flags = 1u;
    CHECK(
        Invoke(owner, "ui_control_set_rich_text", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_resolved_loads == loads_before_text);

    wotbmod::loader::NotifyV3NativeClientServicesUiInputPhase(3u);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_resolved_loads == loads_before_text);
    CHECK(g_allocations == 2u);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_resolved_loads == loads_before_text + 1u);
    CHECK(g_allocations == 3u);
    CHECK(
        g_last_resolved_contents.find("text: \"Runtime text: 75%\"") !=
        std::string::npos);
    CHECK(
        g_last_resolved_contents.find(
            "color: [0.250000, 0.500000, 0.750000, 1.000000]") !=
        std::string::npos);
    CHECK(
        g_last_resolved_contents.find("RichContent:") !=
        std::string::npos);
    CHECK(
        g_last_resolved_contents.find(
            "text: \"Runtime text: 75%\"") != std::string::npos);

    PumpNativeFrames(4u);
    CHECK(g_resolved_loads == loads_before_text + 1u);
    CHECK(g_allocations == 2u);

    const uint32_t loads_before_screen_reset = g_resolved_loads;
    request.name = "Recovered after screen change";
    wotbmod::loader::NotifyV3NativeClientServicesUiInputPhase(1u);
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_OK);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_resolved_loads == loads_before_screen_reset);
    wotbmod::loader::ResetV3NativeClientServicesUiInputState();
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_resolved_loads == loads_before_screen_reset);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_resolved_loads == loads_before_screen_reset + 1u);
    CHECK(
        g_last_resolved_contents.find(
            "text: \"Recovered after screen change\"") !=
        std::string::npos);
    PumpNativeFrames(4u);
    CHECK(g_allocations == 2u);

    const uint32_t clone_calls_before = g_clone_calls;
    wotbmod::v3::ClientHostObjectResponse text_clone = Response();
    CHECK(
        Invoke(owner, "ui_control_clone", &request, &text_clone) ==
        WOTBMOD_V3_OK);
    CHECK(text_clone.object != 0u && text_clone.object != text_control.object);
    CHECK(g_clone_calls == clone_calls_before);
    CHECK(g_allocations == 3u);

    request = Request();
    request.object = text_control.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    request.object = text_clone.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 1u);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_TEXT;
    descriptor.text = "Destroy during deferred load";
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse doomed = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &doomed) ==
        WOTBMOD_V3_OK);
    FakeUi* const doomed_resource = g_last_allocated_ui;
    request = Request();
    request.object = doomed.object;
    request.name = "Pending rebuild";
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_OK);
    g_reentrant_destroy_owner = owner;
    g_reentrant_destroy_object = doomed.object;
    g_reentrant_destroy_result = WOTBMOD_V3_E_PLATFORM;
    g_reentrant_release_target = doomed_resource;
    g_reentrant_target_released = false;
    g_destroy_during_resolved_load = true;
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_reentrant_destroy_result == WOTBMOD_V3_OK);
    CHECK(g_reentrant_target_released);
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(g_allocations == 2u);
    PumpNativeFrames(4u);
    g_reentrant_release_target = nullptr;
    CHECK(g_allocations == 1u);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_BUTTON;
    descriptor.text = "Lua button";
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse button = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &button) ==
        WOTBMOD_V3_OK);
    CHECK(
        g_last_resolved_contents.find("name: \"Caption\"") !=
        std::string::npos);
    CHECK(
        g_last_resolved_contents.find("Background:") !=
        std::string::npos);
    request = Request();
    request.object = button.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 1u);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.text = nullptr;

    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    request.secondary_name = "C:\\mods\\lua_ui_framework.yaml";
    request.name = "LuaUiFrameworkPanel";
    const uint32_t loads_before_template = g_resolved_loads;
    wotbmod::v3::ClientHostObjectResponse templated = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &templated) ==
        WOTBMOD_V3_OK);
    CHECK(templated.object != 0u);
    CHECK(g_resolved_loads == loads_before_template + 1u);
    CHECK(g_last_resolved_type == WOTBMOD_RESOURCE_UI_CONTROL);
    CHECK(g_last_resolved_path == "C:\\mods\\lua_ui_framework.yaml");
    CHECK(g_last_resolved_object == "LuaUiFrameworkPanel");
    CHECK(g_allocations == 2u);
    request = Request();
    request.object = templated.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 1u);

    request = Request();
    request.object = first.object;
    request.vector = {30.0f, 40.0f, 640.0f, 360.0f};
    CHECK(
        Invoke(owner, "ui_control_set_geometry", &request) ==
        WOTBMOD_V3_OK);
    request.flags = 0u;
    CHECK(
        Invoke(owner, "ui_control_set_visible", &request) ==
        WOTBMOD_V3_OK);
    CHECK(
        Invoke(owner, "ui_control_set_enabled", &request) ==
        WOTBMOD_V3_OK);
    CHECK(
        Invoke(owner, "ui_control_set_interactable", &request) ==
        WOTBMOD_V3_OK);

    wotbmod::v3::ClientHostObjectResponse state = Response();
    CHECK(
        Invoke(owner, "ui_control_get_state", &request, &state) ==
        WOTBMOD_V3_OK);
    CHECK(std::fabs(state.rect.x - 30.0f) < 0.001f);
    CHECK(std::fabs(state.rect.width - 640.0f) < 0.001f);
    CHECK(state.value_u32 == 0u);

    wotbmod::v3::ClientHostObjectResponse clone = Response();
    CHECK(
        Invoke(owner, "ui_control_clone", &request, &clone) ==
        WOTBMOD_V3_OK);
    CHECK(clone.object != 0u && clone.object != first.object);
    CHECK(g_allocations == 2u);

    request = Request();
    request.object = clone.object;
    request.related_object = first.object;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_OK);

    const uint32_t add_calls_after_first_parent = g_add_child_calls;
    const uint32_t remove_calls_after_first_parent = g_remove_child_calls;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_add_child_calls == add_calls_after_first_parent);
    CHECK(g_remove_child_calls == remove_calls_after_first_parent);

    FakeUi* const attached_child = g_last_allocated_ui;
    FakeUi* const attached_parent = attached_child->parent;
    FakeUi* const covering_hud = AllocateUi();
    CHECK(covering_hud != nullptr);
    CHECK(
        FakeAddChild(nullptr, attached_parent, covering_hud) ==
        WOTBMOD_OK);
    const uint32_t add_calls_after_hud = g_add_child_calls;
    const uint32_t bring_calls_after_hud = g_bring_to_front_calls;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_add_child_calls == add_calls_after_hud);
    CHECK(g_bring_to_front_calls == bring_calls_after_hud + 1u);
    CHECK(g_remove_child_calls == remove_calls_after_first_parent);
    CHECK(attached_parent->children.back() == attached_child);
    CHECK(FakeRelease(nullptr, covering_hud) == WOTBMOD_OK);

    const uint32_t add_calls_after_raise = g_add_child_calls;
    FakeUi* const detached_child = attached_child;
    FakeUi* const detached_parent = detached_child->parent;
    detached_parent->children.erase(
        std::remove(
            detached_parent->children.begin(),
            detached_parent->children.end(),
            detached_child),
        detached_parent->children.end());
    detached_child->parent = nullptr;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_add_child_calls == add_calls_after_raise + 1u);
    CHECK(g_remove_child_calls == remove_calls_after_first_parent);
    CHECK(detached_child->parent == detached_parent);

    request = Request();
    request.object = clone.object;
    request.related_object = 0u;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_OK);

    request = Request();
    request.object = clone.object;
    request.name = "hangar.top_bar.right";
    CHECK(
        Invoke(owner, "ui_slot_attach", &request) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(g_allocations == 2u);
    CHECK(
        Invoke(owner, "ui_slot_detach", &request) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(g_allocations == 2u);

    wotbmod::v3::ClientHostObjectResponse screen = Response();
    CHECK(
        wotbmod::loader::InvokeV3NativeClientServices(
            owner,
            "ui_get_active_screen",
            nullptr,
            0u,
            &screen,
            sizeof(screen)) == WOTBMOD_V3_OK);
    CHECK(screen.object != 0u);
    CHECK(g_allocations == 3u);

    request = Request();
    request.object = first.object;
    CHECK(
        Invoke(
            owner + 1u,
            "ui_control_destroy",
            &request) == WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    request.object = clone.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    request.object = screen.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 0u);

    WotbModV3SceneEntityDescriptor scene_descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(
        scene_descriptor, WOTBMOD_V3_SCENE_VERSION);
    scene_descriptor.transform.struct_size =
        sizeof(scene_descriptor.transform);
    scene_descriptor.transform.api_version = WOTBMOD_V3_ABI_VERSION;
    scene_descriptor.transform.rotation.w = 1.0f;
    scene_descriptor.transform.scale = {1.0f, 1.0f, 1.0f};
    request = Request();
    request.payload = &scene_descriptor;
    request.payload_size = sizeof(scene_descriptor);
    wotbmod::v3::ClientHostObjectResponse scene = Response();
    CHECK(
        Invoke(owner, "scene_entity_create", &request, &scene) ==
        WOTBMOD_V3_OK);
    CHECK(scene.object != 0u);
    CHECK(g_last_scene_entity != nullptr);
    CHECK(g_allocations == 1u);

    WotbModV3DrawMesh mesh = {};
    WOTBMOD_V3_INIT_STRUCT(mesh, WOTBMOD_V3_RENDER_VERSION);
    mesh.world.values[0] = 1.0f;
    mesh.world.values[5] = 1.0f;
    mesh.world.values[10] = 1.0f;
    mesh.world.values[15] = 1.0f;
    mesh.world.values[12] = 12.0f;
    mesh.world.values[13] = 34.0f;
    mesh.world.values[14] = 56.0f;
    request = Request();
    request.object = scene.object;
    request.payload = &mesh;
    request.payload_size = sizeof(mesh);
    CHECK(
        Invoke(owner, "render_draw_mesh", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 2u);
    CHECK(g_last_active_scene != nullptr);
    CHECK(g_last_scene_entity->parent == g_last_active_scene);
    CHECK(std::fabs(
              g_last_scene_entity->transform.position_x - 12.0f) <
          0.001f);
    mesh.world.values[12] = 24.0f;
    CHECK(
        Invoke(owner, "render_draw_mesh", &request) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 2u);
    CHECK(std::fabs(
              g_last_scene_entity->transform.position_x - 24.0f) <
          0.001f);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_allocations == 1u);
    CHECK(g_last_scene_entity->parent == nullptr);

    request.related_object = 0xDEADBEEFu;
    CHECK(
        Invoke(owner, "render_draw_mesh", &request) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    request = Request();
    request.object = scene.object;
    CHECK(
        Invoke(owner, "scene_entity_destroy", &request) ==
        WOTBMOD_V3_OK);
    g_last_scene_entity = nullptr;
    g_last_active_scene = nullptr;
    CHECK(g_allocations == 0u);

    /* Destroying an attached control has to serve the same retirement window
     * as replacing one. DAVA keeps raw UIControl pointers for pressed/drag
     * state and for frame-local hierarchy traversal, and a mod unmount tears
     * a whole attached tree down inside a single frame, so releasing the
     * native object in that frame hands DAVA a freed pointer. */
    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.text = nullptr;
    descriptor.visible = 1u;
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse retire_root = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &retire_root) ==
        WOTBMOD_V3_OK);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_TEXT;
    descriptor.text = "Retired on destroy";
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse retire_child = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &retire_child) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 2u);
    request = Request();
    request.object = retire_child.object;
    request.related_object = retire_root.object;
    CHECK(
        Invoke(owner, "ui_control_set_parent", &request) ==
        WOTBMOD_V3_OK);
    request = Request();
    request.object = retire_child.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) == WOTBMOD_V3_OK);
    /* The handle is gone for the mod in the same call ... */
    request = Request();
    request.object = retire_child.object;
    request.name = "Destroyed handle";
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    /* ... while the native object outlives the destroying frame. */
    CHECK(g_allocations == 2u);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_allocations == 2u);
    PumpNativeFrames(4u);
    CHECK(g_allocations == 1u);
    /* A control that never entered the hierarchy needs no window. */
    request = Request();
    request.object = retire_root.object;
    CHECK(
        Invoke(owner, "ui_control_destroy", &request) == WOTBMOD_V3_OK);
    CHECK(g_allocations == 0u);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_TEXT;
    descriptor.text = "Shutdown from retired release";
    descriptor.visible = 1u;
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse pump_shutdown_control = Response();
    CHECK(
        Invoke(
            owner,
            "ui_control_create",
            &request,
            &pump_shutdown_control) == WOTBMOD_V3_OK);
    request = Request();
    request.object = pump_shutdown_control.object;
    request.name = "Retire old resource";
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_OK);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_allocations == 2u);

    descriptor.text = "Pending shutdown rebuild";
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse pending_shutdown_control =
        Response();
    CHECK(
        Invoke(
            owner,
            "ui_control_create",
            &request,
            &pending_shutdown_control) == WOTBMOD_V3_OK);
    request = Request();
    request.object = pending_shutdown_control.object;
    request.name = "Must not load after shutdown request";
    CHECK(
        Invoke(owner, "ui_control_set_text", &request) ==
        WOTBMOD_V3_OK);
    const uint32_t loads_before_shutdown_pump = g_resolved_loads;
    wotbmod::loader::NotifyV3NativeClientServicesUiInputPhase(1u);
    PumpNativeFrames(3u);
    CHECK(g_resolved_loads == loads_before_shutdown_pump);
    CHECK(g_allocations == 3u);
    wotbmod::loader::NotifyV3NativeClientServicesUiInputPhase(6u);
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(g_resolved_loads == loads_before_shutdown_pump);
    CHECK(g_allocations == 3u);
    g_shutdown_released_other_ui_inside_callback = false;
    g_expected_allocations_during_shutdown_release = 3u;
    g_shutdown_during_release = true;
    wotbmod::loader::PumpV3NativeClientServicesFrame();
    CHECK(!g_shutdown_during_release);
    CHECK(!g_shutdown_released_other_ui_inside_callback);
    CHECK(g_resolved_loads == loads_before_shutdown_pump);
    CHECK(g_allocations == 0u);
    CHECK(
        Invoke(owner, "ui_control_get_state", &request) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        wotbmod::loader::InitializeV3NativeClientServices(
            &options) == WOTBMOD_V3_OK);

    descriptor.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    descriptor.text = nullptr;
    descriptor.visible = 1u;
    request = Request();
    request.payload = &descriptor;
    request.payload_size = sizeof(descriptor);
    wotbmod::v3::ClientHostObjectResponse shutdown_control = Response();
    CHECK(
        Invoke(owner, "ui_control_create", &request, &shutdown_control) ==
        WOTBMOD_V3_OK);
    CHECK(g_allocations == 1u);
    g_shutdown_during_set_visible = true;
    const uint64_t shutdown_object = shutdown_control.object;
    std::future<WotbModV3Result> shutdown_probe = std::async(
        std::launch::async,
        [owner, shutdown_object]() {
            wotbmod::v3::ClientHostObjectRequest probe = Request();
            probe.object = shutdown_object;
            probe.flags = 0u;
            return Invoke(
                owner,
                "ui_control_set_visible",
                &probe);
        });
    if (shutdown_probe.wait_for(std::chrono::seconds(5)) !=
        std::future_status::ready) {
        std::fprintf(
            stderr,
            "reentrant UI shutdown did not drain within five seconds\n");
        std::_Exit(1);
    }
    CHECK(shutdown_probe.get() == WOTBMOD_V3_OK);
    CHECK(!g_shutdown_during_set_visible);
    CHECK(g_allocations == 0u);
    request = Request();
    request.object = shutdown_object;
    CHECK(
        Invoke(owner, "ui_control_get_state", &request) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);

    wotbmod::loader::ShutdownV3NativeClientServices();
    CHECK(g_allocations == 0u);
    std::printf(
        "V3 native UI bridge: %u passed, %u failed\n",
        g_passes,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
