#include "../include/wotb_mod_runtime.h"
#include "../include/wotb_mod_windows_audio.h"
#include "../include/wotb_mod_dava_sound.h"
#include "../include/wotb_mod_dava_resources.h"
#include "../include/wotbmod/ges_v1.h"
#include "../include/wotbmod/session_cluster_v1.h"

static int VerifyPublicTypes(void) {
    WotbModInfo info = {0};
    WotbModFrameInfo frame = {0};
    WotbModResourceMountInfo mount = {0};
    WotbModResourceLoadRequest load = {0};
    WotbModAudioPlayInfo audio = {0};
    WotbModUiControlGeometry geometry = {0};
    WotbModUiControlState ui_state = {0};
    WotbModSceneTransform transform = {0};
    WotbModVehicleInfo vehicle_info = {0};
    WotbModVehicleSkinAsset skin_asset = {0};
    WotbModVehicleSkinDescriptor skin_descriptor = {0};
    WotbModVehicleSkinInfo skin_info = {0};
    WotbModClientEvent client_event = {0};
    WotbModHostApi host = {0};
    WotbModRuntimeHookBackend hooks = {0};
    WotbModRuntimeResourceBackend resources = {0};
    WotbModRuntimeAudioBackend audio_backend = {0};
    WotbModRuntimeSoundBackend sound_backend = {0};
    WotbModRuntimeGameplayBackend gameplay_backend = {0};
    WotbModRuntimeClientEvent runtime_client_event = {0};
    WotbModWindowsAudioOptions windows_audio = {0};
    WotbModDavaSoundOptions dava_sound = {0};
    WotbModDavaResourcesOptions dava_resources = {0};
    WotbModRuntimeOptions options = {0};
    info.struct_size = (uint32_t)sizeof(info);
    frame.struct_size = (uint32_t)sizeof(frame);
    mount.struct_size = (uint32_t)sizeof(mount);
    load.struct_size = (uint32_t)sizeof(load);
    audio.struct_size = (uint32_t)sizeof(audio);
    geometry.struct_size = (uint32_t)sizeof(geometry);
    ui_state.struct_size = (uint32_t)sizeof(ui_state);
    transform.struct_size = (uint32_t)sizeof(transform);
    vehicle_info.struct_size = (uint32_t)sizeof(vehicle_info);
    skin_asset.struct_size = (uint32_t)sizeof(skin_asset);
    skin_descriptor.struct_size =
        (uint32_t)sizeof(skin_descriptor);
    skin_info.struct_size = (uint32_t)sizeof(skin_info);
    client_event.struct_size = (uint32_t)sizeof(client_event);
    host.struct_size = (uint32_t)sizeof(host);
    hooks.struct_size = (uint32_t)sizeof(hooks);
    resources.struct_size = (uint32_t)sizeof(resources);
    audio_backend.struct_size = (uint32_t)sizeof(audio_backend);
    sound_backend.struct_size = (uint32_t)sizeof(sound_backend);
    gameplay_backend.struct_size = (uint32_t)sizeof(gameplay_backend);
    runtime_client_event.struct_size =
        (uint32_t)sizeof(runtime_client_event);
    windows_audio.struct_size = (uint32_t)sizeof(windows_audio);
    dava_sound.struct_size = (uint32_t)sizeof(dava_sound);
    dava_resources.struct_size = (uint32_t)sizeof(dava_resources);
    options.struct_size = (uint32_t)sizeof(options);
    return info.struct_size > 0 &&
           frame.struct_size > 0 &&
           mount.struct_size > 0 &&
           load.struct_size > 0 &&
           audio.struct_size > 0 &&
           geometry.struct_size > 0 &&
           ui_state.struct_size > 0 &&
           transform.struct_size > 0 &&
           vehicle_info.struct_size > 0 &&
           skin_asset.struct_size > 0 &&
           skin_descriptor.struct_size > 0 &&
           skin_info.struct_size > 0 &&
           client_event.struct_size > 0 &&
           host.struct_size > 0 &&
           hooks.struct_size > 0 &&
           resources.struct_size > 0 &&
           audio_backend.struct_size > 0 &&
           sound_backend.struct_size > 0 &&
           gameplay_backend.struct_size > 0 &&
           runtime_client_event.struct_size > 0 &&
           windows_audio.struct_size > 0 &&
           dava_sound.struct_size > 0 &&
           dava_resources.struct_size > 0 &&
           options.struct_size > 0 &&
           WotbModRuntime_GetHostApi != 0 &&
           WotbModRuntime_ResolveResourcePath != 0 &&
           WotbModRuntime_ResolveVehicleSkinPath != 0 &&
           WotbModRuntime_NotifyUiScreenChanged != 0 &&
           WotbModRuntime_NotifySceneActivated != 0 &&
           WotbModRuntime_NotifySceneDeactivated != 0 &&
           WotbModRuntime_NotifyClientEvent != 0 &&
           WotbModWindowsAudio_Create != 0 &&
           WotbModWindowsAudio_Destroy != 0 &&
           WotbModDavaSound_Create != 0 &&
           WotbModDavaSound_Destroy != 0 &&
           offsetof(
               WotbModHostApi,
               main_thread_enqueue) <
               offsetof(
                   WotbModHostApi,
                   scene_remove_child) &&
           offsetof(
               WotbModHostApi,
               scene_remove_child) <
               offsetof(
                   WotbModHostApi,
                   ui_control_create) &&
           offsetof(
               WotbModHostApi,
               ui_control_create) <
               offsetof(
               WotbModHostApi,
                   scene_get_active) &&
           offsetof(
               WotbModHostApi,
               scene_get_active) <
               offsetof(
                   WotbModHostApi,
                   resource_clone) &&
           offsetof(
               WotbModHostApi,
               resource_clone) <
               offsetof(
                   WotbModHostApi,
                   event_subscribe) &&
           offsetof(
               WotbModHostApi,
               event_subscribe) <
               offsetof(
                   WotbModHostApi,
                   event_unsubscribe) &&
           offsetof(
               WotbModHostApi,
               event_unsubscribe) <
               offsetof(
                   WotbModHostApi,
                   ui_control_find_by_name) &&
           offsetof(
               WotbModHostApi,
               ui_control_find_by_name) <
               offsetof(
                   WotbModHostApi,
                   ui_control_set_disabled) &&
           offsetof(
               WotbModHostApi,
               ui_control_set_disabled) <
               offsetof(
                   WotbModHostApi,
                   vehicle_get_local) &&
           offsetof(
               WotbModHostApi,
               vehicle_get_local) <
               offsetof(
               WotbModHostApi,
               vehicle_get_info) &&
           offsetof(
               WotbModHostApi,
               vehicle_get_info) <
               offsetof(
                   WotbModHostApi,
                   vehicle_skin_register) &&
           offsetof(
               WotbModHostApi,
               vehicle_skin_register) <
               offsetof(
                   WotbModHostApi,
                   vehicle_skin_release) &&
           offsetof(
               WotbModRuntimeResourceBackend,
               registry_changed) <
               offsetof(
                   WotbModRuntimeResourceBackend,
                   ui_set_geometry) &&
           offsetof(
               WotbModRuntimeResourceBackend,
               scene_remove_child) <
               offsetof(
               WotbModRuntimeResourceBackend,
                   ui_create) &&
           offsetof(
               WotbModRuntimeResourceBackend,
               scene_get_active) <
               offsetof(
                   WotbModRuntimeResourceBackend,
                   clone) &&
           offsetof(
               WotbModRuntimeResourceBackend,
               clone) <
               offsetof(
                   WotbModRuntimeResourceBackend,
                   ui_find_by_name) &&
           offsetof(
               WotbModRuntimeResourceBackend,
               ui_find_by_name) <
               offsetof(
                   WotbModRuntimeResourceBackend,
                   ui_set_disabled) &&
           offsetof(
               WotbModRuntimeOptions,
               sound_backend) <
               offsetof(
                   WotbModRuntimeOptions,
                   gameplay_backend) &&
           WotbModDavaResources_GetSceneDrawTarget != 0 &&
           WotbModDavaResources_GetSceneActivateTarget != 0 &&
           WotbModDavaResources_GetSceneDeactivateTarget != 0 &&
           WotbModDavaResources_GetNativeObject != 0 &&
           WotbModDavaResources_TrackActiveScene != 0 &&
           WotbModDavaResources_UntrackActiveScene != 0;
}

int wotbmod_c_abi_compile_test(void) {
    return VerifyPublicTypes() ? 0 : 1;
}
