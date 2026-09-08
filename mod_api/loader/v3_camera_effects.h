#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../include/wotbmod/camera_v1.h"

namespace wotbmod {
namespace loader {

/*
 * Loader-owned camera effects run after the stock camera has produced a
 * readable state and before public AFTER_GAME modifiers. They never touch
 * gameplay/server state and are deliberately bounded so a reviewed mod cannot
 * grow an unbounded per-frame workload. A controller token remains stable for
 * its owner until explicit cancellation, so delayed owner tracking can never
 * refer to an expired controller while newer effects remain active.
 */
class CameraEffectStore final {
public:
    WotbModV3Result QueueTransition(
        WotbModV3Handle owner,
        uint64_t camera,
        const WotbModV3CameraTransition& transition,
        uint64_t* out_controller);

    WotbModV3Result QueueShake(
        WotbModV3Handle owner,
        uint64_t camera,
        const WotbModV3CameraShake& shake,
        uint64_t* out_controller);

    WotbModV3Result Cancel(
        WotbModV3Handle owner,
        uint64_t controller);

    void Apply(
        uint64_t camera,
        double delta_seconds,
        WotbModV3CameraState* inout_state);

    void Clear();

    size_t ActiveTransitionCount() const;
    size_t ActiveShakeCount() const;
    size_t ControllerCount() const;

private:
    struct Controller {
        WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
        uint64_t token = 0u;
    };

    struct Transition {
        WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
        uint64_t camera = 0u;
        uint64_t sequence = 0u;
        WotbModV3CameraTransition descriptor = {};
        WotbModV3CameraState start = {};
        double elapsed_seconds = 0.0;
        bool started = false;
    };

    struct Shake {
        WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
        uint64_t camera = 0u;
        uint64_t sequence = 0u;
        WotbModV3CameraShake descriptor = {};
        double elapsed_seconds = 0.0;
        bool started = false;
    };

    WotbModV3Result EnsureControllerLocked(
        WotbModV3Handle owner,
        uint64_t* out_controller);

    mutable std::mutex mutex_;
    std::vector<Controller> controllers_;
    std::vector<Transition> transitions_;
    std::vector<Shake> shakes_;
    uint64_t next_controller_ = 0u;
    uint64_t next_sequence_ = 0u;
};

}  // namespace loader
}  // namespace wotbmod
