#ifndef WOTBMOD_V3_NATIVE_CAMERA_STATE_H_
#define WOTBMOD_V3_NATIVE_CAMERA_STATE_H_

#include "../include/wotbmod/camera_v1.h"

#include <cmath>

namespace wotbmod {
namespace v3 {
namespace native_camera {

struct CameraWriteMask {
    bool transform = false;
    bool fov = false;
    bool near_plane = false;
    bool far_plane = false;

    bool Any() const {
        return transform || fov || near_plane || far_plane;
    }
};

inline bool SameVec3(
    const WotbModV3Vec3& left,
    const WotbModV3Vec3& right) {
    return left.x == right.x && left.y == right.y && left.z == right.z;
}

inline bool SameVec4(
    const WotbModV3Vec4& left,
    const WotbModV3Vec4& right) {
    return left.x == right.x && left.y == right.y &&
           left.z == right.z && left.w == right.w;
}

inline bool SameTransform(
    const WotbModV3Transform& left,
    const WotbModV3Transform& right) {
    return SameVec3(left.position, right.position) &&
           SameVec4(left.rotation, right.rotation) &&
           SameVec3(left.scale, right.scale);
}

inline CameraWriteMask CameraWritesFor(
    const WotbModV3CameraState& stock,
    const WotbModV3CameraState& modified) {
    CameraWriteMask writes = {};
    writes.transform = !SameTransform(stock.transform, modified.transform);
    writes.fov = stock.fov_degrees != modified.fov_degrees;
    writes.near_plane = stock.near_plane != modified.near_plane;
    writes.far_plane = stock.far_plane != modified.far_plane;
    return writes;
}

inline WotbModV3Vec3 TargetWithPreservedDistance(
    const WotbModV3Vec3& new_position,
    const WotbModV3Vec3& new_forward,
    const WotbModV3Vec3& old_position,
    const WotbModV3Vec3& old_target) {
    const float target_dx = old_target.x - old_position.x;
    const float target_dy = old_target.y - old_position.y;
    const float target_dz = old_target.z - old_position.z;
    float target_distance = std::sqrt(
        target_dx * target_dx + target_dy * target_dy +
        target_dz * target_dz);
    if (!std::isfinite(target_distance) || target_distance <= 0.000001f) {
        target_distance = 1.0f;
    }

    const float forward_length = std::sqrt(
        new_forward.x * new_forward.x +
        new_forward.y * new_forward.y +
        new_forward.z * new_forward.z);
    const float scale =
        std::isfinite(forward_length) && forward_length > 0.000001f
        ? target_distance / forward_length
        : target_distance;
    const WotbModV3Vec3 forward =
        std::isfinite(forward_length) && forward_length > 0.000001f
        ? new_forward
        : WotbModV3Vec3{0.0f, 0.0f, 1.0f};
    return {
        new_position.x + forward.x * scale,
        new_position.y + forward.y * scale,
        new_position.z + forward.z * scale};
}

}  // namespace native_camera
}  // namespace v3
}  // namespace wotbmod

#endif  // WOTBMOD_V3_NATIVE_CAMERA_STATE_H_
