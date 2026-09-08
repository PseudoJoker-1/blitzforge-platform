#include "wotb_mod_api_v3.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct ProbeCounters {
    uint32_t passed;
    uint32_t failed;
    uint32_t unavailable;
};

struct InterfaceProbe {
    const char* name;
    uint32_t version;
    size_t table_size;
};

void CopyText(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    destination[0] = '\0';
    if (!source) return;
#if defined(_MSC_VER)
    strncpy_s(destination, capacity, source, _TRUNCATE);
#else
    std::strncpy(destination, source, capacity - 1u);
    destination[capacity - 1u] = '\0';
#endif
}

template <typename T>
const T* QueryApi(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    uint32_t version) {
    const void* table = nullptr;
    if (!bootstrap || !bootstrap->query_interface ||
        bootstrap->query_interface(
            mod,
            name,
            version,
            &table) != WOTBMOD_V3_OK) {
        return nullptr;
    }
    return static_cast<const T*>(table);
}

void Log(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    uint32_t level,
    const char* message) {
    const WotbModV3CoreApiV1* core =
        QueryApi<WotbModV3CoreApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION);
    if (core && core->log) {
        core->log(
            mod,
            level,
            "v3.example.client-resources",
            message ? message : "");
    }
}

void Record(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters,
    bool passed,
    const char* operation) {
    if (!counters) return;
    if (passed) {
        ++counters->passed;
        return;
    }
    ++counters->failed;
    char message[WOTBMOD_V3_MAX_MESSAGE] = {};
#if defined(_MSC_VER)
    sprintf_s(
        message,
        "FAILED: %s",
        operation ? operation : "unnamed probe");
#else
    std::snprintf(
        message,
        sizeof(message),
        "FAILED: %s",
        operation ? operation : "unnamed probe");
#endif
    Log(bootstrap, mod, WOTBMOD_V3_LOG_ERROR, message);
}

bool IsExpectedUnavailable(WotbModV3Result result) {
    return result == WOTBMOD_V3_E_NOT_SUPPORTED ||
           result == WOTBMOD_V3_E_PERMISSION_DENIED ||
           result == WOTBMOD_V3_E_CLIENT_MISMATCH ||
           result == WOTBMOD_V3_E_PLATFORM;
}

bool IsExpectedObservationResult(WotbModV3Result result) {
    return result == WOTBMOD_V3_OK ||
           result == WOTBMOD_V3_E_NOT_FOUND ||
           result == WOTBMOD_V3_E_INVALID_HANDLE ||
           result == WOTBMOD_V3_E_OBJECT_DESTROYED ||
           result == WOTBMOD_V3_E_WRONG_THREAD ||
           result == WOTBMOD_V3_E_BUSY ||
           IsExpectedUnavailable(result);
}

bool ValidateFunctionTable(
    const void* table,
    uint32_t expected_version,
    size_t expected_size) {
    if (!table || expected_size < sizeof(uint32_t) * 2u) return false;
    uint32_t reported_size = 0u;
    uint32_t reported_version = 0u;
    std::memcpy(&reported_size, table, sizeof(reported_size));
    std::memcpy(
        &reported_version,
        static_cast<const uint8_t*>(table) + sizeof(uint32_t),
        sizeof(reported_version));
    if (reported_size < expected_size ||
        reported_version != expected_version) {
        return false;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(table);
    for (size_t offset = sizeof(uint32_t) * 2u;
         offset + sizeof(uintptr_t) <= expected_size;
         offset += sizeof(uintptr_t)) {
        uintptr_t function_slot = 0u;
        std::memcpy(&function_slot, bytes + offset, sizeof(function_slot));
        if (function_slot == 0u) return false;
    }
    return true;
}

void ProbeInterface(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const InterfaceProbe& probe,
    ProbeCounters* counters) {
    WotbModV3InterfaceInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    const WotbModV3Result info_result =
        bootstrap->get_interface_info(mod, probe.name, &info);
    Record(
        bootstrap,
        mod,
        counters,
        info_result == WOTBMOD_V3_OK,
        probe.name);
    if (info_result != WOTBMOD_V3_OK) return;

    const void* table = nullptr;
    const WotbModV3Result query_result =
        bootstrap->query_interface(
            mod,
            probe.name,
            probe.version,
            &table);
    if (query_result != WOTBMOD_V3_OK) {
        if (IsExpectedUnavailable(query_result)) {
            ++counters->unavailable;
            return;
        }
        Record(
            bootstrap,
            mod,
            counters,
            false,
            probe.name);
        return;
    }
    Record(
        bootstrap,
        mod,
        counters,
        ValidateFunctionTable(
            table,
            probe.version,
            probe.table_size),
        probe.name);
}

WotbModV3Result WOTBMOD_V3_CALL VisibleEntityVisitor(
    WotbModV3Handle,
    const WotbModV3PublicEntitySnapshot* entity,
    void* user_data) {
    uint32_t* count = static_cast<uint32_t*>(user_data);
    if (!entity || !count ||
        entity->struct_size < sizeof(WotbModV3PublicEntitySnapshot)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ++(*count);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL MissingBindingVisitor(
    WotbModV3Handle,
    const WotbModV3MissingBinding* binding,
    void* user_data) {
    uint32_t* count = static_cast<uint32_t*>(user_data);
    if (!binding || !count ||
        binding->struct_size < sizeof(WotbModV3MissingBinding)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ++(*count);
    return WOTBMOD_V3_OK;
}

void ProbeClientAndDevice(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    const WotbModV3ClientApiV1* client =
        QueryApi<WotbModV3ClientApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CLIENT,
            WOTBMOD_V3_CLIENT_VERSION);
    if (client) {
        uint32_t supported = 0u;
        uint32_t binding_pack = 0u;
        uint32_t compatibility =
            WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN;
        uint32_t missing_count = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            client->is_supported(mod, &supported) == WOTBMOD_V3_OK,
            "client.is_supported");
        Record(
            bootstrap,
            mod,
            counters,
            client->get_binding_pack_version(
                mod,
                &binding_pack) == WOTBMOD_V3_OK,
            "client.get_binding_pack_version");
        Record(
            bootstrap,
            mod,
            counters,
            client->get_compatibility_state(
                mod,
                &compatibility) == WOTBMOD_V3_OK,
            "client.get_compatibility_state");
        Record(
            bootstrap,
            mod,
            counters,
            client->enumerate_missing_bindings(
                mod,
                &MissingBindingVisitor,
                &missing_count) == WOTBMOD_V3_OK,
            "client.enumerate_missing_bindings");
    }

    const WotbModV3DeviceApiV1* device =
        QueryApi<WotbModV3DeviceApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_DEVICE,
            WOTBMOD_V3_DEVICE_VERSION);
    if (device) {
        WotbModV3DeviceInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
        uint32_t adapter_count = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            device->get_info(mod, &info) == WOTBMOD_V3_OK,
            "device.get_info");
        Record(
            bootstrap,
            mod,
            counters,
            device->get_graphics_adapter_count(
                mod,
                &adapter_count) == WOTBMOD_V3_OK,
            "device.get_graphics_adapter_count");
        if (adapter_count > 0u) {
            WotbModV3GraphicsAdapterInfo adapter = {};
            WOTBMOD_V3_INIT_STRUCT(adapter, WOTBMOD_V3_ABI_VERSION);
            Record(
                bootstrap,
                mod,
                counters,
                device->get_graphics_adapter_at(
                    mod,
                    0u,
                    &adapter) == WOTBMOD_V3_OK,
                "device.get_graphics_adapter_at");
        }
    }
}

void ProbeUiRenderCamera(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    const WotbModV3UiApiV2* ui =
        QueryApi<WotbModV3UiApiV2>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_UI,
            WOTBMOD_V3_UI_VERSION);
    if (ui) {
        float scale = 0.0f;
        WotbModV3Rect safe_area = {};
        WotbModV3Vec2 viewport = {};
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                ui->get_scale_factor(mod, &scale)),
            "ui.get_scale_factor");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                ui->get_safe_area(mod, &safe_area)),
            "ui.get_safe_area");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                ui->get_viewport_size(mod, &viewport)),
            "ui.get_viewport_size");
    }

    const WotbModV3RenderApiV1* render =
        QueryApi<WotbModV3RenderApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_RENDER,
            WOTBMOD_V3_RENDER_VERSION);
    if (render) {
        uint32_t backend = WOTBMOD_V3_RENDER_BACKEND_NONE;
        WotbModV3Rect viewport = {};
        uint64_t frame = 0u;
        double delta = 0.0;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                render->get_backend(mod, &backend)),
            "render.get_backend");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                render->get_viewport(mod, &viewport)),
            "render.get_viewport");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                render->get_frame_index(mod, &frame)),
            "render.get_frame_index");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                render->get_delta_time(mod, &delta)),
            "render.get_delta_time");
    }

    const WotbModV3RenderNativeApiV1* native =
        QueryApi<WotbModV3RenderNativeApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_RENDER_NATIVE,
            WOTBMOD_V3_RENDER_NATIVE_VERSION);
    if (native) {
        void* device = nullptr;
        void* context = nullptr;
        void* swapchain = nullptr;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                native->get_native_device(mod, &device)),
            "render.native.get_native_device");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                native->get_native_context(mod, &context)),
            "render.native.get_native_context");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                native->get_native_swapchain(mod, &swapchain)),
            "render.native.get_native_swapchain");
    }

    const WotbModV3CameraApiV1* camera =
        QueryApi<WotbModV3CameraApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CAMERA,
            WOTBMOD_V3_CAMERA_VERSION);
    if (camera) {
        WotbModV3CameraHandle active =
            WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result active_result =
            camera->get_active(mod, &active);
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(active_result),
            "camera.get_active");
        if (active_result == WOTBMOD_V3_OK) {
            uint32_t mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
            WotbModV3Transform transform = {};
            WOTBMOD_V3_INIT_STRUCT(
                transform,
                WOTBMOD_V3_ABI_VERSION);
            float fov = 0.0f;
            float near_plane = 0.0f;
            float far_plane = 0.0f;
            Record(
                bootstrap,
                mod,
                counters,
                camera->get_mode(mod, active, &mode) ==
                    WOTBMOD_V3_OK,
                "camera.get_mode");
            Record(
                bootstrap,
                mod,
                counters,
                camera->get_transform(mod, active, &transform) ==
                    WOTBMOD_V3_OK,
                "camera.get_transform");
            Record(
                bootstrap,
                mod,
                counters,
                camera->get_fov(mod, active, &fov) ==
                    WOTBMOD_V3_OK,
                "camera.get_fov");
            Record(
                bootstrap,
                mod,
                counters,
                camera->get_near_plane(
                    mod,
                    active,
                    &near_plane) == WOTBMOD_V3_OK,
                "camera.get_near_plane");
            Record(
                bootstrap,
                mod,
                counters,
                camera->get_far_plane(
                    mod,
                    active,
                    &far_plane) == WOTBMOD_V3_OK,
                "camera.get_far_plane");
        }
    }
}

void ProbeResourcesSceneAudio(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    const WotbModV3ResourcesApiV1* resources =
        QueryApi<WotbModV3ResourcesApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_RESOURCES,
            WOTBMOD_V3_RESOURCES_VERSION);
    if (resources) {
        uint64_t memory = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            resources->get_total_memory_usage(mod, &memory) ==
                WOTBMOD_V3_OK,
            "resources.get_total_memory_usage");

        WotbModV3ResourceLoadDesc desc = {};
        WOTBMOD_V3_INIT_STRUCT(desc, WOTBMOD_V3_ABI_VERSION);
        desc.expected_type = WOTBMOD_V3_RESOURCE_TEXT;
        desc.max_bytes = 4096u;
        CopyText(
            desc.uri,
            sizeof(desc.uri),
            "mod://examples.v3.client_resources/missing-probe.txt");
        WotbModV3ResourceHandle resource =
            WOTBMOD_V3_INVALID_HANDLE;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                resources->load(mod, &desc, &resource)),
            "resources.load missing fixture");
    }

    const WotbModV3SceneApiV1* scene =
        QueryApi<WotbModV3SceneApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_SCENE,
            WOTBMOD_V3_SCENE_VERSION);
    if (scene) {
        WotbModV3SceneHandle active =
            WOTBMOD_V3_INVALID_HANDLE;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                scene->get_active_scene(mod, &active)),
            "scene.get_active_scene");
    }

    const WotbModV3AudioApiV2* audio =
        QueryApi<WotbModV3AudioApiV2>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_AUDIO,
            WOTBMOD_V3_AUDIO_VERSION);
    if (audio) {
        WotbModV3AudioDescriptor descriptor = {};
        WOTBMOD_V3_INIT_STRUCT(
            descriptor,
            WOTBMOD_V3_ABI_VERSION);
        descriptor.uri =
            "mod://examples.v3.client_resources/missing-probe.ogg";
        descriptor.volume = 1.0f;
        descriptor.pitch = 1.0f;
        descriptor.bus = "master";
        WotbModV3AudioHandle handle =
            WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result result =
            audio->create(mod, &descriptor, &handle);
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(result),
            "audio.create missing fixture");
        if (result == WOTBMOD_V3_OK) {
            audio->destroy(mod, handle);
        }
    }
}

void ProbeGameplayAndBigWorld(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    ProbeCounters* counters) {
    const WotbModV3VehicleVisualApiV1* vehicle =
        QueryApi<WotbModV3VehicleVisualApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION);
    if (vehicle) {
        WotbModV3EntityHandle local =
            WOTBMOD_V3_INVALID_HANDLE;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                vehicle->get_local_player_vehicle(mod, &local)),
            "vehicle.visual.get_local_player_vehicle");
    }

    const WotbModV3GameplayCameraApiV1* gameplay_camera =
        QueryApi<WotbModV3GameplayCameraApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
            WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION);
    if (gameplay_camera) {
        float fov = 0.0f;
        WotbModV3GameplayCameraConfig config = {};
        WOTBMOD_V3_INIT_STRUCT(config, WOTBMOD_V3_ABI_VERSION);
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                gameplay_camera->get_fov(mod, &fov)),
            "gameplay.camera.get_fov");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                gameplay_camera->get_config(mod, &config)),
            "gameplay.camera.get_config");
    }

    const WotbModV3GameplayReplayApiV1* replay =
        QueryApi<WotbModV3GameplayReplayApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY,
            WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION);
    if (replay) {
        float duration = 0.0f;
        float position = 0.0f;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                replay->get_duration(mod, &duration)),
            "gameplay.replay.get_duration");
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                replay->get_position(mod, &position)),
            "gameplay.replay.get_position");
    }

    const WotbModV3EntityPublicApiV1* entities =
        QueryApi<WotbModV3EntityPublicApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
            WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    if (entities) {
        uint32_t visible_count = 0u;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                entities->enumerate_visible(
                    mod,
                    &VisibleEntityVisitor,
                    &visible_count)),
            "entity.public.enumerate_visible");
    }

    const WotbModV3BigWorldRpcApiV1* rpc =
        QueryApi<WotbModV3BigWorldRpcApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_BIGWORLD_RPC,
            WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    if (rpc) {
        WotbModV3BigWorldRpcPolicy policy = {};
        WOTBMOD_V3_INIT_STRUCT(policy, WOTBMOD_V3_ABI_VERSION);
        const WotbModV3Result result =
            rpc->get_policy(mod, &policy);
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(result),
            "bigworld.rpc.get_policy");
        if (result == WOTBMOD_V3_OK) {
            Record(
                bootstrap,
                mod,
                counters,
                policy.payload_access == 0u &&
                    policy.outgoing_injection == 0u &&
                    policy.packet_modification == 0u &&
                    policy.packet_drop == 0u &&
                    policy.packet_replay == 0u,
                "bigworld.rpc safety policy");
        }
    }

    const WotbModV3ProjectileApiV1* projectile =
        QueryApi<WotbModV3ProjectileApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_PROJECTILE,
            WOTBMOD_V3_PROJECTILE_VERSION);
    if (projectile) {
        uint32_t owner_scope =
            WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
        Record(
            bootstrap,
            mod,
            counters,
            IsExpectedObservationResult(
                projectile->projectile_get_owner_scope(
                    mod,
                    WOTBMOD_V3_INVALID_HANDLE,
                    &owner_scope)),
            "projectile invalid-handle validation");
    }
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    ProbeCounters counters = {};
    const InterfaceProbe probes[] = {
        {WOTBMOD_V3_IFACE_UI,
         WOTBMOD_V3_UI_VERSION,
         sizeof(WotbModV3UiApiV2)},
        {WOTBMOD_V3_IFACE_RESOURCES,
         WOTBMOD_V3_RESOURCES_VERSION,
         sizeof(WotbModV3ResourcesApiV1)},
        {WOTBMOD_V3_IFACE_RENDER,
         WOTBMOD_V3_RENDER_VERSION,
         sizeof(WotbModV3RenderApiV1)},
        {WOTBMOD_V3_IFACE_RENDER_NATIVE,
         WOTBMOD_V3_RENDER_NATIVE_VERSION,
         sizeof(WotbModV3RenderNativeApiV1)},
        {WOTBMOD_V3_IFACE_CAMERA,
         WOTBMOD_V3_CAMERA_VERSION,
         sizeof(WotbModV3CameraApiV1)},
        {WOTBMOD_V3_IFACE_SCENE,
         WOTBMOD_V3_SCENE_VERSION,
         sizeof(WotbModV3SceneApiV1)},
        {WOTBMOD_V3_IFACE_AUDIO,
         WOTBMOD_V3_AUDIO_VERSION,
         sizeof(WotbModV3AudioApiV2)},
        {WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
         WOTBMOD_V3_VEHICLE_VISUAL_VERSION,
         sizeof(WotbModV3VehicleVisualApiV1)},
        {WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
         WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION,
         sizeof(WotbModV3GameplayCameraApiV1)},
        {WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
         WOTBMOD_V3_GAMEPLAY_HUD_VERSION,
         sizeof(WotbModV3GameplayHudApiV1)},
        {WOTBMOD_V3_IFACE_GAMEPLAY_HANGAR,
         WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION,
         sizeof(WotbModV3GameplayHangarApiV1)},
        {WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY,
         WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION,
         sizeof(WotbModV3GameplayReplayApiV1)},
        {WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
         WOTBMOD_V3_ENTITY_PUBLIC_VERSION,
         sizeof(WotbModV3EntityPublicApiV1)},
        {WOTBMOD_V3_IFACE_BIGWORLD_RPC,
         WOTBMOD_V3_BIGWORLD_RPC_VERSION,
         sizeof(WotbModV3BigWorldRpcApiV1)},
        {WOTBMOD_V3_IFACE_PROJECTILE,
         WOTBMOD_V3_PROJECTILE_VERSION,
         sizeof(WotbModV3ProjectileApiV1)},
        {WOTBMOD_V3_IFACE_CLIENT,
         WOTBMOD_V3_CLIENT_VERSION,
         sizeof(WotbModV3ClientApiV1)},
        {WOTBMOD_V3_IFACE_DEVICE,
         WOTBMOD_V3_DEVICE_VERSION,
         sizeof(WotbModV3DeviceApiV1)}
    };
    for (const InterfaceProbe& probe : probes) {
        ProbeInterface(bootstrap, mod, probe, &counters);
    }

    ProbeClientAndDevice(bootstrap, mod, &counters);
    ProbeUiRenderCamera(bootstrap, mod, &counters);
    ProbeResourcesSceneAudio(bootstrap, mod, &counters);
    ProbeGameplayAndBigWorld(bootstrap, mod, &counters);

    char summary[WOTBMOD_V3_MAX_MESSAGE] = {};
#if defined(_MSC_VER)
    sprintf_s(
        summary,
        "probe complete: passed=%u failed=%u unavailable=%u",
        counters.passed,
        counters.failed,
        counters.unavailable);
#else
    std::snprintf(
        summary,
        sizeof(summary),
        "probe complete: passed=%u failed=%u unavailable=%u",
        counters.passed,
        counters.failed,
        counters.unavailable);
#endif
    Log(
        bootstrap,
        mod,
        counters.failed == 0u
            ? WOTBMOD_V3_LOG_INFO
            : WOTBMOD_V3_LOG_ERROR,
        summary);
}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    Log(
        bootstrap,
        mod,
        WOTBMOD_V3_LOG_INFO,
        "client/resources/gameplay probe disabled");
}

}  // namespace

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info ||
        bootstrap->struct_size < sizeof(WotbModV3Bootstrap) ||
        bootstrap->api_version != WOTBMOD_V3_ABI_VERSION ||
        !bootstrap->query_interface ||
        !bootstrap->get_interface_info ||
        !bootstrap->get_last_error ||
        !bootstrap->get_client_info ||
        mod == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_UNSAFE;
    CopyText(
        out_info->id,
        sizeof(out_info->id),
        "examples.v3.client_resources");
    CopyText(
        out_info->name,
        sizeof(out_info->name),
        "V3 Client Resources Gameplay Probe");
    CopyText(out_info->version, sizeof(out_info->version), "1.0.0");
    CopyText(
        out_info->author,
        sizeof(out_info->author),
        "WotbMod SDK");
    CopyText(
        out_info->description,
        sizeof(out_info->description),
        "Queries every client/resource/gameplay interface, validates every function slot, and performs non-destructive native availability probes.");
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    return WOTBMOD_V3_OK;
}
