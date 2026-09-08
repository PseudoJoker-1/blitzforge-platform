#include "../include/wotbmod/audio_v2.h"
#include "../include/wotbmod/bigworld_rpc_v1.h"
#include "../include/wotbmod/camera_v1.h"
#include "../include/wotbmod/client_v1.h"
#include "../include/wotbmod/device_v1.h"
#include "../include/wotbmod/devtools_v3.h"
#include "../include/wotbmod/entity_public_v1.h"
#include "../include/wotbmod/gameplay_camera_v1.h"
#include "../include/wotbmod/gameplay_hangar_v1.h"
#include "../include/wotbmod/gameplay_hud_v1.h"
#include "../include/wotbmod/gameplay_replay_v1.h"
#include "../include/wotbmod/projectile_v2.h"
#include "../include/wotbmod/render_v1.h"
#include "../include/wotbmod/scene_v1.h"
#include "../include/wotbmod/ui_v3.h"
#include "../include/wotbmod/vehicle_visual_v2.h"
#include "../src/v3/client_services_backend.h"

#include <cstddef>
#include <type_traits>

static_assert(
    std::is_standard_layout<WotbModV3UiApiV2>::value,
    "UI API table must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3UiApiV3>::value,
    "UI V3 API table must retain C ABI layout");
static_assert(
    offsetof(WotbModV3UiApiV3, get_active_screen) ==
        sizeof(WotbModV3UiApiV2),
    "UI V3 must remain prefix-compatible with V2");
static_assert(
    std::is_standard_layout<WotbModV3RenderApiV1>::value,
    "render API table must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3RenderLifecycleEvent>::value,
    "render lifecycle payload must retain C ABI layout");
static_assert(
    sizeof(WotbModV3RenderLifecycleEvent) == 72u,
    "render lifecycle payload size is part of the public ABI");
static_assert(
    offsetof(WotbModV3RenderLifecycleEvent, frame_index) == 32u,
    "render lifecycle frame index offset is part of the public ABI");
static_assert(
    offsetof(WotbModV3RenderLifecycleEvent, previous_viewport) == 40u,
    "render lifecycle previous viewport offset is part of the public ABI");
static_assert(
    offsetof(WotbModV3RenderLifecycleEvent, viewport) == 56u,
    "render lifecycle viewport offset is part of the public ABI");
static_assert(
    std::is_standard_layout<WotbModV3CameraApiV1>::value,
    "camera API table must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3SceneApiV1>::value,
    "scene API table must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3AudioApiV2>::value,
    "audio API table must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3EntityPublicApiV1>::value,
    "entity API table must retain C ABI layout");
static_assert(
    std::is_standard_layout<
        WotbModV3PublicEntityLifecycleEvent>::value,
    "public entity event must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3ObservedRpc>::value,
    "RPC metadata must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3ProjectileApiV1>::value,
    "projectile API table must retain C ABI layout");
static_assert(
    std::is_standard_layout<WotbModV3ProjectileApiV2>::value,
    "projectile V2 API table must retain C ABI layout");
static_assert(
    offsetof(WotbModV3ProjectileApiV2, projectile_get_snapshot) ==
        sizeof(WotbModV3ProjectileApiV1),
    "projectile V2 must remain prefix-compatible with V1");
static_assert(
    std::is_standard_layout<WotbModV3VehicleVisualApiV2>::value,
    "vehicle visual V2 API table must retain C ABI layout");
static_assert(
    offsetof(WotbModV3VehicleVisualApiV2, skin_pack_register) ==
        sizeof(WotbModV3VehicleVisualApiV1),
    "vehicle visual V2 must remain prefix-compatible with V1");
static_assert(
    std::is_standard_layout<WotbModV3DevtoolsApiV3>::value,
    "devtools V3 API table must retain C ABI layout");
static_assert(
    offsetof(WotbModV3DevtoolsApiV3, get_callback_profile) ==
        sizeof(WotbModV3DevtoolsApiV2),
    "devtools V3 must remain prefix-compatible with V2");
static_assert(
    std::is_standard_layout<wotbmod::v3::ClientHostBackend>::value,
    "host bridge must remain a standard-layout ABI structure");

int main() {
    WotbModV3AudioDescriptor audio = {};
    WOTBMOD_V3_INIT_STRUCT(audio, WOTBMOD_V3_AUDIO_VERSION);

    WotbModV3CameraState camera = {};
    WOTBMOD_V3_INIT_STRUCT(camera, WOTBMOD_V3_CAMERA_VERSION);

    WotbModV3PublicEntitySnapshot entity = {};
    WOTBMOD_V3_INIT_STRUCT(
        entity,
        WOTBMOD_V3_ENTITY_PUBLIC_VERSION);

    WotbModV3TracerStyleDescriptor tracer = {};
    WOTBMOD_V3_INIT_STRUCT(
        tracer,
        WOTBMOD_V3_PROJECTILE_VERSION);

    WotbModV3ImpactVisualDescriptor impact = {};
    WOTBMOD_V3_INIT_STRUCT(
        impact,
        WOTBMOD_V3_PROJECTILE_VERSION_2);

    WotbModV3UiControlSnapshot ui = {};
    WOTBMOD_V3_INIT_STRUCT(ui, WOTBMOD_V3_UI_VERSION_3);

    WotbModV3VehicleSkinState skin = {};
    WOTBMOD_V3_INIT_STRUCT(
        skin,
        WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);

    WotbModV3CallbackProfile callbacks = {};
    WOTBMOD_V3_INIT_STRUCT(
        callbacks,
        WOTBMOD_V3_DEVTOOLS_VERSION_3);

    wotbmod::v3::ClientHostBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;

    return audio.struct_size != 0u &&
                   camera.struct_size != 0u &&
                   entity.struct_size != 0u &&
                   tracer.struct_size != 0u &&
                   impact.struct_size != 0u &&
                   ui.struct_size != 0u &&
                   skin.struct_size != 0u &&
                   callbacks.struct_size != 0u &&
                   backend.struct_size != 0u
               ? 0
               : 1;
}
