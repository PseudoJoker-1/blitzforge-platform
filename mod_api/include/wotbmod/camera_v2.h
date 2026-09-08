#pragma once

#include "base.h"

/*
 * wotbmod.camera.state -- READ ONLY observation of two independent camera
 * values. Declared 2026-08-16 as part of the release contract that follows
 * RC1. Backed by re_anchors.md, section "Camera state machine
 * CameraController+0x5C" and the 2026-08-15 correction to 0x0156ED10, both
 * cross-checked against 11.19.0.834 / 41960DBD...E0AD.
 *
 * The two values are NOT the same thing and must never be merged
 * ------------------------------------------------------------
 * 1. CameraController+0x5C is an ANIMATION-CONTROLLER state machine, not a
 *    camera mode. Seven states, written only by CameraController::SwitchState
 *    (0x015A9FA0) and the constructor (initial value 5). Four of the states
 *    own a controller object whose class name is RTTI-proven; two do not and
 *    are deliberately left unnamed; one is the empty initial state.
 * 2. The arcade/sniper bit is a DIFFERENT field on a DIFFERENT object:
 *    GameCamera+0x320, a 2-valued index into the 16-byte per-mode parameter
 *    blocks at +0x360/+0x370 and +0x364/+0x374.
 *
 * Why there is no setter here, and why that is the point
 * -----------------------------------------------------
 * SwitchState itself validates nothing -- it accepts any integer. The engine's
 * transition policy ("if state is 3, 4, 5 or 6, refuse") is duplicated at the
 * EIGHT call sites, outside SwitchState. A direct call therefore bypasses the
 * only thing enforcing the policy and can, for example, tear the camera out of
 * PostMortemAnimationController while that controller still owns the death
 * animation. There is no safe way to express "set the state" against this
 * binary, so this table contains no way to express it at all. Camera writes
 * stay where they already are: the transform/FOV surface of wotbmod.camera and
 * wotbmod.gameplay.camera, which go through paths that were proven safe.
 *
 * Every slot returns WOTBMOD_V3_E_NOT_SUPPORTED until the backend lands.
 * Permission: reuses the existing REVIEWED `camera.battle.read`.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_CAMERA_VERSION_2 2u

/*
 * States 0 and 1 are the two ordinary states, the ones with no animation
 * controller object. The static evidence for what distinguishes them is
 * CONTRADICTORY, so they are exposed as two distinct, honestly-named unknowns.
 * They are NOT arcade and sniper -- that bit is WotbModV3CameraViewMode below.
 * Do not rename these to arcade/sniper without new live evidence; the whole
 * reason they carry these names is that guessing here produced a wrong public
 * mapping once already.
 */
typedef enum WotbModV3CameraAnimationState {
    WOTBMOD_V3_CAMERA_ANIMATION_ORDINARY_A = 0,
    WOTBMOD_V3_CAMERA_ANIMATION_ORDINARY_B = 1,
    /* +0x68, LookOutAnimationController, vtable 0x03692D1C. */
    WOTBMOD_V3_CAMERA_ANIMATION_LOOK_OUT = 2,
    /* +0x6C, PostMortemAnimationController, vtable 0x03692D94. */
    WOTBMOD_V3_CAMERA_ANIMATION_POST_MORTEM = 3,
    /* +0x74, LookOnTargetAnimationController, vtable 0x03692CF0. */
    WOTBMOD_V3_CAMERA_ANIMATION_LOOK_ON_TARGET = 4,
    /* Initial/empty state written by the constructor. No controller object. */
    WOTBMOD_V3_CAMERA_ANIMATION_NONE = 5,
    /* +0x70, ObserverAnimationController, vtable 0x03692D68. */
    WOTBMOD_V3_CAMERA_ANIMATION_OBSERVER = 6,
    /*
     * Reported when the field holds a value the frozen contract does not name.
     * Consumers treat this fail-closed. FreeCamera is NOT part of this machine
     * at all: it is a GameCamera subclass owned by another object and is never
     * reached through SwitchState.
     */
    WOTBMOD_V3_CAMERA_ANIMATION_UNKNOWN = 255
} WotbModV3CameraAnimationState;

/*
 * GameCamera+0x320. CORRECTED 2026-08-15: 0 is SNIPER, non-zero is ARCADE.
 * Value 0 selects GameCamera::GetPivotForSniperMode (0x015B1870, identified by
 * its own __FUNCTION__ literal) at the branch 0x015C460B, and
 * CameraController::OnCameraModeChanged (0x015B5200) maps field0 == 0 to the
 * engine UI state "STATE_view_play_mode_sniper" and raises hint 0x66 SNIPER_ON.
 * Documentation published before that date had this INVERTED. Any backend or
 * binding that still ships "0 = ARCADE" is wrong.
 */
typedef enum WotbModV3CameraViewMode {
    WOTBMOD_V3_CAMERA_VIEW_SNIPER = 0,
    WOTBMOD_V3_CAMERA_VIEW_ARCADE = 1,
    WOTBMOD_V3_CAMERA_VIEW_UNKNOWN = 255
} WotbModV3CameraViewMode;

/*
 * The two values live on two different objects that are not guaranteed to be
 * resolvable at the same moment, so each carries its own validity flag. A
 * backend that can read one and not the other reports exactly that; it never
 * invents the missing half to fill the struct.
 */
typedef struct WotbModV3CameraObservedState {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t animation_state;
    uint32_t animation_state_valid;
    uint32_t view_mode;
    uint32_t view_mode_valid;
} WotbModV3CameraObservedState;

typedef struct WotbModV3CameraApiV2 {
    uint32_t struct_size;
    uint32_t api_version;

    /* CameraController+0x5C. WotbModV3CameraAnimationState. */
    WotbModV3Result(WOTBMOD_V3_CALL* get_animation_state)(
        WotbModV3Handle mod,
        uint32_t* out_state);
    /* GameCamera+0x320. WotbModV3CameraViewMode, 0 = SNIPER. */
    WotbModV3Result(WOTBMOD_V3_CALL* get_view_mode)(
        WotbModV3Handle mod,
        uint32_t* out_mode);
    /*
     * Both values in one call. Succeeds when AT LEAST ONE half is valid; the
     * per-half flags say which. Returns WOTBMOD_V3_E_NOT_SUPPORTED when
     * neither can be read.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* get_observed_state)(
        WotbModV3Handle mod,
        WotbModV3CameraObservedState* out_state);
} WotbModV3CameraApiV2;

#ifdef __cplusplus
}
#endif
