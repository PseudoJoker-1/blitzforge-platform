#include "v3_camera_effects.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../include/wotbmod/gameplay_camera_v1.h"

namespace wotbmod {
namespace loader {
namespace {

constexpr size_t kMaxControllers = 128u;
constexpr size_t kMaxTransitions = 64u;
constexpr size_t kMaxShakes = 64u;
constexpr size_t kMaxShakesPerOwner = 8u;
constexpr float kMaxTransitionSeconds = 30.0f;
constexpr float kMaxShakeSeconds = 30.0f;
constexpr float kMaxShakeAmplitude = 25.0f;
constexpr float kMaxCombinedShakeAmplitude = 25.0f;
constexpr float kMaxShakeFrequency = 120.0f;
constexpr float kMaxShakeFalloff = 16.0f;
constexpr float kMaxNativeFov = 140.0f;
constexpr float kMaxCoordinate = 10000000.0f;
constexpr double kPi = 3.14159265358979323846;

bool Finite(float value) {
    return std::isfinite(value);
}

bool Finite(const WotbModV3Vec3& value) {
    return Finite(value.x) && Finite(value.y) && Finite(value.z);
}

bool Finite(const WotbModV3Vec4& value) {
    return Finite(value.x) && Finite(value.y) &&
           Finite(value.z) && Finite(value.w);
}

bool ValidTransform(const WotbModV3Transform& transform) {
    if (transform.struct_size < sizeof(transform) ||
        transform.api_version != WOTBMOD_V3_ABI_VERSION ||
        !Finite(transform.position) ||
        !Finite(transform.rotation) ||
        !Finite(transform.scale) ||
        std::fabs(transform.position.x) > kMaxCoordinate ||
        std::fabs(transform.position.y) > kMaxCoordinate ||
        std::fabs(transform.position.z) > kMaxCoordinate) {
        return false;
    }
    const double rotation_length_squared =
        static_cast<double>(transform.rotation.x) *
            transform.rotation.x +
        static_cast<double>(transform.rotation.y) *
            transform.rotation.y +
        static_cast<double>(transform.rotation.z) *
            transform.rotation.z +
        static_cast<double>(transform.rotation.w) *
            transform.rotation.w;
    return std::isfinite(rotation_length_squared) &&
           rotation_length_squared > 0.000000000001;
}

WotbModV3Vec3 Lerp(
    const WotbModV3Vec3& start,
    const WotbModV3Vec3& target,
    float amount) {
    return {
        start.x + (target.x - start.x) * amount,
        start.y + (target.y - start.y) * amount,
        start.z + (target.z - start.z) * amount};
}

WotbModV3Vec4 Normalize(const WotbModV3Vec4& value) {
    const double length_squared =
        static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y +
        static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w;
    if (!std::isfinite(length_squared) ||
        length_squared <= 0.000000000001) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inverse_length =
        static_cast<float>(1.0 / std::sqrt(length_squared));
    return {
        value.x * inverse_length,
        value.y * inverse_length,
        value.z * inverse_length,
        value.w * inverse_length};
}

WotbModV3Vec4 Slerp(
    const WotbModV3Vec4& start_value,
    const WotbModV3Vec4& target_value,
    float amount) {
    const WotbModV3Vec4 start = Normalize(start_value);
    WotbModV3Vec4 target = Normalize(target_value);
    float dot =
        start.x * target.x + start.y * target.y +
        start.z * target.z + start.w * target.w;
    if (dot < 0.0f) {
        target = {-target.x, -target.y, -target.z, -target.w};
        dot = -dot;
    }
    dot = std::max(-1.0f, std::min(1.0f, dot));
    if (dot > 0.9995f) {
        return Normalize(
            {start.x + (target.x - start.x) * amount,
             start.y + (target.y - start.y) * amount,
             start.z + (target.z - start.z) * amount,
             start.w + (target.w - start.w) * amount});
    }
    const float angle = std::acos(dot);
    const float sine = std::sin(angle);
    if (!(std::fabs(sine) > 0.000001f)) {
        return start;
    }
    const float start_weight =
        std::sin((1.0f - amount) * angle) / sine;
    const float target_weight =
        std::sin(amount * angle) / sine;
    return Normalize(
        {start.x * start_weight + target.x * target_weight,
         start.y * start_weight + target.y * target_weight,
         start.z * start_weight + target.z * target_weight,
         start.w * start_weight + target.w * target_weight});
}

WotbModV3Vec3 Rotate(
    const WotbModV3Vec4& rotation_value,
    const WotbModV3Vec3& vector) {
    const WotbModV3Vec4 rotation = Normalize(rotation_value);
    const WotbModV3Vec3 q = {
        rotation.x, rotation.y, rotation.z};
    const WotbModV3Vec3 cross = {
        q.y * vector.z - q.z * vector.y,
        q.z * vector.x - q.x * vector.z,
        q.x * vector.y - q.y * vector.x};
    const WotbModV3Vec3 second_cross = {
        q.y * cross.z - q.z * cross.y,
        q.z * cross.x - q.x * cross.z,
        q.x * cross.y - q.y * cross.x};
    return {
        vector.x +
            2.0f * (rotation.w * cross.x + second_cross.x),
        vector.y +
            2.0f * (rotation.w * cross.y + second_cross.y),
        vector.z +
            2.0f * (rotation.w * cross.z + second_cross.z)};
}

float Ease(float amount, uint32_t easing) {
    amount = std::max(0.0f, std::min(1.0f, amount));
    switch (easing) {
        case WOTBMOD_V3_EASING_IN_QUAD:
            return amount * amount;
        case WOTBMOD_V3_EASING_OUT_QUAD:
            return amount * (2.0f - amount);
        case WOTBMOD_V3_EASING_IN_OUT_QUAD:
            return amount < 0.5f
                ? 2.0f * amount * amount
                : -1.0f +
                      (4.0f - 2.0f * amount) * amount;
        case WOTBMOD_V3_EASING_IN_CUBIC:
            return amount * amount * amount;
        case WOTBMOD_V3_EASING_OUT_CUBIC: {
            const float shifted = amount - 1.0f;
            return shifted * shifted * shifted + 1.0f;
        }
        case WOTBMOD_V3_EASING_IN_OUT_CUBIC:
            if (amount < 0.5f) {
                return 4.0f * amount * amount * amount;
            } else {
                const float shifted = 2.0f * amount - 2.0f;
                return 0.5f * shifted * shifted * shifted + 1.0f;
            }
        case WOTBMOD_V3_EASING_LINEAR:
        default:
            return amount;
    }
}

void BlendState(
    const WotbModV3CameraState& start,
    const WotbModV3CameraState& target,
    float amount,
    WotbModV3CameraState* output) {
    output->transform.position = Lerp(
        start.transform.position,
        target.transform.position,
        amount);
    output->transform.rotation = Slerp(
        start.transform.rotation,
        target.transform.rotation,
        amount);
    output->transform.scale = Lerp(
        start.transform.scale,
        target.transform.scale,
        amount);
    output->fov_degrees =
        start.fov_degrees +
        (target.fov_degrees - start.fov_degrees) * amount;
    output->near_plane =
        start.near_plane +
        (target.near_plane - start.near_plane) * amount;
    output->far_plane =
        start.far_plane +
        (target.far_plane - start.far_plane) * amount;
    /*
     * The exact build has no verified native source/setter for mode. Keep the
     * observed UNKNOWN mode rather than manufacturing one from a target.
     */
}

bool ValidTransition(
    const WotbModV3CameraTransition& transition,
    WotbModV3Result* out_error) {
    if (transition.struct_size < sizeof(transition) ||
        transition.api_version != WOTBMOD_V3_CAMERA_VERSION ||
        transition.target.struct_size <
            sizeof(transition.target) ||
        transition.target.api_version !=
            WOTBMOD_V3_CAMERA_VERSION ||
        !ValidTransform(transition.target.transform) ||
        !Finite(transition.target.fov_degrees) ||
        transition.target.fov_degrees < 1.0f ||
        transition.target.fov_degrees > kMaxNativeFov ||
        !Finite(transition.target.near_plane) ||
        transition.target.near_plane < 0.0f ||
        !Finite(transition.target.far_plane) ||
        transition.target.far_plane <=
            transition.target.near_plane ||
        !Finite(transition.duration_seconds) ||
        transition.duration_seconds < 0.0f ||
        transition.duration_seconds > kMaxTransitionSeconds ||
        transition.easing >
            WOTBMOD_V3_EASING_IN_OUT_CUBIC ||
        transition.preserve_game_control > 1u) {
        if (out_error) {
            *out_error = WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return false;
    }
    if (transition.target.mode !=
        WOTBMOD_V3_CAMERA_MODE_UNKNOWN) {
        if (out_error) {
            *out_error = WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        return false;
    }
    return true;
}

bool ValidShake(const WotbModV3CameraShake& shake) {
    return shake.struct_size >= sizeof(shake) &&
           shake.api_version == WOTBMOD_V3_CAMERA_VERSION &&
           Finite(shake.amplitude) && shake.amplitude >= 0.0f &&
           shake.amplitude <= kMaxShakeAmplitude &&
           Finite(shake.frequency) && shake.frequency >= 0.0f &&
           shake.frequency <= kMaxShakeFrequency &&
           Finite(shake.duration_seconds) &&
           shake.duration_seconds >= 0.0f &&
           shake.duration_seconds <= kMaxShakeSeconds &&
           Finite(shake.falloff) && shake.falloff >= 0.0f &&
           shake.falloff <= kMaxShakeFalloff;
}

}  // namespace

WotbModV3Result CameraEffectStore::EnsureControllerLocked(
    WotbModV3Handle owner,
    uint64_t* out_controller) {
    if (!out_controller ||
        owner == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (const Controller& controller : controllers_) {
        if (controller.owner == owner) {
            *out_controller = controller.token;
            return WOTBMOD_V3_OK;
        }
    }
    if (controllers_.size() >= kMaxControllers) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    ++next_controller_;
    uint64_t token =
        UINT64_C(0xCA00000000000000) |
        (next_controller_ & UINT64_C(0x00FFFFFFFFFFFFFF));
    if (token == 0u) {
        token = UINT64_C(0xCA00000000000001);
    }
    controllers_.push_back({owner, token});
    *out_controller = token;
    return WOTBMOD_V3_OK;
}

WotbModV3Result CameraEffectStore::QueueTransition(
    WotbModV3Handle owner,
    uint64_t camera,
    const WotbModV3CameraTransition& transition,
    uint64_t* out_controller) {
    if (camera == 0u || !out_controller) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    WotbModV3Result validation = WOTBMOD_V3_OK;
    if (!ValidTransition(transition, &validation)) {
        return validation;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool replacing = std::any_of(
        transitions_.begin(),
        transitions_.end(),
        [owner, camera](const Transition& current) {
            return current.owner == owner &&
                   current.camera == camera;
        });
    if (!replacing &&
        transitions_.size() >= kMaxTransitions) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    WotbModV3Result result =
        EnsureControllerLocked(owner, out_controller);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    transitions_.erase(
        std::remove_if(
            transitions_.begin(),
            transitions_.end(),
            [owner, camera](const Transition& current) {
                return current.owner == owner &&
                       current.camera == camera;
            }),
        transitions_.end());
    Transition queued = {};
    queued.owner = owner;
    queued.camera = camera;
    queued.sequence = ++next_sequence_;
    queued.descriptor = transition;
    transitions_.push_back(queued);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CameraEffectStore::QueueShake(
    WotbModV3Handle owner,
    uint64_t camera,
    const WotbModV3CameraShake& shake,
    uint64_t* out_controller) {
    if (camera == 0u || !out_controller ||
        !ValidShake(shake)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const bool no_op =
        shake.amplitude == 0.0f ||
        shake.frequency == 0.0f ||
        shake.duration_seconds == 0.0f;
    const size_t owner_count = static_cast<size_t>(std::count_if(
        shakes_.begin(),
        shakes_.end(),
        [owner](const Shake& current) {
            return current.owner == owner;
        }));
    if (!no_op &&
        (owner_count >= kMaxShakesPerOwner ||
         shakes_.size() >= kMaxShakes)) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    WotbModV3Result result =
        EnsureControllerLocked(owner, out_controller);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (no_op) {
        return WOTBMOD_V3_OK;
    }
    Shake queued = {};
    queued.owner = owner;
    queued.camera = camera;
    queued.sequence = ++next_sequence_;
    queued.descriptor = shake;
    shakes_.push_back(queued);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CameraEffectStore::Cancel(
    WotbModV3Handle owner,
    uint64_t controller) {
    if (owner == WOTBMOD_V3_INVALID_HANDLE ||
        controller == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(
        controllers_.begin(),
        controllers_.end(),
        [owner, controller](const Controller& current) {
            return current.owner == owner &&
                   current.token == controller;
        });
    if (found == controllers_.end()) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    controllers_.erase(found);
    transitions_.erase(
        std::remove_if(
            transitions_.begin(),
            transitions_.end(),
            [owner](const Transition& current) {
                return current.owner == owner;
            }),
        transitions_.end());
    shakes_.erase(
        std::remove_if(
            shakes_.begin(),
            shakes_.end(),
            [owner](const Shake& current) {
                return current.owner == owner;
            }),
        shakes_.end());
    return WOTBMOD_V3_OK;
}

void CameraEffectStore::Apply(
    uint64_t camera,
    double delta_seconds,
    WotbModV3CameraState* inout_state) {
    if (camera == 0u || !inout_state ||
        inout_state->struct_size < sizeof(*inout_state) ||
        inout_state->api_version !=
            WOTBMOD_V3_CAMERA_VERSION ||
        !std::isfinite(delta_seconds) ||
        delta_seconds < 0.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t index = 0u; index < transitions_.size();) {
        Transition& effect = transitions_[index];
        if (effect.camera != camera) {
            transitions_.erase(transitions_.begin() + index);
            continue;
        }
        if (!effect.started) {
            effect.start = *inout_state;
            effect.started = true;
        } else {
            effect.elapsed_seconds = std::min(
                static_cast<double>(
                    effect.descriptor.duration_seconds),
                effect.elapsed_seconds + delta_seconds);
        }
        const float amount =
            effect.descriptor.duration_seconds <= 0.0f
                ? 1.0f
                : static_cast<float>(
                      effect.elapsed_seconds /
                      effect.descriptor.duration_seconds);
        const WotbModV3CameraState source =
            effect.descriptor.preserve_game_control != 0u
                ? *inout_state
                : effect.start;
        BlendState(
            source,
            effect.descriptor.target,
            Ease(amount, effect.descriptor.easing),
            inout_state);
        if (amount >= 1.0f) {
            transitions_.erase(transitions_.begin() + index);
        } else {
            ++index;
        }
    }

    WotbModV3Vec3 local_offset = {};
    for (size_t index = 0u; index < shakes_.size();) {
        Shake& effect = shakes_[index];
        if (effect.camera != camera) {
            shakes_.erase(shakes_.begin() + index);
            continue;
        }
        if (!effect.started) {
            effect.started = true;
        } else {
            effect.elapsed_seconds = std::min(
                static_cast<double>(
                    effect.descriptor.duration_seconds),
                effect.elapsed_seconds + delta_seconds);
        }
        const double progress =
            effect.descriptor.duration_seconds <= 0.0f
                ? 1.0
                : effect.elapsed_seconds /
                      effect.descriptor.duration_seconds;
        const double remaining =
            std::max(0.0, 1.0 - progress);
        const double envelope = progress >= 1.0
            ? 0.0
            : std::pow(
                  remaining,
                  static_cast<double>(
                      effect.descriptor.falloff));
        const double phase =
            std::fmod(
                static_cast<double>(effect.sequence) *
                    0.7548776662466927,
                1.0) *
            2.0 * kPi;
        const double angular =
            2.0 * kPi * effect.descriptor.frequency;
        const double amplitude =
            effect.descriptor.amplitude * envelope *
            0.5773502691896258;
        local_offset.x += static_cast<float>(
            std::sin(
                angular * effect.elapsed_seconds + phase) *
            amplitude);
        local_offset.y += static_cast<float>(
            std::sin(
                angular * 1.37013 * effect.elapsed_seconds +
                phase + 2.0943951023931953) *
            amplitude);
        local_offset.z += static_cast<float>(
            std::sin(
                angular * 0.73111 * effect.elapsed_seconds +
                phase + 4.1887902047863905) *
            amplitude);
        if (progress >= 1.0) {
            shakes_.erase(shakes_.begin() + index);
        } else {
            ++index;
        }
    }
    const double offset_length_squared =
        static_cast<double>(local_offset.x) * local_offset.x +
        static_cast<double>(local_offset.y) * local_offset.y +
        static_cast<double>(local_offset.z) * local_offset.z;
    if (offset_length_squared > 0.0) {
        const double offset_length =
            std::sqrt(offset_length_squared);
        if (offset_length > kMaxCombinedShakeAmplitude) {
            const float scale = static_cast<float>(
                kMaxCombinedShakeAmplitude / offset_length);
            local_offset.x *= scale;
            local_offset.y *= scale;
            local_offset.z *= scale;
        }
        const WotbModV3Vec3 world_offset = Rotate(
            inout_state->transform.rotation,
            local_offset);
        inout_state->transform.position.x += world_offset.x;
        inout_state->transform.position.y += world_offset.y;
        inout_state->transform.position.z += world_offset.z;
    }
}

void CameraEffectStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    controllers_.clear();
    transitions_.clear();
    shakes_.clear();
    next_controller_ = 0u;
    next_sequence_ = 0u;
}

size_t CameraEffectStore::ActiveTransitionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transitions_.size();
}

size_t CameraEffectStore::ActiveShakeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return shakes_.size();
}

size_t CameraEffectStore::ControllerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return controllers_.size();
}

}  // namespace loader
}  // namespace wotbmod
