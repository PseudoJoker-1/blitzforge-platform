#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/audio_v2.h"
#include "../include/wotbmod/bigworld_rpc_v1.h"
#include "../include/wotbmod/camera_v1.h"
#include "../include/wotbmod/client_v1.h"
#include "../include/wotbmod/entity_public_v1.h"
#include "../include/wotbmod/projectile_v1.h"
#include "../include/wotbmod/render_v1.h"
#include "../include/wotbmod/scene_v1.h"
#include "../include/wotbmod/ui_v2.h"
#include "../include/wotbmod/vehicle_visual_v2.h"
#include "../include/wotbmod/vfs_v1.h"
#include "../src/v3/client_services_backend.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

using wotbmod::v3::ClientHostBackend;
using wotbmod::v3::ClientHostFrame;
using wotbmod::v3::ClientHostObjectResponse;
using wotbmod::v3::ClientHostPublicEntity;
using wotbmod::v3::ClientHostProjectile;

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
uint32_t g_hostCalls = 0u;
uint32_t g_leaveCalls = 0u;
uint32_t g_uiCreateCalls = 0u;
uint32_t g_cameraGetCalls = 0u;
uint32_t g_cameraWriteCalls = 0u;
uint32_t g_cameraTransitionCalls = 0u;
uint32_t g_cameraShakeCalls = 0u;
uint32_t g_cameraCancelEffectsCalls = 0u;
uint32_t g_audioCustomCalls = 0u;
uint32_t g_audioEventCalls = 0u;
uint32_t g_renderOverlayCalls = 0u;
uint32_t g_projectileVisualCalls = 0u;
uint32_t g_sceneActiveCalls = 0u;
uint32_t g_sceneMutationCalls = 0u;
uint32_t g_sceneDestroyCalls = 0u;
uint32_t g_renderCallbacks = 0u;
uint32_t g_rpcCallbacks = 0u;
uint32_t g_entityVisits = 0u;
uint64_t g_nextObject = UINT64_C(0x1000);
const char* g_entryId = "operation-permission-test";

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

void Copy(char* output, size_t capacity, const char* value) {
#if defined(_MSC_VER)
    strncpy_s(output, capacity, value, _TRUNCATE);
#else
    std::strncpy(output, value, capacity - 1u);
    output[capacity - 1u] = '\0';
#endif
}

bool IsOperation(const char* operation, const char* expected) {
    return operation && std::strcmp(operation, expected) == 0;
}

bool InitializeObjectResponse(
    void* response,
    uint32_t responseSize,
    uint64_t object) {
    if (!response || responseSize < sizeof(ClientHostObjectResponse)) {
        return false;
    }
    ClientHostObjectResponse* typed =
        static_cast<ClientHostObjectResponse*>(response);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    typed->object = object;
    return true;
}

WotbModV3Result WOTBMOD_V3_CALL InvokeHost(
    void*,
    WotbModV3Handle,
    const char* operation,
    const void*,
    uint32_t,
    void* response,
    uint32_t responseSize) {
    if (!operation) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ++g_hostCalls;
    if (IsOperation(operation, "client_leave_to_hangar")) {
        ++g_leaveCalls;
        return WOTBMOD_V3_OK;
    }
    if (IsOperation(operation, "ui_control_create")) {
        ++g_uiCreateCalls;
        return InitializeObjectResponse(
                   response, responseSize, g_nextObject++)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "camera_get_active")) {
        ++g_cameraGetCalls;
        return InitializeObjectResponse(
                   response, responseSize, UINT64_C(0xCAFE))
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "camera_set_fov")) {
        ++g_cameraWriteCalls;
        return WOTBMOD_V3_OK;
    }
    if (IsOperation(operation, "camera_transition_to")) {
        ++g_cameraTransitionCalls;
        return InitializeObjectResponse(
                   response, responseSize, UINT64_C(0xCEFFEC7))
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "camera_add_shake")) {
        ++g_cameraShakeCalls;
        return InitializeObjectResponse(
                   response, responseSize, UINT64_C(0xCEFFEC7))
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "camera_cancel_effects")) {
        ++g_cameraCancelEffectsCalls;
        return WOTBMOD_V3_OK;
    }
    if (IsOperation(operation, "audio_create") ||
        IsOperation(operation, "audio_create_stream")) {
        ++g_audioCustomCalls;
        return InitializeObjectResponse(
                   response, responseSize, g_nextObject++)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "sound_event_create")) {
        ++g_audioEventCalls;
        return InitializeObjectResponse(
                   response, responseSize, g_nextObject++)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "audio_set_listener_transform")) {
        ++g_audioEventCalls;
        return WOTBMOD_V3_OK;
    }
    if (IsOperation(operation, "render_push_state")) {
        ++g_renderOverlayCalls;
        return WOTBMOD_V3_OK;
    }
    if (IsOperation(operation, "rpc_subscribe_observed")) {
        return InitializeObjectResponse(
                   response, responseSize, g_nextObject++)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "projectile_get_visual_entity")) {
        ++g_projectileVisualCalls;
        return InitializeObjectResponse(
                   response, responseSize, g_nextObject++)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "scene_get_active")) {
        ++g_sceneActiveCalls;
        return InitializeObjectResponse(
                   response, responseSize, UINT64_C(0xAC710001))
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (IsOperation(operation, "scene_entity_set_transform")) {
        ++g_sceneMutationCalls;
        return WOTBMOD_V3_OK;
    }
    if (IsOperation(operation, "scene_entity_destroy")) {
        ++g_sceneDestroyCalls;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_OK;
}

ClientHostPublicEntity PublicVehicle(
    uint64_t nativeToken,
    uint32_t publicId,
    uint32_t local,
    uint32_t team,
    WotbModV3Vec3 position) {
    ClientHostPublicEntity entity = {};
    entity.struct_size = sizeof(entity);
    entity.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    entity.native_token = nativeToken;
    entity.valid_fields =
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELDS_ALL;
    WOTBMOD_V3_INIT_STRUCT(
        entity.snapshot,
        WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    entity.snapshot.public_id = publicId;
    entity.snapshot.type = WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE;
    entity.snapshot.visible_to_player = 1u;
    entity.snapshot.local_player = local;
    entity.snapshot.team = team;
    entity.snapshot.health = 1000;
    entity.snapshot.max_health = 1000;
    entity.snapshot.position = position;
    Copy(
        entity.snapshot.public_type,
        sizeof(entity.snapshot.public_type),
        "vehicle");
    Copy(
        entity.snapshot.display_name,
        sizeof(entity.snapshot.display_name),
        local != 0u ? "local" : "visible-enemy");
    return entity;
}

WotbModV3Result WOTBMOD_V3_CALL Entry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *info = {};
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), g_entryId);
    Copy(info->name, sizeof(info->name), g_entryId);
    Copy(info->version, sizeof(info->version), "1.0.0");
    return WOTBMOD_V3_OK;
}

WotbModV3Handle CreateEnabledMod(
    const char* id,
    uint32_t tier,
    const char* const* grants,
    uint32_t grantCount) {
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(id, tier, &mod) ==
        WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod, grants, grantCount, 1u) == WOTBMOD_V3_OK);
    g_entryId = id;
    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(mod, &Entry, &module) ==
        WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK);
    return mod;
}

void DestroyMod(WotbModV3Handle mod) {
    CHECK(WotbModV3Runtime_Disable(mod) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK);
}

template <typename T>
const T* Query(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    uint32_t version) {
    const void* table = nullptr;
    CHECK(
        bootstrap->query_interface(mod, name, version, &table) ==
        WOTBMOD_V3_OK);
    CHECK(table != nullptr);
    return static_cast<const T*>(table);
}

void WOTBMOD_V3_CALL RenderCallback(
    WotbModV3Handle,
    const WotbModV3RenderFrameInfo*,
    void*) {
    ++g_renderCallbacks;
}

void WOTBMOD_V3_CALL RpcCallback(
    WotbModV3Handle,
    const WotbModV3ObservedRpc*,
    void*) {
    ++g_rpcCallbacks;
}

WotbModV3Result WOTBMOD_V3_CALL EntityVisitor(
    WotbModV3Handle,
    const WotbModV3PublicEntitySnapshot* snapshot,
    void*) {
    if (!snapshot) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ++g_entityVisits;
    return WOTBMOD_V3_OK;
}

void PumpFrame() {
    ClientHostFrame frame = {};
    frame.struct_size = sizeof(frame);
    frame.api_version = WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    frame.render_backend = WOTBMOD_V3_RENDER_BACKEND_D3D11;
    frame.frame_index = 42u;
    frame.delta_seconds = 1.0 / 60.0;
    frame.viewport = {0.0f, 0.0f, 1280.0f, 720.0f};
    frame.native_device =
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234u));
    frame.native_context =
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x5678u));
    frame.native_swapchain =
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x9ABCu));
    wotbmod::v3::PumpClientHostFrame(&frame);
}

}  // namespace

int main() {
    std::error_code fixtureError;
    std::filesystem::create_directories(
        "build\\v3_operation_permissions\\cache\\operation-vehicle-skin-v2\\skin",
        fixtureError);
    CHECK(!fixtureError);
    {
        std::ofstream fixture(
            "build\\v3_operation_permissions\\cache\\operation-vehicle-skin-v2\\skin\\replacement.bin",
            std::ios::binary | std::ios::trunc);
        fixture << "skin-pack-test";
        CHECK(fixture.good());
    }

    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_operation_permissions\\mods";
    options.cache_directory = "build\\v3_operation_permissions\\cache";
    options.config_directory = "build\\v3_operation_permissions\\config";
    options.client_version = "operation-permission-test";
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);

    ClientHostBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version = WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    backend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    backend.invoke = &InvokeHost;
    wotbmod::v3::SetClientHostBackend(&backend);

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);

    const char* coreGrant[] = {"core"};
    const WotbModV3Handle denied = CreateEnabledMod(
        "operation-denied",
        WOTBMOD_V3_PERMISSION_UNSAFE,
        coreGrant,
        1u);
    const WotbModV3ClientApiV1* deniedClient =
        Query<WotbModV3ClientApiV1>(
            bootstrap,
            denied,
            WOTBMOD_V3_IFACE_CLIENT,
            WOTBMOD_V3_CLIENT_VERSION);
    const uint32_t leaveBefore = g_leaveCalls;
    CHECK(
        deniedClient->leave_to_hangar(denied) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(g_leaveCalls == leaveBefore);

    const char* uiOwnGrant[] = {"ui.modify.own"};
    WotbModV3Handle mod = CreateEnabledMod(
        "operation-ui-own",
        WOTBMOD_V3_PERMISSION_SAFE,
        uiOwnGrant,
        1u);
    const WotbModV3UiApiV2* ui = Query<WotbModV3UiApiV2>(
        bootstrap, mod, WOTBMOD_V3_IFACE_UI, WOTBMOD_V3_UI_VERSION);
    const uint32_t uiBefore = g_uiCreateCalls;
    CHECK(
        ui->control_create(mod, nullptr, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        ui->slot_find(mod, nullptr, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(g_uiCreateCalls == uiBefore);
    DestroyMod(mod);

    const char* uiCreateGrant[] = {"ui.create"};
    mod = CreateEnabledMod(
        "operation-ui-create",
        WOTBMOD_V3_PERMISSION_SAFE,
        uiCreateGrant,
        1u);
    ui = Query<WotbModV3UiApiV2>(
        bootstrap, mod, WOTBMOD_V3_IFACE_UI, WOTBMOD_V3_UI_VERSION);
    WotbModV3UiControlDescriptor control = {};
    WOTBMOD_V3_INIT_STRUCT(control, WOTBMOD_V3_UI_VERSION);
    control.type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    control.visible = 1u;
    control.id = "permission-test";
    WotbModV3UiHandle controlHandle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->control_create(mod, &control, &controlHandle) ==
        WOTBMOD_V3_OK);
    CHECK(g_uiCreateCalls == uiBefore + 1u);
    DestroyMod(mod);

    const char* stockUiGrant[] = {"ui.modify.game"};
    mod = CreateEnabledMod(
        "operation-ui-stock",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        stockUiGrant,
        1u);
    ui = Query<WotbModV3UiApiV2>(
        bootstrap, mod, WOTBMOD_V3_IFACE_UI, WOTBMOD_V3_UI_VERSION);
    WotbModV3UiHandle slot = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ui->slot_find(mod, "hangar.top_bar.left", &slot) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(slot == WOTBMOD_V3_INVALID_HANDLE);
    DestroyMod(mod);

    const char* battleUiGrant[] = {"battle.ui"};
    mod = CreateEnabledMod(
        "operation-ui-battle",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        battleUiGrant,
        1u);
    ui = Query<WotbModV3UiApiV2>(
        bootstrap, mod, WOTBMOD_V3_IFACE_UI, WOTBMOD_V3_UI_VERSION);
    CHECK(
        ui->slot_find(mod, "battle.hud.top", &slot) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(slot == WOTBMOD_V3_INVALID_HANDLE);
    slot = 0x1234u;
    CHECK(
        ui->slot_find(mod, "hangar.top_bar.left", &slot) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(slot == WOTBMOD_V3_INVALID_HANDLE);
    DestroyMod(mod);

    const char* renderCallbacksGrant[] = {"render.callbacks"};
    mod = CreateEnabledMod(
        "operation-render-callbacks",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        renderCallbacksGrant,
        1u);
    const WotbModV3RenderApiV1* render =
        Query<WotbModV3RenderApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_RENDER,
            WOTBMOD_V3_RENDER_VERSION);
    CHECK(
        render->create_texture(mod, nullptr, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    WotbModV3Token renderToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT,
            0,
            &RenderCallback,
            nullptr,
            &renderToken) == WOTBMOD_V3_OK);
    PumpFrame();
    CHECK(g_renderCallbacks == 1u);
    CHECK(render->unregister_callback(mod, renderToken) == WOTBMOD_V3_OK);
    DestroyMod(mod);

    const char* renderOverlayGrant[] = {"battle.render.overlay"};
    mod = CreateEnabledMod(
        "operation-render-overlay",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        renderOverlayGrant,
        1u);
    render = Query<WotbModV3RenderApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_RENDER,
        WOTBMOD_V3_RENDER_VERSION);
    const uint32_t overlayBefore = g_renderOverlayCalls;
    CHECK(render->push_state(mod) == WOTBMOD_V3_OK);
    CHECK(g_renderOverlayCalls == overlayBefore + 1u);
    CHECK(
        render->register_callback(
            mod, 99u, 0, nullptr, nullptr, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    DestroyMod(mod);

    const char* nativeGrant[] = {"render.native"};
    mod = CreateEnabledMod(
        "operation-render-native",
        WOTBMOD_V3_PERMISSION_UNSAFE,
        nativeGrant,
        1u);
    const WotbModV3RenderNativeApiV1* renderNative =
        Query<WotbModV3RenderNativeApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_RENDER_NATIVE,
            WOTBMOD_V3_RENDER_NATIVE_VERSION);
    PumpFrame();
    void* nativeDevice = nullptr;
    CHECK(
        renderNative->get_native_device(mod, &nativeDevice) ==
        WOTBMOD_V3_OK);
    CHECK(
        nativeDevice ==
        reinterpret_cast<void*>(static_cast<uintptr_t>(0x1234u)));
    CHECK(
        renderNative->get_native_device(denied, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    DestroyMod(mod);

    const char* audioCustomGrant[] = {"audio.custom"};
    mod = CreateEnabledMod(
        "operation-audio-custom",
        WOTBMOD_V3_PERMISSION_SAFE,
        audioCustomGrant,
        1u);
    const WotbModV3AudioApiV2* audio =
        Query<WotbModV3AudioApiV2>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_AUDIO,
            WOTBMOD_V3_AUDIO_VERSION);
    CHECK(
        audio->sound_event_create(mod, nullptr, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    WotbModV3AudioDescriptor audioDescriptor = {};
    WOTBMOD_V3_INIT_STRUCT(
        audioDescriptor,
        WOTBMOD_V3_AUDIO_VERSION);
    audioDescriptor.uri = "permission-test.wav";
    audioDescriptor.volume = 1.0f;
    audioDescriptor.pitch = 1.0f;
    WotbModV3AudioHandle audioHandle = WOTBMOD_V3_INVALID_HANDLE;
    const uint32_t audioCustomBefore = g_audioCustomCalls;
    CHECK(
        audio->create(mod, &audioDescriptor, &audioHandle) ==
        WOTBMOD_V3_OK);
    CHECK(g_audioCustomCalls == audioCustomBefore + 1u);
    CHECK(audio->destroy(mod, audioHandle) == WOTBMOD_V3_OK);
    DestroyMod(mod);

    const char* audioEventsGrant[] = {"audio.events"};
    mod = CreateEnabledMod(
        "operation-audio-events",
        WOTBMOD_V3_PERMISSION_SAFE,
        audioEventsGrant,
        1u);
    audio = Query<WotbModV3AudioApiV2>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_AUDIO,
        WOTBMOD_V3_AUDIO_VERSION);
    CHECK(
        audio->create(mod, nullptr, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    WotbModV3AudioHandle event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        audio->sound_event_create(mod, "permission.event", &event) ==
        WOTBMOD_V3_OK);
    WotbModV3Transform listener = {};
    WOTBMOD_V3_INIT_STRUCT(listener, WOTBMOD_V3_ABI_VERSION);
    listener.rotation.w = 1.0f;
    listener.scale = {1.0f, 1.0f, 1.0f};
    CHECK(
        audio->set_listener_transform(mod, &listener) ==
        WOTBMOD_V3_OK);
    CHECK(audio->sound_event_destroy(mod, event) == WOTBMOD_V3_OK);
    DestroyMod(mod);

    const char* cameraHangarGrant[] = {"camera.hangar"};
    mod = CreateEnabledMod(
        "operation-camera-hangar",
        WOTBMOD_V3_PERMISSION_SAFE,
        cameraHangarGrant,
        1u);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) ==
        WOTBMOD_V3_OK);
    const WotbModV3CameraApiV1* camera =
        Query<WotbModV3CameraApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CAMERA,
            WOTBMOD_V3_CAMERA_VERSION);
    WotbModV3CameraHandle cameraHandle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(camera->get_active(mod, &cameraHandle) == WOTBMOD_V3_OK);
    CHECK(camera->set_fov(mod, cameraHandle, 75.0f) == WOTBMOD_V3_OK);
    WotbModV3CameraTransition transition = {};
    WOTBMOD_V3_INIT_STRUCT(
        transition, WOTBMOD_V3_CAMERA_VERSION);
    WOTBMOD_V3_INIT_STRUCT(
        transition.target, WOTBMOD_V3_CAMERA_VERSION);
    WOTBMOD_V3_INIT_STRUCT(
        transition.target.transform, WOTBMOD_V3_ABI_VERSION);
    transition.target.transform.rotation.w = 1.0f;
    transition.target.transform.scale = {1.0f, 1.0f, 1.0f};
    transition.target.fov_degrees = 80.0f;
    transition.target.near_plane = 0.1f;
    transition.target.far_plane = 1000.0f;
    transition.duration_seconds = 1.0f;
    CHECK(
        camera->transition_to(
            mod, cameraHandle, &transition) == WOTBMOD_V3_OK);
    WotbModV3CameraShake shake = {};
    WOTBMOD_V3_INIT_STRUCT(shake, WOTBMOD_V3_CAMERA_VERSION);
    shake.amplitude = 1.0f;
    shake.frequency = 8.0f;
    shake.duration_seconds = 0.5f;
    shake.falloff = 1.0f;
    CHECK(
        camera->add_shake(
            mod, cameraHandle, &shake) == WOTBMOD_V3_OK);
    CHECK(
        g_cameraTransitionCalls == 1u &&
            g_cameraShakeCalls == 1u &&
            g_cameraCancelEffectsCalls == 0u);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_BATTLE) ==
        WOTBMOD_V3_OK);
    const uint32_t cameraHostBefore = g_hostCalls;
    CHECK(
        camera->get_active(mod, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        camera->set_fov(mod, cameraHandle, 80.0f) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(g_hostCalls == cameraHostBefore);
    DestroyMod(mod);
    CHECK(g_cameraCancelEffectsCalls == 1u);

    const char* cameraBattleGrant[] = {"camera.battle.read"};
    mod = CreateEnabledMod(
        "operation-camera-battle",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        cameraBattleGrant,
        1u);
    camera = Query<WotbModV3CameraApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_CAMERA,
        WOTBMOD_V3_CAMERA_VERSION);
    CHECK(camera->get_active(mod, &cameraHandle) == WOTBMOD_V3_OK);
    DestroyMod(mod);

    const char* cameraTrainingGrants[] = {
        "camera.hangar",
        "gameplay.tweak.camera"};
    mod = CreateEnabledMod(
        "operation-camera-training",
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        cameraTrainingGrants,
        2u);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) ==
        WOTBMOD_V3_OK);
    camera = Query<WotbModV3CameraApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_CAMERA,
        WOTBMOD_V3_CAMERA_VERSION);
    CHECK(camera->get_active(mod, &cameraHandle) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_TRAINING) ==
        WOTBMOD_V3_OK);
    CHECK(camera->set_fov(mod, cameraHandle, 85.0f) == WOTBMOD_V3_OK);
    DestroyMod(mod);

    WotbModV3EntityHandle localVehicle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity localEntity = PublicVehicle(
        UINT64_C(0xE001),
        101u,
        1u,
        1u,
        {1.0f, 2.0f, 3.0f});
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &localEntity,
            &localVehicle) == WOTBMOD_V3_OK);
    WotbModV3EntityHandle enemyVehicle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity enemyEntity = PublicVehicle(
        UINT64_C(0xE002),
        202u,
        0u,
        2u,
        {4.0f, 5.0f, 6.0f});
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &enemyEntity,
            &enemyVehicle) == WOTBMOD_V3_OK);

    const char* skinGrants[] = {
        "vehicle.local.cosmetic",
        "resources.mod",
        "resources.overlay.game"};
    mod = CreateEnabledMod(
        "operation-vehicle-skin-v2",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        skinGrants,
        3u);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) ==
        WOTBMOD_V3_OK);
    const WotbModV3VehicleVisualApiV2* vehicleV2 =
        Query<WotbModV3VehicleVisualApiV2>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    const WotbModV3VfsApiV1* vfs = Query<WotbModV3VfsApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_VFS,
        WOTBMOD_V3_VFS_VERSION);
    WotbModV3VehicleSkinAsset skinAssets[3] = {};
    for (WotbModV3VehicleSkinAsset& asset : skinAssets) {
        WOTBMOD_V3_INIT_STRUCT(
            asset,
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
        asset.replacement_uri = "cache://self/skin/replacement.bin";
    }
    skinAssets[0].kind = WOTBMOD_V3_VEHICLE_SKIN_MESH;
    skinAssets[0].part = WOTBMOD_V3_VEHICLE_PART_HULL;
    skinAssets[0].lod = 0;
    skinAssets[0].stock_game_uri =
        "game://Data/3d/Tanks/Test/Hull_lod0.sc2";
    skinAssets[1].kind = WOTBMOD_V3_VEHICLE_SKIN_MATERIAL;
    skinAssets[1].part = WOTBMOD_V3_VEHICLE_PART_HULL;
    skinAssets[1].lod = -1;
    skinAssets[1].stock_game_uri =
        "game://Data/3d/Tanks/Test/Hull.material.yaml";
    skinAssets[2].kind = WOTBMOD_V3_VEHICLE_SKIN_TEXTURE;
    skinAssets[2].part = WOTBMOD_V3_VEHICLE_PART_HULL;
    skinAssets[2].lod = -1;
    skinAssets[2].stock_game_uri =
        "game://Data/3d/Tanks/Test/Hull.dx11.dds";
    WotbModV3VehicleSkinPack skinPack = {};
    WOTBMOD_V3_INIT_STRUCT(
        skinPack,
        WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    skinPack.id = "test-skin";
    skinPack.vehicle_name = "test-vehicle";
    skinPack.assets = skinAssets;
    skinPack.asset_count = 3u;
    skinPack.priority = 100;
    WotbModV3Handle skinHandle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        vehicleV2->skin_pack_register(mod, &skinPack, &skinHandle) ==
        WOTBMOD_V3_OK);
    WotbModV3VehicleSkinState skinState = {};
    WOTBMOD_V3_INIT_STRUCT(
        skinState,
        WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    CHECK(
        vehicleV2->skin_pack_get_state(mod, skinHandle, &skinState) ==
        WOTBMOD_V3_OK);
    CHECK(
        skinState.applied == 0u && skinState.asset_count == 3u &&
        skinState.mounted_asset_count == 0u);
    CHECK(
        vehicleV2->skin_pack_apply(mod, skinHandle, localVehicle) ==
        WOTBMOD_V3_OK);
    CHECK(
        vehicleV2->skin_pack_apply(mod, skinHandle, localVehicle) ==
        WOTBMOD_V3_OK);
    CHECK(
        vehicleV2->skin_pack_get_state(mod, skinHandle, &skinState) ==
        WOTBMOD_V3_OK);
    CHECK(
        skinState.applied == 1u &&
        skinState.mounted_asset_count == 3u &&
        skinState.vehicle == localVehicle &&
        skinState.requires_model_reload == 1u);
    uint32_t resolvedSize = 0u;
    CHECK(
        vfs->resolve(
            mod,
            skinAssets[0].stock_game_uri,
            nullptr,
            &resolvedSize) == WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(resolvedSize > 1u);
    char resolvedPath[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t resolvedCapacity = sizeof(resolvedPath);
    CHECK(
        vfs->resolve(
            mod,
            skinAssets[0].stock_game_uri,
            resolvedPath,
            &resolvedCapacity) == WOTBMOD_V3_OK);
    CHECK(std::strstr(resolvedPath, "replacement.bin") != nullptr);
    CHECK(
        vehicleV2->skin_pack_rollback(mod, skinHandle) ==
        WOTBMOD_V3_OK);
    CHECK(
        vehicleV2->skin_pack_get_state(mod, skinHandle, &skinState) ==
        WOTBMOD_V3_OK);
    CHECK(
        skinState.applied == 0u &&
        skinState.mounted_asset_count == 0u &&
        skinState.vehicle == WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3VehicleSkinAsset failingAssets[2] = {
        skinAssets[0], skinAssets[1]};
    failingAssets[1].replacement_uri =
        "cache://self/skin/missing-replacement.bin";
    WotbModV3VehicleSkinPack failingPack = skinPack;
    failingPack.id = "test-skin-atomic-failure";
    failingPack.assets = failingAssets;
    failingPack.asset_count = 2u;
    WotbModV3Handle failingHandle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        vehicleV2->skin_pack_register(
            mod, &failingPack, &failingHandle) == WOTBMOD_V3_OK);
    const WotbModV3Result failingApply =
        vehicleV2->skin_pack_apply(mod, failingHandle, localVehicle);
    if (failingApply != WOTBMOD_V3_E_NOT_FOUND) {
        std::fprintf(stderr, "failing skin apply result: %u\n", failingApply);
    }
    CHECK(failingApply == WOTBMOD_V3_E_NOT_FOUND);
    WOTBMOD_V3_INIT_STRUCT(
        skinState,
        WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    CHECK(
        vehicleV2->skin_pack_get_state(
            mod, failingHandle, &skinState) == WOTBMOD_V3_OK);
    CHECK(
        skinState.applied == 0u &&
        skinState.mounted_asset_count == 0u);
    CHECK(
        vehicleV2->skin_pack_release(mod, failingHandle) ==
        WOTBMOD_V3_OK);
    CHECK(
        vehicleV2->skin_pack_apply(mod, skinHandle, localVehicle) ==
        WOTBMOD_V3_OK);
    CHECK(
        vehicleV2->skin_pack_release(mod, skinHandle) ==
        WOTBMOD_V3_OK);
    resolvedSize = 0u;
    const WotbModV3Result afterReleaseResolve = vfs->resolve(
        mod,
        skinAssets[0].stock_game_uri,
        nullptr,
        &resolvedSize);
    if (afterReleaseResolve != WOTBMOD_V3_E_NOT_FOUND) {
        std::fprintf(
            stderr,
            "skin path after release result: %u\n",
            afterReleaseResolve);
    }
    CHECK(afterReleaseResolve == WOTBMOD_V3_E_NOT_FOUND);
    DestroyMod(mod);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_BATTLE) ==
        WOTBMOD_V3_OK);

    const char* entitySafeGrant[] = {"entity.public.visible"};
    mod = CreateEnabledMod(
        "operation-entity-safe",
        WOTBMOD_V3_PERMISSION_SAFE,
        entitySafeGrant,
        1u);
    const WotbModV3EntityPublicApiV1* entity =
        Query<WotbModV3EntityPublicApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
            WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    CHECK(
        entity->get_public_id(
            mod, WOTBMOD_V3_INVALID_HANDLE, nullptr) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(
        entity->get_public_id(
            denied, WOTBMOD_V3_INVALID_HANDLE, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    uint32_t publicId = 0u;
    CHECK(
        entity->get_public_id(
            mod, localVehicle, &publicId) == WOTBMOD_V3_OK);
    CHECK(publicId == 101u);
    CHECK(
        entity->get_public_id(
            mod, enemyVehicle, &publicId) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        entity->enumerate_visible(
            mod, &EntityVisitor, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    DestroyMod(mod);

    const char* entityReviewedGrant[] = {"game.entity.public"};
    mod = CreateEnabledMod(
        "operation-entity-reviewed",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        entityReviewedGrant,
        1u);
    entity = Query<WotbModV3EntityPublicApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
        WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    CHECK(
        entity->get_public_id(
            mod, WOTBMOD_V3_INVALID_HANDLE, nullptr) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(
        entity->get_public_id(
            mod, enemyVehicle, &publicId) == WOTBMOD_V3_OK);
    CHECK(publicId == 202u);
    const uint32_t visitsBefore = g_entityVisits;
    CHECK(
        entity->enumerate_visible(
            mod, &EntityVisitor, nullptr) == WOTBMOD_V3_OK);
    CHECK(g_entityVisits == visitsBefore + 2u);
    DestroyMod(mod);

    const char* vehicleLocalGrant[] = {
        "vehicle.local.cosmetic"};
    mod = CreateEnabledMod(
        "operation-vehicle-local",
        WOTBMOD_V3_PERMISSION_SAFE,
        vehicleLocalGrant,
        1u);
    const WotbModV3VehicleVisualApiV1* vehicle =
        Query<WotbModV3VehicleVisualApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_BATTLE) ==
        WOTBMOD_V3_OK);
    const WotbModV3Color vehicleColor =
        {1.0f, 1.0f, 1.0f, 1.0f};
    CHECK(
        vehicle->set_color_override(
            mod, localVehicle, "", vehicleColor) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        vehicle->set_color_override(
            mod, enemyVehicle, "", vehicleColor) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        vehicle->set_color_override(
            mod,
            UINT64_C(0xE30000000000FFFF),
            "",
            vehicleColor) == WOTBMOD_V3_E_NOT_FOUND);
    WotbModV3Vec3 publicPosition = {};
    CHECK(
        vehicle->get_position(
            mod, enemyVehicle, &publicPosition) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) ==
        WOTBMOD_V3_OK);
    CHECK(
        vehicle->set_color_override(
            mod, enemyVehicle, "", vehicleColor) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    DestroyMod(mod);

    const char* vehiclePublicGrants[] = {
        "vehicle.local.cosmetic",
        "game.entity.public"};
    mod = CreateEnabledMod(
        "operation-vehicle-public",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        vehiclePublicGrants,
        2u);
    vehicle = Query<WotbModV3VehicleVisualApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
        WOTBMOD_V3_VEHICLE_VISUAL_VERSION);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_BATTLE) ==
        WOTBMOD_V3_OK);
    CHECK(
        vehicle->get_position(
            mod, enemyVehicle, &publicPosition) == WOTBMOD_V3_OK);
    CHECK(
        publicPosition.x == 4.0f &&
        publicPosition.y == 5.0f &&
        publicPosition.z == 6.0f);
    DestroyMod(mod);

    const char* rpcGrant[] = {"bigworld.observe"};
    mod = CreateEnabledMod(
        "operation-rpc",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        rpcGrant,
        1u);
    const WotbModV3BigWorldRpcApiV1* rpc =
        Query<WotbModV3BigWorldRpcApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_BIGWORLD_RPC,
            WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    WotbModV3BigWorldRpcPolicy policy = {};
    WOTBMOD_V3_INIT_STRUCT(policy, WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    CHECK(rpc->get_policy(mod, &policy) == WOTBMOD_V3_OK);
    CHECK(policy.metadata_observation == 1u);
    CHECK(
        rpc->get_policy(denied, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    WotbModV3Token rpcToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &rpcToken) == WOTBMOD_V3_OK);
    WotbModV3ObservedRpc observed = {};
    WOTBMOD_V3_INIT_STRUCT(
        observed,
        WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    observed.direction = WOTBMOD_V3_RPC_INCOMING;
    Copy(observed.entity_type, sizeof(observed.entity_type), "Vehicle");
    Copy(observed.method_name, sizeof(observed.method_name), "onHealth");
    CHECK(
        wotbmod::v3::NotifyClientHostObservedRpc(&observed) ==
        WOTBMOD_V3_OK);
    CHECK(g_rpcCallbacks == 1u);
    CHECK(rpc->unsubscribe_observed(mod, rpcToken) == WOTBMOD_V3_OK);
    DestroyMod(mod);

    ClientHostProjectile hostProjectile = {};
    hostProjectile.struct_size = sizeof(hostProjectile);
    hostProjectile.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    hostProjectile.native_token = UINT64_C(0xAA55);
    hostProjectile.visual_scene_object = UINT64_C(0xBB66);
    hostProjectile.owner_scope =
        WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER;
    hostProjectile.visible_direction = {0.0f, 0.0f, 1.0f};
    WotbModV3ProjectileHandle projectileHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::RegisterClientHostProjectile(
            &hostProjectile,
            &projectileHandle) == WOTBMOD_V3_OK);
    ClientHostProjectile enemyProjectile = hostProjectile;
    enemyProjectile.native_token = UINT64_C(0xAA56);
    enemyProjectile.visual_scene_object = UINT64_C(0xBB67);
    enemyProjectile.owner_scope =
        WOTBMOD_V3_PROJECTILE_OWNER_ENEMY_VISIBLE;
    WotbModV3ProjectileHandle enemyProjectileHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::RegisterClientHostProjectile(
            &enemyProjectile,
            &enemyProjectileHandle) == WOTBMOD_V3_OK);

    const char* projectileObserveGrant[] = {
        "visible.projectile.events"};
    mod = CreateEnabledMod(
        "operation-projectile-observe",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        projectileObserveGrant,
        1u);
    const WotbModV3ProjectileApiV1* projectile =
        Query<WotbModV3ProjectileApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_PROJECTILE,
            WOTBMOD_V3_PROJECTILE_VERSION);
    uint32_t ownerScope = 0u;
    CHECK(
        projectile->projectile_get_owner_scope(
            mod, projectileHandle, &ownerScope) == WOTBMOD_V3_OK);
    CHECK(
        projectile->projectile_get_visual_entity(
            mod, projectileHandle, nullptr) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    DestroyMod(mod);

    const char* projectileVisualGrant[] = {
        "gameplay.tweak.projectile_visual"};
    mod = CreateEnabledMod(
        "operation-projectile-visual",
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        projectileVisualGrant,
        1u);
    projectile = Query<WotbModV3ProjectileApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_PROJECTILE,
        WOTBMOD_V3_PROJECTILE_VERSION);
    WotbModV3SceneHandle visual = WOTBMOD_V3_INVALID_HANDLE;
    const uint32_t projectileVisualBefore =
        g_projectileVisualCalls;
    CHECK(
        projectile->projectile_get_visual_entity(
            mod,
            UINT64_C(0xE40000000000FFFF),
            &visual) == WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        projectile->projectile_get_visual_entity(
            mod,
            enemyProjectileHandle,
            &visual) == WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        projectile->projectile_attach_visual(
            mod,
            enemyProjectileHandle,
            WOTBMOD_V3_INVALID_HANDLE,
            WOTBMOD_V3_ATTACHMENT_DETACH_ON_PARENT_DESTROY |
                WOTBMOD_V3_ATTACHMENT_KEEP_LOCAL_TRANSFORM) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(g_projectileVisualCalls == projectileVisualBefore);
    CHECK(
        projectile->projectile_get_visual_entity(
            mod, projectileHandle, &visual) == WOTBMOD_V3_OK);
    CHECK(
        g_projectileVisualCalls ==
        projectileVisualBefore + 1u);
    CHECK(
        projectile->projectile_get_owner_scope(
            mod, projectileHandle, &ownerScope) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    DestroyMod(mod);
    CHECK(
        wotbmod::v3::RemoveClientHostProjectile(projectileHandle) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::RemoveClientHostProjectile(
            enemyProjectileHandle) == WOTBMOD_V3_OK);

    const char* hangarSceneGrant[] = {"hangar.scene"};
    mod = CreateEnabledMod(
        "operation-scene-hangar",
        WOTBMOD_V3_PERMISSION_SAFE,
        hangarSceneGrant,
        1u);
    const WotbModV3SceneApiV1* scene =
        Query<WotbModV3SceneApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_SCENE,
            WOTBMOD_V3_SCENE_VERSION);
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_HANGAR) ==
        WOTBMOD_V3_OK);
    WotbModV3SceneHandle activeScene =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        scene->get_active_scene(mod, &activeScene) ==
        WOTBMOD_V3_OK);
    WotbModV3Transform sceneTransform = {};
    WOTBMOD_V3_INIT_STRUCT(
        sceneTransform,
        WOTBMOD_V3_ABI_VERSION);
    sceneTransform.rotation.w = 1.0f;
    sceneTransform.scale = {1.0f, 1.0f, 1.0f};
    CHECK(
        WotbModV3Runtime_SetContext(WOTBMOD_V3_CONTEXT_BATTLE) ==
        WOTBMOD_V3_OK);
    const uint32_t sceneMutationBefore = g_sceneMutationCalls;
    CHECK(
        scene->entity_set_transform(
            mod, activeScene, &sceneTransform) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(g_sceneMutationCalls == sceneMutationBefore);
    const uint32_t sceneDestroyBefore = g_sceneDestroyCalls;
    DestroyMod(mod);
    CHECK(g_sceneDestroyCalls == sceneDestroyBefore);

    const char* battleSceneGrants[] = {
        "resources.mod",
        "battle.render.overlay"};
    mod = CreateEnabledMod(
        "operation-scene-battle",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        battleSceneGrants,
        2u);
    scene = Query<WotbModV3SceneApiV1>(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_SCENE,
        WOTBMOD_V3_SCENE_VERSION);
    activeScene = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        scene->get_active_scene(mod, &activeScene) ==
        WOTBMOD_V3_OK);
    CHECK(
        scene->entity_set_transform(
            mod, activeScene, &sceneTransform) == WOTBMOD_V3_OK);
    CHECK(g_sceneMutationCalls == sceneMutationBefore + 1u);
    CHECK(
        scene->entity_destroy(mod, activeScene) ==
        WOTBMOD_V3_OK);
    CHECK(g_sceneDestroyCalls == sceneDestroyBefore);
    DestroyMod(mod);

    CHECK(
        wotbmod::v3::RemoveClientHostPublicEntity(
            localVehicle) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::RemoveClientHostPublicEntity(
            enemyVehicle) == WOTBMOD_V3_OK);

    const char* leaveGrant[] = {"client.leave_to_hangar"};
    mod = CreateEnabledMod(
        "operation-leave",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        leaveGrant,
        1u);
    const WotbModV3ClientApiV1* client =
        Query<WotbModV3ClientApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CLIENT,
            WOTBMOD_V3_CLIENT_VERSION);
    const uint32_t allowedLeaveBefore = g_leaveCalls;
    CHECK(client->leave_to_hangar(mod) == WOTBMOD_V3_OK);
    CHECK(g_leaveCalls == allowedLeaveBefore + 1u);
    DestroyMod(mod);

    DestroyMod(denied);
    wotbmod::v3::SetClientHostBackend(nullptr);
    WotbModV3Runtime_Shutdown();
    std::printf(
        "v3 operation permissions checks: %u, failures: %u\n",
        g_checks,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
