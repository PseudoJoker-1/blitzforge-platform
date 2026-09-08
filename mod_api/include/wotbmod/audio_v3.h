#pragma once

#include "base.h"

/*
 * wotbmod.audio.intercept -- semantic interception of engine sound events.
 * Declared 2026-08-16 as part of the release contract that follows RC1.
 * Backed by re_anchors.md, section "SoundEventStub", cross-checked against
 * 11.19.0.834 / 41960DBD...E0AD.
 *
 * The proven contract
 * -------------------
 * The ingress is vtable slot +0x0C of the sound system (CreateSoundEvent).
 * Its name argument is a DAVA::FastName -- ONE dword holding an interned
 * const char* (ctor 0x0090DE50 stores the interned pointer at [this]). The
 * detour therefore matches with strcmp(*(const char**)arg1, wanted): no
 * allocation, no lock, no FastName construction on the hot path.
 *
 * Exactly three outcomes are expressible, and nothing else is
 * -----------------------------------------------------------
 * PASS_THROUGH, SUBSTITUTE (call the original with a different name) and
 * SUPPRESS. There is no fourth.
 *
 * SUPPRESS is the one that has to be got right. Six of the eight real call
 * sites do not null-check the result of CreateSoundEvent -- 0x00786381,
 * 0x00787216, 0x015F84A4, 0x015F8577 and 0x01572348 write it straight into a
 * field. Returning nullptr from the detour is therefore a crash, not a
 * suppression. Suppression must return the engine's OWN null object: the
 * SoundEventStub that base DAVA::SoundSystem::CreateSoundEvent (0x00C4F040)
 * builds via sub_C474F0, vtable 0x0360C7EC, whose 22 slots match
 * WwiseHybridSoundEvent's layout with every setter pointing at the nullsub.
 *
 * The ABI makes that mistake impossible to make from a mod: a callback
 * returns a DECISION and, at most, a NAME. No SoundEvent pointer, handle or
 * anything derived from one ever crosses this boundary in either direction,
 * so there is no expression for "return null" and nothing for a mod to
 * lifetime-manage. Choosing the stub is the host's job and the only thing the
 * host can do for SUPPRESS.
 *
 * Threading and re-entry
 * ----------------------
 * The creation path is guarded by a mutex inside sub_24F39C0 (the critical
 * section at 0x24F3A74), which is the engine itself declaring that concurrent
 * calls are expected. A detour must therefore be written as runnable on an
 * ARBITRARY thread; `thread_role` on the request says which one it landed on.
 * The lock is taken inside that helper rather than around CreateSoundEvent, so
 * the host may call the original from within the detour without self-deadlock.
 *
 * The system chain is three levels deep -- SoundSystemProxy -> (+0x20)
 * WwiseHybridSoundSystem -> (+0x5C) WwiseSoundSystem -- and the hybrid calls
 * the inner system's CreateSoundEvent at 0x024F3BA2, so the public path
 * re-enters the detour. The guard for that is thread-local, and this ABI is
 * shaped so a thread-local bool is the whole implementation: the callback
 * cannot create a sound through this interface, so the only re-entry left is
 * the engine's own, and `is_intercept_active` publishes the same flag so the
 * other audio surfaces can fail closed with WOTBMOD_V3_E_BUSY instead of
 * recursing.
 *
 * Every slot returns WOTBMOD_V3_E_NOT_SUPPORTED until the backend lands.
 * Permission: reuses the existing SAFE `audio.events`, the same grant that
 * already covers WotbModV3AudioApiV2::sound_override_register.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_AUDIO_VERSION_3 3u

typedef enum WotbModV3SoundInterceptDecision {
    /* Call the original with the original name. Also the fail-open default. */
    WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH = 0,
    /*
     * Call the original with `substitute_event_name`. An empty or unresolvable
     * name is NOT silently downgraded to suppression: the host falls back to
     * PASS_THROUGH, because dropping a sound the mod did not ask to drop is a
     * behaviour change the mod never requested.
     */
    WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE = 1,
    /*
     * Do not create the requested event. The host returns the engine's own
     * SoundEventStub. It never returns nullptr.
     */
    WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS = 2
} WotbModV3SoundInterceptDecision;

/*
 * Pure copied POD. `event_name` is a bounded copy of the interned string the
 * FastName pointed at; the interned pointer itself never crosses the ABI.
 */
typedef struct WotbModV3SoundInterceptRequest {
    uint32_t struct_size;
    uint32_t api_version;
    /* WotbModV3ThreadRole of the engine thread the detour landed on. */
    uint32_t thread_role;
    /* WotbModV3GameContext mask, best effort. 0 when it cannot be sampled. */
    uint32_t game_context;
    char event_name[WOTBMOD_V3_MAX_NAME];
} WotbModV3SoundInterceptRequest;

/*
 * The callback fills this in place. The host initialises `decision` to
 * PASS_THROUGH before the call, so a callback that writes nothing, faults, or
 * writes a value outside the three above leaves the game's own behaviour
 * untouched.
 */
typedef struct WotbModV3SoundInterceptResponse {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t decision;
    uint32_t reserved;
    char substitute_event_name[WOTBMOD_V3_MAX_NAME];
} WotbModV3SoundInterceptResponse;

/*
 * Runs on an arbitrary thread, inside the engine's event-creation path, with
 * the mod's own re-entry flag already set. It must not block, must not call
 * back into the runtime, and must not assume a frame boundary.
 */
typedef void(WOTBMOD_V3_CALL* WotbModV3SoundInterceptCallback)(
    WotbModV3Handle mod,
    const WotbModV3SoundInterceptRequest* request,
    WotbModV3SoundInterceptResponse* response,
    void* user_data);

typedef struct WotbModV3AudioApiV3 {
    uint32_t struct_size;
    uint32_t api_version;

    /*
     * `event_name` is an EXACT match and is required: NULL or empty returns
     * WOTBMOD_V3_E_INVALID_ARGUMENT. Prefix, glob and match-everything filters
     * are deliberately not expressible. Exact match is the only form the proof
     * covers (one strcmp against an interned pointer), and it is the only form
     * whose cost on an arbitrary thread inside the creation path is bounded by
     * something the host can reason about.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* intercept_register)(
        WotbModV3Handle mod,
        const char* event_name,
        int32_t priority,
        WotbModV3SoundInterceptCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* intercept_unregister)(
        WotbModV3Handle mod,
        WotbModV3Token token);
    WotbModV3Result(WOTBMOD_V3_CALL* intercept_set_priority)(
        WotbModV3Handle mod,
        WotbModV3Token token,
        int32_t priority);
    /*
     * The thread-local re-entry flag, read as a value. Non-zero means this
     * thread is currently inside an interception callback; every other audio
     * operation returns WOTBMOD_V3_E_BUSY while it is set.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* is_intercept_active)(
        WotbModV3Handle mod,
        uint32_t* out_active);
} WotbModV3AudioApiV3;

#ifdef __cplusplus
}
#endif
