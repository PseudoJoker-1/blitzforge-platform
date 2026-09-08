#include "../loader/v3_native_camera_layout.h"
#include "../loader/v3_native_camera_state.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

namespace {

uint32_t g_passed = 0u;
uint32_t g_failed = 0u;

void Check(bool condition, const char* label) {
    if (condition) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("FAIL: %s\n", label);
}

WotbModV3CameraState CameraState() {
    WotbModV3CameraState state = {};
    state.struct_size = sizeof(state);
    state.api_version = WOTBMOD_V3_CAMERA_VERSION;
    state.transform.struct_size = sizeof(state.transform);
    state.transform.api_version = WOTBMOD_V3_ABI_VERSION;
    state.transform.position = {10.0f, 20.0f, 30.0f};
    state.transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    state.transform.scale = {1.0f, 1.0f, 1.0f};
    state.fov_degrees = 60.0f;
    state.near_plane = 0.1f;
    state.far_plane = 1000.0f;
    return state;
}

}  // namespace

int main() {
    using wotbmod::v3::native_layout::CameraProjectionPlaneOffset;
    using wotbmod::v3::native_layout::kCameraFarPlaneOffset;
    using wotbmod::v3::native_layout::kCameraNearPlaneOffset;

    Check(kCameraNearPlaneOffset == 0x20u, "near plane is DAVA::Camera +0x20");
    Check(kCameraFarPlaneOffset == 0x24u, "far plane is DAVA::Camera +0x24");
    Check(
        CameraProjectionPlaneOffset(true) == kCameraNearPlaneOffset,
        "near selector uses near offset");
    Check(
        CameraProjectionPlaneOffset(false) == kCameraFarPlaneOffset,
        "far selector uses far offset");
    Check(
        kCameraNearPlaneOffset != kCameraFarPlaneOffset,
        "projection fields do not alias");

    const WotbModV3CameraState stock = CameraState();
    WotbModV3CameraState modified = stock;
    auto writes =
        wotbmod::v3::native_camera::CameraWritesFor(stock, modified);
    Check(!writes.Any(), "unchanged camera state performs no native writes");

    modified.transform.position.x += 1.0f;
    writes = wotbmod::v3::native_camera::CameraWritesFor(stock, modified);
    Check(
        writes.transform && !writes.fov && !writes.near_plane &&
            !writes.far_plane,
        "transform change writes only native transform");

    modified = stock;
    modified.fov_degrees = 75.0f;
    writes = wotbmod::v3::native_camera::CameraWritesFor(stock, modified);
    Check(
        !writes.transform && writes.fov && !writes.near_plane &&
            !writes.far_plane,
        "FOV change writes only native FOV");

    modified = stock;
    modified.near_plane = 0.2f;
    modified.far_plane = 1500.0f;
    writes = wotbmod::v3::native_camera::CameraWritesFor(stock, modified);
    Check(
        !writes.transform && !writes.fov && writes.near_plane &&
            writes.far_plane,
        "clipping changes write only native clipping fields");

    const WotbModV3Vec3 old_position = {1.0f, 2.0f, 3.0f};
    const WotbModV3Vec3 old_target = {1.0f, 14.0f, 3.0f};
    const WotbModV3Vec3 new_position = {5.0f, 6.0f, 7.0f};
    const WotbModV3Vec3 new_forward = {0.0f, 0.0f, 1.0f};
    const WotbModV3Vec3 new_target =
        wotbmod::v3::native_camera::TargetWithPreservedDistance(
            new_position,
            new_forward,
            old_position,
            old_target);
    Check(
        std::fabs(new_target.x - 5.0f) < 0.0001f &&
            std::fabs(new_target.y - 6.0f) < 0.0001f &&
            std::fabs(new_target.z - 19.0f) < 0.0001f,
        "camera target preserves the stock twelve-unit distance");

    std::printf(
        "V3 native camera layout: %u passed, %u failed\n",
        g_passed,
        g_failed);
    return g_failed == 0u ? 0 : 1;
}
