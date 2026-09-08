#ifndef WOTBMOD_V3_NATIVE_CAMERA_LAYOUT_H_
#define WOTBMOD_V3_NATIVE_CAMERA_LAYOUT_H_

#include <cstddef>

namespace wotbmod {
namespace v3 {
namespace native_layout {

/*
 * DAVA::Camera projection-plane fields for the verified 11.20.0.887 x86
 * client.  Keep these offsets coupled to the executable fingerprint in
 * v3_native_bindings.cpp; they are not a public ABI.
 */
constexpr std::size_t kCameraNearPlaneOffset = 0x20u;
constexpr std::size_t kCameraFarPlaneOffset = 0x24u;

constexpr std::size_t CameraProjectionPlaneOffset(bool near_plane) {
    return near_plane
        ? kCameraNearPlaneOffset
        : kCameraFarPlaneOffset;
}

}  // namespace native_layout
}  // namespace v3
}  // namespace wotbmod

#endif  // WOTBMOD_V3_NATIVE_CAMERA_LAYOUT_H_
