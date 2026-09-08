#pragma once

#include "../../include/wotbmod/audio_v2.h"
#include "../../include/wotbmod/audio_v3.h"
#include "../../include/wotbmod/base.h"
#include "../../include/wotbmod/bigworld_rpc_v1.h"
#include "../../include/wotbmod/camera_v1.h"
#include "../../include/wotbmod/camera_v2.h"
#include "../../include/wotbmod/entity_public_v1.h"
#include "../../include/wotbmod/projectile_v2.h"
#include "../../include/wotbmod/render_v1.h"
#include "../../include/wotbmod/scene_v1.h"
#include "../../include/wotbmod/scene_v2.h"
#include "../../include/wotbmod/tracer_v1.h"
#include "../../include/wotbmod/ui_v4.h"
#include "../../include/wotbmod/session_cluster_v1.h"

namespace wotbmod {
namespace v3 {

#define WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION 1u

enum ClientHostAudioLifecycleKind : uint32_t {
    CLIENT_HOST_AUDIO_STARTED = 1u,
    CLIENT_HOST_AUDIO_FINISHED = 2u,
    CLIENT_HOST_AUDIO_ERROR = 3u
};

typedef WotbModV3Result (*ClientHostInvokeFn)(
    void* user_data,
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size);

struct ClientHostBackend {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t binding_pack_version;
    uint32_t compatibility_state;
    void* user_data;
    ClientHostInvokeFn invoke;
};

struct ClientHostObjectRequest {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t object;
    uint64_t related_object;
    uint64_t auxiliary_object;
    uint32_t selector;
    uint32_t flags;
    int32_t signed_value;
    uint32_t reserved;
    double scalar0;
    double scalar1;
    WotbModV3Vec4 vector;
    WotbModV3Transform transform;
    const char* name;
    const char* secondary_name;
    const void* payload;
    uint32_t payload_size;
};

struct ClientHostObjectResponse {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t object;
    uint64_t related_object;
    uint32_t value_u32;
    int32_t value_i32;
    double value_f64;
    WotbModV3Vec4 vector;
    WotbModV3Transform transform;
    WotbModV3Rect rect;
    /* 2026-09-05: for UI objects, the DAVA control name and RTTI class the
     * loader read while wrapping; empty when the backend has no identity. */
    char name[64];
    char class_name[64];
};

struct ClientHostSceneNameVisitorRequest {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t object;
    WotbModV3Handle mod;
    WotbModV3SceneNameVisitor visitor;
    void* user_data;
};

struct ClientHostCameraModifierRequest {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mod;
    uint32_t phase;
    int32_t priority;
};

struct ClientHostAudioConflictVisitorRequest {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mod;
    const char* event_name;
    WotbModV3SoundConflictVisitor visitor;
    void* user_data;
};

struct ClientHostAudioSubscriptionRequest {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t object;
    WotbModV3Handle mod;
    uint32_t lifecycle_kind;
    uint32_t reserved;
};

struct ClientHostEntityVisitorRequest {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mod;
    WotbModV3PublicEntityVisitor visitor;
    void* user_data;
};

struct ClientHostRpcObserverRequest {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mod;
    uint32_t direction_mask;
    const char* method_filter;
};

struct ClientHostFrame {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t render_backend;
    uint32_t reserved;
    uint64_t frame_index;
    double delta_seconds;
    WotbModV3Rect viewport;
    void* native_device;
    void* native_context;
    void* native_swapchain;
};

enum ClientHostPublicEntityField : uint64_t {
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_ID =
        UINT64_C(1) << 0,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_TYPE =
        UINT64_C(1) << 1,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_VISIBILITY =
        UINT64_C(1) << 2,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_LOCAL =
        UINT64_C(1) << 3,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_TEAM =
        UINT64_C(1) << 4,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_HEALTH =
        UINT64_C(1) << 5,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_MAX_HEALTH =
        UINT64_C(1) << 6,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_POSITION =
        UINT64_C(1) << 7,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_DIRECTION =
        UINT64_C(1) << 8,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE =
        UINT64_C(1) << 9,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_DISPLAY_NAME =
        UINT64_C(1) << 10,
    /* Player extras (2026-09-06): not part of the public snapshot, reachable
     * through get_public_property as clan_tag / account_id / kills /
     * vehicle_name. */
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_CLAN_TAG =
        UINT64_C(1) << 11,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_ACCOUNT_ID =
        UINT64_C(1) << 12,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_KILLS =
        UINT64_C(1) << 13,
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_NAME =
        UINT64_C(1) << 14,
    /* localized vehicle name in the client's current language */
    CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME =
        UINT64_C(1) << 15,
    CLIENT_HOST_PUBLIC_ENTITY_FIELDS_ALL =
        (UINT64_C(1) << 16) - UINT64_C(1)
};

/*
 * Per-player extras the host sources from the arena roster. They live next
 * to the frozen public snapshot rather than inside it, so the ABI of
 * WotbModV3PublicEntitySnapshot stays untouched; mods read them by name.
 */
struct ClientHostPublicEntityExtras {
    int64_t account_id;
    int32_t kills;
    uint32_t reserved;
    char clan_tag[32];
    char vehicle_name[WOTBMOD_V3_MAX_NAME];
    char vehicle_display_name[WOTBMOD_V3_MAX_NAME];
};

struct ClientHostPublicEntity {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t native_token;
    /*
     * Private host-to-runtime provenance. The public snapshot deliberately has
     * no validity mask: fields absent from this mask are sanitized to zero so
     * snapshot/enumeration callers never observe stale native memory.
     */
    uint64_t valid_fields;
    WotbModV3PublicEntitySnapshot snapshot;
    ClientHostPublicEntityExtras extras;
};

struct ClientHostProjectile {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t native_token;
    uint64_t visual_scene_object;
    uint32_t owner_scope;
    uint32_t shell_type;
    WotbModV3Vec3 visible_position;
    WotbModV3Vec3 visible_direction;
    uint64_t sequence_id;
    uint64_t valid_fields;
    uint64_t timestamp_microseconds;
    uint32_t lifecycle_state;
    uint32_t source;
    uint32_t native_shot_id;
    uint32_t primary_entity_id;
    uint32_t secondary_entity_id;
    uint32_t native_flags;
    uint32_t stock_shot_code;
    WotbModV3Vec3 origin;
    WotbModV3Vec3 impact_position;
};

/*
 * Loader ingress contract: callers must never pass an UNKNOWN projectile
 * scope or a non-local entity that the stock client has not confirmed as
 * visible. The implementation validates the same invariant again.
 */

void SetClientHostBackend(const ClientHostBackend* backend);
void SetClientHostUiViewportSize(uint32_t width, uint32_t height);
void RefreshClientHostUiCaptureSnapshot();
void PumpClientHostFrame(const ClientHostFrame* frame);
/* 2026-09-05: stock HUD extensions, every frame on the DAVA main thread. */
void HudMainThreadTick();
WotbModV3Result NotifyClientHostUiInput(
    uint32_t phase,
    float screen_x,
    float screen_y,
    float delta_x,
    float delta_y,
    uint32_t modifiers);
bool ShouldCaptureClientHostUiInput(float screen_x, float screen_y);
WotbModV3Result ApplyClientHostCameraModifiers(
    uint32_t phase,
    WotbModV3CameraState* inout_state);
WotbModV3Result NotifyClientHostAudioLifecycle(
    uint64_t native_audio_object,
    uint32_t lifecycle_kind,
    uint32_t error_code,
    const char* error_message);
WotbModV3Result NotifyClientHostObservedRpc(
    const WotbModV3ObservedRpc* rpc);
WotbModV3Result PublishClientHostEvent(
    const char* event,
    const void* payload,
    uint32_t payload_size);
WotbModV3Result RegisterClientHostPublicEntity(
    const ClientHostPublicEntity* entity,
    WotbModV3EntityHandle* out_handle);
WotbModV3Result UpdateClientHostPublicEntity(
    WotbModV3EntityHandle handle,
    const ClientHostPublicEntity* entity);
WotbModV3Result RemoveClientHostPublicEntity(
    WotbModV3EntityHandle handle);
WotbModV3Result RegisterClientHostProjectile(
    const ClientHostProjectile* projectile,
    WotbModV3ProjectileHandle* out_handle);
WotbModV3Result UpdateClientHostProjectile(
    WotbModV3ProjectileHandle handle,
    const ClientHostProjectile* projectile);
WotbModV3Result RemoveClientHostProjectile(
    WotbModV3ProjectileHandle handle,
    uint32_t reason = WOTBMOD_V3_PROJECTILE_DESTROY_NATIVE);
WotbModV3Result PublishClientHostProjectileLifecycle(
    WotbModV3ProjectileHandle handle,
    uint32_t previous_state,
    uint32_t reason);

/* =========================================================================
 * DECLARED BACKEND -- the native half of the five interfaces declared
 * 2026-08-16: wotbmod.ui.read, wotbmod.camera.state,
 * wotbmod.audio.intercept, wotbmod.scene.enumerate, wotbmod.tracer.
 *
 * Why this is a second, explicit table and not another `operation` string on
 * ClientHostBackend::invoke
 * -------------------------------------------------------------------------
 * ClientHostBackend is a one-slot dispatcher: everything is marshalled
 * through ClientHostObjectRequest / ClientHostObjectResponse, two structs
 * that were shaped for "an object plus a scalar plus a name". None of the
 * five interfaces below fits that shape. A scene walk returns an array, a
 * style read returns a 40-byte snapshot with its own validity mask, an
 * interception is a callback ingress rather than a call, and a camera read
 * returns two independently-valid halves. Squeezing those through the
 * generic pair would mean casting `payload` to five different private types
 * and validating the cast by convention -- which is exactly the class of
 * mistake this codebase spends its error handling avoiding. Explicit
 * function pointers with explicit argument types let the compiler check the
 * contract, and let a group be absent (null) without inventing a sentinel.
 *
 * Rules that hold for EVERY function in this table
 * -------------------------------------------------------------------------
 * 1. OWNERSHIP. Every pointer the runtime passes in is borrowed for the
 *    duration of the call and is never retained. Every buffer written to is
 *    runtime-owned storage sized by the runtime. No mod-supplied pointer is
 *    ever forwarded to this table: the runtime copies the mod's request in
 *    and copies the answer back out, so a backend never sees mod memory and
 *    a mod never sees engine memory. The runtime keeps no pointer the
 *    backend returns.
 * 2. NO RAW ENGINE POINTERS OUT. Nothing in these structs is an address. The
 *    only opaque token that crosses is `ClientHostUiReadRequest::object`,
 *    which is the same UI token ClientHostBackend already issued to the
 *    runtime through "ui_control_create"; it travels back to its issuer and
 *    never to a mod.
 * 3. RESULT DISCIPLINE. Returning WOTBMOD_V3_OK means the operation was
 *    performed. A backend that cannot answer returns the most precise of
 *    WOTBMOD_V3_E_NOT_SUPPORTED (this build cannot do it at all),
 *    WOTBMOD_V3_E_NOT_FOUND (nothing to read right now),
 *    WOTBMOD_V3_E_WRONG_THREAD, WOTBMOD_V3_E_INVALID_ARGUMENT or
 *    WOTBMOD_V3_E_CLIENT_MISMATCH. It must NEVER return OK with a struct it
 *    did not fill: the runtime re-validates and will answer the mod
 *    WOTBMOD_V3_E_PLATFORM, which reads to the user as a host bug.
 * 4. NO EXCEPTIONS AND NO LONGJMP ACROSS THIS BOUNDARY. The runtime calls
 *    these under /EHsc from threads that may be inside an __except-guarded
 *    region. A backend that can fault must catch its own faults (SEH) and
 *    return a result code.
 * 5. NO RE-ENTRY. A backend function must not call back into any wotbmod
 *    runtime entry point except DispatchClientHostSoundIntercept, and must
 *    not call SetClientHostDeclaredBackend from inside one of its own slots.
 * 6. FINGERPRINT GATE. The whole table is refused unless
 *    `compatibility_state` is WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED or
 *    _DEGRADED. A hash mismatch, missing bindings or UNKNOWN disables all
 *    five interfaces fail-closed; there is no per-slot override.
 * 7. GROUPS ARE ALL-OR-NOTHING. The five groups marked below are installed
 *    independently, but within a group every pointer must be non-null. A
 *    half-filled group is dropped whole and its interface stays UNAVAILABLE.
 * 8. THREAD. Unless a slot says otherwise it is called on the thread the mod
 *    called from, which is any thread, and never with a runtime lock held.
 * ========================================================================= */

#define WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION 1u

/* ---------------------------------------------------------------- ui.read */

enum ClientHostUiReadField : uint32_t {
    /* WotbModV3UiApiV4::control_get_text */
    CLIENT_HOST_UI_READ_FIELD_TEXT = 1u,
    /* WotbModV3UiApiV4::control_get_texture */
    CLIENT_HOST_UI_READ_FIELD_TEXTURE = 2u,
    /* WotbModV3UiApiV4::control_get_font */
    CLIENT_HOST_UI_READ_FIELD_FONT = 3u,
    /*
     * WotbModV3UiApiV4::control_get_live_text: the engine's own current
     * text (DAVA::UITextComponent), not the mirror. The _SET bit for it is
     * WOTBMOD_V3_UI_READ_LIVE_TEXT_SET; clear means "no text component".
     */
    CLIENT_HOST_UI_READ_FIELD_LIVE_TEXT = 4u
};

struct ClientHostUiReadRequest {
    uint32_t struct_size;
    uint32_t api_version;
    /*
     * The native UI token this backend's ClientHostBackend half issued from
     * "ui_control_create" / "ui_get_active_screen". Never zero: the runtime
     * refuses handles without one before calling.
     */
    uint64_t object;
    /* Owner mod, for the backend's own logging only. Never a lookup key. */
    WotbModV3Handle mod;
    /* ClientHostUiReadField. Exactly one field per call. */
    uint32_t field;
    /* 1 when the runtime knows the control is game-owned. Advisory. */
    uint32_t game_owned;
};

struct ClientHostUiReadString {
    uint32_t struct_size;
    uint32_t api_version;
    /*
     * WotbModV3UiReadFlag bits the backend can PROVE. Only the _SET bit for
     * the requested field is read here; every other bit is ignored on a
     * string read.
     *
     * That one bit decides the mod-visible outcome, so it is not optional:
     *   set   -> the runtime copies `value` out and answers OK, even when
     *            `value` is the empty string;
     *   clear -> the runtime answers WOTBMOD_V3_E_NOT_FOUND and copies
     *            nothing.
     * The string getters have no flags field of their own, so this is the
     * only way "never written" and "written to empty" stay distinguishable,
     * which is the whole point of the per-field provenance in ui_v4.h.
     */
    uint32_t flags;
    /* Bytes in `value` NOT counting the terminator. 0 for an empty string. */
    uint32_t length;
    /*
     * NUL-terminated. WOTBMOD_V3_MAX_PATH covers the largest writable field
     * (texture and font are MAX_PATH; text is MAX_MESSAGE). The runtime
     * re-checks the terminator and rejects a longer or unterminated value
     * with WOTBMOD_V3_E_PLATFORM rather than copying it.
     */
    char value[WOTBMOD_V3_MAX_PATH];
};

/*
 * Reads back the LOADER's mirror of what this API previously committed to a
 * control -- never the live DAVA UIStaticText string, which has no proven
 * read path on this fingerprint (see ui_v4.h).
 *
 * Source of truth split, and it is not negotiable:
 *   - the backend owns the mirrored VALUE and the per-field _SET flag,
 *     because the loader writes its mirror inside the same guarded native
 *     call that writes the engine control, so the two cannot disagree;
 *   - the runtime owns WOTBMOD_V3_UI_READ_GAME_OWNED and always ORs its own
 *     bit in, so a backend can never drop the staleness warning.
 *
 * Must return WOTBMOD_V3_E_NOT_FOUND for a token it does not know (a control
 * destroyed behind the runtime's back), and must set `length`/`value` to an
 * empty string with the _SET bit CLEAR -- not an error -- for a field this
 * API never wrote. "Never written" and "written to the empty string" are
 * different answers and the flag is the only thing that distinguishes them.
 * Group: ui_read_string + ui_read_style.
 */
typedef WotbModV3Result (*ClientHostUiReadStringFn)(
    void* user_data,
    const ClientHostUiReadRequest* request,
    ClientHostUiReadString* out_value);

/*
 * Fills WotbModV3UiStyleSnapshot. `request->field` is 0 and unused.
 * The backend sets struct_size/api_version, the four _SET flags it can
 * prove (COLOR, BACKGROUND_COLOR, OPACITY, FONT_SIZE, TEXT_LAYOUT) and the
 * matching values. Fields whose flag is clear must be left as the mirror's
 * construction defaults; they are documented as "not a measurement" and the
 * runtime does not zero them. The runtime rejects unknown flag bits and
 * non-finite floats with WOTBMOD_V3_E_PLATFORM.
 */
typedef WotbModV3Result (*ClientHostUiReadStyleFn)(
    void* user_data,
    const ClientHostUiReadRequest* request,
    WotbModV3UiStyleSnapshot* out_style);

/* ----------------------------------------------------------- camera.state */

/*
 * ONE call answers both halves, deliberately: three public slots
 * (get_animation_state, get_view_mode, get_observed_state) all run through
 * it, so a backend physically cannot report a different animation state to
 * one caller than to another, and cannot fabricate the half it could not
 * read to fill the struct.
 *
 * Fill exactly what was read:
 *   - animation_state = CameraController+0x5C mapped through
 *     WotbModV3CameraAnimationState, animation_state_valid = 1;
 *   - view_mode = GameCamera+0x320 mapped through WotbModV3CameraViewMode
 *     (0 = SNIPER -- the pre-2026-08-15 "0 = ARCADE" mapping is INVERTED and
 *     must not ship), view_mode_valid = 1.
 * A half that could not be resolved -- no CameraController captured yet, no
 * GameCamera at CameraController+0x28, hangar with no battle camera -- leaves
 * BOTH its value and its _valid flag at zero. Return WOTBMOD_V3_OK if either
 * half is valid, WOTBMOD_V3_E_NOT_FOUND if neither is (the runtime turns
 * that into the NOT_SUPPORTED the frozen header mandates).
 *
 * Any value the frozen enums do not name is reported as-is; the runtime
 * normalises it to WOTBMOD_V3_CAMERA_ANIMATION_UNKNOWN / _VIEW_UNKNOWN with
 * the _valid flag left set, because "we read it and cannot name it" is a
 * different fact from "we could not read it".
 *
 * Callable from any thread. It must therefore never dereference a
 * CameraController pointer it did not capture from a SwitchState detour and
 * publish atomically -- reading a stale this-pointer is the one way this
 * slot can crash the game. There is no setter and none may be added: the
 * engine's transition policy lives at the eight SwitchState call sites, not
 * inside SwitchState, so a direct write bypasses the only thing enforcing it.
 * Group: camera_read_observed_state (single member).
 */
typedef WotbModV3Result (*ClientHostCameraStateFn)(
    void* user_data,
    WotbModV3CameraObservedState* out_state);

/* ------------------------------------------------------- audio.intercept */

/*
 * Adds `event_name` to the detour's match set and arms the vtable-slot
 * detour if it is not armed yet -- in that order, never the other way round.
 *
 * THE PUBLICATION RULE, which is the whole reason this is one function and
 * not "install a set" plus "arm":
 *   The match set MUST be an append-only array of immortal, NUL-terminated
 *   copies, published to the detour by a single release store of the element
 *   count. Names are never removed, never rewritten and never freed for the
 *   life of the process.
 * Why: the detour runs on an ARBITRARY thread inside the engine's
 * CreateSoundEvent path (the engine takes its own mutex at
 * WwiseHybridSoundSystem+0x40 inside the factory, which is the engine
 * declaring that concurrent creation is expected). A detour that takes a
 * lock can be made to wait by any other thread; a detour that reads a
 * mutable array needs reclamation that x86 gives no cheap primitive for.
 * Append-only publication needs neither: an acquire load of the count and an
 * index into slots that were written before the count was published is
 * lock-free, wait-free and has no reclamation problem at all. The only cost
 * of never shrinking is that a name whose last mod unregistered still costs
 * one strcmp and one DispatchClientHostSoundIntercept call that answers
 * WOTBMOD_V3_E_NOT_FOUND, i.e. the sound plays exactly as stock.
 *
 * Matching is strcmp against the DEREFERENCED FastName dword -- DAVA
 * FastName is one dword holding an interned const char*, so there is no
 * allocation, no FastNameDB lock and no FastName construction on the hot
 * path. Exact match only; prefix and wildcard sets are not expressible.
 *
 * Idempotent: called again with a name already in the set it must return
 * WOTBMOD_V3_OK and change nothing. Returns WOTBMOD_V3_E_LIMIT_REACHED when
 * the fixed slot count is exhausted, and WOTBMOD_V3_E_NOT_SUPPORTED when the
 * detour cannot be installed on this build. Called on a mod thread with no
 * runtime lock held, and never from inside the detour.
 *
 * SUPPRESSION, restated because getting it wrong is a crash and not a bug:
 * when DispatchClientHostSoundIntercept answers SUPPRESS the detour must
 * return the engine's OWN SoundEventStub -- the object base
 * DAVA::SoundSystem::CreateSoundEvent builds through sub_C474F0 -- and NEVER
 * nullptr. Six of the eight real call sites write the result straight into a
 * field without a null check. If the stub allocation fails, fall back to
 * PASS_THROUGH; a sound that plays is always better than a store to null.
 * Group: audio_intercept_publish_name + audio_intercept_disarm.
 */
typedef WotbModV3Result (*ClientHostSoundInterceptPublishFn)(
    void* user_data,
    const char* event_name);

/*
 * Removes the detour and returns ONLY once no detour invocation is still
 * running on any thread. The runtime calls this while tearing the client
 * services down, and relies on that guarantee: after it returns, no thread
 * may enter DispatchClientHostSoundIntercept again. Idempotent. The match
 * set is not cleared and does not need to be -- nothing reads it once the
 * detour is gone.
 */
typedef WotbModV3Result (*ClientHostSoundInterceptDisarmFn)(
    void* user_data);

/* ------------------------------------------------------ scene.enumerate */

/*
 * Reports the caps this provider will actually enforce, which may be LOWER
 * than the WOTBMOD_V3_SCENE_WALK_MAX_* ceilings but never higher -- the
 * runtime clamps anything larger back to the ceiling. Set node_record_size
 * to sizeof(WotbModV3SceneNodeRecord) as this backend compiled it; the
 * runtime compares it with its own and refuses the whole group on a
 * mismatch, which is the cheap version of an ABI check between two binaries
 * that must agree on a 160-byte record.
 * Touches no engine memory, so it is callable from any thread.
 * Group: scene_get_walk_limits + scene_walk_active.
 */
typedef WotbModV3Result (*ClientHostSceneWalkLimitsFn)(
    void* user_data,
    WotbModV3SceneWalkLimits* out_limits);

/*
 * Breadth-first walk of the active scene, root at index 0 with parent_index
 * WOTBMOD_V3_SCENE_NODE_NO_PARENT.
 *
 * MAIN THREAD ONLY, and this is a safety property rather than a convention.
 * Entity::AddNode and Entity::RemoveNode memmove the child vector with NO
 * lock, and RemoveNode calls Release on the child immediately after
 * compacting, so a walker on another thread can hold a pointer that the next
 * instruction frees. There is no lock and no version counter that could
 * detect it. The runtime already refuses every other thread with
 * WOTBMOD_V3_E_WRONG_THREAD before calling; the backend must check again and
 * return WOTBMOD_V3_E_WRONG_THREAD rather than trusting its caller.
 *
 * Buffer contract:
 *   - `out_nodes` is RUNTIME-owned staging storage for exactly
 *     `node_capacity` records, or null when node_capacity is 0 (the counting
 *     pass). It is never the mod's buffer.
 *   - Write min(reached, node_capacity) records, in walk order, starting at
 *     index 0. Do not write past node_capacity under any circumstance.
 *   - Set *out_reached to how many nodes the walk REACHED, even when that
 *     exceeds node_capacity. The runtime turns reached > capacity into the
 *     mod-visible WOTBMOD_V3_E_BUFFER_TOO_SMALL and never sees a silent
 *     prefix as a complete answer.
 *   - Return WOTBMOD_V3_OK for both the counting pass and a full write.
 *     A short write is not an error at this boundary.
 *
 * Per-record contract: struct_size/api_version stamped, index = the record's
 * own position, parent_index = the parent's position or _NO_PARENT for the
 * root, depth from 0, child_count = the number of children the ENGINE
 * reported even when fewer were emitted, name a bounded NUL-terminated copy
 * of the FastName's interned string, and flags carrying IS_SCENE,
 * HAS_WORLD_MATRIX, NAME_TRUNCATED, CHILDREN_TRUNCATED and DEPTH_LIMITED
 * exactly as they occurred. world_matrix is the Matrix4 at
 * TransformComponent+0x60 and is only meaningful with HAS_WORLD_MATRIX set;
 * leave it identity otherwise. The runtime rejects an unterminated name, an
 * out-of-range parent_index, an unknown flag bit or a non-finite matrix with
 * WOTBMOD_V3_E_PLATFORM -- a malformed record is never handed to a mod.
 *
 * Return WOTBMOD_V3_E_NOT_FOUND when no scene is active. That is the normal
 * answer during login, loading and on UI-only screens; it is not an error
 * condition and must not be reported as NOT_SUPPORTED.
 */
typedef WotbModV3Result (*ClientHostSceneWalkFn)(
    void* user_data,
    const WotbModV3SceneWalkRequest* request,
    WotbModV3SceneNodeRecord* out_nodes,
    uint32_t node_capacity,
    uint32_t* out_reached);

/* ------------------------------------------------------------- tracer */

/*
 * The number of entries the running client's shell-type table actually has.
 * On 11.19.0.834 that is 25, the bound the resolver itself enforces
 * (cmp al,0x19 / jnb error). Report what this build has, never the header
 * constant. Must never report more than
 * WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT; the runtime clamps and logs if it
 * does. Callable from any thread: the table is immutable .rdata.
 * Group: tracer_get_shell_type_count + tracer_get_style.
 */
typedef WotbModV3Result (*ClientHostTracerShellTypeCountFn)(
    void* user_data,
    uint32_t* out_count);

/* ------------------------------------------------------------- ges */

/*
 * wotbmod.ges backend (docs/superpowers/specs/2026-09-03-ges-event-bus-design.md).
 * Group: ges_list_types + ges_observe + ges_publish.
 *
 * ges_list_types visits every GES event type the loader indexed from the
 * client's RTTI, as "Owner::Name". ges_observe registers (observe=1) or
 * releases (observe=0) an engine-native listener for one type; while
 * registered the loader calls wotbmod::v3::GesHostPublish from the engine's
 * own dispatch loop. ges_publish replays that loop with a mod payload; main
 * thread only, WOTBMOD_V3_E_WRONG_THREAD otherwise.
 */
typedef void (*ClientHostGesTypeVisitFn)(void* visit_data, const char* type_name);
typedef WotbModV3Result (*ClientHostGesListTypesFn)(
    void* user_data,
    ClientHostGesTypeVisitFn visit,
    void* visit_data);
typedef WotbModV3Result (*ClientHostGesObserveFn)(
    void* user_data,
    const char* type_name,
    uint32_t observe);
typedef WotbModV3Result (*ClientHostGesPublishFn)(
    void* user_data,
    const char* type_name,
    const void* payload,
    uint32_t payload_size,
    uint32_t flags);

/* ------------------------------------------------- session.cluster */

/*
 * wotbmod.session.cluster backend
 * (docs/superpowers/specs/2026-09-07-cluster-picker-design.md).
 * Group: session_cluster_enumerate + session_cluster_get_current +
 *        session_cluster_change + session_cluster_set_manual.
 *
 * enumerate/get_current: any thread, guarded field reads of the Region and
 * ConnectionManager the loader captured on the client's first
 * LoginManager::OnHostChosen; before that capture both answer
 * WOTBMOD_V3_E_NOT_SUPPORTED. change: MAIN thread only, calls
 * LoginManager::ChangeCluster(id) (id -1 = the client's own auto-select) and
 * sets the client's "manually selected cluster" flag for id >= 0.
 * set_manual(0) clears that flag after a failed switch so the next login
 * auto-selects again.
 */
typedef WotbModV3Result (*ClientHostSessionClusterEnumerateFn)(
    void* user_data,
    WotbModV3ClusterInfo* items,
    uint32_t* inout_count);
typedef WotbModV3Result (*ClientHostSessionClusterGetCurrentFn)(
    void* user_data,
    WotbModV3ClusterInfo* out_info);
typedef WotbModV3Result (*ClientHostSessionClusterChangeFn)(
    void* user_data,
    int32_t cluster_id);
typedef WotbModV3Result (*ClientHostSessionClusterSetManualFn)(
    void* user_data,
    uint32_t manual);

/*
 * Resolves one shell type. `shell_type` has already been bounds-checked by
 * the runtime against WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT; check it again
 * against the real table length and return WOTBMOD_V3_E_INVALID_ARGUMENT if
 * it is out of range. NEVER clamp to the last entry and never substitute a
 * default style -- a wrong style silently applied is worse than an error.
 *
 * On success set style_name to the table's own string, style_id to the
 * matching WotbModV3TracerStyleId (or _UNKNOWN when a future build adds a
 * ninth style, keeping the NAME authoritative), and
 * WOTBMOD_V3_TRACER_STYLE_NAME_VALID.
 *
 * The colour is INDEPENDENT. Set WOTBMOD_V3_TRACER_STYLE_COLOR_VALID and the
 * RGBA only if it was read from a per-shell record that actually existed.
 * The record+0x34 offset is marked INFERRED in re_anchors.md: if it has not
 * been confirmed live on this fingerprint, leave the flag CLEAR and the
 * colour zeroed. Reporting a guessed colour as valid is the failure mode
 * this flag exists to prevent, and the runtime forces the colour to zero
 * whenever the flag is clear.
 *
 * Returning OK without NAME_VALID is a contract violation and the runtime
 * answers the mod WOTBMOD_V3_E_PLATFORM.
 */
typedef WotbModV3Result (*ClientHostTracerStyleFn)(
    void* user_data,
    uint32_t shell_type,
    WotbModV3TracerStyle* out_style);

/*
 * The loader fills this and calls SetClientHostDeclaredBackend. Every
 * function pointer may be null; the five groups are installed independently
 * and a group with any null member is dropped whole.
 */
struct ClientHostDeclaredBackend {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t binding_pack_version;
    /*
     * WOTBMOD_V3_CLIENT_COMPATIBILITY_*. The table is refused unless this is
     * SUPPORTED or DEGRADED -- the exact-fingerprint gate, fail-closed.
     */
    uint32_t compatibility_state;
    /* Opaque loader context, passed back verbatim to every function. */
    void* user_data;

    ClientHostUiReadStringFn ui_read_string;
    ClientHostUiReadStyleFn ui_read_style;

    ClientHostCameraStateFn camera_read_observed_state;

    ClientHostSoundInterceptPublishFn audio_intercept_publish_name;
    ClientHostSoundInterceptDisarmFn audio_intercept_disarm;

    ClientHostSceneWalkLimitsFn scene_get_walk_limits;
    ClientHostSceneWalkFn scene_walk_active;

    ClientHostTracerShellTypeCountFn tracer_get_shell_type_count;
    ClientHostTracerStyleFn tracer_get_style;
    /* ges */
    ClientHostGesListTypesFn ges_list_types;
    ClientHostGesObserveFn ges_observe;
    ClientHostGesPublishFn ges_publish;
    /* session.cluster */
    ClientHostSessionClusterEnumerateFn session_cluster_enumerate;
    ClientHostSessionClusterGetCurrentFn session_cluster_get_current;
    ClientHostSessionClusterChangeFn session_cluster_change;
    ClientHostSessionClusterSetManualFn session_cluster_set_manual;
};

/*
 * Installs or removes the declared backend. Pass null to remove.
 *
 * Removal is synchronous and quiescing: it stops accepting new calls, waits
 * for every in-flight backend call to return, calls audio_intercept_disarm
 * if a sound detour was armed, and only then drops the table. After it
 * returns, no runtime thread holds a pointer into the loader.
 *
 * It also republishes the five interfaces' availability: UNAVAILABLE with
 * the frozen reason when a group is absent, DEGRADED with a precise reason
 * when it is present. It never publishes AVAILABLE -- that needs a manual
 * live PASS on the exact client fingerprint, which is not something this
 * layer can observe.
 *
 * Must not be called from inside a backend function or from the sound
 * detour.
 */
void SetClientHostDeclaredBackend(
    const ClientHostDeclaredBackend* backend);

/*
 * Snapshot of the installed declared backend for callers outside
 * client_services.cpp (ges_services.cpp). Returns false, and zeroes *out,
 * when no backend is installed or it is quiescing. The copy holds plain
 * function pointers; the loader keeps them valid until it uninstalls, which
 * it does only after SetClientHostDeclaredBackend(nullptr) has quiesced.
 */
bool GetClientHostDeclaredBackend(ClientHostDeclaredBackend* out);

/*
 * SOUND DETOUR INGRESS. The armed CreateSoundEvent detour calls this after
 * its own strcmp against the published match set says the name is wanted.
 *
 * `event_name` is the dereferenced FastName string, borrowed for the call.
 * `out_response` is detour-owned storage; the runtime stamps
 * struct_size/api_version and initialises `decision` to PASS_THROUGH before
 * running a single mod callback, so a callback that writes nothing, faults,
 * or writes an out-of-range value leaves the game's own behaviour untouched.
 *
 * Runs on an ARBITRARY thread. Returns:
 *   WOTBMOD_V3_OK              -- out_response is filled; honour it.
 *   WOTBMOD_V3_E_NOT_FOUND     -- nothing wants this name right now.
 *   WOTBMOD_V3_E_BUSY          -- re-entry on this thread (the hybrid system
 *                                 calls the inner system's CreateSoundEvent
 *                                 at 0x024F3BA2, so the public path re-enters
 *                                 the detour; the guard is thread-local).
 *   anything else              -- the runtime is not accepting dispatches.
 * EVERY non-OK return means PASS_THROUGH. The detour must treat it that way
 * and must never derive suppression from a failure.
 *
 * On WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE the runtime guarantees
 * `substitute_event_name` is a non-empty NUL-terminated string; an empty or
 * malformed one has already been downgraded to PASS_THROUGH, because
 * dropping a sound the mod did not ask to drop is a behaviour change the mod
 * never requested.
 *
 * Ordering when several mods want the same name: highest priority first,
 * registration order breaking ties, and the first decision that is not
 * PASS_THROUGH wins and ends the chain.
 *
 * The runtime already wraps each mod callback in its own SEH handler, so a
 * faulting mod cannot unwind into the detour through this call. The detour
 * should still not assume this is the only thing between it and the engine:
 * whatever it does with the answer -- constructing a FastName, fetching the
 * stub -- is its own to guard.
 */
WotbModV3Result DispatchClientHostSoundIntercept(
    const char* event_name,
    WotbModV3SoundInterceptResponse* out_response);

}  // namespace v3
}  // namespace wotbmod
