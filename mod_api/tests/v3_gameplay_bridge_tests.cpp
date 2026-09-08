#include "../loader/v3_managed_renderer.h"
#include "../loader/v3_camera_effects.h"
#include "../loader/v3_native_bindings.h"
#include "../loader/v3_native_client_services.h"
#include "../src/v3/client_services_backend.h"
#include "../include/wotbmod/gameplay_camera_v1.h"
#include "../include/wotbmod/client_v1.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace {

constexpr uint32_t kMaxMockEntities = 16u;
constexpr uint32_t kMaxMockEvents = 32u;

struct MockEntity {
    uint64_t native_token;
    WotbModV3EntityHandle handle;
    WotbModV3PublicEntitySnapshot snapshot;
    bool active;
};

struct MockEvent {
    char topic[WOTBMOD_V3_MAX_NAME];
    uint32_t payload_size;
    WotbModV3PublicEntityLifecycleEvent lifecycle;
};

MockEntity g_entities[kMaxMockEntities] = {};
MockEvent g_events[kMaxMockEvents] = {};
uint32_t g_event_count = 0u;
uint32_t g_rpc_count = 0u;
WotbModV3ObservedRpc g_last_rpc = {};
uint32_t g_last_ui_input_phase = 0u;
uint64_t g_next_handle = 1u;
uint32_t g_failures = 0u;
uint32_t g_passes = 0u;

void Check(bool condition, const char* label) {
    if (condition) {
        ++g_passes;
        return;
    }
    ++g_failures;
    std::printf("FAIL: %s\n", label ? label : "(null)");
}

uint32_t ActiveEntityCount() {
    uint32_t count = 0u;
    for (const MockEntity& entity : g_entities) {
        if (entity.active) ++count;
    }
    return count;
}

MockEntity* FindEntity(WotbModV3EntityHandle handle) {
    for (MockEntity& entity : g_entities) {
        if (entity.active && entity.handle == handle) {
            return &entity;
        }
    }
    return nullptr;
}

MockEntity* FindEntityByToken(uint64_t token) {
    for (MockEntity& entity : g_entities) {
        if (entity.active && entity.native_token == token) {
            return &entity;
        }
    }
    return nullptr;
}

const MockEvent* LastEvent() {
    return g_event_count == 0u
               ? nullptr
               : &g_events[g_event_count - 1u];
}

WotbModV3NativePublicVehicle Vehicle(
    uint32_t public_id,
    int32_t health,
    uint32_t flags,
    void* native_entity) {
    WotbModV3NativePublicVehicle vehicle = {};
    vehicle.struct_size = sizeof(vehicle);
    vehicle.public_id = public_id;
    vehicle.health = health;
    vehicle.max_health = health > 0 ? health : 0;
    vehicle.flags = flags;
    vehicle.native_entity = native_entity;
    return vehicle;
}

WotbModV3CameraState CameraState(
    float x,
    float fov = 60.0f) {
    WotbModV3CameraState state = {};
    state.struct_size = sizeof(state);
    state.api_version = WOTBMOD_V3_CAMERA_VERSION;
    state.transform.struct_size = sizeof(state.transform);
    state.transform.api_version = WOTBMOD_V3_ABI_VERSION;
    state.transform.position = {x, 0.0f, 0.0f};
    state.transform.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
    state.transform.scale = {1.0f, 1.0f, 1.0f};
    state.fov_degrees = fov;
    state.near_plane = 0.1f;
    state.far_plane = 1000.0f;
    state.mode = WOTBMOD_V3_CAMERA_MODE_UNKNOWN;
    return state;
}

WotbModV3CameraTransition Transition(
    float target_x,
    float duration,
    uint32_t easing,
    uint32_t preserve_game_control = 0u) {
    WotbModV3CameraTransition transition = {};
    transition.struct_size = sizeof(transition);
    transition.api_version = WOTBMOD_V3_CAMERA_VERSION;
    transition.target = CameraState(target_x, 100.0f);
    transition.duration_seconds = duration;
    transition.easing = easing;
    transition.preserve_game_control = preserve_game_control;
    return transition;
}

WotbModV3CameraShake Shake(
    float amplitude,
    float frequency,
    float duration,
    float falloff) {
    WotbModV3CameraShake shake = {};
    shake.struct_size = sizeof(shake);
    shake.api_version = WOTBMOD_V3_CAMERA_VERSION;
    shake.amplitude = amplitude;
    shake.frequency = frequency;
    shake.duration_seconds = duration;
    shake.falloff = falloff;
    return shake;
}

bool Near(float actual, float expected, float tolerance = 0.001f) {
    return std::fabs(actual - expected) <= tolerance;
}

void TestCameraEffects() {
    constexpr WotbModV3Handle kOwner = 11u;
    constexpr uint64_t kCamera = UINT64_C(0xC0DEC0DE);
    wotbmod::loader::CameraEffectStore effects;
    uint64_t controller = 0u;

    WotbModV3CameraTransition unsupported =
        Transition(10.0f, 1.0f, WOTBMOD_V3_EASING_LINEAR);
    unsupported.target.mode = WOTBMOD_V3_CAMERA_MODE_ARCADE;
    Check(
        effects.QueueTransition(
            kOwner,
            kCamera,
            unsupported,
            &controller) == WOTBMOD_V3_E_NOT_SUPPORTED,
        "camera transition does not manufacture an unverified mode");
    Check(
        effects.ActiveTransitionCount() == 0u &&
            effects.ControllerCount() == 0u,
        "rejected camera mode allocates no effect state");

    const WotbModV3CameraTransition linear =
        Transition(10.0f, 2.0f, WOTBMOD_V3_EASING_LINEAR);
    Check(
        effects.QueueTransition(
            kOwner,
            kCamera,
            linear,
            &controller) == WOTBMOD_V3_OK &&
            controller != 0u,
        "bounded camera transition queues with owner controller");
    Check(
        effects.ActiveTransitionCount() == 1u &&
            effects.ControllerCount() == 1u,
        "camera transition and owner controller are tracked");

    WotbModV3CameraState state = CameraState(0.0f);
    effects.Apply(kCamera, 0.25, &state);
    Check(
        Near(state.transform.position.x, 0.0f) &&
            Near(state.fov_degrees, 60.0f),
        "camera transition captures the first readable frame");
    state = CameraState(0.0f);
    effects.Apply(kCamera, 1.0, &state);
    Check(
        Near(state.transform.position.x, 5.0f) &&
            Near(state.fov_degrees, 80.0f),
        "linear camera transition reaches deterministic midpoint");
    state = CameraState(0.0f);
    effects.Apply(kCamera, 1.0, &state);
    Check(
        Near(state.transform.position.x, 10.0f) &&
            Near(state.fov_degrees, 100.0f) &&
            effects.ActiveTransitionCount() == 0u &&
            effects.ControllerCount() == 1u,
        "camera transition expires while its owner controller stays stable");
    state = CameraState(3.0f, 70.0f);
    effects.Apply(kCamera, 1.0, &state);
    Check(
        Near(state.transform.position.x, 3.0f) &&
            Near(state.fov_degrees, 70.0f),
        "expired camera transition releases stock camera state");

    const uint64_t original_controller = controller;
    const WotbModV3CameraTransition raced_transition =
        Transition(12.0f, 1.0f, WOTBMOD_V3_EASING_LINEAR);
    const WotbModV3CameraShake raced_shake =
        Shake(1.0f, 2.0f, 1.0f, 1.0f);
    std::atomic<bool> start_race{false};
    WotbModV3Result transition_result =
        WOTBMOD_V3_E_PLATFORM;
    WotbModV3Result shake_result =
        WOTBMOD_V3_E_PLATFORM;
    uint64_t transition_controller = 0u;
    uint64_t shake_controller = 0u;
    std::thread transition_thread([&]() {
        while (!start_race.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        transition_result = effects.QueueTransition(
            kOwner,
            kCamera,
            raced_transition,
            &transition_controller);
    });
    std::thread shake_thread([&]() {
        while (!start_race.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        shake_result = effects.QueueShake(
            kOwner,
            kCamera,
            raced_shake,
            &shake_controller);
    });
    start_race.store(true, std::memory_order_release);
    transition_thread.join();
    shake_thread.join();
    Check(
        transition_result == WOTBMOD_V3_OK &&
            shake_result == WOTBMOD_V3_OK &&
            transition_controller == original_controller &&
            shake_controller == original_controller &&
            effects.ControllerCount() == 1u,
        "concurrent replacement effects reuse the tracked owner controller");
    Check(
        effects.Cancel(kOwner, original_controller) ==
                WOTBMOD_V3_OK &&
            effects.ActiveTransitionCount() == 0u &&
            effects.ActiveShakeCount() == 0u &&
            effects.ControllerCount() == 0u,
        "tracked controller cancels every concurrently queued replacement");
    controller = 0u;

    const WotbModV3CameraTransition preserved =
        Transition(
            10.0f,
            2.0f,
            WOTBMOD_V3_EASING_LINEAR,
            1u);
    Check(
        effects.QueueTransition(
            kOwner,
            kCamera,
            preserved,
            &controller) == WOTBMOD_V3_OK,
        "game-control-preserving transition queues");
    state = CameraState(0.0f);
    effects.Apply(kCamera, 0.0, &state);
    state = CameraState(4.0f);
    effects.Apply(kCamera, 1.0, &state);
    Check(
        Near(state.transform.position.x, 7.0f),
        "preserving transition blends the current stock camera");

    const WotbModV3CameraTransition quadratic =
        Transition(20.0f, 2.0f, WOTBMOD_V3_EASING_IN_QUAD);
    Check(
        effects.QueueTransition(
            kOwner,
            kCamera,
            quadratic,
            &controller) == WOTBMOD_V3_OK &&
            effects.ActiveTransitionCount() == 1u,
        "new owner transition replaces its previous transition");
    state = CameraState(0.0f);
    effects.Apply(kCamera, 0.0, &state);
    state = CameraState(0.0f);
    effects.Apply(kCamera, 1.0, &state);
    Check(
        Near(state.transform.position.x, 5.0f),
        "camera transition applies declared easing");
    effects.Apply(kCamera + 1u, 0.1, &state);
    Check(
        effects.ActiveTransitionCount() == 0u,
        "camera replacement discards effects bound to the old camera");

    const WotbModV3CameraShake shake =
        Shake(2.0f, 4.0f, 1.0f, 1.0f);
    Check(
        effects.QueueShake(
            kOwner,
            kCamera,
            shake,
            &controller) == WOTBMOD_V3_OK &&
            effects.ActiveShakeCount() == 1u,
        "deterministic camera shake queues");
    state = CameraState(0.0f);
    effects.Apply(kCamera, 0.0, &state);
    const float first_shake_length = std::sqrt(
        state.transform.position.x * state.transform.position.x +
        state.transform.position.y * state.transform.position.y +
        state.transform.position.z * state.transform.position.z);
    Check(
        std::isfinite(first_shake_length) &&
            first_shake_length > 0.0f &&
            first_shake_length <= 2.001f,
        "camera shake is finite and amplitude-bounded");
    state = CameraState(0.0f);
    effects.Apply(kCamera, 1.0, &state);
    Check(
        Near(state.transform.position.x, 0.0f) &&
            Near(state.transform.position.y, 0.0f) &&
            Near(state.transform.position.z, 0.0f) &&
            effects.ActiveShakeCount() == 0u,
        "camera shake falls to zero and expires");

    const WotbModV3CameraShake no_op =
        Shake(2.0f, 0.0f, 1.0f, 1.0f);
    Check(
        effects.QueueShake(
            kOwner,
            kCamera,
            no_op,
            &controller) == WOTBMOD_V3_OK &&
            effects.ActiveShakeCount() == 0u,
        "zero-frequency shake is a successful bounded no-op");

    const WotbModV3CameraShake capped =
        Shake(25.0f, 3.0f, 5.0f, 0.0f);
    for (uint32_t index = 0u; index < 8u; ++index) {
        Check(
            effects.QueueShake(
                kOwner,
                kCamera,
                capped,
                &controller) == WOTBMOD_V3_OK,
            "per-owner camera shake within limit queues");
    }
    Check(
        effects.QueueShake(
            kOwner,
            kCamera,
            capped,
            &controller) == WOTBMOD_V3_E_LIMIT_REACHED,
        "per-owner camera shake limit rejects excess work");
    state = CameraState(0.0f);
    effects.Apply(kCamera, 0.0, &state);
    const float combined_length = std::sqrt(
        state.transform.position.x * state.transform.position.x +
        state.transform.position.y * state.transform.position.y +
        state.transform.position.z * state.transform.position.z);
    Check(
        std::isfinite(combined_length) &&
            combined_length <= 25.001f,
        "combined camera shake is globally amplitude-clamped");
    Check(
        effects.Cancel(kOwner + 1u, controller) ==
            WOTBMOD_V3_E_INVALID_HANDLE,
        "camera effect controller rejects a different owner");
    Check(
        effects.Cancel(kOwner, controller) == WOTBMOD_V3_OK &&
            effects.ActiveTransitionCount() == 0u &&
            effects.ActiveShakeCount() == 0u &&
            effects.ControllerCount() == 0u,
        "owner teardown cancels every camera effect");

    const WotbModV3CameraShake invalid =
        Shake(26.0f, 1.0f, 1.0f, 1.0f);
    Check(
        effects.QueueShake(
            kOwner,
            kCamera,
            invalid,
            &controller) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "camera shake rejects unsafe amplitude");
    effects.Clear();
    Check(
        effects.ActiveTransitionCount() == 0u &&
            effects.ActiveShakeCount() == 0u &&
            effects.ControllerCount() == 0u,
        "camera effect store clears on loader shutdown");
}

}  // namespace

namespace wotbmod {
namespace loader {

WotbModV3Result InitializeV3NativeClientServices(
    const V3NativeClientServicesOptions*) {
    return WOTBMOD_V3_OK;
}

WotbModV3Result InvokeV3NativeClientServices(
    WotbModV3Handle,
    const char*,
    const void*,
    uint32_t,
    void*,
    uint32_t) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

void ShutdownV3NativeClientServices() {
}

void NotifyV3NativeClientServicesUiInputPhase(uint32_t phase) {
    g_last_ui_input_phase = phase;
}

void PumpV3NativeClientServicesFrame() {
}

void NotifyV3NativeClientServicesMainThread(uint32_t, uint64_t) {
}

bool IsV3NativeClientServicesMainThread() {
    return false;
}

/*
 * The declared-backend slots this binary does not exercise. They exist so the
 * loader half of wotbmod.ui.read / wotbmod.scene.enumerate can be linked
 * without dragging in the whole client-services translation unit, and they
 * answer NOT_SUPPORTED rather than OK: a stub that claimed success would let
 * this test pass while the real backend was missing.
 */
WotbModV3Result V3NativeUiReadString(
    void*,
    const v3::ClientHostUiReadRequest*,
    v3::ClientHostUiReadString*) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result V3NativeUiReadStyle(
    void*,
    const v3::ClientHostUiReadRequest*,
    WotbModV3UiStyleSnapshot*) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result V3NativeSceneWalkLimits(
    void*,
    WotbModV3SceneWalkLimits*) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result V3NativeSceneWalkActive(
    void*,
    const WotbModV3SceneWalkRequest*,
    WotbModV3SceneNodeRecord*,
    uint32_t,
    uint32_t*) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result ManagedRendererCreate() {
    return WOTBMOD_V3_OK;
}

WotbModV3Result ManagedRendererUpdateFrame(
    void*,
    void*,
    void*,
    uint32_t,
    uint32_t,
    uint64_t,
    double) {
    return WOTBMOD_V3_OK;
}

WotbModV3Result ManagedRendererInvoke(
    WotbModV3Handle,
    const char*,
    const v3::ClientHostObjectRequest*,
    v3::ClientHostObjectResponse*) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

void ManagedRendererShutdown() {
}

}  // namespace loader

namespace v3 {

WotbModV3Result NotifyNativeInput(
    uint32_t,
    uint32_t,
    uint32_t,
    uint32_t,
    float,
    uint32_t) {
    return WOTBMOD_V3_OK;
}

void ResetNativeInputState() {
}

WotbModV3Result ApplyClientHostCameraModifiers(
    uint32_t,
    WotbModV3CameraState*) {
    return WOTBMOD_V3_OK;
}

WotbModV3Result NotifyClientHostUiInput(
    uint32_t,
    float,
    float,
    float,
    float,
    uint32_t) {
    return WOTBMOD_V3_OK;
}

bool ShouldCaptureClientHostUiInput(float, float) {
    return false;
}

WotbModV3Result NotifyClientHostObservedRpc(
    const WotbModV3ObservedRpc* rpc) {
    if (!rpc) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    g_last_rpc = *rpc;
    ++g_rpc_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result PublishClientHostEvent(
    const char* event,
    const void* payload,
    uint32_t payload_size) {
    if (!event || (!payload && payload_size != 0u)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (g_event_count >= kMaxMockEvents) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    MockEvent& recorded = g_events[g_event_count++];
    strncpy_s(
        recorded.topic,
        sizeof(recorded.topic),
        event,
        _TRUNCATE);
    recorded.payload_size = payload_size;
    if (payload &&
        payload_size ==
            sizeof(WotbModV3PublicEntityLifecycleEvent)) {
        std::memcpy(
            &recorded.lifecycle,
            payload,
            sizeof(recorded.lifecycle));
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result RegisterClientHostPublicEntity(
    const ClientHostPublicEntity* entity,
    WotbModV3EntityHandle* out_handle) {
    if (!entity || !out_handle ||
        entity->snapshot.visible_to_player == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    MockEntity* slot = FindEntityByToken(entity->native_token);
    if (!slot) {
        for (MockEntity& candidate : g_entities) {
            if (!candidate.active) {
                slot = &candidate;
                break;
            }
        }
    }
    if (!slot) return WOTBMOD_V3_E_LIMIT_REACHED;
    if (!slot->active) {
        slot->handle =
            UINT64_C(0xE300000000000000) | g_next_handle++;
    }
    slot->active = true;
    slot->native_token = entity->native_token;
    slot->snapshot = entity->snapshot;
    slot->snapshot.handle = slot->handle;
    *out_handle = slot->handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UpdateClientHostPublicEntity(
    WotbModV3EntityHandle handle,
    const ClientHostPublicEntity* entity) {
    MockEntity* current = FindEntity(handle);
    if (!current || !entity ||
        current->native_token != entity->native_token) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    current->snapshot = entity->snapshot;
    current->snapshot.handle = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result RemoveClientHostPublicEntity(
    WotbModV3EntityHandle handle) {
    MockEntity* current = FindEntity(handle);
    if (!current) return WOTBMOD_V3_E_INVALID_HANDLE;
    *current = {};
    return WOTBMOD_V3_OK;
}

/*
 * A RECORDING DOUBLE, NOT A REFUSAL.
 *
 * These four were hard-coded `NOT_SUPPORTED` stubs, and this is the only test
 * binary that links the native bindings at all. So the shot-ingress path -
 * `WotbModV3NativeBindings_ObserveShot`, the one thing that turns a fired shell
 * into a `wotbmod.projectile.*` event - could not be tested even in principle:
 * anything it did ended in a stub that said no.
 *
 * That is how it shipped broken. Live on 11.19.0.834 four shots in a training
 * battle produced four `gameplay.shot_fired` and zero projectile events, and
 * the whole trace history holds 68 shots, 105 hits and not one
 * `wotbmod.projectile.*`. The double now mirrors the real registry closely
 * enough that the ingress path is exercised end to end.
 */
struct MockProjectile {
    bool active;
    WotbModV3ProjectileHandle handle;
    uint64_t native_token;
    uint32_t owner_scope;
    uint32_t lifecycle_state;
    uint32_t stock_shot_code;
    uint32_t primary_entity_id;
};

constexpr uint32_t kMaxMockProjectiles = 16u;
MockProjectile g_projectiles[kMaxMockProjectiles] = {};
uint32_t g_projectile_register_calls = 0u;
uint32_t g_projectile_publish_calls = 0u;

MockProjectile* FindProjectile(WotbModV3ProjectileHandle handle) {
    for (MockProjectile& projectile : g_projectiles) {
        if (projectile.active && projectile.handle == handle) {
            return &projectile;
        }
    }
    return nullptr;
}

uint32_t ActiveProjectileCount() {
    uint32_t count = 0u;
    for (const MockProjectile& projectile : g_projectiles) {
        if (projectile.active) ++count;
    }
    return count;
}

WotbModV3Result RegisterClientHostProjectile(
    const ClientHostProjectile* projectile,
    WotbModV3ProjectileHandle* out_handle) {
    ++g_projectile_register_calls;
    if (!projectile || !out_handle ||
        projectile->native_token == 0u ||
        projectile->owner_scope ==
            WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (MockProjectile& slot : g_projectiles) {
        if (slot.active) continue;
        slot.active = true;
        slot.handle = g_next_handle++;
        slot.native_token = projectile->native_token;
        slot.owner_scope = projectile->owner_scope;
        slot.lifecycle_state = projectile->lifecycle_state;
        slot.stock_shot_code = projectile->stock_shot_code;
        slot.primary_entity_id = projectile->primary_entity_id;
        *out_handle = slot.handle;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_LIMIT_REACHED;
}

WotbModV3Result UpdateClientHostProjectile(
    WotbModV3ProjectileHandle handle,
    const ClientHostProjectile* projectile) {
    MockProjectile* const current = FindProjectile(handle);
    if (!current || !projectile) return WOTBMOD_V3_E_INVALID_HANDLE;
    current->lifecycle_state = projectile->lifecycle_state;
    return WOTBMOD_V3_OK;
}

WotbModV3Result RemoveClientHostProjectile(
    WotbModV3ProjectileHandle handle,
    uint32_t) {
    MockProjectile* const current = FindProjectile(handle);
    if (!current) return WOTBMOD_V3_E_INVALID_HANDLE;
    *current = {};
    return WOTBMOD_V3_OK;
}

WotbModV3Result PublishClientHostProjectileLifecycle(
    WotbModV3ProjectileHandle handle,
    uint32_t,
    uint32_t) {
    ++g_projectile_publish_calls;
    const MockProjectile* const current = FindProjectile(handle);
    if (!current) return WOTBMOD_V3_E_INVALID_HANDLE;
    // The topic the real backend picks from the stored lifecycle state, so
    // the existing event assertions can see a projectile the same way they
    // see an entity.
    const char* topic = nullptr;
    switch (current->lifecycle_state) {
        case WOTBMOD_V3_PROJECTILE_STATE_CREATED:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_CREATED;
            break;
        case WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_UPDATED;
            break;
        case WOTBMOD_V3_PROJECTILE_STATE_IMPACTED:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED;
            break;
        case WOTBMOD_V3_PROJECTILE_STATE_DESTROYED:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED;
            break;
        default:
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const uint32_t marker = current->stock_shot_code;
    return PublishClientHostEvent(topic, &marker, sizeof(marker));
}

/*
 * Declared-backend ingress. Installing the table is a no-op here and the
 * sound-interception dispatch always answers NOT_FOUND, which the detour is
 * contractually required to read as PASS_THROUGH - so this binary can link
 * the loader's sound path without ever suppressing or substituting anything.
 */
void SetClientHostDeclaredBackend(const ClientHostDeclaredBackend*) {
}

WotbModV3Result DispatchClientHostSoundIntercept(
    const char*,
    WotbModV3SoundInterceptResponse*) {
    return WOTBMOD_V3_E_NOT_FOUND;
}

/*
 * The runtime's MAIN async ingress. This binary links the loader without
 * runtime_services.cpp, so these two are stubbed like every other runtime
 * symbol above.
 *
 * PumpMainThread returning 0 is the honest stub answer, and it is the same
 * answer the real function gives while ingress is offline: no callback was
 * drained. Nothing in this suite exercises the queue - it tests the bridge's
 * bindings and detour installation, not async dispatch - so a stub that
 * pretended to drain work would be inventing a result nobody checks.
 */
void SetMainIngressOnline(bool) {
}

uint32_t PumpMainThread(uint32_t) {
    return 0u;
}

WotbModV3Result InvokeMainThreadBlocking(
    WotbModDavaMainThreadCallFn,
    void*,
    uint32_t) {
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

}  // namespace v3
}  // namespace wotbmod

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_NotifyClientEvent(
    const WotbModRuntimeClientEvent* event) {
    return event ? WOTBMOD_OK : WOTBMOD_ERROR_INVALID_ARGUMENT;
}

int main() {
    TestCameraEffects();

    Check(
        WotbModV3NativeBindings_ObserveUiInput(
            2u, 100.0f, 200.0f, 1.0f, -1.0f, 0u) ==
            WOTBMOD_V3_OK &&
            g_last_ui_input_phase == 2u,
        "native UI input phase reaches the client-services safety gate");

    Check(
        WotbModV3NativeBindings_GetInstalledSourceMask() == 0u,
        "native installed-source mask is empty before create");
    WotbModV3NativeBindings_SetGameplayBridgeSources(0x0FFFu);

    Check(
        WotbModV3NativeBindings_IsPublicVehicleFlags(0u) == 0,
        "legacy/V2 gate rejects hidden active vehicle");
    Check(
        WotbModV3NativeBindings_IsPublicVehicleFlags(
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE) != 0,
        "legacy/V2 gate exposes confirmed-spotted vehicle");
    Check(
        WotbModV3NativeBindings_IsPublicVehicleFlags(0u) == 0,
        "legacy/V2 gate removes vehicle after unspot");
    Check(
        WotbModV3NativeBindings_IsPublicVehicleFlags(
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL) != 0,
        "legacy/V2 gate always exposes local vehicle");
    Check(
        WotbModV3NativeBindings_IsPublicVehicleFlags(
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY) != 0,
        "legacy/V2 gate exposes only explicitly confirmed allies");

    WotbModV3NativePublicVehicle hidden =
        Vehicle(101u, 900, 0u, reinterpret_cast<void*>(0x1010u));
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &hidden,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "hidden spawn is rejected");
    Check(
        ActiveEntityCount() == 0u,
        "hidden spawn creates no public registry entry");
    Check(
        g_event_count == 0u,
        "hidden spawn publishes no V3 event");

    WotbModV3NativePublicVehicle ally = Vehicle(
        102u,
        850,
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY,
        reinterpret_cast<void*>(0x1020u));
    ally.team = 1u;
    ally.valid_fields =
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VISIBILITY |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_LOCAL |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE;
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &ally,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED) ==
            WOTBMOD_V3_OK,
        "same-team vehicle is accepted only with confirmed-ally provenance");
    Check(
        ActiveEntityCount() == 1u &&
            LastEvent() &&
            LastEvent()->lifecycle.snapshot.public_id == 102u &&
            LastEvent()->lifecycle.snapshot.team == 1u &&
            LastEvent()->lifecycle.snapshot.local_player == 0u,
        "confirmed ally publishes a sanitized non-local snapshot");
    Check(
        WotbModV3NativeBindings_RemovePublicVehicle(
            102u,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN) ==
            WOTBMOD_V3_OK &&
            ActiveEntityCount() == 0u,
        "confirmed ally is removed without retaining hidden registry state");

    WotbModV3NativePublicVehicle visible = Vehicle(
        101u,
        900,
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE,
        reinterpret_cast<void*>(0x1010u));
    visible.valid_fields =
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VISIBILITY |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_LOCAL |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TEAM |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DISPLAY_NAME;
    visible.team = 2u;
    strcpy_s(visible.display_name, "VisibleEnemy");
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &visible,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE) ==
            WOTBMOD_V3_OK,
        "spot registers visible vehicle");
    Check(
        ActiveEntityCount() == 1u,
        "spot creates one public registry entry");
    const MockEvent* event = LastEvent();
    Check(
        event &&
            std::strcmp(
                event->topic,
                WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED) == 0,
        "spot publishes public entity added");
    Check(
        event &&
            event->lifecycle.snapshot.public_id == 101u &&
            event->lifecycle.snapshot.visible_to_player == 1u &&
            event->lifecycle.snapshot.team == 2u &&
            std::strcmp(
                event->lifecycle.snapshot.display_name,
                "VisibleEnemy") == 0,
        "spot event contains sourced team and display-name attributes");

    /* pose ingress: position/direction land in the snapshot only with their
     * valid_fields bits, and a direction that is not a unit vector is refused */
    const auto closeTo = [](float a, float b) {
        return (a - b) < 0.001f && (b - a) < 0.001f;
    };
    WotbModV3NativePublicVehicle posed = Vehicle(
        202u,
        1200,
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE |
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL,
        reinterpret_cast<void*>(0x2020u));
    posed.valid_fields =
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VISIBILITY |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_LOCAL |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_POSITION |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DIRECTION;
    posed.position[0] = 172.22f;
    posed.position[1] = 35.54f;
    posed.position[2] = -225.11f;
    posed.direction[0] = 0.0f;
    posed.direction[1] = -0.023f;
    posed.direction[2] = 1.0f;
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &posed,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_LOCAL_PLAYER) ==
            WOTBMOD_V3_OK,
        "local vehicle pose is accepted by the ingress");
    event = LastEvent();
    Check(
        event &&
            event->lifecycle.snapshot.public_id == 202u &&
            closeTo(event->lifecycle.snapshot.position.x, 172.22f) &&
            closeTo(event->lifecycle.snapshot.position.y, 35.54f) &&
            closeTo(event->lifecycle.snapshot.position.z, -225.11f) &&
            closeTo(event->lifecycle.snapshot.direction.z, 1.0f),
        "pose event carries the sourced position and direction");
    posed.direction[2] = 5.0f;
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &posed,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a direction that is not a unit vector is refused");
    posed.direction[2] = 1.0f;
    posed.valid_fields &=
        ~static_cast<uint64_t>(
            WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_POSITION |
            WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DIRECTION);
    posed.position[0] = 999.0f;
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &posed,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED) ==
            WOTBMOD_V3_OK,
        "upsert without the pose bits still lands");
    event = LastEvent();
    Check(
        event &&
            event->lifecycle.snapshot.public_id == 202u &&
            event->lifecycle.snapshot.position.x == 0.0f &&
            event->lifecycle.snapshot.direction.z == 0.0f,
        "position and direction are not copied without their bits");
    Check(
        WotbModV3NativeBindings_RemovePublicVehicle(
            202u,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN) ==
            WOTBMOD_V3_OK &&
            ActiveEntityCount() == 1u,
        "posed local vehicle leaves, the spotted vehicle remains");

    /* roster extras: clan tag, account id, kills and vehicle name ride on
     * their own valid_fields bits and are validated like the snapshot text */
    WotbModV3NativePublicVehicle roster = Vehicle(
        303u,
        1500,
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE |
            WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_ALLY,
        reinterpret_cast<void*>(0x3030u));
    roster.valid_fields =
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_VISIBILITY |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_LOCAL |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_MAX_HEALTH |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_DISPLAY_NAME |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_CLAN_TAG |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_ACCOUNT_ID |
        WOTBMOD_V3_NATIVE_PUBLIC_ENTITY_FIELD_KILLS;
    strcpy_s(roster.display_name, "Ivanlesnikov1512");
    strcpy_s(roster.clan_tag, "PIGGG");
    roster.account_id = INT64_C(598289265);
    roster.kills = 2;
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &roster,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE) ==
            WOTBMOD_V3_OK &&
            ActiveEntityCount() == 2u,
        "roster extras are accepted by the ingress");
    event = LastEvent();
    Check(
        event &&
            event->lifecycle.snapshot.public_id == 303u &&
            std::strcmp(
                event->lifecycle.snapshot.display_name,
                "Ivanlesnikov1512") == 0,
        "roster event carries the player name as display_name");
    roster.kills = -1;
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &roster,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "negative kills are refused");
    roster.kills = 2;
    std::memset(roster.clan_tag, 'X', sizeof(roster.clan_tag));
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &roster,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "an unterminated clan tag is refused");
    Check(
        WotbModV3NativeBindings_RemovePublicVehicle(
            303u,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN) ==
            WOTBMOD_V3_OK &&
            ActiveEntityCount() == 1u,
        "roster vehicle leaves, the spotted vehicle remains");

    visible.health = 700;
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &visible,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED) ==
            WOTBMOD_V3_OK,
        "visible health update is accepted");
    Check(
        ActiveEntityCount() == 1u,
        "visible update does not duplicate registry entry");
    event = LastEvent();
    Check(
        event &&
            std::strcmp(
                event->topic,
                WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED) == 0 &&
            event->lifecycle.snapshot.health == 700,
        "visible update publishes sanitized snapshot");

    Check(
        WotbModV3NativeBindings_ObserveRpcMetadata(
            WOTBMOD_V3_RPC_INCOMING,
            101u,
            "Vehicle",
            "Vehicle.set_health") == WOTBMOD_V3_OK,
        "visible incoming RPC metadata is observed");
    Check(
        g_rpc_count == 1u &&
            g_last_rpc.public_entity_id == 101u &&
            std::strcmp(
                g_last_rpc.method_name,
                "Vehicle.set_health") == 0,
        "RPC observation contains metadata only");
    Check(
        WotbModV3NativeBindings_ObserveRpcMetadata(
            WOTBMOD_V3_RPC_OUTGOING,
            101u,
            "Vehicle",
            "Vehicle.request_fire") ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "outgoing RPC metadata is rejected without a native source");
    Check(
        g_rpc_count == 1u,
        "unsupported outgoing RPC publishes no callback");

    Check(
        WotbModV3NativeBindings_RemovePublicVehicle(
            101u,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN) ==
            WOTBMOD_V3_OK,
        "unspot removes public vehicle");
    Check(
        ActiveEntityCount() == 0u,
        "unspot cleans public registry");
    event = LastEvent();
    Check(
        event &&
            std::strcmp(
                event->topic,
                WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED) == 0 &&
            event->lifecycle.snapshot.visible_to_player == 0u,
        "unspot publishes removal without live visibility");
    Check(
        WotbModV3NativeBindings_ObserveRpcMetadata(
            WOTBMOD_V3_RPC_INCOMING,
            101u,
            "Vehicle",
            "Vehicle.set_health") == WOTBMOD_V3_E_NOT_FOUND,
        "hidden vehicle RPC metadata is rejected");
    Check(
        g_rpc_count == 1u,
        "hidden vehicle triggers no RPC callback");

    WotbModV3NativePublicVehicle local = Vehicle(
        201u,
        1000,
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_LOCAL,
        reinterpret_cast<void*>(0x2010u));
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &local,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_LOCAL_PLAYER) ==
            WOTBMOD_V3_OK,
        "local vehicle is public without spotted flag");
    MockEntity* local_entity = nullptr;
    for (MockEntity& candidate : g_entities) {
        if (candidate.active &&
            candidate.snapshot.public_id == 201u) {
            local_entity = &candidate;
            break;
        }
    }
    Check(
        local_entity &&
            local_entity->snapshot.local_player == 1u &&
            local_entity->snapshot.visible_to_player == 1u,
        "local vehicle snapshot is explicitly local and visible");

    // === shot ingress: the path that turns a fired shell into a projectile
    //
    // `ShowShootingDetour` calls three things with the same entity id -
    // `ObservePublicVehicleRpc`, `QueueGameplayEvent(SHOT_FIRED)` and
    // `WotbModV3NativeBindings_ObserveShot`. Live, the first two produce
    // events and the third produces nothing, so this asserts the third on the
    // same state the first two are already proven against above: vehicle 201
    // is registered, local, and visible.
    // WHAT THIS CAN AND CANNOT PROVE, WRITTEN DOWN SO NOBODY RE-DISCOVERS IT.
    //
    // Argument validation is reachable here and is asserted. The rest is not:
    // `ObserveShot` opens with `!g_state.created || gameplay_bridge_sources == 0
    // -> NOT_SUPPORTED`, and `created` is only set by
    // `WotbModV3NativeBindings_Create`, which outside the real client always
    // lands on `COMPATIBILITY_HASH_MISMATCH` and forces the source mask back to
    // zero. So the ingress path is fail-closed behind state that only a matching
    // game client can produce, and no offline binary can walk past this gate.
    //
    // That is exactly why the path shipped broken. Live on 11.19.0.834 the
    // showShooting detour calls three things with one entity id -
    // `ObservePublicVehicleRpc`, `QueueGameplayEvent(SHOT_FIRED)` and
    // `ObserveShot` - and only the third produces nothing: four shots in a
    // training battle gave four `gameplay.shot_fired` and zero projectile
    // events, and the whole trace history holds 68 shots, 105 hits and not one
    // `wotbmod.projectile.*`. Diagnosing which gate refuses needs a live run;
    // `ObserveShot` now logs the refusal for that purpose.
    Check(
        WotbModV3NativeBindings_ObserveShot(0u, 7u) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "a shot with no entity id is refused before any state is consulted");
    Check(
        wotbmod::v3::g_projectile_register_calls == 0u &&
            wotbmod::v3::ActiveProjectileCount() == 0u,
        "a refused shot registers no projectile");
    Check(
        WotbModV3NativeBindings_ObserveShot(201u, 7u) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "shot ingress stays fail-closed until a matching client arms it");

    WotbModV3NativePublicVehicle second = Vehicle(
        202u,
        500,
        WOTBMOD_V3_NATIVE_PUBLIC_VEHICLE_VISIBLE,
        reinterpret_cast<void*>(0x2020u));
    Check(
        WotbModV3NativeBindings_UpsertPublicVehicle(
            &second,
            WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE) ==
            WOTBMOD_V3_OK,
        "second public vehicle registers");
    WotbModV3NativeBindings_ResetPublicGameplayState(
        WOTBMOD_V3_PUBLIC_ENTITY_REASON_SHUTDOWN);
    Check(
        ActiveEntityCount() == 0u,
        "battle/shutdown reset removes every public entity");

    char executable[MAX_PATH] = {};
    char temporary[MAX_PATH] = {};
    const DWORD executable_length = GetModuleFileNameA(
        nullptr, executable, static_cast<DWORD>(sizeof(executable)));
    const DWORD temporary_length = GetTempPathA(
        static_cast<DWORD>(sizeof(temporary)), temporary);
    const std::string report_path =
        std::string(temporary) +
        "wotbmod-binding-validation-test.json";
    WotbModV3NativeBindingsOptions create_options = {};
    create_options.struct_size = sizeof(create_options);
    create_options.game_module = GetModuleHandleA(nullptr);
    create_options.game_executable_path = executable;
    create_options.binding_validation_report_path =
        report_path.c_str();
    WotbModRuntimeV3ClientBackend backend = {};
    Check(
        executable_length != 0u &&
            executable_length < sizeof(executable) &&
            temporary_length != 0u &&
            temporary_length < sizeof(temporary) &&
            WotbModV3NativeBindings_Create(
                &create_options, &backend) == WOTBMOD_V3_OK &&
            backend.compatibility_state ==
                WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH,
        "updated-client mismatch creates a fail-closed binding backend");
    std::ifstream report(report_path, std::ios::binary);
    const std::string report_json(
        (std::istreambuf_iterator<char>(report)),
        std::istreambuf_iterator<char>());
    Check(
        report_json.find(
            "\"compatibility\": \"hash_mismatch\"") !=
                std::string::npos &&
            report_json.find(
                "\"mandatory_failed_count\": 11") !=
                std::string::npos &&
            report_json.find(
                "\"state\":\"not_checked\"") !=
                std::string::npos,
        "client update writes a complete machine-readable binding verdict");
    WotbModV3NativeBindings_Shutdown();
    DeleteFileA(report_path.c_str());

    std::printf(
        "V3 safe gameplay bridge: %u passed, %u failed\n",
        g_passes,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
