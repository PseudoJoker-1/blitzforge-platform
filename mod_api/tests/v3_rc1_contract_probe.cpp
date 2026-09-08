#include "../include/wotb_mod_api_v3.h"

/*
 * The five tables declared 2026-08-16 are not reachable through
 * wotb_mod_api_v3.h yet -- that aggregate header is owned by the wave that
 * wires the backends in. Including them directly here still freezes their x86
 * layout from the moment they are declared, which is the point of this probe.
 */
#include "../include/wotbmod/ui_v4.h"
#include "../include/wotbmod/camera_v2.h"
#include "../include/wotbmod/audio_v3.h"
#include "../include/wotbmod/scene_v2.h"
#include "../include/wotbmod/tracer_v1.h"

#include <cstdio>

#define CHECK_SIZE(type, expected)                                        \
    static_assert(sizeof(type) == expected, #type " RC1 table size changed")

int main() {
    static_assert(sizeof(void*) == 4u, "RC1 ABI probe must be built for x86");
    CHECK_SIZE(WotbModV3ArchiveApiV1, 48u);
    CHECK_SIZE(WotbModV3AsyncApiV1, 88u);
    CHECK_SIZE(WotbModV3AudioApiV2, 204u);
    CHECK_SIZE(WotbModV3AudioApiV3, 24u);
    CHECK_SIZE(WotbModV3BigWorldRpcApiV1, 20u);
    CHECK_SIZE(WotbModV3CameraApiV1, 64u);
    CHECK_SIZE(WotbModV3CameraApiV2, 20u);
    CHECK_SIZE(WotbModV3CapabilitiesApiV1, 28u);
    CHECK_SIZE(WotbModV3CatalogApiV1, 20u);
    CHECK_SIZE(WotbModV3ClientApiV1, 28u);
    CHECK_SIZE(WotbModV3ContentApiV1, 36u);
    CHECK_SIZE(WotbModV3CoreApiV1, 40u);
    CHECK_SIZE(WotbModV3DeviceApiV1, 20u);
    CHECK_SIZE(WotbModV3DevtoolsApiV1, 24u);
    CHECK_SIZE(WotbModV3DevtoolsApiV2, 56u);
    CHECK_SIZE(WotbModV3DevtoolsApiV3, 68u);
    CHECK_SIZE(WotbModV3DiagnosticsApiV1, 20u);
    CHECK_SIZE(WotbModV3DiagnosticsApiV2, 40u);
    CHECK_SIZE(WotbModV3EntityPublicApiV1, 40u);
    CHECK_SIZE(WotbModV3EventsApiV1, 44u);
    CHECK_SIZE(WotbModV3GameplayCameraApiV1, 128u);
    CHECK_SIZE(WotbModV3GameplayHangarApiV1, 52u);
    CHECK_SIZE(WotbModV3GameplayHudApiV1, 144u);
    CHECK_SIZE(WotbModV3GameplayReplayApiV1, 44u);
    CHECK_SIZE(WotbModV3HandlesApiV1, 24u);
    CHECK_SIZE(WotbModV3HooksApiV1, 72u);
    CHECK_SIZE(WotbModV3HttpApiV1, 60u);
    CHECK_SIZE(WotbModV3InputApiV1, 56u);
    CHECK_SIZE(WotbModV3IntermodApiV1, 52u);
    CHECK_SIZE(WotbModV3LifecycleApiV1, 60u);
    CHECK_SIZE(WotbModV3LoadersApiV1, 36u);
    CHECK_SIZE(WotbModV3ManifestApiV1, 68u);
    CHECK_SIZE(WotbModV3PermissionsApiV1, 24u);
    CHECK_SIZE(WotbModV3ProjectileApiV1, 48u);
    CHECK_SIZE(WotbModV3ProjectileApiV2, 64u);
    CHECK_SIZE(WotbModV3RenderApiV1, 84u);
    CHECK_SIZE(WotbModV3RenderNativeApiV1, 20u);
    CHECK_SIZE(WotbModV3ResourcesApiV1, 52u);
    CHECK_SIZE(WotbModV3SceneApiV1, 120u);
    CHECK_SIZE(WotbModV3SceneApiV2, 16u);
    CHECK_SIZE(WotbModV3SettingsApiV1, 84u);
    CHECK_SIZE(WotbModV3StorageApiV1, 64u);
    CHECK_SIZE(WotbModV3TracerApiV1, 16u);
    CHECK_SIZE(WotbModV3UiApiV2, 296u);
    CHECK_SIZE(WotbModV3UiApiV3, 304u);
    // Declared 2026-08-16 (post-RC1, never SUPPORTED live); grew by one slot
    // on 2026-09-04 (control_get_live_text), struct_size-gated for callers.
    CHECK_SIZE(WotbModV3UiApiV4, 28u);
    CHECK_SIZE(WotbModV3UnsafeNativeApiV1, 12u);
    CHECK_SIZE(WotbModV3VehicleVisualApiV1, 128u);
    CHECK_SIZE(WotbModV3VehicleVisualApiV2, 148u);
    CHECK_SIZE(WotbModV3VfsApiV1, 64u);
    CHECK_SIZE(WotbModV3YamlApiV1, 52u);
    std::printf("RC1 ABI TABLES OK: 51 x86 sizes frozen\n");
    return 0;
}
