#include "wotb_mod_v3_internal.h"
#include "client_services_backend.h"
#include "ges_services.h"
#include "session_cluster_services.h"
#include "data_services_backend.h"

#include "../../include/wotbmod/audio_v2.h"
#include "../../include/wotbmod/audio_v3.h"
#include "../../include/wotbmod/bigworld_rpc_v1.h"
#include "../../include/wotbmod/camera_v1.h"
#include "../../include/wotbmod/camera_v2.h"
#include "../../include/wotbmod/client_v1.h"
#include "../../include/wotbmod/device_v1.h"
#include "../../include/wotbmod/entity_public_v1.h"
#include "../../include/wotbmod/gameplay_camera_v1.h"
#include "../../include/wotbmod/gameplay_hangar_v1.h"
#include "../../include/wotbmod/gameplay_hud_v1.h"
#include "../../include/wotbmod/gameplay_replay_v1.h"
#include "../../include/wotbmod/interface_ids.h"
#include "../../include/wotbmod/projectile_v2.h"
#include "../../include/wotbmod/render_v1.h"
#include "../../include/wotbmod/scene_v1.h"
#include "../../include/wotbmod/scene_v2.h"
#include "../../include/wotbmod/tracer_v1.h"
#include "../../include/wotbmod/ui_v3.h"
#include "../../include/wotbmod/ui_v4.h"
#include "../../include/wotbmod/vehicle_visual_v2.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dxgi1_2.h>
#include <intrin.h>
#include <winternl.h>
#endif

namespace wotbmod {
namespace v3 {
namespace {

constexpr uint32_t kUiMagic = 0x55494333u;
constexpr uint32_t kUiSlotMagic = 0x55495333u;
constexpr uint32_t kStyleMagic = 0x53545933u;
constexpr uint32_t kSceneMagic = 0x53434E33u;
constexpr uint32_t kAudioMagic = 0x41554433u;
constexpr uint32_t kRenderMagic = 0x524E4433u;
constexpr uint32_t kSubscriptionMagic = 0x53554233u;
constexpr uint32_t kCameraMagic = 0x43414D33u;
constexpr uint32_t kCameraEffectOwnerMagic = 0x43454633u;
constexpr uint32_t kTracerStyleMagic = 0x54524333u;
constexpr uint32_t kImpactVisualMagic = 0x49565033u;
constexpr uint32_t kVehicleProfileMagic = 0x56505233u;
constexpr uint32_t kMaxGraphDepth = 256u;
constexpr uint32_t kMaxTextureDimension = 16384u;
constexpr uint32_t kMaxUiChildren = 4096u;

std::mutex g_clientMutex;
std::mutex g_clientFramePumpMutex;
std::mutex g_uiCaptureMutex;
std::mutex g_hostBackendTransitionMutex;
std::condition_variable g_hostBackendQuiesced;
double g_lastDeltaSeconds = 0.0;
ClientHostBackend g_hostBackend = {};
ClientHostFrame g_hostFrame = {};
bool g_hostBackendInstalled = false;
bool g_clientServicesRunning = false;
bool g_hostBackendAcceptingInvocations = false;
bool g_hostFrameValid = false;
uint64_t g_hostBackendGeneration = 1u;
uint32_t g_hostBackendInvocationsInFlight = 0u;
thread_local uint32_t g_hostBackendInvocationDepth = 0u;

/*
 * Declared-2026-08-16 backend (ui.read, camera.state, audio.intercept,
 * scene.enumerate, tracer). Deliberately a separate table, mutex and
 * in-flight counter from ClientHostBackend: the sound detour reaches
 * DispatchClientHostSoundIntercept on an arbitrary engine thread, and it
 * must not be able to wait behind whatever the generic host bridge is doing.
 */
std::mutex g_declaredBackendMutex;
std::mutex g_declaredBackendTransitionMutex;
std::condition_variable g_declaredBackendQuiesced;
ClientHostDeclaredBackend g_declaredBackend = {};
bool g_declaredBackendInstalled = false;
bool g_declaredBackendAcceptingInvocations = false;
uint32_t g_declaredBackendInvocationsInFlight = 0u;
bool g_soundInterceptArmed = false;
thread_local uint32_t g_declaredBackendInvocationDepth = 0u;
/*
 * Non-zero while THIS thread is inside a sound-interception callback.
 * audio_v3.h makes this observable through is_intercept_active and requires
 * every other audio operation to answer WOTBMOD_V3_E_BUSY while it is set:
 * the hybrid sound system calls the inner system's CreateSoundEvent at
 * 0x024F3BA2, so a mod that touched audio from its callback would re-enter
 * the detour on the same thread.
 */
thread_local uint32_t g_soundInterceptDepth = 0u;
std::atomic<bool> g_acceptingHostReleaseRetries{false};
std::atomic<uint64_t> g_uiViewportSize{0u};
thread_local uint64_t g_lastExternalFrame =
    std::numeric_limits<uint64_t>::max();

struct RenderLifecycleState {
    ClientHostFrame frame = {};
    bool observed = false;
    bool everAvailable = false;
};

RenderLifecycleState g_renderLifecycle = {};

struct HostPublicEntityRecord {
    uint64_t nativeToken = 0u;
    uint64_t validFields = 0u;
    WotbModV3PublicEntitySnapshot snapshot = {};
    ClientHostPublicEntityExtras extras = {};
};

struct HostProjectileRecord {
    uint64_t nativeToken = 0u;
    uint64_t visualSceneObject = 0u;
    uint32_t ownerScope = WOTBMOD_V3_PROJECTILE_OWNER_UNKNOWN;
    uint32_t shellType = 0u;
    WotbModV3Vec3 visiblePosition = {};
    WotbModV3Vec3 visibleDirection = {};
    uint64_t sequenceId = 0u;
    uint64_t validFields = 0u;
    uint64_t timestampMicroseconds = 0u;
    uint32_t lifecycleState = WOTBMOD_V3_PROJECTILE_STATE_CREATED;
    uint32_t source = WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT;
    uint32_t nativeShotId = 0u;
    uint32_t primaryEntityId = 0u;
    uint32_t secondaryEntityId = 0u;
    uint32_t nativeFlags = 0u;
    uint32_t stockShotCode = 0u;
    WotbModV3Vec3 origin = {};
    WotbModV3Vec3 impactPosition = {};
};

struct ImpactVisualObject;

struct ManagedImpactInstance {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle visual = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3SceneHandle entity = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3SceneHandle activeScene = WOTBMOD_V3_INVALID_HANDLE;
    double remainingSeconds = 0.0;
};

struct PendingUiInput {
    uint32_t phase = 0u;
    WotbModV3Vec2 pointer = {};
    WotbModV3Vec2 delta = {};
    uint32_t modifiers = 0u;
};

struct AudioLifecycleState;

struct HostAudioRecord {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3AudioHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    std::shared_ptr<AudioLifecycleState> lifecycleState;
};

struct PendingHostRelease {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    std::string operation;
    uint64_t object = 0u;
    uint64_t backendGeneration = 0u;
};

struct SubscriptionOwnerPublicationState {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t epoch = 1u;
    bool stopping = false;
};

constexpr size_t kMaxSubscriptionOwnerStates = 128u;
std::array<
    SubscriptionOwnerPublicationState,
    kMaxSubscriptionOwnerStates> g_subscriptionOwnerStates = {};
using AudioLifecycleTestHook = void (*)();
std::atomic<AudioLifecycleTestHook>
    g_audioLifecycleBeforeStateUpdateHook{nullptr};

std::unordered_map<WotbModV3EntityHandle, HostPublicEntityRecord>
    g_hostEntities;
std::unordered_map<uint64_t, WotbModV3EntityHandle>
    g_hostEntityTokens;
std::unordered_map<WotbModV3ProjectileHandle, HostProjectileRecord>
    g_hostProjectiles;
std::unordered_map<uint64_t, WotbModV3ProjectileHandle>
    g_hostProjectileTokens;
std::unordered_map<WotbModV3Handle, ImpactVisualObject*>
    g_impactVisuals;
std::vector<ManagedImpactInstance> g_impactInstances;
std::vector<PendingUiInput> g_pendingUiInputs;
std::vector<WotbModV3Rect> g_uiCaptureRects;
std::unordered_map<WotbModV3Handle, WotbModV3UiHandle>
    g_uiHoveredControls;
std::unordered_map<WotbModV3Handle, WotbModV3UiHandle>
    g_uiPressedControls;
std::unordered_map<WotbModV3Handle, uint32_t> g_uiDragging;
WotbModV3Vec2 g_lastUiPointer = {};
bool g_haveLastUiPointer = false;
std::unordered_map<uint64_t, HostAudioRecord> g_hostAudioObjects;
std::mutex g_pendingHostReleaseMutex;
std::vector<PendingHostRelease> g_pendingHostReleases;
std::atomic<uint64_t> g_nextHostEntityHandle{1u};
std::atomic<uint64_t> g_nextHostProjectileHandle{1u};

bool IsFinite(float value) {
    return std::isfinite(static_cast<double>(value)) != 0;
}

bool IsFinite(double value) {
    return std::isfinite(value) != 0;
}

bool IsFinite(const WotbModV3Vec2& value) {
    return IsFinite(value.x) && IsFinite(value.y);
}

bool IsFinite(const WotbModV3Vec3& value) {
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
}

bool IsFinite(const WotbModV3Vec4& value) {
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z) &&
           IsFinite(value.w);
}

bool IsFinite(const WotbModV3Color& value) {
    return IsFinite(value.r) && IsFinite(value.g) && IsFinite(value.b) &&
           IsFinite(value.a);
}

bool IsFinite(const WotbModV3Rect& value) {
    return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.width) &&
           IsFinite(value.height);
}

bool IsFinite(const WotbModV3Transform& value) {
    return IsFinite(value.position) && IsFinite(value.rotation) &&
           IsFinite(value.scale);
}

bool ValidText(const char* value, size_t limit, bool allowEmpty = true) {
    if (!value) {
        return false;
    }
    const size_t length = std::strlen(value);
    return length < limit && (allowEmpty || length != 0u);
}

template <typename T>
bool ValidStruct(const T* value, uint32_t version) {
    return value && value->struct_size >= sizeof(T) &&
           value->api_version == version;
}

WotbModV3Result Invalid(
    WotbModV3Handle mod,
    const char* message) {
    return SetError(mod, WOTBMOD_V3_E_INVALID_ARGUMENT, message);
}

WotbModV3Result NotFound(
    WotbModV3Handle mod,
    const char* message) {
    return SetError(mod, WOTBMOD_V3_E_NOT_FOUND, message);
}

WotbModV3Result CheckEitherNamedPermission(
    WotbModV3Handle mod,
    const char* firstName,
    uint32_t firstTier,
    const char* secondName,
    uint32_t secondTier) {
    const WotbModV3Result first =
        CheckNamedPermission(mod, firstName, firstTier);
    if (first == WOTBMOD_V3_OK) {
        return first;
    }
    return CheckNamedPermission(mod, secondName, secondTier);
}

WotbModV3Result NativeUnavailable(
    WotbModV3Handle mod,
    uint32_t permissionTier,
    uint64_t contexts,
    const char* capability,
    const char* operation) {
    const WotbModV3Result access =
        CheckAccess(mod, permissionTier, contexts, capability);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    std::string message(operation ? operation : "native operation");
    message += ": native client backend is not installed";
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        message.c_str(),
        "{\"backend\":\"missing\"}");
}

bool GetHostBackend(ClientHostBackend* outBackend) {
    if (!outBackend) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (!g_hostBackendInstalled) {
        return false;
    }
    *outBackend = g_hostBackend;
    return true;
}

void AdvanceNonzeroGeneration(uint64_t* generation) {
    if (!generation) {
        return;
    }
    ++*generation;
    if (*generation == 0u) {
        ++*generation;
    }
}

SubscriptionOwnerPublicationState*
FindSubscriptionOwnerStateLocked(WotbModV3Handle owner) {
    for (SubscriptionOwnerPublicationState& state :
         g_subscriptionOwnerStates) {
        if (state.owner == owner) {
            return &state;
        }
    }
    return nullptr;
}

SubscriptionOwnerPublicationState*
GetOrCreateSubscriptionOwnerStateLocked(WotbModV3Handle owner) {
    SubscriptionOwnerPublicationState* state =
        FindSubscriptionOwnerStateLocked(owner);
    if (state) {
        return state;
    }
    for (SubscriptionOwnerPublicationState& candidate :
         g_subscriptionOwnerStates) {
        if (candidate.owner == WOTBMOD_V3_INVALID_HANDLE) {
            candidate = {};
            candidate.owner = owner;
            return &candidate;
        }
    }
    return nullptr;
}

bool SetSubscriptionOwnerStoppingLocked(
    WotbModV3Handle owner,
    bool stopping) {
    SubscriptionOwnerPublicationState* state =
        GetOrCreateSubscriptionOwnerStateLocked(owner);
    if (!state) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.client-services",
            "subscription owner publication registry is full");
        return false;
    }
    if (stopping) {
        AdvanceNonzeroGeneration(&state->epoch);
    }
    state->stopping = stopping;
    return true;
}

void PruneStaleSubscriptionOwners() {
    std::array<
        SubscriptionOwnerPublicationState,
        kMaxSubscriptionOwnerStates> states = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        states = g_subscriptionOwnerStates;
    }
    for (const SubscriptionOwnerPublicationState& state : states) {
        if (state.owner == WOTBMOD_V3_INVALID_HANDLE ||
            CheckMod(state.owner) == WOTBMOD_V3_OK) {
            continue;
        }
        std::lock_guard<std::mutex> lock(g_clientMutex);
        SubscriptionOwnerPublicationState* current =
            FindSubscriptionOwnerStateLocked(state.owner);
        if (current && current->epoch == state.epoch) {
            *current = {};
        }
    }
}

bool CaptureSubscriptionPublicationEpoch(
    WotbModV3Handle owner,
    uint64_t* outEpoch) {
    if (!outEpoch) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    SubscriptionOwnerPublicationState* state =
        GetOrCreateSubscriptionOwnerStateLocked(owner);
    if (!g_clientServicesRunning || !state || state->stopping) {
        return false;
    }
    *outEpoch = state->epoch;
    return true;
}

WotbModV3Result BeginSubscriptionCreation(
    WotbModV3Handle owner,
    uint64_t* outEpoch) {
    if (CaptureSubscriptionPublicationEpoch(owner, outEpoch)) {
        return WOTBMOD_V3_OK;
    }
    return SetError(
        owner,
        WOTBMOD_V3_E_OBJECT_DESTROYED,
        "subscription owner is stopping");
}

bool CanPublishSubscriptionLocked(
    WotbModV3Handle owner,
    uint64_t epoch) {
    SubscriptionOwnerPublicationState* state =
        FindSubscriptionOwnerStateLocked(owner);
    return g_clientServicesRunning && state && epoch != 0u &&
           epoch == state->epoch && !state->stopping;
}

void ClientSubscriptionOwnerStopping(WotbModV3Handle owner) {
    PruneStaleSubscriptionOwners();
    std::lock_guard<std::mutex> lock(g_clientMutex);
    SetSubscriptionOwnerStoppingLocked(owner, true);
}

void ClientSubscriptionLifecycleChanged(
    WotbModV3Handle owner,
    uint32_t transition) {
    if (transition == LIFECYCLE_TRANSITION_PRELOAD ||
        transition == LIFECYCLE_TRANSITION_UNLOADING) {
        PruneStaleSubscriptionOwners();
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (transition == LIFECYCLE_TRANSITION_UNLOADING) {
        SetSubscriptionOwnerStoppingLocked(owner, true);
    } else if (
        transition == LIFECYCLE_TRANSITION_LOADED ||
        transition == LIFECYCLE_TRANSITION_ENABLED ||
        transition == LIFECYCLE_TRANSITION_DISABLED) {
        SetSubscriptionOwnerStoppingLocked(owner, false);
    }
}

class HostBackendInvocationLease final {
public:
    HostBackendInvocationLease(
        uint64_t expectedGeneration,
        bool allowStopped) {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (!g_hostBackendInstalled || !g_hostBackend.invoke ||
            (!allowStopped &&
             (!g_clientServicesRunning ||
              !g_hostBackendAcceptingInvocations))) {
            return;
        }
        if (expectedGeneration != 0u &&
            expectedGeneration != g_hostBackendGeneration) {
            staleGeneration_ = true;
            return;
        }
        backend_ = g_hostBackend;
        generation_ = g_hostBackendGeneration;
        ++g_hostBackendInvocationsInFlight;
        ++g_hostBackendInvocationDepth;
        acquired_ = true;
    }

    ~HostBackendInvocationLease() {
        if (!acquired_) {
            return;
        }
        if (g_hostBackendInvocationDepth > 0u) {
            --g_hostBackendInvocationDepth;
        }
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (g_hostBackendInvocationsInFlight > 0u) {
            --g_hostBackendInvocationsInFlight;
        }
        g_hostBackendQuiesced.notify_all();
    }

    HostBackendInvocationLease(
        const HostBackendInvocationLease&) = delete;
    HostBackendInvocationLease& operator=(
        const HostBackendInvocationLease&) = delete;

    explicit operator bool() const {
        return acquired_;
    }

    bool staleGeneration() const {
        return staleGeneration_;
    }

    const ClientHostBackend& backend() const {
        return backend_;
    }

    uint64_t generation() const {
        return generation_;
    }

private:
    ClientHostBackend backend_ = {};
    uint64_t generation_ = 0u;
    bool acquired_ = false;
    bool staleGeneration_ = false;
};

bool GetHostFrame(ClientHostFrame* outFrame) {
    if (!outFrame) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (!g_hostFrameValid) {
        return false;
    }
    *outFrame = g_hostFrame;
    return true;
}

WotbModV3Result InvokeHost(
    WotbModV3Handle mod,
    uint32_t permissionTier,
    uint64_t contexts,
    const char* operation,
    const void* request,
    uint32_t requestSize,
    void* response,
    uint32_t responseSize,
    uint64_t* outBackendGeneration = nullptr,
    uint64_t expectedBackendGeneration = 0u) {
    if (outBackendGeneration) {
        *outBackendGeneration = 0u;
    }
    const WotbModV3Result access =
        CheckAccess(mod, permissionTier, contexts, nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    HostBackendInvocationLease invocation(
        expectedBackendGeneration,
        false);
    if (!invocation) {
        if (invocation.staleGeneration()) {
            return SetError(
                mod,
                WOTBMOD_V3_E_OBJECT_DESTROYED,
                "native client object belongs to a replaced backend");
        }
        std::string message(operation ? operation : "native operation");
        message += ": native client backend is not installed";
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            message.c_str(),
            "{\"backend\":\"missing\"}");
    }
    if (outBackendGeneration) {
        *outBackendGeneration = invocation.generation();
    }
    try {
        return invocation.backend().invoke(
            invocation.backend().user_data,
            mod,
            operation,
            request,
            requestSize,
            response,
            responseSize);
    } catch (...) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CALLBACK_FAULT,
            "native client backend threw an exception");
    }
}

ClientHostObjectRequest MakeHostRequest() {
    ClientHostObjectRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return request;
}

ClientHostObjectResponse MakeHostResponse() {
    ClientHostObjectResponse response = {};
    response.struct_size = sizeof(response);
    response.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return response;
}

void QueuePendingHostRelease(
    WotbModV3Handle owner,
    const char* operation,
    uint64_t object,
    uint64_t backendGeneration = 0u) {
    if (!operation || operation[0] == '\0' || object == 0u) {
        return;
    }
    if (!g_acceptingHostReleaseRetries.load(
            std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::mutex> lock(
        g_pendingHostReleaseMutex);
    for (const PendingHostRelease& pending :
         g_pendingHostReleases) {
        if (pending.owner == owner &&
            pending.object == object &&
            pending.backendGeneration == backendGeneration &&
            pending.operation == operation) {
            return;
        }
    }
    if (g_pendingHostReleases.size() >= 4096u) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.client-services",
            "native host release retry queue is full");
        return;
    }
    try {
        g_pendingHostReleases.push_back(
            {owner, operation, object, backendGeneration});
    } catch (...) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.client-services",
            "failed to queue a native host release retry");
    }
}

WotbModV3Result TryReleaseHostObject(
    WotbModV3Handle owner,
    const char* operation,
    uint64_t object,
    uint64_t backendGeneration = 0u,
    bool allowStopped = false,
    bool* outStaleGeneration = nullptr,
    uint64_t* outInvokedBackendGeneration = nullptr) {
    if (outStaleGeneration) {
        *outStaleGeneration = false;
    }
    if (outInvokedBackendGeneration) {
        *outInvokedBackendGeneration = 0u;
    }
    if (!operation || operation[0] == '\0' || object == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    HostBackendInvocationLease invocation(
        backendGeneration,
        allowStopped);
    if (!invocation) {
        if (invocation.staleGeneration()) {
            if (outStaleGeneration) {
                *outStaleGeneration = true;
            }
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (outInvokedBackendGeneration) {
        *outInvokedBackendGeneration = invocation.generation();
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = object;
    try {
        return invocation.backend().invoke(
            invocation.backend().user_data,
            owner,
            operation,
            &request,
            sizeof(request),
            nullptr,
            0u);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

bool IsRetriableHostReleaseFailure(WotbModV3Result result) {
    switch (result) {
        case WOTBMOD_V3_E_BUSY:
        case WOTBMOD_V3_E_IO:
        case WOTBMOD_V3_E_CALLBACK_FAULT:
        case WOTBMOD_V3_E_PLATFORM:
        case WOTBMOD_V3_E_TIMEOUT:
            return true;
        default:
            return false;
    }
}

void QueueHostReleaseAfterFailure(
    WotbModV3Handle owner,
    const char* operation,
    uint64_t object,
    uint64_t backendGeneration,
    WotbModV3Result result) {
    if (!IsRetriableHostReleaseFailure(result)) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "v3.client-services",
            "terminal native host release failure was dropped");
        return;
    }
    QueuePendingHostRelease(
        owner,
        operation,
        object,
        backendGeneration);
}

void ReleaseHostObject(
    WotbModV3Handle owner,
    const char* operation,
    uint64_t object,
    uint64_t backendGeneration = 0u) {
    if (object == 0u) {
        return;
    }
    bool staleGeneration = false;
    uint64_t invokedBackendGeneration = 0u;
    const WotbModV3Result result = TryReleaseHostObject(
        owner,
        operation,
        object,
        backendGeneration,
        false,
        &staleGeneration,
        &invokedBackendGeneration);
    if (result == WOTBMOD_V3_OK) {
        return;
    }
    if (staleGeneration) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "v3.client-services",
            "stale native host object was not forwarded to a replacement backend");
        return;
    }
    QueueHostReleaseAfterFailure(
        owner,
        operation,
        object,
        backendGeneration != 0u
            ? backendGeneration
            : invokedBackendGeneration,
        result);
}

void RetryPendingHostReleases(bool allowStopped = false) {
    std::vector<PendingHostRelease> pending;
    {
        std::lock_guard<std::mutex> lock(
            g_pendingHostReleaseMutex);
        pending.swap(g_pendingHostReleases);
    }
    if (pending.empty()) {
        return;
    }
    for (const PendingHostRelease& release : pending) {
        bool staleGeneration = false;
        const WotbModV3Result result = TryReleaseHostObject(
            release.owner,
            release.operation.c_str(),
            release.object,
            release.backendGeneration,
            allowStopped,
            &staleGeneration);
        if (result == WOTBMOD_V3_OK) {
            continue;
        }
        if (staleGeneration) {
            RuntimeLog(
                WOTBMOD_V3_LOG_WARNING,
                "v3.client-services",
                "stale native host release retry was dropped");
            continue;
        }
        if (IsRetriableHostReleaseFailure(result)) {
            QueuePendingHostRelease(
                release.owner,
                release.operation.c_str(),
                release.object,
                release.backendGeneration);
        } else {
            RuntimeLog(
                WOTBMOD_V3_LOG_WARNING,
                "v3.client-services",
                "terminal native host release retry was dropped");
        }
    }
}

WotbModV3Result CopyString(
    WotbModV3Handle mod,
    const std::string& value,
    char* buffer,
    uint32_t* inoutSize) {
    if (!inoutSize) {
        return Invalid(mod, "output size pointer is null");
    }
    const size_t requiredSize = value.size() + 1u;
    if (requiredSize > std::numeric_limits<uint32_t>::max()) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "string exceeds ABI size limit");
    }
    const uint32_t required = static_cast<uint32_t>(requiredSize);
    if (!buffer || *inoutSize < required) {
        *inoutSize = required;
        return SetError(
            mod,
            WOTBMOD_V3_E_BUFFER_TOO_SMALL,
            "output buffer is too small");
    }
    std::memcpy(buffer, value.c_str(), required);
    *inoutSize = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ResolveVirtualFilePath(
    WotbModV3Handle mod,
    const char* uri,
    std::string* outPath) {
    if (!outPath || !ValidText(uri, WOTBMOD_V3_MAX_PATH, false)) {
        return Invalid(mod, "mod resource URI is invalid");
    }
    outPath->clear();
    constexpr const char* kModScheme = "mod://";
    constexpr size_t kModSchemeLength = 6u;
    constexpr const char* kGameScheme = "game://";
    constexpr size_t kGameSchemeLength = 7u;
    const bool isMod =
        std::strncmp(uri, kModScheme, kModSchemeLength) == 0;
    const bool isGame =
        std::strncmp(uri, kGameScheme, kGameSchemeLength) == 0;
    if (!isMod && !isGame) {
        return WOTBMOD_V3_OK;
    }
    uint32_t requiredSize = 0u;
    WotbModV3Result result = ResolveVfsUriPhysical(
        mod,
        uri,
        nullptr,
        &requiredSize);
    if (result != WOTBMOD_V3_E_BUFFER_TOO_SMALL ||
        requiredSize == 0u ||
        requiredSize > WOTBMOD_V3_MAX_PATH * 4u) {
        return result == WOTBMOD_V3_OK
            ? SetError(
                  mod,
                  WOTBMOD_V3_E_PLATFORM,
                  "VFS returned an invalid physical path size")
            : result;
    }
    std::vector<char> physicalPath(requiredSize);
    result = ResolveVfsUriPhysical(
        mod,
        uri,
        physicalPath.data(),
        &requiredSize);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    *outPath = physicalPath.data();
    return WOTBMOD_V3_OK;
}

struct UiControl {
    uint32_t magic = kUiMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle self = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
    bool gameOwned = false;
    uint32_t type = WOTBMOD_V3_UI_CONTROL_CONTAINER;
    std::string id;
    std::string text;
    std::string texture;
    std::string font;
    std::string localizationKey;
    std::string tooltip;
    std::string accessibilityLabel;
    WotbModV3UiHandle parent = WOTBMOD_V3_INVALID_HANDLE;
    std::vector<WotbModV3UiHandle> children;
    WotbModV3Vec2 position = {};
    WotbModV3Vec2 size = {};
    WotbModV3Vec2 anchor = {};
    WotbModV3Vec2 pivot = {};
    WotbModV3UiEdges margin = {};
    WotbModV3UiEdges padding = {};
    WotbModV3Vec2 minSize = {};
    WotbModV3Vec2 maxSize = {std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::max()};
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    WotbModV3Color backgroundColor = {0.10f, 0.12f, 0.16f, 0.92f};
    float opacity = 1.0f;
    float fontSize = 16.0f;
    int32_t zOrder = 0;
    uint32_t visible = 1;
    uint32_t enabled = 1;
    uint32_t interactable = 1;
    uint32_t focused = 0;
    uint32_t textAlignment = WOTBMOD_V3_UI_TEXT_ALIGN_LEFT;
    uint32_t textWrap = 0;
    uint32_t richText = 0;
    WotbModV3UiLayoutDescriptor layout = {};
    uint64_t layoutRevision = 0;
    double minimum = 0.0;
    double maximum = 1.0;
    double value = 0.0;
    uint32_t checked = 0;
    std::vector<WotbModV3UiChoiceDescriptor> choiceAbi;
    std::vector<std::string> choiceIds;
    std::vector<std::string> choiceLabels;
    std::vector<std::string> choiceValues;
    std::vector<WotbModV3Handle> styleOverrides;
};

struct UiSlot {
    uint32_t magic = kUiSlotMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle self = WOTBMOD_V3_INVALID_HANDLE;
    std::string id;
};

struct UiStyleOverride {
    uint32_t magic = kStyleMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle self = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t fields = 0;
    int32_t priority = 0;
    WotbModV3Color color = {};
    WotbModV3Color background = {};
    float opacity = 1.0f;
    float fontSize = 16.0f;
    int32_t zOrder = 0;
    std::string font;
    std::string texture;
    WotbModV3Color previousColor = {};
    WotbModV3Color previousBackground = {};
    float previousOpacity = 1.0f;
    float previousFontSize = 16.0f;
    int32_t previousZOrder = 0;
    std::string previousFont;
    std::string previousTexture;
    bool active = true;
};

enum class SubscriptionKind {
    UiEvent,
    AudioStarted,
    AudioFinished,
    AudioError,
    Render,
    CameraModifier,
    EntityProperty,
    RpcObserved,
    AudioOverride,
    /*
     * wotbmod.audio.intercept. It owns no native object: the detour lives in
     * the loader and the match set there is append-only, so there is nothing
     * to release and SubscriptionReleaseOperation deliberately returns null
     * for this kind. Unregistering removes the callback from this registry;
     * the name stays published and answers PASS_THROUGH.
     */
    AudioIntercept
};

struct SubscriptionCallbackState {
    std::recursive_mutex dispatchMutex;
    std::mutex mutex;
    std::condition_variable quiesced;
    bool active = true;
    uint32_t inFlight = 0u;
    uint64_t deactivationEpoch = 0u;
    std::unordered_map<std::thread::id, uint32_t> inFlightByThread;
    WotbModV3AudioLifecycleCallback audioLifecycleCallback = nullptr;
    WotbModV3UiEventCallback uiCallback = nullptr;
    WotbModV3AudioErrorCallback audioErrorCallback = nullptr;
    WotbModV3RenderCallback renderCallback = nullptr;
    WotbModV3CameraModifierCallback cameraModifierCallback = nullptr;
    WotbModV3PublicPropertyCallback entityPropertyCallback = nullptr;
    WotbModV3RpcObservedCallback rpcObservedCallback = nullptr;
    WotbModV3SoundInterceptCallback soundInterceptCallback = nullptr;
    void* userData = nullptr;
};

struct Subscription {
    uint32_t magic = kSubscriptionMagic;
    std::recursive_mutex operationMutex;
    bool unregistering = false;
    bool unregistered = false;
    bool destroying = false;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Token self = WOTBMOD_V3_INVALID_HANDLE;
    SubscriptionKind kind = SubscriptionKind::UiEvent;
    WotbModV3Handle target = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
    uint64_t backendGeneration = 0u;
    uint64_t publicationEpoch = 0u;
    uint32_t eventType = 0;
    uint32_t phase = 0;
    int32_t priority = 0;
    WotbModV3UiEventCallback uiCallback = nullptr;
    WotbModV3AudioLifecycleCallback audioLifecycleCallback = nullptr;
    WotbModV3AudioErrorCallback audioErrorCallback = nullptr;
    WotbModV3RenderCallback renderCallback = nullptr;
    WotbModV3CameraModifierCallback cameraModifierCallback = nullptr;
    WotbModV3PublicPropertyCallback entityPropertyCallback = nullptr;
    WotbModV3RpcObservedCallback rpcObservedCallback = nullptr;
    WotbModV3SoundInterceptCallback soundInterceptCallback = nullptr;
    /*
     * Monotonic creation order. Sound interception dispatches highest
     * priority first and breaks ties by this, so two mods that pick the same
     * priority get a deterministic order instead of whatever the hash map
     * happens to yield today.
     */
    uint64_t creationOrder = 0u;
    std::string property;
    std::string methodFilter;
    void* userData = nullptr;
    std::shared_ptr<SubscriptionCallbackState> callbackState;
};

struct SubscriptionHandleObject {
    std::shared_ptr<Subscription> subscription;
};

bool HasManagedCallback(SubscriptionKind kind) {
    switch (kind) {
        case SubscriptionKind::UiEvent:
        case SubscriptionKind::AudioStarted:
        case SubscriptionKind::AudioFinished:
        case SubscriptionKind::AudioError:
        case SubscriptionKind::Render:
        case SubscriptionKind::CameraModifier:
        case SubscriptionKind::EntityProperty:
        case SubscriptionKind::RpcObserved:
        case SubscriptionKind::AudioIntercept:
            return true;
        default:
            return false;
    }
}

class SubscriptionCallbackLease final {
public:
    explicit SubscriptionCallbackLease(
        const std::shared_ptr<SubscriptionCallbackState>& state)
        : state_(state) {
        if (!state_) {
            return;
        }
        dispatchLock_ =
            std::unique_lock<std::recursive_mutex>(
                state_->dispatchMutex);
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->active) {
            return;
        }
        const std::thread::id threadId =
            std::this_thread::get_id();
        try {
            ++state_->inFlightByThread[threadId];
        } catch (...) {
            return;
        }
        ++state_->inFlight;
        acquired_ = true;
    }

    ~SubscriptionCallbackLease() {
        if (!acquired_) {
            return;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        const std::thread::id threadId =
            std::this_thread::get_id();
        const auto found =
            state_->inFlightByThread.find(threadId);
        if (found != state_->inFlightByThread.end()) {
            if (found->second > 1u) {
                --found->second;
            } else {
                state_->inFlightByThread.erase(found);
            }
        }
        if (state_->inFlight > 0u) {
            --state_->inFlight;
        }
        state_->quiesced.notify_all();
    }

    SubscriptionCallbackLease(
        const SubscriptionCallbackLease&) = delete;
    SubscriptionCallbackLease& operator=(
        const SubscriptionCallbackLease&) = delete;

    explicit operator bool() const {
        return acquired_;
    }

private:
    std::shared_ptr<SubscriptionCallbackState> state_;
    std::unique_lock<std::recursive_mutex> dispatchLock_;
    bool acquired_ = false;
};

struct SubscriptionCallbackDeactivation {
    uint64_t epoch = 0u;
    bool changed = false;
};

SubscriptionCallbackDeactivation
DeactivateAndWaitForSubscriptionCallbacks(
    const std::shared_ptr<SubscriptionCallbackState>& state) {
    SubscriptionCallbackDeactivation result = {};
    if (!state) {
        return result;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    result.changed = state->active;
    if (result.changed) {
        state->active = false;
        ++state->deactivationEpoch;
        if (state->deactivationEpoch == 0u) {
            ++state->deactivationEpoch;
        }
    }
    result.epoch = state->deactivationEpoch;
    const auto current =
        state->inFlightByThread.find(std::this_thread::get_id());
    const uint32_t currentThreadFrames =
        current == state->inFlightByThread.end()
            ? 0u
            : current->second;
    state->quiesced.wait(
        lock,
        [&]() {
            return state->inFlight <= currentThreadFrames;
        });
    return result;
}

void ReactivateSubscriptionCallbacksAfterFailure(
    const std::shared_ptr<SubscriptionCallbackState>& state,
    const SubscriptionCallbackDeactivation& deactivation) {
    if (!state || !deactivation.changed ||
        deactivation.epoch == 0u) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->deactivationEpoch == deactivation.epoch) {
        state->active = true;
    }
}

struct CameraEffectOwner {
    uint32_t magic = kCameraEffectOwnerMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle self = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
};

std::unordered_map<WotbModV3UiHandle, UiControl*> g_uiControls;
std::unordered_map<WotbModV3UiHandle, UiSlot*> g_uiSlots;
std::unordered_map<
    WotbModV3Token,
    std::shared_ptr<Subscription>> g_subscriptions;
std::unordered_map<WotbModV3Handle, CameraEffectOwner*>
    g_cameraEffectOwners;

const char* const kStableUiSlots[] = {
    "hangar.top_bar.left",
    "hangar.top_bar.right",
    "hangar.vehicle_panel.actions",
    "battle.hud.top",
    "battle.hud.bottom",
    "battle.results.actions",
    "settings.mods"};

WotbModV3Result CheckUiOwnPermission(WotbModV3Handle mod) {
    return CheckNamedPermission(
        mod,
        "ui.modify.own",
        WOTBMOD_V3_PERMISSION_SAFE);
}

WotbModV3Result CheckUiSlotPermission(
    WotbModV3Handle mod,
    const char* slotId) {
    if (slotId && std::strncmp(slotId, "battle.", 7u) == 0) {
        return CheckEitherNamedPermission(
            mod,
            "battle.ui",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            "ui.modify.game",
            WOTBMOD_V3_PERMISSION_REVIEWED);
    }
    return CheckNamedPermission(
        mod,
        "ui.modify.game",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

WotbModV3Result CheckAnyUiSlotPermission(WotbModV3Handle mod) {
    return CheckEitherNamedPermission(
        mod,
        "ui.modify.game",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        "battle.ui",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

WotbModV3Result ReleaseUiControlStyles(UiControl* control) {
    if (!control) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    for (;;) {
        WotbModV3Handle style = WOTBMOD_V3_INVALID_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            if (control->styleOverrides.empty()) break;
            style = control->styleOverrides.back();
        }
        const WotbModV3Result result =
            ReleaseOwnedHandle(control->owner, style);
        if (result != WOTBMOD_V3_OK) return result;
    }
    return WOTBMOD_V3_OK;
}

void DestroyUiControl(void* object) {
    UiControl* control = static_cast<UiControl*>(object);
    if (!control) {
        return;
    }
    ReleaseUiControlStyles(control);
    ReleaseHostObject(
        control->owner,
        "ui_control_destroy",
        control->nativeObject);
    control->nativeObject = 0u;
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto parentIt = g_uiControls.find(control->parent);
    if (parentIt != g_uiControls.end()) {
        std::vector<WotbModV3UiHandle>& siblings = parentIt->second->children;
        siblings.erase(
            std::remove(siblings.begin(), siblings.end(), control->self),
            siblings.end());
    }
    for (WotbModV3UiHandle child : control->children) {
        const auto childIt = g_uiControls.find(child);
        if (childIt != g_uiControls.end() &&
            childIt->second->parent == control->self) {
            childIt->second->parent = WOTBMOD_V3_INVALID_HANDLE;
        }
    }
    g_uiControls.erase(control->self);
    control->magic = 0;
    delete control;
}

void DestroyUiSlot(void* object) {
    UiSlot* slot = static_cast<UiSlot*>(object);
    if (!slot) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    g_uiSlots.erase(slot->self);
    slot->magic = 0;
    delete slot;
}

const char* SubscriptionReleaseOperation(
    SubscriptionKind kind) {
    switch (kind) {
        case SubscriptionKind::AudioStarted:
        case SubscriptionKind::AudioFinished:
        case SubscriptionKind::AudioError:
            return "audio_unsubscribe";
        case SubscriptionKind::CameraModifier:
            return "camera_remove_modifier";
        case SubscriptionKind::EntityProperty:
            return "entity_unsubscribe_public_property";
        case SubscriptionKind::RpcObserved:
            return "rpc_unsubscribe_observed";
        case SubscriptionKind::AudioOverride:
            return "sound_override_unregister";
        default:
            return nullptr;
    }
}

void DeleteSubscriptionState(Subscription* subscription) {
    if (!subscription) {
        return;
    }
    const char* const operation =
        SubscriptionReleaseOperation(subscription->kind);
    if (operation && subscription->nativeObject != 0u) {
        QueuePendingHostRelease(
            subscription->owner,
            operation,
            subscription->nativeObject,
            subscription->backendGeneration);
        subscription->nativeObject = 0u;
    }
    subscription->magic = 0u;
    delete subscription;
}

void EraseSubscriptionRegistration(
    const std::shared_ptr<Subscription>& subscription);

void DestroySubscription(void* object) {
    SubscriptionHandleObject* holder =
        static_cast<SubscriptionHandleObject*>(object);
    if (!holder) {
        return;
    }
    const std::shared_ptr<Subscription> subscription =
        holder->subscription;
    delete holder;
    if (!subscription) {
        return;
    }
    bool ownsUnregister = false;
    {
        std::unique_lock<std::recursive_mutex> operationLock(
            subscription->operationMutex);
        subscription->destroying = true;
        if (!subscription->unregistering &&
            !subscription->unregistered) {
            subscription->unregistering = true;
            ownsUnregister = true;
        }
    }
    EraseSubscriptionRegistration(subscription);
    if (!ownsUnregister) {
        return;
    }
    DeactivateAndWaitForSubscriptionCallbacks(
        subscription->callbackState);
    const char* releaseOperation = nullptr;
    uint64_t nativeObject = 0u;
    uint64_t backendGeneration = 0u;
    {
        std::unique_lock<std::recursive_mutex> operationLock(
            subscription->operationMutex);
        releaseOperation =
            SubscriptionReleaseOperation(subscription->kind);
        nativeObject = subscription->nativeObject;
        backendGeneration = subscription->backendGeneration;
        subscription->nativeObject = 0u;
        subscription->unregistered = true;
        subscription->unregistering = false;
    }
    if (releaseOperation && nativeObject != 0u) {
        ReleaseHostObject(
            subscription->owner,
            releaseOperation,
            nativeObject,
            backendGeneration);
    }
}

void DestroyCameraEffectOwner(void* object) {
    CameraEffectOwner* effectOwner =
        static_cast<CameraEffectOwner*>(object);
    if (!effectOwner) {
        return;
    }
    if (effectOwner->nativeObject != 0u) {
        ReleaseHostObject(
            effectOwner->owner,
            "camera_cancel_effects",
            effectOwner->nativeObject);
        effectOwner->nativeObject = 0u;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto found =
            g_cameraEffectOwners.find(effectOwner->owner);
        if (found != g_cameraEffectOwners.end() &&
            found->second == effectOwner) {
            g_cameraEffectOwners.erase(found);
        }
    }
    effectOwner->magic = 0u;
    delete effectOwner;
}

WotbModV3Result TrackCameraEffectOwner(
    WotbModV3Handle mod,
    uint64_t nativeObject) {
    if (nativeObject == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "camera backend returned an invalid effect controller");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto existing = g_cameraEffectOwners.find(mod);
    if (existing != g_cameraEffectOwners.end()) {
        CameraEffectOwner* effectOwner = existing->second;
        if (!effectOwner ||
            effectOwner->magic != kCameraEffectOwnerMagic) {
            return SetError(
                mod,
                WOTBMOD_V3_E_INVALID_HANDLE,
                "camera effect owner state is invalid");
        }
        effectOwner->nativeObject = nativeObject;
        return WOTBMOD_V3_OK;
    }
    CameraEffectOwner* effectOwner =
        new (std::nothrow) CameraEffectOwner();
    if (!effectOwner) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "camera effect owner allocation failed");
    }
    effectOwner->owner = mod;
    effectOwner->nativeObject = nativeObject;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SUBSCRIPTION,
        effectOwner,
        &DestroyCameraEffectOwner,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        effectOwner->magic = 0u;
        delete effectOwner;
        return result;
    }
    effectOwner->self = handle;
    g_cameraEffectOwners[mod] = effectOwner;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetUiControl(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    UiControl** outControl) {
    if (!outControl) {
        return Invalid(mod, "UI control output pointer is null");
    }
    const WotbModV3Result modResult = CheckMod(mod);
    if (modResult != WOTBMOD_V3_OK) {
        return modResult;
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_UI_CONTROL,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiControl* control = static_cast<UiControl*>(object);
    if (!control || control->magic != kUiMagic) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not a V3 UI control");
    }
    const WotbModV3Result permission = control->gameOwned
        ? CheckNamedPermission(
              mod,
              "ui.modify.game",
              WOTBMOD_V3_PERMISSION_REVIEWED)
        : CheckUiOwnPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    *outControl = control;
    return WOTBMOD_V3_OK;
}

/* The public control type of a stock DAVA class, from its RTTI name. */
uint32_t UiTypeFromClassName(const char* className) {
    if (!className || !className[0]) return WOTBMOD_V3_UI_CONTROL_CONTAINER;
    if (std::strstr(className, "UIStaticText")) return WOTBMOD_V3_UI_CONTROL_TEXT;
    if (std::strstr(className, "UITextField")) return WOTBMOD_V3_UI_CONTROL_TEXT_INPUT;
    if (std::strstr(className, "UIButton")) return WOTBMOD_V3_UI_CONTROL_BUTTON;
    if (std::strstr(className, "UISwitch")) return WOTBMOD_V3_UI_CONTROL_CHECKBOX;
    if (std::strstr(className, "UISlider")) return WOTBMOD_V3_UI_CONTROL_SLIDER;
    if (std::strstr(className, "UIScrollView")) return WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW;
    if (std::strstr(className, "UIList")) return WOTBMOD_V3_UI_CONTROL_LIST;
    return WOTBMOD_V3_UI_CONTROL_CONTAINER;
}

WotbModV3Result WrapHostUiControl(
    WotbModV3Handle mod,
    const ClientHostObjectResponse& response,
    bool gameOwned,
    WotbModV3UiHandle parent,
    const char* id,
    WotbModV3UiHandle* outControl) {
    if (!outControl || response.object == 0u ||
        !IsFinite(response.rect) || response.rect.width < 0.0f ||
        response.rect.height < 0.0f) {
        return Invalid(mod, "native UI response is invalid");
    }
    UiControl* control = new (std::nothrow) UiControl();
    if (!control) {
        ReleaseHostObject(mod, "ui_control_destroy", response.object);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI wrapper allocation failed");
    }
    control->owner = mod;
    control->nativeObject = response.object;
    control->gameOwned = gameOwned;
    control->type = UiTypeFromClassName(response.class_name);
    control->parent = parent;
    control->position = {response.rect.x, response.rect.y};
    control->size = {response.rect.width, response.rect.height};
    control->visible = (response.value_u32 & 1u) != 0u ? 1u : 0u;
    control->interactable =
        (response.value_u32 & 2u) != 0u ? 1u : 0u;
    control->enabled =
        (response.value_u32 & 4u) != 0u ? 1u : 0u;
    /* A game-owned control is named by the engine: the DAVA name the loader
     * read wins over a caller-supplied placeholder; an unnamed control keeps
     * the placeholder ("active_screen") or stays empty. */
    control->id = (gameOwned && response.name[0] != '\0')
        ? response.name
        : (id ? id : "");
    WOTBMOD_V3_INIT_STRUCT(
        control->layout,
        WOTBMOD_V3_UI_VERSION);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_UI_CONTROL,
        control,
        &DestroyUiControl,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(mod, "ui_control_destroy", response.object);
        control->nativeObject = 0u;
        delete control;
        return result;
    }
    control->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_uiControls[handle] = control;
        const auto parentIt = g_uiControls.find(parent);
        if (parentIt != g_uiControls.end()) {
            parentIt->second->children.push_back(handle);
        }
    }
    *outControl = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result RefreshUiControlFromHost(
    WotbModV3Handle mod,
    UiControl* control) {
    if (!control || control->nativeObject == 0u) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    ClientHostObjectResponse response = MakeHostResponse();
    const uint32_t tier = control->gameOwned
        ? WOTBMOD_V3_PERMISSION_REVIEWED
        : WOTBMOD_V3_PERMISSION_SAFE;
    const WotbModV3Result result = InvokeHost(
        mod,
        tier,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_get_state",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!IsFinite(response.rect) || response.rect.width < 0.0f ||
        response.rect.height < 0.0f) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "native UI state is invalid");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->position = {response.rect.x, response.rect.y};
    control->size = {response.rect.width, response.rect.height};
    control->visible = (response.value_u32 & 1u) != 0u ? 1u : 0u;
    control->interactable =
        (response.value_u32 & 2u) != 0u ? 1u : 0u;
    control->enabled =
        (response.value_u32 & 4u) != 0u ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetUiSlot(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    UiSlot** outSlot) {
    const WotbModV3Result permission = CheckAnyUiSlotPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outSlot) {
        return Invalid(mod, "UI slot output pointer is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_UI_SLOT,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiSlot* slot = static_cast<UiSlot*>(object);
    if (!slot || slot->magic != kUiSlotMagic) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not a V3 UI slot");
    }
    *outSlot = slot;
    return WOTBMOD_V3_OK;
}

WotbModV3Result CreateSubscription(
    WotbModV3Handle mod,
    Subscription* subscription,
    WotbModV3Token* outToken) {
    if (!subscription || !outToken) {
        delete subscription;
        return Invalid(mod, "subscription arguments are invalid");
    }
    std::shared_ptr<Subscription> sharedSubscription;
    try {
        sharedSubscription =
            std::shared_ptr<Subscription>(
                subscription,
                &DeleteSubscriptionState);
    } catch (...) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "subscription ownership allocation failed");
    }
    subscription = nullptr;
    sharedSubscription->owner = mod;
    if (sharedSubscription->publicationEpoch == 0u &&
        !CaptureSubscriptionPublicationEpoch(
            mod,
            &sharedSubscription->publicationEpoch)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "subscription owner is stopping");
    }
    if (HasManagedCallback(sharedSubscription->kind)) {
        try {
            sharedSubscription->callbackState =
                std::make_shared<SubscriptionCallbackState>();
        } catch (...) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "callback subscription state allocation failed");
        }
        SubscriptionCallbackState& state =
            *sharedSubscription->callbackState;
        state.uiCallback = sharedSubscription->uiCallback;
        state.audioLifecycleCallback =
            sharedSubscription->audioLifecycleCallback;
        state.audioErrorCallback =
            sharedSubscription->audioErrorCallback;
        state.renderCallback = sharedSubscription->renderCallback;
        state.cameraModifierCallback =
            sharedSubscription->cameraModifierCallback;
        state.entityPropertyCallback =
            sharedSubscription->entityPropertyCallback;
        state.rpcObservedCallback =
            sharedSubscription->rpcObservedCallback;
        state.soundInterceptCallback =
            sharedSubscription->soundInterceptCallback;
        state.userData = sharedSubscription->userData;
    }
    SubscriptionHandleObject* holder =
        new (std::nothrow) SubscriptionHandleObject();
    if (!holder) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "subscription handle allocation failed");
    }
    holder->subscription = sharedSubscription;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SUBSCRIPTION,
        holder,
        &DestroySubscription,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        DestroySubscription(holder);
        if (result == WOTBMOD_V3_E_CANCELLED ||
            result == WOTBMOD_V3_E_INVALID_HANDLE) {
            return SetError(
                mod,
                WOTBMOD_V3_E_OBJECT_DESTROYED,
                "subscription owner was destroyed during creation");
        }
        return result;
    }
    bool published = false;
    {
        std::unique_lock<std::recursive_mutex> operationLock(
            sharedSubscription->operationMutex);
        sharedSubscription->self = handle;
        if (!sharedSubscription->destroying &&
            !sharedSubscription->unregistered) {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            if (CanPublishSubscriptionLocked(
                    mod,
                    sharedSubscription->publicationEpoch)) {
                g_subscriptions[handle] = sharedSubscription;
                published = true;
            }
        }
    }
    if (!published) {
        const WotbModV3Result releaseResult =
            ReleaseOwnedHandle(mod, handle);
        if (releaseResult != WOTBMOD_V3_OK &&
            releaseResult != WOTBMOD_V3_E_INVALID_HANDLE) {
            RuntimeLog(
                WOTBMOD_V3_LOG_WARNING,
                "v3.client-services",
                "failed to release an unpublished subscription");
        }
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "subscription owner was destroyed during creation");
    }
    *outToken = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetSubscription(
    WotbModV3Handle mod,
    WotbModV3Token token,
    SubscriptionKind kind,
    std::shared_ptr<Subscription>* outSubscription) {
    if (!outSubscription) {
        return Invalid(mod, "subscription output pointer is null");
    }
    std::shared_ptr<Subscription> subscription;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto found = g_subscriptions.find(token);
        if (found != g_subscriptions.end()) {
            subscription = found->second;
        }
    }
    if (!subscription || subscription->magic != kSubscriptionMagic ||
        subscription->owner != mod || subscription->kind != kind) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "subscription has a different service type");
    }
    *outSubscription = subscription;
    return WOTBMOD_V3_OK;
}

void EraseSubscriptionRegistration(
    const std::shared_ptr<Subscription>& subscription) {
    if (!subscription) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto found = g_subscriptions.find(subscription->self);
    if (found != g_subscriptions.end() &&
        found->second == subscription) {
        g_subscriptions.erase(found);
    }
}

WotbModV3Result UnregisterSubscription(
    WotbModV3Handle mod,
    WotbModV3Token token,
    const std::shared_ptr<Subscription>& subscription,
    uint32_t permissionTier,
    uint64_t contexts,
    const char* nativeOperation) {
    if (!subscription) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "subscription is invalid");
    }
    if (nativeOperation) {
        const WotbModV3Result access =
            CheckAccess(mod, permissionTier, contexts, nullptr);
        if (access != WOTBMOD_V3_OK) {
            return access;
        }
    }
    uint64_t nativeObject = 0u;
    uint64_t backendGeneration = 0u;
    {
        std::unique_lock<std::recursive_mutex> operationLock(
            subscription->operationMutex);
        if (subscription->destroying ||
            subscription->unregistered) {
            return SetError(
                mod,
                WOTBMOD_V3_E_OBJECT_DESTROYED,
                "subscription is already being destroyed");
        }
        if (subscription->unregistering) {
            return SetError(
                mod,
                WOTBMOD_V3_E_OBJECT_DESTROYED,
                "subscription unregister is already in progress");
        }
        subscription->unregistering = true;
        nativeObject = subscription->nativeObject;
        backendGeneration = subscription->backendGeneration;
    }
    const SubscriptionCallbackDeactivation deactivation =
        DeactivateAndWaitForSubscriptionCallbacks(
            subscription->callbackState);
    WotbModV3Result nativeResult = WOTBMOD_V3_OK;
    bool staleGeneration = false;
    if (nativeOperation && nativeObject != 0u) {
        nativeResult = TryReleaseHostObject(
            mod,
            nativeOperation,
            nativeObject,
            backendGeneration,
            false,
            &staleGeneration);
        if (staleGeneration) {
            nativeResult = WOTBMOD_V3_OK;
        }
    }
    bool destroyedDuringUnregister = false;
    {
        std::unique_lock<std::recursive_mutex> operationLock(
            subscription->operationMutex);
        if (subscription->destroying) {
            destroyedDuringUnregister = true;
            subscription->nativeObject = 0u;
            subscription->unregistered = true;
            subscription->unregistering = false;
        } else if (nativeResult != WOTBMOD_V3_OK) {
            ReactivateSubscriptionCallbacksAfterFailure(
                subscription->callbackState,
                deactivation);
            subscription->unregistering = false;
        } else {
            subscription->nativeObject = 0u;
            subscription->unregistered = true;
            subscription->unregistering = false;
        }
    }
    if (destroyedDuringUnregister) {
        if (nativeResult != WOTBMOD_V3_OK &&
            nativeOperation && nativeObject != 0u) {
            QueueHostReleaseAfterFailure(
                mod,
                nativeOperation,
                nativeObject,
                backendGeneration,
                nativeResult);
        }
        EraseSubscriptionRegistration(subscription);
        return nativeResult;
    }
    if (nativeResult != WOTBMOD_V3_OK) {
        return nativeResult;
    }

    EraseSubscriptionRegistration(subscription);
    const WotbModV3Result releaseResult =
        ReleaseOwnedHandle(mod, token);
    return releaseResult == WOTBMOD_V3_E_INVALID_HANDLE
        ? WOTBMOD_V3_OK
        : releaseResult;
}

WotbModV3Result UiControlCreateInternal(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl);

WotbModV3Result WOTBMOD_V3_CALL UiControlCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl) {
    const WotbModV3Result namedPermission = CheckNamedPermission(
        mod,
        "ui.create",
        WOTBMOD_V3_PERMISSION_SAFE);
    if (namedPermission != WOTBMOD_V3_OK) {
        return namedPermission;
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    return UiControlCreateInternal(mod, descriptor, outControl);
}

/* The creation proper, behind the caller-facing gate: the stock HUD backend
 * builds its texture overlays here under its own gameplay.tweak.hud grant. */
WotbModV3Result UiControlCreateInternal(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl) {
    if (!ValidStruct(descriptor, WOTBMOD_V3_UI_VERSION) || !outControl ||
        descriptor->type > WOTBMOD_V3_UI_CONTROL_TABS ||
        !IsFinite(descriptor->geometry) ||
        descriptor->geometry.width < 0.0f ||
        descriptor->geometry.height < 0.0f ||
        (descriptor->id &&
         !ValidText(descriptor->id, WOTBMOD_V3_MAX_ID)) ||
        (descriptor->text &&
         !ValidText(descriptor->text, WOTBMOD_V3_MAX_MESSAGE)) ||
        (descriptor->texture_uri &&
         !ValidText(descriptor->texture_uri, WOTBMOD_V3_MAX_PATH))) {
        return Invalid(mod, "invalid UI control descriptor");
    }
    std::string resolvedTemplatePath;
    const bool templateRequested =
        descriptor->type == WOTBMOD_V3_UI_CONTROL_CONTAINER &&
        descriptor->texture_uri && descriptor->texture_uri[0];
    if (templateRequested) {
        if (!descriptor->id || !descriptor->id[0]) {
            return Invalid(
                mod,
                "template-backed UI controls require an object id");
        }
        const WotbModV3Result resolved = ResolveVirtualFilePath(
            mod,
            descriptor->texture_uri,
            &resolvedTemplatePath);
        if (resolved != WOTBMOD_V3_OK) {
            return resolved;
        }
        if (resolvedTemplatePath.empty()) {
            return Invalid(
                mod,
                "UI template URI must use mod:// or game://");
        }
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = templateRequested ? descriptor->id : nullptr;
    request.secondary_name = templateRequested
        ? resolvedTemplatePath.c_str()
        : nullptr;
    request.payload = descriptor;
    request.payload_size = sizeof(*descriptor);
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_create",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "UI backend returned an invalid control object");
    }
    UiControl* control = new (std::nothrow) UiControl();
    if (!control) {
        ReleaseHostObject(
            mod,
            "ui_control_destroy",
            response.object);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI control allocation failed");
    }
    control->owner = mod;
    control->nativeObject = response.object;
    control->type = descriptor->type;
    control->visible = descriptor->visible ? 1u : 0u;
    control->id = descriptor->id ? descriptor->id : "";
    control->text = descriptor->text ? descriptor->text : "";
    control->texture =
        descriptor->texture_uri ? descriptor->texture_uri : "";
    control->position = {descriptor->geometry.x, descriptor->geometry.y};
    control->size = {
        descriptor->geometry.width,
        descriptor->geometry.height};
    WOTBMOD_V3_INIT_STRUCT(
        control->layout,
        WOTBMOD_V3_UI_VERSION);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_UI_CONTROL,
        control,
        &DestroyUiControl,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "ui_control_destroy",
            control->nativeObject);
        control->nativeObject = 0u;
        delete control;
        return result;
    }
    control->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_uiControls[handle] = control;
    }
    *outControl = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlClone(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3UiHandle* outClone) {
    UiControl* source = nullptr;
    WotbModV3Result result = GetUiControl(mod, handle, &source);
    if (result != WOTBMOD_V3_OK || !outClone) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "clone output pointer is null");
    }
    UiControl copy;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        copy = *source;
    }
    const uint64_t sourceNativeObject = copy.nativeObject;
    copy.nativeObject = 0u;
    copy.self = WOTBMOD_V3_INVALID_HANDLE;
    copy.parent = WOTBMOD_V3_INVALID_HANDLE;
    copy.children.clear();
    copy.styleOverrides.clear();
    copy.id.clear();
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = sourceNativeObject;
    ClientHostObjectResponse response = MakeHostResponse();
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_clone",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "UI backend returned an invalid cloned control");
    }
    copy.nativeObject = response.object;
    UiControl* clone = new (std::nothrow) UiControl(std::move(copy));
    if (!clone) {
        ReleaseHostObject(
            mod,
            "ui_control_destroy",
            response.object);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI clone allocation failed");
    }
    WotbModV3Handle cloneHandle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_UI_CONTROL,
        clone,
        &DestroyUiControl,
        &cloneHandle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "ui_control_destroy",
            clone->nativeObject);
        clone->nativeObject = 0u;
        delete clone;
        return result;
    }
    clone->self = cloneHandle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_uiControls[cloneHandle] = clone;
    }
    *outClone = cloneHandle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlDestroy(
    WotbModV3Handle mod,
    WotbModV3UiHandle control) {
    UiControl* checked = nullptr;
    WotbModV3Result result = GetUiControl(mod, control, &checked);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = ReleaseUiControlStyles(checked);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = checked->nativeObject;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_destroy",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    checked->nativeObject = 0u;
    return ReleaseOwnedHandle(mod, control);
}

WotbModV3Result UiSetParentCore(
    WotbModV3Handle mod,
    UiControl* control,
    UiControl* parent) {
    if (!control || control == parent) {
        return Invalid(mod, "UI control cannot parent itself");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (parent && parent->children.size() >= kMaxUiChildren &&
        std::find(
            parent->children.begin(),
            parent->children.end(),
            control->self) == parent->children.end()) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI parent child limit reached");
    }
    if (parent) {
        WotbModV3UiHandle cursor = parent->self;
        uint32_t depth = 0;
        while (cursor != WOTBMOD_V3_INVALID_HANDLE &&
               depth++ < kMaxGraphDepth) {
            if (cursor == control->self) {
                return SetError(
                    mod,
                    WOTBMOD_V3_E_CONFLICT,
                    "UI hierarchy cycle rejected");
            }
            const auto it = g_uiControls.find(cursor);
            cursor = it != g_uiControls.end()
                         ? it->second->parent
                         : WOTBMOD_V3_INVALID_HANDLE;
        }
        if (depth >= kMaxGraphDepth) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "UI hierarchy exceeds maximum depth");
        }
    }
    const auto oldParentIt = g_uiControls.find(control->parent);
    if (oldParentIt != g_uiControls.end()) {
        std::vector<WotbModV3UiHandle>& oldChildren =
            oldParentIt->second->children;
        oldChildren.erase(
            std::remove(
                oldChildren.begin(),
                oldChildren.end(),
                control->self),
            oldChildren.end());
    }
    control->parent =
        parent ? parent->self : WOTBMOD_V3_INVALID_HANDLE;
    if (parent &&
        std::find(
            parent->children.begin(),
            parent->children.end(),
            control->self) == parent->children.end()) {
        parent->children.push_back(control->self);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlSetParent(
    WotbModV3Handle mod,
    WotbModV3UiHandle controlHandle,
    WotbModV3UiHandle parentHandle) {
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiControl(mod, controlHandle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiControl* parent = nullptr;
    if (parentHandle != WOTBMOD_V3_INVALID_HANDLE) {
        result = GetUiControl(mod, parentHandle, &parent);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    /*
     * Reject all managed-graph errors before changing the native hierarchy.
     * UiSetParentCore repeats these checks after the synchronous host call so
     * the managed mirror is only committed after the DAVA operation succeeds.
     */
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (control == parent) {
            return Invalid(mod, "UI control cannot parent itself");
        }
        if (parent && parent->children.size() >= kMaxUiChildren &&
            std::find(
                parent->children.begin(),
                parent->children.end(),
                control->self) == parent->children.end()) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "UI parent child limit reached");
        }
        WotbModV3UiHandle cursor =
            parent ? parent->self : WOTBMOD_V3_INVALID_HANDLE;
        uint32_t depth = 0u;
        while (cursor != WOTBMOD_V3_INVALID_HANDLE &&
               depth++ < kMaxGraphDepth) {
            if (cursor == control->self) {
                return SetError(
                    mod,
                    WOTBMOD_V3_E_CONFLICT,
                    "UI hierarchy cycle rejected");
            }
            const auto it = g_uiControls.find(cursor);
            cursor = it != g_uiControls.end()
                         ? it->second->parent
                         : WOTBMOD_V3_INVALID_HANDLE;
        }
        if (depth >= kMaxGraphDepth) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "UI hierarchy exceeds maximum depth");
        }
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.related_object =
        parent ? parent->nativeObject : 0u;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_parent",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UiSetParentCore(mod, control, parent);
}

WotbModV3Result WOTBMOD_V3_CALL UiControlAddChild(
    WotbModV3Handle mod,
    WotbModV3UiHandle parent,
    WotbModV3UiHandle child) {
    return UiControlSetParent(mod, child, parent);
}

WotbModV3Result WOTBMOD_V3_CALL UiControlRemoveChild(
    WotbModV3Handle mod,
    WotbModV3UiHandle parentHandle,
    WotbModV3UiHandle childHandle) {
    UiControl* parent = nullptr;
    UiControl* child = nullptr;
    WotbModV3Result result = GetUiControl(mod, parentHandle, &parent);
    if (result == WOTBMOD_V3_OK) {
        result = GetUiControl(mod, childHandle, &child);
    }
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (child->parent != parent->self) {
            return SetError(
                mod,
                WOTBMOD_V3_E_NOT_FOUND,
                "UI child is not attached to the specified parent");
        }
    }
    return UiControlSetParent(
        mod,
        childHandle,
        WOTBMOD_V3_INVALID_HANDLE);
}

WotbModV3Result WOTBMOD_V3_CALL UiControlGetParent(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3UiHandle* outParent) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK || !outParent) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "parent output pointer is null");
    }
    if (!control->gameOwned) {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        *outParent = control->parent;
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result nativeResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_get_parent",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (nativeResult == WOTBMOD_V3_E_NOT_FOUND) {
        *outParent = WOTBMOD_V3_INVALID_HANDLE;
        return WOTBMOD_V3_OK;
    }
    if (nativeResult != WOTBMOD_V3_OK) {
        return nativeResult;
    }
    return WrapHostUiControl(
        mod,
        response,
        true,
        WOTBMOD_V3_INVALID_HANDLE,
        nullptr,
        outParent);
}

WotbModV3Result WOTBMOD_V3_CALL UiControlGetChildCount(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t* outCount) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK || !outCount) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "child count output pointer is null");
    }
    if (!control->gameOwned) {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        *outCount = static_cast<uint32_t>(control->children.size());
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result nativeResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_get_child_count",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (nativeResult != WOTBMOD_V3_OK) {
        return nativeResult;
    }
    *outCount = response.value_u32;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlGetChildAt(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t index,
    WotbModV3UiHandle* outChild) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK || !outChild) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "child output pointer is null");
    }
    if (!control->gameOwned) {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (index >= control->children.size()) {
            return NotFound(mod, "UI child index is out of range");
        }
        *outChild = control->children[index];
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.selector = index;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result nativeResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_get_child_at",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (nativeResult != WOTBMOD_V3_OK) {
        return nativeResult;
    }
    return WrapHostUiControl(
        mod,
        response,
        true,
        handle,
        nullptr,
        outChild);
}

WotbModV3Result UiFindNativeByName(
    WotbModV3Handle mod,
    UiControl* root,
    const char* name,
    bool recursive,
    WotbModV3UiHandle* outControl) {
    if (!root || root->nativeObject == 0u || !name || !outControl) {
        return Invalid(mod, "native UI find arguments are invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = root->nativeObject;
    request.name = name;
    request.flags = recursive ? 1u : 0u;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_find_by_name",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return WrapHostUiControl(
        mod,
        response,
        root->gameOwned,
        root->self,
        name,
        outControl);
}

WotbModV3Result WOTBMOD_V3_CALL UiControlSetId(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    const char* id) {
    if (!ValidText(id, WOTBMOD_V3_MAX_ID, false)) {
        return Invalid(mod, "UI ID is invalid");
    }
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    for (const auto& pair : g_uiControls) {
        if (pair.second != control && pair.second->owner == mod &&
            pair.second->id == id) {
            return SetError(
                mod,
                WOTBMOD_V3_E_ALREADY_EXISTS,
                "UI ID is already owned by this mod");
        }
    }
    control->id = id;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlGetId(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    char* buffer,
    uint32_t* inoutSize) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::string id;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        id = control->id;
    }
    return CopyString(mod, id, buffer, inoutSize);
}

bool UiIsDescendantLocked(
    WotbModV3UiHandle root,
    WotbModV3UiHandle candidate) {
    if (root == WOTBMOD_V3_INVALID_HANDLE) {
        return true;
    }
    WotbModV3UiHandle cursor = candidate;
    uint32_t depth = 0;
    while (cursor != WOTBMOD_V3_INVALID_HANDLE &&
           depth++ < kMaxGraphDepth) {
        if (cursor == root) {
            return true;
        }
        const auto it = g_uiControls.find(cursor);
        cursor = it != g_uiControls.end()
                     ? it->second->parent
                     : WOTBMOD_V3_INVALID_HANDLE;
    }
    return false;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlFindById(
    WotbModV3Handle mod,
    WotbModV3UiHandle root,
    const char* id,
    WotbModV3UiHandle* outControl) {
    if (!ValidText(id, WOTBMOD_V3_MAX_ID, false) || !outControl) {
        return Invalid(mod, "UI find arguments are invalid");
    }
    UiControl* checked = nullptr;
    if (root != WOTBMOD_V3_INVALID_HANDLE) {
        const WotbModV3Result result = GetUiControl(mod, root, &checked);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
        if (checked->nativeObject != 0u) {
            return UiFindNativeByName(
                mod, checked, id, true, outControl);
        }
    } else {
        const WotbModV3Result result = CheckUiOwnPermission(mod);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    for (const auto& pair : g_uiControls) {
        const UiControl* control = pair.second;
        if (control->owner == mod && control->id == id &&
            UiIsDescendantLocked(root, control->self)) {
            *outControl = control->self;
            return WOTBMOD_V3_OK;
        }
    }
    return NotFound(mod, "UI ID was not found");
}

WotbModV3Result WOTBMOD_V3_CALL UiControlFindByPath(
    WotbModV3Handle mod,
    WotbModV3UiHandle root,
    const char* path,
    WotbModV3UiHandle* outControl) {
    if (!ValidText(path, WOTBMOD_V3_MAX_PATH, false) || !outControl) {
        return Invalid(mod, "UI path arguments are invalid");
    }
    UiControl* checked = nullptr;
    if (root != WOTBMOD_V3_INVALID_HANDLE) {
        const WotbModV3Result result =
            GetUiControl(mod, root, &checked);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    } else {
        const WotbModV3Result result = CheckUiOwnPermission(mod);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    std::string value(path);
    if (!checked || !checked->gameOwned) {
        const size_t slash = value.find_last_of('/');
        const std::string terminal = slash == std::string::npos
            ? value
            : value.substr(slash + 1u);
        if (terminal.empty()) {
            return Invalid(mod, "UI path has no terminal ID");
        }
        return UiControlFindById(
            mod, root, terminal.c_str(), outControl);
    }
    WotbModV3UiHandle currentHandle = root;
    UiControl* current = checked;
    size_t offset = 0u;
    while (offset < value.size()) {
        while (offset < value.size() && value[offset] == '/') {
            ++offset;
        }
        if (offset >= value.size()) break;
        const size_t slash = value.find('/', offset);
        const std::string segment = value.substr(
            offset,
            slash == std::string::npos
                ? std::string::npos
                : slash - offset);
        if (segment.empty()) {
            return Invalid(mod, "UI path contains an empty segment");
        }
        WotbModV3UiHandle next = WOTBMOD_V3_INVALID_HANDLE;
        WotbModV3Result result = UiFindNativeByName(
            mod, current, segment.c_str(), false, &next);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
        currentHandle = next;
        result = GetUiControl(mod, currentHandle, &current);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
        if (slash == std::string::npos) break;
        offset = slash + 1u;
    }
    if (currentHandle == root) {
        return Invalid(mod, "UI path has no control name");
    }
    *outControl = currentHandle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlGetOwner(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Handle* outOwner) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK || !outOwner) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "owner output pointer is null");
    }
    *outOwner = control->owner;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiControlIsAlive(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t* outAlive) {
    if (!outAlive) {
        return Invalid(mod, "alive output pointer is null");
    }
    const WotbModV3Result modResult = CheckMod(mod);
    if (modResult != WOTBMOD_V3_OK) {
        return modResult;
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_UI_CONTROL,
        &object,
        nullptr);
    const bool alive = result == WOTBMOD_V3_OK && object &&
        static_cast<UiControl*>(object)->magic == kUiMagic;
    if (alive) {
        const UiControl* control = static_cast<UiControl*>(object);
        const WotbModV3Result permission = control->gameOwned
            ? CheckNamedPermission(
                  mod,
                  "ui.modify.game",
                  WOTBMOD_V3_PERMISSION_REVIEWED)
            : CheckUiOwnPermission(mod);
        if (permission != WOTBMOD_V3_OK) {
            return permission;
        }
    }
    *outAlive = alive ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiGetActiveScreen(
    WotbModV3Handle mod,
    WotbModV3UiHandle* outScreen) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "ui.modify.game",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outScreen) {
        return Invalid(mod, "active screen output pointer is null");
    }
    *outScreen = WOTBMOD_V3_INVALID_HANDLE;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_get_active_screen",
        nullptr,
        0u,
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return WrapHostUiControl(
        mod,
        response,
        true,
        WOTBMOD_V3_INVALID_HANDLE,
        "active_screen",
        outScreen);
}

WotbModV3Result WOTBMOD_V3_CALL UiControlGetSnapshot(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3UiControlSnapshot* outSnapshot) {
    if (!outSnapshot || outSnapshot->struct_size < sizeof(*outSnapshot) ||
        outSnapshot->api_version != WOTBMOD_V3_UI_VERSION_3) {
        return Invalid(mod, "UI snapshot output is invalid");
    }
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (control->gameOwned) {
        result = RefreshUiControlFromHost(mod, control);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    uint32_t childCount = 0u;
    result = UiControlGetChildCount(mod, handle, &childCount);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3UiControlSnapshot snapshot = {};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.api_version = WOTBMOD_V3_UI_VERSION_3;
    snapshot.control = handle;
    snapshot.child_count = childCount;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        snapshot.parent = control->parent;
        snapshot.type = control->type;
        snapshot.geometry = {
            control->position.x,
            control->position.y,
            control->size.x,
            control->size.y};
        snapshot.z_order = control->zOrder;
        snapshot.flags =
            (control->visible != 0u
                 ? WOTBMOD_V3_UI_SNAPSHOT_VISIBLE : 0u) |
            (control->enabled != 0u
                 ? WOTBMOD_V3_UI_SNAPSHOT_ENABLED : 0u) |
            (control->interactable != 0u
                 ? WOTBMOD_V3_UI_SNAPSHOT_INTERACTABLE : 0u) |
            (control->focused != 0u
                 ? WOTBMOD_V3_UI_SNAPSHOT_FOCUSED : 0u) |
            (control->gameOwned
                 ? WOTBMOD_V3_UI_SNAPSHOT_GAME_OWNED : 0u);
#if defined(_MSC_VER)
        strncpy_s(
            snapshot.id,
            sizeof(snapshot.id),
            control->id.c_str(),
            _TRUNCATE);
#else
        std::strncpy(
            snapshot.id,
            control->id.c_str(),
            sizeof(snapshot.id) - 1u);
#endif
    }
    *outSnapshot = snapshot;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSlotFind(
    WotbModV3Handle mod,
    const char* slotId,
    WotbModV3UiHandle* outSlot) {
    if (outSlot) *outSlot = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result anyPermission =
        CheckAnyUiSlotPermission(mod);
    if (anyPermission != WOTBMOD_V3_OK) {
        return anyPermission;
    }
    if (!outSlot) {
        return Invalid(mod, "UI slot output is required");
    }
    if (!ValidText(slotId, WOTBMOD_V3_MAX_ID, false)) {
        return Invalid(mod, "UI slot arguments are invalid");
    }
    const WotbModV3Result slotPermission =
        CheckUiSlotPermission(mod, slotId);
    if (slotPermission != WOTBMOD_V3_OK) {
        return slotPermission;
    }
    bool stable = false;
    for (const char* candidate : kStableUiSlots) {
        if (std::strcmp(candidate, slotId) == 0) {
            stable = true;
            break;
        }
    }
    if (!stable) {
        return NotFound(mod, "unknown stable UI slot");
    }
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "exact native UI extension point is not resolved for this stable slot",
        "{\"backend\":\"native_ui_semantic_extension_points\",\"fallback\":\"disabled\"}");
}

WotbModV3Result WOTBMOD_V3_CALL UiSlotAttach(
    WotbModV3Handle mod,
    WotbModV3UiHandle slotHandle,
    WotbModV3UiHandle controlHandle,
    int32_t priority) {
    const WotbModV3Result anyPermission =
        CheckAnyUiSlotPermission(mod);
    if (anyPermission != WOTBMOD_V3_OK) {
        return anyPermission;
    }
    UiSlot* slot = nullptr;
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiSlot(mod, slotHandle, &slot);
    if (result == WOTBMOD_V3_OK) {
        result = GetUiControl(mod, controlHandle, &control);
    }
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckUiSlotPermission(mod, slot->id.c_str());
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    (void)priority;
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "exact native UI extension point attachment is not available",
        "{\"backend\":\"native_ui_semantic_extension_points\",\"fallback\":\"disabled\"}");
}

WotbModV3Result WOTBMOD_V3_CALL UiSlotDetach(
    WotbModV3Handle mod,
    WotbModV3UiHandle slotHandle,
    WotbModV3UiHandle controlHandle) {
    const WotbModV3Result anyPermission =
        CheckAnyUiSlotPermission(mod);
    if (anyPermission != WOTBMOD_V3_OK) {
        return anyPermission;
    }
    UiSlot* slot = nullptr;
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiSlot(mod, slotHandle, &slot);
    if (result == WOTBMOD_V3_OK) {
        result = GetUiControl(mod, controlHandle, &control);
    }
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckUiSlotPermission(mod, slot->id.c_str());
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "exact native UI extension point detachment is not available",
        "{\"backend\":\"native_ui_semantic_extension_points\",\"fallback\":\"disabled\"}");
}

WotbModV3Result WOTBMOD_V3_CALL UiSlotEnumerate(
    WotbModV3Handle mod,
    WotbModV3UiSlotVisitor visitor,
    void* userData) {
    const WotbModV3Result namedPermission = CheckNamedPermission(
        mod,
        "ui.modify.game",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (namedPermission != WOTBMOD_V3_OK) {
        return namedPermission;
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!visitor) {
        return Invalid(mod, "UI slot visitor is null");
    }
    (void)userData;
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "exact native UI extension points cannot be enumerated",
        "{\"backend\":\"native_ui_semantic_extension_points\",\"fallback\":\"disabled\"}");
}

WotbModV3Result UiSetVec2(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Vec2 value,
    WotbModV3Vec2 UiControl::*member,
    bool requireNonnegative,
    const char* operation) {
    if (!IsFinite(value) ||
        (requireNonnegative && (value.x < 0.0f || value.y < 0.0f))) {
        return Invalid(mod, "UI vector value is invalid");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.vector = {value.x, value.y, 0.0f, 0.0f};
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->*member = value;
    ++control->layoutRevision;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UiSetGeometry(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    const WotbModV3Vec2* position,
    const WotbModV3Vec2* size) {
    if ((position && !IsFinite(*position)) ||
        (size &&
         (!IsFinite(*size) || size->x < 0.0f || size->y < 0.0f))) {
        return Invalid(mod, "UI geometry value is invalid");
    }
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3Vec2 nextPosition = {};
    WotbModV3Vec2 nextSize = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        nextPosition = position ? *position : control->position;
        nextSize = size ? *size : control->size;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.vector = {
        nextPosition.x,
        nextPosition.y,
        nextSize.x,
        nextSize.y};
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_geometry",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        control->position = nextPosition;
        control->size = nextSize;
        ++control->layoutRevision;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result UiRefreshState(
    WotbModV3Handle mod,
    UiControl* control) {
    if (!control) {
        return Invalid(mod, "UI control is null");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_get_state",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!IsFinite(response.rect) ||
        response.rect.width < 0.0f ||
        response.rect.height < 0.0f) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "UI backend returned an invalid control state");
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        control->position = {response.rect.x, response.rect.y};
        control->size = {
            response.rect.width,
            response.rect.height};
        control->visible =
            (response.value_u32 & 1u) != 0u ? 1u : 0u;
        control->interactable =
            (response.value_u32 & 2u) != 0u ? 1u : 0u;
        control->enabled =
            (response.value_u32 & 4u) != 0u ? 1u : 0u;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetPosition(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3Vec2 position) {
    return UiSetGeometry(mod, control, &position, nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL UiGetPosition(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Vec2* outPosition) {
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK || !outPosition) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "position output pointer is null");
    }
    result = UiRefreshState(mod, control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    *outPosition = control->position;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetSize(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3Vec2 size) {
    return UiSetGeometry(mod, control, nullptr, &size);
}

WotbModV3Result WOTBMOD_V3_CALL UiGetSize(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Vec2* outSize) {
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK || !outSize) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "size output pointer is null");
    }
    result = UiRefreshState(mod, control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    *outSize = control->size;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetAnchor(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3Vec2 anchor) {
    if (anchor.x < 0.0f || anchor.x > 1.0f ||
        anchor.y < 0.0f || anchor.y > 1.0f) {
        return Invalid(mod, "UI anchor must be within [0, 1]");
    }
    return UiSetVec2(
        mod,
        control,
        anchor,
        &UiControl::anchor,
        false,
        "ui_control_set_anchor");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetPivot(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3Vec2 pivot) {
    if (pivot.x < 0.0f || pivot.x > 1.0f ||
        pivot.y < 0.0f || pivot.y > 1.0f) {
        return Invalid(mod, "UI pivot must be within [0, 1]");
    }
    return UiSetVec2(
        mod,
        control,
        pivot,
        &UiControl::pivot,
        false,
        "ui_control_set_pivot");
}

WotbModV3Result UiSetEdges(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3UiEdges value,
    WotbModV3UiEdges UiControl::*member,
    const char* operation) {
    if (!IsFinite(value.left) || !IsFinite(value.top) ||
        !IsFinite(value.right) || !IsFinite(value.bottom)) {
        return Invalid(mod, "UI edges contain a non-finite value");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.vector = {
        value.left,
        value.top,
        value.right,
        value.bottom};
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->*member = value;
    ++control->layoutRevision;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetMargin(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3UiEdges margin) {
    return UiSetEdges(
        mod,
        control,
        margin,
        &UiControl::margin,
        "ui_control_set_margin");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetPadding(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3UiEdges padding) {
    return UiSetEdges(
        mod,
        control,
        padding,
        &UiControl::padding,
        "ui_control_set_padding");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetMinSize(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3Vec2 size) {
    return UiSetVec2(
        mod,
        control,
        size,
        &UiControl::minSize,
        true,
        "ui_control_set_min_size");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetMaxSize(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3Vec2 size) {
    return UiSetVec2(
        mod,
        control,
        size,
        &UiControl::maxSize,
        true,
        "ui_control_set_max_size");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetZOrder(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    int32_t zOrder) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.signed_value = zOrder;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_z_order",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->zOrder = zOrder;
    return WOTBMOD_V3_OK;
}

bool ValidLayout(const WotbModV3UiLayoutDescriptor& layout) {
    return layout.type <= WOTBMOD_V3_UI_LAYOUT_OVERLAY &&
           layout.direction <= WOTBMOD_V3_UI_DIRECTION_BOTTOM_TO_TOP &&
           layout.main_alignment <= WOTBMOD_V3_UI_ALIGN_STRETCH &&
           layout.cross_alignment <= WOTBMOD_V3_UI_ALIGN_STRETCH &&
           IsFinite(layout.spacing) && layout.spacing >= 0.0f &&
           IsFinite(layout.weight) && layout.weight >= 0.0f;
}

WotbModV3Result UiApplyLayout(
    WotbModV3Handle mod,
    UiControl* control,
    const WotbModV3UiLayoutDescriptor& layout) {
    (void)mod;
    /*
     * Layout is intentionally managed by the API. DAVA exposes verified
     * geometry and hierarchy primitives, but no stable cross-version ABI for
     * its layout component objects. Keeping the descriptor here lets
     * layout_invalidate perform deterministic geometry updates without
     * manufacturing native component ownership.
     */
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->layout = layout;
    ++control->layoutRevision;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiLayoutSet(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    const WotbModV3UiLayoutDescriptor* layout) {
    if (!ValidStruct(layout, WOTBMOD_V3_UI_VERSION) ||
        !ValidLayout(*layout)) {
        return Invalid(mod, "invalid UI layout descriptor");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UiApplyLayout(mod, control, *layout);
}

WotbModV3Result WOTBMOD_V3_CALL UiLayoutSetType(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t type) {
    if (type > WOTBMOD_V3_UI_LAYOUT_OVERLAY) {
        return Invalid(mod, "invalid UI layout type");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3UiLayoutDescriptor layout = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        layout = control->layout;
    }
    layout.type = type;
    return UiApplyLayout(mod, control, layout);
}

WotbModV3Result WOTBMOD_V3_CALL UiLayoutSetDirection(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t direction) {
    if (direction > WOTBMOD_V3_UI_DIRECTION_BOTTOM_TO_TOP) {
        return Invalid(mod, "invalid UI layout direction");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3UiLayoutDescriptor layout = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        layout = control->layout;
    }
    layout.direction = direction;
    return UiApplyLayout(mod, control, layout);
}

WotbModV3Result WOTBMOD_V3_CALL UiLayoutSetSpacing(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    float spacing) {
    if (!IsFinite(spacing) || spacing < 0.0f) {
        return Invalid(mod, "invalid UI layout spacing");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3UiLayoutDescriptor layout = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        layout = control->layout;
    }
    layout.spacing = spacing;
    return UiApplyLayout(mod, control, layout);
}

WotbModV3Result WOTBMOD_V3_CALL UiLayoutSetAlignment(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t mainAlignment,
    uint32_t crossAlignment) {
    if (mainAlignment > WOTBMOD_V3_UI_ALIGN_STRETCH ||
        crossAlignment > WOTBMOD_V3_UI_ALIGN_STRETCH) {
        return Invalid(mod, "invalid UI layout alignment");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3UiLayoutDescriptor layout = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        layout = control->layout;
    }
    layout.main_alignment = mainAlignment;
    layout.cross_alignment = crossAlignment;
    return UiApplyLayout(mod, control, layout);
}

WotbModV3Result WOTBMOD_V3_CALL UiLayoutSetWeight(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    float weight) {
    if (!IsFinite(weight) || weight < 0.0f) {
        return Invalid(mod, "invalid UI layout weight");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3UiLayoutDescriptor layout = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        layout = control->layout;
    }
    layout.weight = weight;
    return UiApplyLayout(mod, control, layout);
}

WotbModV3Result WOTBMOD_V3_CALL UiLayoutInvalidate(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle) {
    UiControl* control = nullptr;
    WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }

    struct Item {
        WotbModV3UiHandle handle = WOTBMOD_V3_INVALID_HANDLE;
        WotbModV3Vec2 oldPosition = {};
        WotbModV3Vec2 oldSize = {};
        WotbModV3Vec2 minSize = {};
        WotbModV3Vec2 maxSize = {};
        WotbModV3UiEdges margin = {};
        float weight = 0.0f;
        WotbModV3Vec2 position = {};
        WotbModV3Vec2 size = {};
    };

    WotbModV3UiLayoutDescriptor layout = {};
    WotbModV3Vec2 containerSize = {};
    WotbModV3UiEdges padding = {};
    std::vector<Item> items;
    try {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        layout = control->layout;
        containerSize = control->size;
        padding = control->padding;
        items.reserve(control->children.size());
        for (WotbModV3UiHandle childHandle : control->children) {
            const auto found = g_uiControls.find(childHandle);
            if (found == g_uiControls.end() || !found->second ||
                found->second->magic != kUiMagic) {
                continue;
            }
            const UiControl& child = *found->second;
            Item item;
            item.handle = childHandle;
            item.oldPosition = child.position;
            item.oldSize = child.size;
            item.minSize = child.minSize;
            item.maxSize = child.maxSize;
            item.margin = child.margin;
            item.weight = child.layout.weight;
            item.position = child.position;
            item.size = child.size;
            items.push_back(item);
        }
    } catch (...) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI layout snapshot allocation failed");
    }

    if (layout.type == WOTBMOD_V3_UI_LAYOUT_ABSOLUTE || items.empty()) {
        return WOTBMOD_V3_OK;
    }
    const float innerWidth = std::max(
        0.0f,
        containerSize.x - padding.left - padding.right);
    const float innerHeight = std::max(
        0.0f,
        containerSize.y - padding.top - padding.bottom);
    auto clampSize = [](float value, float minimum, float maximum) {
        return std::max(minimum, std::min(value, maximum));
    };

    if (layout.type == WOTBMOD_V3_UI_LAYOUT_OVERLAY) {
        for (Item& item : items) {
            item.position = {
                padding.left + item.margin.left,
                padding.top + item.margin.top};
            if (layout.main_alignment == WOTBMOD_V3_UI_ALIGN_STRETCH ||
                layout.cross_alignment == WOTBMOD_V3_UI_ALIGN_STRETCH) {
                item.size = {
                    clampSize(
                        innerWidth - item.margin.left - item.margin.right,
                        item.minSize.x,
                        item.maxSize.x),
                    clampSize(
                        innerHeight - item.margin.top - item.margin.bottom,
                        item.minSize.y,
                        item.maxSize.y)};
            }
        }
    } else if (layout.type == WOTBMOD_V3_UI_LAYOUT_GRID) {
        const uint32_t columns = std::max(1u, layout.columns);
        const uint32_t rows = static_cast<uint32_t>(
            (items.size() + columns - 1u) / columns);
        const float cellWidth = std::max(
            0.0f,
            (innerWidth - layout.spacing * (columns - 1u)) / columns);
        const float cellHeight = std::max(
            0.0f,
            (innerHeight - layout.spacing * (rows - 1u)) / rows);
        for (size_t index = 0u; index < items.size(); ++index) {
            Item& item = items[index];
            const uint32_t column = static_cast<uint32_t>(index) % columns;
            const uint32_t row = static_cast<uint32_t>(index) / columns;
            item.position = {
                padding.left + column * (cellWidth + layout.spacing) +
                    item.margin.left,
                padding.top + row * (cellHeight + layout.spacing) +
                    item.margin.top};
            item.size = {
                clampSize(
                    cellWidth - item.margin.left - item.margin.right,
                    item.minSize.x,
                    item.maxSize.x),
                clampSize(
                    cellHeight - item.margin.top - item.margin.bottom,
                    item.minSize.y,
                    item.maxSize.y)};
        }
    } else {
        const bool horizontal =
            layout.type == WOTBMOD_V3_UI_LAYOUT_HORIZONTAL ||
            (layout.type == WOTBMOD_V3_UI_LAYOUT_FLEX &&
             layout.direction <= WOTBMOD_V3_UI_DIRECTION_RIGHT_TO_LEFT);
        const bool reverse =
            layout.direction == WOTBMOD_V3_UI_DIRECTION_RIGHT_TO_LEFT ||
            layout.direction == WOTBMOD_V3_UI_DIRECTION_BOTTOM_TO_TOP;
        const float innerMain = horizontal ? innerWidth : innerHeight;
        const float innerCross = horizontal ? innerHeight : innerWidth;
        float margins = 0.0f;
        float fixed = 0.0f;
        float totalWeight = 0.0f;
        for (Item& item : items) {
            margins += horizontal
                ? item.margin.left + item.margin.right
                : item.margin.top + item.margin.bottom;
            const float weight =
                item.weight > 0.0f
                    ? item.weight
                    : (layout.main_alignment == WOTBMOD_V3_UI_ALIGN_STRETCH
                           ? 1.0f
                           : 0.0f);
            item.weight = weight;
            if (weight > 0.0f) {
                totalWeight += weight;
            } else {
                fixed += horizontal ? item.size.x : item.size.y;
            }
        }
        const float spacingTotal =
            layout.spacing * static_cast<float>(items.size() - 1u);
        const float remaining = std::max(
            0.0f, innerMain - margins - spacingTotal - fixed);
        float contentMain = margins + spacingTotal;
        for (Item& item : items) {
            float main = horizontal ? item.size.x : item.size.y;
            if (item.weight > 0.0f && totalWeight > 0.0f) {
                main = remaining * item.weight / totalWeight;
            }
            main = horizontal
                ? clampSize(main, item.minSize.x, item.maxSize.x)
                : clampSize(main, item.minSize.y, item.maxSize.y);
            if (horizontal) item.size.x = main;
            else item.size.y = main;
            contentMain += main;
        }
        float cursor = 0.0f;
        if (layout.main_alignment == WOTBMOD_V3_UI_ALIGN_CENTER) {
            cursor = (innerMain - contentMain) * 0.5f;
        } else if (layout.main_alignment == WOTBMOD_V3_UI_ALIGN_END) {
            cursor = innerMain - contentMain;
        }
        cursor = std::max(0.0f, cursor);
        for (size_t step = 0u; step < items.size(); ++step) {
            const size_t index = reverse ? items.size() - 1u - step : step;
            Item& item = items[index];
            const float marginBefore = horizontal
                ? (reverse ? item.margin.right : item.margin.left)
                : (reverse ? item.margin.bottom : item.margin.top);
            const float marginAfter = horizontal
                ? (reverse ? item.margin.left : item.margin.right)
                : (reverse ? item.margin.top : item.margin.bottom);
            cursor += marginBefore;
            float cross = horizontal ? item.size.y : item.size.x;
            if (layout.cross_alignment == WOTBMOD_V3_UI_ALIGN_STRETCH) {
                const float crossMargins = horizontal
                    ? item.margin.top + item.margin.bottom
                    : item.margin.left + item.margin.right;
                cross = std::max(0.0f, innerCross - crossMargins);
            }
            cross = horizontal
                ? clampSize(cross, item.minSize.y, item.maxSize.y)
                : clampSize(cross, item.minSize.x, item.maxSize.x);
            float crossPosition = horizontal
                ? item.margin.top
                : item.margin.left;
            const float crossMargins = horizontal
                ? item.margin.top + item.margin.bottom
                : item.margin.left + item.margin.right;
            if (layout.cross_alignment == WOTBMOD_V3_UI_ALIGN_CENTER) {
                crossPosition =
                    (innerCross - cross - crossMargins) * 0.5f +
                    (horizontal ? item.margin.top : item.margin.left);
            } else if (layout.cross_alignment == WOTBMOD_V3_UI_ALIGN_END) {
                crossPosition = innerCross - cross -
                    (horizontal ? item.margin.bottom : item.margin.right);
            }
            if (horizontal) {
                item.position = {
                    padding.left + cursor,
                    padding.top + std::max(0.0f, crossPosition)};
                item.size.y = cross;
                cursor += item.size.x + marginAfter + layout.spacing;
            } else {
                item.position = {
                    padding.left + std::max(0.0f, crossPosition),
                    padding.top + cursor};
                item.size.x = cross;
                cursor += item.size.y + marginAfter + layout.spacing;
            }
        }
    }

    size_t applied = 0u;
    for (; applied < items.size(); ++applied) {
        result = UiSetGeometry(
            mod,
            items[applied].handle,
            &items[applied].position,
            &items[applied].size);
        if (result != WOTBMOD_V3_OK) break;
    }
    if (result != WOTBMOD_V3_OK) {
        while (applied > 0u) {
            --applied;
            UiSetGeometry(
                mod,
                items[applied].handle,
                &items[applied].oldPosition,
                &items[applied].oldSize);
        }
        return result;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiGetScaleFactor(
    WotbModV3Handle mod,
    float* outScale) {
    if (!outScale) {
        return Invalid(mod, "UI scale output pointer is null");
    }
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui.metrics",
        "ui_get_scale_factor");
}

WotbModV3Result WOTBMOD_V3_CALL UiGetSafeArea(
    WotbModV3Handle mod,
    WotbModV3Rect* outArea) {
    if (!outArea) {
        return Invalid(mod, "UI safe area output pointer is null");
    }
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui.metrics",
        "ui_get_safe_area");
}

WotbModV3Result WOTBMOD_V3_CALL UiGetViewportSize(
    WotbModV3Handle mod,
    WotbModV3Vec2* outSize) {
    if (!outSize) {
        return Invalid(mod, "UI viewport output pointer is null");
    }
    const WotbModV3Result permission = CheckUiOwnPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    const uint64_t packedViewport =
        g_uiViewportSize.load(std::memory_order_acquire);
    const uint32_t nativeWidth =
        static_cast<uint32_t>(packedViewport >> 32u);
    const uint32_t nativeHeight =
        static_cast<uint32_t>(packedViewport & UINT32_MAX);
    if (nativeWidth != 0u && nativeHeight != 0u) {
        *outSize = {
            static_cast<float>(nativeWidth),
            static_cast<float>(nativeHeight)};
        return WOTBMOD_V3_OK;
    }
    ClientHostFrame frame = {};
    if (!GetHostFrame(&frame)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "UI viewport is unavailable before a real host frame");
    }
    if (!IsFinite(frame.viewport) || frame.viewport.width <= 0.0f ||
        frame.viewport.height <= 0.0f) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "UI viewport is unavailable before a real host frame");
    }
    *outSize = {frame.viewport.width, frame.viewport.height};
    return WOTBMOD_V3_OK;
}

WotbModV3Result UiSetString(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    const char* value,
    size_t limit,
    std::string UiControl::*member,
    const char* operation) {
    if (!ValidText(value, limit)) {
        return Invalid(mod, "UI string value is invalid");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.name = value;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->*member = value;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetText(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    const char* text) {
    return UiSetString(
        mod,
        control,
        text,
        WOTBMOD_V3_MAX_MESSAGE,
        &UiControl::text,
        "ui_control_set_text");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetTexture(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    const char* uri) {
    return UiSetString(
        mod,
        control,
        uri,
        WOTBMOD_V3_MAX_PATH,
        &UiControl::texture,
        "ui_control_set_texture");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetColor(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Color color) {
    if (!IsFinite(color)) {
        return Invalid(mod, "UI color is invalid");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.vector = {color.r, color.g, color.b, color.a};
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_color",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->color = color;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetOpacity(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    float opacity) {
    if (!IsFinite(opacity) || opacity < 0.0f || opacity > 1.0f) {
        return Invalid(mod, "UI opacity must be within [0, 1]");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.scalar0 = opacity;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_opacity",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->opacity = opacity;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UiSetBool(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t value,
    uint32_t UiControl::*member,
    const char* operation) {
    if (value > 1u) {
        return Invalid(mod, "UI boolean value must be 0 or 1");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.flags = value;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->*member = value;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetVisible(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    uint32_t value) {
    return UiSetBool(
        mod,
        control,
        value,
        &UiControl::visible,
        "ui_control_set_visible");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetFont(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    const char* uri) {
    return UiSetString(
        mod,
        control,
        uri,
        WOTBMOD_V3_MAX_PATH,
        &UiControl::font,
        "ui_control_set_font");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetFontSize(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    float size) {
    if (!IsFinite(size) || size <= 0.0f || size > 512.0f) {
        return Invalid(mod, "UI font size is out of range");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.scalar0 = size;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_font_size",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->fontSize = size;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetTextAlignment(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t alignment) {
    if (alignment > WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY) {
        return Invalid(mod, "invalid UI text alignment");
    }
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.selector = alignment;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_text_alignment",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->textAlignment = alignment;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiSetTextWrap(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    uint32_t value) {
    return UiSetBool(
        mod,
        control,
        value,
        &UiControl::textWrap,
        "ui_control_set_text_wrap");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetRichText(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    uint32_t value) {
    return UiSetBool(
        mod,
        control,
        value,
        &UiControl::richText,
        "ui_control_set_rich_text");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetLocalizationKey(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    const char* key) {
    return UiSetString(
        mod,
        control,
        key,
        WOTBMOD_V3_MAX_NAME,
        &UiControl::localizationKey,
        "ui_control_set_localization_key");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetTooltip(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    const char* value) {
    return UiSetString(
        mod,
        control,
        value,
        WOTBMOD_V3_MAX_MESSAGE,
        &UiControl::tooltip,
        "ui_control_set_tooltip");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetAccessibilityLabel(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    const char* value) {
    return UiSetString(
        mod,
        control,
        value,
        WOTBMOD_V3_MAX_MESSAGE,
        &UiControl::accessibilityLabel,
        "ui_control_set_accessibility_label");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetEnabled(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    uint32_t value) {
    return UiSetBool(
        mod,
        control,
        value,
        &UiControl::enabled,
        "ui_control_set_enabled");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetInteractable(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    uint32_t value) {
    return UiSetBool(
        mod,
        control,
        value,
        &UiControl::interactable,
        "ui_control_set_interactable");
}

WotbModV3Result WOTBMOD_V3_CALL UiSetFocus(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    uint32_t value) {
    return UiSetBool(
        mod,
        control,
        value,
        &UiControl::focused,
        "ui_control_set_focus");
}

WotbModV3Result WOTBMOD_V3_CALL UiEventSubscribe(
    WotbModV3Handle mod,
    WotbModV3UiHandle controlHandle,
    uint32_t eventType,
    WotbModV3UiEventCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    UiControl* control = nullptr;
    const WotbModV3Result result =
        GetUiControl(mod, controlHandle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (eventType < WOTBMOD_V3_UI_EVENT_CLICK ||
        eventType > WOTBMOD_V3_UI_EVENT_CANCEL || !callback || !outToken) {
        return Invalid(mod, "invalid UI event subscription");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI subscription allocation failed");
    }
    subscription->kind = SubscriptionKind::UiEvent;
    subscription->target = controlHandle;
    subscription->eventType = eventType;
    subscription->uiCallback = callback;
    subscription->userData = userData;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL UiEventUnsubscribe(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    std::shared_ptr<Subscription> subscription;
    const WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::UiEvent,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiControl* control = nullptr;
    const WotbModV3Result permission = GetUiControl(
        mod,
        static_cast<WotbModV3UiHandle>(subscription->target),
        &control);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
}

WotbModV3Result UiCreateTyped(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    uint32_t type,
    WotbModV3UiHandle* outControl) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "ui.create",
        WOTBMOD_V3_PERMISSION_SAFE);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(descriptor, WOTBMOD_V3_UI_VERSION)) {
        return Invalid(mod, "invalid component descriptor");
    }
    WotbModV3UiControlDescriptor copy = *descriptor;
    copy.type = type;
    return UiControlCreate(mod, &copy, outControl);
}

WotbModV3Result WOTBMOD_V3_CALL UiButtonCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl) {
    return UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_BUTTON,
        outControl);
}

WotbModV3Result WOTBMOD_V3_CALL UiCheckboxCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    uint32_t checked,
    WotbModV3UiHandle* outControl) {
    if (checked > 1u) {
        return Invalid(mod, "checkbox value must be 0 or 1");
    }
    WotbModV3Result result = UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_CHECKBOX,
        outControl);
    if (result == WOTBMOD_V3_OK) {
        UiControl* control = nullptr;
        result = GetUiControl(mod, *outControl, &control);
        if (result == WOTBMOD_V3_OK) {
            control->checked = checked;
        }
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL UiSliderCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    double minimum,
    double maximum,
    double value,
    WotbModV3UiHandle* outControl) {
    if (!IsFinite(minimum) || !IsFinite(maximum) || !IsFinite(value) ||
        maximum <= minimum || value < minimum || value > maximum) {
        return Invalid(mod, "slider range is invalid");
    }
    WotbModV3Result result = UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_SLIDER,
        outControl);
    if (result == WOTBMOD_V3_OK) {
        UiControl* control = nullptr;
        result = GetUiControl(mod, *outControl, &control);
        if (result == WOTBMOD_V3_OK) {
            control->minimum = minimum;
            control->maximum = maximum;
            control->value = value;
        }
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL UiDropdownCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    const WotbModV3UiChoiceDescriptor* choices,
    uint32_t choiceCount,
    WotbModV3UiHandle* outControl) {
    if (choiceCount != 0u && !choices) {
        return Invalid(mod, "dropdown choices pointer is null");
    }
    for (uint32_t index = 0; index < choiceCount; ++index) {
        if (!ValidStruct(&choices[index], WOTBMOD_V3_UI_VERSION) ||
            !ValidText(choices[index].id, WOTBMOD_V3_MAX_ID, false) ||
            !ValidText(
                choices[index].label,
                WOTBMOD_V3_MAX_NAME,
                false) ||
            !ValidText(
                choices[index].value,
                WOTBMOD_V3_MAX_NAME)) {
            return Invalid(mod, "dropdown choice is invalid");
        }
    }
    WotbModV3Result result = UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_DROPDOWN,
        outControl);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiControl* control = nullptr;
    result = GetUiControl(mod, *outControl, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    control->choiceIds.reserve(choiceCount);
    control->choiceLabels.reserve(choiceCount);
    control->choiceValues.reserve(choiceCount);
    for (uint32_t index = 0; index < choiceCount; ++index) {
        control->choiceIds.emplace_back(choices[index].id);
        control->choiceLabels.emplace_back(choices[index].label);
        control->choiceValues.emplace_back(choices[index].value);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiTextInputCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl) {
    return UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_TEXT_INPUT,
        outControl);
}

WotbModV3Result WOTBMOD_V3_CALL UiScrollViewCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl) {
    return UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_SCROLL_VIEW,
        outControl);
}

WotbModV3Result WOTBMOD_V3_CALL UiListCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl) {
    return UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_LIST,
        outControl);
}

WotbModV3Result WOTBMOD_V3_CALL UiTabsCreate(
    WotbModV3Handle mod,
    const WotbModV3UiControlDescriptor* descriptor,
    WotbModV3UiHandle* outControl) {
    return UiCreateTyped(
        mod,
        descriptor,
        WOTBMOD_V3_UI_CONTROL_TABS,
        outControl);
}

WotbModV3Result WOTBMOD_V3_CALL UiDialogShow(
    WotbModV3Handle mod,
    const WotbModV3UiDialogDescriptor* descriptor,
    WotbModV3UiHandle* outDialog) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "ui.create",
        WOTBMOD_V3_PERMISSION_SAFE);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(descriptor, WOTBMOD_V3_UI_VERSION) || !outDialog ||
        !ValidText(descriptor->title, WOTBMOD_V3_MAX_NAME) ||
        !ValidText(descriptor->message, WOTBMOD_V3_MAX_MESSAGE)) {
        return Invalid(mod, "invalid UI dialog descriptor");
    }
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui.dialogs",
        "ui_dialog_show");
}

WotbModV3Result WOTBMOD_V3_CALL UiConfirmShow(
    WotbModV3Handle mod,
    const WotbModV3UiDialogDescriptor* descriptor,
    WotbModV3UiHandle* outDialog) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "ui.create",
        WOTBMOD_V3_PERMISSION_SAFE);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(descriptor, WOTBMOD_V3_UI_VERSION) || !outDialog ||
        !ValidText(descriptor->title, WOTBMOD_V3_MAX_NAME) ||
        !ValidText(descriptor->message, WOTBMOD_V3_MAX_MESSAGE)) {
        return Invalid(mod, "invalid UI confirmation descriptor");
    }
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui.dialogs",
        "ui_confirm_show");
}

WotbModV3Result WOTBMOD_V3_CALL UiToastShow(
    WotbModV3Handle mod,
    const char* message,
    float durationSeconds) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "ui.create",
        WOTBMOD_V3_PERMISSION_SAFE);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(message, WOTBMOD_V3_MAX_MESSAGE, false) ||
        !IsFinite(durationSeconds) || durationSeconds <= 0.0f ||
        durationSeconds > 60.0f) {
        return Invalid(mod, "invalid UI toast arguments");
    }
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui.toasts",
        "ui_toast_show");
}

bool ValidStylePatch(const WotbModV3UiStylePatch& patch) {
    const uint32_t knownFields =
        WOTBMOD_V3_UI_STYLE_COLOR |
        WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR |
        WOTBMOD_V3_UI_STYLE_OPACITY |
        WOTBMOD_V3_UI_STYLE_FONT |
        WOTBMOD_V3_UI_STYLE_FONT_SIZE |
        WOTBMOD_V3_UI_STYLE_TEXTURE |
        WOTBMOD_V3_UI_STYLE_Z_ORDER;
    if ((patch.fields & ~knownFields) != 0u ||
        ((patch.fields & WOTBMOD_V3_UI_STYLE_COLOR) &&
         !IsFinite(patch.color)) ||
        ((patch.fields & WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR) &&
         !IsFinite(patch.background_color)) ||
        ((patch.fields & WOTBMOD_V3_UI_STYLE_OPACITY) &&
         (!IsFinite(patch.opacity) || patch.opacity < 0.0f ||
          patch.opacity > 1.0f)) ||
        ((patch.fields & WOTBMOD_V3_UI_STYLE_FONT_SIZE) &&
         (!IsFinite(patch.font_size) || patch.font_size <= 0.0f ||
          patch.font_size > 512.0f)) ||
        ((patch.fields & WOTBMOD_V3_UI_STYLE_FONT) &&
         !ValidText(patch.font_uri, WOTBMOD_V3_MAX_PATH)) ||
        ((patch.fields & WOTBMOD_V3_UI_STYLE_TEXTURE) &&
         !ValidText(patch.texture_uri, WOTBMOD_V3_MAX_PATH))) {
        return false;
    }
    return true;
}

WotbModV3Result GetStyle(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    UiStyleOverride** outStyle) {
    const WotbModV3Result permission = CheckUiOwnPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outStyle) {
        return Invalid(mod, "style output pointer is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_STYLE_OVERRIDE,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiStyleOverride* style = static_cast<UiStyleOverride*>(object);
    if (!style || style->magic != kStyleMagic) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not a UI style override");
    }
    *outStyle = style;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UiSetBackgroundColor(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Color color) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) return result;
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.vector = {color.r, color.g, color.b, color.a};
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_background_color",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) return hostResult;
    std::lock_guard<std::mutex> lock(g_clientMutex);
    control->backgroundColor = color;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyStylePatch(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    const WotbModV3UiStylePatch& patch) {
    WotbModV3Result result = WOTBMOD_V3_OK;
#define APPLY_STYLE_FIELD(field, expression)                             \
    do {                                                                  \
        if ((patch.fields & (field)) != 0u) {                             \
            result = (expression);                                        \
            if (result != WOTBMOD_V3_OK) return result;                   \
        }                                                                 \
    } while (false)
    APPLY_STYLE_FIELD(
        WOTBMOD_V3_UI_STYLE_COLOR,
        UiSetColor(mod, control, patch.color));
    APPLY_STYLE_FIELD(
        WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR,
        UiSetBackgroundColor(mod, control, patch.background_color));
    APPLY_STYLE_FIELD(
        WOTBMOD_V3_UI_STYLE_OPACITY,
        UiSetOpacity(mod, control, patch.opacity));
    APPLY_STYLE_FIELD(
        WOTBMOD_V3_UI_STYLE_FONT,
        UiSetFont(mod, control, patch.font_uri));
    APPLY_STYLE_FIELD(
        WOTBMOD_V3_UI_STYLE_FONT_SIZE,
        UiSetFontSize(mod, control, patch.font_size));
    APPLY_STYLE_FIELD(
        WOTBMOD_V3_UI_STYLE_TEXTURE,
        UiSetTexture(mod, control, patch.texture_uri));
    APPLY_STYLE_FIELD(
        WOTBMOD_V3_UI_STYLE_Z_ORDER,
        UiSetZOrder(mod, control, patch.z_order));
#undef APPLY_STYLE_FIELD
    return WOTBMOD_V3_OK;
}

WotbModV3UiStylePatch PreviousStylePatch(
    const UiStyleOverride& style,
    uint32_t fields) {
    WotbModV3UiStylePatch patch = {};
    WOTBMOD_V3_INIT_STRUCT(patch, WOTBMOD_V3_UI_VERSION);
    patch.fields = fields;
    patch.color = style.previousColor;
    patch.background_color = style.previousBackground;
    patch.opacity = style.previousOpacity;
    patch.font_size = style.previousFontSize;
    patch.z_order = style.previousZOrder;
    patch.font_uri = style.previousFont.c_str();
    patch.texture_uri = style.previousTexture.c_str();
    return patch;
}

void OverlayStylePatch(
    WotbModV3UiStylePatch* destination,
    const WotbModV3UiStylePatch& source) {
    if (!destination) return;
    if (source.fields & WOTBMOD_V3_UI_STYLE_COLOR) {
        destination->color = source.color;
    }
    if (source.fields & WOTBMOD_V3_UI_STYLE_BACKGROUND_COLOR) {
        destination->background_color = source.background_color;
    }
    if (source.fields & WOTBMOD_V3_UI_STYLE_OPACITY) {
        destination->opacity = source.opacity;
    }
    if (source.fields & WOTBMOD_V3_UI_STYLE_FONT) {
        destination->font_uri = source.font_uri;
    }
    if (source.fields & WOTBMOD_V3_UI_STYLE_FONT_SIZE) {
        destination->font_size = source.font_size;
    }
    if (source.fields & WOTBMOD_V3_UI_STYLE_TEXTURE) {
        destination->texture_uri = source.texture_uri;
    }
    if (source.fields & WOTBMOD_V3_UI_STYLE_Z_ORDER) {
        destination->z_order = source.z_order;
    }
}

WotbModV3UiStylePatch ActiveStylePatch(
    const UiStyleOverride& style,
    uint32_t fields) {
    WotbModV3UiStylePatch patch = PreviousStylePatch(style, fields);
    WotbModV3UiStylePatch active = {};
    WOTBMOD_V3_INIT_STRUCT(active, WOTBMOD_V3_UI_VERSION);
    active.fields = style.fields;
    active.color = style.color;
    active.background_color = style.background;
    active.opacity = style.opacity;
    active.font_size = style.fontSize;
    active.z_order = style.zOrder;
    active.font_uri = style.font.c_str();
    active.texture_uri = style.texture.c_str();
    OverlayStylePatch(&patch, active);
    return patch;
}

void StoreStylePatch(
    UiStyleOverride* style,
    const WotbModV3UiStylePatch& patch) {
    if (!style) return;
    style->fields = patch.fields;
    style->priority = patch.priority;
    style->color = patch.color;
    style->background = patch.background_color;
    style->opacity = patch.opacity;
    style->fontSize = patch.font_size;
    style->zOrder = patch.z_order;
    style->font = patch.font_uri ? patch.font_uri : "";
    style->texture = patch.texture_uri ? patch.texture_uri : "";
}

void DestroyUiStyle(void* object) {
    UiStyleOverride* style = static_cast<UiStyleOverride*>(object);
    if (!style) return;
    if (style->active) {
        const WotbModV3UiStylePatch previous =
            PreviousStylePatch(*style, style->fields);
        ApplyStylePatch(style->owner, style->control, previous);
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto found = g_uiControls.find(style->control);
        if (found != g_uiControls.end() && found->second) {
            std::vector<WotbModV3Handle>& overrides =
                found->second->styleOverrides;
            overrides.erase(
                std::remove(overrides.begin(), overrides.end(), style->self),
                overrides.end());
        }
    }
    style->magic = 0u;
    delete style;
}

WotbModV3Result WOTBMOD_V3_CALL UiStylePush(
    WotbModV3Handle mod,
    WotbModV3UiHandle controlHandle,
    const WotbModV3UiStylePatch* patch,
    WotbModV3Handle* outOverride) {
    UiControl* control = nullptr;
    WotbModV3Result result =
        GetUiControl(mod, controlHandle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!ValidStruct(patch, WOTBMOD_V3_UI_VERSION) ||
        !ValidStylePatch(*patch) || !outOverride) {
        return Invalid(mod, "invalid UI style override");
    }
    UiStyleOverride* style = new (std::nothrow) UiStyleOverride();
    if (!style) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI style override allocation failed");
    }
    style->owner = mod;
    style->control = controlHandle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        style->previousColor = control->color;
        style->previousBackground = control->backgroundColor;
        style->previousOpacity = control->opacity;
        style->previousFontSize = control->fontSize;
        style->previousZOrder = control->zOrder;
        style->previousFont = control->font;
        style->previousTexture = control->texture;
    }
    StoreStylePatch(style, *patch);
    result = ApplyStylePatch(mod, controlHandle, *patch);
    if (result != WOTBMOD_V3_OK) {
        const WotbModV3UiStylePatch previous =
            PreviousStylePatch(*style, patch->fields);
        ApplyStylePatch(mod, controlHandle, previous);
        style->active = false;
        delete style;
        return result;
    }
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_STYLE_OVERRIDE,
        style,
        &DestroyUiStyle,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        const WotbModV3UiStylePatch previous =
            PreviousStylePatch(*style, patch->fields);
        ApplyStylePatch(mod, controlHandle, previous);
        style->active = false;
        delete style;
        return result;
    }
    style->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        control->styleOverrides.push_back(handle);
    }
    *outOverride = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiStyleUpdate(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    const WotbModV3UiStylePatch* patch) {
    if (!ValidStruct(patch, WOTBMOD_V3_UI_VERSION) ||
        !ValidStylePatch(*patch)) {
        return Invalid(mod, "invalid UI style update");
    }
    UiStyleOverride* style = nullptr;
    WotbModV3Result result = GetStyle(mod, handle, &style);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiControl* control = nullptr;
    result = GetUiControl(mod, style->control, &control);
    if (result != WOTBMOD_V3_OK) {
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "style target UI control was destroyed");
    }
    bool orderConflict = false;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        orderConflict = control->styleOverrides.empty() ||
            control->styleOverrides.back() != handle;
    }
    if (orderConflict) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "UI style overrides must be updated in stack order");
    }
    const uint32_t affected = style->fields | patch->fields;
    WotbModV3UiStylePatch desired =
        PreviousStylePatch(*style, affected);
    OverlayStylePatch(&desired, *patch);
    result = ApplyStylePatch(mod, style->control, desired);
    if (result != WOTBMOD_V3_OK) {
        const WotbModV3UiStylePatch rollback =
            ActiveStylePatch(*style, affected);
        ApplyStylePatch(mod, style->control, rollback);
        return result;
    }
    StoreStylePatch(style, *patch);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL UiStylePop(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    UiStyleOverride* style = nullptr;
    WotbModV3Result result = GetStyle(mod, handle, &style);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    UiControl* control = nullptr;
    result = GetUiControl(mod, style->control, &control);
    if (result != WOTBMOD_V3_OK) {
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "style target UI control was destroyed");
    }
    bool orderConflict = false;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        orderConflict = control->styleOverrides.empty() ||
            control->styleOverrides.back() != handle;
    }
    if (orderConflict) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "UI style overrides must be removed in stack order");
    }
    const WotbModV3UiStylePatch previous =
        PreviousStylePatch(*style, style->fields);
    result = ApplyStylePatch(mod, style->control, previous);
    if (result != WOTBMOD_V3_OK) return result;
    style->active = false;
    return ReleaseOwnedHandle(mod, handle);
}

struct SceneParameterValue {
    uint32_t type = 0;
    float values[16] = {};
    WotbModV3RenderHandle texture = WOTBMOD_V3_INVALID_HANDLE;
    std::string stringValue;
};

struct SceneAnimationState {
    float duration = 0.0f;
    float speed = 1.0f;
    uint32_t loop = 0;
};

struct SceneEntity {
    uint32_t magic = kSceneMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3SceneHandle self = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
    bool ownsNativeObject = true;
    bool gameOwned = false;
    std::string name;
    std::string resourceUri;
    WotbModV3SceneHandle parent = WOTBMOD_V3_INVALID_HANDLE;
    std::vector<WotbModV3SceneHandle> children;
    WotbModV3Transform local = {};
    WotbModV3SceneBounds bounds = {};
    uint32_t renderLayer = 0;
    int32_t renderOrder = 0;
    float lodBias = 0.0f;
    std::string attachmentNode;
    uint32_t attachmentPolicy =
        WOTBMOD_V3_ATTACHMENT_DETACH_ON_PARENT_DESTROY |
        WOTBMOD_V3_ATTACHMENT_KEEP_LOCAL_TRANSFORM;
    std::map<std::string, SceneAnimationState> animations;
    std::map<std::string, SceneParameterValue> parameters;
};

std::unordered_map<WotbModV3SceneHandle, SceneEntity*> g_sceneEntities;

WotbModV3Result CheckSceneModResourceAccess(WotbModV3Handle mod) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "resources.mod",
        WOTBMOD_V3_PERMISSION_SAFE);
    return permission == WOTBMOD_V3_OK
               ? CheckAccess(
                     mod,
                     WOTBMOD_V3_PERMISSION_SAFE,
                     WOTBMOD_V3_CONTEXT_ALL,
                     nullptr)
               : permission;
}

WotbModV3Result CheckSceneGameAccess(WotbModV3Handle mod) {
    const uint64_t context = CurrentContext();
    WotbModV3Result permission = WOTBMOD_V3_E_INCOMPATIBLE;
    uint32_t tier = WOTBMOD_V3_PERMISSION_SAFE;
    if (context == WOTBMOD_V3_CONTEXT_HANGAR) {
        permission = CheckNamedPermission(
            mod,
            "hangar.scene",
            WOTBMOD_V3_PERMISSION_SAFE);
    } else if (context == WOTBMOD_V3_CONTEXT_REPLAY) {
        permission = CheckEitherNamedPermission(
            mod,
            "camera.replay",
            WOTBMOD_V3_PERMISSION_SAFE,
            "battle.render.overlay",
            WOTBMOD_V3_PERMISSION_REVIEWED);
    } else if (context == WOTBMOD_V3_CONTEXT_BATTLE ||
               context == WOTBMOD_V3_CONTEXT_TRAINING) {
        tier = WOTBMOD_V3_PERMISSION_REVIEWED;
        permission = CheckNamedPermission(
            mod,
            "battle.render.overlay",
            WOTBMOD_V3_PERMISSION_REVIEWED);
    } else {
        return SetError(
            mod,
            WOTBMOD_V3_E_INCOMPATIBLE,
            "active game scene is unavailable in the current context");
    }
    return permission == WOTBMOD_V3_OK
               ? CheckAccess(mod, tier, context, nullptr)
               : permission;
}

WotbModV3Result CheckSceneMutationAccess(
    WotbModV3Handle mod,
    const SceneEntity* entity) {
    if (!entity) {
        return Invalid(mod, "scene entity is invalid");
    }
    return entity->gameOwned
               ? CheckSceneGameAccess(mod)
               : CheckSceneModResourceAccess(mod);
}

WotbModV3Result CheckSceneMutationAccess(
    WotbModV3Handle mod,
    const SceneEntity* first,
    const SceneEntity* second) {
    WotbModV3Result result = CheckSceneMutationAccess(mod, first);
    if (result != WOTBMOD_V3_OK || !second || second == first) {
        return result;
    }
    return CheckSceneMutationAccess(mod, second);
}

void DestroySceneEntity(void* object) {
    SceneEntity* entity = static_cast<SceneEntity*>(object);
    if (!entity) {
        return;
    }
    if (entity->ownsNativeObject) {
        ReleaseHostObject(
            entity->owner,
            "scene_entity_destroy",
            entity->nativeObject);
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto parentIt = g_sceneEntities.find(entity->parent);
    if (parentIt != g_sceneEntities.end()) {
        std::vector<WotbModV3SceneHandle>& siblings =
            parentIt->second->children;
        siblings.erase(
            std::remove(siblings.begin(), siblings.end(), entity->self),
            siblings.end());
    }
    for (WotbModV3SceneHandle childHandle : entity->children) {
        const auto childIt = g_sceneEntities.find(childHandle);
        if (childIt != g_sceneEntities.end() &&
            childIt->second->parent == entity->self) {
            childIt->second->parent = WOTBMOD_V3_INVALID_HANDLE;
            childIt->second->attachmentNode.clear();
        }
    }
    g_sceneEntities.erase(entity->self);
    entity->magic = 0;
    delete entity;
}

WotbModV3Result GetSceneEntity(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    SceneEntity** outEntity) {
    if (!outEntity) {
        return Invalid(mod, "scene entity output pointer is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_SCENE_ENTITY,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    SceneEntity* entity = static_cast<SceneEntity*>(object);
    if (!entity || entity->magic != kSceneMagic) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not a V3 scene entity");
    }
    *outEntity = entity;
    return WOTBMOD_V3_OK;
}

WotbModV3Vec4 QuaternionNormalize(WotbModV3Vec4 value) {
    const double length = std::sqrt(
        static_cast<double>(value.x) * value.x +
        static_cast<double>(value.y) * value.y +
        static_cast<double>(value.z) * value.z +
        static_cast<double>(value.w) * value.w);
    if (length <= 1.0e-9) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inverse = static_cast<float>(1.0 / length);
    return {
        value.x * inverse,
        value.y * inverse,
        value.z * inverse,
        value.w * inverse};
}

WotbModV3Vec4 QuaternionMultiply(
    const WotbModV3Vec4& left,
    const WotbModV3Vec4& right) {
    return QuaternionNormalize({
        left.w * right.x + left.x * right.w + left.y * right.z -
            left.z * right.y,
        left.w * right.y - left.x * right.z + left.y * right.w +
            left.z * right.x,
        left.w * right.z + left.x * right.y - left.y * right.x +
            left.z * right.w,
        left.w * right.w - left.x * right.x - left.y * right.y -
            left.z * right.z});
}

WotbModV3Vec3 QuaternionRotate(
    const WotbModV3Vec4& rotationValue,
    const WotbModV3Vec3& value) {
    const WotbModV3Vec4 rotation = QuaternionNormalize(rotationValue);
    const WotbModV3Vec3 q = {rotation.x, rotation.y, rotation.z};
    const WotbModV3Vec3 twiceCross = {
        2.0f * (q.y * value.z - q.z * value.y),
        2.0f * (q.z * value.x - q.x * value.z),
        2.0f * (q.x * value.y - q.y * value.x)};
    return {
        value.x + rotation.w * twiceCross.x +
            (q.y * twiceCross.z - q.z * twiceCross.y),
        value.y + rotation.w * twiceCross.y +
            (q.z * twiceCross.x - q.x * twiceCross.z),
        value.z + rotation.w * twiceCross.z +
            (q.x * twiceCross.y - q.y * twiceCross.x)};
}

WotbModV3Transform ComposeTransform(
    const WotbModV3Transform& parent,
    const WotbModV3Transform& local) {
    WotbModV3Transform result = {};
    WOTBMOD_V3_INIT_STRUCT(result, WOTBMOD_V3_ABI_VERSION);
    result.scale = {
        parent.scale.x * local.scale.x,
        parent.scale.y * local.scale.y,
        parent.scale.z * local.scale.z};
    result.rotation =
        QuaternionMultiply(parent.rotation, local.rotation);
    const WotbModV3Vec3 scaled = {
        local.position.x * parent.scale.x,
        local.position.y * parent.scale.y,
        local.position.z * parent.scale.z};
    const WotbModV3Vec3 rotated =
        QuaternionRotate(parent.rotation, scaled);
    result.position = {
        parent.position.x + rotated.x,
        parent.position.y + rotated.y,
        parent.position.z + rotated.z};
    return result;
}

WotbModV3Transform RelativeTransform(
    const WotbModV3Transform& parent,
    const WotbModV3Transform& world) {
    WotbModV3Transform result = {};
    WOTBMOD_V3_INIT_STRUCT(result, WOTBMOD_V3_ABI_VERSION);
    const WotbModV3Vec4 normalizedParent =
        QuaternionNormalize(parent.rotation);
    const WotbModV3Vec4 inverseRotation = {
        -normalizedParent.x,
        -normalizedParent.y,
        -normalizedParent.z,
        normalizedParent.w};
    const WotbModV3Vec3 offset = {
        world.position.x - parent.position.x,
        world.position.y - parent.position.y,
        world.position.z - parent.position.z};
    const WotbModV3Vec3 unrotated =
        QuaternionRotate(inverseRotation, offset);
    result.position = {
        parent.scale.x != 0.0f ? unrotated.x / parent.scale.x : 0.0f,
        parent.scale.y != 0.0f ? unrotated.y / parent.scale.y : 0.0f,
        parent.scale.z != 0.0f ? unrotated.z / parent.scale.z : 0.0f};
    result.rotation =
        QuaternionMultiply(inverseRotation, world.rotation);
    result.scale = {
        parent.scale.x != 0.0f ? world.scale.x / parent.scale.x : 0.0f,
        parent.scale.y != 0.0f ? world.scale.y / parent.scale.y : 0.0f,
        parent.scale.z != 0.0f ? world.scale.z / parent.scale.z : 0.0f};
    return result;
}

WotbModV3Result WorldTransformLocked(
    WotbModV3Handle mod,
    SceneEntity* entity,
    WotbModV3Transform* outTransform) {
    if (!entity || !outTransform) {
        return Invalid(mod, "world transform arguments are invalid");
    }
    std::vector<const SceneEntity*> lineage;
    const SceneEntity* cursor = entity;
    for (uint32_t depth = 0; cursor && depth < kMaxGraphDepth; ++depth) {
        lineage.push_back(cursor);
        if (cursor->parent == WOTBMOD_V3_INVALID_HANDLE) {
            cursor = nullptr;
            break;
        }
        const auto it = g_sceneEntities.find(cursor->parent);
        if (it == g_sceneEntities.end()) {
            cursor = nullptr;
            break;
        }
        cursor = it->second;
    }
    if (cursor) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "scene hierarchy exceeds maximum depth");
    }
    WotbModV3Transform world = lineage.back()->local;
    for (size_t index = lineage.size() - 1u; index > 0u; --index) {
        world = ComposeTransform(world, lineage[index - 1u]->local);
    }
    *outTransform = world;
    return WOTBMOD_V3_OK;
}

bool ValidSceneTransform(const WotbModV3Transform* transform) {
    return transform && transform->struct_size >= sizeof(*transform) &&
           transform->api_version == WOTBMOD_V3_ABI_VERSION &&
           IsFinite(*transform);
}

WotbModV3Result WOTBMOD_V3_CALL SceneEntityCreate(
    WotbModV3Handle mod,
    const WotbModV3SceneEntityDescriptor* descriptor,
    WotbModV3SceneHandle* outEntity) {
    const WotbModV3Result access = CheckSceneModResourceAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!ValidStruct(descriptor, WOTBMOD_V3_SCENE_VERSION) || !outEntity ||
        !ValidText(descriptor->name, WOTBMOD_V3_MAX_NAME) ||
        !ValidSceneTransform(&descriptor->transform)) {
        return Invalid(mod, "invalid scene entity descriptor");
    }
    SceneEntity* entity = new (std::nothrow) SceneEntity();
    if (!entity) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "scene entity allocation failed");
    }
    entity->owner = mod;
    entity->name = descriptor->name;
    entity->local = descriptor->transform;
    entity->local.rotation =
        QuaternionNormalize(entity->local.rotation);
    entity->renderLayer = descriptor->render_layer;
    entity->renderOrder = descriptor->render_order;
    WOTBMOD_V3_INIT_STRUCT(
        entity->bounds,
        WOTBMOD_V3_SCENE_VERSION);
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = descriptor;
    request.payload_size = sizeof(*descriptor);
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_create",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (hostResult != WOTBMOD_V3_OK) {
        delete entity;
        return hostResult;
    }
    if (response.object == 0u) {
        delete entity;
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "scene backend returned an invalid entity object");
    }
    entity->nativeObject = response.object;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SCENE_ENTITY,
        entity,
        &DestroySceneEntity,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "scene_entity_destroy",
            entity->nativeObject);
        entity->nativeObject = 0u;
        delete entity;
        return result;
    }
    entity->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_sceneEntities[handle] = entity;
    }
    *outEntity = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneEntityLoad(
    WotbModV3Handle mod,
    const char* resourceUri,
    WotbModV3SceneHandle* outEntity) {
    const WotbModV3Result permission =
        CheckSceneModResourceAccess(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(resourceUri, WOTBMOD_V3_MAX_PATH, false) ||
        !outEntity) {
        return Invalid(mod, "scene load arguments are invalid");
    }
    SceneEntity* entity = new (std::nothrow) SceneEntity();
    if (!entity) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "scene entity allocation failed");
    }
    entity->owner = mod;
    entity->name = resourceUri;
    entity->resourceUri = resourceUri;
    WOTBMOD_V3_INIT_STRUCT(
        entity->local,
        WOTBMOD_V3_ABI_VERSION);
    entity->local.rotation.w = 1.0f;
    entity->local.scale = {1.0f, 1.0f, 1.0f};
    WOTBMOD_V3_INIT_STRUCT(
        entity->bounds,
        WOTBMOD_V3_SCENE_VERSION);
    std::string resolvedFilePath;
    WotbModV3Result result =
        ResolveVirtualFilePath(mod, resourceUri, &resolvedFilePath);
    if (result != WOTBMOD_V3_OK) {
        delete entity;
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = resourceUri;
    request.secondary_name = resolvedFilePath.empty()
        ? nullptr
        : resolvedFilePath.c_str();
    ClientHostObjectResponse response = MakeHostResponse();
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_load",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        delete entity;
        return result;
    }
    if (response.object == 0u) {
        delete entity;
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "scene backend returned an invalid loaded entity");
    }
    entity->nativeObject = response.object;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SCENE_ENTITY,
        entity,
        &DestroySceneEntity,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "scene_entity_destroy",
            entity->nativeObject);
        entity->nativeObject = 0u;
        delete entity;
        return result;
    }
    entity->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_sceneEntities[handle] = entity;
    }
    *outEntity = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneEntityClone(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    WotbModV3SceneHandle* outClone) {
    SceneEntity* source = nullptr;
    WotbModV3Result result = GetSceneEntity(mod, handle, &source);
    if (result != WOTBMOD_V3_OK || !outClone) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "scene clone output pointer is null");
    }
    result = CheckSceneMutationAccess(mod, source);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    SceneEntity copy;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        copy = *source;
    }
    copy.self = WOTBMOD_V3_INVALID_HANDLE;
    copy.parent = WOTBMOD_V3_INVALID_HANDLE;
    copy.children.clear();
    copy.name += ".clone";
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = source->nativeObject;
    ClientHostObjectResponse response = MakeHostResponse();
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_clone",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "scene backend returned an invalid clone object");
    }
    copy.nativeObject = response.object;
    copy.ownsNativeObject = true;
    copy.gameOwned = false;
    SceneEntity* clone =
        new (std::nothrow) SceneEntity(std::move(copy));
    if (!clone) {
        ReleaseHostObject(
            mod,
            "scene_entity_destroy",
            response.object);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "scene clone allocation failed");
    }
    WotbModV3Handle cloneHandle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SCENE_ENTITY,
        clone,
        &DestroySceneEntity,
        &cloneHandle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "scene_entity_destroy",
            clone->nativeObject);
        clone->nativeObject = 0u;
        delete clone;
        return result;
    }
    clone->self = cloneHandle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_sceneEntities[cloneHandle] = clone;
    }
    *outClone = cloneHandle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneEntityDestroy(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle) {
    SceneEntity* entity = nullptr;
    WotbModV3Result result = GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::vector<WotbModV3SceneHandle> destroyChildren;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (WotbModV3SceneHandle childHandle : entity->children) {
            const auto it = g_sceneEntities.find(childHandle);
            if (it != g_sceneEntities.end() &&
                (it->second->attachmentPolicy &
                 WOTBMOD_V3_ATTACHMENT_DESTROY_ON_PARENT_DESTROY) != 0u) {
                destroyChildren.push_back(childHandle);
            }
        }
    }
    for (WotbModV3SceneHandle child : destroyChildren) {
        result = SceneEntityDestroy(mod, child);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    if (entity->ownsNativeObject && entity->nativeObject != 0u) {
        ClientHostObjectRequest request = MakeHostRequest();
        request.object = entity->nativeObject;
        result = InvokeHost(
            mod,
            WOTBMOD_V3_PERMISSION_SAFE,
            WOTBMOD_V3_CONTEXT_ALL,
            "scene_entity_destroy",
            &request,
            sizeof(request),
            nullptr,
            0u);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    entity->nativeObject = 0u;
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result WOTBMOD_V3_CALL SceneGetActive(
    WotbModV3Handle mod,
    WotbModV3SceneHandle* outScene) {
    const WotbModV3Result permission = CheckSceneGameAccess(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outScene) {
        return Invalid(mod, "active scene output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_get_active",
        nullptr,
        0u,
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return NotFound(mod, "active scene is unavailable");
    }
    SceneEntity* entity = new (std::nothrow) SceneEntity();
    if (!entity) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "active scene wrapper allocation failed");
    }
    entity->owner = mod;
    entity->name = "active_scene";
    entity->nativeObject = response.object;
    entity->ownsNativeObject = false;
    entity->gameOwned = true;
    WOTBMOD_V3_INIT_STRUCT(
        entity->local,
        WOTBMOD_V3_ABI_VERSION);
    entity->local.rotation.w = 1.0f;
    entity->local.scale = {1.0f, 1.0f, 1.0f};
    WOTBMOD_V3_INIT_STRUCT(
        entity->bounds,
        WOTBMOD_V3_SCENE_VERSION);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SCENE_ENTITY,
        entity,
        &DestroySceneEntity,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        entity->nativeObject = 0u;
        delete entity;
        return result;
    }
    entity->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_sceneEntities[handle] = entity;
    }
    *outScene = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result SceneSetParentCore(
    WotbModV3Handle mod,
    SceneEntity* entity,
    SceneEntity* parent,
    bool preserveWorld) {
    if (!entity || entity == parent) {
        return Invalid(mod, "scene entity cannot parent itself");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    WotbModV3Transform previousWorld = {};
    if (preserveWorld) {
        const WotbModV3Result worldResult =
            WorldTransformLocked(mod, entity, &previousWorld);
        if (worldResult != WOTBMOD_V3_OK) {
            return worldResult;
        }
    }
    if (parent) {
        WotbModV3SceneHandle cursor = parent->self;
        uint32_t depth = 0;
        while (cursor != WOTBMOD_V3_INVALID_HANDLE &&
               depth++ < kMaxGraphDepth) {
            if (cursor == entity->self) {
                return SetError(
                    mod,
                    WOTBMOD_V3_E_CONFLICT,
                    "scene hierarchy cycle rejected");
            }
            const auto it = g_sceneEntities.find(cursor);
            cursor = it != g_sceneEntities.end()
                         ? it->second->parent
                         : WOTBMOD_V3_INVALID_HANDLE;
        }
        if (depth >= kMaxGraphDepth) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "scene hierarchy exceeds maximum depth");
        }
    }
    const auto oldParentIt = g_sceneEntities.find(entity->parent);
    if (oldParentIt != g_sceneEntities.end()) {
        std::vector<WotbModV3SceneHandle>& children =
            oldParentIt->second->children;
        children.erase(
            std::remove(children.begin(), children.end(), entity->self),
            children.end());
    }
    entity->parent =
        parent ? parent->self : WOTBMOD_V3_INVALID_HANDLE;
    if (parent &&
        std::find(
            parent->children.begin(),
            parent->children.end(),
            entity->self) == parent->children.end()) {
        parent->children.push_back(entity->self);
    }
    if (preserveWorld) {
        if (parent) {
            WotbModV3Transform parentWorld = {};
            const WotbModV3Result parentResult =
                WorldTransformLocked(mod, parent, &parentWorld);
            if (parentResult != WOTBMOD_V3_OK) {
                return parentResult;
            }
            entity->local =
                RelativeTransform(parentWorld, previousWorld);
        } else {
            entity->local = previousWorld;
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneGetParent(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    WotbModV3SceneHandle* outParent) {
    SceneEntity* entity = nullptr;
    const WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK || !outParent) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "scene parent output pointer is null");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    *outParent = entity->parent;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetParent(
    WotbModV3Handle mod,
    WotbModV3SceneHandle entityHandle,
    WotbModV3SceneHandle parentHandle) {
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, entityHandle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    SceneEntity* parent = nullptr;
    if (parentHandle != WOTBMOD_V3_INVALID_HANDLE) {
        result = GetSceneEntity(mod, parentHandle, &parent);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    result = CheckSceneMutationAccess(mod, entity, parent);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.related_object =
        parent ? parent->nativeObject : 0u;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_parent",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return SceneSetParentCore(mod, entity, parent, false);
}

WotbModV3Result WOTBMOD_V3_CALL SceneAddChild(
    WotbModV3Handle mod,
    WotbModV3SceneHandle parent,
    WotbModV3SceneHandle child) {
    return SceneSetParent(mod, child, parent);
}

WotbModV3Result WOTBMOD_V3_CALL SceneRemoveChild(
    WotbModV3Handle mod,
    WotbModV3SceneHandle parentHandle,
    WotbModV3SceneHandle childHandle) {
    SceneEntity* parent = nullptr;
    SceneEntity* child = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, parentHandle, &parent);
    if (result == WOTBMOD_V3_OK) {
        result = GetSceneEntity(mod, childHandle, &child);
    }
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, child, parent);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (child->parent != parent->self) {
            return NotFound(
                mod,
                "scene child is not attached to the specified parent");
        }
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = child->nativeObject;
    request.related_object = 0u;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_parent",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return SceneSetParentCore(mod, child, nullptr, false);
}

WotbModV3Result WOTBMOD_V3_CALL SceneAttachEx(
    WotbModV3Handle mod,
    WotbModV3SceneHandle entityHandle,
    WotbModV3SceneHandle parentHandle,
    const char* node,
    uint32_t policy) {
    const uint32_t destructionMask =
        WOTBMOD_V3_ATTACHMENT_DETACH_ON_PARENT_DESTROY |
        WOTBMOD_V3_ATTACHMENT_DESTROY_ON_PARENT_DESTROY;
    const uint32_t transformMask =
        WOTBMOD_V3_ATTACHMENT_KEEP_WORLD_TRANSFORM |
        WOTBMOD_V3_ATTACHMENT_KEEP_LOCAL_TRANSFORM;
    const uint32_t knownMask = destructionMask | transformMask;
    if ((policy & ~knownMask) != 0u ||
        (policy & destructionMask) == destructionMask ||
        (policy & transformMask) == transformMask ||
        (policy & destructionMask) == 0u ||
        (policy & transformMask) == 0u ||
        !ValidText(node, WOTBMOD_V3_MAX_PATH)) {
        return Invalid(mod, "invalid scene attachment policy");
    }
    SceneEntity* entity = nullptr;
    SceneEntity* parent = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, entityHandle, &entity);
    if (result == WOTBMOD_V3_OK) {
        result = GetSceneEntity(mod, parentHandle, &parent);
    }
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity, parent);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.related_object = parent->nativeObject;
    request.flags = policy;
    request.name = node;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_attach_ex",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = SceneSetParentCore(
        mod,
        entity,
        parent,
        (policy & WOTBMOD_V3_ATTACHMENT_KEEP_WORLD_TRANSFORM) != 0u);
    if (result == WOTBMOD_V3_OK) {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        entity->attachmentNode = node;
        entity->attachmentPolicy = policy;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetTransform(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const WotbModV3Transform* transform) {
    if (!ValidSceneTransform(transform)) {
        return Invalid(mod, "invalid scene transform");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.payload = transform;
    request.payload_size = sizeof(*transform);
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_transform",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    entity->local = *transform;
    entity->local.rotation =
        QuaternionNormalize(entity->local.rotation);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneGetTransform(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    WotbModV3Transform* outTransform) {
    SceneEntity* entity = nullptr;
    const WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK || !outTransform) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "scene transform output pointer is null");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    WotbModV3Transform nativeTransform = {};
    WOTBMOD_V3_INIT_STRUCT(
        nativeTransform,
        WOTBMOD_V3_ABI_VERSION);
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_get_transform",
        &request,
        sizeof(request),
        &nativeTransform,
        sizeof(nativeTransform));
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    if (!ValidSceneTransform(&nativeTransform)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "scene backend returned an invalid transform");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    entity->local = nativeTransform;
    *outTransform = nativeTransform;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneGetWorldTransform(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    WotbModV3Transform* outTransform) {
    SceneEntity* entity = nullptr;
    const WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK || !outTransform) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(
                         mod,
                         "world transform output pointer is null");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    WOTBMOD_V3_INIT_STRUCT(
        *outTransform,
        WOTBMOD_V3_ABI_VERSION);
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_get_world_transform",
        &request,
        sizeof(request),
        outTransform,
        sizeof(*outTransform));
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    return ValidSceneTransform(outTransform)
        ? WOTBMOD_V3_OK
        : SetError(
              mod,
              WOTBMOD_V3_E_PLATFORM,
              "scene backend returned an invalid world transform");
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetWorldTransform(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const WotbModV3Transform* transform) {
    if (!ValidSceneTransform(transform)) {
        return Invalid(mod, "invalid world transform");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.payload = transform;
    request.payload_size = sizeof(*transform);
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_world_transform",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto parentIt = g_sceneEntities.find(entity->parent);
    if (parentIt == g_sceneEntities.end()) {
        entity->local = *transform;
    } else {
        WotbModV3Transform parentWorld = {};
        const WotbModV3Result worldResult =
            WorldTransformLocked(mod, parentIt->second, &parentWorld);
        if (worldResult != WOTBMOD_V3_OK) {
            return worldResult;
        }
        entity->local = RelativeTransform(parentWorld, *transform);
    }
    entity->local.rotation =
        QuaternionNormalize(entity->local.rotation);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneGetBounds(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    WotbModV3SceneBounds* outBounds) {
    SceneEntity* entity = nullptr;
    const WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK || !outBounds) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "scene bounds output pointer is null");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    WOTBMOD_V3_INIT_STRUCT(
        *outBounds,
        WOTBMOD_V3_SCENE_VERSION);
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_get_bounds",
        &request,
        sizeof(request),
        outBounds,
        sizeof(*outBounds));
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    if (!IsFinite(outBounds->minimum) ||
        !IsFinite(outBounds->maximum)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "scene backend returned invalid bounds");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    entity->bounds = *outBounds;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetRenderLayer(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    uint32_t layer) {
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.selector = layer;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_render_layer",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    entity->renderLayer = layer;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetRenderOrder(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    int32_t order) {
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.signed_value = order;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_render_order",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    entity->renderOrder = order;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetLodBias(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    float bias) {
    if (!IsFinite(bias) || bias < -16.0f || bias > 16.0f) {
        return Invalid(mod, "scene LOD bias is out of range");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.scalar0 = static_cast<double>(bias);
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_lod_bias",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    entity->lodBias = bias;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneListNodes(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    WotbModV3SceneNameVisitor visitor,
    void* userData) {
    SceneEntity* entity = nullptr;
    const WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK || !visitor) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "scene node visitor is null");
    }
    ClientHostSceneNameVisitorRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.object = entity->nativeObject;
    request.mod = mod;
    request.visitor = visitor;
    request.user_data = userData;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_list_nodes",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL SceneFindNodeByPath(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const char* path,
    WotbModV3SceneHandle* outNode) {
    if (!ValidText(path, WOTBMOD_V3_MAX_PATH, false) || !outNode) {
        return Invalid(mod, "scene path arguments are invalid");
    }
    SceneEntity* root = nullptr;
    const WotbModV3Result result =
        GetSceneEntity(mod, handle, &root);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = root->nativeObject;
    request.name = path;
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_find_node_by_path",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    if (response.object == 0u) {
        return NotFound(mod, "scene node path was not found");
    }
    SceneEntity* node = new (std::nothrow) SceneEntity();
    if (!node) {
        if (!root->gameOwned) {
            ReleaseHostObject(
                mod,
                "scene_entity_destroy",
                response.object);
        }
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "scene node wrapper allocation failed");
    }
    node->owner = mod;
    node->name = path;
    node->nativeObject = response.object;
    node->ownsNativeObject = !root->gameOwned;
    node->gameOwned = root->gameOwned;
    WOTBMOD_V3_INIT_STRUCT(node->local, WOTBMOD_V3_ABI_VERSION);
    node->local.rotation.w = 1.0f;
    node->local.scale = {1.0f, 1.0f, 1.0f};
    WOTBMOD_V3_INIT_STRUCT(
        node->bounds,
        WOTBMOD_V3_SCENE_VERSION);
    WotbModV3Handle nodeHandle = WOTBMOD_V3_INVALID_HANDLE;
    hostResult = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SCENE_ENTITY,
        node,
        &DestroySceneEntity,
        &nodeHandle);
    if (hostResult != WOTBMOD_V3_OK) {
        if (node->ownsNativeObject) {
            ReleaseHostObject(
                mod,
                "scene_entity_destroy",
                node->nativeObject);
        }
        node->nativeObject = 0u;
        delete node;
        return hostResult;
    }
    node->self = nodeHandle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_sceneEntities[nodeHandle] = node;
    }
    *outNode = nodeHandle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneListAnimations(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    WotbModV3SceneNameVisitor visitor,
    void* userData) {
    SceneEntity* entity = nullptr;
    const WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK || !visitor) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "animation visitor is null");
    }
    ClientHostSceneNameVisitorRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.object = entity->nativeObject;
    request.mod = mod;
    request.visitor = visitor;
    request.user_data = userData;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_list_animations",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result FindAnimation(
    WotbModV3Handle mod,
    SceneEntity* entity,
    const char* animation,
    SceneAnimationState** outState) {
    if (!ValidText(animation, WOTBMOD_V3_MAX_NAME, false) ||
        !outState) {
        return Invalid(mod, "animation arguments are invalid");
    }
    const auto it = entity->animations.find(animation);
    if (it == entity->animations.end()) {
        return NotFound(mod, "animation is not present on the entity");
    }
    *outState = &it->second;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneGetAnimationDuration(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const char* animation,
    float* outSeconds) {
    SceneEntity* entity = nullptr;
    WotbModV3Result result = GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK || !outSeconds) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(
                         mod,
                         "animation duration output pointer is null");
    }
    if (!ValidText(animation, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "animation name is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.name = animation;
    ClientHostObjectResponse response = MakeHostResponse();
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_get_animation_duration",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!IsFinite(response.value_f64) ||
        response.value_f64 < 0.0 ||
        response.value_f64 >
            static_cast<double>(
                std::numeric_limits<float>::max())) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "scene backend returned invalid animation duration");
    }
    *outSeconds = static_cast<float>(response.value_f64);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetAnimationSpeed(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const char* animation,
    float speed) {
    if (!IsFinite(speed) || speed < 0.0f || speed > 16.0f) {
        return Invalid(mod, "animation speed is out of range");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result = GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!ValidText(animation, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "animation name is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.name = animation;
    request.scalar0 = static_cast<double>(speed);
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_animation_speed",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result == WOTBMOD_V3_OK) {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        entity->animations[animation].speed = speed;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetAnimationLoop(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const char* animation,
    uint32_t loop) {
    if (loop > 1u) {
        return Invalid(mod, "animation loop must be 0 or 1");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result = GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!ValidText(animation, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "animation name is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.name = animation;
    request.flags = loop;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_set_animation_loop",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result == WOTBMOD_V3_OK) {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        entity->animations[animation].loop = loop;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SceneBlendAnimation(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const char* fromAnimation,
    const char* toAnimation,
    float durationSeconds) {
    if (!IsFinite(durationSeconds) || durationSeconds < 0.0f) {
        return Invalid(mod, "animation blend duration is invalid");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result = GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!ValidText(fromAnimation, WOTBMOD_V3_MAX_NAME, false) ||
        !ValidText(toAnimation, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "animation blend names are invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.name = fromAnimation;
    request.secondary_name = toAnimation;
    request.scalar0 = durationSeconds;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_blend_animation",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

bool ConvertSceneParameter(
    const WotbModV3SceneParameter& parameter,
    SceneParameterValue* outValue) {
    if (!outValue ||
        parameter.type < WOTBMOD_V3_SCENE_PARAMETER_FLOAT ||
        parameter.type > WOTBMOD_V3_SCENE_PARAMETER_TEXTURE_URI) {
        return false;
    }
    SceneParameterValue value;
    value.type = parameter.type;
    switch (parameter.type) {
        case WOTBMOD_V3_SCENE_PARAMETER_FLOAT:
            value.values[0] = parameter.value.scalar;
            if (!IsFinite(value.values[0])) {
                return false;
            }
            break;
        case WOTBMOD_V3_SCENE_PARAMETER_VEC2:
            value.values[0] = parameter.value.vec2.x;
            value.values[1] = parameter.value.vec2.y;
            if (!IsFinite(parameter.value.vec2)) {
                return false;
            }
            break;
        case WOTBMOD_V3_SCENE_PARAMETER_VEC3:
            value.values[0] = parameter.value.vec3.x;
            value.values[1] = parameter.value.vec3.y;
            value.values[2] = parameter.value.vec3.z;
            if (!IsFinite(parameter.value.vec3)) {
                return false;
            }
            break;
        case WOTBMOD_V3_SCENE_PARAMETER_VEC4:
            value.values[0] = parameter.value.vec4.x;
            value.values[1] = parameter.value.vec4.y;
            value.values[2] = parameter.value.vec4.z;
            value.values[3] = parameter.value.vec4.w;
            if (!IsFinite(parameter.value.vec4)) {
                return false;
            }
            break;
        case WOTBMOD_V3_SCENE_PARAMETER_COLOR:
            value.values[0] = parameter.value.color.r;
            value.values[1] = parameter.value.color.g;
            value.values[2] = parameter.value.color.b;
            value.values[3] = parameter.value.color.a;
            if (!IsFinite(parameter.value.color)) {
                return false;
            }
            break;
        case WOTBMOD_V3_SCENE_PARAMETER_MATRIX4:
            std::memcpy(
                value.values,
                parameter.value.matrix4.values,
                sizeof(value.values));
            for (float matrixValue : value.values) {
                if (!IsFinite(matrixValue)) {
                    return false;
                }
            }
            break;
        case WOTBMOD_V3_SCENE_PARAMETER_TEXTURE_URI:
            if (!ValidText(
                    parameter.value.texture_uri,
                    WOTBMOD_V3_MAX_PATH,
                    false)) {
                return false;
            }
            value.stringValue = parameter.value.texture_uri;
            break;
        default:
            return false;
    }
    *outValue = std::move(value);
    return true;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetMaterialParameter(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const char* materialPath,
    const WotbModV3SceneParameter* parameter) {
    if (!ValidText(materialPath, WOTBMOD_V3_MAX_PATH) ||
        !ValidStruct(parameter, WOTBMOD_V3_SCENE_VERSION) ||
        !ValidText(parameter->name, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "invalid scene material parameter");
    }
    SceneParameterValue value;
    if (!ConvertSceneParameter(*parameter, &value)) {
        return Invalid(mod, "scene material parameter value is invalid");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    const std::string key =
        std::string(materialPath) + "\n" + parameter->name;
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.name = materialPath;
    request.secondary_name = parameter->name;
    request.payload = parameter;
    request.payload_size = sizeof(*parameter);
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        materialPath[0]
            ? "scene_entity_set_material_parameter"
            : "scene_entity_set_shader_parameter",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    entity->parameters[key] = std::move(value);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneClearMaterialParameter(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const char* materialPath,
    const char* parameterName) {
    if (!ValidText(materialPath, WOTBMOD_V3_MAX_PATH) ||
        !ValidText(parameterName, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "invalid material parameter key");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, handle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    const std::string key =
        std::string(materialPath) + "\n" + parameterName;
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.name = materialPath;
    request.secondary_name = parameterName;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "scene_entity_clear_material_parameter",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (entity->parameters.erase(key) == 0u) {
        return NotFound(mod, "material parameter override was not found");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneSetShaderParameter(
    WotbModV3Handle mod,
    WotbModV3SceneHandle handle,
    const WotbModV3SceneParameter* parameter) {
    return SceneSetMaterialParameter(
        mod,
        handle,
        "",
        parameter);
}

struct RenderParameterValue {
    uint32_t type = 0;
    float values[16] = {};
    WotbModV3RenderHandle texture = WOTBMOD_V3_INVALID_HANDLE;
};

struct RenderResource {
    uint32_t magic = kRenderMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3RenderHandle self = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
    uint32_t kind = 0;
    std::string debugName;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t rowPitch = 0;
    uint32_t dynamic = 0;
    std::vector<uint8_t> pixels;
    std::string shaderUri;
    uint32_t blendEnabled = 0;
    uint32_t depthTestEnabled = 0;
    std::map<std::string, RenderParameterValue> parameters;
};

WotbModV3Result CheckRenderCallbacksPermission(WotbModV3Handle mod) {
    return CheckNamedPermission(
        mod,
        "render.callbacks",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

WotbModV3Result CheckRenderOverlayPermission(WotbModV3Handle mod) {
    return CheckNamedPermission(
        mod,
        "battle.render.overlay",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

void DestroyRenderResource(void* object) {
    RenderResource* resource = static_cast<RenderResource*>(object);
    if (resource) {
        ReleaseHostObject(
            resource->owner,
            resource->kind == WOTBMOD_V3_RENDER_RESOURCE_TEXTURE
                ? "render_destroy_texture"
                : "render_destroy_material",
            resource->nativeObject);
        resource->magic = 0;
        delete resource;
    }
}

WotbModV3Result GetRenderResource(
    WotbModV3Handle mod,
    WotbModV3RenderHandle handle,
    uint32_t expectedKind,
    RenderResource** outResource) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outResource) {
        return Invalid(mod, "render resource output pointer is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_RENDER_RESOURCE,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    RenderResource* resource = static_cast<RenderResource*>(object);
    if (!resource || resource->magic != kRenderMagic ||
        resource->kind != expectedKind) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "render resource has a different type");
    }
    *outResource = resource;
    return WOTBMOD_V3_OK;
}

uint32_t TextureBytesPerPixel(uint32_t format) {
    switch (format) {
        case WOTBMOD_V3_TEXTURE_RGBA8_UNORM:
        case WOTBMOD_V3_TEXTURE_BGRA8_UNORM:
            return 4u;
        case WOTBMOD_V3_TEXTURE_R8_UNORM:
            return 1u;
        case WOTBMOD_V3_TEXTURE_RGBA16_FLOAT:
            return 8u;
        default:
            return 0u;
    }
}

WotbModV3Result WOTBMOD_V3_CALL RenderRegisterCallback(
    WotbModV3Handle mod,
    uint32_t phase,
    int32_t priority,
    WotbModV3RenderCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    const WotbModV3Result namedPermission =
        CheckRenderCallbacksPermission(mod);
    if (namedPermission != WOTBMOD_V3_OK) {
        return namedPermission;
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (phase > WOTBMOD_V3_RENDER_PHASE_PRESENT || !callback ||
        !outToken) {
        return Invalid(mod, "invalid render callback registration");
    }
    *outToken = WOTBMOD_V3_INVALID_HANDLE;
    if (phase != WOTBMOD_V3_RENDER_PHASE_PRESENT) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "only the PRESENT render phase has a real client ingress");
    }
    ClientHostBackend backend = {};
    if (!GetHostBackend(&backend)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "render callback backend is not installed");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "render subscription allocation failed");
    }
    subscription->kind = SubscriptionKind::Render;
    subscription->phase = phase;
    subscription->priority = priority;
    subscription->renderCallback = callback;
    subscription->userData = userData;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL RenderUnregisterCallback(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    const WotbModV3Result permission =
        CheckRenderCallbacksPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    const WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::Render,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL RenderSetCallbackPriority(
    WotbModV3Handle mod,
    WotbModV3Token token,
    int32_t priority) {
    const WotbModV3Result permission =
        CheckRenderCallbacksPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    const WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::Render,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::unique_lock<std::recursive_mutex> operationLock(
        subscription->operationMutex);
    if (subscription->destroying ||
        subscription->unregistered ||
        subscription->unregistering) {
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "render subscription is no longer active");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    subscription->priority = priority;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetBackend(
    WotbModV3Handle mod,
    uint32_t* outBackend) {
    const WotbModV3Result permission =
        CheckRenderCallbacksPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    const WotbModV3Result result = CheckMod(mod);
    if (result != WOTBMOD_V3_OK || !outBackend) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(mod, "render backend output pointer is null");
    }
    *outBackend = WOTBMOD_V3_RENDER_BACKEND_NONE;
    ClientHostFrame frame = {};
    if (!GetHostFrame(&frame) ||
        frame.render_backend == WOTBMOD_V3_RENDER_BACKEND_NONE) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "render backend is unavailable before a real host frame");
    }
    *outBackend = frame.render_backend;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetViewport(
    WotbModV3Handle mod,
    WotbModV3Rect* outViewport) {
    const WotbModV3Result namedPermission =
        CheckRenderCallbacksPermission(mod);
    if (namedPermission != WOTBMOD_V3_OK) {
        return namedPermission;
    }
    if (!outViewport) {
        return Invalid(mod, "render viewport output pointer is null");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    ClientHostFrame frame = {};
    if (!GetHostFrame(&frame)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "render viewport is unavailable before a host frame");
    }
    *outViewport = frame.viewport;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetFrameIndex(
    WotbModV3Handle mod,
    uint64_t* outFrameIndex) {
    const WotbModV3Result permission =
        CheckRenderCallbacksPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    const WotbModV3Result result = CheckMod(mod);
    if (result != WOTBMOD_V3_OK || !outFrameIndex) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(
                         mod,
                         "render frame index output pointer is null");
    }
    *outFrameIndex = CurrentFrameIndex();
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetDeltaTime(
    WotbModV3Handle mod,
    double* outDeltaSeconds) {
    const WotbModV3Result permission =
        CheckRenderCallbacksPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    const WotbModV3Result result = CheckMod(mod);
    if (result != WOTBMOD_V3_OK || !outDeltaSeconds) {
        return result != WOTBMOD_V3_OK
                   ? result
                   : Invalid(
                         mod,
                         "render delta output pointer is null");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    *outDeltaSeconds = g_lastDeltaSeconds;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderCreateTexture(
    WotbModV3Handle mod,
    const WotbModV3TextureDescriptor* descriptor,
    WotbModV3RenderHandle* outTexture) {
    const WotbModV3Result namedPermission =
        CheckRenderOverlayPermission(mod);
    if (namedPermission != WOTBMOD_V3_OK) {
        return namedPermission;
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!ValidStruct(descriptor, WOTBMOD_V3_RENDER_VERSION) ||
        !outTexture || descriptor->width == 0u ||
        descriptor->height == 0u ||
        descriptor->width > kMaxTextureDimension ||
        descriptor->height > kMaxTextureDimension ||
        descriptor->dynamic > 1u ||
        (descriptor->debug_name &&
         !ValidText(descriptor->debug_name, WOTBMOD_V3_MAX_NAME))) {
        return Invalid(mod, "invalid render texture descriptor");
    }
    const uint32_t bytesPerPixel =
        TextureBytesPerPixel(descriptor->format);
    if (bytesPerPixel == 0u ||
        descriptor->width >
            std::numeric_limits<uint32_t>::max() / bytesPerPixel) {
        return Invalid(mod, "unsupported texture format or dimensions");
    }
    const uint32_t minimumPitch = descriptor->width * bytesPerPixel;
    const uint32_t rowPitch =
        descriptor->row_pitch ? descriptor->row_pitch : minimumPitch;
    if (rowPitch < minimumPitch ||
        descriptor->height >
            std::numeric_limits<uint32_t>::max() / rowPitch) {
        return Invalid(mod, "texture row pitch is invalid");
    }
    const uint32_t requiredBytes = rowPitch * descriptor->height;
    if ((descriptor->initial_data && descriptor->initial_data_size <
                                         requiredBytes) ||
        (!descriptor->initial_data &&
         descriptor->initial_data_size != 0u)) {
        return Invalid(mod, "texture initial data size is invalid");
    }
    RenderResource* resource =
        new (std::nothrow) RenderResource();
    if (!resource) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "render texture allocation failed");
    }
    resource->owner = mod;
    resource->kind = WOTBMOD_V3_RENDER_RESOURCE_TEXTURE;
    resource->debugName =
        descriptor->debug_name ? descriptor->debug_name : "";
    resource->width = descriptor->width;
    resource->height = descriptor->height;
    resource->format = descriptor->format;
    resource->rowPitch = rowPitch;
    resource->dynamic = descriptor->dynamic;
    try {
        resource->pixels.resize(requiredBytes);
    } catch (...) {
        delete resource;
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "render texture pixel allocation failed");
    }
    if (descriptor->initial_data) {
        std::memcpy(
            resource->pixels.data(),
            descriptor->initial_data,
            requiredBytes);
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = descriptor;
    request.payload_size = sizeof(*descriptor);
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_create_texture",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (hostResult != WOTBMOD_V3_OK) {
        delete resource;
        return hostResult;
    }
    if (response.object == 0u) {
        delete resource;
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "render backend returned an invalid texture object");
    }
    resource->nativeObject = response.object;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RENDER_RESOURCE,
        resource,
        &DestroyRenderResource,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "render_destroy_texture",
            resource->nativeObject);
        resource->nativeObject = 0u;
        delete resource;
        return result;
    }
    resource->self = handle;
    *outTexture = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderUpdateTexture(
    WotbModV3Handle mod,
    WotbModV3RenderHandle handle,
    const WotbModV3Rect* region,
    const void* data,
    uint32_t dataSize,
    uint32_t rowPitch) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!data || dataSize == 0u) {
        return Invalid(mod, "texture update data is empty");
    }
    RenderResource* texture = nullptr;
    WotbModV3Result result = GetRenderResource(
        mod,
        handle,
        WOTBMOD_V3_RENDER_RESOURCE_TEXTURE,
        &texture);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    const uint32_t bytesPerPixel =
        TextureBytesPerPixel(texture->format);
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = texture->width;
    uint32_t height = texture->height;
    if (region) {
        if (!IsFinite(*region) || region->x < 0.0f ||
            region->y < 0.0f || region->width <= 0.0f ||
            region->height <= 0.0f ||
            std::floor(region->x) != region->x ||
            std::floor(region->y) != region->y ||
            std::floor(region->width) != region->width ||
            std::floor(region->height) != region->height) {
            return Invalid(mod, "texture update region is invalid");
        }
        x = static_cast<uint32_t>(region->x);
        y = static_cast<uint32_t>(region->y);
        width = static_cast<uint32_t>(region->width);
        height = static_cast<uint32_t>(region->height);
        if (x > texture->width || y > texture->height ||
            width > texture->width - x ||
            height > texture->height - y) {
            return Invalid(mod, "texture update region is out of bounds");
        }
    }
    const uint32_t minimumPitch = width * bytesPerPixel;
    const uint32_t sourcePitch = rowPitch ? rowPitch : minimumPitch;
    if (sourcePitch < minimumPitch ||
        height > std::numeric_limits<uint32_t>::max() / sourcePitch ||
        dataSize < sourcePitch * height) {
        return Invalid(mod, "texture update row pitch or size is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = texture->nativeObject;
    request.selector = region ? 1u : 0u;
    request.scalar0 = static_cast<double>(sourcePitch);
    request.vector = {
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(width),
        static_cast<float>(height)};
    request.payload = data;
    request.payload_size = dataSize;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_update_texture",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    const uint8_t* source = static_cast<const uint8_t*>(data);
    std::lock_guard<std::mutex> lock(g_clientMutex);
    for (uint32_t row = 0; row < height; ++row) {
        uint8_t* destination =
            texture->pixels.data() +
            static_cast<size_t>(y + row) * texture->rowPitch +
            static_cast<size_t>(x) * bytesPerPixel;
        std::memcpy(
            destination,
            source + static_cast<size_t>(row) * sourcePitch,
            minimumPitch);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderDestroyTexture(
    WotbModV3Handle mod,
    WotbModV3RenderHandle handle) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    RenderResource* texture = nullptr;
    const WotbModV3Result result = GetRenderResource(
        mod,
        handle,
        WOTBMOD_V3_RENDER_RESOURCE_TEXTURE,
        &texture);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = texture->nativeObject;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_destroy_texture",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    texture->nativeObject = 0u;
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result WOTBMOD_V3_CALL RenderCreateMaterial(
    WotbModV3Handle mod,
    const WotbModV3MaterialDescriptor* descriptor,
    WotbModV3RenderHandle* outMaterial) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(descriptor, WOTBMOD_V3_RENDER_VERSION) ||
        !outMaterial ||
        !ValidText(
            descriptor->shader_uri,
            WOTBMOD_V3_MAX_PATH,
            false) ||
        (descriptor->debug_name &&
         !ValidText(descriptor->debug_name, WOTBMOD_V3_MAX_NAME)) ||
        descriptor->blend_enabled > 1u ||
        descriptor->depth_test_enabled > 1u) {
        return Invalid(mod, "invalid render material descriptor");
    }
    RenderResource* resource =
        new (std::nothrow) RenderResource();
    if (!resource) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "render material allocation failed");
    }
    resource->owner = mod;
    resource->kind = WOTBMOD_V3_RENDER_RESOURCE_MATERIAL;
    resource->debugName =
        descriptor->debug_name ? descriptor->debug_name : "";
    resource->shaderUri = descriptor->shader_uri;
    resource->blendEnabled = descriptor->blend_enabled;
    resource->depthTestEnabled = descriptor->depth_test_enabled;
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = descriptor;
    request.payload_size = sizeof(*descriptor);
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_create_material",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (hostResult != WOTBMOD_V3_OK) {
        delete resource;
        return hostResult;
    }
    if (response.object == 0u) {
        delete resource;
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "render backend returned an invalid material object");
    }
    resource->nativeObject = response.object;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RENDER_RESOURCE,
        resource,
        &DestroyRenderResource,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "render_destroy_material",
            resource->nativeObject);
        resource->nativeObject = 0u;
        delete resource;
        return result;
    }
    resource->self = handle;
    *outMaterial = handle;
    return WOTBMOD_V3_OK;
}

bool ConvertRenderParameter(
    WotbModV3Handle mod,
    const WotbModV3RenderParameter& parameter,
    RenderParameterValue* outValue) {
    if (!outValue ||
        parameter.type < WOTBMOD_V3_RENDER_PARAMETER_FLOAT ||
        parameter.type > WOTBMOD_V3_RENDER_PARAMETER_TEXTURE) {
        return false;
    }
    RenderParameterValue value;
    value.type = parameter.type;
    switch (parameter.type) {
        case WOTBMOD_V3_RENDER_PARAMETER_FLOAT:
            value.values[0] = parameter.value.scalar;
            if (!IsFinite(value.values[0])) {
                return false;
            }
            break;
        case WOTBMOD_V3_RENDER_PARAMETER_VEC2:
            value.values[0] = parameter.value.vec2.x;
            value.values[1] = parameter.value.vec2.y;
            if (!IsFinite(parameter.value.vec2)) {
                return false;
            }
            break;
        case WOTBMOD_V3_RENDER_PARAMETER_VEC3:
            value.values[0] = parameter.value.vec3.x;
            value.values[1] = parameter.value.vec3.y;
            value.values[2] = parameter.value.vec3.z;
            if (!IsFinite(parameter.value.vec3)) {
                return false;
            }
            break;
        case WOTBMOD_V3_RENDER_PARAMETER_VEC4:
            value.values[0] = parameter.value.vec4.x;
            value.values[1] = parameter.value.vec4.y;
            value.values[2] = parameter.value.vec4.z;
            value.values[3] = parameter.value.vec4.w;
            if (!IsFinite(parameter.value.vec4)) {
                return false;
            }
            break;
        case WOTBMOD_V3_RENDER_PARAMETER_COLOR:
            value.values[0] = parameter.value.color.r;
            value.values[1] = parameter.value.color.g;
            value.values[2] = parameter.value.color.b;
            value.values[3] = parameter.value.color.a;
            if (!IsFinite(parameter.value.color)) {
                return false;
            }
            break;
        case WOTBMOD_V3_RENDER_PARAMETER_MATRIX4:
            std::memcpy(
                value.values,
                parameter.value.matrix4.values,
                sizeof(value.values));
            for (float matrixValue : value.values) {
                if (!IsFinite(matrixValue)) {
                    return false;
                }
            }
            break;
        case WOTBMOD_V3_RENDER_PARAMETER_TEXTURE: {
            RenderResource* texture = nullptr;
            if (GetRenderResource(
                    mod,
                    parameter.value.texture,
                    WOTBMOD_V3_RENDER_RESOURCE_TEXTURE,
                    &texture) != WOTBMOD_V3_OK) {
                return false;
            }
            value.texture = texture->self;
            break;
        }
        default:
            return false;
    }
    *outValue = value;
    return true;
}

WotbModV3Result WOTBMOD_V3_CALL RenderSetMaterialParameter(
    WotbModV3Handle mod,
    WotbModV3RenderHandle handle,
    const WotbModV3RenderParameter* parameter) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(parameter, WOTBMOD_V3_RENDER_VERSION) ||
        !ValidText(parameter->name, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "invalid render material parameter");
    }
    RenderResource* material = nullptr;
    WotbModV3Result result = GetRenderResource(
        mod,
        handle,
        WOTBMOD_V3_RENDER_RESOURCE_MATERIAL,
        &material);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    RenderParameterValue value;
    if (!ConvertRenderParameter(mod, *parameter, &value)) {
        return Invalid(mod, "render material parameter value is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = material->nativeObject;
    request.payload = parameter;
    request.payload_size = sizeof(*parameter);
    if (parameter->type == WOTBMOD_V3_RENDER_PARAMETER_TEXTURE) {
        RenderResource* texture = nullptr;
        result = GetRenderResource(
            mod,
            parameter->value.texture,
            WOTBMOD_V3_RENDER_RESOURCE_TEXTURE,
            &texture);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
        request.auxiliary_object = texture->nativeObject;
    }
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_set_material_parameter",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    material->parameters[parameter->name] = value;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderDestroyMaterial(
    WotbModV3Handle mod,
    WotbModV3RenderHandle handle) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    RenderResource* material = nullptr;
    const WotbModV3Result result = GetRenderResource(
        mod,
        handle,
        WOTBMOD_V3_RENDER_RESOURCE_MATERIAL,
        &material);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = material->nativeObject;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_destroy_material",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    material->nativeObject = 0u;
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result WOTBMOD_V3_CALL RenderDrawSprite(
    WotbModV3Handle mod,
    const WotbModV3DrawSprite* draw) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(draw, WOTBMOD_V3_RENDER_VERSION) ||
        !IsFinite(draw->destination) || !IsFinite(draw->source_uv) ||
        !IsFinite(draw->color) || !IsFinite(draw->rotation_radians) ||
        !IsFinite(draw->z)) {
        return Invalid(mod, "invalid sprite draw command");
    }
    RenderResource* texture = nullptr;
    WotbModV3Result result = GetRenderResource(
        mod,
        draw->texture,
        WOTBMOD_V3_RENDER_RESOURCE_TEXTURE,
        &texture);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    RenderResource* material = nullptr;
    if (draw->material != WOTBMOD_V3_INVALID_HANDLE) {
        result = GetRenderResource(
            mod,
            draw->material,
            WOTBMOD_V3_RENDER_RESOURCE_MATERIAL,
            &material);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = texture->nativeObject;
    request.related_object =
        material ? material->nativeObject : 0u;
    request.payload = draw;
    request.payload_size = sizeof(*draw);
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_draw_sprite",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL RenderDrawText(
    WotbModV3Handle mod,
    const WotbModV3DrawText* draw) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(draw, WOTBMOD_V3_RENDER_VERSION) ||
        !ValidText(draw->text, WOTBMOD_V3_MAX_MESSAGE) ||
        !ValidText(draw->font_uri, WOTBMOD_V3_MAX_PATH, false) ||
        !IsFinite(draw->position) || !IsFinite(draw->color) ||
        !IsFinite(draw->font_size) || draw->font_size <= 0.0f ||
        !IsFinite(draw->max_width) || draw->max_width < 0.0f) {
        return Invalid(mod, "invalid text draw command");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = draw;
    request.payload_size = sizeof(*draw);
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_draw_text",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL RenderDrawLine(
    WotbModV3Handle mod,
    const WotbModV3DrawLine* draw) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(draw, WOTBMOD_V3_RENDER_VERSION) ||
        !IsFinite(draw->from) || !IsFinite(draw->to) ||
        !IsFinite(draw->color) || !IsFinite(draw->width) ||
        draw->width <= 0.0f) {
        return Invalid(mod, "invalid line draw command");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = draw;
    request.payload_size = sizeof(*draw);
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_draw_line",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL RenderDrawMesh(
    WotbModV3Handle mod,
    const WotbModV3DrawMesh* draw) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(draw, WOTBMOD_V3_RENDER_VERSION)) {
        return Invalid(mod, "invalid mesh draw command");
    }
    SceneEntity* entity = nullptr;
    WotbModV3Result result =
        GetSceneEntity(mod, draw->scene_entity, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    RenderResource* material = nullptr;
    if (draw->material != WOTBMOD_V3_INVALID_HANDLE) {
        result = GetRenderResource(
            mod,
            draw->material,
            WOTBMOD_V3_RENDER_RESOURCE_MATERIAL,
            &material);
        if (result != WOTBMOD_V3_OK) {
            return result;
        }
    }
    for (float value : draw->world.values) {
        if (!IsFinite(value)) {
            return Invalid(mod, "mesh world matrix is invalid");
        }
    }
    if (entity->nativeObject == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "scene entity has no native render object");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity->nativeObject;
    request.related_object =
        draw->material != WOTBMOD_V3_INVALID_HANDLE
            ? material->nativeObject
            : 0u;
    request.payload = draw;
    request.payload_size = sizeof(*draw);
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_draw_mesh",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL RenderPushState(
    WotbModV3Handle mod) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_push_state",
        nullptr,
        0u,
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL RenderPopState(
    WotbModV3Handle mod) {
    const WotbModV3Result permission =
        CheckRenderOverlayPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_ALL,
        "render_pop_state",
        nullptr,
        0u,
        nullptr,
        0u);
}

WotbModV3Result RenderNativePointer(
    WotbModV3Handle mod,
    void** outPointer,
    uint32_t pointerKind,
    const char* operation) {
    const WotbModV3Result namedPermission = CheckNamedPermission(
        mod,
        "render.native",
        WOTBMOD_V3_PERMISSION_UNSAFE);
    if (namedPermission != WOTBMOD_V3_OK) {
        return namedPermission;
    }
    if (!outPointer) {
        return Invalid(mod, "native render pointer output is null");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_UNSAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    ClientHostFrame frame = {};
    if (!GetHostFrame(&frame)) {
        *outPointer = nullptr;
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            operation);
    }
    switch (pointerKind) {
        case 0u:
            *outPointer = frame.native_device;
            break;
        case 1u:
            *outPointer = frame.native_context;
            break;
        case 2u:
            *outPointer = frame.native_swapchain;
            break;
        default:
            *outPointer = nullptr;
            return Invalid(mod, "native render pointer kind is invalid");
    }
    if (!*outPointer) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            operation);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetNativeDevice(
    WotbModV3Handle mod,
    void** outDevice) {
    return RenderNativePointer(
        mod,
        outDevice,
        0u,
        "render_get_native_device");
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetNativeContext(
    WotbModV3Handle mod,
    void** outContext) {
    return RenderNativePointer(
        mod,
        outContext,
        1u,
        "render_get_native_context");
}

WotbModV3Result WOTBMOD_V3_CALL RenderGetNativeSwapchain(
    WotbModV3Handle mod,
    void** outSwapchain) {
    return RenderNativePointer(
        mod,
        outSwapchain,
        2u,
        "render_get_native_swapchain");
}

void ClientRenderFramePump(
    uint64_t frameIndex,
    double deltaSeconds,
    const ClientHostFrame* frameSnapshot) {
    struct CallbackCopy {
        WotbModV3Handle owner;
        uint32_t phase;
        int32_t priority;
        std::shared_ptr<SubscriptionCallbackState> state;
    };
    std::vector<CallbackCopy> callbacks;
    ClientHostFrame hostFrame = {};
    bool hasHostFrame = frameSnapshot != nullptr;
    if (frameSnapshot) {
        hostFrame = *frameSnapshot;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_lastDeltaSeconds = deltaSeconds;
        if (!frameSnapshot && g_hostFrameValid) {
            hostFrame = g_hostFrame;
            hasHostFrame = true;
        }
        callbacks.reserve(g_subscriptions.size());
        for (const auto& pair : g_subscriptions) {
            const Subscription* subscription = pair.second.get();
            if (subscription &&
                subscription->kind == SubscriptionKind::Render &&
                subscription->phase ==
                    WOTBMOD_V3_RENDER_PHASE_PRESENT &&
                subscription->callbackState) {
                callbacks.push_back(
                    {subscription->owner,
                     subscription->phase,
                     subscription->priority,
                     subscription->callbackState});
            }
        }
    }
    std::stable_sort(
        callbacks.begin(),
        callbacks.end(),
        [](const CallbackCopy& left, const CallbackCopy& right) {
            if (left.phase != right.phase) {
                return left.phase < right.phase;
            }
            return left.priority > right.priority;
        });
    for (const CallbackCopy& callback : callbacks) {
        if (!IsModEnabled(callback.owner) ||
            CheckRenderCallbacksPermission(callback.owner) !=
                WOTBMOD_V3_OK ||
            CheckAccess(
                callback.owner,
                WOTBMOD_V3_PERMISSION_REVIEWED,
                WOTBMOD_V3_CONTEXT_ALL,
                nullptr) != WOTBMOD_V3_OK) {
            continue;
        }
        SubscriptionCallbackLease lease(callback.state);
        if (!lease) {
            continue;
        }
        const WotbModV3RenderCallback invoke =
            callback.state->renderCallback;
        void* const userData = callback.state->userData;
        if (!invoke ||
            EnterModCallbackCoalescible(callback.owner) !=
                WOTBMOD_V3_OK) {
            continue;
        }
        WotbModV3RenderFrameInfo frame = {};
        WOTBMOD_V3_INIT_STRUCT(frame, WOTBMOD_V3_RENDER_VERSION);
        frame.phase = callback.phase;
        frame.backend = hasHostFrame
            ? hostFrame.render_backend
            : WOTBMOD_V3_RENDER_BACKEND_NONE;
        frame.frame_index = frameIndex;
        frame.delta_seconds = deltaSeconds;
        if (hasHostFrame) {
            frame.viewport = hostFrame.viewport;
        }
        const uint32_t previousRole =
            SetCurrentThreadRole(WOTBMOD_V3_THREAD_RENDER);
        try {
            invoke(
                callback.owner,
                &frame,
                userData);
        } catch (...) {
            SetError(
                callback.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "render callback threw an exception");
        }
        SetCurrentThreadRole(previousRole);
        LeaveModCallback(callback.owner);
    }
}

bool UiWorldRectLocked(
    const UiControl* control,
    WotbModV3Rect* outRect,
    uint32_t* outDepth) {
    if (!control || !outRect || !outDepth) return false;
    WotbModV3Rect rect = {
        control->position.x,
        control->position.y,
        control->size.x,
        control->size.y};
    if (!IsFinite(rect) || rect.width < 0.0f || rect.height < 0.0f ||
        control->visible == 0u || control->enabled == 0u ||
        control->interactable == 0u) {
        return false;
    }
    WotbModV3UiHandle parent = control->parent;
    uint32_t depth = 0u;
    while (parent != WOTBMOD_V3_INVALID_HANDLE &&
           depth < kMaxGraphDepth) {
        const auto found = g_uiControls.find(parent);
        if (found == g_uiControls.end() || !found->second ||
            found->second->visible == 0u ||
            found->second->enabled == 0u) {
            return false;
        }
        rect.x += found->second->position.x;
        rect.y += found->second->position.y;
        parent = found->second->parent;
        ++depth;
    }
    if (depth == kMaxGraphDepth &&
        parent != WOTBMOD_V3_INVALID_HANDLE) {
        return false;
    }
    *outRect = rect;
    *outDepth = depth;
    return true;
}

void RefreshUiCaptureSnapshot() {
    std::vector<WotbModV3Rect> next;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (g_clientServicesRunning) {
            next.reserve(g_subscriptions.size());
            for (const auto& pair : g_subscriptions) {
                const Subscription* subscription = pair.second.get();
                if (!subscription ||
                    subscription->magic != kSubscriptionMagic ||
                    subscription->kind != SubscriptionKind::UiEvent) {
                    continue;
                }
                const auto control =
                    g_uiControls.find(subscription->target);
                if (control == g_uiControls.end() || !control->second ||
                    control->second->gameOwned) {
                    continue;
                }
                WotbModV3Rect rect = {};
                uint32_t depth = 0u;
                if (UiWorldRectLocked(control->second, &rect, &depth)) {
                    next.push_back(rect);
                }
            }
        }
    }
    bool changed = false;
    std::vector<WotbModV3Rect> current;
    {
        std::lock_guard<std::mutex> captureLock(g_uiCaptureMutex);
        changed = next.size() != g_uiCaptureRects.size();
        for (size_t index = 0u; !changed && index < next.size(); ++index) {
            const WotbModV3Rect& a = next[index];
            const WotbModV3Rect& b = g_uiCaptureRects[index];
            changed = a.x != b.x || a.y != b.y ||
                      a.width != b.width || a.height != b.height;
        }
        g_uiCaptureRects.swap(next);
        if (changed) current = g_uiCaptureRects;
    }
    if (!changed) return;

    // AN INPUT-CAPTURE RECT IS INVISIBLE, SO SAY IT OUT LOUD.
    //
    // Every rect here swallows stock clicks: the loader's window hook returns
    // 0 for a press inside one, and the game never sees it. Nothing draws the
    // rect - RC1 does not render mod-created controls - so a mod that parks a
    // container over a stock button silently kills that button, with no
    // symptom beyond "this part of the UI stopped working". That is exactly
    // what sample.ui_transaction did at (20,20,260,80), on top of the
    // universal back button, for a whole session.
    //
    // Logging the set whenever it changes turns a mystery into one grep. It is
    // per-change, not per-frame, so an overlay that legitimately holds a rect
    // costs one line when it appears and one when it goes.
    if (current.empty()) {
        RuntimeLog(
            WOTBMOD_V3_LOG_INFO,
            "v3.client-services",
            "UI input capture released; stock clicks pass through again");
        return;
    }
    char message[256] = {};
    int written = std::snprintf(
        message,
        sizeof(message),
        "UI input capture now swallows stock clicks in %zu rect(s):",
        current.size());
    for (size_t index = 0u;
         index < current.size() && written > 0 &&
         static_cast<size_t>(written) < sizeof(message);
         ++index) {
        const int added = std::snprintf(
            message + written,
            sizeof(message) - static_cast<size_t>(written),
            " [%.1f,%.1f %.1fx%.1f]",
            current[index].x,
            current[index].y,
            current[index].width,
            current[index].height);
        if (added <= 0) break;
        written += added;
    }
    RuntimeLog(WOTBMOD_V3_LOG_WARNING, "v3.client-services", message);
}

WotbModV3UiHandle HitTestSubscribedUiLocked(
    WotbModV3Handle owner,
    const WotbModV3Vec2& pointer) {
    WotbModV3UiHandle best = WOTBMOD_V3_INVALID_HANDLE;
    int32_t bestZ = std::numeric_limits<int32_t>::min();
    uint32_t bestDepth = 0u;
    for (const auto& pair : g_subscriptions) {
        const Subscription* subscription = pair.second.get();
        if (!subscription || subscription->magic != kSubscriptionMagic ||
            subscription->kind != SubscriptionKind::UiEvent ||
            subscription->owner != owner) {
            continue;
        }
        const auto found = g_uiControls.find(subscription->target);
        if (found == g_uiControls.end() || !found->second) continue;
        WotbModV3Rect rect = {};
        uint32_t depth = 0u;
        if (!UiWorldRectLocked(found->second, &rect, &depth) ||
            pointer.x < rect.x || pointer.y < rect.y ||
            pointer.x > rect.x + rect.width ||
            pointer.y > rect.y + rect.height) {
            continue;
        }
        if (best == WOTBMOD_V3_INVALID_HANDLE ||
            found->second->zOrder > bestZ ||
            (found->second->zOrder == bestZ && depth >= bestDepth)) {
            best = subscription->target;
            bestZ = found->second->zOrder;
            bestDepth = depth;
        }
    }
    return best;
}

struct PendingUiDispatch {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t type = 0u;
    PendingUiInput input = {};
};

void AppendUiDispatch(
    std::vector<PendingUiDispatch>* output,
    WotbModV3Handle owner,
    WotbModV3UiHandle control,
    uint32_t type,
    const PendingUiInput& input) {
    if (!output || owner == WOTBMOD_V3_INVALID_HANDLE ||
        control == WOTBMOD_V3_INVALID_HANDLE || type == 0u) {
        return;
    }
    output->push_back({owner, control, type, input});
}

void DispatchUiInputCallbacks(
    const std::vector<PendingUiDispatch>& dispatches) {
    struct CallbackCopy {
        WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
        WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
        uint32_t type = 0u;
        PendingUiInput input = {};
        bool gameOwned = false;
        std::shared_ptr<SubscriptionCallbackState> state;
    };
    std::vector<CallbackCopy> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (const PendingUiDispatch& dispatch : dispatches) {
            const auto ui = g_uiControls.find(dispatch.control);
            if (ui == g_uiControls.end() || !ui->second) continue;
            for (const auto& pair : g_subscriptions) {
                const Subscription* subscription = pair.second.get();
                if (!subscription ||
                    subscription->magic != kSubscriptionMagic ||
                    subscription->kind != SubscriptionKind::UiEvent ||
                    subscription->owner != dispatch.owner ||
                    subscription->target != dispatch.control ||
                    subscription->eventType != dispatch.type ||
                    !subscription->callbackState) {
                    continue;
                }
                callbacks.push_back(
                    {dispatch.owner,
                     dispatch.control,
                     dispatch.type,
                     dispatch.input,
                     ui->second->gameOwned,
                     subscription->callbackState});
            }
        }
    }
    for (const CallbackCopy& callback : callbacks) {
        if (!IsModEnabled(callback.owner)) continue;
        const WotbModV3Result permission = callback.gameOwned
            ? CheckNamedPermission(
                  callback.owner,
                  "ui.modify.game",
                  WOTBMOD_V3_PERMISSION_REVIEWED)
            : CheckUiOwnPermission(callback.owner);
        if (permission != WOTBMOD_V3_OK) continue;
        SubscriptionCallbackLease lease(callback.state);
        if (!lease) continue;
        const WotbModV3UiEventCallback invoke =
            callback.state->uiCallback;
        void* const userData = callback.state->userData;
        if (!invoke ||
            EnterModCallbackCoalescible(callback.owner) !=
                WOTBMOD_V3_OK) {
            continue;
        }
        WotbModV3UiEvent event = {};
        event.struct_size = sizeof(event);
        event.api_version = WOTBMOD_V3_UI_VERSION;
        event.type = callback.type;
        event.modifiers = callback.input.modifiers;
        event.control = callback.control;
        event.pointer = callback.input.pointer;
        event.delta = callback.input.delta;
        try {
            invoke(callback.owner, &event, userData);
        } catch (...) {
            SetError(
                callback.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "UI event callback threw an exception");
        }
        LeaveModCallback(callback.owner);
    }
}

void PumpManagedUiInput() {
    std::vector<PendingUiInput> inputs;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        inputs.swap(g_pendingUiInputs);
    }
    for (const PendingUiInput& input : inputs) {
        std::vector<PendingUiDispatch> dispatches;
        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            std::vector<WotbModV3Handle> owners;
            owners.reserve(g_subscriptions.size());
            for (const auto& pair : g_subscriptions) {
                const Subscription* subscription = pair.second.get();
                if (!subscription ||
                    subscription->kind != SubscriptionKind::UiEvent) {
                    continue;
                }
                if (std::find(
                        owners.begin(), owners.end(), subscription->owner) ==
                    owners.end()) {
                    owners.push_back(subscription->owner);
                }
            }
            for (WotbModV3Handle owner : owners) {
                const WotbModV3UiHandle hit =
                    HitTestSubscribedUiLocked(owner, input.pointer);
                WotbModV3UiHandle pressed = WOTBMOD_V3_INVALID_HANDLE;
                const auto pressedIt = g_uiPressedControls.find(owner);
                if (pressedIt != g_uiPressedControls.end()) {
                    pressed = pressedIt->second;
                }
                switch (input.phase) {
                    case 1u: /* DAVA UIEvent::Phase::BEGAN */
                        if (hit != WOTBMOD_V3_INVALID_HANDLE) {
                            g_uiPressedControls[owner] = hit;
                            g_uiDragging[owner] = 0u;
                            AppendUiDispatch(
                                &dispatches, owner, hit,
                                WOTBMOD_V3_UI_EVENT_POINTER_DOWN, input);
                        }
                        break;
                    case 2u: /* DRAG */
                        if (pressed != WOTBMOD_V3_INVALID_HANDLE) {
                            if (g_uiDragging[owner] == 0u) {
                                g_uiDragging[owner] = 1u;
                                AppendUiDispatch(
                                    &dispatches, owner, pressed,
                                    WOTBMOD_V3_UI_EVENT_DRAG_START, input);
                            }
                            AppendUiDispatch(
                                &dispatches, owner, pressed,
                                WOTBMOD_V3_UI_EVENT_DRAG, input);
                        }
                        break;
                    case 3u: /* ENDED */
                        if (pressed != WOTBMOD_V3_INVALID_HANDLE) {
                            AppendUiDispatch(
                                &dispatches, owner, pressed,
                                WOTBMOD_V3_UI_EVENT_POINTER_UP, input);
                            if (g_uiDragging[owner] != 0u) {
                                AppendUiDispatch(
                                    &dispatches, owner, pressed,
                                    WOTBMOD_V3_UI_EVENT_DRAG_END, input);
                            } else if (pressed == hit) {
                                AppendUiDispatch(
                                    &dispatches, owner, pressed,
                                    WOTBMOD_V3_UI_EVENT_CLICK, input);
                            }
                        }
                        g_uiPressedControls.erase(owner);
                        g_uiDragging.erase(owner);
                        break;
                    case 4u: { /* MOVE */
                        WotbModV3UiHandle previous =
                            WOTBMOD_V3_INVALID_HANDLE;
                        const auto hover = g_uiHoveredControls.find(owner);
                        if (hover != g_uiHoveredControls.end()) {
                            previous = hover->second;
                        }
                        if (previous != hit) {
                            AppendUiDispatch(
                                &dispatches, owner, previous,
                                WOTBMOD_V3_UI_EVENT_POINTER_LEAVE, input);
                            AppendUiDispatch(
                                &dispatches, owner, hit,
                                WOTBMOD_V3_UI_EVENT_POINTER_ENTER, input);
                            if (hit == WOTBMOD_V3_INVALID_HANDLE) {
                                g_uiHoveredControls.erase(owner);
                            } else {
                                g_uiHoveredControls[owner] = hit;
                            }
                        }
                        break;
                    }
                    case 5u: /* WHEEL */
                        AppendUiDispatch(
                            &dispatches, owner, hit,
                            WOTBMOD_V3_UI_EVENT_SCROLL, input);
                        break;
                    case 6u: /* CANCELLED */
                        if (pressed != WOTBMOD_V3_INVALID_HANDLE) {
                            if (g_uiDragging[owner] != 0u) {
                                AppendUiDispatch(
                                    &dispatches, owner, pressed,
                                    WOTBMOD_V3_UI_EVENT_DRAG_END, input);
                            }
                            AppendUiDispatch(
                                &dispatches, owner, pressed,
                                WOTBMOD_V3_UI_EVENT_CANCEL, input);
                        }
                        g_uiPressedControls.erase(owner);
                        g_uiDragging.erase(owner);
                        break;
                    default:
                        break;
                }
            }
        }
        DispatchUiInputCallbacks(dispatches);
    }
}

void PumpManagedImpactInstances(double deltaSeconds) {
    std::vector<ManagedImpactInstance> expiredImpacts;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (ManagedImpactInstance& instance : g_impactInstances) {
            instance.remainingSeconds -=
                std::max(0.0, deltaSeconds);
        }
        auto firstExpired = std::stable_partition(
            g_impactInstances.begin(),
            g_impactInstances.end(),
            [](const ManagedImpactInstance& instance) {
                return instance.remainingSeconds > 0.0;
            });
        expiredImpacts.assign(
            firstExpired, g_impactInstances.end());
        g_impactInstances.erase(
            firstExpired, g_impactInstances.end());
    }
    for (const ManagedImpactInstance& instance : expiredImpacts) {
        if (instance.entity != WOTBMOD_V3_INVALID_HANDLE) {
            ReleaseOwnedHandle(instance.owner, instance.entity);
        }
        if (instance.activeScene != WOTBMOD_V3_INVALID_HANDLE) {
            ReleaseOwnedHandle(instance.owner, instance.activeScene);
        }
    }
}

void ClientRuntimeFramePump(
    uint64_t frameIndex,
    double deltaSeconds) {
    std::lock_guard<std::mutex> frameLock(
        g_clientFramePumpMutex);
    if (g_lastExternalFrame == frameIndex) {
        g_lastExternalFrame =
            std::numeric_limits<uint64_t>::max();
        return;
    }
    PumpManagedUiInput();
    RefreshUiCaptureSnapshot();
    PumpManagedImpactInstances(deltaSeconds);
    SessionClusterFrameTick(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    ClientRenderFramePump(frameIndex, deltaSeconds, nullptr);
}

enum class AudioObjectKind {
    Clip,
    SoundEvent
};

struct AudioLifecycleState {
    std::mutex mutex;
    std::condition_variable quiesced;
    bool active = true;
    uint32_t inFlight = 0u;
    std::unordered_map<std::thread::id, uint32_t> inFlightByThread;
    std::atomic<uint32_t> state{WOTBMOD_V3_AUDIO_STATE_CREATED};
};

class AudioLifecycleLease final {
public:
    AudioLifecycleLease() = default;

    explicit AudioLifecycleLease(
        const std::shared_ptr<AudioLifecycleState>& state)
        : state_(state) {
        Acquire();
    }

    bool Acquire(
        const std::shared_ptr<AudioLifecycleState>& state) {
        if (state_ || acquired_) {
            return false;
        }
        state_ = state;
        return Acquire();
    }

private:
    bool Acquire() {
        if (!state_) {
            return false;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->active) {
            return false;
        }
        try {
            ++state_->inFlightByThread[std::this_thread::get_id()];
        } catch (...) {
            return false;
        }
        ++state_->inFlight;
        acquired_ = true;
        return true;
    }

public:
    ~AudioLifecycleLease() {
        if (!acquired_) {
            return;
        }
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found =
            state_->inFlightByThread.find(std::this_thread::get_id());
        if (found != state_->inFlightByThread.end()) {
            if (found->second > 1u) {
                --found->second;
            } else {
                state_->inFlightByThread.erase(found);
            }
        }
        if (state_->inFlight > 0u) {
            --state_->inFlight;
        }
        state_->quiesced.notify_all();
    }

    AudioLifecycleLease(const AudioLifecycleLease&) = delete;
    AudioLifecycleLease& operator=(const AudioLifecycleLease&) = delete;

    explicit operator bool() const {
        return acquired_;
    }

private:
    std::shared_ptr<AudioLifecycleState> state_;
    bool acquired_ = false;
};

void DeactivateAndWaitForAudioLifecycle(
    const std::shared_ptr<AudioLifecycleState>& state) {
    if (!state) {
        return;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->active = false;
    const auto current =
        state->inFlightByThread.find(std::this_thread::get_id());
    const uint32_t currentThreadFrames =
        current == state->inFlightByThread.end()
            ? 0u
            : current->second;
    state->quiesced.wait(
        lock,
        [&]() {
            return state->inFlight <= currentThreadFrames;
        });
}

/*
 * The bare grants, with no interception re-entry guard. Only the two slots
 * that must remain callable from inside an interception callback use these:
 * WotbModV3AudioApiV3::is_intercept_active, whose entire job is to report
 * that this thread is inside one, and intercept_unregister, which a mod may
 * legitimately want to reach from anywhere.
 */
WotbModV3Result CheckAudioCustomGrant(WotbModV3Handle mod) {
    return CheckNamedPermission(
        mod,
        "audio.custom",
        WOTBMOD_V3_PERMISSION_SAFE);
}

WotbModV3Result CheckAudioEventsGrant(WotbModV3Handle mod) {
    return CheckNamedPermission(
        mod,
        "audio.events",
        WOTBMOD_V3_PERMISSION_SAFE);
}

/*
 * audio_v3.h: "Non-zero means this thread is currently inside an
 * interception callback; every other audio operation returns
 * WOTBMOD_V3_E_BUSY while it is set."
 *
 * The guard lives here rather than at each of the ~45 audio slots because
 * every one of them starts by checking one of the two grants below -- either
 * directly, or through AudioCreateCore or GetAudioObject. One place to add
 * it is one place that cannot be forgotten when a slot is added. The grant
 * is checked FIRST so a mod that never had the permission still learns that,
 * rather than learning about a re-entry state it is not entitled to observe.
 */
WotbModV3Result CheckAudioInterceptReentry(WotbModV3Handle mod) {
    if (g_soundInterceptDepth == 0u) {
        return WOTBMOD_V3_OK;
    }
    return SetError(
        mod,
        WOTBMOD_V3_E_BUSY,
        "audio operations are refused inside a sound interception callback",
        "{\"reason\":\"sound_intercept_reentry\"}");
}

WotbModV3Result CheckAudioCustomPermission(WotbModV3Handle mod) {
    const WotbModV3Result permission = CheckAudioCustomGrant(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    return CheckAudioInterceptReentry(mod);
}

WotbModV3Result CheckAudioEventsPermission(WotbModV3Handle mod) {
    const WotbModV3Result permission = CheckAudioEventsGrant(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    return CheckAudioInterceptReentry(mod);
}

struct AudioObject {
    uint32_t magic = kAudioMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3AudioHandle self = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
    AudioObjectKind kind = AudioObjectKind::Clip;
    std::string uri;
    std::string eventName;
    std::string bus;
    uint32_t flags = 0;
    uint32_t priority = 0;
    std::shared_ptr<AudioLifecycleState> lifecycleState;
    float volume = 1.0f;
    float pitch = 1.0f;
    float pan = 0.0f;
    WotbModV3Vec3 position = {};
    float minDistance = 1.0f;
    float maxDistance = 100.0f;
    uint32_t loop = 0;
    float speed = 1.0f;
    WotbModV3Vec3 direction = {0.0f, 0.0f, 1.0f};
    WotbModV3Vec3 velocity = {};
    int32_t loopCount = 0;
    std::map<std::string, float> requestedParameters;
};

void DestroyAudioObject(void* object) {
    AudioObject* audio = static_cast<AudioObject*>(object);
    if (audio) {
        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            const auto found =
                g_hostAudioObjects.find(audio->nativeObject);
            if (found != g_hostAudioObjects.end() &&
                found->second.handle == audio->self) {
                g_hostAudioObjects.erase(found);
            }
        }
        DeactivateAndWaitForAudioLifecycle(
            audio->lifecycleState);
        if (audio->nativeObject != 0u) {
            ReleaseHostObject(
                audio->owner,
                audio->kind == AudioObjectKind::Clip
                    ? "audio_destroy"
                    : "sound_event_destroy",
                audio->nativeObject);
            audio->nativeObject = 0u;
        }
        audio->magic = 0;
        delete audio;
    }
}

WotbModV3Result GetAudioObject(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    AudioObjectKind kind,
    AudioObject** outAudio) {
    const WotbModV3Result permission =
        kind == AudioObjectKind::Clip
            ? CheckAudioCustomPermission(mod)
            : CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outAudio) {
        return Invalid(mod, "audio output pointer is null");
    }
    void* object = nullptr;
    const uint32_t handleType =
        kind == AudioObjectKind::Clip
            ? WOTBMOD_V3_HANDLE_AUDIO
            : WOTBMOD_V3_HANDLE_SOUND_EVENT;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        handleType,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    AudioObject* audio = static_cast<AudioObject*>(object);
    if (!audio || audio->magic != kAudioMagic || audio->kind != kind) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "audio handle has a different type");
    }
    *outAudio = audio;
    return WOTBMOD_V3_OK;
}

WotbModV3Result AudioCreateCore(
    WotbModV3Handle mod,
    const WotbModV3AudioDescriptor* descriptor,
    bool stream,
    WotbModV3AudioHandle* outAudio) {
    const WotbModV3Result permission =
        CheckAudioCustomPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    const uint32_t knownFlags =
        WOTBMOD_V3_AUDIO_CREATE_SPATIAL |
        WOTBMOD_V3_AUDIO_CREATE_LOOP |
        WOTBMOD_V3_AUDIO_CREATE_STREAM;
    if (!ValidStruct(descriptor, WOTBMOD_V3_AUDIO_VERSION) || !outAudio ||
        !ValidText(descriptor->uri, WOTBMOD_V3_MAX_PATH, false) ||
        (descriptor->flags & ~knownFlags) != 0u ||
        !IsFinite(descriptor->volume) || descriptor->volume < 0.0f ||
        descriptor->volume > 4.0f || !IsFinite(descriptor->pitch) ||
        descriptor->pitch < 0.01f || descriptor->pitch > 4.0f ||
        (descriptor->bus &&
         !ValidText(descriptor->bus, WOTBMOD_V3_MAX_NAME))) {
        return Invalid(mod, "invalid audio descriptor");
    }
    std::string resolvedFilePath;
    WotbModV3Result pathResult =
        ResolveVirtualFilePath(mod, descriptor->uri, &resolvedFilePath);
    if (pathResult != WOTBMOD_V3_OK) {
        return pathResult;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.flags = descriptor->flags |
                    (stream ? WOTBMOD_V3_AUDIO_CREATE_STREAM : 0u);
    request.selector = descriptor->priority;
    request.scalar0 = descriptor->volume;
    request.scalar1 = descriptor->pitch;
    request.name = descriptor->uri;
    request.secondary_name =
        resolvedFilePath.empty() ? nullptr : resolvedFilePath.c_str();
    request.payload = descriptor;
    request.payload_size = sizeof(*descriptor);
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        stream ? "audio_create_stream" : "audio_create",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "audio backend returned an invalid object");
    }
    AudioObject* audio = new (std::nothrow) AudioObject();
    if (!audio) {
        ReleaseHostObject(
            mod,
            "audio_destroy",
            response.object);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "audio object allocation failed");
    }
    try {
        audio->lifecycleState =
            std::make_shared<AudioLifecycleState>();
    } catch (...) {
        ReleaseHostObject(
            mod,
            "audio_destroy",
            response.object);
        delete audio;
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "audio lifecycle state allocation failed");
    }
    audio->owner = mod;
    audio->nativeObject = response.object;
    audio->uri = descriptor->uri;
    audio->bus = descriptor->bus ? descriptor->bus : "";
    audio->flags = descriptor->flags |
                   (stream ? WOTBMOD_V3_AUDIO_CREATE_STREAM : 0u);
    audio->priority = descriptor->priority;
    audio->volume = descriptor->volume;
    audio->pitch = descriptor->pitch;
    audio->loop =
        (audio->flags & WOTBMOD_V3_AUDIO_CREATE_LOOP) != 0u ? 1u : 0u;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_AUDIO,
        audio,
        &DestroyAudioObject,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "audio_destroy",
            audio->nativeObject);
        audio->nativeObject = 0u;
        delete audio;
        return result;
    }
    audio->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_hostAudioObjects[audio->nativeObject] = {
            mod,
            handle,
            audio->lifecycleState};
    }
    *outAudio = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioCreate(
    WotbModV3Handle mod,
    const WotbModV3AudioDescriptor* descriptor,
    WotbModV3AudioHandle* outAudio) {
    return AudioCreateCore(mod, descriptor, false, outAudio);
}

WotbModV3Result WOTBMOD_V3_CALL AudioCreateStream(
    WotbModV3Handle mod,
    const WotbModV3AudioDescriptor* descriptor,
    WotbModV3AudioHandle* outAudio) {
    return AudioCreateCore(mod, descriptor, true, outAudio);
}

WotbModV3Result AudioHostOperation(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    const char* operation,
    ClientHostObjectRequest* request,
    ClientHostObjectResponse* response,
    AudioObject** outAudio = nullptr) {
    AudioObject* audio = nullptr;
    const WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest localRequest = MakeHostRequest();
    ClientHostObjectRequest* effectiveRequest =
        request ? request : &localRequest;
    effectiveRequest->object = audio->nativeObject;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        effectiveRequest,
        sizeof(*effectiveRequest),
        response,
        response ? sizeof(*response) : 0u);
    if (hostResult == WOTBMOD_V3_OK && outAudio) {
        *outAudio = audio;
    }
    return hostResult;
}

WotbModV3Result AudioNativeOperation(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    const char* operation,
    uint32_t successState) {
    AudioObject* audio = nullptr;
    const WotbModV3Result result = AudioHostOperation(
        mod,
        handle,
        operation,
        nullptr,
        nullptr,
        &audio);
    if (result == WOTBMOD_V3_OK) {
        audio->lifecycleState->state.store(
            successState,
            std::memory_order_release);
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL AudioPreload(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio) {
    return AudioNativeOperation(
        mod,
        audio,
        "audio_preload",
        WOTBMOD_V3_AUDIO_STATE_PRELOADED);
}

WotbModV3Result WOTBMOD_V3_CALL AudioPlay(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio) {
    return AudioNativeOperation(
        mod,
        audio,
        "audio_play",
        WOTBMOD_V3_AUDIO_STATE_PLAYING);
}

WotbModV3Result WOTBMOD_V3_CALL AudioPause(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio) {
    return AudioNativeOperation(
        mod,
        audio,
        "audio_pause",
        WOTBMOD_V3_AUDIO_STATE_PAUSED);
}

WotbModV3Result WOTBMOD_V3_CALL AudioResume(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio) {
    return AudioNativeOperation(
        mod,
        audio,
        "audio_resume",
        WOTBMOD_V3_AUDIO_STATE_PLAYING);
}

WotbModV3Result WOTBMOD_V3_CALL AudioStop(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    float fadeSeconds) {
    if (!IsFinite(fadeSeconds) || fadeSeconds < 0.0f ||
        fadeSeconds > 60.0f) {
        return Invalid(mod, "audio stop fade is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.scalar0 = fadeSeconds;
    AudioObject* object = nullptr;
    const WotbModV3Result result = AudioHostOperation(
        mod,
        audio,
        "audio_stop",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->lifecycleState->state.store(
            WOTBMOD_V3_AUDIO_STATE_STOPPED,
            std::memory_order_release);
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL AudioDestroy(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle) {
    AudioObject* audio = nullptr;
    const WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = audio->nativeObject;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_destroy",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_hostAudioObjects.erase(audio->nativeObject);
    }
    audio->nativeObject = 0u;
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result AudioQueryState(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    uint32_t* outState) {
    if (!outState) {
        return Invalid(mod, "audio state output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    AudioObject* audio = nullptr;
    const WotbModV3Result result = AudioHostOperation(
        mod,
        handle,
        "audio_get_state",
        nullptr,
        &response,
        &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.value_u32 > WOTBMOD_V3_AUDIO_STATE_ERROR) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "audio backend returned an invalid playback state");
    }
    audio->lifecycleState->state.store(
        response.value_u32,
        std::memory_order_release);
    *outState = response.value_u32;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioIsPlaying(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    uint32_t* outPlaying) {
    if (!outPlaying) {
        return Invalid(mod, "playing output pointer is null");
    }
    uint32_t state = WOTBMOD_V3_AUDIO_STATE_CREATED;
    const WotbModV3Result result =
        AudioQueryState(mod, handle, &state);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    *outPlaying = state == WOTBMOD_V3_AUDIO_STATE_PLAYING ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioGetState(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    uint32_t* outState) {
    return AudioQueryState(mod, handle, outState);
}

WotbModV3Result AudioSetFloat(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    float value,
    float minimum,
    float maximum,
    float AudioObject::*member,
    const char* message,
    const char* operation) {
    if (!IsFinite(value) || value < minimum || value > maximum) {
        return Invalid(mod, message);
    }
    AudioObject* audio = nullptr;
    WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = audio->nativeObject;
    request.scalar0 = value;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    audio->*member = value;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetVolume(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    float volume) {
    return AudioSetFloat(
        mod,
        audio,
        volume,
        0.0f,
        4.0f,
        &AudioObject::volume,
        "audio volume is out of range",
        "audio_set_volume");
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetPitch(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    float pitch) {
    return AudioSetFloat(
        mod,
        audio,
        pitch,
        0.01f,
        4.0f,
        &AudioObject::pitch,
        "audio pitch is out of range",
        "audio_set_pitch");
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetPan(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    float pan) {
    return AudioSetFloat(
        mod,
        audio,
        pan,
        -1.0f,
        1.0f,
        &AudioObject::pan,
        "audio pan is out of range",
        "audio_set_pan");
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetBus(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    const char* bus) {
    const WotbModV3Result eventsPermission =
        CheckAudioEventsPermission(mod);
    if (eventsPermission != WOTBMOD_V3_OK) {
        return eventsPermission;
    }
    if (!ValidText(bus, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "audio bus name is invalid");
    }
    AudioObject* audio = nullptr;
    const WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = audio->nativeObject;
    request.name = bus;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_set_bus",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    audio->bus = bus;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetPosition3d(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    WotbModV3Vec3 position) {
    if (!IsFinite(position)) {
        return Invalid(mod, "audio 3D position is invalid");
    }
    AudioObject* audio = nullptr;
    const WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = audio->nativeObject;
    request.vector = {position.x, position.y, position.z, 0.0f};
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_set_position_3d",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    audio->position = position;
    audio->flags |= WOTBMOD_V3_AUDIO_CREATE_SPATIAL;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetMinDistance(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    float distance) {
    return AudioSetFloat(
        mod,
        audio,
        distance,
        0.0f,
        100000.0f,
        &AudioObject::minDistance,
        "audio minimum distance is out of range",
        "audio_set_min_distance");
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetMaxDistance(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    float distance) {
    AudioObject* audio = nullptr;
    WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!IsFinite(distance) || distance < audio->minDistance ||
        distance > 1000000.0f) {
        return Invalid(
            mod,
            "audio maximum distance is below minimum or out of range");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = audio->nativeObject;
    request.scalar0 = distance;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_set_max_distance",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    audio->maxDistance = distance;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioFadeTo(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    float targetVolume,
    float durationSeconds) {
    if (!IsFinite(targetVolume) || targetVolume < 0.0f ||
        targetVolume > 4.0f || !IsFinite(durationSeconds) ||
        durationSeconds < 0.0f || durationSeconds > 3600.0f) {
        return Invalid(mod, "audio fade arguments are invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.scalar0 = targetVolume;
    request.scalar1 = durationSeconds;
    return AudioHostOperation(
        mod,
        handle,
        "audio_fade_to",
        &request,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL AudioSeek(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    double seconds) {
    if (!IsFinite(seconds) || seconds < 0.0) {
        return Invalid(mod, "audio seek position is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.scalar0 = seconds;
    return AudioHostOperation(
        mod,
        handle,
        "audio_seek",
        &request,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL AudioGetPosition(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    double* outSeconds) {
    if (!outSeconds) {
        return Invalid(mod, "audio position output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = AudioHostOperation(
        mod,
        handle,
        "audio_get_position",
        nullptr,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (!IsFinite(response.value_f64) ||
            response.value_f64 < 0.0) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "audio backend returned an invalid playback position");
        }
        *outSeconds = response.value_f64;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL AudioGetDuration(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    double* outSeconds) {
    if (!outSeconds) {
        return Invalid(mod, "audio duration output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = AudioHostOperation(
        mod,
        handle,
        "audio_get_duration",
        nullptr,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (!IsFinite(response.value_f64) ||
            response.value_f64 < 0.0) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "audio backend returned an invalid clip duration");
        }
        *outSeconds = response.value_f64;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetLoop(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    uint32_t loop) {
    if (loop > 1u) {
        return Invalid(mod, "audio loop must be 0 or 1");
    }
    AudioObject* audio = nullptr;
    const WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = audio->nativeObject;
    request.flags = loop;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_set_loop",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    audio->loop = loop;
    if (loop) {
        audio->flags |= WOTBMOD_V3_AUDIO_CREATE_LOOP;
    } else {
        audio->flags &= ~WOTBMOD_V3_AUDIO_CREATE_LOOP;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result AudioSubscribeLifecycle(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    SubscriptionKind kind,
    WotbModV3AudioLifecycleCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    AudioObject* audio = nullptr;
    WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!callback || !outToken) {
        return Invalid(mod, "audio lifecycle callback is null");
    }
    uint64_t publicationEpoch = 0u;
    result = BeginSubscriptionCreation(mod, &publicationEpoch);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostAudioSubscriptionRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.object = audio->nativeObject;
    request.mod = mod;
    request.lifecycle_kind =
        kind == SubscriptionKind::AudioStarted
            ? CLIENT_HOST_AUDIO_STARTED
            : CLIENT_HOST_AUDIO_FINISHED;
    ClientHostObjectResponse response = MakeHostResponse();
    const char* operation =
        kind == SubscriptionKind::AudioStarted
            ? "audio_subscribe_started"
            : "audio_subscribe_finished";
    uint64_t backendGeneration = 0u;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &backendGeneration);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "audio backend returned an invalid subscription");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        ReleaseHostObject(
            mod,
            "audio_unsubscribe",
            response.object,
            backendGeneration);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "audio subscription allocation failed");
    }
    subscription->kind = kind;
    subscription->target = audio->self;
    subscription->nativeObject = response.object;
    subscription->backendGeneration = backendGeneration;
    subscription->publicationEpoch = publicationEpoch;
    subscription->audioLifecycleCallback = callback;
    subscription->userData = userData;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL AudioOnStarted(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    WotbModV3AudioLifecycleCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    return AudioSubscribeLifecycle(
        mod,
        audio,
        SubscriptionKind::AudioStarted,
        callback,
        userData,
        outToken);
}

WotbModV3Result WOTBMOD_V3_CALL AudioOnFinished(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    WotbModV3AudioLifecycleCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    return AudioSubscribeLifecycle(
        mod,
        audio,
        SubscriptionKind::AudioFinished,
        callback,
        userData,
        outToken);
}

WotbModV3Result WOTBMOD_V3_CALL AudioOnError(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    WotbModV3AudioErrorCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    AudioObject* audio = nullptr;
    WotbModV3Result result =
        GetAudioObject(mod, handle, AudioObjectKind::Clip, &audio);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!callback || !outToken) {
        return Invalid(mod, "audio error callback is null");
    }
    uint64_t publicationEpoch = 0u;
    result = BeginSubscriptionCreation(mod, &publicationEpoch);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostAudioSubscriptionRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.object = audio->nativeObject;
    request.mod = mod;
    request.lifecycle_kind = CLIENT_HOST_AUDIO_ERROR;
    ClientHostObjectResponse response = MakeHostResponse();
    uint64_t backendGeneration = 0u;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_subscribe_error",
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &backendGeneration);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "audio backend returned an invalid subscription");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        ReleaseHostObject(
            mod,
            "audio_unsubscribe",
            response.object,
            backendGeneration);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "audio error subscription allocation failed");
    }
    subscription->kind = SubscriptionKind::AudioError;
    subscription->target = audio->self;
    subscription->nativeObject = response.object;
    subscription->backendGeneration = backendGeneration;
    subscription->publicationEpoch = publicationEpoch;
    subscription->audioErrorCallback = callback;
    subscription->userData = userData;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL AudioUnsubscribe(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    const WotbModV3Result permission =
        CheckAudioCustomPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto found = g_subscriptions.find(token);
        if (found != g_subscriptions.end()) {
            subscription = found->second;
        }
    }
    if (!subscription || subscription->magic != kSubscriptionMagic ||
        subscription->owner != mod ||
        (subscription->kind != SubscriptionKind::AudioStarted &&
         subscription->kind != SubscriptionKind::AudioFinished &&
         subscription->kind != SubscriptionKind::AudioError)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "token is not an audio subscription");
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_unsubscribe");
}

WotbModV3Result WOTBMOD_V3_CALL AudioOverrideRegister(
    WotbModV3Handle mod,
    const char* eventName,
    const char* replacementUri,
    int32_t priority,
    WotbModV3Token* outToken) {
    const WotbModV3Result permission =
        CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(eventName, WOTBMOD_V3_MAX_NAME, false) ||
        !ValidText(replacementUri, WOTBMOD_V3_MAX_PATH, false) ||
        !outToken) {
        return Invalid(mod, "invalid sound override registration");
    }
    uint64_t publicationEpoch = 0u;
    WotbModV3Result result =
        BeginSubscriptionCreation(mod, &publicationEpoch);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::string resolvedFilePath;
    result = ResolveVirtualFilePath(
        mod,
        replacementUri,
        &resolvedFilePath);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = eventName;
    request.secondary_name = resolvedFilePath.empty()
        ? replacementUri
        : resolvedFilePath.c_str();
    request.signed_value = priority;
    ClientHostObjectResponse response = MakeHostResponse();
    uint64_t backendGeneration = 0u;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "sound_override_register",
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &backendGeneration);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "audio backend returned an invalid override token");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        ReleaseHostObject(
            mod,
            "sound_override_unregister",
            response.object,
            backendGeneration);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "sound override allocation failed");
    }
    subscription->kind = SubscriptionKind::AudioOverride;
    subscription->nativeObject = response.object;
    subscription->backendGeneration = backendGeneration;
    subscription->publicationEpoch = publicationEpoch;
    subscription->priority = priority;
    subscription->property = eventName;
    subscription->methodFilter = replacementUri;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL AudioOverrideUnregister(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    const WotbModV3Result permission =
        CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::AudioOverride,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "sound_override_unregister");
}

WotbModV3Result WOTBMOD_V3_CALL AudioOverrideSetPriority(
    WotbModV3Handle mod,
    WotbModV3Token token,
    int32_t priority) {
    const WotbModV3Result permission =
        CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::AudioOverride,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::unique_lock<std::recursive_mutex> operationLock(
        subscription->operationMutex);
    if (subscription->destroying ||
        subscription->unregistered ||
        subscription->unregistering) {
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "sound override is no longer active");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = subscription->nativeObject;
    request.signed_value = priority;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "sound_override_set_priority",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result == WOTBMOD_V3_OK) {
        subscription->priority = priority;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL AudioOverrideGetConflicts(
    WotbModV3Handle mod,
    const char* eventName,
    WotbModV3SoundConflictVisitor visitor,
    void* userData) {
    const WotbModV3Result permission =
        CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(eventName, WOTBMOD_V3_MAX_NAME, false) || !visitor) {
        return Invalid(mod, "invalid sound conflict query");
    }
    ClientHostAudioConflictVisitorRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.mod = mod;
    request.event_name = eventName;
    request.visitor = visitor;
    request.user_data = userData;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "sound_override_get_conflicts",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventCreate(
    WotbModV3Handle mod,
    const char* eventName,
    WotbModV3AudioHandle* outEvent) {
    const WotbModV3Result permission =
        CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(eventName, WOTBMOD_V3_MAX_NAME, false) || !outEvent) {
        return Invalid(mod, "invalid sound event name");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = eventName;
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "sound_event_create",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "audio backend returned an invalid sound event");
    }
    AudioObject* event = new (std::nothrow) AudioObject();
    if (!event) {
        ReleaseHostObject(
            mod,
            "sound_event_destroy",
            response.object);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "sound event allocation failed");
    }
    event->owner = mod;
    event->kind = AudioObjectKind::SoundEvent;
    event->nativeObject = response.object;
    event->eventName = eventName;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SOUND_EVENT,
        event,
        &DestroyAudioObject,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "sound_event_destroy",
            event->nativeObject);
        event->nativeObject = 0u;
        delete event;
        return result;
    }
    event->self = handle;
    *outEvent = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result SoundEventNative(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    const char* operation,
    ClientHostObjectRequest* request = nullptr,
    ClientHostObjectResponse* response = nullptr,
    AudioObject** outEvent = nullptr) {
    AudioObject* event = nullptr;
    const WotbModV3Result result = GetAudioObject(
        mod,
        handle,
        AudioObjectKind::SoundEvent,
        &event);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest localRequest = MakeHostRequest();
    ClientHostObjectRequest* effectiveRequest =
        request ? request : &localRequest;
    effectiveRequest->object = event->nativeObject;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        operation,
        effectiveRequest,
        sizeof(*effectiveRequest),
        response,
        response ? sizeof(*response) : 0u);
    if (hostResult == WOTBMOD_V3_OK && outEvent) {
        *outEvent = event;
    }
    return hostResult;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventTrigger(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event) {
    return SoundEventNative(mod, event, "sound_event_trigger");
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventStop(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    uint32_t force) {
    if (force > 1u) {
        return Invalid(mod, "sound event force flag must be 0 or 1");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.flags = force;
    return SoundEventNative(
        mod,
        event,
        "sound_event_stop",
        &request);
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetPaused(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    uint32_t paused) {
    if (paused > 1u) {
        return Invalid(mod, "sound event paused flag must be 0 or 1");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.flags = paused;
    return SoundEventNative(
        mod,
        event,
        "sound_event_set_paused",
        &request);
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetVolume(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    float volume) {
    if (!IsFinite(volume) || volume < 0.0f || volume > 4.0f) {
        return Invalid(mod, "sound event volume is out of range");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.scalar0 = volume;
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_volume",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->volume = volume;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetPosition(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    WotbModV3Vec3 position) {
    if (!IsFinite(position)) {
        return Invalid(mod, "sound event position is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.vector = {position.x, position.y, position.z, 0.0f};
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_position",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->position = position;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetParameter(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    const char* name,
    float value) {
    if (!ValidText(name, WOTBMOD_V3_MAX_NAME, false) ||
        !IsFinite(value)) {
        return Invalid(mod, "sound event parameter is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = name;
    request.scalar0 = value;
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_parameter",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->requestedParameters[name] = value;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventGetParameter(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    const char* name,
    float* outValue) {
    if (!ValidText(name, WOTBMOD_V3_MAX_NAME, false) || !outValue) {
        return Invalid(mod, "sound event parameter query is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = name;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_get_parameter",
        &request,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (!IsFinite(response.value_f64)) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "audio backend returned an invalid event parameter");
        }
        *outValue = static_cast<float>(response.value_f64);
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventHasParameter(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    const char* name,
    uint32_t* outHas) {
    if (!ValidText(name, WOTBMOD_V3_MAX_NAME, false) || !outHas) {
        return Invalid(mod, "sound event parameter query is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = name;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_has_parameter",
        &request,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (response.value_u32 > 1u) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "audio backend returned an invalid parameter flag");
        }
        *outHas = response.value_u32;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventGetName(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle,
    char* buffer,
    uint32_t* inoutSize) {
    AudioObject* event = nullptr;
    const WotbModV3Result result = GetAudioObject(
        mod,
        handle,
        AudioObjectKind::SoundEvent,
        &event);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return CopyString(mod, event->eventName, buffer, inoutSize);
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventGetBus(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    char* buffer,
    uint32_t* inoutSize) {
    if (!inoutSize) {
        return Invalid(mod, "sound event bus size pointer is null");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = buffer;
    request.payload_size = buffer ? *inoutSize : 0u;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_get_bus",
        &request,
        &response);
    if (response.value_u32 != 0u) {
        *inoutSize = response.value_u32;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetSpeed(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    float speed) {
    if (!IsFinite(speed) || speed < 0.0f || speed > 16.0f) {
        return Invalid(mod, "sound event speed is out of range");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.scalar0 = speed;
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_speed",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->speed = speed;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetDirection(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    WotbModV3Vec3 direction) {
    if (!IsFinite(direction)) {
        return Invalid(mod, "sound event direction is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.vector = {direction.x, direction.y, direction.z, 0.0f};
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_direction",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->direction = direction;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetVelocity(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    WotbModV3Vec3 velocity) {
    if (!IsFinite(velocity)) {
        return Invalid(mod, "sound event velocity is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.vector = {velocity.x, velocity.y, velocity.z, 0.0f};
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_velocity",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->velocity = velocity;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetLoopCount(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    int32_t loopCount) {
    if (loopCount < -1 || loopCount > 1000000) {
        return Invalid(mod, "sound event loop count is out of range");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.signed_value = loopCount;
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_loop_count",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->loopCount = loopCount;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventSetPriority(
    WotbModV3Handle mod,
    WotbModV3AudioHandle event,
    int32_t priority) {
    ClientHostObjectRequest request = MakeHostRequest();
    request.signed_value = priority;
    AudioObject* object = nullptr;
    const WotbModV3Result result = SoundEventNative(
        mod,
        event,
        "sound_event_set_priority",
        &request,
        nullptr,
        &object);
    if (result == WOTBMOD_V3_OK) {
        object->priority = static_cast<uint32_t>(priority);
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL SoundEventDestroy(
    WotbModV3Handle mod,
    WotbModV3AudioHandle handle) {
    AudioObject* event = nullptr;
    const WotbModV3Result result = GetAudioObject(
        mod,
        handle,
        AudioObjectKind::SoundEvent,
        &event);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = event->nativeObject;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "sound_event_destroy",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    event->nativeObject = 0u;
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetListenerTransform(
    WotbModV3Handle mod,
    const WotbModV3Transform* transform) {
    const WotbModV3Result permission =
        CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidSceneTransform(transform)) {
        return Invalid(mod, "audio listener transform is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.transform = *transform;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_set_listener_transform",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL AudioSetBusVolume(
    WotbModV3Handle mod,
    const char* bus,
    float volume) {
    const WotbModV3Result permission =
        CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(bus, WOTBMOD_V3_MAX_NAME, false) ||
        !IsFinite(volume) || volume < 0.0f || volume > 4.0f) {
        return Invalid(mod, "audio bus volume arguments are invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = bus;
    request.scalar0 = volume;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "audio_set_bus_volume",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

constexpr uint64_t kCameraReadContexts =
    WOTBMOD_V3_CONTEXT_HANGAR |
    WOTBMOD_V3_CONTEXT_BATTLE |
    WOTBMOD_V3_CONTEXT_REPLAY |
    WOTBMOD_V3_CONTEXT_TRAINING;
constexpr uint64_t kCameraWriteContexts =
    WOTBMOD_V3_CONTEXT_HANGAR |
    WOTBMOD_V3_CONTEXT_REPLAY |
    WOTBMOD_V3_CONTEXT_TRAINING;

WotbModV3Result CheckCameraReadPermission(WotbModV3Handle mod) {
    switch (CurrentContext()) {
        case WOTBMOD_V3_CONTEXT_HANGAR:
            return CheckNamedPermission(
                mod,
                "camera.hangar",
                WOTBMOD_V3_PERMISSION_SAFE);
        case WOTBMOD_V3_CONTEXT_REPLAY:
            return CheckNamedPermission(
                mod,
                "camera.replay",
                WOTBMOD_V3_PERMISSION_SAFE);
        case WOTBMOD_V3_CONTEXT_BATTLE:
        case WOTBMOD_V3_CONTEXT_TRAINING:
            return CheckNamedPermission(
                mod,
                "camera.battle.read",
                WOTBMOD_V3_PERMISSION_REVIEWED);
        default:
            return SetError(
                mod,
                WOTBMOD_V3_E_INCOMPATIBLE,
                "camera reads are unavailable in the current context");
    }
}

WotbModV3Result CheckCameraWritePermission(WotbModV3Handle mod) {
    switch (CurrentContext()) {
        case WOTBMOD_V3_CONTEXT_HANGAR:
            return CheckNamedPermission(
                mod,
                "camera.hangar",
                WOTBMOD_V3_PERMISSION_SAFE);
        case WOTBMOD_V3_CONTEXT_REPLAY:
            return CheckNamedPermission(
                mod,
                "camera.replay",
                WOTBMOD_V3_PERMISSION_SAFE);
        case WOTBMOD_V3_CONTEXT_TRAINING:
            return CheckNamedPermission(
                mod,
                "gameplay.tweak.camera",
                WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK);
        case WOTBMOD_V3_CONTEXT_BATTLE:
            return SetError(
                mod,
                WOTBMOD_V3_E_PERMISSION_DENIED,
                "camera writes are denied in live battle");
        default:
            return SetError(
                mod,
                WOTBMOD_V3_E_INCOMPATIBLE,
                "camera writes are unavailable in the current context");
    }
}

uint32_t CameraPermissionTier(bool write) {
    if (write && CurrentContext() == WOTBMOD_V3_CONTEXT_TRAINING) {
        return WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK;
    }
    if (!write &&
        (CurrentContext() == WOTBMOD_V3_CONTEXT_BATTLE ||
         CurrentContext() == WOTBMOD_V3_CONTEXT_TRAINING)) {
        return WOTBMOD_V3_PERMISSION_REVIEWED;
    }
    return WOTBMOD_V3_PERMISSION_SAFE;
}

struct CameraObject {
    uint32_t magic = kCameraMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
};

void DestroyCameraObject(void* object) {
    CameraObject* camera = static_cast<CameraObject*>(object);
    if (camera) {
        camera->nativeObject = 0u;
        camera->magic = 0u;
        delete camera;
    }
}

WotbModV3Result GetCameraObject(
    WotbModV3Handle mod,
    WotbModV3CameraHandle handle,
    CameraObject** outCamera) {
    if (!outCamera) {
        return Invalid(mod, "camera output pointer is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_CAMERA,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    CameraObject* camera = static_cast<CameraObject*>(object);
    if (!camera || camera->magic != kCameraMagic ||
        camera->nativeObject == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not a live client camera");
    }
    *outCamera = camera;
    return WOTBMOD_V3_OK;
}

WotbModV3Result InvokeCamera(
    WotbModV3Handle mod,
    WotbModV3CameraHandle handle,
    bool write,
    const char* operation,
    ClientHostObjectRequest* request,
    ClientHostObjectResponse* response) {
    const WotbModV3Result permission = write
        ? CheckCameraWritePermission(mod)
        : CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    CameraObject* camera = nullptr;
    const WotbModV3Result result =
        GetCameraObject(mod, handle, &camera);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest localRequest = MakeHostRequest();
    ClientHostObjectRequest* effectiveRequest =
        request ? request : &localRequest;
    effectiveRequest->object = camera->nativeObject;
    return InvokeHost(
        mod,
        CameraPermissionTier(write),
        write ? kCameraWriteContexts : kCameraReadContexts,
        operation,
        effectiveRequest,
        sizeof(*effectiveRequest),
        response,
        response ? sizeof(*response) : 0u);
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetActive(
    WotbModV3Handle mod,
    WotbModV3CameraHandle* outCamera) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outCamera) {
        return Invalid(mod, "active camera output pointer is null");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result result = InvokeHost(
        mod,
        CameraPermissionTier(false),
        kCameraReadContexts,
        "camera_get_active",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_FOUND,
            "the client has no active camera");
    }
    CameraObject* camera = new (std::nothrow) CameraObject();
    if (!camera) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "camera wrapper allocation failed");
    }
    camera->owner = mod;
    camera->nativeObject = response.object;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_CAMERA,
        camera,
        &DestroyCameraObject,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        delete camera;
        return result;
    }
    *outCamera = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetMode(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    uint32_t* outMode) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outMode) {
        return Invalid(mod, "camera mode output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        false,
        "camera_get_mode",
        nullptr,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (response.value_u32 >
            WOTBMOD_V3_CAMERA_MODE_CINEMATIC) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "camera backend returned an invalid mode");
        }
        uint32_t mode = response.value_u32;
        /* Hangar/replay are runtime contexts, not values exposed by the
         * verified arcade/sniper controller callback. They can still be
         * reported without guessing an unverified native enum. */
        if (mode == WOTBMOD_V3_CAMERA_MODE_UNKNOWN) {
            const uint64_t context = CurrentContext();
            if ((context & WOTBMOD_V3_CONTEXT_HANGAR) != 0u) {
                mode = WOTBMOD_V3_CAMERA_MODE_HANGAR;
            } else if ((context & WOTBMOD_V3_CONTEXT_REPLAY) != 0u) {
                mode = WOTBMOD_V3_CAMERA_MODE_REPLAY;
            }
        }
        *outMode = mode;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetTransform(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    WotbModV3Transform* outTransform) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outTransform) {
        return Invalid(mod, "camera transform output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        false,
        "camera_get_transform",
        nullptr,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (!ValidSceneTransform(&response.transform)) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "camera backend returned an invalid transform");
        }
        *outTransform = response.transform;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL CameraSetTransform(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    const WotbModV3Transform* transform) {
    const WotbModV3Result permission =
        CheckCameraWritePermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidSceneTransform(transform)) {
        return Invalid(mod, "camera transform is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.transform = *transform;
    return InvokeCamera(
        mod,
        camera,
        true,
        "camera_set_transform",
        &request,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetFov(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    float* outDegrees) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outDegrees) {
        return Invalid(mod, "camera FOV output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        false,
        "camera_get_fov",
        nullptr,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (!IsFinite(response.value_f64) ||
            response.value_f64 < 1.0 ||
            response.value_f64 > 179.0) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "camera backend returned an invalid FOV");
        }
        *outDegrees = static_cast<float>(response.value_f64);
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL CameraSetFov(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    float degrees) {
    const WotbModV3Result permission =
        CheckCameraWritePermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!IsFinite(degrees) || degrees < 10.0f || degrees > 170.0f) {
        return Invalid(mod, "camera FOV is out of range");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.scalar0 = degrees;
    return InvokeCamera(
        mod,
        camera,
        true,
        "camera_set_fov",
        &request,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetNearPlane(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    float* outNearPlane) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outNearPlane) {
        return Invalid(mod, "camera near plane output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        false,
        "camera_get_near_plane",
        nullptr,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (!IsFinite(response.value_f64) ||
            response.value_f64 < 0.0) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "camera backend returned an invalid near plane");
        }
        *outNearPlane = static_cast<float>(response.value_f64);
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL CameraGetFarPlane(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    float* outFarPlane) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outFarPlane) {
        return Invalid(mod, "camera far plane output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        false,
        "camera_get_far_plane",
        nullptr,
        &response);
    if (result == WOTBMOD_V3_OK) {
        if (!IsFinite(response.value_f64) ||
            response.value_f64 <= 0.0) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "camera backend returned an invalid far plane");
        }
        *outFarPlane = static_cast<float>(response.value_f64);
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL CameraWorldToScreen(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    const WotbModV3Vec3* world,
    WotbModV3Vec3* outScreen) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!world || !outScreen || !IsFinite(*world)) {
        return Invalid(mod, "camera world-to-screen arguments are invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.vector = {world->x, world->y, world->z, 0.0f};
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        false,
        "camera_world_to_screen",
        &request,
        &response);
    if (result == WOTBMOD_V3_OK) {
        const WotbModV3Vec3 projected = {
            response.vector.x,
            response.vector.y,
            response.vector.z};
        if (!IsFinite(projected)) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "camera backend returned invalid screen coordinates");
        }
        *outScreen = projected;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL CameraScreenToWorld(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    const WotbModV3Vec3* screen,
    WotbModV3Vec3* outWorld) {
    const WotbModV3Result permission =
        CheckCameraReadPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!screen || !outWorld || !IsFinite(*screen)) {
        return Invalid(mod, "camera screen-to-world arguments are invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.vector = {screen->x, screen->y, screen->z, 0.0f};
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        false,
        "camera_screen_to_world",
        &request,
        &response);
    if (result == WOTBMOD_V3_OK) {
        const WotbModV3Vec3 world = {
            response.vector.x,
            response.vector.y,
            response.vector.z};
        if (!IsFinite(world)) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "camera backend returned invalid world coordinates");
        }
        *outWorld = world;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL CameraAddModifier(
    WotbModV3Handle mod,
    uint32_t phase,
    int32_t priority,
    WotbModV3CameraModifierCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    const WotbModV3Result permission =
        CheckCameraWritePermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (phase > WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME || !callback ||
        !outToken) {
        return Invalid(mod, "camera modifier arguments are invalid");
    }
    uint64_t publicationEpoch = 0u;
    WotbModV3Result result =
        BeginSubscriptionCreation(mod, &publicationEpoch);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostCameraModifierRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.mod = mod;
    request.phase = phase;
    request.priority = priority;
    ClientHostObjectResponse response = MakeHostResponse();
    uint64_t backendGeneration = 0u;
    result = InvokeHost(
        mod,
        CameraPermissionTier(true),
        kCameraWriteContexts,
        "camera_add_modifier",
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &backendGeneration);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "camera backend returned an invalid modifier token");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        ReleaseHostObject(
            mod,
            "camera_remove_modifier",
            response.object,
            backendGeneration);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "camera modifier allocation failed");
    }
    subscription->kind = SubscriptionKind::CameraModifier;
    subscription->nativeObject = response.object;
    subscription->backendGeneration = backendGeneration;
    subscription->publicationEpoch = publicationEpoch;
    subscription->phase = phase;
    subscription->priority = priority;
    subscription->cameraModifierCallback = callback;
    subscription->userData = userData;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL CameraRemoveModifier(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    const WotbModV3Result permission =
        CheckCameraWritePermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::CameraModifier,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        CameraPermissionTier(true),
        kCameraWriteContexts,
        "camera_remove_modifier");
}

WotbModV3Result WOTBMOD_V3_CALL CameraTransitionTo(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    const WotbModV3CameraTransition* transition) {
    const WotbModV3Result permission =
        CheckCameraWritePermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(transition, WOTBMOD_V3_CAMERA_VERSION) ||
        !ValidStruct(
            &transition->target,
            WOTBMOD_V3_CAMERA_VERSION) ||
        !ValidSceneTransform(&transition->target.transform) ||
        !IsFinite(transition->target.fov_degrees) ||
        transition->target.fov_degrees < 1.0f ||
        transition->target.fov_degrees > 140.0f ||
        !IsFinite(transition->target.near_plane) ||
        transition->target.near_plane < 0.0f ||
        !IsFinite(transition->target.far_plane) ||
        transition->target.far_plane <=
            transition->target.near_plane ||
        !IsFinite(transition->duration_seconds) ||
        transition->duration_seconds < 0.0f ||
        transition->duration_seconds > 30.0f ||
        transition->easing >
            WOTBMOD_V3_EASING_IN_OUT_CUBIC ||
        transition->preserve_game_control > 1u) {
        return Invalid(mod, "camera transition is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = transition;
    request.payload_size = sizeof(*transition);
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        true,
        "camera_transition_to",
        &request,
        &response);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    const WotbModV3Result tracked =
        TrackCameraEffectOwner(mod, response.object);
    if (tracked != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod, "camera_cancel_effects", response.object);
    }
    return tracked;
}

WotbModV3Result WOTBMOD_V3_CALL CameraAddShake(
    WotbModV3Handle mod,
    WotbModV3CameraHandle camera,
    const WotbModV3CameraShake* shake) {
    const WotbModV3Result permission =
        CheckCameraWritePermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(shake, WOTBMOD_V3_CAMERA_VERSION) ||
        !IsFinite(shake->amplitude) || shake->amplitude < 0.0f ||
        shake->amplitude > 25.0f ||
        !IsFinite(shake->frequency) || shake->frequency < 0.0f ||
        shake->frequency > 120.0f ||
        !IsFinite(shake->duration_seconds) ||
        shake->duration_seconds < 0.0f ||
        shake->duration_seconds > 30.0f ||
        !IsFinite(shake->falloff) ||
        shake->falloff < 0.0f || shake->falloff > 16.0f) {
        return Invalid(mod, "camera shake is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.payload = shake;
    request.payload_size = sizeof(*shake);
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeCamera(
        mod,
        camera,
        true,
        "camera_add_shake",
        &request,
        &response);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    const WotbModV3Result tracked =
        TrackCameraEffectOwner(mod, response.object);
    if (tracked != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod, "camera_cancel_effects", response.object);
    }
    return tracked;
}

WotbModV3Result GameplayCameraUnavailable(
    WotbModV3Handle mod,
    const char* operation) {
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        WOTBMOD_V3_CONTEXT_HANGAR |
            WOTBMOD_V3_CONTEXT_BATTLE |
            WOTBMOD_V3_CONTEXT_REPLAY |
            WOTBMOD_V3_CONTEXT_TRAINING,
        "gameplay.tweak.camera",
        operation);
}

WotbModV3Result GameplayFreeCameraUnavailable(
    WotbModV3Handle mod,
    const char* operation) {
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_HANGAR |
            WOTBMOD_V3_CONTEXT_REPLAY |
            WOTBMOD_V3_CONTEXT_TRAINING,
        "gameplay.tweak.freecam",
        operation);
}

bool ValidGameplayFov(float degrees) {
    return IsFinite(degrees) && degrees >= 30.0f && degrees <= 140.0f;
}

constexpr uint64_t kGameplayCameraContexts =
    WOTBMOD_V3_CONTEXT_HANGAR |
    WOTBMOD_V3_CONTEXT_BATTLE |
    WOTBMOD_V3_CONTEXT_REPLAY |
    WOTBMOD_V3_CONTEXT_TRAINING;

WotbModV3Result InvokeGameplayCameraFov(
    WotbModV3Handle mod,
    const char* operation,
    ClientHostObjectRequest* request,
    ClientHostObjectResponse* response) {
    ClientHostObjectRequest activeRequest = MakeHostRequest();
    ClientHostObjectResponse activeResponse = MakeHostResponse();
    WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kGameplayCameraContexts,
        "camera_get_active",
        &activeRequest,
        sizeof(activeRequest),
        &activeResponse,
        sizeof(activeResponse));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (activeResponse.object == 0u) {
        return NotFound(mod, "the client has no active camera");
    }
    ClientHostObjectRequest localRequest = MakeHostRequest();
    ClientHostObjectRequest* effectiveRequest =
        request ? request : &localRequest;
    effectiveRequest->object = activeResponse.object;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kGameplayCameraContexts,
        operation,
        effectiveRequest,
        sizeof(*effectiveRequest),
        response,
        response ? sizeof(*response) : 0u);
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetFov(
    WotbModV3Handle mod,
    float degrees) {
    if (!ValidGameplayFov(degrees)) {
        return Invalid(mod, "gameplay FOV must be within [30, 140]");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.scalar0 = degrees;
    return InvokeGameplayCameraFov(
        mod,
        "camera_set_fov",
        &request,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraGetFov(
    WotbModV3Handle mod,
    float* outDegrees) {
    if (!outDegrees) {
        return Invalid(mod, "gameplay FOV output pointer is null");
    }
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeGameplayCameraFov(
        mod,
        "camera_get_fov",
        nullptr,
        &response);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!IsFinite(response.value_f64) ||
        response.value_f64 < 1.0 ||
        response.value_f64 > 179.0) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "camera backend returned an invalid gameplay FOV");
    }
    *outDegrees = static_cast<float>(response.value_f64);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetFovHangar(
    WotbModV3Handle mod,
    float degrees) {
    return ValidGameplayFov(degrees)
               ? GameplayCameraUnavailable(mod, "camera_set_fov_hangar")
               : Invalid(mod, "hangar FOV must be within [30, 140]");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetFovBattle(
    WotbModV3Handle mod,
    float degrees) {
    return ValidGameplayFov(degrees)
               ? GameplayCameraUnavailable(mod, "camera_set_fov_battle")
               : Invalid(mod, "battle FOV must be within [30, 140]");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetFovSniper(
    WotbModV3Handle mod,
    float degrees) {
    if (!IsFinite(degrees) || degrees < 10.0f || degrees > 90.0f) {
        return Invalid(mod, "sniper FOV must be within [10, 90]");
    }
    return GameplayCameraUnavailable(mod, "camera_set_fov_sniper");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraResetFov(
    WotbModV3Handle mod) {
    return InvokeGameplayCameraFov(
        mod,
        "camera_reset_fov",
        nullptr,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetZoomSteps(
    WotbModV3Handle mod,
    const float* steps,
    uint32_t count) {
    if (!steps || count == 0u || count > 32u) {
        return Invalid(mod, "zoom step array is invalid");
    }
    float previous = 0.0f;
    for (uint32_t index = 0; index < count; ++index) {
        if (!IsFinite(steps[index]) || steps[index] <= previous ||
            steps[index] < 1.0f || steps[index] > 64.0f) {
            return Invalid(
                mod,
                "zoom steps must be finite, ascending, and within [1, 64]");
        }
        previous = steps[index];
    }
    return GameplayCameraUnavailable(mod, "camera_set_zoom_steps");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraGetZoomSteps(
    WotbModV3Handle mod,
    float* outSteps,
    uint32_t* inoutCount) {
    if (!inoutCount) {
        return Invalid(mod, "zoom step count pointer is null");
    }
    (void)outSteps;
    return GameplayCameraUnavailable(mod, "camera_get_zoom_steps");
}

WotbModV3Result GameplayCameraSetMultiplier(
    WotbModV3Handle mod,
    float multiplier,
    float maximum,
    const char* operation) {
    if (!IsFinite(multiplier) || multiplier <= 0.0f ||
        multiplier > maximum) {
        return Invalid(mod, "camera multiplier is out of range");
    }
    return GameplayCameraUnavailable(mod, operation);
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetZoomMultiplier(
    WotbModV3Handle mod,
    float multiplier) {
    return GameplayCameraSetMultiplier(
        mod,
        multiplier,
        8.0f,
        "camera_set_zoom_multiplier");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetMaxZoom(
    WotbModV3Handle mod,
    float multiplier) {
    return GameplayCameraSetMultiplier(
        mod,
        multiplier,
        64.0f,
        "camera_set_max_zoom");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetTransitionMode(
    WotbModV3Handle mod,
    uint32_t mode) {
    if (mode > WOTBMOD_V3_CAMERA_TRANSITION_CUSTOM) {
        return Invalid(mod, "camera transition mode is invalid");
    }
    return GameplayCameraUnavailable(mod, "camera_set_transition_mode");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetTransitionDuration(
    WotbModV3Handle mod,
    float milliseconds) {
    if (!IsFinite(milliseconds) || milliseconds < 0.0f ||
        milliseconds > 2000.0f) {
        return Invalid(
            mod,
            "camera transition duration must be within [0, 2000] ms");
    }
    return GameplayCameraUnavailable(
        mod,
        "camera_set_transition_duration");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetTransitionEasing(
    WotbModV3Handle mod,
    uint32_t easing) {
    if (easing > WOTBMOD_V3_EASING_IN_OUT_CUBIC) {
        return Invalid(mod, "camera transition easing is invalid");
    }
    return GameplayCameraUnavailable(
        mod,
        "camera_set_transition_easing");
}

WotbModV3Result GameplayCameraSetBool(
    WotbModV3Handle mod,
    uint32_t enabled,
    const char* operation) {
    if (enabled > 1u) {
        return Invalid(mod, "camera boolean value must be 0 or 1");
    }
    return GameplayCameraUnavailable(mod, operation);
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetShakeEnabled(
    WotbModV3Handle mod,
    uint32_t enabled) {
    return GameplayCameraSetBool(
        mod,
        enabled,
        "camera_set_shake_enabled");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetShakeIntensity(
    WotbModV3Handle mod,
    float multiplier) {
    if (!IsFinite(multiplier) || multiplier < 0.0f ||
        multiplier > 4.0f) {
        return Invalid(mod, "camera shake intensity is out of range");
    }
    return GameplayCameraUnavailable(
        mod,
        "camera_set_shake_intensity");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetPostprocessing(
    WotbModV3Handle mod,
    uint32_t enabled) {
    return GameplayCameraSetBool(
        mod,
        enabled,
        "camera_set_postprocessing_enabled");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetBloom(
    WotbModV3Handle mod,
    uint32_t enabled) {
    return GameplayCameraSetBool(
        mod,
        enabled,
        "camera_set_bloom_enabled");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetMotionBlur(
    WotbModV3Handle mod,
    uint32_t enabled) {
    return GameplayCameraSetBool(
        mod,
        enabled,
        "camera_set_motion_blur_enabled");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetVignette(
    WotbModV3Handle mod,
    uint32_t enabled) {
    return GameplayCameraSetBool(
        mod,
        enabled,
        "camera_set_vignette_enabled");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetColorGrading(
    WotbModV3Handle mod,
    const char* lutUri) {
    if (!ValidText(lutUri, WOTBMOD_V3_MAX_PATH)) {
        return Invalid(mod, "color grading LUT URI is invalid");
    }
    return GameplayCameraUnavailable(mod, "camera_set_color_grading");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetDofEnabled(
    WotbModV3Handle mod,
    uint32_t enabled) {
    return GameplayCameraSetBool(
        mod,
        enabled,
        "camera_set_dof_enabled");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraSetDofParams(
    WotbModV3Handle mod,
    float focus,
    float aperture,
    float maxBlur) {
    if (!IsFinite(focus) || focus < 0.0f || !IsFinite(aperture) ||
        aperture <= 0.0f || !IsFinite(maxBlur) || maxBlur < 0.0f) {
        return Invalid(mod, "depth-of-field parameters are invalid");
    }
    return GameplayCameraUnavailable(mod, "camera_set_dof_params");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraFreeEnable(
    WotbModV3Handle mod,
    uint32_t enabled) {
    if (enabled > 1u) {
        return Invalid(mod, "free camera enabled must be 0 or 1");
    }
    return GameplayFreeCameraUnavailable(mod, "camera_free_enable");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraFreeSetSpeed(
    WotbModV3Handle mod,
    float speed) {
    if (!IsFinite(speed) || speed <= 0.0f || speed > 10000.0f) {
        return Invalid(mod, "free camera speed is out of range");
    }
    return GameplayFreeCameraUnavailable(mod, "camera_free_set_speed");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraFreeSetPosition(
    WotbModV3Handle mod,
    WotbModV3Vec3 position) {
    if (!IsFinite(position)) {
        return Invalid(mod, "free camera position is invalid");
    }
    return GameplayFreeCameraUnavailable(
        mod,
        "camera_free_set_position");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraFreeSetRotation(
    WotbModV3Handle mod,
    WotbModV3Vec3 rotation) {
    if (!IsFinite(rotation)) {
        return Invalid(mod, "free camera rotation is invalid");
    }
    return GameplayFreeCameraUnavailable(
        mod,
        "camera_free_set_rotation");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraFreeGetPosition(
    WotbModV3Handle mod,
    WotbModV3Vec3* outPosition) {
    if (!outPosition) {
        return Invalid(mod, "free camera position output is null");
    }
    return GameplayFreeCameraUnavailable(
        mod,
        "camera_free_get_position");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraFreeGetRotation(
    WotbModV3Handle mod,
    WotbModV3Vec3* outRotation) {
    if (!outRotation) {
        return Invalid(mod, "free camera rotation output is null");
    }
    return GameplayFreeCameraUnavailable(
        mod,
        "camera_free_get_rotation");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraGetConfig(
    WotbModV3Handle mod,
    WotbModV3GameplayCameraConfig* outConfig) {
    if (!outConfig) {
        return Invalid(mod, "gameplay camera config output is null");
    }
    return GameplayCameraUnavailable(mod, "camera_get_config");
}

WotbModV3Result WOTBMOD_V3_CALL GameplayCameraApplyConfig(
    WotbModV3Handle mod,
    const WotbModV3GameplayCameraConfig* config) {
    if (!ValidStruct(config, WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION) ||
        !ValidGameplayFov(config->fov_hangar) ||
        !ValidGameplayFov(config->fov_battle) ||
        !IsFinite(config->fov_sniper) || config->fov_sniper < 10.0f ||
        config->fov_sniper > 90.0f ||
        !IsFinite(config->zoom_multiplier) ||
        config->zoom_multiplier <= 0.0f ||
        config->zoom_multiplier > 8.0f ||
        config->transition_mode >
            WOTBMOD_V3_CAMERA_TRANSITION_CUSTOM ||
        !IsFinite(config->transition_duration_ms) ||
        config->transition_duration_ms < 0.0f ||
        config->transition_duration_ms > 2000.0f ||
        config->transition_easing >
            WOTBMOD_V3_EASING_IN_OUT_CUBIC ||
        config->shake_enabled > 1u ||
        !IsFinite(config->shake_intensity) ||
        config->shake_intensity < 0.0f ||
        config->shake_intensity > 4.0f ||
        config->postprocessing_enabled > 1u ||
        config->bloom_enabled > 1u ||
        config->motion_blur_enabled > 1u ||
        config->vignette_enabled > 1u ||
        config->dof_enabled > 1u) {
        return Invalid(mod, "gameplay camera config is invalid");
    }
    return GameplayCameraUnavailable(mod, "camera_apply_config");
}

WotbModV3Result GameplayHudUnavailable(
    WotbModV3Handle mod,
    const char* operation,
    uint32_t permissionTier = WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
    const char* capability = "gameplay.tweak.hud") {
    return NativeUnavailable(
        mod,
        permissionTier,
        WOTBMOD_V3_CONTEXT_BATTLE |
            WOTBMOD_V3_CONTEXT_REPLAY |
            WOTBMOD_V3_CONTEXT_TRAINING,
        capability,
        operation);
}

/* ---- HUD on stock DAVA controls -------------------------------------
 *
 * The cheapest honest backend for the HUD interface drives the client's own
 * battle controls by name: visibility and geometry through the same host
 * operations the UI interface uses on game-owned controls (opacity has no
 * host path for game-owned controls - live 2026-09-05). Everything
 * hangs on the names below, which come from LIVE_UI_TREE.jsonl of a training
 * battle (ui.live_text row of the validation mod, 11.20.0.887, dumped
 * 2026-09-05; copy in docs/evidence/LIVE_UI_TREE_battle_11_20_0_887.jsonl). Every
 * name is unique on BattleScreen, so a recursive find from the screen root is
 * unambiguous. An empty name means "not mapped on this client yet" and the
 * slot answers E_NOT_SUPPORTED with that reason; nothing is guessed.
 *
 *   minimap       HUDLayer/MinimapHolder/Minimap (252x252, bottom-left)
 *   sixth_sense   HUDLayer/NotificationHolder/NotificationIconContainer/Lamp
 *   reticle       BattleUILayer/AimContainer/LayoutProtector/GunAim (the
 *                 client reticle with its 32 aimSpread segments; GunAimServer
 *                 and UISightCursor are siblings and stay untouched)
 *   damagelog     HUDLayer/RibbonsContainerLayoutProtector/RibbonsContainer -
 *                 Blitz has no textual damage log; the hit/assist ribbons that
 *                 pop left of centre are its damage feed
 *   session_stats HUDLayer/DamageStatisticsContainerLayoutProtector/
 *                 DamageStatisticsContainer/DamageStatistics (DamageDealt,
 *                 DamageAssisted, DamageBlocked counters on the left edge)
 */
struct HudStockControl {
    const char* key;
    const char* name;   /* DAVA control name on the battle screen; "" = unmapped */
};

const HudStockControl kHudStockControls[] = {
    {"damagelog", "RibbonsContainer"},
    {"minimap", "Minimap"},
    {"sixth_sense", "Lamp"},
    {"reticle", "GunAim"},
    {"session_stats", "DamageStatistics"},
};

struct HudOriginalState {
    WotbModV3Vec2 position = {0.0f, 0.0f};
    WotbModV3Vec2 size = {0.0f, 0.0f};
    uint32_t visible = 1u;
    bool captured = false;
};

std::mutex g_hudMutex;
std::map<std::string, HudOriginalState> g_hudOriginals;

const char* HudStockName(const char* key) {
    for (const HudStockControl& entry : kHudStockControls) {
        if (std::strcmp(entry.key, key) == 0) return entry.name;
    }
    return "";
}

/* Tier and context through CheckAccess, the named grant through
 * CheckNamedPermission - the same split the camera writes use. The stock
 * controls are driven by the UI host bridge, which is not a registered native
 * capability; passing "gameplay.tweak.hud" as a capability name made every
 * slot answer "required capability is not registered" (live 2026-09-05). */
WotbModV3Result HudAccess(WotbModV3Handle mod) {
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        WOTBMOD_V3_CONTEXT_BATTLE |
            WOTBMOD_V3_CONTEXT_REPLAY |
            WOTBMOD_V3_CONTEXT_TRAINING,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    return CheckNamedPermission(
        mod,
        "gameplay.tweak.hud",
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK);
}

/* Access check, then the stock control of `key` on the active screen as a
 * mod-owned wrapper (the caller destroys it). Captures the control's
 * original geometry and visibility the first time it is touched, so reset
 * can put it back. */
WotbModV3Result HudResolve(
    WotbModV3Handle mod,
    const char* key,
    const char* operation,
    WotbModV3UiHandle* outControl) {
    *outControl = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result access = HudAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    const char* name = HudStockName(key);
    if (!name || !name[0]) {
        std::string message(operation);
        message += ": the stock control for \"";
        message += key;
        message += "\" is not mapped on this client build";
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            message.c_str(),
            "{\"backend\":\"stock-control\",\"mapped\":false}");
    }
    WotbModV3UiHandle screen = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result screenResult = UiGetActiveScreen(mod, &screen);
    if (screenResult != WOTBMOD_V3_OK) return screenResult;
    UiControl* root = nullptr;
    WotbModV3Result result = GetUiControl(mod, screen, &root);
    if (result == WOTBMOD_V3_OK) {
        result = UiFindNativeByName(mod, root, name, true, outControl);
    }
    UiControlDestroy(mod, screen);
    if (result != WOTBMOD_V3_OK) {
        std::string message(operation);
        message += ": stock control \"";
        message += name;
        message += "\" is not on the active screen";
        return SetError(mod, WOTBMOD_V3_E_NOT_FOUND, message.c_str());
    }
    WotbModV3UiControlSnapshot snapshot = {};
    WOTBMOD_V3_INIT_STRUCT(snapshot, WOTBMOD_V3_UI_VERSION_3);
    if (UiControlGetSnapshot(mod, *outControl, &snapshot) == WOTBMOD_V3_OK) {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        HudOriginalState& original = g_hudOriginals[key];
        if (!original.captured) {
            original.position = {snapshot.geometry.x, snapshot.geometry.y};
            original.size = {snapshot.geometry.width, snapshot.geometry.height};
            original.visible =
                (snapshot.flags & WOTBMOD_V3_UI_SNAPSHOT_VISIBLE) != 0u ? 1u : 0u;
            original.captured = true;
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result HudSetVisible(
    WotbModV3Handle mod, const char* key, const char* operation, uint32_t enabled) {
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result resolved = HudResolve(mod, key, operation, &control);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    const WotbModV3Result result = UiSetVisible(mod, control, enabled);
    UiControlDestroy(mod, control);
    return result;
}

WotbModV3Result HudSetOpacity(
    WotbModV3Handle mod, const char* key, const char* operation, float opacity) {
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result resolved = HudResolve(mod, key, operation, &control);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    const WotbModV3Result result = UiSetOpacity(mod, control, opacity);
    UiControlDestroy(mod, control);
    return result;
}

/* Scales the control around its original top-left corner. */
WotbModV3Result HudSetScale(
    WotbModV3Handle mod, const char* key, const char* operation, float scale) {
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result resolved = HudResolve(mod, key, operation, &control);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    WotbModV3Vec2 size = {0.0f, 0.0f};
    {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        const HudOriginalState& original = g_hudOriginals[key];
        size = {original.size.x * scale, original.size.y * scale};
    }
    const WotbModV3Result result = UiSetGeometry(mod, control, nullptr, &size);
    UiControlDestroy(mod, control);
    return result;
}

WotbModV3Result HudSetPosition(
    WotbModV3Handle mod, const char* key, const char* operation, WotbModV3Vec2 position) {
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result resolved = HudResolve(mod, key, operation, &control);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    const WotbModV3Result result = UiSetGeometry(mod, control, &position, nullptr);
    UiControlDestroy(mod, control);
    return result;
}

/* Anchors keep the control's size and pin it to one of nine screen points. */
WotbModV3Result HudSetAnchor(
    WotbModV3Handle mod, const char* key, const char* operation, uint32_t anchor) {
    WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result resolved = HudResolve(mod, key, operation, &control);
    if (resolved != WOTBMOD_V3_OK) return resolved;
    WotbModV3UiHandle screen = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiControlSnapshot screenSnapshot = {};
    WOTBMOD_V3_INIT_STRUCT(screenSnapshot, WOTBMOD_V3_UI_VERSION_3);
    WotbModV3Result result = UiGetActiveScreen(mod, &screen);
    if (result == WOTBMOD_V3_OK) {
        result = UiControlGetSnapshot(mod, screen, &screenSnapshot);
        UiControlDestroy(mod, screen);
    }
    if (result != WOTBMOD_V3_OK) {
        UiControlDestroy(mod, control);
        return result;
    }
    WotbModV3Vec2 size = {0.0f, 0.0f};
    {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        size = g_hudOriginals[key].size;
    }
    const float width = screenSnapshot.geometry.width;
    const float height = screenSnapshot.geometry.height;
    const float column = static_cast<float>(anchor % 3u);   /* 0 left, 1 centre, 2 right */
    const float row = static_cast<float>(anchor / 3u);      /* 0 top, 1 middle, 2 bottom */
    const WotbModV3Vec2 position = {
        (width - size.x) * column * 0.5f,
        (height - size.y) * row * 0.5f};
    result = UiSetGeometry(mod, control, &position, nullptr);
    UiControlDestroy(mod, control);
    return result;
}

/* ===================================================================== */
/* Stock HUD extensions (2026-09-05).                                     */
/*                                                                        */
/* Everything below drives the battle screen's own controls, named after  */
/* docs/evidence/LIVE_UI_TREE_battle_11_20_0_887.jsonl. A slot records    */
/* the desired state under g_hudMutex; HudMainThreadTick, called from the */
/* runtime's main-thread pump every frame, resolves the controls (cached  */
/* wrappers, dropped when the screen is rebuilt), applies the state and   */
/* keeps enforcing the parts the game overwrites: the flash and ribbon    */
/* visibility, the reticle colour the game changes by target. A slot      */
/* called on the DAVA main thread applies at once so its result is real;  */
/* from any other thread the change lands within a frame and the slot     */
/* only vouches for the recorded state.                                   */
/* ===================================================================== */
namespace hudx {

constexpr const char* kGunAim = "GunAim";
constexpr const char* kSniperCursor = "sightCursorZoom";
constexpr const char* kReloadContainer = "reloadAndFuelContainer";
constexpr const char* kDrumReload = "DrumReload";
constexpr const char* kHitHighlight = "HitHighlight";
constexpr const char* kRicochetHighlight = "RecochetHighlight";
constexpr const char* kLamp = "Lamp";
constexpr const char* kLampIcon = "Icon";
constexpr const char* kRibbons = "RibbonsContainer";
constexpr const char* kRibbonContent = "content";
constexpr const char* kMinimap = "Minimap";
constexpr const char* kDamageStats = "DamageStatistics";
constexpr const char* kDamageDealt = "DamageDealt";

enum Feature : size_t {
    kFeatureReticleTint = 0u,
    kFeatureDispersion,
    kFeatureReload,
    kFeatureHit,
    kFeatureRibbons,
    kFeatureStats,
    kFeatureMinimapOpacity,
    kFeatureOverlayReticle,
    kFeatureOverlaySniper,
    kFeatureOverlayLamp,
    kFeatureSixthSense,
    kFeatureCount
};

struct Desired {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    bool reticleColorSet = false;
    WotbModV3Color reticleColor = {1.0f, 1.0f, 1.0f, 1.0f};
    bool dispersionSet = false;
    uint32_t dispersionEnabled = 1u;
    bool reloadSet = false;
    uint32_t reloadEnabled = 1u;
    uint32_t hitStyle = WOTBMOD_V3_HIT_STYLE_GAME_DEFAULT;
    bool hitColorSet = false;
    WotbModV3Color hitColor = {1.0f, 1.0f, 1.0f, 1.0f};
    bool ricochetColorSet = false;
    WotbModV3Color ricochetColor = {1.0f, 1.0f, 1.0f, 1.0f};
    uint32_t ribbonCap = 0u;
    bool statsFieldsSet = false;
    uint32_t statsFields = 0u;
    bool minimapOpacitySet = false;
    float minimapOpacity = 1.0f;
    std::string overlayTexture[3];
    std::string sixthSoundUri;
    WotbModV3AudioHandle sixthAudio = WOTBMOD_V3_INVALID_HANDLE;
    float sixthDelayMs = 0.0f;
};

Desired g_desired;                     /* guarded by g_hudMutex */
std::atomic<bool> g_armed{false};
std::atomic<bool> g_resetPending{false};
WotbModV3Result g_lastResult[kFeatureCount] = {};

/* Main-thread state: our wrappers to the stock controls and what we changed
 * on them, keyed by native object. */
struct Cached {
    WotbModV3UiHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t object = 0u;
};
std::map<std::string, Cached> g_cache;
std::map<uint64_t, WotbModV3Color> g_colorOriginals;
std::map<uint64_t, uint32_t> g_visibleOriginals;
std::map<uint64_t, WotbModV3Rect> g_rectOriginals;
struct OverlayState {
    WotbModV3UiHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t target = 0u;
    std::string texture;
};
OverlayState g_overlays[3];
std::vector<WotbModV3UiHandle> g_reticleChildren;
uint64_t g_reticleChildrenOf = 0u;
std::vector<WotbModV3UiHandle> g_minimapNodes;
uint64_t g_fadedObject = 0u;
float g_fadedOpacity = -1.0f;
uint64_t g_statsObject = 0u;
uint32_t g_statsApplied = 0xFFFFFFFFu;
uint32_t g_hitStyleApplied = WOTBMOD_V3_HIT_STYLE_GAME_DEFAULT;
bool g_dispersionHidden = false;
bool g_reloadHidden = false;
bool g_lampWasVisible = false;
bool g_lampForced = false;
std::chrono::steady_clock::time_point g_lampShownAt;
uint64_t g_screenObject = 0u;
uint32_t g_frame = 0u;

bool OnMainThread() {
    return CurrentThreadRole() == WOTBMOD_V3_THREAD_MAIN;
}

uint64_t ObjectOf(WotbModV3Handle mod, WotbModV3UiHandle handle) {
    UiControl* control = nullptr;
    if (handle == WOTBMOD_V3_INVALID_HANDLE ||
        GetUiControl(mod, handle, &control) != WOTBMOD_V3_OK || !control) {
        return 0u;
    }
    return control->nativeObject;
}

bool Snapshot(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3UiControlSnapshot* out) {
    *out = WotbModV3UiControlSnapshot{};
    WOTBMOD_V3_INIT_STRUCT(*out, WOTBMOD_V3_UI_VERSION_3);
    return handle != WOTBMOD_V3_INVALID_HANDLE &&
           UiControlGetSnapshot(mod, handle, out) == WOTBMOD_V3_OK;
}

bool Visible(WotbModV3Handle mod, WotbModV3UiHandle handle, bool* out) {
    WotbModV3UiControlSnapshot snapshot = {};
    if (!Snapshot(mod, handle, &snapshot)) return false;
    *out = (snapshot.flags & WOTBMOD_V3_UI_SNAPSHOT_VISIBLE) != 0u;
    return true;
}

WotbModV3Result GetBackgroundColor(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Color* out) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) return result;
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result host = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_get_background_color",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (host != WOTBMOD_V3_OK) return host;
    *out = {response.vector.x, response.vector.y, response.vector.z,
            response.vector.w};
    return WOTBMOD_V3_OK;
}

WotbModV3Result SetBackgroundColor(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    WotbModV3Color color) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) return result;
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = control->nativeObject;
    request.vector = {color.r, color.g, color.b, color.a};
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "ui_control_set_background_color",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

bool SameRgb(const WotbModV3Color& a, const WotbModV3Color& b) {
    return std::fabs(a.r - b.r) < 0.002f && std::fabs(a.g - b.g) < 0.002f &&
           std::fabs(a.b - b.b) < 0.002f;
}

/* Remembers the colour the control had before we first touched it. */
void RememberColor(WotbModV3Handle mod, WotbModV3UiHandle handle, const WotbModV3Color& current) {
    const uint64_t object = ObjectOf(mod, handle);
    if (object && g_colorOriginals.find(object) == g_colorOriginals.end()) {
        g_colorOriginals[object] = current;
    }
}

void HideRemember(WotbModV3Handle mod, WotbModV3UiHandle handle) {
    bool visible = false;
    if (!Visible(mod, handle, &visible)) return;
    const uint64_t object = ObjectOf(mod, handle);
    if (object && g_visibleOriginals.find(object) == g_visibleOriginals.end()) {
        g_visibleOriginals[object] = visible ? 1u : 0u;
    }
    if (visible) UiSetVisible(mod, handle, 0u);
}

void RestoreVisible(WotbModV3Handle mod, WotbModV3UiHandle handle) {
    const uint64_t object = ObjectOf(mod, handle);
    const auto it = g_visibleOriginals.find(object);
    if (it == g_visibleOriginals.end()) return;
    UiSetVisible(mod, handle, it->second);
    g_visibleOriginals.erase(it);
}

WotbModV3Result FindOnScreen(
    WotbModV3Handle mod,
    const char* name,
    WotbModV3UiHandle* out) {
    *out = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle screen = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = UiGetActiveScreen(mod, &screen);
    if (result != WOTBMOD_V3_OK) return result;
    UiControl* root = nullptr;
    result = GetUiControl(mod, screen, &root);
    if (result == WOTBMOD_V3_OK) {
        result = UiFindNativeByName(mod, root, name, true, out);
    }
    UiControlDestroy(mod, screen);
    return result;
}

WotbModV3Result FindChild(
    WotbModV3Handle mod,
    WotbModV3UiHandle parent,
    const char* name,
    bool recursive,
    WotbModV3UiHandle* out) {
    *out = WOTBMOD_V3_INVALID_HANDLE;
    UiControl* root = nullptr;
    const WotbModV3Result result = GetUiControl(mod, parent, &root);
    if (result != WOTBMOD_V3_OK) return result;
    return UiFindNativeByName(mod, root, name, recursive, out);
}

/* Cached wrapper of a stock control: `parent` INVALID means "search the
 * active screen", otherwise the direct children of that wrapper. */
void DestroyOverlay(WotbModV3Handle mod, size_t index);

WotbModV3Result Resolve(
    WotbModV3Handle mod,
    const char* key,
    const char* name,
    WotbModV3UiHandle parent,
    WotbModV3UiHandle* out) {
    const auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        /* The client rebuilds the HUD (loading screen -> battle, or a
         * respawn): the native control behind a cached wrapper is gone and
         * every later call would answer E_OBJECT_DESTROYED forever. A
         * snapshot is the liveness probe the UI bridge already has; a dead
         * wrapper is dropped and the control looked up afresh. 6 Sep 2026:
         * a reticle overlay applied on the loading screen left
         * reticle.texture answering E_OBJECT_DESTROYED for the whole battle. */
        WotbModV3UiControlSnapshot probe = {};
        if (Snapshot(mod, it->second.handle, &probe)) {
            *out = it->second.handle;
            return WOTBMOD_V3_OK;
        }
        UiControlDestroy(mod, it->second.handle);
        g_cache.erase(it);
        /* Wrappers whose parent was this control are stale as well. */
        for (size_t i = 0u; i < 3u; ++i) {
            if (g_overlays[i].target != 0u) DestroyOverlay(mod, i);
        }
    }
    WotbModV3UiHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = parent == WOTBMOD_V3_INVALID_HANDLE
        ? FindOnScreen(mod, name, &handle)
        : FindChild(mod, parent, name, false, &handle);
    if (result == WOTBMOD_V3_E_NOT_FOUND) {
        std::string message("stock control \"");
        message += name;
        message += "\" is not on the active screen";
        return SetError(mod, WOTBMOD_V3_E_NOT_FOUND, message.c_str());
    }
    if (result != WOTBMOD_V3_OK) return result;
    Cached entry;
    entry.handle = handle;
    entry.object = ObjectOf(mod, handle);
    g_cache[key] = entry;
    *out = handle;
    return WOTBMOD_V3_OK;
}

void DestroyOverlay(WotbModV3Handle mod, size_t index) {
    OverlayState& overlay = g_overlays[index];
    if (overlay.handle != WOTBMOD_V3_INVALID_HANDLE) {
        UiControlDestroy(mod, overlay.handle);
    }
    overlay = OverlayState{};
}

void ReleaseHandles(WotbModV3Handle mod, std::vector<WotbModV3UiHandle>* handles) {
    for (const WotbModV3UiHandle handle : *handles) UiControlDestroy(mod, handle);
    handles->clear();
}

/* Drops every wrapper without touching the game: the screen they pointed
 * at is gone (battle ended or a new one started). */
void ForgetScreen(WotbModV3Handle mod) {
    for (size_t i = 0u; i < 3u; ++i) DestroyOverlay(mod, i);
    ReleaseHandles(mod, &g_reticleChildren);
    ReleaseHandles(mod, &g_minimapNodes);
    for (const auto& entry : g_cache) UiControlDestroy(mod, entry.second.handle);
    g_cache.clear();
    g_colorOriginals.clear();
    g_visibleOriginals.clear();
    g_rectOriginals.clear();
    g_reticleChildrenOf = 0u;
    g_fadedObject = 0u;
    g_fadedOpacity = -1.0f;
    g_statsObject = 0u;
    g_statsApplied = 0xFFFFFFFFu;
    g_hitStyleApplied = WOTBMOD_V3_HIT_STYLE_GAME_DEFAULT;
    g_dispersionHidden = false;
    g_reloadHidden = false;
    g_lampWasVisible = false;
    g_lampForced = false;
}

/* Puts every touched control back and drops the wrappers. */
void RestoreAll(WotbModV3Handle mod) {
    auto restoreOne = [&](WotbModV3UiHandle handle) {
        const uint64_t object = ObjectOf(mod, handle);
        if (!object) return;
        const auto color = g_colorOriginals.find(object);
        if (color != g_colorOriginals.end()) SetBackgroundColor(mod, handle, color->second);
        const auto rect = g_rectOriginals.find(object);
        if (rect != g_rectOriginals.end()) {
            const WotbModV3Vec2 position = {rect->second.x, rect->second.y};
            const WotbModV3Vec2 size = {rect->second.width, rect->second.height};
            UiSetGeometry(mod, handle, &position, &size);
        }
        const auto visible = g_visibleOriginals.find(object);
        if (visible != g_visibleOriginals.end()) UiSetVisible(mod, handle, visible->second);
    };
    for (size_t i = 0u; i < 3u; ++i) DestroyOverlay(mod, i);
    for (const WotbModV3UiHandle handle : g_reticleChildren) restoreOne(handle);
    for (const WotbModV3UiHandle handle : g_minimapNodes) restoreOne(handle);
    for (const auto& entry : g_cache) restoreOne(entry.second.handle);
    ForgetScreen(mod);
}

/* The aimSpread segments are created and dropped by the game as the gun
 * settles and moves (live 2026-09-05: 32 in one battle, none a battle later
 * on the same tank), so the child list is rebuilt whenever the count moves. */
void EnsureReticleChildren(WotbModV3Handle mod, WotbModV3UiHandle aim) {
    const uint64_t object = ObjectOf(mod, aim);
    uint32_t count = 0u;
    if (UiControlGetChildCount(mod, aim, &count) != WOTBMOD_V3_OK) count = 0u;
    if (g_reticleChildrenOf == object &&
        static_cast<uint32_t>(g_reticleChildren.size()) == count) {
        return;
    }
    ReleaseHandles(mod, &g_reticleChildren);
    if (count != 0u) {
        for (uint32_t i = 0u; i < count && i < 64u; ++i) {
            WotbModV3UiHandle child = WOTBMOD_V3_INVALID_HANDLE;
            if (UiControlGetChildAt(mod, aim, i, &child) == WOTBMOD_V3_OK &&
                child != WOTBMOD_V3_INVALID_HANDLE) {
                g_reticleChildren.push_back(child);
            }
        }
    }
    g_reticleChildrenOf = object;
}

WotbModV3Result ApplyReticleTint(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle aim = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = Resolve(mod, "GunAim", kGunAim, WOTBMOD_V3_INVALID_HANDLE, &aim);
    if (result != WOTBMOD_V3_OK) return result;
    EnsureReticleChildren(mod, aim);
    /* The game recolours the reticle by target (friend, foe, nothing), so the
     * tint is re-asserted every frame on the root and its spread segments. */
    size_t tinted = 0u;
    auto tint = [&](WotbModV3UiHandle handle) {
        WotbModV3Color current = {};
        if (GetBackgroundColor(mod, handle, &current) != WOTBMOD_V3_OK) return;
        RememberColor(mod, handle, current);
        ++tinted;
        if (SameRgb(current, d.reticleColor) &&
            std::fabs(current.a - d.reticleColor.a) < 0.002f) {
            return;
        }
        SetBackgroundColor(mod, handle, d.reticleColor);
    };
    tint(aim);
    for (const WotbModV3UiHandle child : g_reticleChildren) tint(child);
    /* GunAim itself is a bare container; the segments carry the sprites and
     * exist only while the gun is aiming, so an empty pass is not a failure:
     * the tint lands the moment the game creates them. */
    (void)tinted;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyDispersion(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle aim = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = Resolve(mod, "GunAim", kGunAim, WOTBMOD_V3_INVALID_HANDLE, &aim);
    if (result != WOTBMOD_V3_OK) return result;
    EnsureReticleChildren(mod, aim);
    if (d.dispersionEnabled == 0u) {
        for (const WotbModV3UiHandle child : g_reticleChildren) HideRemember(mod, child);
        g_dispersionHidden = true;
    } else if (g_dispersionHidden) {
        for (const WotbModV3UiHandle child : g_reticleChildren) RestoreVisible(mod, child);
        g_dispersionHidden = false;
    }
    /* No segments right now means the gun is settled; the hide is enforced
     * each frame as they appear. */
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyReload(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle container = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle drum = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result =
        Resolve(mod, "reloadAndFuelContainer", kReloadContainer, WOTBMOD_V3_INVALID_HANDLE, &container);
    if (result != WOTBMOD_V3_OK) return result;
    Resolve(mod, "DrumReload", kDrumReload, WOTBMOD_V3_INVALID_HANDLE, &drum);
    if (d.reloadEnabled == 0u) {
        HideRemember(mod, container);
        if (drum != WOTBMOD_V3_INVALID_HANDLE) HideRemember(mod, drum);
        g_reloadHidden = true;
    } else if (g_reloadHidden) {
        RestoreVisible(mod, container);
        if (drum != WOTBMOD_V3_INVALID_HANDLE) RestoreVisible(mod, drum);
        g_reloadHidden = false;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyHit(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle hit = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle ricochet = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result =
        Resolve(mod, "HitHighlight", kHitHighlight, WOTBMOD_V3_INVALID_HANDLE, &hit);
    if (result != WOTBMOD_V3_OK) return result;
    Resolve(mod, "RecochetHighlight", kRicochetHighlight, WOTBMOD_V3_INVALID_HANDLE, &ricochet);
    const WotbModV3UiHandle flashes[2] = {hit, ricochet};
    /* Style: MINIMAL hides the full-screen flashes for good (the game shows
     * them again on every hit, so this is enforced each frame); COMPACT
     * shrinks them to the central 40% of their own rectangle. */
    if (d.hitStyle == WOTBMOD_V3_HIT_STYLE_MINIMAL) {
        for (const WotbModV3UiHandle flash : flashes) {
            if (flash != WOTBMOD_V3_INVALID_HANDLE) HideRemember(mod, flash);
        }
    } else if (g_hitStyleApplied == WOTBMOD_V3_HIT_STYLE_MINIMAL) {
        for (const WotbModV3UiHandle flash : flashes) {
            if (flash != WOTBMOD_V3_INVALID_HANDLE) RestoreVisible(mod, flash);
        }
    }
    if (d.hitStyle == WOTBMOD_V3_HIT_STYLE_COMPACT &&
        g_hitStyleApplied != WOTBMOD_V3_HIT_STYLE_COMPACT) {
        for (const WotbModV3UiHandle flash : flashes) {
            WotbModV3UiControlSnapshot snapshot = {};
            if (flash == WOTBMOD_V3_INVALID_HANDLE || !Snapshot(mod, flash, &snapshot)) continue;
            const uint64_t object = ObjectOf(mod, flash);
            if (object && g_rectOriginals.find(object) == g_rectOriginals.end()) {
                g_rectOriginals[object] = snapshot.geometry;
            }
            const WotbModV3Rect& base = g_rectOriginals[object];
            const WotbModV3Vec2 position = {base.x + base.width * 0.3f, base.y + base.height * 0.3f};
            const WotbModV3Vec2 size = {base.width * 0.4f, base.height * 0.4f};
            UiSetGeometry(mod, flash, &position, &size);
        }
    } else if (d.hitStyle != WOTBMOD_V3_HIT_STYLE_COMPACT &&
               g_hitStyleApplied == WOTBMOD_V3_HIT_STYLE_COMPACT) {
        for (const WotbModV3UiHandle flash : flashes) {
            const uint64_t object = ObjectOf(mod, flash);
            const auto rect = g_rectOriginals.find(object);
            if (rect == g_rectOriginals.end()) continue;
            const WotbModV3Vec2 position = {rect->second.x, rect->second.y};
            const WotbModV3Vec2 size = {rect->second.width, rect->second.height};
            UiSetGeometry(mod, flash, &position, &size);
            g_rectOriginals.erase(rect);
        }
    }
    g_hitStyleApplied = d.hitStyle;
    /* Colours: the game animates the flash alpha, so only RGB is ours and it
     * is re-asserted each frame while the flash is up. */
    auto recolour = [&](WotbModV3UiHandle flash, const WotbModV3Color& wanted) -> WotbModV3Result {
        if (flash == WOTBMOD_V3_INVALID_HANDLE) return WOTBMOD_V3_E_NOT_FOUND;
        WotbModV3Color current = {};
        const WotbModV3Result read = GetBackgroundColor(mod, flash, &current);
        /* a hidden flash has no background component yet (the game creates
         * it with the first hit); the colour is re-tried every frame */
        if (read == WOTBMOD_V3_E_NOT_FOUND) return WOTBMOD_V3_OK;
        if (read != WOTBMOD_V3_OK) return read;
        RememberColor(mod, flash, current);
        if (SameRgb(current, wanted)) return WOTBMOD_V3_OK;
        const WotbModV3Color next = {wanted.r, wanted.g, wanted.b, current.a};
        return SetBackgroundColor(mod, flash, next);
    };
    WotbModV3Result worst = WOTBMOD_V3_OK;
    if (d.hitColorSet) {
        const WotbModV3Result one = recolour(hit, d.hitColor);
        if (one != WOTBMOD_V3_OK) worst = one;
    }
    if (d.ricochetColorSet) {
        const WotbModV3Result one = recolour(ricochet, d.ricochetColor);
        if (one != WOTBMOD_V3_OK) worst = one;
    }
    return worst;
}

WotbModV3Result ApplyRibbons(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle ribbons = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle content = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result =
        Resolve(mod, "RibbonsContainer", kRibbons, WOTBMOD_V3_INVALID_HANDLE, &ribbons);
    if (result != WOTBMOD_V3_OK) return result;
    result = Resolve(mod, "RibbonsContainer/content", kRibbonContent, ribbons, &content);
    if (result != WOTBMOD_V3_OK) return result;
    uint32_t count = 0u;
    result = UiControlGetChildCount(mod, content, &count);
    if (result != WOTBMOD_V3_OK) return result;
    if (count <= d.ribbonCap) return WOTBMOD_V3_OK;
    /* Ribbons are appended as they happen: the oldest come first. */
    for (uint32_t i = 0u; i + d.ribbonCap < count && i < 64u; ++i) {
        WotbModV3UiHandle ribbon = WOTBMOD_V3_INVALID_HANDLE;
        if (UiControlGetChildAt(mod, content, i, &ribbon) != WOTBMOD_V3_OK ||
            ribbon == WOTBMOD_V3_INVALID_HANDLE) {
            continue;
        }
        bool visible = false;
        if (Visible(mod, ribbon, &visible) && visible) UiSetVisible(mod, ribbon, 0u);
        UiControlDestroy(mod, ribbon);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplyStats(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle block = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3UiHandle dealt = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result =
        Resolve(mod, "DamageStatistics", kDamageStats, WOTBMOD_V3_INVALID_HANDLE, &block);
    if (result != WOTBMOD_V3_OK) return result;
    result = Resolve(mod, "DamageStatistics/DamageDealt", kDamageDealt, block, &dealt);
    if (result != WOTBMOD_V3_OK) return result;
    const uint64_t object = ObjectOf(mod, block);
    if (g_statsObject == object && g_statsApplied == d.statsFields) return WOTBMOD_V3_OK;
    auto set = [&](WotbModV3UiHandle handle, bool wanted) {
        if (wanted) {
            RestoreVisible(mod, handle);
            bool visible = false;
            if (Visible(mod, handle, &visible) && !visible) {
                const uint64_t o = ObjectOf(mod, handle);
                if (o && g_visibleOriginals.find(o) == g_visibleOriginals.end()) g_visibleOriginals[o] = 0u;
                UiSetVisible(mod, handle, 1u);
            }
        } else {
            HideRemember(mod, handle);
        }
    };
    set(block, d.statsFields != 0u);
    set(dealt, (d.statsFields & WOTBMOD_V3_STAT_DAMAGE) != 0u);
    g_statsObject = object;
    g_statsApplied = d.statsFields;
    return WOTBMOD_V3_OK;
}

void CollectDrawable(
    WotbModV3Handle mod,
    WotbModV3UiHandle root,
    uint32_t depth,
    std::vector<WotbModV3UiHandle>* out) {
    if (depth > 6u || out->size() >= 256u) return;
    uint32_t count = 0u;
    if (UiControlGetChildCount(mod, root, &count) != WOTBMOD_V3_OK) return;
    for (uint32_t i = 0u; i < count && i < 128u; ++i) {
        WotbModV3UiHandle child = WOTBMOD_V3_INVALID_HANDLE;
        if (UiControlGetChildAt(mod, root, i, &child) != WOTBMOD_V3_OK ||
            child == WOTBMOD_V3_INVALID_HANDLE) {
            continue;
        }
        out->push_back(child);
        CollectDrawable(mod, child, depth + 1u, out);
    }
}

WotbModV3Result ApplyMinimapOpacity(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle minimap = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result =
        Resolve(mod, "Minimap", kMinimap, WOTBMOD_V3_INVALID_HANDLE, &minimap);
    if (result != WOTBMOD_V3_OK) return result;
    const uint64_t object = ObjectOf(mod, minimap);
    if (g_fadedObject == object && std::fabs(g_fadedOpacity - d.minimapOpacity) < 0.002f) {
        return WOTBMOD_V3_OK;
    }
    if (g_fadedObject != object) {
        /* the root wrapper belongs to the cache; this list holds the
         * descendants only */
        ReleaseHandles(mod, &g_minimapNodes);
        CollectDrawable(mod, minimap, 0u, &g_minimapNodes);
    }
    size_t faded = 0u;
    auto fade = [&](WotbModV3UiHandle handle) {
        WotbModV3Color current = {};
        if (GetBackgroundColor(mod, handle, &current) != WOTBMOD_V3_OK) return;
        RememberColor(mod, handle, current);
        const uint64_t o = ObjectOf(mod, handle);
        const WotbModV3Color& base = g_colorOriginals[o];
        WotbModV3Color next = base;
        next.a = base.a * d.minimapOpacity;
        SetBackgroundColor(mod, handle, next);
        ++faded;
    };
    fade(minimap);
    for (const WotbModV3UiHandle node : g_minimapNodes) fade(node);
    g_fadedObject = object;
    g_fadedOpacity = d.minimapOpacity;
    if (faded == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "minimap_set_opacity: no control under Minimap carries a background to fade");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result EnsureOverlay(
    WotbModV3Handle mod,
    size_t index,
    const char* key,
    const char* targetName,
    const char* iconChild,
    const std::string& texture) {
    OverlayState& overlay = g_overlays[index];
    /* The control that actually draws the picture: the icon child when the
     * target has one, otherwise the target itself. */
    const std::string pictureKey =
        iconChild ? std::string(key) + "/picture" : std::string(key);
    if (texture.empty()) {
        if (overlay.handle != WOTBMOD_V3_INVALID_HANDLE) {
            /* texture cleared: the hidden original comes back */
            const auto it = g_cache.find(pictureKey);
            if (it != g_cache.end()) {
                const auto color = g_colorOriginals.find(it->second.object);
                if (color != g_colorOriginals.end()) {
                    SetBackgroundColor(mod, it->second.handle, color->second);
                    g_colorOriginals.erase(color);
                }
            }
            DestroyOverlay(mod, index);
        }
        return WOTBMOD_V3_OK;
    }
    WotbModV3UiHandle target = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = Resolve(mod, key, targetName, WOTBMOD_V3_INVALID_HANDLE, &target);
    if (result != WOTBMOD_V3_OK) return result;
    const uint64_t targetObject = ObjectOf(mod, target);
    if (overlay.handle != WOTBMOD_V3_INVALID_HANDLE && overlay.target == targetObject &&
        overlay.texture == texture) {
        return WOTBMOD_V3_OK;
    }
    DestroyOverlay(mod, index);
    WotbModV3UiControlSnapshot snapshot = {};
    if (!Snapshot(mod, target, &snapshot)) {
        return SetError(mod, WOTBMOD_V3_E_OBJECT_DESTROYED, "HUD overlay target vanished");
    }
    WotbModV3UiControlDescriptor descriptor = {};
    WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_UI_VERSION);
    descriptor.type = WOTBMOD_V3_UI_CONTROL_IMAGE;
    descriptor.visible = 1u;
    descriptor.id = "wotbmod.hud.overlay";
    descriptor.text = nullptr;
    descriptor.texture_uri = texture.c_str();
    descriptor.geometry = {0.0f, 0.0f, snapshot.geometry.width, snapshot.geometry.height};
    WotbModV3UiHandle created = WOTBMOD_V3_INVALID_HANDLE;
    result = UiControlCreateInternal(mod, &descriptor, &created);
    if (result != WOTBMOD_V3_OK) return result;
    result = UiControlSetParent(mod, created, target);
    if (result != WOTBMOD_V3_OK) {
        UiControlDestroy(mod, created);
        return result;
    }
    /* The original picture goes fully transparent underneath: the target's
     * own background, or the icon child that actually draws it. */
    WotbModV3UiHandle pictured = target;
    if (iconChild) {
        WotbModV3UiHandle icon = WOTBMOD_V3_INVALID_HANDLE;
        if (Resolve(mod, pictureKey.c_str(), iconChild, target, &icon) == WOTBMOD_V3_OK) {
            pictured = icon;
        }
    }
    WotbModV3Color current = {};
    if (GetBackgroundColor(mod, pictured, &current) == WOTBMOD_V3_OK) {
        RememberColor(mod, pictured, current);
        current.a = 0.0f;
        SetBackgroundColor(mod, pictured, current);
    }
    overlay.handle = created;
    overlay.target = targetObject;
    overlay.texture = texture;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ApplySixthSense(WotbModV3Handle mod, const Desired& d) {
    WotbModV3UiHandle lamp = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = Resolve(mod, "Lamp", kLamp, WOTBMOD_V3_INVALID_HANDLE, &lamp);
    if (result != WOTBMOD_V3_OK) return result;
    bool visible = false;
    if (!Visible(mod, lamp, &visible)) return WOTBMOD_V3_OK;
    const auto now = std::chrono::steady_clock::now();
    if (visible && !g_lampWasVisible) {
        g_lampWasVisible = true;
        g_lampShownAt = now;
        g_lampForced = false;
        if (d.sixthAudio != WOTBMOD_V3_INVALID_HANDLE) {
            AudioStop(mod, d.sixthAudio, 0.0f);
            AudioPlay(mod, d.sixthAudio);
        }
        return WOTBMOD_V3_OK;
    }
    if (!visible && g_lampWasVisible && !g_lampForced) {
        const double elapsed = std::chrono::duration<double, std::milli>(now - g_lampShownAt).count();
        if (d.sixthDelayMs > 0.0f && elapsed < d.sixthDelayMs) {
            /* the game hid it early: keep the lamp up until the delay is over */
            UiSetVisible(mod, lamp, 1u);
            g_lampForced = true;
        } else {
            g_lampWasVisible = false;
        }
        return WOTBMOD_V3_OK;
    }
    if (g_lampForced) {
        const double elapsed = std::chrono::duration<double, std::milli>(now - g_lampShownAt).count();
        if (elapsed >= d.sixthDelayMs) {
            UiSetVisible(mod, lamp, 0u);
            g_lampForced = false;
            g_lampWasVisible = false;
        }
    }
    return WOTBMOD_V3_OK;
}

/* One pass over everything desired; each feature's result is kept for the
 * slot that asked for it. Main thread only. */
void ApplyAll(WotbModV3Handle mod, const Desired& d) {
    if (d.reticleColorSet) g_lastResult[kFeatureReticleTint] = ApplyReticleTint(mod, d);
    if (d.dispersionSet) g_lastResult[kFeatureDispersion] = ApplyDispersion(mod, d);
    if (d.reloadSet) g_lastResult[kFeatureReload] = ApplyReload(mod, d);
    if (d.hitStyle != WOTBMOD_V3_HIT_STYLE_GAME_DEFAULT || d.hitColorSet ||
        d.ricochetColorSet || g_hitStyleApplied != WOTBMOD_V3_HIT_STYLE_GAME_DEFAULT) {
        g_lastResult[kFeatureHit] = ApplyHit(mod, d);
    }
    if (d.ribbonCap != 0u) g_lastResult[kFeatureRibbons] = ApplyRibbons(mod, d);
    if (d.statsFieldsSet) g_lastResult[kFeatureStats] = ApplyStats(mod, d);
    if (d.minimapOpacitySet) g_lastResult[kFeatureMinimapOpacity] = ApplyMinimapOpacity(mod, d);
    g_lastResult[kFeatureOverlayReticle] =
        EnsureOverlay(mod, 0u, "GunAim", kGunAim, nullptr, d.overlayTexture[0]);
    g_lastResult[kFeatureOverlaySniper] =
        EnsureOverlay(mod, 1u, "sightCursorZoom", kSniperCursor, nullptr, d.overlayTexture[1]);
    g_lastResult[kFeatureOverlayLamp] =
        EnsureOverlay(mod, 2u, "Lamp", kLamp, kLampIcon, d.overlayTexture[2]);
    if (!d.sixthSoundUri.empty() || d.sixthDelayMs > 0.0f) {
        g_lastResult[kFeatureSixthSense] = ApplySixthSense(mod, d);
    }
}

bool InBattle() {
    const uint64_t context = CurrentContext();
    return (context & (WOTBMOD_V3_CONTEXT_BATTLE | WOTBMOD_V3_CONTEXT_REPLAY |
                       WOTBMOD_V3_CONTEXT_TRAINING)) != 0u;
}

/* Runs the whole pass now when the caller is already on the main thread
 * and answers with that feature's result; otherwise the tick applies it
 * within a frame and the slot vouches for the recorded state only. */
WotbModV3Result Commit(WotbModV3Handle mod, Feature feature) {
    g_armed.store(true);
    if (!OnMainThread()) return WOTBMOD_V3_OK;
    Desired copy;
    {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        copy = g_desired;
    }
    g_lastResult[feature] = WOTBMOD_V3_OK;
    ApplyAll(mod, copy);
    return g_lastResult[feature];
}

}  // namespace hudx

/* Called from the runtime's main-thread pump every frame (through the
 * exported HudMainThreadTick below: this file keeps its helpers unnamed). */
void HudMainThreadTickImpl() {
    using namespace hudx;
    if (!g_armed.load() && !g_resetPending.load()) return;
    Desired copy;
    {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        copy = g_desired;
    }
    const WotbModV3Handle mod = copy.owner;
    if (mod == WOTBMOD_V3_INVALID_HANDLE) return;
    const uint32_t previousRole = SetCurrentThreadRole(WOTBMOD_V3_THREAD_MAIN);
    if (g_resetPending.exchange(false)) {
        if (CheckMod(mod) == WOTBMOD_V3_OK) RestoreAll(mod);
        else ForgetScreen(mod);
        g_armed.store(false);
        SetCurrentThreadRole(previousRole);
        return;
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) {
        /* the owner went away without reset(): nothing can be restored with
         * its handles, drop the state so the next owner starts clean */
        ForgetScreen(mod);
        std::lock_guard<std::mutex> lock(g_hudMutex);
        g_desired = Desired{};
        g_armed.store(false);
        SetCurrentThreadRole(previousRole);
        return;
    }
    if (!InBattle()) {
        SetCurrentThreadRole(previousRole);
        return;
    }
    ++g_frame;
    /* Every 30 frames: is this still the screen the wrappers were made for?
     * A rebuilt battle screen means every cached control is gone. */
    if (g_frame % 30u == 1u) {
        WotbModV3UiHandle screen = WOTBMOD_V3_INVALID_HANDLE;
        if (UiGetActiveScreen(mod, &screen) == WOTBMOD_V3_OK) {
            const uint64_t object = ObjectOf(mod, screen);
            UiControlDestroy(mod, screen);
            if (object != g_screenObject) {
                ForgetScreen(mod);
                g_screenObject = object;
            }
        }
    }
    ApplyAll(mod, copy);
    SetCurrentThreadRole(previousRole);
}

WotbModV3Result HudResetStock(WotbModV3Handle mod) {
    std::vector<std::pair<std::string, HudOriginalState>> originals;
    WotbModV3AudioHandle audio = WOTBMOD_V3_INVALID_HANDLE;
    bool extended = false;
    {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        for (const auto& entry : g_hudOriginals) {
            if (entry.second.captured) originals.push_back(entry);
        }
        audio = hudx::g_desired.sixthAudio;
        extended = hudx::g_desired.owner != WOTBMOD_V3_INVALID_HANDLE;
        hudx::g_desired = hudx::Desired{};
    }
    if (audio != WOTBMOD_V3_INVALID_HANDLE) {
        AudioStop(mod, audio, 0.0f);
        AudioDestroy(mod, audio);
    }
    if (extended) {
        if (hudx::OnMainThread()) {
            const uint32_t previousRole = SetCurrentThreadRole(WOTBMOD_V3_THREAD_MAIN);
            hudx::RestoreAll(mod);
            SetCurrentThreadRole(previousRole);
            hudx::g_armed.store(false);
        } else {
            hudx::g_resetPending.store(true);
        }
    }
    WotbModV3Result worst = WOTBMOD_V3_OK;
    for (const auto& entry : originals) {
        WotbModV3UiHandle control = WOTBMOD_V3_INVALID_HANDLE;
        const WotbModV3Result resolved =
            HudResolve(mod, entry.first.c_str(), "hud_reset", &control);
        if (resolved != WOTBMOD_V3_OK) { worst = resolved; continue; }
        UiSetGeometry(mod, control, &entry.second.position, &entry.second.size);
        UiSetVisible(mod, control, entry.second.visible);
        UiSetOpacity(mod, control, 1.0f);
        UiControlDestroy(mod, control);
    }
    if (originals.empty() && !extended) {
        /* Nothing was ever touched: still an honest answer about access. */
        return HudAccess(mod);
    }
    return worst;
}

/* A slot that only records desired state: access first, then the record,
 * then Commit applies it when the caller is on the main thread. */
#define WOTBMOD_HUD_RECORD(feature, ...)                                   \
    do {                                                                    \
        const WotbModV3Result access_ = HudAccess(mod);                     \
        if (access_ != WOTBMOD_V3_OK) return access_;                       \
        {                                                                   \
            std::lock_guard<std::mutex> lock_(g_hudMutex);                  \
            hudx::g_desired.owner = mod;                                    \
            __VA_ARGS__;                                                    \
        }                                                                   \
        return hudx::Commit(mod, feature);                                  \
    } while (0)

/* Texture URIs: mod:// and game:// resolve to the physical file, which is
 * what the loader's YAML `sprite:` key can open; anything else is passed as
 * the caller wrote it (a game resource path). */
WotbModV3Result HudResolveTexture(
    WotbModV3Handle mod,
    const char* uri,
    std::string* out) {
    if (!ValidText(uri, WOTBMOD_V3_MAX_PATH)) {
        return Invalid(mod, "HUD resource URI is invalid");
    }
    /* The overlay is a control the mod owns, so the mod needs the grant that
     * covers its own controls; said up front rather than from deep inside
     * the parenting call (live 2026-09-05: "did not declare or receive the
     * named permission" with no hint which one). */
    if (CheckNamedPermission(mod, "ui.modify.own", WOTBMOD_V3_PERMISSION_SAFE) != WOTBMOD_V3_OK) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "HUD texture overlays are mod-owned image controls; the manifest must request ui.modify.own");
    }
    std::string physical;
    const bool virtualUri =
        std::strncmp(uri, "mod://", 6u) == 0 || std::strncmp(uri, "game://", 7u) == 0;
    if (virtualUri) {
        /* gameplay.tweak.hud already covers the caller; the VFS permission
         * that ResolveVfsUriPhysical asks for is not required on top. */
        const WotbModV3Result resolved = ResolveUriPhysicalForOwner(mod, uri, &physical);
        if (resolved != WOTBMOD_V3_OK) return resolved;
    }
    *out = physical.empty() ? std::string(uri) : physical;
    for (char& character : *out) {
        if (character == '\\') character = '/';
    }
    return WOTBMOD_V3_OK;
}

#define WOTBMOD_DEFINE_HUD_PATH(functionName, operationName)             \
    WotbModV3Result WOTBMOD_V3_CALL functionName(                       \
        WotbModV3Handle mod,                                             \
        const char* uri) {                                               \
        if (!ValidText(uri, WOTBMOD_V3_MAX_PATH)) {                      \
            return Invalid(mod, "HUD resource URI is invalid");          \
        }                                                                \
        return GameplayHudUnavailable(mod, operationName);               \
    }

#define WOTBMOD_DEFINE_HUD_BOOL(functionName, operationName)             \
    WotbModV3Result WOTBMOD_V3_CALL functionName(                       \
        WotbModV3Handle mod,                                             \
        uint32_t enabled) {                                              \
        if (enabled > 1u) {                                              \
            return Invalid(mod, "HUD boolean must be 0 or 1");           \
        }                                                                \
        return GameplayHudUnavailable(mod, operationName);               \
    }

#define WOTBMOD_DEFINE_HUD_COLOR(functionName, operationName)            \
    WotbModV3Result WOTBMOD_V3_CALL functionName(                       \
        WotbModV3Handle mod,                                             \
        uint32_t rgba) {                                                 \
        (void)rgba;                                                      \
        return GameplayHudUnavailable(mod, operationName);               \
    }

WotbModV3Result WOTBMOD_V3_CALL HudReticleSetTexture(
    WotbModV3Handle mod,
    const char* uri) {
    std::string texture;
    if (uri && uri[0]) {
        const WotbModV3Result resolved = HudResolveTexture(mod, uri, &texture);
        if (resolved != WOTBMOD_V3_OK) return resolved;
    }
    WOTBMOD_HUD_RECORD(hudx::kFeatureOverlayReticle,
        hudx::g_desired.overlayTexture[0] = texture);
}

WotbModV3Result WOTBMOD_V3_CALL HudReticleSetColor(
    WotbModV3Handle mod,
    uint32_t rgba) {
    const WotbModV3Color color = {
        static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgba & 0xFFu) / 255.0f};
    WOTBMOD_HUD_RECORD(hudx::kFeatureReticleTint,
        hudx::g_desired.reticleColorSet = true;
        hudx::g_desired.reticleColor = color);
}

WotbModV3Result WOTBMOD_V3_CALL HudReticleSetSize(
    WotbModV3Handle mod,
    float scale) {
    if (!IsFinite(scale) || scale < 0.25f || scale > 4.0f) {
        return Invalid(mod, "reticle scale is out of range");
    }
    return HudSetScale(mod, "reticle", "reticle_set_size", scale);
}

WotbModV3Result WOTBMOD_V3_CALL HudReticleSetSniperTexture(
    WotbModV3Handle mod,
    const char* uri) {
    std::string texture;
    if (uri && uri[0]) {
        const WotbModV3Result resolved = HudResolveTexture(mod, uri, &texture);
        if (resolved != WOTBMOD_V3_OK) return resolved;
    }
    WOTBMOD_HUD_RECORD(hudx::kFeatureOverlaySniper,
        hudx::g_desired.overlayTexture[1] = texture);
}

WotbModV3Result WOTBMOD_V3_CALL HudReticleSetReloading(
    WotbModV3Handle mod,
    uint32_t enabled) {
    if (enabled > 1u) return Invalid(mod, "HUD boolean must be 0 or 1");
    WOTBMOD_HUD_RECORD(hudx::kFeatureReload,
        hudx::g_desired.reloadSet = true;
        hudx::g_desired.reloadEnabled = enabled);
}

WotbModV3Result WOTBMOD_V3_CALL HudReticleSetDispersion(
    WotbModV3Handle mod,
    uint32_t enabled) {
    if (enabled > 1u) return Invalid(mod, "HUD boolean must be 0 or 1");
    WOTBMOD_HUD_RECORD(hudx::kFeatureDispersion,
        hudx::g_desired.dispersionSet = true;
        hudx::g_desired.dispersionEnabled = enabled);
}
WotbModV3Result WOTBMOD_V3_CALL HudDamageLogSetEnabled(
    WotbModV3Handle mod,
    uint32_t enabled) {
    if (enabled > 1u) return Invalid(mod, "HUD boolean must be 0 or 1");
    return HudSetVisible(mod, "damagelog", "damagelog_set_enabled", enabled);
}

WotbModV3Result WOTBMOD_V3_CALL HudDamageLogSetPosition(
    WotbModV3Handle mod,
    uint32_t anchor) {
    if (anchor > WOTBMOD_V3_HUD_ANCHOR_BOTTOM_RIGHT) {
        return Invalid(mod, "damage log anchor is invalid");
    }
    return HudSetAnchor(mod, "damagelog", "damagelog_set_position", anchor);
}

WotbModV3Result WOTBMOD_V3_CALL HudDamageLogSetMaxEntries(
    WotbModV3Handle mod,
    uint32_t count) {
    if (count == 0u || count > 256u) {
        return Invalid(mod, "damage log entry limit is out of range");
    }
    /* The ribbon feed keeps at most `count` ribbons on screen: older ones are
     * hidden as new ones arrive (enforced each frame). */
    WOTBMOD_HUD_RECORD(hudx::kFeatureRibbons,
        hudx::g_desired.ribbonCap = count);
}

WOTBMOD_DEFINE_HUD_BOOL(
    HudDamageLogSetShowBlocked,
    "damagelog_set_show_blocked")
WOTBMOD_DEFINE_HUD_BOOL(
    HudDamageLogSetShowRicochet,
    "damagelog_set_show_ricochet")
WOTBMOD_DEFINE_HUD_BOOL(
    HudDamageLogSetShowModule,
    "damagelog_set_show_module_damage")

WotbModV3Result WOTBMOD_V3_CALL HudDamageLogSetFormat(
    WotbModV3Handle mod,
    const char* format) {
    if (!ValidText(format, WOTBMOD_V3_MAX_MESSAGE, false)) {
        return Invalid(mod, "damage log format is invalid");
    }
    return GameplayHudUnavailable(mod, "damagelog_set_format");
}

WotbModV3Result WOTBMOD_V3_CALL HudDamageLogSetFilterOwn(
    WotbModV3Handle mod,
    uint32_t enabled) {
    if (enabled > 1u) return Invalid(mod, "HUD boolean must be 0 or 1");
    const WotbModV3Result access = HudAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    /* The Blitz ribbon feed only ever shows the local player's own events,
     * so "own only" is already the case; showing everyone's is not a state
     * the client can enter. */
    if (enabled == 1u) return WOTBMOD_V3_OK;
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "damagelog_set_filter_own: the ribbon feed carries only the local player's events; there is no feed of other players' damage to show");
}
WotbModV3Result WOTBMOD_V3_CALL HudSessionStatsSetEnabled(
    WotbModV3Handle mod,
    uint32_t enabled) {
    if (enabled > 1u) return Invalid(mod, "HUD boolean must be 0 or 1");
    return HudSetVisible(
        mod, "session_stats", "session_stats_set_enabled", enabled);
}

WotbModV3Result WOTBMOD_V3_CALL HudSessionStatsSetFields(
    WotbModV3Handle mod,
    uint32_t fields) {
    const uint32_t known =
        WOTBMOD_V3_STAT_DAMAGE |
        WOTBMOD_V3_STAT_KILLS |
        WOTBMOD_V3_STAT_WN8 |
        WOTBMOD_V3_STAT_WINRATE |
        WOTBMOD_V3_STAT_SHOTS |
        WOTBMOD_V3_STAT_PENETRATION_RATE;
    if ((fields & ~known) != 0u) {
        return Invalid(mod, "session stats field mask is invalid");
    }
    /* The stock block shows damage dealt, assisted and blocked; only the
     * DAMAGE bit has a stock row behind it. Zero hides the whole block. */
    if ((fields & ~static_cast<uint32_t>(WOTBMOD_V3_STAT_DAMAGE)) != 0u) {
        const WotbModV3Result access = HudAccess(mod);
        if (access != WOTBMOD_V3_OK) return access;
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "session_stats_set_fields: the stock block has rows for damage only; kills, WN8, win rate, shots and penetration rate have no stock control");
    }
    WOTBMOD_HUD_RECORD(hudx::kFeatureStats,
        hudx::g_desired.statsFieldsSet = true;
        hudx::g_desired.statsFields = fields);
}

WotbModV3Result WOTBMOD_V3_CALL HudMinimapSetSize(
    WotbModV3Handle mod,
    float scale) {
    if (!IsFinite(scale) || scale < 0.25f || scale > 4.0f) {
        return Invalid(mod, "minimap scale is out of range");
    }
    return HudSetScale(mod, "minimap", "minimap_set_size", scale);
}

WotbModV3Result WOTBMOD_V3_CALL HudMinimapSetOpacity(
    WotbModV3Handle mod,
    float opacity) {
    if (!IsFinite(opacity) || opacity < 0.0f || opacity > 1.0f) {
        return Invalid(mod, "minimap opacity must be within [0, 1]");
    }
    /* Fades the background colour alpha of Minimap and every control under
     * it (the map image, markers, grid), keeping their own alpha ratio. */
    WOTBMOD_HUD_RECORD(hudx::kFeatureMinimapOpacity,
        hudx::g_desired.minimapOpacitySet = true;
        hudx::g_desired.minimapOpacity = opacity);
}

WotbModV3Result WOTBMOD_V3_CALL HudMinimapSetLastKnown(
    WotbModV3Handle mod,
    uint32_t enabled) {
    if (enabled > 1u) {
        return Invalid(mod, "minimap last-known flag must be 0 or 1");
    }
    return GameplayHudUnavailable(
        mod,
        "minimap_set_show_last_known",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        "gameplay.hud.last_known");
}

WOTBMOD_DEFINE_HUD_BOOL(
    HudMinimapSetArtilleryRange,
    "minimap_set_show_artillery_range")
WOTBMOD_DEFINE_HUD_BOOL(
    HudMinimapSetDrawing,
    "minimap_set_show_drawing")

WotbModV3Result WOTBMOD_V3_CALL HudMinimapAddMarker(
    WotbModV3Handle mod,
    float worldX,
    float worldZ,
    const char* label,
    uint32_t color,
    uint32_t* outMarkerId) {
    if (!IsFinite(worldX) || !IsFinite(worldZ) ||
        !ValidText(label, WOTBMOD_V3_MAX_NAME) || !outMarkerId) {
        return Invalid(mod, "minimap marker arguments are invalid");
    }
    (void)color;
    return GameplayHudUnavailable(
        mod,
        "minimap_add_marker",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        "gameplay.hud.markers");
}

WotbModV3Result WOTBMOD_V3_CALL HudMinimapRemoveMarker(
    WotbModV3Handle mod,
    uint32_t markerId) {
    if (markerId == 0u) {
        return Invalid(mod, "minimap marker ID is invalid");
    }
    return GameplayHudUnavailable(
        mod,
        "minimap_remove_marker",
        WOTBMOD_V3_PERMISSION_REVIEWED,
        "gameplay.hud.markers");
}

WotbModV3Result WOTBMOD_V3_CALL HudSixthSenseSetTexture(
    WotbModV3Handle mod,
    const char* uri) {
    std::string texture;
    if (uri && uri[0]) {
        const WotbModV3Result resolved = HudResolveTexture(mod, uri, &texture);
        if (resolved != WOTBMOD_V3_OK) return resolved;
    }
    WOTBMOD_HUD_RECORD(hudx::kFeatureOverlayLamp,
        hudx::g_desired.overlayTexture[2] = texture);
}

/* The sound plays through the mod's own audio object each time the lamp
 * lights up; the stock sound is not silenced (no hook on it yet). */
WotbModV3Result WOTBMOD_V3_CALL HudSixthSenseSetSound(
    WotbModV3Handle mod,
    const char* uri) {
    if (!ValidText(uri, WOTBMOD_V3_MAX_PATH)) {
        return Invalid(mod, "HUD resource URI is invalid");
    }
    const WotbModV3Result access = HudAccess(mod);
    if (access != WOTBMOD_V3_OK) return access;
    WotbModV3AudioHandle audio = WOTBMOD_V3_INVALID_HANDLE;
    if (uri[0]) {
        WotbModV3AudioDescriptor descriptor = {};
        WOTBMOD_V3_INIT_STRUCT(descriptor, WOTBMOD_V3_AUDIO_VERSION);
        descriptor.uri = uri;
        descriptor.volume = 1.0f;
        descriptor.pitch = 1.0f;
        descriptor.priority = 50u;
        descriptor.bus = "master";
        const WotbModV3Result created = AudioCreateCore(mod, &descriptor, false, &audio);
        if (created != WOTBMOD_V3_OK) return created;
    }
    WotbModV3AudioHandle previous = WOTBMOD_V3_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_hudMutex);
        previous = hudx::g_desired.sixthAudio;
        hudx::g_desired.owner = mod;
        hudx::g_desired.sixthAudio = audio;
        hudx::g_desired.sixthSoundUri = uri;
    }
    if (previous != WOTBMOD_V3_INVALID_HANDLE) {
        AudioStop(mod, previous, 0.0f);
        AudioDestroy(mod, previous);
    }
    return hudx::Commit(mod, hudx::kFeatureSixthSense);
}

WotbModV3Result WOTBMOD_V3_CALL HudSixthSenseSetPosition(
    WotbModV3Handle mod,
    WotbModV3Vec2 position) {
    if (!IsFinite(position)) {
        return Invalid(mod, "sixth sense position is invalid");
    }
    return HudSetPosition(mod, "sixth_sense", "sixth_sense_set_position", position);
}

WotbModV3Result WOTBMOD_V3_CALL HudSixthSenseSetScale(
    WotbModV3Handle mod,
    float scale) {
    if (!IsFinite(scale) || scale < 0.1f || scale > 8.0f) {
        return Invalid(mod, "sixth sense scale is out of range");
    }
    return HudSetScale(mod, "sixth_sense", "sixth_sense_set_scale", scale);
}

WotbModV3Result WOTBMOD_V3_CALL HudSixthSenseSetDelay(
    WotbModV3Handle mod,
    float milliseconds) {
    if (!IsFinite(milliseconds) || milliseconds < 0.0f ||
        milliseconds > 10000.0f) {
        return Invalid(mod, "sixth sense visual delay is out of range");
    }
    /* Minimum time the lamp stays lit: when the game hides it earlier, the
     * lamp is kept visible until the delay has passed since it lit up. */
    WOTBMOD_HUD_RECORD(hudx::kFeatureSixthSense,
        hudx::g_desired.sixthDelayMs = milliseconds);
}

WotbModV3Result WOTBMOD_V3_CALL HudHitIndicatorSetStyle(
    WotbModV3Handle mod,
    uint32_t style) {
    if (style > WOTBMOD_V3_HIT_STYLE_MINIMAL) {
        return Invalid(mod, "hit indicator style is invalid");
    }
    if (style == WOTBMOD_V3_HIT_STYLE_DIRECTIONAL) {
        const WotbModV3Result access = HudAccess(mod);
        if (access != WOTBMOD_V3_OK) return access;
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "hit_indicator_set_style: the stock flash has no direction; DIRECTIONAL needs an indicator the client does not draw");
    }
    /* GAME_DEFAULT restores the flashes, COMPACT shrinks HitHighlight and
     * RecochetHighlight to the central 40% of the screen, MINIMAL keeps them
     * hidden (re-asserted each frame, the game shows them on every hit). */
    WOTBMOD_HUD_RECORD(hudx::kFeatureHit,
        hudx::g_desired.hitStyle = style);
}

WotbModV3Color HudUnpackColor(uint32_t rgba) {
    return {
        static_cast<float>((rgba >> 24) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((rgba >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(rgba & 0xFFu) / 255.0f};
}

/* HitHighlight is the flash for a hit taken, RecochetHighlight the one for
 * a ricochet; penetration and critical hits share the first flash and have
 * no control of their own, so those two colours stay unsupported. */
WotbModV3Result WOTBMOD_V3_CALL HudHitIndicatorSetHitColor(
    WotbModV3Handle mod,
    uint32_t rgba) {
    const WotbModV3Color color = HudUnpackColor(rgba);
    WOTBMOD_HUD_RECORD(hudx::kFeatureHit,
        hudx::g_desired.hitColorSet = true;
        hudx::g_desired.hitColor = color);
}
WOTBMOD_DEFINE_HUD_COLOR(
    HudHitIndicatorSetPenColor,
    "hit_indicator_set_color_pen")
WotbModV3Result WOTBMOD_V3_CALL HudHitIndicatorSetRicochetColor(
    WotbModV3Handle mod,
    uint32_t rgba) {
    const WotbModV3Color color = HudUnpackColor(rgba);
    WOTBMOD_HUD_RECORD(hudx::kFeatureHit,
        hudx::g_desired.ricochetColorSet = true;
        hudx::g_desired.ricochetColor = color);
}
WOTBMOD_DEFINE_HUD_COLOR(
    HudHitIndicatorSetCritColor,
    "hit_indicator_set_color_crit")

WotbModV3Result WOTBMOD_V3_CALL HudReset(
    WotbModV3Handle mod) {
    return HudResetStock(mod);
}

#undef WOTBMOD_DEFINE_HUD_COLOR
#undef WOTBMOD_DEFINE_HUD_BOOL
#undef WOTBMOD_DEFINE_HUD_PATH

WotbModV3Result GameplayHangarUnavailable(
    WotbModV3Handle mod,
    const char* operation) {
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        WOTBMOD_V3_CONTEXT_HANGAR,
        "gameplay.tweak.hangar",
        operation);
}

#define WOTBMOD_DEFINE_HANGAR_PATH(functionName, operationName)          \
    WotbModV3Result WOTBMOD_V3_CALL functionName(                       \
        WotbModV3Handle mod,                                             \
        const char* uri) {                                               \
        if (!ValidText(uri, WOTBMOD_V3_MAX_PATH)) {                      \
            return Invalid(mod, "hangar resource URI is invalid");       \
        }                                                                \
        return GameplayHangarUnavailable(mod, operationName);            \
    }

WOTBMOD_DEFINE_HANGAR_PATH(HangarSetBackground, "hangar_set_background")
WOTBMOD_DEFINE_HANGAR_PATH(
    HangarSetBackgroundVideo,
    "hangar_set_background_video")
WOTBMOD_DEFINE_HANGAR_PATH(HangarSetMusic, "hangar_set_music")
WOTBMOD_DEFINE_HANGAR_PATH(
    HangarSetFloorTexture,
    "hangar_set_floor_texture")
WOTBMOD_DEFINE_HANGAR_PATH(HangarSetSkybox, "hangar_set_skybox")

WotbModV3Result WOTBMOD_V3_CALL HangarSetLighting(
    WotbModV3Handle mod,
    uint32_t preset) {
    if (preset > WOTBMOD_V3_HANGAR_LIGHTING_CINEMATIC) {
        return Invalid(mod, "hangar lighting preset is invalid");
    }
    return GameplayHangarUnavailable(mod, "hangar_set_lighting");
}

WotbModV3Result WOTBMOD_V3_CALL HangarSetPreviewAngle(
    WotbModV3Handle mod,
    float yaw,
    float pitch) {
    if (!IsFinite(yaw) || !IsFinite(pitch) || pitch < -89.0f ||
        pitch > 89.0f) {
        return Invalid(mod, "hangar preview angle is invalid");
    }
    return GameplayHangarUnavailable(
        mod,
        "hangar_set_vehicle_preview_angle");
}

WotbModV3Result WOTBMOD_V3_CALL HangarSetPreviewZoom(
    WotbModV3Handle mod,
    float zoom) {
    if (!IsFinite(zoom) || zoom < 0.1f || zoom > 10.0f) {
        return Invalid(mod, "hangar preview zoom is out of range");
    }
    return GameplayHangarUnavailable(
        mod,
        "hangar_set_vehicle_preview_zoom");
}

WotbModV3Result WOTBMOD_V3_CALL HangarHideUiElements(
    WotbModV3Handle mod,
    uint32_t mask) {
    const uint32_t known =
        WOTBMOD_V3_HANGAR_UI_BANNERS |
        WOTBMOD_V3_HANGAR_UI_NEWS |
        WOTBMOD_V3_HANGAR_UI_OFFERS |
        WOTBMOD_V3_HANGAR_UI_CHAT;
    if ((mask & ~known) != 0u) {
        return Invalid(mod, "hangar UI mask is invalid");
    }
    return GameplayHangarUnavailable(
        mod,
        "hangar_hide_ui_elements");
}

WotbModV3Result WOTBMOD_V3_CALL HangarSetOrbitSpeed(
    WotbModV3Handle mod,
    float multiplier) {
    if (!IsFinite(multiplier) || multiplier < 0.0f ||
        multiplier > 10.0f) {
        return Invalid(mod, "hangar orbit speed is out of range");
    }
    return GameplayHangarUnavailable(
        mod,
        "hangar_set_camera_orbit_speed");
}

WotbModV3Result WOTBMOD_V3_CALL HangarReset(
    WotbModV3Handle mod) {
    return GameplayHangarUnavailable(mod, "hangar_reset");
}

#undef WOTBMOD_DEFINE_HANGAR_PATH

WotbModV3Result GameplayReplayUnavailable(
    WotbModV3Handle mod,
    const char* operation) {
    return NativeUnavailable(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        WOTBMOD_V3_CONTEXT_REPLAY,
        "gameplay.tweak.replay",
        operation);
}

WotbModV3Result WOTBMOD_V3_CALL ReplaySetSpeed(
    WotbModV3Handle mod,
    float multiplier) {
    if (!IsFinite(multiplier) || multiplier < 0.1f ||
        multiplier > 16.0f) {
        return Invalid(mod, "replay speed must be within [0.1, 16]");
    }
    return GameplayReplayUnavailable(mod, "replay_set_speed");
}

WotbModV3Result WOTBMOD_V3_CALL ReplaySeek(
    WotbModV3Handle mod,
    float timestamp) {
    if (!IsFinite(timestamp) || timestamp < 0.0f) {
        return Invalid(mod, "replay seek timestamp is invalid");
    }
    return GameplayReplayUnavailable(mod, "replay_seek");
}

WotbModV3Result WOTBMOD_V3_CALL ReplayGetDuration(
    WotbModV3Handle mod,
    float* outSeconds) {
    if (!outSeconds) {
        return Invalid(mod, "replay duration output pointer is null");
    }
    return GameplayReplayUnavailable(mod, "replay_get_duration");
}

WotbModV3Result WOTBMOD_V3_CALL ReplayGetPosition(
    WotbModV3Handle mod,
    float* outSeconds) {
    if (!outSeconds) {
        return Invalid(mod, "replay position output pointer is null");
    }
    return GameplayReplayUnavailable(mod, "replay_get_position");
}

WotbModV3Result WOTBMOD_V3_CALL ReplayAddMarker(
    WotbModV3Handle mod,
    float timestamp,
    const char* label,
    uint32_t* outMarkerId) {
    if (!IsFinite(timestamp) || timestamp < 0.0f ||
        !ValidText(label, WOTBMOD_V3_MAX_NAME, false) ||
        !outMarkerId) {
        return Invalid(mod, "replay marker arguments are invalid");
    }
    return GameplayReplayUnavailable(mod, "replay_add_marker");
}

WotbModV3Result WOTBMOD_V3_CALL ReplayRemoveMarker(
    WotbModV3Handle mod,
    uint32_t markerId) {
    if (markerId == 0u) {
        return Invalid(mod, "replay marker ID is invalid");
    }
    return GameplayReplayUnavailable(mod, "replay_remove_marker");
}

WotbModV3Result WOTBMOD_V3_CALL ReplaySetCameraMode(
    WotbModV3Handle mod,
    uint32_t mode) {
    if (mode > WOTBMOD_V3_REPLAY_CAMERA_FIRST_PERSON) {
        return Invalid(mod, "replay camera mode is invalid");
    }
    return GameplayReplayUnavailable(mod, "replay_set_camera_mode");
}

WotbModV3Result WOTBMOD_V3_CALL ReplaySetFollowVehicle(
    WotbModV3Handle mod,
    uint32_t publicEntityId) {
    if (publicEntityId == 0u) {
        return Invalid(mod, "replay follow entity ID is invalid");
    }
    return GameplayReplayUnavailable(
        mod,
        "replay_set_follow_vehicle");
}

WotbModV3Result WOTBMOD_V3_CALL ReplayExportClip(
    WotbModV3Handle mod,
    float start,
    float end,
    const char* outputUri) {
    if (!IsFinite(start) || !IsFinite(end) || start < 0.0f ||
        end <= start ||
        !ValidText(outputUri, WOTBMOD_V3_MAX_PATH, false)) {
        return Invalid(mod, "replay export arguments are invalid");
    }
    return GameplayReplayUnavailable(mod, "replay_export_clip");
}

struct VehicleVisualProfileObject {
    uint32_t magic = kVehicleProfileMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle self = WOTBMOD_V3_INVALID_HANDLE;
    std::string id;
    std::string vehicleName;
    std::string hullMesh;
    std::string turretMesh;
    std::string gunMesh;
    std::string chassisMesh;
    std::string materialOverlay;
    std::string texturePack;
    int32_t priority = 0;
    uint32_t hangarOnly = 0;
    struct SkinAsset {
        uint32_t kind = 0u;
        uint32_t part = 0u;
        int32_t lod = -1;
        std::string stockGameUri;
        std::string replacementUri;
    };
    std::mutex operationMutex;
    std::vector<SkinAsset> assets;
    std::vector<WotbModV3Handle> mounts;
    WotbModV3EntityHandle appliedVehicle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t applyGeneration = 0u;
    bool exactPathPack = false;
};

void DestroyVehicleProfile(void* object) {
    VehicleVisualProfileObject* profile =
        static_cast<VehicleVisualProfileObject*>(object);
    if (profile) {
        std::vector<WotbModV3Handle> mounts;
        {
            std::lock_guard<std::mutex> lock(profile->operationMutex);
            mounts.swap(profile->mounts);
        }
        for (auto it = mounts.rbegin(); it != mounts.rend(); ++it) {
            ReleaseOwnedHandle(profile->owner, *it);
        }
        profile->magic = 0;
        delete profile;
    }
}

WotbModV3Result CheckVehicleLocalCosmeticPermission(
    WotbModV3Handle mod) {
    return CheckEitherNamedPermission(
        mod,
        "vehicle.local.cosmetic",
        WOTBMOD_V3_PERMISSION_SAFE,
        "gameplay.tweak.vehicle",
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK);
}

WotbModV3Result GetVehicleProfile(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    VehicleVisualProfileObject** outProfile) {
    const WotbModV3Result permission =
        CheckVehicleLocalCosmeticPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outProfile) {
        return Invalid(mod, "vehicle profile output pointer is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_RESOURCE,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    VehicleVisualProfileObject* profile =
        static_cast<VehicleVisualProfileObject*>(object);
    if (!profile || profile->magic != kVehicleProfileMagic) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not a vehicle visual profile");
    }
    *outProfile = profile;
    return WOTBMOD_V3_OK;
}

WotbModV3Result VehicleVisualUnavailable(
    WotbModV3Handle mod,
    const char* operation) {
    std::string message(operation ? operation : "vehicle visual");
    message += ": native client backend is not installed";
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        message.c_str(),
        "{\"backend\":\"missing\"}");
}

WotbModV3Result VehiclePublicReadUnavailable(
    WotbModV3Handle mod,
    const char* operation) {
    std::string message(operation ? operation : "vehicle read");
    message += ": value is not represented by the public vehicle registry";
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        message.c_str());
}

constexpr uint64_t kVehiclePublicReadContexts =
    WOTBMOD_V3_CONTEXT_HANGAR |
    WOTBMOD_V3_CONTEXT_BATTLE |
    WOTBMOD_V3_CONTEXT_REPLAY |
    WOTBMOD_V3_CONTEXT_TRAINING;

bool IsSafePublicVehicleSnapshot(
    const WotbModV3PublicEntitySnapshot& snapshot) {
    return snapshot.type == WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE &&
           (snapshot.local_player != 0u ||
            snapshot.visible_to_player != 0u);
}

WotbModV3Result GetRegisteredPublicVehicle(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    HostPublicEntityRecord* outRecord) {
    if (!outRecord) {
        return Invalid(mod, "public vehicle output pointer is null");
    }
    if (vehicle == WOTBMOD_V3_INVALID_HANDLE) {
        return Invalid(mod, "entity handle is invalid");
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto found = g_hostEntities.find(vehicle);
    if (found == g_hostEntities.end() ||
        !IsSafePublicVehicleSnapshot(found->second.snapshot)) {
        return NotFound(
            mod,
            "vehicle is not local or currently visible to the player");
    }
    *outRecord = found->second;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetSafePublicVehicle(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    HostPublicEntityRecord* outRecord) {
    if (vehicle == WOTBMOD_V3_INVALID_HANDLE) {
        const WotbModV3Result permission =
            CheckVehicleLocalCosmeticPermission(mod);
        return permission == WOTBMOD_V3_OK
                   ? Invalid(mod, "entity handle is invalid")
                   : permission;
    }
    HostPublicEntityRecord record = {};
    WotbModV3Result result =
        GetRegisteredPublicVehicle(mod, vehicle, &record);
    if (result != WOTBMOD_V3_OK) {
        const WotbModV3Result permission = CheckNamedPermission(
            mod,
            "game.entity.public",
            WOTBMOD_V3_PERMISSION_REVIEWED);
        return permission == WOTBMOD_V3_OK ? result : permission;
    }
    const bool local = record.snapshot.local_player != 0u;
    const WotbModV3Result permission =
        local
            ? CheckVehicleLocalCosmeticPermission(mod)
            : CheckNamedPermission(
                  mod,
                  "game.entity.public",
                  WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    result = CheckAccess(
        mod,
        local
            ? WOTBMOD_V3_PERMISSION_SAFE
            : WOTBMOD_V3_PERMISSION_REVIEWED,
        kVehiclePublicReadContexts,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    *outRecord = record;
    return WOTBMOD_V3_OK;
}

WotbModV3Result CheckVehicleMutationAccess(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    HostPublicEntityRecord* outRecord = nullptr) {
    const WotbModV3Result permission =
        CheckVehicleLocalCosmeticPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    HostPublicEntityRecord record = {};
    WotbModV3Result result =
        GetRegisteredPublicVehicle(mod, vehicle, &record);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kVehiclePublicReadContexts,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (record.snapshot.local_player == 0u &&
        CurrentContext() != WOTBMOD_V3_CONTEXT_HANGAR) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "vehicle visual mutation is limited to the local player or hangar vehicle");
    }
    if (outRecord) {
        *outRecord = record;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetLocal(
    WotbModV3Handle mod,
    WotbModV3EntityHandle* outVehicle) {
    const WotbModV3Result permission =
        CheckVehicleLocalCosmeticPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outVehicle) {
        return Invalid(mod, "local vehicle output pointer is null");
    }
    *outVehicle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kVehiclePublicReadContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    bool multipleLocalVehicles = false;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        for (const auto& pair : g_hostEntities) {
            const WotbModV3PublicEntitySnapshot& snapshot =
                pair.second.snapshot;
            if (!IsSafePublicVehicleSnapshot(snapshot) ||
                snapshot.local_player == 0u) {
                continue;
            }
            if (*outVehicle != WOTBMOD_V3_INVALID_HANDLE) {
                *outVehicle = WOTBMOD_V3_INVALID_HANDLE;
                multipleLocalVehicles = true;
                break;
            }
            *outVehicle = pair.first;
        }
    }
    if (multipleLocalVehicles) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "multiple local vehicles are registered");
    }
    return *outVehicle != WOTBMOD_V3_INVALID_HANDLE
               ? WOTBMOD_V3_OK
               : NotFound(mod, "local player vehicle is not registered");
}

WotbModV3Result GetSafePublicVehicleEnemy(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    HostPublicEntityRecord* outRecord,
    uint32_t* outEnemy) {
    if (!outRecord || !outEnemy) {
        return Invalid(mod, "vehicle enemy output pointer is null");
    }
    const WotbModV3Result access =
        GetSafePublicVehicle(mod, vehicle, outRecord);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (outRecord->snapshot.local_player != 0u) {
        *outEnemy = 0u;
        return WOTBMOD_V3_OK;
    }
    bool multipleLocalVehicles = false;
    bool teamUnavailable = false;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const HostPublicEntityRecord* local = nullptr;
        for (const auto& pair : g_hostEntities) {
            if (IsSafePublicVehicleSnapshot(
                    pair.second.snapshot) &&
                pair.second.snapshot.local_player != 0u) {
                if (local) {
                    multipleLocalVehicles = true;
                    break;
                }
                local = &pair.second;
            }
        }
        if (!multipleLocalVehicles) {
            if (!local ||
                (local->validFields &
                 CLIENT_HOST_PUBLIC_ENTITY_FIELD_TEAM) == 0u ||
                (outRecord->validFields &
                 CLIENT_HOST_PUBLIC_ENTITY_FIELD_TEAM) == 0u) {
                teamUnavailable = true;
            } else {
                *outEnemy =
                    outRecord->snapshot.team != local->snapshot.team
                        ? 1u
                        : 0u;
            }
        }
    }
    if (multipleLocalVehicles) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "multiple local vehicles are registered");
    }
    if (teamUnavailable) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "public team relationship is unavailable");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleIsLocal(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t* outValue) {
    if (!outValue) {
        return Invalid(mod, "vehicle local flag output pointer is null");
    }
    HostPublicEntityRecord record = {};
    const WotbModV3Result result =
        GetSafePublicVehicle(mod, vehicle, &record);
    if (result == WOTBMOD_V3_OK) {
        *outValue = record.snapshot.local_player != 0u ? 1u : 0u;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleIsHangar(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t* outValue) {
    if (!outValue) {
        return Invalid(mod, "vehicle hangar flag output pointer is null");
    }
    HostPublicEntityRecord record = {};
    const WotbModV3Result result =
        GetSafePublicVehicle(mod, vehicle, &record);
    if (result == WOTBMOD_V3_OK) {
        *outValue =
            CurrentContext() == WOTBMOD_V3_CONTEXT_HANGAR ? 1u : 0u;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetAppearanceState(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t* outState) {
    if (!outState) {
        return Invalid(
            mod,
            "vehicle appearance state output pointer is null");
    }
    HostPublicEntityRecord record = {};
    const WotbModV3Result result =
        GetSafePublicVehicle(mod, vehicle, &record);
    return result == WOTBMOD_V3_OK
               ? VehiclePublicReadUnavailable(
                     mod,
                     "vehicle_get_appearance_state")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetEnemy(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t* outEnemy) {
    if (!outEnemy) {
        return Invalid(mod, "vehicle enemy flag output pointer is null");
    }
    HostPublicEntityRecord record = {};
    return GetSafePublicVehicleEnemy(
        mod,
        vehicle,
        &record,
        outEnemy);
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetEnemyName(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    char* buffer,
    uint32_t* inoutSize) {
    if (!inoutSize) {
        return Invalid(mod, "enemy name size pointer is null");
    }
    HostPublicEntityRecord record = {};
    uint32_t enemy = 0u;
    const WotbModV3Result result = GetSafePublicVehicleEnemy(
        mod,
        vehicle,
        &record,
        &enemy);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (enemy == 0u) {
        return NotFound(
            mod,
            "vehicle is not an enemy");
    }
    if ((record.validFields &
         CLIENT_HOST_PUBLIC_ENTITY_FIELD_DISPLAY_NAME) == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "visible enemy display name is unavailable from the native source");
    }
    if (record.snapshot.display_name[0] == '\0') {
        return NotFound(mod, "visible enemy display name is empty");
    }
    return CopyString(
        mod,
        std::string(record.snapshot.display_name),
        buffer,
        inoutSize);
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetPosition(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    WotbModV3Vec3* outPosition) {
    if (!outPosition) {
        return Invalid(mod, "vehicle position output pointer is null");
    }
    HostPublicEntityRecord record = {};
    const WotbModV3Result result =
        GetSafePublicVehicle(mod, vehicle, &record);
    if (result == WOTBMOD_V3_OK) {
        if ((record.validFields &
             CLIENT_HOST_PUBLIC_ENTITY_FIELD_POSITION) == 0u) {
            return SetError(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                "vehicle position is unavailable from the native source");
        }
        *outPosition = record.snapshot.position;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetPartEntity(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t part,
    WotbModV3SceneHandle* outEntity) {
    if (part < WOTBMOD_V3_VEHICLE_PART_HULL ||
        part > WOTBMOD_V3_VEHICLE_PART_EFFECTS || !outEntity) {
        return Invalid(mod, "vehicle part query is invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_get_part_entity")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleFindAttachmentPoint(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* pointName,
    WotbModV3Transform* outTransform) {
    if (!ValidText(pointName, WOTBMOD_V3_MAX_PATH, false) ||
        !outTransform) {
        return Invalid(mod, "vehicle attachment point query is invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_find_attachment_point")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleAttachEntityEx(
    WotbModV3Handle mod,
    const WotbModV3VehicleAttachment* attachment) {
    if (!ValidStruct(
            attachment,
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION) ||
        attachment->vehicle == WOTBMOD_V3_INVALID_HANDLE ||
        attachment->entity == WOTBMOD_V3_INVALID_HANDLE ||
        !ValidText(
            attachment->attachment_point,
            WOTBMOD_V3_MAX_PATH,
            false) ||
        !ValidSceneTransform(&attachment->local_transform)) {
        return Invalid(mod, "vehicle attachment is invalid");
    }
    WotbModV3Result result =
        CheckVehicleMutationAccess(mod, attachment->vehicle);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    SceneEntity* entity = nullptr;
    result = GetSceneEntity(mod, attachment->entity, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckSceneMutationAccess(mod, entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return VehicleVisualUnavailable(mod, "vehicle_attach_entity_ex");
}

WotbModV3Result VehiclePathEffect(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* path,
    const char* operation) {
    if (!ValidText(path, WOTBMOD_V3_MAX_PATH, false)) {
        return Invalid(mod, "vehicle resource path is invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(mod, operation)
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSetDecal(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* slot,
    const char* textureUri) {
    if (!ValidText(slot, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "vehicle decal slot is invalid");
    }
    return VehiclePathEffect(
        mod,
        vehicle,
        textureUri,
        "vehicle_set_decal_override");
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSetTexture(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* materialPath,
    const char* textureSlot,
    const char* textureUri) {
    if (!ValidText(materialPath, WOTBMOD_V3_MAX_PATH) ||
        !ValidText(textureSlot, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "vehicle texture override target is invalid");
    }
    return VehiclePathEffect(
        mod,
        vehicle,
        textureUri,
        "vehicle_set_texture_override");
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSetColor(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* materialPath,
    WotbModV3Color color) {
    if (!ValidText(materialPath, WOTBMOD_V3_MAX_PATH) ||
        !IsFinite(color)) {
        return Invalid(mod, "vehicle color override is invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_set_color_override")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleGetAnimation(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t index,
    char* buffer,
    uint32_t* inoutSize) {
    if (!inoutSize) {
        return Invalid(mod, "vehicle animation size pointer is null");
    }
    (void)index;
    (void)buffer;
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_get_available_animation")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehiclePlayAnimation(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* animation,
    float blendSeconds) {
    if (!ValidText(animation, WOTBMOD_V3_MAX_NAME, false) ||
        !IsFinite(blendSeconds) || blendSeconds < 0.0f ||
        blendSeconds > 60.0f) {
        return Invalid(mod, "vehicle animation arguments are invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_play_animation")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleRestoreAppearance(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle) {
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_restore_appearance")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSetPartVisible(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t part,
    uint32_t visible) {
    if (part < WOTBMOD_V3_VEHICLE_PART_HULL ||
        part > WOTBMOD_V3_VEHICLE_PART_EFFECTS || visible > 1u) {
        return Invalid(mod, "vehicle part visibility arguments are invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_set_part_visible")
               : result;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSetMaterial(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* materialPath,
    const char* replacementUri) {
    if (!ValidText(materialPath, WOTBMOD_V3_MAX_PATH)) {
        return Invalid(mod, "vehicle material path is invalid");
    }
    return VehiclePathEffect(
        mod,
        vehicle,
        replacementUri,
        "vehicle_set_material_override");
}

#define WOTBMOD_DEFINE_VEHICLE_PATH(functionName, operationName)         \
    WotbModV3Result WOTBMOD_V3_CALL functionName(                       \
        WotbModV3Handle mod,                                             \
        WotbModV3EntityHandle vehicle,                                   \
        const char* uri) {                                               \
        return VehiclePathEffect(mod, vehicle, uri, operationName);      \
    }

WOTBMOD_DEFINE_VEHICLE_PATH(
    VehicleApplySkin,
    "vehicle_skin_apply_to_entity")
WOTBMOD_DEFINE_VEHICLE_PATH(
    VehicleSetCustomSkin,
    "vehicle_set_custom_skin")
WOTBMOD_DEFINE_VEHICLE_PATH(
    VehicleSetCustomCamouflage,
    "vehicle_set_custom_camouflage")
WOTBMOD_DEFINE_VEHICLE_PATH(
    VehicleSetEngineSound,
    "vehicle_set_engine_sound")
WOTBMOD_DEFINE_VEHICLE_PATH(
    VehicleSetGunSound,
    "vehicle_set_gun_sound")

#undef WOTBMOD_DEFINE_VEHICLE_PATH

WotbModV3Result WOTBMOD_V3_CALL VehicleSetEmblem(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t slot,
    const char* uri) {
    if (slot > 31u) {
        return Invalid(mod, "vehicle emblem slot is out of range");
    }
    return VehiclePathEffect(
        mod,
        vehicle,
        uri,
        "vehicle_set_emblem");
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSetInscription(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    uint32_t slot,
    const char* text,
    const char* fontUri) {
    if (slot > 31u ||
        !ValidText(text, WOTBMOD_V3_MAX_MESSAGE) ||
        !ValidText(fontUri, WOTBMOD_V3_MAX_PATH, false)) {
        return Invalid(mod, "vehicle inscription arguments are invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    return result == WOTBMOD_V3_OK
               ? VehicleVisualUnavailable(
                     mod,
                     "vehicle_set_inscription")
               : result;
}

WotbModV3Result VehicleHangarAnimation(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* animation,
    const char* operation) {
    if (!ValidText(animation, WOTBMOD_V3_MAX_NAME, false)) {
        return Invalid(mod, "vehicle hangar animation name is invalid");
    }
    const WotbModV3Result result =
        CheckVehicleMutationAccess(mod, vehicle);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (CurrentContext() != WOTBMOD_V3_CONTEXT_HANGAR) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INCOMPATIBLE,
            "hangar vehicle animation is unavailable outside the hangar");
    }
    return VehicleVisualUnavailable(mod, operation);
}

WotbModV3Result WOTBMOD_V3_CALL VehiclePlayHangarAnimation(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* animation) {
    return VehicleHangarAnimation(
        mod,
        vehicle,
        animation,
        "vehicle_play_hangar_animation");
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSetHangarIdleAnimation(
    WotbModV3Handle mod,
    WotbModV3EntityHandle vehicle,
    const char* animation) {
    return VehicleHangarAnimation(
        mod,
        vehicle,
        animation,
        "vehicle_set_hangar_idle_animation");
}

bool ValidOptionalPath(const char* value) {
    return !value || ValidText(value, WOTBMOD_V3_MAX_PATH);
}

WotbModV3Result WOTBMOD_V3_CALL VehicleProfileRegister(
    WotbModV3Handle mod,
    const WotbModV3VehicleVisualProfile* descriptor,
    WotbModV3Handle* outProfile) {
    const WotbModV3Result permission =
        CheckVehicleLocalCosmeticPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(
            descriptor,
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION) ||
        !outProfile ||
        !ValidText(descriptor->id, WOTBMOD_V3_MAX_ID, false) ||
        !ValidText(
            descriptor->vehicle_name,
            WOTBMOD_V3_MAX_NAME,
            false) ||
        !ValidOptionalPath(descriptor->hull_mesh_uri) ||
        !ValidOptionalPath(descriptor->turret_mesh_uri) ||
        !ValidOptionalPath(descriptor->gun_mesh_uri) ||
        !ValidOptionalPath(descriptor->chassis_mesh_uri) ||
        !ValidOptionalPath(descriptor->material_overlay_uri) ||
        !ValidOptionalPath(descriptor->texture_pack_uri) ||
        descriptor->hangar_only > 1u) {
        return Invalid(mod, "invalid vehicle visual profile");
    }
    const bool hasReplacement =
        (descriptor->hull_mesh_uri && *descriptor->hull_mesh_uri) ||
        (descriptor->turret_mesh_uri && *descriptor->turret_mesh_uri) ||
        (descriptor->gun_mesh_uri && *descriptor->gun_mesh_uri) ||
        (descriptor->chassis_mesh_uri &&
         *descriptor->chassis_mesh_uri) ||
        (descriptor->material_overlay_uri &&
         *descriptor->material_overlay_uri) ||
        (descriptor->texture_pack_uri &&
         *descriptor->texture_pack_uri);
    if (!hasReplacement) {
        return Invalid(
            mod,
            "vehicle visual profile contains no replacement resource");
    }
    VehicleVisualProfileObject* profile =
        new (std::nothrow) VehicleVisualProfileObject();
    if (!profile) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "vehicle profile allocation failed");
    }
    profile->owner = mod;
    profile->id = descriptor->id;
    profile->vehicleName = descriptor->vehicle_name;
    profile->hullMesh =
        descriptor->hull_mesh_uri ? descriptor->hull_mesh_uri : "";
    profile->turretMesh =
        descriptor->turret_mesh_uri ? descriptor->turret_mesh_uri : "";
    profile->gunMesh =
        descriptor->gun_mesh_uri ? descriptor->gun_mesh_uri : "";
    profile->chassisMesh =
        descriptor->chassis_mesh_uri
            ? descriptor->chassis_mesh_uri
            : "";
    profile->materialOverlay =
        descriptor->material_overlay_uri
            ? descriptor->material_overlay_uri
            : "";
    profile->texturePack =
        descriptor->texture_pack_uri
            ? descriptor->texture_pack_uri
            : "";
    profile->priority = descriptor->priority;
    profile->hangarOnly = descriptor->hangar_only;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        profile,
        &DestroyVehicleProfile,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        delete profile;
        return result;
    }
    profile->self = handle;
    *outProfile = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleProfileApply(
    WotbModV3Handle mod,
    WotbModV3Handle profileHandle,
    WotbModV3EntityHandle vehicle) {
    VehicleVisualProfileObject* profile = nullptr;
    WotbModV3Result result =
        GetVehicleProfile(mod, profileHandle, &profile);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    result = CheckVehicleMutationAccess(mod, vehicle);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (profile->hangarOnly != 0u &&
        CurrentContext() != WOTBMOD_V3_CONTEXT_HANGAR) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INCOMPATIBLE,
            "hangar-only vehicle profile cannot be applied in this context");
    }
    return VehicleVisualUnavailable(
        mod,
        "vehicle_visual_profile_apply");
}

WotbModV3Result WOTBMOD_V3_CALL VehicleProfileRelease(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    VehicleVisualProfileObject* profile = nullptr;
    const WotbModV3Result result =
        GetVehicleProfile(mod, handle, &profile);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result CheckVehicleSkinPackPermissions(
    WotbModV3Handle mod) {
    WotbModV3Result result = CheckVehicleLocalCosmeticPermission(mod);
    if (result != WOTBMOD_V3_OK) return result;
    result = CheckNamedPermission(
        mod,
        "resources.mod",
        WOTBMOD_V3_PERMISSION_SAFE);
    if (result != WOTBMOD_V3_OK) return result;
    return CheckNamedPermission(
        mod,
        "resources.overlay.game",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSkinPackRegister(
    WotbModV3Handle mod,
    const WotbModV3VehicleSkinPack* descriptor,
    WotbModV3Handle* outPack) {
    const WotbModV3Result permission =
        CheckVehicleSkinPackPermissions(mod);
    if (permission != WOTBMOD_V3_OK) return permission;
    if (!descriptor || descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->api_version !=
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2 ||
        !outPack ||
        !ValidText(descriptor->id, WOTBMOD_V3_MAX_ID, false) ||
        !ValidText(
            descriptor->vehicle_name,
            WOTBMOD_V3_MAX_NAME,
            false) ||
        !descriptor->assets || descriptor->asset_count == 0u ||
        descriptor->asset_count > WOTBMOD_V3_VEHICLE_SKIN_MAX_ASSETS ||
        descriptor->priority < -100000 ||
        descriptor->priority > 100000 ||
        descriptor->hangar_only > 1u) {
        return Invalid(mod, "invalid vehicle skin pack");
    }
    std::vector<VehicleVisualProfileObject::SkinAsset> assets;
    try {
        assets.reserve(descriptor->asset_count);
    } catch (...) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "vehicle skin asset allocation failed");
    }
    for (uint32_t index = 0u;
         index < descriptor->asset_count;
         ++index) {
        const WotbModV3VehicleSkinAsset& source =
            descriptor->assets[index];
        if (source.struct_size < sizeof(source) ||
            source.api_version !=
                WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2 ||
            source.kind < WOTBMOD_V3_VEHICLE_SKIN_MESH ||
            source.kind > WOTBMOD_V3_VEHICLE_SKIN_TEXTURE ||
            source.part > WOTBMOD_V3_VEHICLE_PART_EFFECTS ||
            source.lod < -1 || source.lod > 15 ||
            !ValidText(
                source.stock_game_uri,
                WOTBMOD_V3_MAX_PATH,
                false) ||
            !ValidText(
                source.replacement_uri,
                WOTBMOD_V3_MAX_PATH,
                false) ||
            std::strncmp(source.stock_game_uri, "game://", 7u) != 0 ||
            (std::strncmp(source.replacement_uri, "mod://", 6u) != 0 &&
             std::strncmp(source.replacement_uri, "data://", 7u) != 0 &&
             std::strncmp(source.replacement_uri, "cache://", 8u) != 0)) {
            return Invalid(mod, "invalid vehicle skin asset");
        }
        for (const auto& existing : assets) {
            if (_stricmp(
                    existing.stockGameUri.c_str(),
                    source.stock_game_uri) == 0) {
                return SetError(
                    mod,
                    WOTBMOD_V3_E_CONFLICT,
                    "vehicle skin pack contains duplicate stock paths");
            }
        }
        try {
            VehicleVisualProfileObject::SkinAsset asset;
            asset.kind = source.kind;
            asset.part = source.part;
            asset.lod = source.lod;
            asset.stockGameUri = source.stock_game_uri;
            asset.replacementUri = source.replacement_uri;
            assets.push_back(std::move(asset));
        } catch (...) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "vehicle skin asset allocation failed");
        }
    }
    VehicleVisualProfileObject* profile =
        new (std::nothrow) VehicleVisualProfileObject();
    if (!profile) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "vehicle skin pack allocation failed");
    }
    profile->owner = mod;
    profile->id = descriptor->id;
    profile->vehicleName = descriptor->vehicle_name;
    profile->priority = descriptor->priority;
    profile->hangarOnly = descriptor->hangar_only;
    profile->assets = std::move(assets);
    profile->exactPathPack = true;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        profile,
        &DestroyVehicleProfile,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        delete profile;
        return result;
    }
    profile->self = handle;
    *outPack = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result RollbackVehicleSkinPack(
    WotbModV3Handle mod,
    VehicleVisualProfileObject* profile) {
    if (!profile || !profile->exactPathPack) {
        return Invalid(mod, "handle is not an exact-path vehicle skin pack");
    }
    std::vector<WotbModV3Handle> mounts;
    {
        std::lock_guard<std::mutex> lock(profile->operationMutex);
        mounts.swap(profile->mounts);
        profile->appliedVehicle = WOTBMOD_V3_INVALID_HANDLE;
    }
    WotbModV3Result firstFailure = WOTBMOD_V3_OK;
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it) {
        WotbModV3Result result = UnmountVfsForRuntime(mod, *it);
        if (result != WOTBMOD_V3_OK) {
            /* Cleanup must remain recoverable even if permissions changed. */
            const WotbModV3Result forced = ReleaseOwnedHandle(mod, *it);
            if (forced != WOTBMOD_V3_OK &&
                firstFailure == WOTBMOD_V3_OK) {
                firstFailure = result;
            }
        }
    }
    return firstFailure;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSkinPackApply(
    WotbModV3Handle mod,
    WotbModV3Handle packHandle,
    WotbModV3EntityHandle vehicle) {
    WotbModV3Result result = CheckVehicleSkinPackPermissions(mod);
    if (result != WOTBMOD_V3_OK) return result;
    VehicleVisualProfileObject* profile = nullptr;
    result = GetVehicleProfile(mod, packHandle, &profile);
    if (result != WOTBMOD_V3_OK) return result;
    if (!profile->exactPathPack) {
        return Invalid(mod, "handle is not a V2 vehicle skin pack");
    }
    result = CheckVehicleMutationAccess(mod, vehicle);
    if (result != WOTBMOD_V3_OK) return result;
    if (profile->hangarOnly != 0u &&
        CurrentContext() != WOTBMOD_V3_CONTEXT_HANGAR) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INCOMPATIBLE,
            "hangar-only vehicle skin cannot be applied here");
    }
    {
        std::lock_guard<std::mutex> lock(profile->operationMutex);
        if (!profile->mounts.empty() &&
            profile->appliedVehicle == vehicle) {
            return WOTBMOD_V3_OK;
        }
    }
    /* Resolve every source before mutating the active overlay set. */
    for (const auto& asset : profile->assets) {
        uint32_t required = 0u;
        result = ResolveVfsUriPhysical(
            mod,
            asset.replacementUri.c_str(),
            nullptr,
            &required);
        if (result != WOTBMOD_V3_E_BUFFER_TOO_SMALL || required == 0u) {
            return result == WOTBMOD_V3_OK
                ? SetError(
                      mod,
                      WOTBMOD_V3_E_NOT_FOUND,
                      "vehicle replacement resource is unavailable")
                : result;
        }
    }
    uint32_t generation = 0u;
    {
        std::lock_guard<std::mutex> lock(profile->operationMutex);
        generation = ++profile->applyGeneration;
        if (generation == 0u) generation = ++profile->applyGeneration;
    }
    std::vector<WotbModV3Handle> staged;
    try {
        staged.reserve(profile->assets.size());
    } catch (...) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "vehicle skin mount staging allocation failed");
    }
    for (uint32_t index = 0u;
         index < profile->assets.size();
         ++index) {
        char provider[WOTBMOD_V3_MAX_ID] = {};
#if defined(_MSC_VER)
        sprintf_s(
            provider,
            "veh_%016llx_%u_%u",
            static_cast<unsigned long long>(profile->self),
            generation,
            index);
#else
        std::snprintf(
            provider,
            sizeof(provider),
            "veh_%016llx_%u_%u",
            static_cast<unsigned long long>(profile->self),
            generation,
            index);
#endif
        WotbModV3Handle mount = WOTBMOD_V3_INVALID_HANDLE;
        result = MountVfsOverlayForRuntime(
            mod,
            provider,
            profile->assets[index].stockGameUri.c_str(),
            profile->assets[index].replacementUri.c_str(),
            profile->priority,
            &mount);
        if (result != WOTBMOD_V3_OK) {
            for (auto it = staged.rbegin(); it != staged.rend(); ++it) {
                UnmountVfsForRuntime(mod, *it);
            }
            return result;
        }
        staged.push_back(mount);
    }
    std::vector<WotbModV3Handle> previous;
    {
        std::lock_guard<std::mutex> lock(profile->operationMutex);
        previous.swap(profile->mounts);
        profile->mounts.swap(staged);
        profile->appliedVehicle = vehicle;
    }
    for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
        result = UnmountVfsForRuntime(mod, *it);
        if (result != WOTBMOD_V3_OK) {
            ReleaseOwnedHandle(mod, *it);
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSkinPackRollback(
    WotbModV3Handle mod,
    WotbModV3Handle packHandle) {
    WotbModV3Result result = CheckVehicleSkinPackPermissions(mod);
    if (result != WOTBMOD_V3_OK) return result;
    VehicleVisualProfileObject* profile = nullptr;
    result = GetVehicleProfile(mod, packHandle, &profile);
    if (result != WOTBMOD_V3_OK) return result;
    return RollbackVehicleSkinPack(mod, profile);
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSkinPackGetState(
    WotbModV3Handle mod,
    WotbModV3Handle packHandle,
    WotbModV3VehicleSkinState* outState) {
    if (!outState || outState->struct_size < sizeof(*outState) ||
        outState->api_version !=
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2) {
        return Invalid(mod, "vehicle skin state output is invalid");
    }
    VehicleVisualProfileObject* profile = nullptr;
    WotbModV3Result result =
        GetVehicleProfile(mod, packHandle, &profile);
    if (result != WOTBMOD_V3_OK) return result;
    if (!profile->exactPathPack) {
        return Invalid(mod, "handle is not a V2 vehicle skin pack");
    }
    WotbModV3VehicleSkinState state = {};
    state.struct_size = sizeof(state);
    state.api_version = WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2;
    state.asset_count = static_cast<uint32_t>(profile->assets.size());
    state.requires_model_reload = 1u;
    {
        std::lock_guard<std::mutex> lock(profile->operationMutex);
        state.applied = profile->mounts.empty() ? 0u : 1u;
        state.mounted_asset_count =
            static_cast<uint32_t>(profile->mounts.size());
        state.vehicle = profile->appliedVehicle;
    }
    *outState = state;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL VehicleSkinPackRelease(
    WotbModV3Handle mod,
    WotbModV3Handle packHandle) {
    WotbModV3Result result = CheckVehicleSkinPackPermissions(mod);
    if (result != WOTBMOD_V3_OK) return result;
    VehicleVisualProfileObject* profile = nullptr;
    result = GetVehicleProfile(mod, packHandle, &profile);
    if (result != WOTBMOD_V3_OK) return result;
    result = RollbackVehicleSkinPack(mod, profile);
    if (result != WOTBMOD_V3_OK) return result;
    return ReleaseOwnedHandle(mod, packHandle);
}

constexpr uint64_t kEntityPublicContexts =
    WOTBMOD_V3_CONTEXT_BATTLE |
    WOTBMOD_V3_CONTEXT_REPLAY |
    WOTBMOD_V3_CONTEXT_TRAINING;

bool GetHostPublicEntity(
    WotbModV3EntityHandle handle,
    HostPublicEntityRecord* outRecord) {
    if (!outRecord) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto found = g_hostEntities.find(handle);
    if (found == g_hostEntities.end()) {
        return false;
    }
    *outRecord = found->second;
    return true;
}

uint64_t PublicPropertyField(const char* property) {
    if (std::strcmp(property, "public_id") == 0 ||
        std::strcmp(property, "id") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_ID;
    }
    if (std::strcmp(property, "type") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_TYPE;
    }
    if (std::strcmp(property, "visible_to_player") == 0 ||
        std::strcmp(property, "visible") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_VISIBILITY;
    }
    if (std::strcmp(property, "local_player") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_LOCAL;
    }
    if (std::strcmp(property, "team") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_TEAM;
    }
    if (std::strcmp(property, "health") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_HEALTH;
    }
    if (std::strcmp(property, "max_health") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_MAX_HEALTH;
    }
    if (std::strcmp(property, "position") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_POSITION;
    }
    if (std::strcmp(property, "direction") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_DIRECTION;
    }
    if (std::strcmp(property, "public_type") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE;
    }
    if (std::strcmp(property, "display_name") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_DISPLAY_NAME;
    }
    if (std::strcmp(property, "clan_tag") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_CLAN_TAG;
    }
    if (std::strcmp(property, "account_id") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_ACCOUNT_ID;
    }
    if (std::strcmp(property, "kills") == 0 ||
        std::strcmp(property, "frags") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_KILLS;
    }
    if (std::strcmp(property, "vehicle_name") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_NAME;
    }
    if (std::strcmp(property, "vehicle_display_name") == 0) {
        return CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME;
    }
    return 0u;
}

void InitializePublicValue(
    WotbModV3PublicValue* value,
    uint32_t type) {
    *value = {};
    WOTBMOD_V3_INIT_STRUCT(
        *value,
        WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    value->type = type;
}

bool PublicPropertyFromSnapshot(
    const WotbModV3PublicEntitySnapshot& snapshot,
    const char* property,
    WotbModV3PublicValue* outValue) {
    if (std::strcmp(property, "public_id") == 0 ||
        std::strcmp(property, "id") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_INT64);
        outValue->value.integer = snapshot.public_id;
    } else if (std::strcmp(property, "type") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_INT64);
        outValue->value.integer = snapshot.type;
    } else if (
        std::strcmp(property, "visible_to_player") == 0 ||
        std::strcmp(property, "visible") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_BOOL);
        outValue->value.boolean = snapshot.visible_to_player;
    } else if (std::strcmp(property, "local_player") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_BOOL);
        outValue->value.boolean = snapshot.local_player;
    } else if (std::strcmp(property, "team") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_INT64);
        outValue->value.integer = snapshot.team;
    } else if (std::strcmp(property, "health") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_INT64);
        outValue->value.integer = snapshot.health;
    } else if (std::strcmp(property, "max_health") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_INT64);
        outValue->value.integer = snapshot.max_health;
    } else if (std::strcmp(property, "position") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_VEC3);
        outValue->value.vec3 = snapshot.position;
    } else if (std::strcmp(property, "direction") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_VEC3);
        outValue->value.vec3 = snapshot.direction;
    } else if (std::strcmp(property, "public_type") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_STRING);
        std::memcpy(
            outValue->value.string_value,
            snapshot.public_type,
            sizeof(outValue->value.string_value));
    } else if (std::strcmp(property, "display_name") == 0) {
        InitializePublicValue(
            outValue,
            WOTBMOD_V3_PUBLIC_VALUE_STRING);
        std::memcpy(
            outValue->value.string_value,
            snapshot.display_name,
            sizeof(outValue->value.string_value));
    } else {
        return false;
    }
    return true;
}

WotbModV3Result CheckEntityPublicPermission(
    WotbModV3Handle mod,
    bool local) {
    const WotbModV3Result permission =
        local
            ? CheckEitherNamedPermission(
                  mod,
                  "entity.public.visible",
                  WOTBMOD_V3_PERMISSION_SAFE,
                  "game.entity.public",
                  WOTBMOD_V3_PERMISSION_REVIEWED)
            : CheckNamedPermission(
                  mod,
                  "game.entity.public",
                  WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    return CheckAccess(
        mod,
        local
            ? WOTBMOD_V3_PERMISSION_SAFE
            : WOTBMOD_V3_PERMISSION_REVIEWED,
        kEntityPublicContexts,
        nullptr);
}

WotbModV3Result CheckEntityPublicAccess(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity) {
    if (entity == WOTBMOD_V3_INVALID_HANDLE) {
        const WotbModV3Result permission =
            CheckEitherNamedPermission(
                mod,
                "entity.public.visible",
                WOTBMOD_V3_PERMISSION_SAFE,
                "game.entity.public",
                WOTBMOD_V3_PERMISSION_REVIEWED);
        return permission == WOTBMOD_V3_OK
                   ? Invalid(mod, "entity handle is invalid")
                   : permission;
    }
    HostPublicEntityRecord record = {};
    const bool local =
        GetHostPublicEntity(entity, &record) &&
        record.snapshot.local_player != 0u;
    return CheckEntityPublicPermission(mod, local);
}

WotbModV3Result WOTBMOD_V3_CALL EntityGetPublicId(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity,
    uint32_t* outId) {
    const WotbModV3Result access =
        CheckEntityPublicAccess(mod, entity);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!outId) {
        return Invalid(mod, "public entity ID output pointer is null");
    }
    HostPublicEntityRecord record = {};
    if (GetHostPublicEntity(entity, &record)) {
        if (record.snapshot.visible_to_player == 0u) {
            return NotFound(
                mod,
                "public entity is not visible to the player");
        }
        *outId = record.snapshot.public_id;
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kEntityPublicContexts,
        "entity_get_public_id",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result == WOTBMOD_V3_OK) {
        *outId = response.value_u32;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL EntityGetPublicType(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity,
    char* buffer,
    uint32_t* inoutSize) {
    const WotbModV3Result access =
        CheckEntityPublicAccess(mod, entity);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!inoutSize) {
        return Invalid(mod, "public entity type size pointer is null");
    }
    HostPublicEntityRecord record = {};
    if (GetHostPublicEntity(entity, &record)) {
        if (record.snapshot.visible_to_player == 0u) {
            return NotFound(
                mod,
                "public entity is not visible to the player");
        }
        if ((record.validFields &
             CLIENT_HOST_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE) == 0u) {
            return SetError(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                "public entity type name is unavailable from the native source");
        }
        return CopyString(
            mod,
            std::string(record.snapshot.public_type),
            buffer,
            inoutSize);
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity;
    request.payload = buffer;
    request.payload_size = buffer ? *inoutSize : 0u;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kEntityPublicContexts,
        "entity_get_public_type",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (response.value_u32 != 0u) {
        *inoutSize = response.value_u32;
    }
    return result;
}

/* Snapshot fields plus the roster extras that sit beside the snapshot. */
bool PublicPropertyFromRecord(
    const WotbModV3PublicEntitySnapshot& snapshot,
    const ClientHostPublicEntityExtras& extras,
    const char* property,
    WotbModV3PublicValue* outValue) {
    if (std::strcmp(property, "clan_tag") == 0) {
        InitializePublicValue(outValue, WOTBMOD_V3_PUBLIC_VALUE_STRING);
        strncpy_s(
            outValue->value.string_value,
            sizeof(outValue->value.string_value),
            extras.clan_tag,
            _TRUNCATE);
        return true;
    }
    if (std::strcmp(property, "account_id") == 0) {
        InitializePublicValue(outValue, WOTBMOD_V3_PUBLIC_VALUE_INT64);
        outValue->value.integer = extras.account_id;
        return true;
    }
    if (std::strcmp(property, "kills") == 0 ||
        std::strcmp(property, "frags") == 0) {
        InitializePublicValue(outValue, WOTBMOD_V3_PUBLIC_VALUE_INT64);
        outValue->value.integer = extras.kills;
        return true;
    }
    if (std::strcmp(property, "vehicle_name") == 0) {
        InitializePublicValue(outValue, WOTBMOD_V3_PUBLIC_VALUE_STRING);
        strncpy_s(
            outValue->value.string_value,
            sizeof(outValue->value.string_value),
            extras.vehicle_name,
            _TRUNCATE);
        return true;
    }
    if (std::strcmp(property, "vehicle_display_name") == 0) {
        InitializePublicValue(outValue, WOTBMOD_V3_PUBLIC_VALUE_STRING);
        strncpy_s(
            outValue->value.string_value,
            sizeof(outValue->value.string_value),
            extras.vehicle_display_name,
            _TRUNCATE);
        return true;
    }
    return PublicPropertyFromSnapshot(snapshot, property, outValue);
}

WotbModV3Result WOTBMOD_V3_CALL EntityGetPublicProperty(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity,
    const char* property,
    WotbModV3PublicValue* outValue) {
    const WotbModV3Result access =
        CheckEntityPublicAccess(mod, entity);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!ValidText(property, WOTBMOD_V3_MAX_NAME, false) ||
        !outValue) {
        return Invalid(mod, "public entity property query is invalid");
    }
    HostPublicEntityRecord record = {};
    if (GetHostPublicEntity(entity, &record)) {
        if (record.snapshot.visible_to_player == 0u) {
            return NotFound(
                mod,
                "public entity is not visible to the player");
        }
        const uint64_t field = PublicPropertyField(property);
        if (field == 0u) {
            return NotFound(
                mod,
                "public entity property was not found");
        }
        if ((record.validFields & field) == 0u) {
            return SetError(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                "public entity property is unavailable from the native source");
        }
        if (!PublicPropertyFromRecord(
                record.snapshot,
                record.extras,
                property,
                outValue)) {
            return NotFound(
                mod,
                "public entity property was not found");
        }
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity;
    request.name = property;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kEntityPublicContexts,
        "entity_get_public_property",
        &request,
        sizeof(request),
        outValue,
        sizeof(*outValue));
}

WotbModV3Result WOTBMOD_V3_CALL EntitySubscribeProperty(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity,
    const char* property,
    WotbModV3PublicPropertyCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    const WotbModV3Result access =
        CheckEntityPublicAccess(mod, entity);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!ValidText(property, WOTBMOD_V3_MAX_NAME, false) || !callback ||
        !outToken) {
        return Invalid(
            mod,
            "public entity property subscription is invalid");
    }
    *outToken = WOTBMOD_V3_INVALID_HANDLE;
    HostPublicEntityRecord record = {};
    if (!GetHostPublicEntity(entity, &record)) {
        return NotFound(
            mod,
            "public entity is not registered by the client host");
    }
    if (record.snapshot.visible_to_player == 0u) {
        return NotFound(
            mod,
            "public entity is not visible to the player");
    }
    const uint64_t field = PublicPropertyField(property);
    if (field == 0u) {
        return NotFound(
            mod,
            "public entity property was not found");
    }
    if ((record.validFields & field) == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "public entity property is unavailable from the native source");
    }
    WotbModV3PublicValue value = {};
    if (!PublicPropertyFromRecord(
            record.snapshot, record.extras, property, &value)) {
        return NotFound(
            mod,
            "public entity property was not found");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "public entity subscription allocation failed");
    }
    subscription->kind = SubscriptionKind::EntityProperty;
    subscription->target = entity;
    subscription->property = property;
    subscription->entityPropertyCallback = callback;
    subscription->userData = userData;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL EntityUnsubscribeProperty(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    const WotbModV3Result permission = CheckEitherNamedPermission(
        mod,
        "entity.public.visible",
        WOTBMOD_V3_PERMISSION_SAFE,
        "game.entity.public",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    const WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::EntityProperty,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        WOTBMOD_V3_PERMISSION_SAFE,
        kEntityPublicContexts,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL EntityIsVisible(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity,
    uint32_t* outVisible) {
    const WotbModV3Result access =
        CheckEntityPublicAccess(mod, entity);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!outVisible) {
        return Invalid(mod, "entity visibility output pointer is null");
    }
    HostPublicEntityRecord record = {};
    if (GetHostPublicEntity(entity, &record)) {
        *outVisible = record.snapshot.visible_to_player;
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kEntityPublicContexts,
        "entity_is_visible_to_player",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result == WOTBMOD_V3_OK) {
        if (response.value_u32 > 1u) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "entity backend returned an invalid visibility flag");
        }
        *outVisible = response.value_u32;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL EntityGetSnapshot(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity,
    WotbModV3PublicEntitySnapshot* outSnapshot) {
    const WotbModV3Result access =
        CheckEntityPublicAccess(mod, entity);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!outSnapshot) {
        return Invalid(mod, "entity snapshot output pointer is null");
    }
    HostPublicEntityRecord record = {};
    if (GetHostPublicEntity(entity, &record)) {
        if (record.snapshot.visible_to_player == 0u) {
            return NotFound(
                mod,
                "public entity is not visible to the player");
        }
        *outSnapshot = record.snapshot;
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = entity;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kEntityPublicContexts,
        "entity_get_public_snapshot",
        &request,
        sizeof(request),
        outSnapshot,
        sizeof(*outSnapshot));
}

WotbModV3Result WOTBMOD_V3_CALL EntityEnumerateVisible(
    WotbModV3Handle mod,
    WotbModV3PublicEntityVisitor visitor,
    void* userData) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "game.entity.public",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!visitor) {
        return Invalid(mod, "visible entity visitor is null");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kEntityPublicContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    std::vector<WotbModV3PublicEntitySnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        snapshots.reserve(g_hostEntities.size());
        for (const auto& pair : g_hostEntities) {
            if (pair.second.snapshot.visible_to_player != 0u) {
                snapshots.push_back(pair.second.snapshot);
            }
        }
    }
    if (snapshots.empty()) {
        ClientHostEntityVisitorRequest request = {};
        request.struct_size = sizeof(request);
        request.api_version =
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
        request.mod = mod;
        request.visitor = visitor;
        request.user_data = userData;
        return InvokeHost(
            mod,
            WOTBMOD_V3_PERMISSION_REVIEWED,
            kEntityPublicContexts,
            "entity_enumerate_visible",
            &request,
            sizeof(request),
            nullptr,
            0u);
    }
    for (const WotbModV3PublicEntitySnapshot& snapshot : snapshots) {
        if (EnterModCallback(mod) != WOTBMOD_V3_OK) {
            return SetError(
                mod,
                WOTBMOD_V3_E_CANCELLED,
                "mod is no longer accepting callbacks");
        }
        WotbModV3Result visitResult = WOTBMOD_V3_OK;
        try {
            visitResult = visitor(mod, &snapshot, userData);
        } catch (...) {
            visitResult = SetError(
                mod,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "public entity visitor threw an exception");
        }
        LeaveModCallback(mod);
        if (visitResult != WOTBMOD_V3_OK) {
            return visitResult;
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RpcGetPolicy(
    WotbModV3Handle mod,
    WotbModV3BigWorldRpcPolicy* outPolicy) {
    const WotbModV3Result namedPermission = CheckNamedPermission(
        mod,
        "bigworld.observe",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (namedPermission != WOTBMOD_V3_OK) {
        return namedPermission;
    }
    const WotbModV3Result result = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_BATTLE |
            WOTBMOD_V3_CONTEXT_REPLAY |
            WOTBMOD_V3_CONTEXT_TRAINING,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (!outPolicy || outPolicy->struct_size < sizeof(*outPolicy) ||
        outPolicy->api_version != WOTBMOD_V3_BIGWORLD_RPC_VERSION) {
        return Invalid(mod, "RPC policy output structure is invalid");
    }
    outPolicy->metadata_observation = 0u;
    outPolicy->payload_access = 0u;
    outPolicy->outgoing_injection = 0u;
    outPolicy->packet_modification = 0u;
    outPolicy->packet_drop = 0u;
    outPolicy->packet_replay = 0u;

    /*
     * Probe the same host path used by subscribe_observed. A backend can be
     * installed while the reviewed native ingress hook is still absent, so
     * backend presence alone is not evidence that metadata observation works.
     * The temporary native subscription is removed before reporting support.
     */
    ClientHostRpcObserverRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.mod = mod;
    request.direction_mask = WOTBMOD_V3_RPC_INCOMING;
    ClientHostObjectResponse response = MakeHostResponse();
    uint64_t backendGeneration = 0u;
    WotbModV3Result probeResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kEntityPublicContexts,
        "rpc_subscribe_observed",
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &backendGeneration);
    if (probeResult != WOTBMOD_V3_OK) {
        return probeResult;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "RPC backend returned an invalid policy probe token");
    }
    bool staleGeneration = false;
    probeResult = TryReleaseHostObject(
        mod,
        "rpc_unsubscribe_observed",
        response.object,
        backendGeneration,
        false,
        &staleGeneration);
    if (probeResult != WOTBMOD_V3_OK) {
        if (!staleGeneration) {
            QueueHostReleaseAfterFailure(
                mod,
                "rpc_unsubscribe_observed",
                response.object,
                backendGeneration,
                probeResult);
        }
        return probeResult;
    }
    outPolicy->metadata_observation = 1u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL RpcSubscribeObserved(
    WotbModV3Handle mod,
    uint32_t directionMask,
    const char* methodFilter,
    WotbModV3RpcObservedCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "bigworld.observe",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if ((directionMask & ~3u) != 0u || directionMask == 0u ||
        (methodFilter &&
         !ValidText(methodFilter, WOTBMOD_V3_MAX_NAME)) ||
        !callback || !outToken) {
        return Invalid(mod, "RPC observation subscription is invalid");
    }
    if ((directionMask & WOTBMOD_V3_RPC_OUTGOING) != 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "Outgoing RPC observation has no reviewed native source");
    }
    uint64_t publicationEpoch = 0u;
    WotbModV3Result result =
        BeginSubscriptionCreation(mod, &publicationEpoch);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostRpcObserverRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    request.mod = mod;
    request.direction_mask = directionMask;
    request.method_filter = methodFilter;
    ClientHostObjectResponse response = MakeHostResponse();
    uint64_t backendGeneration = 0u;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kEntityPublicContexts,
        "rpc_subscribe_observed",
        &request,
        sizeof(request),
        &response,
        sizeof(response),
        &backendGeneration);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "RPC backend returned an invalid subscription token");
    }
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        ReleaseHostObject(
            mod,
            "rpc_unsubscribe_observed",
            response.object,
            backendGeneration);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "RPC subscription allocation failed");
    }
    subscription->kind = SubscriptionKind::RpcObserved;
    subscription->nativeObject = response.object;
    subscription->backendGeneration = backendGeneration;
    subscription->publicationEpoch = publicationEpoch;
    subscription->eventType = directionMask;
    subscription->methodFilter =
        methodFilter ? methodFilter : "";
    subscription->rpcObservedCallback = callback;
    subscription->userData = userData;
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL RpcUnsubscribeObserved(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "bigworld.observe",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::RpcObserved,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kEntityPublicContexts,
        "rpc_unsubscribe_observed");
}

struct TracerStyleObject {
    uint32_t magic = kTracerStyleMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle self = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t nativeObject = 0u;
    std::string id;
    std::string textureUri;
    WotbModV3Color color = {1.0f, 1.0f, 1.0f, 1.0f};
    float width = 1.0f;
    float lifetime = 1.0f;
    float fadeStart = 0.75f;
    int32_t priority = 0;
};

struct ImpactVisualObject {
    uint32_t magic = kImpactVisualMagic;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle self = WOTBMOD_V3_INVALID_HANDLE;
    std::string id;
    std::string sceneUri;
    uint32_t flags = WOTBMOD_V3_IMPACT_VISUAL_NATIVE_SCENE;
    uint32_t ownerScopeMask = 0u;
    uint32_t shellType = 0u;
    uint32_t maxInstances = 8u;
    float lifetimeSeconds = 1.0f;
    float uniformScale = 1.0f;
    int32_t priority = 0;
};

WotbModV3ProjectileSnapshot ProjectileSnapshotFromRecord(
    WotbModV3ProjectileHandle handle,
    const HostProjectileRecord& record) {
    WotbModV3ProjectileSnapshot snapshot = {};
    snapshot.struct_size = sizeof(snapshot);
    snapshot.api_version = WOTBMOD_V3_PROJECTILE_VERSION_2;
    snapshot.projectile = handle;
    snapshot.sequence_id = record.sequenceId;
    snapshot.valid_fields = record.validFields;
    snapshot.timestamp_microseconds = record.timestampMicroseconds;
    snapshot.lifecycle_state = record.lifecycleState;
    snapshot.source = record.source;
    snapshot.owner_scope = record.ownerScope;
    snapshot.shell_type = record.shellType;
    snapshot.native_shot_id = record.nativeShotId;
    snapshot.primary_entity_id = record.primaryEntityId;
    snapshot.secondary_entity_id = record.secondaryEntityId;
    snapshot.native_flags = record.nativeFlags;
    snapshot.stock_shot_code = record.stockShotCode;
    snapshot.origin = record.origin;
    snapshot.visible_direction = record.visibleDirection;
    snapshot.visible_position = record.visiblePosition;
    snapshot.impact_position = record.impactPosition;
    return snapshot;
}

void AssignHostProjectileRecord(
    HostProjectileRecord* record,
    const ClientHostProjectile& projectile) {
    if (!record) return;
    record->nativeToken = projectile.native_token;
    record->visualSceneObject = projectile.visual_scene_object;
    record->ownerScope = projectile.owner_scope;
    record->shellType = projectile.shell_type;
    record->visiblePosition = projectile.visible_position;
    record->visibleDirection = projectile.visible_direction;
    record->sequenceId = projectile.sequence_id != 0u
        ? projectile.sequence_id
        : projectile.native_token;
    record->validFields = projectile.valid_fields;
    record->timestampMicroseconds = projectile.timestamp_microseconds;
    record->lifecycleState = projectile.lifecycle_state != 0u
        ? projectile.lifecycle_state
        : WOTBMOD_V3_PROJECTILE_STATE_CREATED;
    record->source = projectile.source != 0u
        ? projectile.source
        : WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT;
    record->nativeShotId = projectile.native_shot_id;
    record->primaryEntityId = projectile.primary_entity_id;
    record->secondaryEntityId = projectile.secondary_entity_id;
    record->nativeFlags = projectile.native_flags;
    record->stockShotCode = projectile.stock_shot_code;
    record->origin = projectile.origin;
    record->impactPosition = projectile.impact_position;
}

WotbModV3Result CheckProjectileObservePermission(
    WotbModV3Handle mod) {
    return CheckNamedPermission(
        mod,
        "visible.projectile.events",
        WOTBMOD_V3_PERMISSION_REVIEWED);
}

WotbModV3Result CheckProjectileVisualPermission(
    WotbModV3Handle mod) {
    return CheckNamedPermission(
        mod,
        "gameplay.tweak.projectile_visual",
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK);
}

void DestroyTracerStyle(void* object) {
    TracerStyleObject* style = static_cast<TracerStyleObject*>(object);
    if (style) {
        if (style->nativeObject != 0u) {
            ReleaseHostObject(
                style->owner,
                "tracer_style_unregister",
                style->nativeObject);
            style->nativeObject = 0u;
        }
        style->magic = 0;
        delete style;
    }
}

WotbModV3Result GetTracerStyle(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    TracerStyleObject** outStyle) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outStyle) {
        return Invalid(mod, "tracer style output pointer is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_RESOURCE,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    TracerStyleObject* style = static_cast<TracerStyleObject*>(object);
    if (!style || style->magic != kTracerStyleMagic) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not a tracer style");
    }
    *outStyle = style;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL TracerStyleRegister(
    WotbModV3Handle mod,
    const WotbModV3TracerStyleDescriptor* descriptor,
    WotbModV3Handle* outStyle) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(descriptor, WOTBMOD_V3_PROJECTILE_VERSION) ||
        !outStyle ||
        !ValidText(descriptor->id, WOTBMOD_V3_MAX_ID, false) ||
        !ValidText(
            descriptor->texture_uri,
            WOTBMOD_V3_MAX_PATH,
            false) ||
        !IsFinite(descriptor->color) || !IsFinite(descriptor->width) ||
        descriptor->width <= 0.0f || descriptor->width > 100.0f ||
        !IsFinite(descriptor->lifetime_seconds) ||
        descriptor->lifetime_seconds <= 0.0f ||
        descriptor->lifetime_seconds > 60.0f ||
        !IsFinite(descriptor->fade_start) ||
        descriptor->fade_start < 0.0f ||
        descriptor->fade_start > 1.0f) {
        return Invalid(mod, "invalid tracer style descriptor");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.name = descriptor->id;
    request.secondary_name = descriptor->texture_uri;
    request.signed_value = descriptor->priority;
    request.scalar0 = descriptor->width;
    request.scalar1 = descriptor->lifetime_seconds;
    request.vector = {
        descriptor->color.r,
        descriptor->color.g,
        descriptor->color.b,
        descriptor->color.a};
    request.payload = descriptor;
    request.payload_size = sizeof(*descriptor);
    ClientHostObjectResponse response = MakeHostResponse();
    WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        "tracer_style_register",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    if (response.object == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "projectile backend returned an invalid tracer style");
    }
    TracerStyleObject* style =
        new (std::nothrow) TracerStyleObject();
    if (!style) {
        ReleaseHostObject(
            mod,
            "tracer_style_unregister",
            response.object);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "tracer style allocation failed");
    }
    style->owner = mod;
    style->nativeObject = response.object;
    style->id = descriptor->id;
    style->textureUri = descriptor->texture_uri;
    style->color = descriptor->color;
    style->width = descriptor->width;
    style->lifetime = descriptor->lifetime_seconds;
    style->fadeStart = descriptor->fade_start;
    style->priority = descriptor->priority;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        style,
        &DestroyTracerStyle,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "tracer_style_unregister",
            style->nativeObject);
        style->nativeObject = 0u;
        delete style;
        return result;
    }
    style->self = handle;
    *outStyle = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL TracerStyleUnregister(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    TracerStyleObject* style = nullptr;
    const WotbModV3Result result =
        GetTracerStyle(mod, handle, &style);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = style->nativeObject;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        "tracer_style_unregister",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    style->nativeObject = 0u;
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result WOTBMOD_V3_CALL TracerSetTexture(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    const char* textureUri) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(textureUri, WOTBMOD_V3_MAX_PATH, false)) {
        return Invalid(mod, "tracer texture URI is invalid");
    }
    TracerStyleObject* style = nullptr;
    const WotbModV3Result result =
        GetTracerStyle(mod, handle, &style);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = style->nativeObject;
    request.name = textureUri;
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        "tracer_set_texture",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    style->textureUri = textureUri;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL TracerSetColor(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    WotbModV3Color color) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!IsFinite(color)) {
        return Invalid(mod, "tracer color is invalid");
    }
    TracerStyleObject* style = nullptr;
    const WotbModV3Result result =
        GetTracerStyle(mod, handle, &style);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = style->nativeObject;
    request.vector = {color.r, color.g, color.b, color.a};
    const WotbModV3Result hostResult = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        "tracer_set_color",
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (hostResult != WOTBMOD_V3_OK) {
        return hostResult;
    }
    style->color = color;
    return WOTBMOD_V3_OK;
}

WotbModV3Result TracerSetFloat(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    float value,
    float minimum,
    float maximum,
    float TracerStyleObject::*member,
    const char* error,
    const char* operation) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!IsFinite(value) || value < minimum || value > maximum) {
        return Invalid(mod, error);
    }
    TracerStyleObject* style = nullptr;
    WotbModV3Result result =
        GetTracerStyle(mod, handle, &style);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = style->nativeObject;
    request.scalar0 = value;
    result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        operation,
        &request,
        sizeof(request),
        nullptr,
        0u);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    style->*member = value;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL TracerSetWidth(
    WotbModV3Handle mod,
    WotbModV3Handle style,
    float width) {
    return TracerSetFloat(
        mod,
        style,
        width,
        0.01f,
        100.0f,
        &TracerStyleObject::width,
        "tracer width is out of range",
        "tracer_set_width");
}

WotbModV3Result WOTBMOD_V3_CALL TracerSetLifetime(
    WotbModV3Handle mod,
    WotbModV3Handle style,
    float seconds) {
    return TracerSetFloat(
        mod,
        style,
        seconds,
        0.01f,
        60.0f,
        &TracerStyleObject::lifetime,
        "tracer lifetime is out of range",
        "tracer_set_lifetime");
}

WotbModV3Result WOTBMOD_V3_CALL TracerSetFade(
    WotbModV3Handle mod,
    WotbModV3Handle style,
    float fadeStart) {
    return TracerSetFloat(
        mod,
        style,
        fadeStart,
        0.0f,
        1.0f,
        &TracerStyleObject::fadeStart,
        "tracer fade start must be within [0, 1]",
        "tracer_set_fade");
}

bool GetHostProjectile(
    WotbModV3ProjectileHandle handle,
    HostProjectileRecord* outRecord) {
    if (!outRecord) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto found = g_hostProjectiles.find(handle);
    if (found == g_hostProjectiles.end()) {
        return false;
    }
    *outRecord = found->second;
    return true;
}

WotbModV3Result CheckProjectileVisualTarget(
    WotbModV3Handle mod,
    WotbModV3ProjectileHandle projectile,
    HostProjectileRecord* outRecord) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outRecord) {
        return Invalid(mod, "projectile visual record output is null");
    }
    if (projectile == WOTBMOD_V3_INVALID_HANDLE) {
        return Invalid(mod, "projectile handle is invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!GetHostProjectile(projectile, outRecord)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "projectile is not registered by the client host");
    }
    if (outRecord->ownerScope !=
            WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER &&
        outRecord->ownerScope !=
            WOTBMOD_V3_PROJECTILE_OWNER_REPLAY) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "projectile visual mutation is limited to the local player or replay");
    }
    if (outRecord->ownerScope ==
            WOTBMOD_V3_PROJECTILE_OWNER_REPLAY &&
        CurrentContext() != WOTBMOD_V3_CONTEXT_REPLAY) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INCOMPATIBLE,
            "replay projectile visuals are unavailable outside replay context");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result CheckProjectileAccess(
    WotbModV3Handle mod,
    WotbModV3ProjectileHandle projectile) {
    const WotbModV3Result permission =
        CheckProjectileObservePermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (projectile == WOTBMOD_V3_INVALID_HANDLE) {
        return Invalid(mod, "projectile handle is invalid");
    }
    return CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kEntityPublicContexts,
        nullptr);
}

WotbModV3Result WrapOwnedHostSceneObject(
    WotbModV3Handle mod,
    uint64_t nativeObject,
    const char* name,
    WotbModV3SceneHandle* outEntity) {
    if (nativeObject == 0u || !outEntity) {
        return NotFound(mod, "projectile visual entity is unavailable");
    }
    SceneEntity* entity = new (std::nothrow) SceneEntity();
    if (!entity) {
        ReleaseHostObject(
            mod,
            "scene_entity_destroy",
            nativeObject);
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "projectile visual wrapper allocation failed");
    }
    entity->owner = mod;
    entity->name = name ? name : "projectile_visual";
    entity->nativeObject = nativeObject;
    entity->ownsNativeObject = true;
    WOTBMOD_V3_INIT_STRUCT(
        entity->local,
        WOTBMOD_V3_ABI_VERSION);
    entity->local.rotation.w = 1.0f;
    entity->local.scale = {1.0f, 1.0f, 1.0f};
    WOTBMOD_V3_INIT_STRUCT(
        entity->bounds,
        WOTBMOD_V3_SCENE_VERSION);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_SCENE_ENTITY,
        entity,
        &DestroySceneEntity,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        ReleaseHostObject(
            mod,
            "scene_entity_destroy",
            entity->nativeObject);
        entity->nativeObject = 0u;
        delete entity;
        return result;
    }
    entity->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_sceneEntities[handle] = entity;
    }
    *outEntity = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ProjectileGetVisualEntity(
    WotbModV3Handle mod,
    WotbModV3ProjectileHandle projectile,
    WotbModV3SceneHandle* outEntity) {
    HostProjectileRecord record = {};
    const WotbModV3Result permission =
        CheckProjectileVisualTarget(mod, projectile, &record);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outEntity) {
        return Invalid(mod, "projectile visual output pointer is null");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = record.nativeToken;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        "projectile_get_visual_entity",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return WrapOwnedHostSceneObject(
        mod,
        response.object,
        "projectile_visual",
        outEntity);
}

WotbModV3Result WOTBMOD_V3_CALL ProjectileAttachVisual(
    WotbModV3Handle mod,
    WotbModV3ProjectileHandle projectile,
    WotbModV3SceneHandle entityHandle,
    uint32_t lifetimePolicy) {
    HostProjectileRecord record = {};
    WotbModV3Result result =
        CheckProjectileVisualTarget(mod, projectile, &record);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    SceneEntity* entity = nullptr;
    result = GetSceneEntity(mod, entityHandle, &entity);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    const uint32_t known =
        WOTBMOD_V3_ATTACHMENT_DETACH_ON_PARENT_DESTROY |
        WOTBMOD_V3_ATTACHMENT_DESTROY_ON_PARENT_DESTROY |
        WOTBMOD_V3_ATTACHMENT_KEEP_WORLD_TRANSFORM |
        WOTBMOD_V3_ATTACHMENT_KEEP_LOCAL_TRANSFORM;
    if ((lifetimePolicy & ~known) != 0u) {
        return Invalid(mod, "projectile visual lifetime policy is invalid");
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = record.nativeToken;
    request.related_object = entity->nativeObject;
    request.flags = lifetimePolicy;
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        kEntityPublicContexts,
        "projectile_attach_visual",
        &request,
        sizeof(request),
        nullptr,
        0u);
}

WotbModV3Result WOTBMOD_V3_CALL ProjectileGetOwnerScope(
    WotbModV3Handle mod,
    WotbModV3ProjectileHandle projectile,
    uint32_t* outScope) {
    const WotbModV3Result access =
        CheckProjectileAccess(mod, projectile);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!outScope) {
        return Invalid(mod, "projectile owner scope output pointer is null");
    }
    HostProjectileRecord record = {};
    if (GetHostProjectile(projectile, &record)) {
        *outScope = record.ownerScope;
        return WOTBMOD_V3_OK;
    }
    ClientHostObjectRequest request = MakeHostRequest();
    request.object = projectile;
    ClientHostObjectResponse response = MakeHostResponse();
    const WotbModV3Result result = InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kEntityPublicContexts,
        "projectile_get_owner_scope",
        &request,
        sizeof(request),
        &response,
        sizeof(response));
    if (result == WOTBMOD_V3_OK) {
        if (response.value_u32 >
            WOTBMOD_V3_PROJECTILE_OWNER_REPLAY) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "projectile backend returned an invalid owner scope");
        }
        *outScope = response.value_u32;
    }
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL ProjectileGetSnapshot(
    WotbModV3Handle mod,
    WotbModV3ProjectileHandle projectile,
    WotbModV3ProjectileSnapshot* outSnapshot) {
    const WotbModV3Result access =
        CheckProjectileAccess(mod, projectile);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!outSnapshot ||
        outSnapshot->struct_size < sizeof(*outSnapshot)) {
        return Invalid(mod, "projectile snapshot output is invalid");
    }
    HostProjectileRecord record = {};
    if (!GetHostProjectile(projectile, &record)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "projectile is not registered by the client host");
    }
    *outSnapshot = ProjectileSnapshotFromRecord(projectile, record);
    return WOTBMOD_V3_OK;
}

bool ValidImpactVisualDescriptor(
    const WotbModV3ImpactVisualDescriptor* descriptor) {
    constexpr uint32_t kKnownFlags =
        WOTBMOD_V3_IMPACT_VISUAL_NATIVE_SCENE;
    constexpr uint32_t kKnownScopes =
        (1u << WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER) |
        (1u << WOTBMOD_V3_PROJECTILE_OWNER_ALLY_VISIBLE) |
        (1u << WOTBMOD_V3_PROJECTILE_OWNER_ENEMY_VISIBLE) |
        (1u << WOTBMOD_V3_PROJECTILE_OWNER_REPLAY);
    return ValidStruct(
               descriptor,
               WOTBMOD_V3_PROJECTILE_VERSION_2) &&
           ValidText(descriptor->id, WOTBMOD_V3_MAX_ID, false) &&
           ValidText(
               descriptor->scene_uri,
               WOTBMOD_V3_MAX_PATH,
               false) &&
           descriptor->flags ==
               WOTBMOD_V3_IMPACT_VISUAL_NATIVE_SCENE &&
           (descriptor->flags & ~kKnownFlags) == 0u &&
           descriptor->owner_scope_mask != 0u &&
           (descriptor->owner_scope_mask & ~kKnownScopes) == 0u &&
           descriptor->max_instances >= 1u &&
           descriptor->max_instances <= 64u &&
           IsFinite(descriptor->lifetime_seconds) &&
           descriptor->lifetime_seconds >= 0.05f &&
           descriptor->lifetime_seconds <= 30.0f &&
           IsFinite(descriptor->uniform_scale) &&
           descriptor->uniform_scale > 0.0f &&
           descriptor->uniform_scale <= 100.0f;
}

void CopyImpactVisualDescriptor(
    ImpactVisualObject* visual,
    const WotbModV3ImpactVisualDescriptor& descriptor) {
    visual->id = descriptor.id;
    visual->sceneUri = descriptor.scene_uri;
    visual->flags = descriptor.flags;
    visual->ownerScopeMask = descriptor.owner_scope_mask;
    visual->shellType = descriptor.shell_type;
    visual->maxInstances = descriptor.max_instances;
    visual->lifetimeSeconds = descriptor.lifetime_seconds;
    visual->uniformScale = descriptor.uniform_scale;
    visual->priority = descriptor.priority;
}

void DestroyImpactVisual(void* object) {
    ImpactVisualObject* visual =
        static_cast<ImpactVisualObject*>(object);
    if (!visual) return;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_impactVisuals.erase(visual->self);
    }
    visual->magic = 0u;
    delete visual;
}

WotbModV3Result GetImpactVisual(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    ImpactVisualObject** outVisual) {
    if (!outVisual) {
        return Invalid(mod, "impact visual output is null");
    }
    void* object = nullptr;
    const WotbModV3Result result = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_RESOURCE,
        &object,
        nullptr);
    if (result != WOTBMOD_V3_OK) return result;
    ImpactVisualObject* visual =
        static_cast<ImpactVisualObject*>(object);
    if (!visual || visual->magic != kImpactVisualMagic) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is not an impact visual");
    }
    *outVisual = visual;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ImpactVisualRegister(
    WotbModV3Handle mod,
    const WotbModV3ImpactVisualDescriptor* descriptor,
    WotbModV3Handle* outVisual) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) return permission;
    if (!ValidImpactVisualDescriptor(descriptor) || !outVisual) {
        return Invalid(mod, "invalid impact visual descriptor");
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        uint32_t owned = 0u;
        for (const auto& entry : g_impactVisuals) {
            if (entry.second && entry.second->owner == mod) ++owned;
        }
        if (owned >= 32u) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "impact visual registration limit reached");
        }
    }
    ImpactVisualObject* visual =
        new (std::nothrow) ImpactVisualObject();
    if (!visual) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "impact visual allocation failed");
    }
    visual->owner = mod;
    CopyImpactVisualDescriptor(visual, *descriptor);
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_RESOURCE,
        visual,
        &DestroyImpactVisual,
        &handle);
    if (created != WOTBMOD_V3_OK) {
        delete visual;
        return created;
    }
    visual->self = handle;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_impactVisuals[handle] = visual;
    }
    *outVisual = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ImpactVisualUpdate(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    const WotbModV3ImpactVisualDescriptor* descriptor) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) return permission;
    if (!ValidImpactVisualDescriptor(descriptor)) {
        return Invalid(mod, "invalid impact visual descriptor");
    }
    ImpactVisualObject* visual = nullptr;
    const WotbModV3Result found =
        GetImpactVisual(mod, handle, &visual);
    if (found != WOTBMOD_V3_OK) return found;
    std::lock_guard<std::mutex> lock(g_clientMutex);
    CopyImpactVisualDescriptor(visual, *descriptor);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ImpactVisualUnregister(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    const WotbModV3Result permission =
        CheckProjectileVisualPermission(mod);
    if (permission != WOTBMOD_V3_OK) return permission;
    ImpactVisualObject* visual = nullptr;
    const WotbModV3Result found =
        GetImpactVisual(mod, handle, &visual);
    if (found != WOTBMOD_V3_OK) return found;
    (void)visual;
    return ReleaseOwnedHandle(mod, handle);
}

struct ImpactVisualCandidate {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle visual = WOTBMOD_V3_INVALID_HANDLE;
    std::string sceneUri;
    uint32_t maxInstances = 0u;
    float lifetimeSeconds = 0.0f;
    float uniformScale = 1.0f;
    int32_t priority = 0;
};

void SpawnImpactVisuals(
    const WotbModV3ProjectileSnapshot& snapshot) {
    if ((snapshot.valid_fields &
         WOTBMOD_V3_PROJECTILE_FIELD_IMPACT_POSITION) == 0u ||
        !IsFinite(snapshot.impact_position) ||
        snapshot.owner_scope <
            WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER ||
        snapshot.owner_scope > WOTBMOD_V3_PROJECTILE_OWNER_REPLAY) {
        return;
    }
    std::vector<ImpactVisualCandidate> candidates;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        candidates.reserve(g_impactVisuals.size());
        for (const auto& entry : g_impactVisuals) {
            const ImpactVisualObject* visual = entry.second;
            if (!visual || visual->magic != kImpactVisualMagic ||
                (visual->ownerScopeMask &
                 (1u << snapshot.owner_scope)) == 0u ||
                (visual->shellType != 0u &&
                 visual->shellType != snapshot.shell_type)) {
                continue;
            }
            uint32_t active = 0u;
            for (const ManagedImpactInstance& instance :
                 g_impactInstances) {
                if (instance.visual == visual->self) ++active;
            }
            if (active >= visual->maxInstances) continue;
            candidates.push_back(
                {visual->owner,
                 visual->self,
                 visual->sceneUri,
                 visual->maxInstances,
                 visual->lifetimeSeconds,
                 visual->uniformScale,
                 visual->priority});
        }
    }
    std::stable_sort(
        candidates.begin(),
        candidates.end(),
        [](const ImpactVisualCandidate& left,
           const ImpactVisualCandidate& right) {
            return left.priority > right.priority;
        });
    if (candidates.size() > 8u) candidates.resize(8u);

    for (const ImpactVisualCandidate& candidate : candidates) {
        if (!IsModEnabled(candidate.owner) ||
            CheckProjectileVisualPermission(candidate.owner) !=
                WOTBMOD_V3_OK) {
            continue;
        }
        std::string resolvedPath;
        if (ResolveVirtualFilePath(
                candidate.owner,
                candidate.sceneUri.c_str(),
                &resolvedPath) != WOTBMOD_V3_OK) {
            continue;
        }
        ClientHostObjectRequest load = MakeHostRequest();
        load.name = candidate.sceneUri.c_str();
        load.secondary_name = resolvedPath.empty()
            ? nullptr
            : resolvedPath.c_str();
        ClientHostObjectResponse loaded = MakeHostResponse();
        if (InvokeHost(
                candidate.owner,
                WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
                kEntityPublicContexts,
                "scene_entity_load",
                &load,
                sizeof(load),
                &loaded,
                sizeof(loaded)) != WOTBMOD_V3_OK ||
            loaded.object == 0u) {
            continue;
        }
        WotbModV3SceneHandle entity = WOTBMOD_V3_INVALID_HANDLE;
        if (WrapOwnedHostSceneObject(
                candidate.owner,
                loaded.object,
                "managed_impact_visual",
                &entity) != WOTBMOD_V3_OK) {
            continue;
        }
        ClientHostObjectResponse activeResponse = MakeHostResponse();
        if (InvokeHost(
                candidate.owner,
                WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
                kEntityPublicContexts,
                "scene_get_active",
                nullptr,
                0u,
                &activeResponse,
                sizeof(activeResponse)) != WOTBMOD_V3_OK ||
            activeResponse.object == 0u) {
            ReleaseOwnedHandle(candidate.owner, entity);
            continue;
        }
        WotbModV3SceneHandle active = WOTBMOD_V3_INVALID_HANDLE;
        if (WrapOwnedHostSceneObject(
                candidate.owner,
                activeResponse.object,
                "managed_impact_active_scene",
                &active) != WOTBMOD_V3_OK) {
            ReleaseOwnedHandle(candidate.owner, entity);
            continue;
        }
        SceneEntity* entityObject = nullptr;
        SceneEntity* activeObject = nullptr;
        if (GetSceneEntity(candidate.owner, entity, &entityObject) !=
                WOTBMOD_V3_OK ||
            GetSceneEntity(candidate.owner, active, &activeObject) !=
                WOTBMOD_V3_OK) {
            ReleaseOwnedHandle(candidate.owner, entity);
            ReleaseOwnedHandle(candidate.owner, active);
            continue;
        }
        WotbModV3Transform transform = {};
        WOTBMOD_V3_INIT_STRUCT(transform, WOTBMOD_V3_ABI_VERSION);
        transform.position = snapshot.impact_position;
        transform.rotation.w = 1.0f;
        transform.scale = {
            candidate.uniformScale,
            candidate.uniformScale,
            candidate.uniformScale};
        ClientHostObjectRequest setTransform = MakeHostRequest();
        setTransform.object = entityObject->nativeObject;
        setTransform.payload = &transform;
        setTransform.payload_size = sizeof(transform);
        ClientHostObjectRequest setParent = MakeHostRequest();
        setParent.object = entityObject->nativeObject;
        setParent.related_object = activeObject->nativeObject;
        const bool attached =
            InvokeHost(
                candidate.owner,
                WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
                kEntityPublicContexts,
                "scene_entity_set_transform",
                &setTransform,
                sizeof(setTransform),
                nullptr,
                0u) == WOTBMOD_V3_OK &&
            InvokeHost(
                candidate.owner,
                WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
                kEntityPublicContexts,
                "scene_entity_set_parent",
                &setParent,
                sizeof(setParent),
                nullptr,
                0u) == WOTBMOD_V3_OK;
        if (!attached) {
            ReleaseOwnedHandle(candidate.owner, entity);
            ReleaseOwnedHandle(candidate.owner, active);
            continue;
        }
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(g_clientMutex);
            entityObject->local = transform;
            entityObject->parent = active;
            activeObject->children.push_back(entity);
            if (g_impactInstances.size() < 256u) {
                g_impactInstances.push_back(
                    {candidate.owner,
                     candidate.visual,
                     entity,
                     active,
                     candidate.lifetimeSeconds});
                queued = true;
            }
        }
        if (!queued) {
            ReleaseOwnedHandle(candidate.owner, entity);
            ReleaseOwnedHandle(candidate.owner, active);
        }
    }
}

struct MissingBindingDescriptor {
    const char* capability;
    const char* symbol;
    const char* reason;
};

const MissingBindingDescriptor kMissingClientBindings[] = {
    {"render.callbacks", "native_render_pipeline",
     "render frame phase and command backend is not installed"},
    {"render.native", "native_rhi_objects",
     "native RHI device, context, and swapchain backend is not installed"},
    {"camera.read", "native_camera",
     "native camera discovery and projection backend is not installed"},
    {"scene.native", "native_scene_graph",
     "native scene loading and attachment backend is not installed"},
    {"audio.native", "native_sound_engine",
     "native sound engine backend is not installed"},
    {"gameplay.tweak.vehicle", "native_vehicle_appearance",
     "native vehicle visual backend is not installed"},
    {"gameplay.camera", "native_gameplay_camera",
     "native gameplay camera backend is not installed"},
    {"gameplay.hud", "native_gameplay_hud",
     "native gameplay HUD backend is not installed"},
    {"gameplay.tweak.hangar", "native_hangar",
     "native hangar backend is not installed"},
    {"gameplay.replay", "native_replay",
     "native replay backend is not installed"},
    {"game.entity.public", "native_public_entities",
     "native visible public-entity backend is not installed"},
    {"visible.entity.rpc", "native_rpc_observer",
     "reviewed metadata-only RPC observation backend is not installed"},
    {"visible.projectile.events", "native_projectiles",
     "native visible projectile event backend is not installed"},
    {"client.leave_to_hangar", "native_leave_to_hangar",
     "native leave-to-hangar backend is not installed"}};

/*
 * These are operation-level gaps in the current binding pack, even when the
 * pack's required anchors and the host bridge itself validate successfully.
 * Keeping them separate prevents a globally SUPPORTED binding pack from
 * falsely implying that every public client operation has a native ingress.
 */
const MissingBindingDescriptor kPartialClientBindings[] = {
    {"ui.slots", "native_ui_semantic_extension_points",
     "base active-screen/root UIControl access is separate and does not resolve exact named extension points; semantic slots return NOT_SUPPORTED"},
    {"ui.advanced", "native_ui_widgets_layout_style_events",
     "active-screen inspection, runtime native controls, text/image/font/color/background mutation, independent clone, managed layout/style ownership and budgeted native input events are available; raw DAVA class/component injection, localization/tooltip/accessibility/focus and native animation components are not provided"},
    {"render.callbacks", "native_render_before_ui_after_ui",
     "PRESENT has a real DXGI ingress; BEFORE_UI and AFTER_UI phases are not implemented"},
    {"camera.modifiers", "native_camera_modifier_phases",
     "verified active-camera FOV is available; native BEFORE_GAME and BEFORE_UI modifier ingress is not implemented"},
    {"scene.advanced", "native_scene_nodes_bounds_animation_material",
     "base entity create/load/clone, hierarchy attachment, transforms and managed draw_mesh scene attachment are available; native node enumeration, bounds, animation and live NMaterial/shader mutation are not proven"},
    {"audio.advanced", "native_audio_stream_mixer_listener",
     "custom clip playback and reviewed native sound-event lifecycle are available; native streaming, timed start/fade/seek/duration, listener control and bus mixer APIs are not implemented"},
    {"gameplay.tweak.vehicle", "native_vehicle_appearance_full",
     "transactional exact-path mesh/material/texture skin packs with explicit LOD and rollback are available; already-cached model hot swap and live NMaterial property mutation are not proven"},
    {"gameplay.camera", "native_gameplay_camera_advanced",
     "verified transform/FOV/projection, managed transitions and shake are available; arcade/sniper are native and hangar/replay are context-derived, while postmortem/free/cinematic ownership is not proven"},
    {"gameplay.hud", "native_gameplay_hud",
     "damage log visibility/anchor, minimap size/opacity, sixth-sense position/scale, reticle size and reset drive the client's own battle controls by name once the names are mapped for the build; textures, sounds, formats, session stats, markers and hit-indicator styling are not implemented"},
    {"gameplay.tweak.hangar", "native_hangar_customization",
     "native hangar background, lighting, floor and preview customization is not implemented"},
    {"gameplay.replay", "native_replay_controls",
     "native replay speed, seek, markers, free-camera and export controls are not implemented"},
    {"game.entity.public", "native_public_entity_optional_fields",
     "native vehicle ingress provides ID, type, visibility, local flag, health, max-health and public type; team, position, direction and display name are not sourced"},
    {"visible.entity.rpc", "native_rpc_observer_generic",
     "curated incoming public RPC metadata is available; generic RPC interception and every payload operation are intentionally unavailable"},
    {"visible.projectile.events", "native_tracer_style",
     "stock shot and impact ingress, provenance-aware lifecycle records, managed visual attachment and impact visuals are available; TracerManager style-object ownership and stock tracer mutation are not proven"},
    {"resources.dava", "native_yaml_archive_objects",
     "bounded generic YAML, directory archives and DVPL are available; DAVA YAML node and ResourceArchive object ABI bridges are not implemented"},
    {"input.actions", "native_input_event_ingress",
     "WndProc/DAVA input ingress, bounded action queues and coalescing are available; unverified device-specific DAVA payload fields are intentionally not invented"}};

template <size_t N>
void CopyFixed(char (&destination)[N], const char* source) {
    if (!source) {
        destination[0] = '\0';
        return;
    }
#if defined(_MSC_VER)
    strncpy_s(destination, N, source, _TRUNCATE);
#else
    std::strncpy(destination, source, N - 1u);
    destination[N - 1u] = '\0';
#endif
}

WotbModV3Result WOTBMOD_V3_CALL ClientIsSupported(
    WotbModV3Handle mod,
    uint32_t* outSupported) {
    if (!outSupported) {
        return Invalid(mod, "client support output pointer is null");
    }
    const WotbModV3Result result = CheckMod(mod);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostBackend backend = {};
    *outSupported =
        GetHostBackend(&backend) &&
        (backend.compatibility_state ==
             WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED ||
         backend.compatibility_state ==
             WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED)
            ? 1u
            : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ClientGetBindingPackVersion(
    WotbModV3Handle mod,
    uint32_t* outVersion) {
    if (!outVersion) {
        return Invalid(mod, "binding-pack version output pointer is null");
    }
    const WotbModV3Result result = CheckMod(mod);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostBackend backend = {};
    *outVersion = GetHostBackend(&backend)
        ? backend.binding_pack_version
        : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ClientGetCompatibilityState(
    WotbModV3Handle mod,
    uint32_t* outState) {
    if (!outState) {
        return Invalid(mod, "client compatibility output pointer is null");
    }
    const WotbModV3Result result = CheckMod(mod);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    ClientHostBackend backend = {};
    if (!GetHostBackend(&backend)) {
        *outState =
            WOTBMOD_V3_CLIENT_COMPATIBILITY_BINDINGS_MISSING;
    } else {
        *outState = backend.compatibility_state ==
                WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED
            ? WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED
            : backend.compatibility_state;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ClientEnumerateMissingBindings(
    WotbModV3Handle mod,
    WotbModV3MissingBindingVisitor visitor,
    void* userData) {
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (!visitor) {
        return Invalid(mod, "missing-binding visitor is null");
    }
    ClientHostBackend backend = {};
    const bool supportedPack =
        GetHostBackend(&backend) &&
        backend.compatibility_state ==
            WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    const auto visitBindings =
        [mod, visitor, userData](
            const MissingBindingDescriptor* descriptors,
            size_t count) -> WotbModV3Result {
            for (size_t index = 0u; index < count; ++index) {
                const MissingBindingDescriptor& descriptor =
                    descriptors[index];
                WotbModV3MissingBinding binding = {};
                binding.struct_size = sizeof(binding);
                binding.api_version = WOTBMOD_V3_CLIENT_VERSION;
                CopyFixed(binding.capability, descriptor.capability);
                CopyFixed(binding.symbol, descriptor.symbol);
                CopyFixed(binding.reason, descriptor.reason);
                const WotbModV3Result visit =
                    visitor(mod, &binding, userData);
                if (visit != WOTBMOD_V3_OK) {
                    return visit;
                }
            }
            return WOTBMOD_V3_OK;
        };
    if (!supportedPack) {
        const WotbModV3Result missing = visitBindings(
            kMissingClientBindings,
            sizeof(kMissingClientBindings) /
                sizeof(kMissingClientBindings[0]));
        if (missing != WOTBMOD_V3_OK) {
            return missing;
        }
    }
    const WotbModV3Result partial = visitBindings(
        kPartialClientBindings,
        sizeof(kPartialClientBindings) /
            sizeof(kPartialClientBindings[0]));
    if (partial != WOTBMOD_V3_OK) {
        return partial;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ClientLeaveToHangar(
    WotbModV3Handle mod) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "client.leave_to_hangar",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    return InvokeHost(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        WOTBMOD_V3_CONTEXT_BATTLE |
            WOTBMOD_V3_CONTEXT_REPLAY |
            WOTBMOD_V3_CONTEXT_TRAINING,
        "client_leave_to_hangar",
        nullptr,
        0u,
        nullptr,
        0u);
}

struct GraphicsAdapterSnapshot {
    uint32_t vendorId = 0u;
    uint32_t deviceId = 0u;
    uint32_t subsystemId = 0u;
    uint32_t revision = 0u;
    uint32_t software = 0u;
    uint64_t dedicatedVideoMemory = 0u;
    uint64_t dedicatedSystemMemory = 0u;
    uint64_t sharedSystemMemory = 0u;
    std::string name;
};

std::once_flag g_adapterOnce;
std::vector<GraphicsAdapterSnapshot> g_adapters;

#if defined(_WIN32)
std::string WideToUtf8(const wchar_t* value) {
    if (!value || !*value) {
        return std::string();
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 1) {
        return std::string();
    }
    std::vector<char> buffer(static_cast<size_t>(required));
    const int written = WideCharToMultiByte(
        CP_UTF8,
        0,
        value,
        -1,
        buffer.data(),
        required,
        nullptr,
        nullptr);
    return written > 0 ? std::string(buffer.data()) : std::string();
}

void EnumerateGraphicsAdapters() {
    HMODULE module = LoadLibraryW(L"dxgi.dll");
    if (!module) {
        return;
    }
    typedef HRESULT(WINAPI* CreateDxgiFactory1Fn)(REFIID, void**);
    CreateDxgiFactory1Fn createFactory =
        reinterpret_cast<CreateDxgiFactory1Fn>(
            GetProcAddress(module, "CreateDXGIFactory1"));
    if (!createFactory) {
        FreeLibrary(module);
        return;
    }
    IDXGIFactory1* factory = nullptr;
    const HRESULT created = createFactory(
        __uuidof(IDXGIFactory1),
        reinterpret_cast<void**>(&factory));
    if (FAILED(created) || !factory) {
        FreeLibrary(module);
        return;
    }
    for (UINT index = 0u;; ++index) {
        IDXGIAdapter1* adapter = nullptr;
        const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
        if (enumerated == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumerated) || !adapter) {
            continue;
        }
        DXGI_ADAPTER_DESC1 descriptor = {};
        if (SUCCEEDED(adapter->GetDesc1(&descriptor))) {
            GraphicsAdapterSnapshot snapshot;
            snapshot.vendorId = descriptor.VendorId;
            snapshot.deviceId = descriptor.DeviceId;
            snapshot.subsystemId = descriptor.SubSysId;
            snapshot.revision = descriptor.Revision;
            snapshot.software =
                (descriptor.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u
                    ? 1u
                    : 0u;
            snapshot.dedicatedVideoMemory =
                static_cast<uint64_t>(
                    descriptor.DedicatedVideoMemory);
            snapshot.dedicatedSystemMemory =
                static_cast<uint64_t>(
                    descriptor.DedicatedSystemMemory);
            snapshot.sharedSystemMemory =
                static_cast<uint64_t>(
                    descriptor.SharedSystemMemory);
            snapshot.name = WideToUtf8(descriptor.Description);
            g_adapters.push_back(std::move(snapshot));
        }
        adapter->Release();
    }
    factory->Release();
    FreeLibrary(module);
}

uint32_t ProcessArchitecture() {
#if defined(_M_X64)
    return WOTBMOD_V3_ARCH_X64;
#elif defined(_M_IX86)
    return WOTBMOD_V3_ARCH_X86;
#elif defined(_M_ARM64)
    return WOTBMOD_V3_ARCH_ARM64;
#elif defined(_M_ARM)
    return WOTBMOD_V3_ARCH_ARM32;
#else
    return WOTBMOD_V3_ARCH_UNKNOWN;
#endif
}

uint32_t OperatingSystemArchitecture(const SYSTEM_INFO& info) {
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_INTEL:
            return WOTBMOD_V3_ARCH_X86;
        case PROCESSOR_ARCHITECTURE_AMD64:
            return WOTBMOD_V3_ARCH_X64;
        case PROCESSOR_ARCHITECTURE_ARM:
            return WOTBMOD_V3_ARCH_ARM32;
#if defined(PROCESSOR_ARCHITECTURE_ARM64)
        case PROCESSOR_ARCHITECTURE_ARM64:
            return WOTBMOD_V3_ARCH_ARM64;
#endif
        default:
            return WOTBMOD_V3_ARCH_UNKNOWN;
    }
}

std::string ProcessorName() {
    int query[4] = {};
    __cpuid(query, static_cast<int>(0x80000000u));
    const uint32_t maximum =
        static_cast<uint32_t>(query[0]);
    if (maximum < 0x80000004u) {
        return std::string("Unknown processor");
    }
    char brand[49] = {};
    int* words = reinterpret_cast<int*>(brand);
    __cpuid(words, static_cast<int>(0x80000002u));
    __cpuid(words + 4, static_cast<int>(0x80000003u));
    __cpuid(words + 8, static_cast<int>(0x80000004u));
    std::string result(brand);
    const size_t first = result.find_first_not_of(' ');
    if (first == std::string::npos) {
        return std::string("Unknown processor");
    }
    result.erase(0u, first);
    const size_t last = result.find_last_not_of(' ');
    if (last != std::string::npos) {
        result.erase(last + 1u);
    }
    return result;
}

std::string OperatingSystemName() {
    typedef LONG(WINAPI* RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW version = {};
    version.dwOSVersionInfoSize = sizeof(version);
    HMODULE module = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn getVersion = module
        ? reinterpret_cast<RtlGetVersionFn>(
              GetProcAddress(module, "RtlGetVersion"))
        : nullptr;
    if (!getVersion || getVersion(&version) != 0) {
        return std::string("Windows");
    }
    char value[WOTBMOD_V3_MAX_NAME] = {};
#if defined(_MSC_VER)
    sprintf_s(
        value,
        "Windows %lu.%lu build %lu",
        version.dwMajorVersion,
        version.dwMinorVersion,
        version.dwBuildNumber);
#else
    std::snprintf(
        value,
        sizeof(value),
        "Windows %lu.%lu build %lu",
        version.dwMajorVersion,
        version.dwMinorVersion,
        version.dwBuildNumber);
#endif
    return std::string(value);
}
#else
void EnumerateGraphicsAdapters() {
}

uint32_t ProcessArchitecture() {
    return sizeof(void*) == 8u
        ? WOTBMOD_V3_ARCH_X64
        : WOTBMOD_V3_ARCH_X86;
}
#endif

WotbModV3Result DeviceAccess(WotbModV3Handle mod) {
    return CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL DeviceGetInfo(
    WotbModV3Handle mod,
    WotbModV3DeviceInfo* outInfo) {
    if (!ValidStruct(outInfo, WOTBMOD_V3_DEVICE_VERSION)) {
        return Invalid(mod, "device info structure header is invalid");
    }
    const WotbModV3Result access = DeviceAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    std::call_once(g_adapterOnce, &EnumerateGraphicsAdapters);
    WotbModV3DeviceInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_DEVICE_VERSION;
    info.process_architecture = ProcessArchitecture();
    info.logical_processor_count =
        std::thread::hardware_concurrency();
    info.graphics_adapter_count =
        static_cast<uint32_t>(g_adapters.size());
#if defined(_WIN32)
    SYSTEM_INFO systemInfo = {};
    GetNativeSystemInfo(&systemInfo);
    info.operating_system_architecture =
        OperatingSystemArchitecture(systemInfo);
    if (info.logical_processor_count == 0u) {
        info.logical_processor_count =
            systemInfo.dwNumberOfProcessors;
    }
    MEMORYSTATUSEX memory = {};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        info.physical_memory_bytes =
            static_cast<uint64_t>(memory.ullTotalPhys);
        info.available_memory_bytes =
            static_cast<uint64_t>(memory.ullAvailPhys);
    }
    const std::string os = OperatingSystemName();
    const std::string processor = ProcessorName();
    CopyFixed(info.operating_system, os.c_str());
    CopyFixed(info.processor, processor.c_str());
#else
    info.operating_system_architecture =
        info.process_architecture;
    CopyFixed(info.operating_system, "Unsupported platform");
    CopyFixed(info.processor, "Unknown processor");
#endif
    if (!g_adapters.empty()) {
        const GraphicsAdapterSnapshot* primary = &g_adapters.front();
        for (const GraphicsAdapterSnapshot& adapter : g_adapters) {
            if (!adapter.software) {
                primary = &adapter;
                break;
            }
        }
        CopyFixed(
            info.primary_graphics_adapter,
            primary->name.c_str());
    }
    *outInfo = info;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL DeviceGetGraphicsAdapterCount(
    WotbModV3Handle mod,
    uint32_t* outCount) {
    if (!outCount) {
        return Invalid(mod, "graphics adapter count output is null");
    }
    const WotbModV3Result access = DeviceAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    std::call_once(g_adapterOnce, &EnumerateGraphicsAdapters);
    *outCount = static_cast<uint32_t>(g_adapters.size());
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL DeviceGetGraphicsAdapterAt(
    WotbModV3Handle mod,
    uint32_t index,
    WotbModV3GraphicsAdapterInfo* outInfo) {
    if (!ValidStruct(outInfo, WOTBMOD_V3_DEVICE_VERSION)) {
        return Invalid(
            mod,
            "graphics adapter structure header is invalid");
    }
    const WotbModV3Result access = DeviceAccess(mod);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    std::call_once(g_adapterOnce, &EnumerateGraphicsAdapters);
    if (index >= g_adapters.size()) {
        return NotFound(mod, "graphics adapter index is out of range");
    }
    const GraphicsAdapterSnapshot& snapshot = g_adapters[index];
    WotbModV3GraphicsAdapterInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_DEVICE_VERSION;
    info.index = index;
    info.vendor_id = snapshot.vendorId;
    info.device_id = snapshot.deviceId;
    info.subsystem_id = snapshot.subsystemId;
    info.revision = snapshot.revision;
    info.software_adapter = snapshot.software;
    info.dedicated_video_memory_bytes =
        snapshot.dedicatedVideoMemory;
    info.dedicated_system_memory_bytes =
        snapshot.dedicatedSystemMemory;
    info.shared_system_memory_bytes =
        snapshot.sharedSystemMemory;
    CopyFixed(info.name, snapshot.name.c_str());
    *outInfo = info;
    return WOTBMOD_V3_OK;
}

/* =====================================================================
 * Interfaces declared 2026-08-16: wotbmod.ui.read, wotbmod.camera.state,
 * wotbmod.audio.intercept, wotbmod.scene.enumerate, wotbmod.tracer.
 *
 * This is the PORTABLE half only. Nothing below dereferences a game
 * pointer, resolves an RVA or knows a struct offset. Every slot validates
 * its arguments, enforces its grant, its context and its thread, and then
 * either calls through ClientHostDeclaredBackend or answers
 * WOTBMOD_V3_E_NOT_SUPPORTED with a reason that says which of the two it
 * was. There is no path here that returns WOTBMOD_V3_OK without a backend
 * having actually performed the operation.
 * ===================================================================== */

constexpr uint64_t kUiReadContexts = WOTBMOD_V3_CONTEXT_ALL;
/*
 * The animation-state machine belongs to the battle/replay CameraController
 * reached from the AvatarContext. There is no such controller on a hangar
 * screen, so a hangar caller gets an honest WOTBMOD_V3_E_INCOMPATIBLE from
 * the context gate instead of a fabricated "state 5".
 */
constexpr uint64_t kCameraStateContexts =
    WOTBMOD_V3_CONTEXT_BATTLE |
    WOTBMOD_V3_CONTEXT_REPLAY |
    WOTBMOD_V3_CONTEXT_TRAINING;
constexpr uint64_t kAudioInterceptContexts = WOTBMOD_V3_CONTEXT_ALL;
/*
 * A scene can be active in the hangar as well as in battle, and the walk
 * answers WOTBMOD_V3_E_NOT_FOUND wherever one is not, so the context gate
 * would add nothing but a second, less precise refusal.
 */
constexpr uint64_t kSceneEnumerateContexts = WOTBMOD_V3_CONTEXT_ALL;
/*
 * The tracer table is immutable .rdata. Reading it in the hangar so a mod
 * can build its style map before a battle starts is strictly better than
 * forcing the lookup into the battle frame budget.
 */
constexpr uint64_t kTracerContexts = WOTBMOD_V3_CONTEXT_ALL;
constexpr uint64_t kGesContexts = WOTBMOD_V3_CONTEXT_ALL;

constexpr uint32_t kMaxSoundInterceptsPerMod = 32u;

/*
 * These five must stay byte-identical to kInterfaceAvailabilityDefaults in
 * wotb_mod_v3_runtime.cpp. Registration picks the default up from there;
 * these copies exist only so that REMOVING a backend restores exactly the
 * text a fresh process would have published, instead of the generic
 * "interface implementation is unavailable" fallback.
 */
const char* const kUiReadUnavailableReason =
    "UI text, texture, font and style readback has no backend; every slot returns NOT_SUPPORTED";
const char* const kCameraStateUnavailableReason =
    "camera animation-state and arcade/sniper view-mode observation has no backend; every slot returns NOT_SUPPORTED";
const char* const kAudioInterceptUnavailableReason =
    "semantic sound-event interception has no backend; every slot returns NOT_SUPPORTED";
const char* const kSceneEnumerateUnavailableReason =
    "game-owned scene enumeration has no backend; every slot returns NOT_SUPPORTED";
const char* const kTracerUnavailableReason =
    "stock tracer shell-type style lookup has no backend; every slot returns NOT_SUPPORTED";
const char* const kGesUnavailableReason =
    "GES event bus backend is not installed on this client build; every slot returns NOT_SUPPORTED";
const char* const kSessionClusterUnavailableReason =
    "login cluster switch backend is not installed on this client build; every slot returns NOT_SUPPORTED";

/*
 * DEGRADED, never AVAILABLE, even with a complete backend installed.
 * AVAILABLE would claim the whole declared surface works on this exact
 * client, and that claim needs a manual live PASS on the fingerprint, which
 * this layer cannot observe. Each reason names the part that is real and the
 * part that is structurally out of reach.
 */
const char* const kUiReadDegradedReason =
    "readback of the loader mirror of values this API committed is available; the engine's own live control text has no proven read path and is not represented";
const char* const kCameraStateDegradedReason =
    "read-only animation state and arcade/sniper view mode are available, each with its own validity flag; there is deliberately no setter because the engine transition policy lives outside SwitchState";
const char* const kAudioInterceptDegradedReason =
    "exact-name pass-through, substitute and suppress interception is available; prefix, wildcard and match-everything filters are deliberately not expressible";
const char* const kSceneEnumerateDegradedReason =
    "bounded main-thread breadth-first snapshots of the active scene are available; there is deliberately no cursor, handle, visitor or off-thread walk";
const char* const kTracerDegradedReason =
    "stock shell-type style names resolved from the client own table are available; the per-shell colour is reported only when the record was really read, and tracer creation and mutation are not represented";
const char* const kGesDegradedReason =
    "engine-native GES listener registration, delivery as wotbmod.ges.* system events and schema-gated publish are available; payload sizes and fields are known only for schema-registered types, and the interface stays LIVE_TEST_PENDING until the validation mod records a PASS on this fingerprint";

const char* const kSessionClusterDegradedReason =
    "enumerate, get_current and change are available once the client's LoginManager has been captured on its first login; CONNECTED/FAILED are judged by the hangar context returning, not by the client, and the interface stays LIVE_TEST_PENDING until the validation mod records a PASS on this fingerprint";

std::atomic<uint64_t> g_nextSubscriptionOrder{1u};
std::atomic<bool> g_soundInterceptDispatchEnabled{false};

/*
 * Mirrors HostBackendInvocationLease, against the declared table and its own
 * counter. Separate on purpose: SetClientHostDeclaredBackend must be able to
 * quiesce these five interfaces without waiting on whatever the generic
 * ClientHostBackend bridge is doing, and vice versa.
 */
class DeclaredBackendLease final {
public:
    DeclaredBackendLease() {
        std::lock_guard<std::mutex> lock(g_declaredBackendMutex);
        if (!g_declaredBackendInstalled ||
            !g_declaredBackendAcceptingInvocations) {
            return;
        }
        backend_ = g_declaredBackend;
        ++g_declaredBackendInvocationsInFlight;
        ++g_declaredBackendInvocationDepth;
        acquired_ = true;
    }

    ~DeclaredBackendLease() {
        if (!acquired_) {
            return;
        }
        if (g_declaredBackendInvocationDepth > 0u) {
            --g_declaredBackendInvocationDepth;
        }
        std::lock_guard<std::mutex> lock(g_declaredBackendMutex);
        if (g_declaredBackendInvocationsInFlight > 0u) {
            --g_declaredBackendInvocationsInFlight;
        }
        g_declaredBackendQuiesced.notify_all();
    }

    DeclaredBackendLease(const DeclaredBackendLease&) = delete;
    DeclaredBackendLease& operator=(
        const DeclaredBackendLease&) = delete;

    explicit operator bool() const {
        return acquired_;
    }

    const ClientHostDeclaredBackend& backend() const {
        return backend_;
    }

private:
    ClientHostDeclaredBackend backend_ = {};
    bool acquired_ = false;
};

WotbModV3Result DeclaredBackendMissing(
    WotbModV3Handle mod,
    const char* operation) {
    std::string message(operation ? operation : "native operation");
    message += ": native client backend is not installed";
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        message.c_str(),
        "{\"backend\":\"missing\"}");
}

WotbModV3Result DeclaredBackendFailure(
    WotbModV3Handle mod,
    const char* operation,
    WotbModV3Result result) {
    std::string message(operation ? operation : "native operation");
    switch (result) {
        case WOTBMOD_V3_E_NOT_SUPPORTED:
            message +=
                ": the native backend does not implement this on this client";
            break;
        case WOTBMOD_V3_E_NOT_FOUND:
            message += ": the native backend has nothing to report right now";
            break;
        case WOTBMOD_V3_E_WRONG_THREAD:
            message += ": the native backend refused this thread";
            break;
        case WOTBMOD_V3_E_CALLBACK_FAULT:
            message += ": the native backend raised an exception";
            break;
        case WOTBMOD_V3_E_PLATFORM:
            message += ": the native backend returned a malformed answer";
            break;
        default:
            message += ": the native backend refused the request";
            break;
    }
    return SetError(mod, result, message.c_str());
}

WotbModV3Result DeclaredBackendMalformed(
    WotbModV3Handle mod,
    const char* operation,
    const char* detail) {
    std::string message(operation ? operation : "native operation");
    message += ": ";
    message += detail ? detail : "the native backend returned a malformed answer";
    return SetError(
        mod,
        WOTBMOD_V3_E_PLATFORM,
        message.c_str(),
        "{\"backend\":\"malformed\"}");
}

void PublishDeclaredInterfaceAvailability() {
    ClientHostDeclaredBackend backend = {};
    bool installed = false;
    {
        std::lock_guard<std::mutex> lock(g_declaredBackendMutex);
        installed = g_declaredBackendInstalled;
        if (installed) {
            backend = g_declaredBackend;
        }
    }
    struct AvailabilityEntry {
        const char* name;
        bool present;
        const char* degradedReason;
        const char* unavailableReason;
    };
    const AvailabilityEntry entries[] = {
        {WOTBMOD_V3_IFACE_UI_READ,
         installed && backend.ui_read_string && backend.ui_read_style,
         kUiReadDegradedReason,
         kUiReadUnavailableReason},
        {WOTBMOD_V3_IFACE_CAMERA_STATE,
         installed && backend.camera_read_observed_state != nullptr,
         kCameraStateDegradedReason,
         kCameraStateUnavailableReason},
        {WOTBMOD_V3_IFACE_AUDIO_INTERCEPT,
         installed && backend.audio_intercept_publish_name &&
             backend.audio_intercept_disarm,
         kAudioInterceptDegradedReason,
         kAudioInterceptUnavailableReason},
        {WOTBMOD_V3_IFACE_SCENE_ENUMERATE,
         installed && backend.scene_get_walk_limits &&
             backend.scene_walk_active,
         kSceneEnumerateDegradedReason,
         kSceneEnumerateUnavailableReason},
        {WOTBMOD_V3_IFACE_TRACER,
         installed && backend.tracer_get_shell_type_count &&
             backend.tracer_get_style,
         kTracerDegradedReason,
         kTracerUnavailableReason},
        {WOTBMOD_V3_IFACE_GES,
         installed && backend.ges_list_types && backend.ges_observe &&
             backend.ges_publish,
         kGesDegradedReason,
         kGesUnavailableReason},
        {WOTBMOD_V3_IFACE_SESSION_CLUSTER,
         installed && backend.session_cluster_enumerate &&
             backend.session_cluster_get_current &&
             backend.session_cluster_change &&
             backend.session_cluster_set_manual,
         kSessionClusterDegradedReason,
         kSessionClusterUnavailableReason}};
    for (const AvailabilityEntry& entry : entries) {
        /*
         * E_NOT_FOUND here simply means the interface has not been
         * registered yet, which is the normal state when a backend is
         * installed before RegisterClientServices runs. Registration then
         * picks up the frozen UNAVAILABLE default and the next publication
         * corrects it.
         */
        SetInterfaceAvailability(
            entry.name,
            entry.present
                ? WOTBMOD_V3_CAPABILITY_DEGRADED
                : WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
            entry.present
                ? entry.degradedReason
                : entry.unavailableReason);
    }
}

/*
 * Quiesce, disarm, drop. Called by SetClientHostDeclaredBackend on every
 * install and removal, and by ClientServicesShutdown, always under
 * g_declaredBackendTransitionMutex so two of them cannot interleave.
 *
 * The order matters and is not arbitrary:
 *   1. stop accepting new backend calls and wait for the in-flight ones,
 *      so no thread is still inside the loader when the table is dropped;
 *   2. clear the dispatch gate, which the sound detour reads through an
 *      atomic and never through g_declaredBackendMutex -- so a detour can
 *      never be blocked on the lock this teardown is holding;
 *   3. only then disarm, which by contract returns after the last detour
 *      invocation has left;
 *   4. drop the table.
 */
void UninstallDeclaredBackend() {
    ClientHostDeclaredBackend previous = {};
    bool armed = false;
    {
        std::unique_lock<std::mutex> lock(g_declaredBackendMutex);
        g_declaredBackendAcceptingInvocations = false;
        g_declaredBackendQuiesced.wait(
            lock,
            []() {
                return g_declaredBackendInvocationsInFlight == 0u;
            });
        previous = g_declaredBackend;
        armed = g_soundInterceptArmed;
    }
    g_soundInterceptDispatchEnabled.store(
        false,
        std::memory_order_release);
    if (armed && previous.audio_intercept_disarm) {
        try {
            previous.audio_intercept_disarm(previous.user_data);
        } catch (...) {
            RuntimeLog(
                WOTBMOD_V3_LOG_ERROR,
                "v3.client-services",
                "native sound interception disarm raised an exception");
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_declaredBackendMutex);
        g_declaredBackend = {};
        g_declaredBackendInstalled = false;
        g_declaredBackendAcceptingInvocations = false;
        g_soundInterceptArmed = false;
    }
}

/*
 * The mod's interception callback runs on an arbitrary engine thread, inside
 * the sound-creation path, with the engine's own state half-built. A fault
 * there must not unwind into the engine, and it must not unwind through this
 * runtime either: under /EHsc MSVC runs no C++ destructor while unwinding to
 * an __except, so a fault crossing the dispatch loop would abandon the
 * subscription's dispatch lock and the depth counter for good. The handler
 * therefore sits in its own frame, immediately around the call, and that
 * frame deliberately owns nothing with a destructor (which is also why it
 * cannot be folded into the loop: /EHsc forbids __try in a function that
 * owns unwindable objects, C2712).
 */
#if defined(_MSC_VER)
bool CallSoundInterceptCallbackProtected(
    WotbModV3SoundInterceptCallback callback,
    WotbModV3Handle owner,
    const WotbModV3SoundInterceptRequest* request,
    WotbModV3SoundInterceptResponse* response,
    void* userData) {
    __try {
        callback(owner, request, response, userData);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#else
bool CallSoundInterceptCallbackProtected(
    WotbModV3SoundInterceptCallback callback,
    WotbModV3Handle owner,
    const WotbModV3SoundInterceptRequest* request,
    WotbModV3SoundInterceptResponse* response,
    void* userData) {
    try {
        callback(owner, request, response, userData);
        return true;
    } catch (...) {
        return false;
    }
}
#endif

class SoundInterceptDepthGuard final {
public:
    SoundInterceptDepthGuard() {
        ++g_soundInterceptDepth;
    }

    ~SoundInterceptDepthGuard() {
        if (g_soundInterceptDepth > 0u) {
            --g_soundInterceptDepth;
        }
    }

    SoundInterceptDepthGuard(const SoundInterceptDepthGuard&) = delete;
    SoundInterceptDepthGuard& operator=(
        const SoundInterceptDepthGuard&) = delete;
};

/* ------------------------------------------------------- wotbmod.ui.read */

WotbModV3Result CallDeclaredUiReadString(
    const DeclaredBackendLease& lease,
    const ClientHostUiReadRequest& request,
    ClientHostUiReadString* value) {
    try {
        return lease.backend().ui_read_string(
            lease.backend().user_data,
            &request,
            value);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

WotbModV3Result CallDeclaredUiReadStyle(
    const DeclaredBackendLease& lease,
    const ClientHostUiReadRequest& request,
    WotbModV3UiStyleSnapshot* style) {
    try {
        return lease.backend().ui_read_style(
            lease.backend().user_data,
            &request,
            style);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

/*
 * Resolves the control, applies the same own/game-owned permission split
 * every other UI slot uses, and builds the backend request. The runtime is
 * the authority on gameOwned and stamps it into the request AND back into
 * the answer, so a backend can never lose the staleness warning.
 */
WotbModV3Result BeginUiRead(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t field,
    ClientHostUiReadRequest* outRequest,
    bool* outGameOwned) {
    UiControl* control = nullptr;
    const WotbModV3Result result = GetUiControl(mod, handle, &control);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    uint64_t nativeObject = 0u;
    bool gameOwned = false;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        nativeObject = control->nativeObject;
        gameOwned = control->gameOwned;
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        gameOwned
            ? WOTBMOD_V3_PERMISSION_REVIEWED
            : WOTBMOD_V3_PERMISSION_SAFE,
        kUiReadContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    if (nativeObject == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "UI control no longer has a native object to read back");
    }
    *outRequest = {};
    outRequest->struct_size = sizeof(*outRequest);
    outRequest->api_version =
        WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
    outRequest->object = nativeObject;
    outRequest->mod = mod;
    outRequest->field = field;
    outRequest->game_owned = gameOwned ? 1u : 0u;
    *outGameOwned = gameOwned;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UiReadString(
    WotbModV3Handle mod,
    WotbModV3UiHandle handle,
    uint32_t field,
    uint32_t setFlag,
    size_t publicLimit,
    const char* operation,
    char* buffer,
    uint32_t* inoutSize) {
    if (!inoutSize) {
        return Invalid(mod, "UI read size pointer is null");
    }
    ClientHostUiReadRequest request = {};
    bool gameOwned = false;
    WotbModV3Result result =
        BeginUiRead(mod, handle, field, &request, &gameOwned);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().ui_read_string) {
        return DeclaredBackendMissing(mod, operation);
    }
    ClientHostUiReadString value = {};
    value.struct_size = sizeof(value);
    value.api_version =
        WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
    result = CallDeclaredUiReadString(lease, request, &value);
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(mod, operation, result);
    }
    if (value.length >= sizeof(value.value) ||
        value.length >= publicLimit ||
        value.value[value.length] != '\0' ||
        std::memchr(value.value, '\0', value.length) != nullptr) {
        return DeclaredBackendMalformed(
            mod,
            operation,
            "mirrored string is unterminated or longer than the ABI allows");
    }
    if ((value.flags & setFlag) == 0u) {
        /*
         * ui_v4.h makes "never written" a first-class answer. Reporting it
         * as an empty string would be indistinguishable from a control this
         * API deliberately blanked, which is exactly the distinction the
         * _SET flags exist to preserve.
         */
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_FOUND,
            field == CLIENT_HOST_UI_READ_FIELD_LIVE_TEXT
                ? "the control has no text component"
                : "this API has not written that field on this control",
            gameOwned
                ? "{\"mirror\":\"unset\",\"game_owned\":true}"
                : "{\"mirror\":\"unset\",\"game_owned\":false}");
    }
    std::string mirrored;
    try {
        mirrored.assign(value.value, value.length);
    } catch (...) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "UI readback allocation failed");
    }
    return CopyString(mod, mirrored, buffer, inoutSize);
}

WotbModV3Result WOTBMOD_V3_CALL UiReadGetText(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    char* buffer,
    uint32_t* inoutSize) {
    return UiReadString(
        mod,
        control,
        CLIENT_HOST_UI_READ_FIELD_TEXT,
        WOTBMOD_V3_UI_READ_TEXT_SET,
        WOTBMOD_V3_MAX_MESSAGE,
        "ui_control_get_text",
        buffer,
        inoutSize);
}

WotbModV3Result WOTBMOD_V3_CALL UiReadGetLiveText(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    char* buffer,
    uint32_t* inoutSize) {
    return UiReadString(
        mod,
        control,
        CLIENT_HOST_UI_READ_FIELD_LIVE_TEXT,
        WOTBMOD_V3_UI_READ_LIVE_TEXT_SET,
        WOTBMOD_V3_MAX_MESSAGE,
        "ui_control_get_live_text",
        buffer,
        inoutSize);
}

WotbModV3Result WOTBMOD_V3_CALL UiReadGetTexture(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    char* buffer,
    uint32_t* inoutSize) {
    return UiReadString(
        mod,
        control,
        CLIENT_HOST_UI_READ_FIELD_TEXTURE,
        WOTBMOD_V3_UI_READ_TEXTURE_SET,
        WOTBMOD_V3_MAX_PATH,
        "ui_control_get_texture",
        buffer,
        inoutSize);
}

WotbModV3Result WOTBMOD_V3_CALL UiReadGetFont(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    char* buffer,
    uint32_t* inoutSize) {
    return UiReadString(
        mod,
        control,
        CLIENT_HOST_UI_READ_FIELD_FONT,
        WOTBMOD_V3_UI_READ_FONT_SET,
        WOTBMOD_V3_MAX_PATH,
        "ui_control_get_font",
        buffer,
        inoutSize);
}

WotbModV3Result WOTBMOD_V3_CALL UiReadGetStyle(
    WotbModV3Handle mod,
    WotbModV3UiHandle control,
    WotbModV3UiStyleSnapshot* outStyle) {
    if (!ValidStruct(outStyle, WOTBMOD_V3_UI_VERSION_4)) {
        return Invalid(mod, "UI style snapshot output is invalid");
    }
    ClientHostUiReadRequest request = {};
    bool gameOwned = false;
    WotbModV3Result result =
        BeginUiRead(mod, control, 0u, &request, &gameOwned);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().ui_read_style) {
        return DeclaredBackendMissing(mod, "ui_control_get_style");
    }
    WotbModV3UiStyleSnapshot style = {};
    style.struct_size = sizeof(style);
    style.api_version = WOTBMOD_V3_UI_VERSION_4;
    result = CallDeclaredUiReadStyle(lease, request, &style);
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(
            mod,
            "ui_control_get_style",
            result);
    }
    constexpr uint32_t knownFlags =
        WOTBMOD_V3_UI_READ_GAME_OWNED |
        WOTBMOD_V3_UI_READ_TEXT_SET |
        WOTBMOD_V3_UI_READ_TEXTURE_SET |
        WOTBMOD_V3_UI_READ_FONT_SET |
        WOTBMOD_V3_UI_READ_COLOR_SET |
        WOTBMOD_V3_UI_READ_BACKGROUND_COLOR_SET |
        WOTBMOD_V3_UI_READ_OPACITY_SET |
        WOTBMOD_V3_UI_READ_FONT_SIZE_SET |
        WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET;
    if ((style.flags & ~knownFlags) != 0u ||
        !IsFinite(style.opacity) || !IsFinite(style.font_size) ||
        !IsFinite(style.color) || !IsFinite(style.background_color) ||
        style.text_alignment > WOTBMOD_V3_UI_TEXT_ALIGN_JUSTIFY ||
        style.text_wrap > 1u || style.rich_text > 1u) {
        return DeclaredBackendMalformed(
            mod,
            "ui_control_get_style",
            "style snapshot carries unknown flags or non-finite values");
    }
    style.struct_size = sizeof(style);
    style.api_version = WOTBMOD_V3_UI_VERSION_4;
    /*
     * The runtime, not the backend, is the authority on this bit: the
     * gameOwned flag lives in the handle record this layer owns, and a
     * caller that loses it would read a stale mirror as if it were the
     * engine's truth.
     */
    if (gameOwned) {
        style.flags |= WOTBMOD_V3_UI_READ_GAME_OWNED;
    } else {
        style.flags &= ~static_cast<uint32_t>(
            WOTBMOD_V3_UI_READ_GAME_OWNED);
    }
    *outStyle = style;
    return WOTBMOD_V3_OK;
}

/* -------------------------------------------------- wotbmod.camera.state */

uint32_t NormalizeCameraAnimationState(uint32_t raw) {
    switch (raw) {
        case WOTBMOD_V3_CAMERA_ANIMATION_ORDINARY_A:
        case WOTBMOD_V3_CAMERA_ANIMATION_ORDINARY_B:
        case WOTBMOD_V3_CAMERA_ANIMATION_LOOK_OUT:
        case WOTBMOD_V3_CAMERA_ANIMATION_POST_MORTEM:
        case WOTBMOD_V3_CAMERA_ANIMATION_LOOK_ON_TARGET:
        case WOTBMOD_V3_CAMERA_ANIMATION_NONE:
        case WOTBMOD_V3_CAMERA_ANIMATION_OBSERVER:
            return raw;
        default:
            return WOTBMOD_V3_CAMERA_ANIMATION_UNKNOWN;
    }
}

uint32_t NormalizeCameraViewMode(uint32_t raw) {
    switch (raw) {
        /* 0 = SNIPER. The pre-2026-08-15 mapping was INVERTED. */
        case WOTBMOD_V3_CAMERA_VIEW_SNIPER:
        case WOTBMOD_V3_CAMERA_VIEW_ARCADE:
            return raw;
        default:
            return WOTBMOD_V3_CAMERA_VIEW_UNKNOWN;
    }
}

WotbModV3Result CallDeclaredCameraState(
    const DeclaredBackendLease& lease,
    WotbModV3CameraObservedState* state) {
    try {
        return lease.backend().camera_read_observed_state(
            lease.backend().user_data,
            state);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

/*
 * All three public slots run through this one backend call, so a backend
 * physically cannot answer get_animation_state and get_observed_state
 * differently, and cannot invent the half it failed to read.
 */
WotbModV3Result ReadCameraObservedState(
    WotbModV3Handle mod,
    const char* operation,
    WotbModV3CameraObservedState* outState) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "camera.battle.read",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kCameraStateContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().camera_read_observed_state) {
        return DeclaredBackendMissing(mod, operation);
    }
    WotbModV3CameraObservedState state = {};
    state.struct_size = sizeof(state);
    state.api_version = WOTBMOD_V3_CAMERA_VERSION_2;
    const WotbModV3Result result =
        CallDeclaredCameraState(lease, &state);
    if (result == WOTBMOD_V3_E_NOT_FOUND) {
        /*
         * camera_v2.h mandates NOT_SUPPORTED when neither half can be read,
         * so the precision lives in the message rather than the code.
         */
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "no camera controller is resolvable right now",
            "{\"camera\":\"unresolved\"}");
    }
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(mod, operation, result);
    }
    state.struct_size = sizeof(state);
    state.api_version = WOTBMOD_V3_CAMERA_VERSION_2;
    state.animation_state_valid =
        state.animation_state_valid != 0u ? 1u : 0u;
    state.view_mode_valid = state.view_mode_valid != 0u ? 1u : 0u;
    /*
     * "Read it and cannot name it" is a different fact from "could not read
     * it", so an unnamed raw value keeps its validity flag and becomes
     * UNKNOWN; only the backend clears a validity flag.
     */
    state.animation_state = state.animation_state_valid != 0u
        ? NormalizeCameraAnimationState(state.animation_state)
        : 0u;
    state.view_mode = state.view_mode_valid != 0u
        ? NormalizeCameraViewMode(state.view_mode)
        : 0u;
    if (state.animation_state_valid == 0u &&
        state.view_mode_valid == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "neither camera value could be read",
            "{\"camera\":\"unresolved\"}");
    }
    *outState = state;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraStateGetAnimationState(
    WotbModV3Handle mod,
    uint32_t* outState) {
    if (!outState) {
        return Invalid(mod, "camera animation state output is null");
    }
    WotbModV3CameraObservedState state = {};
    const WotbModV3Result result = ReadCameraObservedState(
        mod,
        "camera_get_animation_state",
        &state);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (state.animation_state_valid == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "the camera animation state could not be read",
            "{\"camera\":\"animation_state_unavailable\"}");
    }
    *outState = state.animation_state;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraStateGetViewMode(
    WotbModV3Handle mod,
    uint32_t* outMode) {
    if (!outMode) {
        return Invalid(mod, "camera view mode output is null");
    }
    WotbModV3CameraObservedState state = {};
    const WotbModV3Result result = ReadCameraObservedState(
        mod,
        "camera_get_view_mode",
        &state);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (state.view_mode_valid == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "the camera view mode could not be read",
            "{\"camera\":\"view_mode_unavailable\"}");
    }
    *outMode = state.view_mode;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL CameraStateGetObserved(
    WotbModV3Handle mod,
    WotbModV3CameraObservedState* outState) {
    if (!ValidStruct(outState, WOTBMOD_V3_CAMERA_VERSION_2)) {
        return Invalid(mod, "camera observed state output is invalid");
    }
    WotbModV3CameraObservedState state = {};
    const WotbModV3Result result = ReadCameraObservedState(
        mod,
        "camera_get_observed_state",
        &state);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    *outState = state;
    return WOTBMOD_V3_OK;
}

/* ----------------------------------------------- wotbmod.audio.intercept */

WotbModV3Result CallDeclaredInterceptPublish(
    const DeclaredBackendLease& lease,
    const char* eventName) {
    try {
        return lease.backend().audio_intercept_publish_name(
            lease.backend().user_data,
            eventName);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

WotbModV3Result WOTBMOD_V3_CALL AudioInterceptRegister(
    WotbModV3Handle mod,
    const char* eventName,
    int32_t priority,
    WotbModV3SoundInterceptCallback callback,
    void* userData,
    WotbModV3Token* outToken) {
    const WotbModV3Result permission = CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidText(eventName, WOTBMOD_V3_MAX_NAME, false) || !callback ||
        !outToken) {
        return Invalid(mod, "invalid sound interception registration");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        kAudioInterceptContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        uint32_t owned = 0u;
        for (const auto& pair : g_subscriptions) {
            const Subscription* subscription = pair.second.get();
            if (subscription &&
                subscription->kind == SubscriptionKind::AudioIntercept &&
                subscription->owner == mod) {
                ++owned;
            }
        }
        if (owned >= kMaxSoundInterceptsPerMod) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "sound interception limit reached for this mod");
        }
    }
    uint64_t publicationEpoch = 0u;
    WotbModV3Result result =
        BeginSubscriptionCreation(mod, &publicationEpoch);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().audio_intercept_publish_name ||
        !lease.backend().audio_intercept_disarm) {
        return DeclaredBackendMissing(mod, "sound_intercept_register");
    }
    /*
     * The name is published and the detour armed BEFORE the callback exists,
     * never after: a detour that is live while the match set is still being
     * built would have to read a mutable set from an arbitrary thread. If
     * this succeeds but the subscription below fails, the surviving state is
     * a published name with no listener, which costs one strcmp and one
     * dispatch that answers NOT_FOUND -- the sound plays exactly as stock.
     */
    result = CallDeclaredInterceptPublish(lease, eventName);
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(
            mod,
            "sound_intercept_register",
            result);
    }
    {
        std::lock_guard<std::mutex> lock(g_declaredBackendMutex);
        g_soundInterceptArmed = true;
    }
    g_soundInterceptDispatchEnabled.store(
        true,
        std::memory_order_release);
    Subscription* subscription = new (std::nothrow) Subscription();
    if (!subscription) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "sound interception allocation failed");
    }
    try {
        subscription->property = eventName;
    } catch (...) {
        delete subscription;
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "sound interception allocation failed");
    }
    subscription->kind = SubscriptionKind::AudioIntercept;
    subscription->publicationEpoch = publicationEpoch;
    subscription->priority = priority;
    subscription->soundInterceptCallback = callback;
    subscription->userData = userData;
    subscription->creationOrder = g_nextSubscriptionOrder.fetch_add(
        1u,
        std::memory_order_relaxed);
    return CreateSubscription(mod, subscription, outToken);
}

WotbModV3Result WOTBMOD_V3_CALL AudioInterceptUnregister(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    /*
     * The bare grant, not CheckAudioEventsPermission: a mod must be able to
     * withdraw an interception from anywhere, including a thread that is
     * currently inside one of its own interception callbacks. Unregistering
     * quiesces the callback, which is safe from another thread and is a
     * no-op deadlock-wise on this one because the lease is recursive.
     */
    const WotbModV3Result permission = CheckAudioEventsGrant(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    const WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::AudioIntercept,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    return UnregisterSubscription(
        mod,
        token,
        subscription,
        WOTBMOD_V3_PERMISSION_SAFE,
        kAudioInterceptContexts,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL AudioInterceptSetPriority(
    WotbModV3Handle mod,
    WotbModV3Token token,
    int32_t priority) {
    const WotbModV3Result permission = CheckAudioEventsPermission(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    std::shared_ptr<Subscription> subscription;
    WotbModV3Result result = GetSubscription(
        mod,
        token,
        SubscriptionKind::AudioIntercept,
        &subscription);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    std::unique_lock<std::recursive_mutex> operationLock(
        subscription->operationMutex);
    if (subscription->destroying || subscription->unregistered ||
        subscription->unregistering) {
        return SetError(
            mod,
            WOTBMOD_V3_E_OBJECT_DESTROYED,
            "sound interception is no longer active");
    }
    /*
     * Priority ordering is entirely runtime-side -- the detour only decides
     * whether a name is wanted at all -- so there is nothing to tell the
     * backend and no host round-trip to fail halfway through.
     */
    std::lock_guard<std::mutex> lock(g_clientMutex);
    subscription->priority = priority;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AudioInterceptIsActive(
    WotbModV3Handle mod,
    uint32_t* outActive) {
    /*
     * Deliberately the bare grant: this slot exists to report that the
     * calling thread is inside an interception callback, so applying the
     * re-entry guard to it would make it answer BUSY exactly when its answer
     * matters.
     */
    const WotbModV3Result permission = CheckAudioEventsGrant(mod);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outActive) {
        return Invalid(mod, "sound interception state output is null");
    }
    *outActive = g_soundInterceptDepth != 0u ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

/* ---------------------------------------------- wotbmod.scene.enumerate */

WotbModV3Result CallDeclaredSceneLimits(
    const DeclaredBackendLease& lease,
    WotbModV3SceneWalkLimits* limits) {
    try {
        return lease.backend().scene_get_walk_limits(
            lease.backend().user_data,
            limits);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

WotbModV3Result CallDeclaredSceneWalk(
    const DeclaredBackendLease& lease,
    const WotbModV3SceneWalkRequest& request,
    WotbModV3SceneNodeRecord* nodes,
    uint32_t capacity,
    uint32_t* reached) {
    try {
        return lease.backend().scene_walk_active(
            lease.backend().user_data,
            &request,
            nodes,
            capacity,
            reached);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

uint32_t ClampWalkLimit(uint32_t reported, uint32_t ceiling) {
    if (reported == 0u || reported > ceiling) {
        return ceiling;
    }
    return reported;
}

/*
 * Effective caps: the contract ceilings from scene_v2.h, lowered by whatever
 * the provider says it will really enforce. A provider that reports 0 or
 * something larger than the ceiling is treated as "no opinion" and gets the
 * ceiling, because the ceiling is the number the ABI froze.
 */
WotbModV3Result ResolveSceneWalkLimits(
    WotbModV3Handle mod,
    const DeclaredBackendLease& lease,
    const char* operation,
    WotbModV3SceneWalkLimits* outLimits) {
    WotbModV3SceneWalkLimits limits = {};
    limits.struct_size = sizeof(limits);
    limits.api_version = WOTBMOD_V3_SCENE_VERSION_2;
    const WotbModV3Result result =
        CallDeclaredSceneLimits(lease, &limits);
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(mod, operation, result);
    }
    if (limits.node_record_size !=
        sizeof(WotbModV3SceneNodeRecord)) {
        return DeclaredBackendMalformed(
            mod,
            operation,
            "provider node record size does not match this runtime");
    }
    outLimits->struct_size = sizeof(*outLimits);
    outLimits->api_version = WOTBMOD_V3_SCENE_VERSION_2;
    outLimits->max_depth = ClampWalkLimit(
        limits.max_depth,
        WOTBMOD_V3_SCENE_WALK_MAX_DEPTH);
    outLimits->max_children_per_node = ClampWalkLimit(
        limits.max_children_per_node,
        WOTBMOD_V3_SCENE_WALK_MAX_CHILDREN);
    outLimits->max_nodes = ClampWalkLimit(
        limits.max_nodes,
        WOTBMOD_V3_SCENE_WALK_MAX_NODES);
    outLimits->node_record_size =
        static_cast<uint32_t>(sizeof(WotbModV3SceneNodeRecord));
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SceneEnumerateGetLimits(
    WotbModV3Handle mod,
    WotbModV3SceneWalkLimits* outLimits) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "game.entity.public",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(outLimits, WOTBMOD_V3_SCENE_VERSION_2)) {
        return Invalid(mod, "scene walk limits output is invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kSceneEnumerateContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().scene_get_walk_limits ||
        !lease.backend().scene_walk_active) {
        return DeclaredBackendMissing(mod, "scene_get_walk_limits");
    }
    WotbModV3SceneWalkLimits limits = {};
    const WotbModV3Result result = ResolveSceneWalkLimits(
        mod,
        lease,
        "scene_get_walk_limits",
        &limits);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    *outLimits = limits;
    return WOTBMOD_V3_OK;
}

bool ValidSceneNodeRecord(
    const WotbModV3SceneNodeRecord* records,
    uint32_t index,
    uint32_t maxDepth,
    uint32_t maxChildren) {
    const WotbModV3SceneNodeRecord& record = records[index];
    constexpr uint32_t knownFlags =
        WOTBMOD_V3_SCENE_NODE_IS_SCENE |
        WOTBMOD_V3_SCENE_NODE_HAS_WORLD_MATRIX |
        WOTBMOD_V3_SCENE_NODE_NAME_TRUNCATED |
        WOTBMOD_V3_SCENE_NODE_CHILDREN_TRUNCATED |
        WOTBMOD_V3_SCENE_NODE_DEPTH_LIMITED;
    if (record.struct_size < sizeof(record) ||
        record.api_version != WOTBMOD_V3_SCENE_VERSION_2 ||
        record.index != index ||
        (record.flags & ~knownFlags) != 0u ||
        record.depth > maxDepth ||
        std::memchr(record.name, '\0', sizeof(record.name)) == nullptr) {
        return false;
    }
    if ((record.flags & WOTBMOD_V3_SCENE_NODE_CHILDREN_TRUNCATED) == 0u &&
        record.child_count > maxChildren) {
        return false;
    }
    if (index == 0u) {
        if (record.parent_index != WOTBMOD_V3_SCENE_NODE_NO_PARENT ||
            record.depth != 0u) {
            return false;
        }
    } else {
        /*
         * Breadth-first is mandated by the frozen header, so a parent always
         * occupies a lower index than its child. Checking it here means a
         * mod that walks parent_index can never be sent off the front of its
         * own buffer or into a cycle.
         */
        if (record.parent_index >= index ||
            record.depth != records[record.parent_index].depth + 1u) {
            return false;
        }
    }
    for (size_t component = 0u;
         component < sizeof(record.world_matrix.values) /
             sizeof(record.world_matrix.values[0]);
         ++component) {
        if (!IsFinite(record.world_matrix.values[component])) {
            return false;
        }
    }
    return true;
}

WotbModV3Result WOTBMOD_V3_CALL SceneEnumerateWalkActive(
    WotbModV3Handle mod,
    const WotbModV3SceneWalkRequest* request,
    WotbModV3SceneNodeRecord* outNodes,
    uint32_t* inoutCount) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "game.entity.public",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(request, WOTBMOD_V3_SCENE_VERSION_2) || !inoutCount) {
        return Invalid(mod, "scene walk request or count is invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kSceneEnumerateContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    /*
     * Enforced HERE, before a single engine pointer can be reached, and
     * again inside the backend. Entity::AddNode and Entity::RemoveNode
     * memmove the child vector with no lock and RemoveNode releases the
     * child immediately after compacting, so an off-thread walker can hold a
     * pointer that the next instruction frees. There is no lock and no
     * version counter that could detect it; refusing the thread is the only
     * defence that exists.
     */
    if (CurrentThreadRole() != WOTBMOD_V3_THREAD_MAIN) {
        return SetError(
            mod,
            WOTBMOD_V3_E_WRONG_THREAD,
            "the active scene may only be walked on the main thread",
            "{\"thread\":\"main_required\"}");
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().scene_get_walk_limits ||
        !lease.backend().scene_walk_active) {
        return DeclaredBackendMissing(mod, "scene_walk_active_scene");
    }
    WotbModV3SceneWalkLimits limits = {};
    WotbModV3Result result = ResolveSceneWalkLimits(
        mod,
        lease,
        "scene_walk_active_scene",
        &limits);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    WotbModV3SceneWalkRequest effective = {};
    effective.struct_size = sizeof(effective);
    effective.api_version = WOTBMOD_V3_SCENE_VERSION_2;
    effective.max_depth =
        ClampWalkLimit(request->max_depth, limits.max_depth);
    effective.max_children_per_node = ClampWalkLimit(
        request->max_children_per_node,
        limits.max_children_per_node);
    effective.max_nodes =
        ClampWalkLimit(request->max_nodes, limits.max_nodes);
    const uint32_t requestedCapacity = outNodes ? *inoutCount : 0u;
    const uint32_t stagingCapacity =
        std::min(requestedCapacity, effective.max_nodes);
    /*
     * The backend never sees the mod's buffer. It fills runtime staging that
     * this layer sized itself, every record is validated, and only then is
     * anything copied out -- so a backend bug cannot become a heap overflow
     * in mod memory, and a malformed record cannot reach a mod at all.
     */
    std::vector<WotbModV3SceneNodeRecord> staging;
    if (stagingCapacity != 0u) {
        try {
            staging.resize(stagingCapacity);
        } catch (...) {
            return SetError(
                mod,
                WOTBMOD_V3_E_LIMIT_REACHED,
                "scene walk staging allocation failed");
        }
    }
    uint32_t reached = 0u;
    result = CallDeclaredSceneWalk(
        lease,
        effective,
        staging.empty() ? nullptr : staging.data(),
        stagingCapacity,
        &reached);
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(
            mod,
            "scene_walk_active_scene",
            result);
    }
    if (reached > effective.max_nodes) {
        return DeclaredBackendMalformed(
            mod,
            "scene_walk_active_scene",
            "walk reported more nodes than the cap it was given");
    }
    const uint32_t written = std::min(reached, stagingCapacity);
    for (uint32_t index = 0u; index < written; ++index) {
        if (!ValidSceneNodeRecord(
                staging.data(),
                index,
                effective.max_depth,
                effective.max_children_per_node)) {
            return DeclaredBackendMalformed(
                mod,
                "scene_walk_active_scene",
                "walk produced a malformed node record");
        }
    }
    if (outNodes && written != 0u) {
        std::memcpy(
            outNodes,
            staging.data(),
            static_cast<size_t>(written) *
                sizeof(WotbModV3SceneNodeRecord));
    }
    *inoutCount = reached;
    if (!outNodes) {
        /* Counting pass: the caller asked how big a buffer it needs. */
        return WOTBMOD_V3_OK;
    }
    if (reached > requestedCapacity) {
        return SetError(
            mod,
            WOTBMOD_V3_E_BUFFER_TOO_SMALL,
            "scene walk buffer is smaller than the walk reached");
    }
    return WOTBMOD_V3_OK;
}

/* ------------------------------------------------------- wotbmod.tracer */

WotbModV3Result CallDeclaredTracerCount(
    const DeclaredBackendLease& lease,
    uint32_t* count) {
    try {
        return lease.backend().tracer_get_shell_type_count(
            lease.backend().user_data,
            count);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

WotbModV3Result CallDeclaredTracerStyle(
    const DeclaredBackendLease& lease,
    uint32_t shellType,
    WotbModV3TracerStyle* style) {
    try {
        return lease.backend().tracer_get_style(
            lease.backend().user_data,
            shellType,
            style);
    } catch (...) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
}

WotbModV3Result ResolveTracerShellTypeCount(
    WotbModV3Handle mod,
    const DeclaredBackendLease& lease,
    const char* operation,
    uint32_t* outCount) {
    uint32_t count = 0u;
    const WotbModV3Result result =
        CallDeclaredTracerCount(lease, &count);
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(mod, operation, result);
    }
    if (count == 0u) {
        return DeclaredBackendMalformed(
            mod,
            operation,
            "shell-type table is empty");
    }
    /*
     * The resolver gates on cmp al,0x19 / jnb error, so 25 is the exact
     * bound the image itself enforces. A backend reporting more is clamped
     * rather than trusted: a mod looping to the reported count would
     * otherwise walk past the end of the table.
     */
    *outCount =
        std::min(count, WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL TracerGetShellTypeCount(
    WotbModV3Handle mod,
    uint32_t* outCount) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "visible.projectile.events",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!outCount) {
        return Invalid(mod, "tracer shell type count output is null");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kTracerContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().tracer_get_shell_type_count ||
        !lease.backend().tracer_get_style) {
        return DeclaredBackendMissing(
            mod,
            "tracer_get_shell_type_count");
    }
    uint32_t count = 0u;
    const WotbModV3Result result = ResolveTracerShellTypeCount(
        mod,
        lease,
        "tracer_get_shell_type_count",
        &count);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    *outCount = count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL TracerGetStyleForShellType(
    WotbModV3Handle mod,
    uint32_t shellType,
    WotbModV3TracerStyle* outStyle) {
    const WotbModV3Result permission = CheckNamedPermission(
        mod,
        "visible.projectile.events",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) {
        return permission;
    }
    if (!ValidStruct(outStyle, WOTBMOD_V3_TRACER_VERSION)) {
        return Invalid(mod, "tracer style output is invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        kTracerContexts,
        nullptr);
    if (access != WOTBMOD_V3_OK) {
        return access;
    }
    /*
     * Out of range is an argument error and never a clamp to the last entry.
     * The frozen ceiling is checked before the backend is even consulted so
     * that the answer does not depend on whether a backend is installed.
     */
    if (shellType >= WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT) {
        return Invalid(mod, "shell type is outside the stock tracer table");
    }
    DeclaredBackendLease lease;
    if (!lease || !lease.backend().tracer_get_shell_type_count ||
        !lease.backend().tracer_get_style) {
        return DeclaredBackendMissing(
            mod,
            "tracer_get_style_for_shell_type");
    }
    uint32_t count = 0u;
    WotbModV3Result result = ResolveTracerShellTypeCount(
        mod,
        lease,
        "tracer_get_style_for_shell_type",
        &count);
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (shellType >= count) {
        return Invalid(
            mod,
            "shell type is outside this client shell-type table");
    }
    WotbModV3TracerStyle style = {};
    style.struct_size = sizeof(style);
    style.api_version = WOTBMOD_V3_TRACER_VERSION;
    style.shell_type = shellType;
    result = CallDeclaredTracerStyle(lease, shellType, &style);
    if (result != WOTBMOD_V3_OK) {
        return DeclaredBackendFailure(
            mod,
            "tracer_get_style_for_shell_type",
            result);
    }
    constexpr uint32_t knownFlags =
        WOTBMOD_V3_TRACER_STYLE_NAME_VALID |
        WOTBMOD_V3_TRACER_STYLE_COLOR_VALID;
    if ((style.flags & ~knownFlags) != 0u ||
        (style.flags & WOTBMOD_V3_TRACER_STYLE_NAME_VALID) == 0u ||
        std::memchr(
            style.style_name,
            '\0',
            sizeof(style.style_name)) == nullptr ||
        style.style_name[0] == '\0') {
        return DeclaredBackendMalformed(
            mod,
            "tracer_get_style_for_shell_type",
            "style name was not resolved from the client table");
    }
    style.struct_size = sizeof(style);
    style.api_version = WOTBMOD_V3_TRACER_VERSION;
    /* Echoed back by this layer so a copied record stays self-describing. */
    style.shell_type = shellType;
    style.reserved = 0u;
    if (style.style_id > WOTBMOD_V3_TRACER_STYLE_STT_TRACER) {
        /*
         * The NAME is the source of truth. A ninth style in a future build
         * degrades to UNKNOWN with the real string still attached, which is
         * what tracer_v1.h asks for, rather than a wrong existing id.
         */
        style.style_id = WOTBMOD_V3_TRACER_STYLE_UNKNOWN;
    }
    if ((style.flags & WOTBMOD_V3_TRACER_STYLE_COLOR_VALID) != 0u &&
        !IsFinite(style.color)) {
        /*
         * The colour and the name are independently valid. A non-finite
         * colour costs the caller the colour, not the style name it asked
         * for, and it is never handed on as if it meant something.
         */
        style.flags &= ~static_cast<uint32_t>(
            WOTBMOD_V3_TRACER_STYLE_COLOR_VALID);
    }
    if ((style.flags & WOTBMOD_V3_TRACER_STYLE_COLOR_VALID) == 0u) {
        style.color = {};
    }
    *outStyle = style;
    return WOTBMOD_V3_OK;
}

WotbModV3UiApiV2 BuildUiApi() {
    WotbModV3UiApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_UI_VERSION;
    api.control_create = &UiControlCreate;
    api.control_clone = &UiControlClone;
    api.control_destroy = &UiControlDestroy;
    api.control_add_child = &UiControlAddChild;
    api.control_remove_child = &UiControlRemoveChild;
    api.control_set_parent = &UiControlSetParent;
    api.control_get_parent = &UiControlGetParent;
    api.control_get_child_count = &UiControlGetChildCount;
    api.control_get_child_at = &UiControlGetChildAt;
    api.control_set_id = &UiControlSetId;
    api.control_get_id = &UiControlGetId;
    api.control_find_by_id = &UiControlFindById;
    api.control_find_by_path = &UiControlFindByPath;
    api.control_get_owner_mod = &UiControlGetOwner;
    api.control_is_alive = &UiControlIsAlive;
    api.slot_find = &UiSlotFind;
    api.slot_attach = &UiSlotAttach;
    api.slot_detach = &UiSlotDetach;
    api.slot_enumerate = &UiSlotEnumerate;
    api.control_set_position = &UiSetPosition;
    api.control_get_position = &UiGetPosition;
    api.control_set_size = &UiSetSize;
    api.control_get_size = &UiGetSize;
    api.control_set_anchor = &UiSetAnchor;
    api.control_set_pivot = &UiSetPivot;
    api.control_set_margin = &UiSetMargin;
    api.control_set_padding = &UiSetPadding;
    api.control_set_min_size = &UiSetMinSize;
    api.control_set_max_size = &UiSetMaxSize;
    api.control_set_z_order = &UiSetZOrder;
    api.layout_set = &UiLayoutSet;
    api.layout_set_type = &UiLayoutSetType;
    api.layout_set_direction = &UiLayoutSetDirection;
    api.layout_set_spacing = &UiLayoutSetSpacing;
    api.layout_set_alignment = &UiLayoutSetAlignment;
    api.layout_set_weight = &UiLayoutSetWeight;
    api.layout_invalidate = &UiLayoutInvalidate;
    api.get_scale_factor = &UiGetScaleFactor;
    api.get_safe_area = &UiGetSafeArea;
    api.get_viewport_size = &UiGetViewportSize;
    api.control_set_text = &UiSetText;
    api.control_set_texture = &UiSetTexture;
    api.control_set_color = &UiSetColor;
    api.control_set_opacity = &UiSetOpacity;
    api.control_set_visible = &UiSetVisible;
    api.control_set_font = &UiSetFont;
    api.control_set_font_size = &UiSetFontSize;
    api.control_set_text_alignment = &UiSetTextAlignment;
    api.control_set_text_wrap = &UiSetTextWrap;
    api.control_set_rich_text = &UiSetRichText;
    api.control_set_localization_key = &UiSetLocalizationKey;
    api.control_set_tooltip = &UiSetTooltip;
    api.control_set_accessibility_label =
        &UiSetAccessibilityLabel;
    api.control_set_enabled = &UiSetEnabled;
    api.control_set_interactable = &UiSetInteractable;
    api.control_set_focus = &UiSetFocus;
    api.event_subscribe = &UiEventSubscribe;
    api.event_unsubscribe = &UiEventUnsubscribe;
    api.button_create = &UiButtonCreate;
    api.checkbox_create = &UiCheckboxCreate;
    api.slider_create = &UiSliderCreate;
    api.dropdown_create = &UiDropdownCreate;
    api.text_input_create = &UiTextInputCreate;
    api.scroll_view_create = &UiScrollViewCreate;
    api.list_create = &UiListCreate;
    api.tabs_create = &UiTabsCreate;
    api.dialog_show = &UiDialogShow;
    api.confirm_show = &UiConfirmShow;
    api.toast_show = &UiToastShow;
    api.style_push = &UiStylePush;
    api.style_update = &UiStyleUpdate;
    api.style_pop = &UiStylePop;
    return api;
}

WotbModV3UiApiV3 BuildUiApiV3() {
    WotbModV3UiApiV3 api = {};
    api.v2 = BuildUiApi();
    api.v2.struct_size = sizeof(api);
    api.v2.api_version = WOTBMOD_V3_UI_VERSION_3;
    api.get_active_screen = &UiGetActiveScreen;
    api.control_get_snapshot = &UiControlGetSnapshot;
    return api;
}

WotbModV3RenderApiV1 BuildRenderApi() {
    WotbModV3RenderApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_RENDER_VERSION;
    api.register_callback = &RenderRegisterCallback;
    api.unregister_callback = &RenderUnregisterCallback;
    api.set_callback_priority = &RenderSetCallbackPriority;
    api.get_backend = &RenderGetBackend;
    api.get_viewport = &RenderGetViewport;
    api.get_frame_index = &RenderGetFrameIndex;
    api.get_delta_time = &RenderGetDeltaTime;
    api.create_texture = &RenderCreateTexture;
    api.update_texture = &RenderUpdateTexture;
    api.destroy_texture = &RenderDestroyTexture;
    api.create_material = &RenderCreateMaterial;
    api.set_material_parameter = &RenderSetMaterialParameter;
    api.destroy_material = &RenderDestroyMaterial;
    api.draw_sprite = &RenderDrawSprite;
    api.draw_text = &RenderDrawText;
    api.draw_line = &RenderDrawLine;
    api.draw_mesh = &RenderDrawMesh;
    api.push_state = &RenderPushState;
    api.pop_state = &RenderPopState;
    return api;
}

WotbModV3RenderNativeApiV1 BuildRenderNativeApi() {
    WotbModV3RenderNativeApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_RENDER_NATIVE_VERSION;
    api.get_native_device = &RenderGetNativeDevice;
    api.get_native_context = &RenderGetNativeContext;
    api.get_native_swapchain = &RenderGetNativeSwapchain;
    return api;
}

WotbModV3CameraApiV1 BuildCameraApi() {
    WotbModV3CameraApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_CAMERA_VERSION;
    api.get_active = &CameraGetActive;
    api.get_mode = &CameraGetMode;
    api.get_transform = &CameraGetTransform;
    api.set_transform = &CameraSetTransform;
    api.get_fov = &CameraGetFov;
    api.set_fov = &CameraSetFov;
    api.get_near_plane = &CameraGetNearPlane;
    api.get_far_plane = &CameraGetFarPlane;
    api.world_to_screen = &CameraWorldToScreen;
    api.screen_to_world = &CameraScreenToWorld;
    api.add_modifier = &CameraAddModifier;
    api.remove_modifier = &CameraRemoveModifier;
    api.transition_to = &CameraTransitionTo;
    api.add_shake = &CameraAddShake;
    return api;
}

WotbModV3SceneApiV1 BuildSceneApi() {
    WotbModV3SceneApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_SCENE_VERSION;
    api.entity_create = &SceneEntityCreate;
    api.entity_load = &SceneEntityLoad;
    api.entity_clone = &SceneEntityClone;
    api.entity_destroy = &SceneEntityDestroy;
    api.get_active_scene = &SceneGetActive;
    api.entity_get_parent = &SceneGetParent;
    api.entity_set_parent = &SceneSetParent;
    api.entity_add_child = &SceneAddChild;
    api.entity_remove_child = &SceneRemoveChild;
    api.entity_attach_ex = &SceneAttachEx;
    api.entity_set_transform = &SceneSetTransform;
    api.entity_get_transform = &SceneGetTransform;
    api.entity_get_world_transform = &SceneGetWorldTransform;
    api.entity_set_world_transform = &SceneSetWorldTransform;
    api.entity_get_bounds = &SceneGetBounds;
    api.entity_set_render_layer = &SceneSetRenderLayer;
    api.entity_set_render_order = &SceneSetRenderOrder;
    api.entity_set_lod_bias = &SceneSetLodBias;
    api.entity_list_nodes = &SceneListNodes;
    api.entity_find_node_by_path = &SceneFindNodeByPath;
    api.entity_list_animations = &SceneListAnimations;
    api.entity_get_animation_duration =
        &SceneGetAnimationDuration;
    api.entity_set_animation_speed = &SceneSetAnimationSpeed;
    api.entity_set_animation_loop = &SceneSetAnimationLoop;
    api.entity_blend_animation = &SceneBlendAnimation;
    api.entity_set_material_parameter =
        &SceneSetMaterialParameter;
    api.entity_clear_material_parameter =
        &SceneClearMaterialParameter;
    api.entity_set_shader_parameter = &SceneSetShaderParameter;
    return api;
}

WotbModV3AudioApiV2 BuildAudioApi() {
    WotbModV3AudioApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_AUDIO_VERSION;
    api.create = &AudioCreate;
    api.create_stream = &AudioCreateStream;
    api.preload = &AudioPreload;
    api.play = &AudioPlay;
    api.pause = &AudioPause;
    api.resume = &AudioResume;
    api.stop = &AudioStop;
    api.destroy = &AudioDestroy;
    api.is_playing = &AudioIsPlaying;
    api.get_state = &AudioGetState;
    api.set_volume = &AudioSetVolume;
    api.set_pitch = &AudioSetPitch;
    api.set_pan = &AudioSetPan;
    api.set_bus = &AudioSetBus;
    api.set_position_3d = &AudioSetPosition3d;
    api.set_min_distance = &AudioSetMinDistance;
    api.set_max_distance = &AudioSetMaxDistance;
    api.fade_to = &AudioFadeTo;
    api.seek = &AudioSeek;
    api.get_position = &AudioGetPosition;
    api.get_duration = &AudioGetDuration;
    api.set_loop = &AudioSetLoop;
    api.on_started = &AudioOnStarted;
    api.on_finished = &AudioOnFinished;
    api.on_error = &AudioOnError;
    api.unsubscribe = &AudioUnsubscribe;
    api.sound_override_register = &AudioOverrideRegister;
    api.sound_override_unregister = &AudioOverrideUnregister;
    api.sound_override_set_priority = &AudioOverrideSetPriority;
    api.sound_override_get_conflicts =
        &AudioOverrideGetConflicts;
    api.sound_event_create = &SoundEventCreate;
    api.sound_event_trigger = &SoundEventTrigger;
    api.sound_event_stop = &SoundEventStop;
    api.sound_event_set_paused = &SoundEventSetPaused;
    api.sound_event_set_volume = &SoundEventSetVolume;
    api.sound_event_set_position = &SoundEventSetPosition;
    api.sound_event_set_parameter = &SoundEventSetParameter;
    api.sound_event_get_parameter = &SoundEventGetParameter;
    api.sound_event_has_parameter = &SoundEventHasParameter;
    api.sound_event_get_name = &SoundEventGetName;
    api.sound_event_get_bus = &SoundEventGetBus;
    api.sound_event_set_speed = &SoundEventSetSpeed;
    api.sound_event_set_direction = &SoundEventSetDirection;
    api.sound_event_set_velocity = &SoundEventSetVelocity;
    api.sound_event_set_loop_count = &SoundEventSetLoopCount;
    api.sound_event_set_priority = &SoundEventSetPriority;
    api.sound_event_destroy = &SoundEventDestroy;
    api.set_listener_transform = &AudioSetListenerTransform;
    api.set_bus_volume = &AudioSetBusVolume;
    return api;
}

WotbModV3VehicleVisualApiV1 BuildVehicleVisualApi() {
    WotbModV3VehicleVisualApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_VEHICLE_VISUAL_VERSION;
    api.get_local_player_vehicle = &VehicleGetLocal;
    api.is_local_player = &VehicleIsLocal;
    api.is_hangar_vehicle = &VehicleIsHangar;
    api.get_appearance_state = &VehicleGetAppearanceState;
    api.get_enemy = &VehicleGetEnemy;
    api.get_enemy_name = &VehicleGetEnemyName;
    api.get_position = &VehicleGetPosition;
    api.get_part_entity = &VehicleGetPartEntity;
    api.find_attachment_point = &VehicleFindAttachmentPoint;
    api.attach_entity_ex = &VehicleAttachEntityEx;
    api.set_decal_override = &VehicleSetDecal;
    api.set_texture_override = &VehicleSetTexture;
    api.set_color_override = &VehicleSetColor;
    api.get_available_animation = &VehicleGetAnimation;
    api.play_animation = &VehiclePlayAnimation;
    api.restore_appearance = &VehicleRestoreAppearance;
    api.set_part_visible = &VehicleSetPartVisible;
    api.set_material_override = &VehicleSetMaterial;
    api.skin_apply_to_entity = &VehicleApplySkin;
    api.set_custom_skin = &VehicleSetCustomSkin;
    api.set_custom_camouflage = &VehicleSetCustomCamouflage;
    api.set_emblem = &VehicleSetEmblem;
    api.set_inscription = &VehicleSetInscription;
    api.set_engine_sound = &VehicleSetEngineSound;
    api.set_gun_sound = &VehicleSetGunSound;
    api.play_hangar_animation = &VehiclePlayHangarAnimation;
    api.set_hangar_idle_animation =
        &VehicleSetHangarIdleAnimation;
    api.profile_register = &VehicleProfileRegister;
    api.profile_apply = &VehicleProfileApply;
    api.profile_release = &VehicleProfileRelease;
    return api;
}

WotbModV3VehicleVisualApiV2 BuildVehicleVisualApiV2() {
    WotbModV3VehicleVisualApiV2 api = {};
    api.v1 = BuildVehicleVisualApi();
    api.v1.struct_size = sizeof(api);
    api.v1.api_version = WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2;
    api.skin_pack_register = &VehicleSkinPackRegister;
    api.skin_pack_apply = &VehicleSkinPackApply;
    api.skin_pack_rollback = &VehicleSkinPackRollback;
    api.skin_pack_get_state = &VehicleSkinPackGetState;
    api.skin_pack_release = &VehicleSkinPackRelease;
    return api;
}

WotbModV3GameplayCameraApiV1 BuildGameplayCameraApi() {
    WotbModV3GameplayCameraApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION;
    api.set_fov = &GameplayCameraSetFov;
    api.get_fov = &GameplayCameraGetFov;
    api.set_fov_hangar = &GameplayCameraSetFovHangar;
    api.set_fov_battle = &GameplayCameraSetFovBattle;
    api.set_fov_sniper = &GameplayCameraSetFovSniper;
    api.reset_fov = &GameplayCameraResetFov;
    api.set_zoom_steps = &GameplayCameraSetZoomSteps;
    api.get_zoom_steps = &GameplayCameraGetZoomSteps;
    api.set_zoom_multiplier = &GameplayCameraSetZoomMultiplier;
    api.set_max_zoom = &GameplayCameraSetMaxZoom;
    api.set_transition_mode =
        &GameplayCameraSetTransitionMode;
    api.set_transition_duration =
        &GameplayCameraSetTransitionDuration;
    api.set_transition_easing =
        &GameplayCameraSetTransitionEasing;
    api.set_shake_enabled = &GameplayCameraSetShakeEnabled;
    api.set_shake_intensity = &GameplayCameraSetShakeIntensity;
    api.set_postprocessing_enabled =
        &GameplayCameraSetPostprocessing;
    api.set_bloom_enabled = &GameplayCameraSetBloom;
    api.set_motion_blur_enabled = &GameplayCameraSetMotionBlur;
    api.set_vignette_enabled = &GameplayCameraSetVignette;
    api.set_color_grading = &GameplayCameraSetColorGrading;
    api.set_dof_enabled = &GameplayCameraSetDofEnabled;
    api.set_dof_params = &GameplayCameraSetDofParams;
    api.free_enable = &GameplayCameraFreeEnable;
    api.free_set_speed = &GameplayCameraFreeSetSpeed;
    api.free_set_position = &GameplayCameraFreeSetPosition;
    api.free_set_rotation = &GameplayCameraFreeSetRotation;
    api.free_get_position = &GameplayCameraFreeGetPosition;
    api.free_get_rotation = &GameplayCameraFreeGetRotation;
    api.get_config = &GameplayCameraGetConfig;
    api.apply_config = &GameplayCameraApplyConfig;
    return api;
}

WotbModV3GameplayHudApiV1 BuildGameplayHudApi() {
    WotbModV3GameplayHudApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GAMEPLAY_HUD_VERSION;
    api.reticle_set_texture = &HudReticleSetTexture;
    api.reticle_set_color = &HudReticleSetColor;
    api.reticle_set_size = &HudReticleSetSize;
    api.reticle_set_sniper_texture = &HudReticleSetSniperTexture;
    api.reticle_set_reloading_indicator = &HudReticleSetReloading;
    api.reticle_set_dispersion_circle = &HudReticleSetDispersion;
    api.damagelog_set_enabled = &HudDamageLogSetEnabled;
    api.damagelog_set_position = &HudDamageLogSetPosition;
    api.damagelog_set_max_entries = &HudDamageLogSetMaxEntries;
    api.damagelog_set_show_blocked = &HudDamageLogSetShowBlocked;
    api.damagelog_set_show_ricochet =
        &HudDamageLogSetShowRicochet;
    api.damagelog_set_show_module_damage =
        &HudDamageLogSetShowModule;
    api.damagelog_set_format = &HudDamageLogSetFormat;
    api.damagelog_set_filter_own = &HudDamageLogSetFilterOwn;
    api.session_stats_set_enabled = &HudSessionStatsSetEnabled;
    api.session_stats_set_fields = &HudSessionStatsSetFields;
    api.minimap_set_size = &HudMinimapSetSize;
    api.minimap_set_opacity = &HudMinimapSetOpacity;
    api.minimap_set_show_last_known = &HudMinimapSetLastKnown;
    api.minimap_set_show_artillery_range =
        &HudMinimapSetArtilleryRange;
    api.minimap_set_show_drawing = &HudMinimapSetDrawing;
    api.minimap_add_marker = &HudMinimapAddMarker;
    api.minimap_remove_marker = &HudMinimapRemoveMarker;
    api.sixth_sense_set_texture = &HudSixthSenseSetTexture;
    api.sixth_sense_set_sound = &HudSixthSenseSetSound;
    api.sixth_sense_set_position = &HudSixthSenseSetPosition;
    api.sixth_sense_set_scale = &HudSixthSenseSetScale;
    api.sixth_sense_set_delay_ms = &HudSixthSenseSetDelay;
    api.hit_indicator_set_style = &HudHitIndicatorSetStyle;
    api.hit_indicator_set_color_hit =
        &HudHitIndicatorSetHitColor;
    api.hit_indicator_set_color_pen =
        &HudHitIndicatorSetPenColor;
    api.hit_indicator_set_color_ricochet =
        &HudHitIndicatorSetRicochetColor;
    api.hit_indicator_set_color_crit =
        &HudHitIndicatorSetCritColor;
    api.reset = &HudReset;
    return api;
}

WotbModV3GameplayHangarApiV1 BuildGameplayHangarApi() {
    WotbModV3GameplayHangarApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION;
    api.set_background = &HangarSetBackground;
    api.set_background_video = &HangarSetBackgroundVideo;
    api.set_music = &HangarSetMusic;
    api.set_lighting = &HangarSetLighting;
    api.set_vehicle_preview_angle = &HangarSetPreviewAngle;
    api.set_vehicle_preview_zoom = &HangarSetPreviewZoom;
    api.set_floor_texture = &HangarSetFloorTexture;
    api.set_skybox = &HangarSetSkybox;
    api.hide_ui_elements = &HangarHideUiElements;
    api.set_camera_orbit_speed = &HangarSetOrbitSpeed;
    api.reset = &HangarReset;
    return api;
}

WotbModV3GameplayReplayApiV1 BuildGameplayReplayApi() {
    WotbModV3GameplayReplayApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION;
    api.set_speed = &ReplaySetSpeed;
    api.seek = &ReplaySeek;
    api.get_duration = &ReplayGetDuration;
    api.get_position = &ReplayGetPosition;
    api.add_marker = &ReplayAddMarker;
    api.remove_marker = &ReplayRemoveMarker;
    api.set_camera_mode = &ReplaySetCameraMode;
    api.set_follow_vehicle = &ReplaySetFollowVehicle;
    api.export_clip = &ReplayExportClip;
    return api;
}

WotbModV3EntityPublicApiV1 BuildEntityPublicApi() {
    WotbModV3EntityPublicApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_ENTITY_PUBLIC_VERSION;
    api.get_public_id = &EntityGetPublicId;
    api.get_public_type = &EntityGetPublicType;
    api.get_public_property = &EntityGetPublicProperty;
    api.subscribe_public_property = &EntitySubscribeProperty;
    api.unsubscribe_public_property = &EntityUnsubscribeProperty;
    api.is_visible_to_player = &EntityIsVisible;
    api.get_snapshot = &EntityGetSnapshot;
    api.enumerate_visible = &EntityEnumerateVisible;
    return api;
}

WotbModV3BigWorldRpcApiV1 BuildBigWorldRpcApi() {
    WotbModV3BigWorldRpcApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_BIGWORLD_RPC_VERSION;
    api.get_policy = &RpcGetPolicy;
    api.subscribe_observed = &RpcSubscribeObserved;
    api.unsubscribe_observed = &RpcUnsubscribeObserved;
    return api;
}

WotbModV3ProjectileApiV2 BuildProjectileApi() {
    WotbModV3ProjectileApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_PROJECTILE_VERSION_2;
    api.tracer_style_register = &TracerStyleRegister;
    api.tracer_style_unregister = &TracerStyleUnregister;
    api.tracer_set_texture = &TracerSetTexture;
    api.tracer_set_color = &TracerSetColor;
    api.tracer_set_width = &TracerSetWidth;
    api.tracer_set_lifetime = &TracerSetLifetime;
    api.tracer_set_fade = &TracerSetFade;
    api.projectile_get_visual_entity = &ProjectileGetVisualEntity;
    api.projectile_attach_visual = &ProjectileAttachVisual;
    api.projectile_get_owner_scope = &ProjectileGetOwnerScope;
    api.projectile_get_snapshot = &ProjectileGetSnapshot;
    api.impact_visual_register = &ImpactVisualRegister;
    api.impact_visual_update = &ImpactVisualUpdate;
    api.impact_visual_unregister = &ImpactVisualUnregister;
    return api;
}

WotbModV3ClientApiV1 BuildClientApi() {
    WotbModV3ClientApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_CLIENT_VERSION;
    api.is_supported = &ClientIsSupported;
    api.get_binding_pack_version = &ClientGetBindingPackVersion;
    api.get_compatibility_state = &ClientGetCompatibilityState;
    api.enumerate_missing_bindings =
        &ClientEnumerateMissingBindings;
    api.leave_to_hangar = &ClientLeaveToHangar;
    return api;
}

WotbModV3DeviceApiV1 BuildDeviceApi() {
    WotbModV3DeviceApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_DEVICE_VERSION;
    api.get_info = &DeviceGetInfo;
    api.get_graphics_adapter_count =
        &DeviceGetGraphicsAdapterCount;
    api.get_graphics_adapter_at = &DeviceGetGraphicsAdapterAt;
    return api;
}

WotbModV3UiApiV4 BuildUiApiV4() {
    WotbModV3UiApiV4 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_UI_VERSION_4;
    api.control_get_text = &UiReadGetText;
    api.control_get_live_text = &UiReadGetLiveText;
    api.control_get_texture = &UiReadGetTexture;
    api.control_get_font = &UiReadGetFont;
    api.control_get_style = &UiReadGetStyle;
    return api;
}

WotbModV3CameraApiV2 BuildCameraApiV2() {
    WotbModV3CameraApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_CAMERA_VERSION_2;
    api.get_animation_state = &CameraStateGetAnimationState;
    api.get_view_mode = &CameraStateGetViewMode;
    api.get_observed_state = &CameraStateGetObserved;
    return api;
}

WotbModV3AudioApiV3 BuildAudioApiV3() {
    WotbModV3AudioApiV3 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_AUDIO_VERSION_3;
    api.intercept_register = &AudioInterceptRegister;
    api.intercept_unregister = &AudioInterceptUnregister;
    api.intercept_set_priority = &AudioInterceptSetPriority;
    api.is_intercept_active = &AudioInterceptIsActive;
    return api;
}

WotbModV3SceneApiV2 BuildSceneApiV2() {
    WotbModV3SceneApiV2 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_SCENE_VERSION_2;
    api.get_limits = &SceneEnumerateGetLimits;
    api.walk_active_scene = &SceneEnumerateWalkActive;
    return api;
}

WotbModV3TracerApiV1 BuildTracerApi() {
    WotbModV3TracerApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_TRACER_VERSION;
    api.get_shell_type_count = &TracerGetShellTypeCount;
    api.get_style_for_shell_type = &TracerGetStyleForShellType;
    return api;
}

const WotbModV3UiApiV2 kUiApi = BuildUiApi();
const WotbModV3UiApiV3 kUiApiV3 = BuildUiApiV3();
const WotbModV3UiApiV4 kUiApiV4 = BuildUiApiV4();
const WotbModV3CameraApiV2 kCameraApiV2 = BuildCameraApiV2();
const WotbModV3AudioApiV3 kAudioApiV3 = BuildAudioApiV3();
const WotbModV3SceneApiV2 kSceneApiV2 = BuildSceneApiV2();
const WotbModV3TracerApiV1 kTracerApi = BuildTracerApi();
const WotbModV3RenderApiV1 kRenderApi = BuildRenderApi();
const WotbModV3RenderNativeApiV1 kRenderNativeApi =
    BuildRenderNativeApi();
const WotbModV3CameraApiV1 kCameraApi = BuildCameraApi();
const WotbModV3SceneApiV1 kSceneApi = BuildSceneApi();
const WotbModV3AudioApiV2 kAudioApi = BuildAudioApi();
const WotbModV3VehicleVisualApiV1 kVehicleVisualApi =
    BuildVehicleVisualApi();
const WotbModV3VehicleVisualApiV2 kVehicleVisualApiV2 =
    BuildVehicleVisualApiV2();
const WotbModV3GameplayCameraApiV1 kGameplayCameraApi =
    BuildGameplayCameraApi();
const WotbModV3GameplayHudApiV1 kGameplayHudApi =
    BuildGameplayHudApi();
const WotbModV3GameplayHangarApiV1 kGameplayHangarApi =
    BuildGameplayHangarApi();
const WotbModV3GameplayReplayApiV1 kGameplayReplayApi =
    BuildGameplayReplayApi();
const WotbModV3EntityPublicApiV1 kEntityPublicApi =
    BuildEntityPublicApi();
const WotbModV3BigWorldRpcApiV1 kBigWorldRpcApi =
    BuildBigWorldRpcApi();
const WotbModV3ProjectileApiV2 kProjectileApi =
    BuildProjectileApi();
const WotbModV3ClientApiV1 kClientApi = BuildClientApi();
const WotbModV3DeviceApiV1 kDeviceApi = BuildDeviceApi();

bool FixedTextTerminated(const char* text, size_t size) {
    return text && std::memchr(text, '\0', size) != nullptr;
}

bool ValidClientHostPublicEntity(
    const ClientHostPublicEntity* entity) {
    constexpr uint64_t kRequiredFields =
        CLIENT_HOST_PUBLIC_ENTITY_FIELD_ID |
        CLIENT_HOST_PUBLIC_ENTITY_FIELD_TYPE |
        CLIENT_HOST_PUBLIC_ENTITY_FIELD_VISIBILITY |
        CLIENT_HOST_PUBLIC_ENTITY_FIELD_LOCAL;
    if (!entity ||
        entity->struct_size < sizeof(*entity) ||
        entity->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
        entity->native_token == 0u ||
        (entity->valid_fields &
         ~CLIENT_HOST_PUBLIC_ENTITY_FIELDS_ALL) != 0u ||
        (entity->valid_fields & kRequiredFields) !=
            kRequiredFields ||
        !ValidStruct(
            &entity->snapshot,
            WOTBMOD_V3_ENTITY_PUBLIC_VERSION) ||
        entity->snapshot.public_id == 0u ||
        entity->snapshot.type > WOTBMOD_V3_PUBLIC_ENTITY_EFFECT ||
        entity->snapshot.visible_to_player > 1u ||
        entity->snapshot.local_player > 1u ||
        (entity->snapshot.visible_to_player == 0u &&
         entity->snapshot.local_player == 0u) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_HEALTH) != 0u &&
         entity->snapshot.health < 0) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_MAX_HEALTH) != 0u &&
         entity->snapshot.max_health < 0) ||
        ((entity->valid_fields &
          (CLIENT_HOST_PUBLIC_ENTITY_FIELD_HEALTH |
           CLIENT_HOST_PUBLIC_ENTITY_FIELD_MAX_HEALTH)) ==
             (CLIENT_HOST_PUBLIC_ENTITY_FIELD_HEALTH |
              CLIENT_HOST_PUBLIC_ENTITY_FIELD_MAX_HEALTH) &&
         entity->snapshot.health > entity->snapshot.max_health) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_POSITION) != 0u &&
         !IsFinite(entity->snapshot.position)) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_DIRECTION) != 0u &&
         !IsFinite(entity->snapshot.direction)) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE) != 0u &&
         !FixedTextTerminated(
             entity->snapshot.public_type,
             sizeof(entity->snapshot.public_type))) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_DISPLAY_NAME) != 0u &&
         !FixedTextTerminated(
             entity->snapshot.display_name,
             sizeof(entity->snapshot.display_name))) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_CLAN_TAG) != 0u &&
         !FixedTextTerminated(
             entity->extras.clan_tag,
             sizeof(entity->extras.clan_tag))) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_NAME) != 0u &&
         !FixedTextTerminated(
             entity->extras.vehicle_name,
             sizeof(entity->extras.vehicle_name))) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_KILLS) != 0u &&
         entity->extras.kills < 0) ||
        ((entity->valid_fields &
          CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME) != 0u &&
         !FixedTextTerminated(
             entity->extras.vehicle_display_name,
             sizeof(entity->extras.vehicle_display_name)))) {
        return false;
    }
    return true;
}

ClientHostPublicEntityExtras SanitizePublicEntityExtras(
    const ClientHostPublicEntityExtras& source,
    uint64_t validFields) {
    ClientHostPublicEntityExtras extras = source;
    extras.reserved = 0u;
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_ACCOUNT_ID) == 0u) {
        extras.account_id = 0;
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_KILLS) == 0u) {
        extras.kills = 0;
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_CLAN_TAG) == 0u) {
        std::memset(extras.clan_tag, 0, sizeof(extras.clan_tag));
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_NAME) == 0u) {
        std::memset(extras.vehicle_name, 0, sizeof(extras.vehicle_name));
    }
    if ((validFields &
         CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_DISPLAY_NAME) == 0u) {
        std::memset(
            extras.vehicle_display_name, 0, sizeof(extras.vehicle_display_name));
    }
    return extras;
}

WotbModV3PublicEntitySnapshot SanitizePublicEntitySnapshot(
    const WotbModV3PublicEntitySnapshot& source,
    uint64_t validFields) {
    WotbModV3PublicEntitySnapshot snapshot = source;
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_ID) == 0u) {
        snapshot.public_id = 0u;
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_TYPE) == 0u) {
        snapshot.type = WOTBMOD_V3_PUBLIC_ENTITY_UNKNOWN;
    }
    if ((validFields &
         CLIENT_HOST_PUBLIC_ENTITY_FIELD_VISIBILITY) == 0u) {
        snapshot.visible_to_player = 0u;
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_LOCAL) == 0u) {
        snapshot.local_player = 0u;
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_TEAM) == 0u) {
        snapshot.team = 0u;
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_HEALTH) == 0u) {
        snapshot.health = 0;
    }
    if ((validFields &
         CLIENT_HOST_PUBLIC_ENTITY_FIELD_MAX_HEALTH) == 0u) {
        snapshot.max_health = 0;
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_POSITION) == 0u) {
        snapshot.position = {};
    }
    if ((validFields & CLIENT_HOST_PUBLIC_ENTITY_FIELD_DIRECTION) == 0u) {
        snapshot.direction = {};
    }
    if ((validFields &
         CLIENT_HOST_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE) == 0u) {
        std::memset(
            snapshot.public_type,
            0,
            sizeof(snapshot.public_type));
    }
    if ((validFields &
         CLIENT_HOST_PUBLIC_ENTITY_FIELD_DISPLAY_NAME) == 0u) {
        std::memset(
            snapshot.display_name,
            0,
            sizeof(snapshot.display_name));
    }
    return snapshot;
}

bool ValidClientHostProjectile(
    const ClientHostProjectile* projectile) {
    if (!projectile ||
        projectile->struct_size < sizeof(*projectile) ||
        projectile->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
        projectile->native_token == 0u ||
        projectile->owner_scope <
            WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER ||
        projectile->owner_scope > WOTBMOD_V3_PROJECTILE_OWNER_REPLAY ||
        (projectile->lifecycle_state != 0u &&
         projectile->lifecycle_state <
            WOTBMOD_V3_PROJECTILE_STATE_CREATED) ||
        projectile->lifecycle_state >
            WOTBMOD_V3_PROJECTILE_STATE_DESTROYED ||
        (projectile->source != 0u &&
         projectile->source <
            WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT) ||
        projectile->source >
            WOTBMOD_V3_PROJECTILE_SOURCE_API_MANAGED ||
        (projectile->valid_fields &
         ~static_cast<uint64_t>(WOTBMOD_V3_PROJECTILE_FIELDS_ALL)) != 0u ||
        !IsFinite(projectile->visible_position) ||
        !IsFinite(projectile->visible_direction) ||
        !IsFinite(projectile->origin) ||
        !IsFinite(projectile->impact_position)) {
        return false;
    }
    return true;
}

void DispatchEntityPropertyCallbacks(
    WotbModV3EntityHandle entity,
    const WotbModV3PublicEntitySnapshot& snapshot,
    const ClientHostPublicEntityExtras& extras,
    uint64_t validFields) {
    struct CallbackCopy {
        WotbModV3Handle owner;
        std::string property;
        std::shared_ptr<SubscriptionCallbackState> state;
    };
    std::vector<CallbackCopy> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        callbacks.reserve(g_subscriptions.size());
        for (const auto& pair : g_subscriptions) {
            const Subscription* subscription = pair.second.get();
            if (!subscription ||
                subscription->magic != kSubscriptionMagic ||
                subscription->kind !=
                    SubscriptionKind::EntityProperty ||
                subscription->target != entity ||
                !subscription->callbackState) {
                continue;
            }
            callbacks.push_back(
                {subscription->owner,
                 subscription->property,
                 subscription->callbackState});
        }
    }
    for (const CallbackCopy& callback : callbacks) {
        if (snapshot.visible_to_player == 0u &&
            callback.property != "visible" &&
            callback.property != "visible_to_player") {
            continue;
        }
        const uint64_t field =
            PublicPropertyField(callback.property.c_str());
        WotbModV3PublicValue value = {};
        if (field == 0u ||
            (validFields & field) == 0u ||
            !PublicPropertyFromRecord(
                snapshot,
                extras,
                callback.property.c_str(),
                &value) ||
            !IsModEnabled(callback.owner) ||
            CheckEntityPublicPermission(
                callback.owner,
                snapshot.local_player != 0u) != WOTBMOD_V3_OK) {
            continue;
        }
        SubscriptionCallbackLease lease(callback.state);
        if (!lease) {
            continue;
        }
        const WotbModV3PublicPropertyCallback invoke =
            callback.state->entityPropertyCallback;
        void* const userData = callback.state->userData;
        if (!invoke ||
            EnterModCallbackCoalescible(callback.owner) !=
                WOTBMOD_V3_OK) {
            continue;
        }
        try {
            invoke(
                callback.owner,
                entity,
                callback.property.c_str(),
                &value,
                userData);
        } catch (...) {
            SetError(
                callback.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "public entity property callback threw an exception");
        }
        LeaveModCallback(callback.owner);
    }
}

void ClientServicesShutdown() {
    std::lock_guard<std::mutex> transitionLock(
        g_hostBackendTransitionMutex);
    /*
     * Before anything else, and specifically before g_subscriptions is
     * cleared below: the sound detour reaches the interception subscriptions
     * from an arbitrary engine thread, so it has to be gone before the
     * container it reads is torn down.
     */
    {
        std::lock_guard<std::mutex> declaredTransitionLock(
            g_declaredBackendTransitionMutex);
        UninstallDeclaredBackend();
    }
    PublishDeclaredInterfaceAvailability();
    g_acceptingHostReleaseRetries.store(
        false,
        std::memory_order_release);
    g_uiViewportSize.store(0u, std::memory_order_release);
    g_audioLifecycleBeforeStateUpdateHook.store(
        nullptr,
        std::memory_order_release);
    {
        std::unique_lock<std::mutex> lock(g_clientMutex);
        g_clientServicesRunning = false;
        g_hostBackendAcceptingInvocations = false;
        g_hostBackendQuiesced.wait(
            lock,
            []() {
                return g_hostBackendInvocationsInFlight == 0u;
            });
    }
    RetryPendingHostReleases(true);
    {
        std::lock_guard<std::mutex> frameLock(
            g_clientFramePumpMutex);
        std::lock_guard<std::mutex> lock(g_clientMutex);
        AdvanceNonzeroGeneration(&g_hostBackendGeneration);
        g_subscriptionOwnerStates.fill(
            SubscriptionOwnerPublicationState{});
        g_hostBackend = {};
        g_hostFrame = {};
        g_hostBackendInstalled = false;
        g_hostFrameValid = false;
        g_lastExternalFrame =
            std::numeric_limits<uint64_t>::max();
        g_lastDeltaSeconds = 0.0;
        g_renderLifecycle = {};
        g_uiControls.clear();
        g_uiSlots.clear();
        g_subscriptions.clear();
        g_cameraEffectOwners.clear();
        g_sceneEntities.clear();
        g_hostEntities.clear();
        g_hostEntityTokens.clear();
        g_hostProjectiles.clear();
        g_hostProjectileTokens.clear();
        g_impactVisuals.clear();
        g_impactInstances.clear();
        g_pendingUiInputs.clear();
        g_uiHoveredControls.clear();
        g_uiPressedControls.clear();
        g_uiDragging.clear();
        g_lastUiPointer = {};
        g_haveLastUiPointer = false;
        g_hostAudioObjects.clear();
    }
    {
        std::lock_guard<std::mutex> captureLock(g_uiCaptureMutex);
        g_uiCaptureRects.clear();
    }
    {
        std::lock_guard<std::mutex> lock(
            g_pendingHostReleaseMutex);
        if (!g_pendingHostReleases.empty()) {
            RuntimeLog(
                WOTBMOD_V3_LOG_WARNING,
                "v3.client-services",
                "native host release retries were dropped at shutdown");
            g_pendingHostReleases.clear();
        }
    }
}

void RegisterClientInterface(
    const InterfaceRegistration& registration) {
    const WotbModV3Result result = RegisterInterface(registration);
    if (result == WOTBMOD_V3_OK ||
        result == WOTBMOD_V3_E_ALREADY_EXISTS) {
        return;
    }
    char message[WOTBMOD_V3_MAX_MESSAGE] = {};
#if defined(_MSC_VER)
    sprintf_s(
        message,
        "failed to register client interface %s v%u (result=%u)",
        registration.name,
        registration.version,
        static_cast<unsigned>(result));
#else
    std::snprintf(
        message,
        sizeof(message),
        "failed to register client interface %s v%u (result=%u)",
        registration.name,
        registration.version,
        static_cast<unsigned>(result));
#endif
    RuntimeLog(
        WOTBMOD_V3_LOG_ERROR,
        "v3.client-services",
        message);
}

}  // namespace

void SetAudioLifecycleBeforeStateUpdateHookForTesting(
    void (*hook)()) {
    g_audioLifecycleBeforeStateUpdateHook.store(
        hook,
        std::memory_order_release);
}

void SetClientHostBackend(const ClientHostBackend* backend) {
    if (g_hostBackendInvocationDepth != 0u) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "v3.client-services",
            "ignored reentrant client backend replacement");
        return;
    }
    const bool validBackend =
        backend &&
        backend->struct_size >= sizeof(ClientHostBackend) &&
        backend->api_version ==
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION &&
        backend->compatibility_state <=
            WOTBMOD_V3_CLIENT_COMPATIBILITY_HASH_MISMATCH &&
        backend->invoke;
    bool installed = false;
    bool replacing = false;
    {
        std::lock_guard<std::mutex> transitionLock(
            g_hostBackendTransitionMutex);
        {
            std::unique_lock<std::mutex> lock(g_clientMutex);
            replacing =
                !validBackend ||
                !g_hostBackendInstalled ||
                g_hostBackend.invoke != backend->invoke ||
                g_hostBackend.user_data != backend->user_data;
            if (replacing) {
                g_hostBackendAcceptingInvocations = false;
                g_hostBackendQuiesced.wait(
                    lock,
                    []() {
                        return g_hostBackendInvocationsInFlight == 0u;
                    });
            }
        }
        {
            std::unique_lock<std::mutex> frameLock(
                g_clientFramePumpMutex);
            std::unique_lock<std::mutex> lock(g_clientMutex);
            if (replacing) {
                AdvanceNonzeroGeneration(&g_hostBackendGeneration);
                g_hostEntities.clear();
                g_hostEntityTokens.clear();
                g_hostProjectiles.clear();
                g_hostProjectileTokens.clear();
                g_hostAudioObjects.clear();
                g_hostFrame = {};
                g_hostFrameValid = false;
                g_lastExternalFrame =
                    std::numeric_limits<uint64_t>::max();
                g_renderLifecycle = {};
            }
            if (validBackend) {
                g_hostBackend = *backend;
                g_hostBackendInstalled = true;
                g_hostBackendAcceptingInvocations =
                    g_clientServicesRunning;
                installed = true;
            } else {
                g_hostBackend = {};
                g_hostBackendInstalled = false;
                g_hostBackendAcceptingInvocations = false;
            }
        }
    }
    if (installed) {
        RetryPendingHostReleases();
    }
}

bool GetClientHostDeclaredBackend(ClientHostDeclaredBackend* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_declaredBackendMutex);
    if (!g_declaredBackendInstalled ||
        !g_declaredBackendAcceptingInvocations) {
        *out = ClientHostDeclaredBackend{};
        return false;
    }
    *out = g_declaredBackend;
    return true;
}

void SetClientHostDeclaredBackend(
    const ClientHostDeclaredBackend* backend) {
    GesSetDeclaredBackendAccessor(&GetClientHostDeclaredBackend);
    SessionClusterSetDeclaredBackendAccessor(&GetClientHostDeclaredBackend);
    if (g_declaredBackendInvocationDepth != 0u) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "v3.client-services",
            "ignored reentrant declared client backend replacement");
        return;
    }
    /*
     * The exact-fingerprint gate, fail-closed and whole-table. UNKNOWN,
     * BINDINGS_MISSING and HASH_MISMATCH all mean the loader could not prove
     * it is looking at the client these bindings were derived from, and
     * every one of the five interfaces reads or detours engine memory by
     * fixed offset. There is no per-slot override and no degraded install.
     */
    const bool acceptable =
        backend != nullptr &&
        backend->struct_size >= sizeof(ClientHostDeclaredBackend) &&
        backend->api_version ==
            WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION &&
        (backend->compatibility_state ==
             WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED ||
         backend->compatibility_state ==
             WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED);
    {
        std::lock_guard<std::mutex> transitionLock(
            g_declaredBackendTransitionMutex);
        UninstallDeclaredBackend();
        if (backend && !acceptable) {
            RuntimeLog(
                WOTBMOD_V3_LOG_WARNING,
                "v3.client-services",
                "refused a declared client backend that failed the ABI or client fingerprint gate");
        }
        if (acceptable) {
            ClientHostDeclaredBackend installed = *backend;
            installed.struct_size = sizeof(installed);
            /*
             * Groups are all-or-nothing. Half a group is a loader that has
             * not finished wiring an interface, and installing the half that
             * exists would let one slot answer while its sibling denies --
             * which is exactly the "one status describing two things" lie
             * these interfaces were split up to avoid.
             */
            if (!installed.ui_read_string || !installed.ui_read_style) {
                installed.ui_read_string = nullptr;
                installed.ui_read_style = nullptr;
            }
            if (!installed.audio_intercept_publish_name ||
                !installed.audio_intercept_disarm) {
                installed.audio_intercept_publish_name = nullptr;
                installed.audio_intercept_disarm = nullptr;
            }
            if (!installed.scene_get_walk_limits ||
                !installed.scene_walk_active) {
                installed.scene_get_walk_limits = nullptr;
                installed.scene_walk_active = nullptr;
            }
            if (!installed.tracer_get_shell_type_count ||
                !installed.tracer_get_style) {
                installed.tracer_get_shell_type_count = nullptr;
                installed.tracer_get_style = nullptr;
            }
            if (!installed.ges_list_types || !installed.ges_observe ||
                !installed.ges_publish) {
                installed.ges_list_types = nullptr;
                installed.ges_observe = nullptr;
                installed.ges_publish = nullptr;
            }
            std::lock_guard<std::mutex> lock(g_declaredBackendMutex);
            g_declaredBackend = installed;
            g_declaredBackendInstalled = true;
            g_declaredBackendAcceptingInvocations = true;
        }
    }
    PublishDeclaredInterfaceAvailability();
}

WotbModV3Result DispatchClientHostSoundIntercept(
    const char* eventName,
    WotbModV3SoundInterceptResponse* outResponse) {
    if (!eventName || !outResponse ||
        std::memchr(eventName, '\0', WOTBMOD_V3_MAX_NAME) == nullptr) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_soundInterceptDispatchEnabled.load(
            std::memory_order_acquire)) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    /*
     * The hybrid sound system calls the inner system's CreateSoundEvent from
     * inside its own, so the public path re-enters this detour on the same
     * thread. The guard is thread-local because that re-entry is, and
     * because a process-wide flag would drop a legitimate interception on
     * another thread that happened to be creating a sound at the same time.
     */
    if (g_soundInterceptDepth != 0u) {
        return WOTBMOD_V3_E_BUSY;
    }
    /*
     * Stamped before anything can go wrong, so every early return still
     * leaves the detour holding a well-formed PASS_THROUGH answer rather
     * than whatever was on its stack.
     */
    outResponse->struct_size = sizeof(*outResponse);
    outResponse->api_version = WOTBMOD_V3_AUDIO_VERSION_3;
    outResponse->decision = WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH;
    outResponse->reserved = 0u;
    outResponse->substitute_event_name[0] = '\0';
    struct InterceptCandidate {
        WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
        int32_t priority = 0;
        uint64_t creationOrder = 0u;
        std::shared_ptr<SubscriptionCallbackState> state;
    };
    std::vector<InterceptCandidate> candidates;
    try {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (!g_clientServicesRunning) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        for (const auto& pair : g_subscriptions) {
            const std::shared_ptr<Subscription>& subscription =
                pair.second;
            if (!subscription ||
                subscription->magic != kSubscriptionMagic ||
                subscription->kind !=
                    SubscriptionKind::AudioIntercept ||
                !subscription->callbackState ||
                subscription->property != eventName) {
                continue;
            }
            InterceptCandidate candidate;
            candidate.owner = subscription->owner;
            candidate.priority = subscription->priority;
            candidate.creationOrder = subscription->creationOrder;
            candidate.state = subscription->callbackState;
            candidates.push_back(candidate);
        }
    } catch (...) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (candidates.empty()) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const InterceptCandidate& left,
           const InterceptCandidate& right) {
            if (left.priority != right.priority) {
                return left.priority > right.priority;
            }
            return left.creationOrder < right.creationOrder;
        });
    WotbModV3SoundInterceptRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version = WOTBMOD_V3_AUDIO_VERSION_3;
    /*
     * Stamped by the runtime rather than taken from the detour: thread role
     * and game context are runtime facts, and a detour that mis-stated them
     * would be handing a mod a wrong premise for its decision.
     */
    request.thread_role = CurrentThreadRole();
    request.game_context = static_cast<uint32_t>(CurrentContext());
    CopyFixed(request.event_name, eventName);
    SoundInterceptDepthGuard depthGuard;
    for (const InterceptCandidate& candidate : candidates) {
        if (!IsModEnabled(candidate.owner) ||
            CheckAudioEventsGrant(candidate.owner) != WOTBMOD_V3_OK ||
            CheckAccess(
                candidate.owner,
                WOTBMOD_V3_PERMISSION_SAFE,
                kAudioInterceptContexts,
                nullptr) != WOTBMOD_V3_OK) {
            continue;
        }
        SubscriptionCallbackLease lease(candidate.state);
        if (!lease) {
            continue;
        }
        const WotbModV3SoundInterceptCallback callback =
            candidate.state->soundInterceptCallback;
        void* const userData = candidate.state->userData;
        if (!callback ||
            EnterModCallback(candidate.owner) != WOTBMOD_V3_OK) {
            continue;
        }
        WotbModV3SoundInterceptResponse local = {};
        local.struct_size = sizeof(local);
        local.api_version = WOTBMOD_V3_AUDIO_VERSION_3;
        local.decision = WOTBMOD_V3_SOUND_INTERCEPT_PASS_THROUGH;
        local.substitute_event_name[0] = '\0';
        const bool completed = CallSoundInterceptCallbackProtected(
            callback,
            candidate.owner,
            &request,
            &local,
            userData);
        LeaveModCallback(candidate.owner);
        if (!completed) {
            SetError(
                candidate.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "sound interception callback raised an exception");
            continue;
        }
        if (local.decision == WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS) {
            outResponse->decision =
                WOTBMOD_V3_SOUND_INTERCEPT_SUPPRESS;
            break;
        }
        if (local.decision == WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE &&
            std::memchr(
                local.substitute_event_name,
                '\0',
                sizeof(local.substitute_event_name)) != nullptr &&
            local.substitute_event_name[0] != '\0') {
            outResponse->decision =
                WOTBMOD_V3_SOUND_INTERCEPT_SUBSTITUTE;
            CopyFixed(
                outResponse->substitute_event_name,
                local.substitute_event_name);
            break;
        }
        /*
         * PASS_THROUGH, an unnamed decision, or a SUBSTITUTE with an empty
         * or unterminated name: discarded, and the chain continues. audio_v3.h
         * requires that an unusable substitution fall back to pass-through
         * rather than to suppression -- dropping a sound the mod did not ask
         * to drop is a behaviour change it never requested -- and a mod that
         * wrote nonsense does not get to veto the mods behind it.
         */
    }
    return WOTBMOD_V3_OK;
}

void SetClientHostUiViewportSize(uint32_t width, uint32_t height) {
    const uint64_t packedViewport =
        (static_cast<uint64_t>(width) << 32u) |
        static_cast<uint64_t>(height);
    g_uiViewportSize.store(packedViewport, std::memory_order_release);
}

WotbModV3Result NotifyClientHostUiInput(
    uint32_t phase,
    float screenX,
    float screenY,
    float deltaX,
    float deltaY,
    uint32_t modifiers) {
    if (phase > 11u || !IsFinite(screenX) || !IsFinite(screenY) ||
        !IsFinite(deltaX) || !IsFinite(deltaY)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    if (!g_clientServicesRunning) {
        return WOTBMOD_V3_E_OBJECT_DESTROYED;
    }
    PendingUiInput input = {};
    input.phase = phase;
    input.pointer = {screenX, screenY};
    input.delta = {deltaX, deltaY};
    input.modifiers = modifiers;
    if (!g_pendingUiInputs.empty() && phase != 2u && phase != 4u) {
        const PendingUiInput& previous = g_pendingUiInputs.back();
        if (previous.phase == input.phase &&
            previous.pointer.x == input.pointer.x &&
            previous.pointer.y == input.pointer.y &&
            previous.delta.x == input.delta.x &&
            previous.delta.y == input.delta.y &&
            previous.modifiers == input.modifiers) {
            // The Win32 and DAVA hooks can observe the same physical pointer
            // transition. Deliver it once so click handlers are not doubled.
            return WOTBMOD_V3_OK;
        }
    }
    if ((phase == 2u || phase == 4u) && g_haveLastUiPointer &&
        deltaX == 0.0f && deltaY == 0.0f) {
        input.delta = {
            screenX - g_lastUiPointer.x,
            screenY - g_lastUiPointer.y};
    }
    g_lastUiPointer = input.pointer;
    g_haveLastUiPointer = true;
    if (!g_pendingUiInputs.empty() &&
        (phase == 2u || phase == 4u) &&
        g_pendingUiInputs.back().phase == phase &&
        g_pendingUiInputs.back().modifiers == modifiers) {
        PendingUiInput& pending = g_pendingUiInputs.back();
        pending.pointer = input.pointer;
        pending.delta.x += input.delta.x;
        pending.delta.y += input.delta.y;
        return WOTBMOD_V3_OK;
    }
    if (g_pendingUiInputs.size() >= 256u) {
        const auto coalescible = std::find_if(
            g_pendingUiInputs.begin(),
            g_pendingUiInputs.end(),
            [](const PendingUiInput& candidate) {
                return candidate.phase == 2u || candidate.phase == 4u;
            });
        if (coalescible == g_pendingUiInputs.end()) {
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        g_pendingUiInputs.erase(coalescible);
    }
    g_pendingUiInputs.push_back(input);
    return WOTBMOD_V3_OK;
}

bool ShouldCaptureClientHostUiInput(float screenX, float screenY) {
    if (!IsFinite(screenX) || !IsFinite(screenY)) return false;
    std::lock_guard<std::mutex> captureLock(g_uiCaptureMutex);
    for (const WotbModV3Rect& rect : g_uiCaptureRects) {
        if (screenX >= rect.x && screenY >= rect.y &&
            screenX <= rect.x + rect.width &&
            screenY <= rect.y + rect.height) {
            return true;
        }
    }
    return false;
}

void RefreshClientHostUiCaptureSnapshot() {
    RefreshUiCaptureSnapshot();
}

void HudMainThreadTick() {
    HudMainThreadTickImpl();
}

void PumpClientHostFrame(const ClientHostFrame* frame) {
    if (!frame ||
        frame->struct_size < sizeof(ClientHostFrame)) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "v3.client-services",
            "ignored an invalid client host frame");
        return;
    }
    const ClientHostFrame frameSnapshot = *frame;
    const bool hasNoNativeObjects =
        !frameSnapshot.native_device &&
        !frameSnapshot.native_context &&
        !frameSnapshot.native_swapchain;
    const bool hasCompleteD3d11Objects =
        frameSnapshot.native_device &&
        frameSnapshot.native_context &&
        frameSnapshot.native_swapchain;
    if (frameSnapshot.api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
        (frameSnapshot.render_backend !=
             WOTBMOD_V3_RENDER_BACKEND_NONE &&
         frameSnapshot.render_backend !=
             WOTBMOD_V3_RENDER_BACKEND_D3D11) ||
        (frameSnapshot.render_backend ==
                 WOTBMOD_V3_RENDER_BACKEND_NONE
             ? !hasNoNativeObjects
             : !hasCompleteD3d11Objects) ||
        !IsFinite(frameSnapshot.delta_seconds) ||
        frameSnapshot.delta_seconds < 0.0 ||
        !IsFinite(frameSnapshot.viewport) ||
        frameSnapshot.viewport.width < 0.0f ||
        frameSnapshot.viewport.height < 0.0f) {
        RuntimeLog(
            WOTBMOD_V3_LOG_WARNING,
            "v3.client-services",
            "ignored an invalid client host frame");
        return;
    }
    std::lock_guard<std::mutex> frameLock(
        g_clientFramePumpMutex);
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        if (!g_clientServicesRunning) {
            return;
        }
    }
    ObserveExternalFrame(frameSnapshot.frame_index);
    RetryPendingHostReleases();
    struct PendingRenderEvent {
        const char* topic;
        WotbModV3RenderLifecycleEvent payload;
    };
    PendingRenderEvent pending[5] = {};
    uint32_t pendingCount = 0u;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const ClientHostFrame previous =
            g_renderLifecycle.observed
                ? g_renderLifecycle.frame
                : ClientHostFrame{};
        const bool previousAvailable =
            g_renderLifecycle.observed &&
            previous.render_backend ==
                WOTBMOD_V3_RENDER_BACKEND_D3D11 &&
            previous.native_device &&
            previous.native_context &&
            previous.native_swapchain;
        const bool available =
            frameSnapshot.render_backend ==
                WOTBMOD_V3_RENDER_BACKEND_D3D11;
        uint32_t componentChanges = 0u;
        if (previous.render_backend !=
            frameSnapshot.render_backend) {
            componentChanges |=
                WOTBMOD_V3_RENDER_LIFECYCLE_BACKEND;
        }
        if (previous.native_device !=
            frameSnapshot.native_device) {
            componentChanges |=
                WOTBMOD_V3_RENDER_LIFECYCLE_DEVICE;
        }
        if (previous.native_context !=
            frameSnapshot.native_context) {
            componentChanges |=
                WOTBMOD_V3_RENDER_LIFECYCLE_CONTEXT;
        }
        if (previous.native_swapchain !=
            frameSnapshot.native_swapchain) {
            componentChanges |=
                WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN;
        }
        const bool viewportChanged =
            previous.viewport.width !=
                frameSnapshot.viewport.width ||
            previous.viewport.height !=
                frameSnapshot.viewport.height;
        if (viewportChanged) {
            componentChanges |=
                WOTBMOD_V3_RENDER_LIFECYCLE_VIEWPORT;
        }
        if (previousAvailable != available) {
            componentChanges |=
                WOTBMOD_V3_RENDER_LIFECYCLE_AVAILABILITY;
        }

        WotbModV3RenderLifecycleEvent payload = {};
        WOTBMOD_V3_INIT_STRUCT(
            payload,
            WOTBMOD_V3_RENDER_VERSION);
        payload.previous_backend = previous.render_backend;
        payload.backend = frameSnapshot.render_backend;
        payload.component_changes = componentChanges;
        payload.previous_device_available =
            previousAvailable ? 1u : 0u;
        payload.device_available = available ? 1u : 0u;
        payload.frame_index = frameSnapshot.frame_index;
        payload.previous_viewport = previous.viewport;
        payload.viewport = frameSnapshot.viewport;

        const auto appendEvent =
            [&](const char* topic) {
                if (pendingCount >=
                    sizeof(pending) / sizeof(pending[0])) {
                    return;
                }
                pending[pendingCount].topic = topic;
                pending[pendingCount].payload = payload;
                ++pendingCount;
            };
        constexpr uint32_t kNativeIdentityChanges =
            WOTBMOD_V3_RENDER_LIFECYCLE_DEVICE |
            WOTBMOD_V3_RENDER_LIFECYCLE_CONTEXT |
            WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN;
        const bool nativeIdentityChanged =
            previousAvailable && available &&
            (componentChanges & kNativeIdentityChanges) != 0u;
        if (previousAvailable &&
            (!available || nativeIdentityChanged)) {
            appendEvent(WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST);
        }
        if (previous.render_backend !=
            frameSnapshot.render_backend) {
            appendEvent(WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED);
        }
        if (available &&
            (!previousAvailable || nativeIdentityChanged)) {
            appendEvent(
                g_renderLifecycle.everAvailable
                    ? WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED
                    : WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED);
        }
        if (g_renderLifecycle.observed &&
            previousAvailable && available &&
            viewportChanged) {
            appendEvent(WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED);
        }

        g_hostFrame = frameSnapshot;
        g_hostFrameValid = true;
        g_lastExternalFrame = frameSnapshot.frame_index;
        g_lastDeltaSeconds = frameSnapshot.delta_seconds;
        g_renderLifecycle.frame = frameSnapshot;
        g_renderLifecycle.observed = true;
        g_renderLifecycle.everAvailable =
            g_renderLifecycle.everAvailable || available;
    }
    if (pendingCount != 0u) {
        const uint32_t previousRole =
            SetCurrentThreadRole(WOTBMOD_V3_THREAD_RENDER);
        for (uint32_t index = 0u; index < pendingCount; ++index) {
            const WotbModV3Result published =
                PublishClientHostEvent(
                    pending[index].topic,
                    &pending[index].payload,
                    sizeof(pending[index].payload));
            if (published != WOTBMOD_V3_OK) {
                RuntimeLog(
                    WOTBMOD_V3_LOG_WARNING,
                    "v3.client-services",
                    "failed to publish a render lifecycle event");
            }
        }
        SetCurrentThreadRole(previousRole);
    }
    PumpManagedUiInput();
    RefreshUiCaptureSnapshot();
    PumpManagedImpactInstances(frameSnapshot.delta_seconds);
    ClientRenderFramePump(
        frameSnapshot.frame_index,
        frameSnapshot.delta_seconds,
        &frameSnapshot);
}

WotbModV3Result ApplyClientHostCameraModifiers(
    uint32_t phase,
    WotbModV3CameraState* inoutState) {
    if (phase > WOTBMOD_V3_CAMERA_MODIFIER_AFTER_GAME ||
        !ValidStruct(inoutState, WOTBMOD_V3_CAMERA_VERSION) ||
        !ValidSceneTransform(&inoutState->transform) ||
        !IsFinite(inoutState->fov_degrees) ||
        inoutState->fov_degrees < 1.0f ||
        inoutState->fov_degrees > 179.0f ||
        !IsFinite(inoutState->near_plane) ||
        inoutState->near_plane < 0.0f ||
        !IsFinite(inoutState->far_plane) ||
        inoutState->far_plane <= inoutState->near_plane ||
        inoutState->mode > WOTBMOD_V3_CAMERA_MODE_CINEMATIC) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    struct CallbackCopy {
        WotbModV3Handle owner;
        int32_t priority;
        std::shared_ptr<SubscriptionCallbackState> state;
    };
    std::vector<CallbackCopy> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        callbacks.reserve(g_subscriptions.size());
        for (const auto& pair : g_subscriptions) {
            const Subscription* subscription = pair.second.get();
            if (!subscription ||
                subscription->magic != kSubscriptionMagic ||
                subscription->kind !=
                    SubscriptionKind::CameraModifier ||
                subscription->phase != phase ||
                !subscription->callbackState) {
                continue;
            }
            callbacks.push_back(
                {subscription->owner,
                 subscription->priority,
                 subscription->callbackState});
        }
    }
    std::stable_sort(
        callbacks.begin(),
        callbacks.end(),
        [](const CallbackCopy& left, const CallbackCopy& right) {
            return left.priority > right.priority;
        });
    WotbModV3Result firstFailure = WOTBMOD_V3_OK;
    for (const CallbackCopy& callback : callbacks) {
        if (!IsModEnabled(callback.owner) ||
            CheckCameraWritePermission(callback.owner) !=
                WOTBMOD_V3_OK ||
            CheckAccess(
                callback.owner,
                CameraPermissionTier(true),
                kCameraWriteContexts,
                nullptr) != WOTBMOD_V3_OK) {
            continue;
        }
        SubscriptionCallbackLease lease(callback.state);
        if (!lease) {
            continue;
        }
        const WotbModV3CameraModifierCallback invoke =
            callback.state->cameraModifierCallback;
        void* const userData = callback.state->userData;
        if (!invoke ||
            EnterModCallbackCoalescible(callback.owner) !=
                WOTBMOD_V3_OK) {
            continue;
        }
        const WotbModV3CameraState previous = *inoutState;
        try {
            invoke(
                callback.owner,
                inoutState,
                userData);
        } catch (...) {
            *inoutState = previous;
            if (firstFailure == WOTBMOD_V3_OK) {
                firstFailure = WOTBMOD_V3_E_CALLBACK_FAULT;
            }
            SetError(
                callback.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "camera modifier threw an exception");
        }
        LeaveModCallback(callback.owner);
        if (!ValidStruct(
                inoutState,
                WOTBMOD_V3_CAMERA_VERSION) ||
            !ValidSceneTransform(&inoutState->transform) ||
            !IsFinite(inoutState->fov_degrees) ||
            inoutState->fov_degrees < 1.0f ||
            inoutState->fov_degrees > 179.0f ||
            !IsFinite(inoutState->near_plane) ||
            inoutState->near_plane < 0.0f ||
            !IsFinite(inoutState->far_plane) ||
            inoutState->far_plane <= inoutState->near_plane ||
            inoutState->mode >
                WOTBMOD_V3_CAMERA_MODE_CINEMATIC) {
            *inoutState = previous;
            if (firstFailure == WOTBMOD_V3_OK) {
                firstFailure = WOTBMOD_V3_E_INVALID_ARGUMENT;
            }
            SetError(
                callback.owner,
                WOTBMOD_V3_E_INVALID_ARGUMENT,
                "camera modifier produced an invalid state");
        }
    }
    return firstFailure;
}

WotbModV3Result NotifyClientHostAudioLifecycle(
    uint64_t nativeAudioObject,
    uint32_t lifecycleKind,
    uint32_t errorCode,
    const char* errorMessage) {
    if (nativeAudioObject == 0u ||
        lifecycleKind < CLIENT_HOST_AUDIO_STARTED ||
        lifecycleKind > CLIENT_HOST_AUDIO_ERROR ||
        (errorMessage &&
         std::memchr(
             errorMessage,
             '\0',
             WOTBMOD_V3_MAX_MESSAGE) == nullptr)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    HostAudioRecord audioRecord = {};
    struct CallbackCopy {
        WotbModV3Handle owner;
        WotbModV3AudioHandle audio;
        std::shared_ptr<SubscriptionCallbackState> state;
    };
    std::vector<CallbackCopy> callbacks;
    AudioLifecycleLease lifecycleLease;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto audio =
            g_hostAudioObjects.find(nativeAudioObject);
        if (audio == g_hostAudioObjects.end()) {
            return WOTBMOD_V3_E_NOT_FOUND;
        }
        audioRecord = audio->second;
        if (!lifecycleLease.Acquire(
                audioRecord.lifecycleState)) {
            return WOTBMOD_V3_E_OBJECT_DESTROYED;
        }
        const SubscriptionKind wantedKind =
            lifecycleKind == CLIENT_HOST_AUDIO_STARTED
                ? SubscriptionKind::AudioStarted
                : (lifecycleKind == CLIENT_HOST_AUDIO_FINISHED
                       ? SubscriptionKind::AudioFinished
                       : SubscriptionKind::AudioError);
        callbacks.reserve(g_subscriptions.size());
        for (const auto& pair : g_subscriptions) {
            const Subscription* subscription = pair.second.get();
            if (!subscription ||
                subscription->magic != kSubscriptionMagic ||
                subscription->kind != wantedKind ||
                subscription->target != audioRecord.handle ||
                !subscription->callbackState) {
                continue;
            }
            callbacks.push_back(
                {subscription->owner,
                 audioRecord.handle,
                 subscription->callbackState});
        }
    }
    const AudioLifecycleTestHook testHook =
        g_audioLifecycleBeforeStateUpdateHook.load(
            std::memory_order_acquire);
    if (testHook) {
        testHook();
    }
    audioRecord.lifecycleState->state.store(
        lifecycleKind == CLIENT_HOST_AUDIO_STARTED
            ? WOTBMOD_V3_AUDIO_STATE_PLAYING
            : (lifecycleKind == CLIENT_HOST_AUDIO_FINISHED
                   ? WOTBMOD_V3_AUDIO_STATE_STOPPED
                   : WOTBMOD_V3_AUDIO_STATE_ERROR),
        std::memory_order_release);
    for (const CallbackCopy& callback : callbacks) {
        if (!IsModEnabled(callback.owner) ||
            CheckAudioCustomPermission(callback.owner) !=
                WOTBMOD_V3_OK ||
            CheckAccess(
                callback.owner,
                WOTBMOD_V3_PERMISSION_SAFE,
                WOTBMOD_V3_CONTEXT_ALL,
                nullptr) != WOTBMOD_V3_OK) {
            continue;
        }
        SubscriptionCallbackLease lease(callback.state);
        if (!lease) {
            continue;
        }
        const WotbModV3AudioLifecycleCallback lifecycleCallback =
            callback.state->audioLifecycleCallback;
        const WotbModV3AudioErrorCallback errorCallback =
            callback.state->audioErrorCallback;
        void* const userData = callback.state->userData;
        if ((lifecycleKind == CLIENT_HOST_AUDIO_ERROR
                 ? errorCallback == nullptr
                 : lifecycleCallback == nullptr) ||
            EnterModCallback(callback.owner) != WOTBMOD_V3_OK) {
            continue;
        }
        try {
            if (lifecycleKind == CLIENT_HOST_AUDIO_ERROR) {
                WotbModV3AudioError error = {};
                WOTBMOD_V3_INIT_STRUCT(
                    error,
                    WOTBMOD_V3_AUDIO_VERSION);
                error.audio = callback.audio;
                error.code = errorCode;
                CopyFixed(
                    error.message,
                    errorMessage ? errorMessage : "");
                errorCallback(
                    callback.owner,
                    &error,
                    userData);
            } else {
                lifecycleCallback(
                    callback.owner,
                    callback.audio,
                    userData);
            }
        } catch (...) {
            SetError(
                callback.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "audio lifecycle callback threw an exception");
        }
        LeaveModCallback(callback.owner);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result NotifyClientHostObservedRpc(
    const WotbModV3ObservedRpc* rpc) {
    if (!ValidStruct(rpc, WOTBMOD_V3_BIGWORLD_RPC_VERSION) ||
        (rpc->direction != WOTBMOD_V3_RPC_INCOMING &&
         rpc->direction != WOTBMOD_V3_RPC_OUTGOING) ||
        !FixedTextTerminated(
            rpc->entity_type,
            sizeof(rpc->entity_type)) ||
        !FixedTextTerminated(
            rpc->method_name,
            sizeof(rpc->method_name)) ||
        rpc->method_name[0] == '\0') {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    struct CallbackCopy {
        WotbModV3Handle owner;
        std::shared_ptr<SubscriptionCallbackState> state;
    };
    std::vector<CallbackCopy> callbacks;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        callbacks.reserve(g_subscriptions.size());
        for (const auto& pair : g_subscriptions) {
            const Subscription* subscription = pair.second.get();
            if (!subscription ||
                subscription->magic != kSubscriptionMagic ||
                subscription->kind !=
                    SubscriptionKind::RpcObserved ||
                !subscription->callbackState ||
                (subscription->eventType & rpc->direction) == 0u ||
                (!subscription->methodFilter.empty() &&
                 subscription->methodFilter != rpc->method_name)) {
                continue;
            }
            callbacks.push_back(
                {subscription->owner,
                 subscription->callbackState});
        }
    }
    for (const CallbackCopy& callback : callbacks) {
        if (!IsModEnabled(callback.owner) ||
            CheckNamedPermission(
                callback.owner,
                "bigworld.observe",
                WOTBMOD_V3_PERMISSION_REVIEWED) != WOTBMOD_V3_OK ||
            CheckAccess(
                callback.owner,
                WOTBMOD_V3_PERMISSION_REVIEWED,
                kEntityPublicContexts,
                nullptr) != WOTBMOD_V3_OK) {
            continue;
        }
        SubscriptionCallbackLease lease(callback.state);
        if (!lease) {
            continue;
        }
        const WotbModV3RpcObservedCallback invoke =
            callback.state->rpcObservedCallback;
        void* const userData = callback.state->userData;
        if (!invoke ||
            EnterModCallbackCoalescible(callback.owner) !=
                WOTBMOD_V3_OK) {
            continue;
        }
        try {
            invoke(
                callback.owner,
                rpc,
                userData);
        } catch (...) {
            SetError(
                callback.owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "RPC observer callback threw an exception");
        }
        LeaveModCallback(callback.owner);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result PublishClientHostEvent(
    const char* event,
    const void* payload,
    uint32_t payloadSize) {
    if (!ValidText(event, WOTBMOD_V3_MAX_NAME, false) ||
        (!payload && payloadSize != 0u)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const bool renderDeviceCreated =
        std::strcmp(
            event,
            WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED) == 0;
    const bool renderDeviceLost =
        std::strcmp(
            event,
            WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST) == 0;
    const bool renderDeviceRestored =
        std::strcmp(
            event,
            WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED) == 0;
    const bool renderSwapchainResized =
        std::strcmp(
            event,
            WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED) == 0;
    const bool renderBackendChanged =
        std::strcmp(
            event,
            WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED) == 0;
    if (renderDeviceCreated ||
        renderDeviceLost ||
        renderDeviceRestored ||
        renderSwapchainResized ||
        renderBackendChanged) {
        if (!payload ||
            payloadSize <
                sizeof(WotbModV3RenderLifecycleEvent)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const WotbModV3RenderLifecycleEvent* render =
            static_cast<const WotbModV3RenderLifecycleEvent*>(
                payload);
        constexpr uint32_t kRenderLifecycleChanges =
            WOTBMOD_V3_RENDER_LIFECYCLE_BACKEND |
            WOTBMOD_V3_RENDER_LIFECYCLE_DEVICE |
            WOTBMOD_V3_RENDER_LIFECYCLE_CONTEXT |
            WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN |
            WOTBMOD_V3_RENDER_LIFECYCLE_VIEWPORT |
            WOTBMOD_V3_RENDER_LIFECYCLE_AVAILABILITY;
        const bool previousBackendValid =
            render->previous_backend ==
                WOTBMOD_V3_RENDER_BACKEND_NONE ||
            render->previous_backend ==
                WOTBMOD_V3_RENDER_BACKEND_D3D11;
        const bool backendValid =
            render->backend ==
                WOTBMOD_V3_RENDER_BACKEND_NONE ||
            render->backend ==
                WOTBMOD_V3_RENDER_BACKEND_D3D11;
        const bool availabilityValid =
            render->previous_device_available <= 1u &&
            render->device_available <= 1u &&
            (render->previous_device_available != 0u) ==
                (render->previous_backend ==
                 WOTBMOD_V3_RENDER_BACKEND_D3D11) &&
            (render->device_available != 0u) ==
                (render->backend ==
                 WOTBMOD_V3_RENDER_BACKEND_D3D11);
        const bool viewportValid =
            IsFinite(render->previous_viewport) &&
            render->previous_viewport.width >= 0.0f &&
            render->previous_viewport.height >= 0.0f &&
            IsFinite(render->viewport) &&
            render->viewport.width >= 0.0f &&
            render->viewport.height >= 0.0f;
        const bool resizeChanged =
            render->previous_viewport.width !=
                render->viewport.width ||
            render->previous_viewport.height !=
                render->viewport.height;
        constexpr uint32_t kNativeIdentityChanges =
            WOTBMOD_V3_RENDER_LIFECYCLE_DEVICE |
            WOTBMOD_V3_RENDER_LIFECYCLE_CONTEXT |
            WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN;
        const bool eventSemanticsValid =
            (renderDeviceCreated &&
             render->previous_device_available == 0u &&
             render->device_available == 1u) ||
            (renderDeviceRestored &&
             render->device_available == 1u &&
             (render->previous_device_available == 0u ||
              (render->previous_device_available == 1u &&
               (render->component_changes &
                kNativeIdentityChanges) != 0u))) ||
            (renderDeviceLost &&
             render->previous_device_available == 1u &&
             ((render->device_available == 0u) ||
              (render->component_changes &
               kNativeIdentityChanges) != 0u)) ||
            (renderSwapchainResized &&
             render->previous_device_available == 1u &&
             render->device_available == 1u &&
             resizeChanged) ||
            (renderBackendChanged &&
             render->previous_backend != render->backend);
        if (!ValidStruct(
                render,
                WOTBMOD_V3_RENDER_VERSION) ||
            !previousBackendValid ||
            !backendValid ||
            !availabilityValid ||
            !viewportValid ||
            render->reserved != 0u ||
            render->component_changes == 0u ||
            (render->component_changes &
             ~kRenderLifecycleChanges) != 0u ||
            !eventSemanticsValid) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
    } else if (std::strcmp(
            event,
            WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED) == 0) {
        if (!payload ||
            payloadSize < sizeof(WotbModV3LocalShellFiredEvent)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const WotbModV3LocalShellFiredEvent* shell =
            static_cast<const WotbModV3LocalShellFiredEvent*>(
                payload);
        if (!ValidStruct(
                shell,
                WOTBMOD_V3_PROJECTILE_VERSION) ||
            !IsFinite(shell->origin) ||
            !IsFinite(shell->visible_direction)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        HostProjectileRecord projectile = {};
        if (!GetHostProjectile(shell->projectile, &projectile) ||
            projectile.ownerScope !=
                WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
    } else if (
        std::strcmp(
            event,
            WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED) == 0 ||
        std::strcmp(
            event,
            WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED) == 0) {
        if (!payload ||
            payloadSize < sizeof(WotbModV3VisibleTracerEvent)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const WotbModV3VisibleTracerEvent* tracer =
            static_cast<const WotbModV3VisibleTracerEvent*>(
                payload);
        if (!ValidStruct(
                tracer,
                WOTBMOD_V3_PROJECTILE_VERSION) ||
            tracer->owner_scope >
                WOTBMOD_V3_PROJECTILE_OWNER_REPLAY ||
            !IsFinite(tracer->visible_position) ||
            !IsFinite(tracer->visible_direction)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        HostProjectileRecord projectile = {};
        if (!GetHostProjectile(tracer->projectile, &projectile) ||
            projectile.ownerScope != tracer->owner_scope) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
    }
    return PublishSystemEvent(event, payload, payloadSize, 0u);
}

WotbModV3Result RegisterClientHostPublicEntity(
    const ClientHostPublicEntity* entity,
    WotbModV3EntityHandle* outHandle) {
    if (!ValidClientHostPublicEntity(entity) || !outHandle) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto existing =
        g_hostEntityTokens.find(entity->native_token);
    if (existing != g_hostEntityTokens.end()) {
        HostPublicEntityRecord& record =
            g_hostEntities[existing->second];
        record.nativeToken = entity->native_token;
        record.validFields = entity->valid_fields;
        record.snapshot = SanitizePublicEntitySnapshot(
            entity->snapshot,
            entity->valid_fields);
        record.extras = SanitizePublicEntityExtras(
            entity->extras,
            entity->valid_fields);
        record.snapshot.handle = existing->second;
        *outHandle = existing->second;
        return WOTBMOD_V3_OK;
    }
    constexpr uint64_t kEntityHandlePrefix =
        UINT64_C(0xE300000000000000);
    WotbModV3EntityHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    do {
        const uint64_t sequence =
            g_nextHostEntityHandle.fetch_add(
                1u,
                std::memory_order_relaxed);
        handle = kEntityHandlePrefix |
                 (sequence & UINT64_C(0x00FFFFFFFFFFFFFF));
    } while (
        handle == WOTBMOD_V3_INVALID_HANDLE ||
        g_hostEntities.find(handle) != g_hostEntities.end());
    HostPublicEntityRecord record = {};
    record.nativeToken = entity->native_token;
    record.validFields = entity->valid_fields;
    record.snapshot = SanitizePublicEntitySnapshot(
        entity->snapshot,
        entity->valid_fields);
    record.extras = SanitizePublicEntityExtras(
        entity->extras,
        entity->valid_fields);
    record.snapshot.handle = handle;
    g_hostEntities.emplace(handle, record);
    g_hostEntityTokens.emplace(entity->native_token, handle);
    *outHandle = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UpdateClientHostPublicEntity(
    WotbModV3EntityHandle handle,
    const ClientHostPublicEntity* entity) {
    if (handle == WOTBMOD_V3_INVALID_HANDLE ||
        !ValidClientHostPublicEntity(entity)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    WotbModV3PublicEntitySnapshot snapshot =
        SanitizePublicEntitySnapshot(
            entity->snapshot,
            entity->valid_fields);
    const ClientHostPublicEntityExtras extras =
        SanitizePublicEntityExtras(
            entity->extras,
            entity->valid_fields);
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto found = g_hostEntities.find(handle);
        if (found == g_hostEntities.end()) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        if (found->second.nativeToken != entity->native_token) {
            return WOTBMOD_V3_E_CONFLICT;
        }
        snapshot.handle = handle;
        found->second.validFields = entity->valid_fields;
        found->second.snapshot = snapshot;
        found->second.extras = extras;
    }
    DispatchEntityPropertyCallbacks(
        handle,
        snapshot,
        extras,
        entity->valid_fields);
    return WOTBMOD_V3_OK;
}

WotbModV3Result RemoveClientHostPublicEntity(
    WotbModV3EntityHandle handle) {
    if (handle == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    WotbModV3PublicEntitySnapshot removed = {};
    uint64_t validFields = 0u;
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto found = g_hostEntities.find(handle);
        if (found == g_hostEntities.end()) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        removed = found->second.snapshot;
        validFields = found->second.validFields;
        removed.visible_to_player = 0u;
        g_hostEntityTokens.erase(found->second.nativeToken);
        g_hostEntities.erase(found);
    }
    DispatchEntityPropertyCallbacks(
        handle, removed, ClientHostPublicEntityExtras{}, validFields);
    return WOTBMOD_V3_OK;
}

WotbModV3Result RegisterClientHostProjectile(
    const ClientHostProjectile* projectile,
    WotbModV3ProjectileHandle* outHandle) {
    if (!ValidClientHostProjectile(projectile) || !outHandle) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto existing =
        g_hostProjectileTokens.find(projectile->native_token);
    if (existing != g_hostProjectileTokens.end()) {
        HostProjectileRecord& record =
            g_hostProjectiles[existing->second];
        AssignHostProjectileRecord(&record, *projectile);
        *outHandle = existing->second;
        return WOTBMOD_V3_OK;
    }
    constexpr uint64_t kProjectileHandlePrefix =
        UINT64_C(0xE400000000000000);
    WotbModV3ProjectileHandle handle =
        WOTBMOD_V3_INVALID_HANDLE;
    do {
        const uint64_t sequence =
            g_nextHostProjectileHandle.fetch_add(
                1u,
                std::memory_order_relaxed);
        handle = kProjectileHandlePrefix |
                 (sequence & UINT64_C(0x00FFFFFFFFFFFFFF));
    } while (
        handle == WOTBMOD_V3_INVALID_HANDLE ||
        g_hostProjectiles.find(handle) !=
            g_hostProjectiles.end());
    HostProjectileRecord record = {};
    AssignHostProjectileRecord(&record, *projectile);
    g_hostProjectiles.emplace(handle, record);
    g_hostProjectileTokens.emplace(
        projectile->native_token,
        handle);
    *outHandle = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result UpdateClientHostProjectile(
    WotbModV3ProjectileHandle handle,
    const ClientHostProjectile* projectile) {
    if (handle == WOTBMOD_V3_INVALID_HANDLE ||
        !ValidClientHostProjectile(projectile)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lock(g_clientMutex);
    const auto found = g_hostProjectiles.find(handle);
    if (found == g_hostProjectiles.end()) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    if (found->second.nativeToken != projectile->native_token) {
        return WOTBMOD_V3_E_CONFLICT;
    }
    AssignHostProjectileRecord(&found->second, *projectile);
    return WOTBMOD_V3_OK;
}

WotbModV3Result RemoveClientHostProjectile(
    WotbModV3ProjectileHandle handle,
    uint32_t reason) {
    if (handle == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    HostProjectileRecord removed = {};
    {
        std::lock_guard<std::mutex> lock(g_clientMutex);
        const auto found = g_hostProjectiles.find(handle);
        if (found == g_hostProjectiles.end()) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        removed = found->second;
        g_hostProjectileTokens.erase(found->second.nativeToken);
        g_hostProjectiles.erase(found);
    }
    const uint32_t previousState = removed.lifecycleState;
    removed.lifecycleState = WOTBMOD_V3_PROJECTILE_STATE_DESTROYED;
    WotbModV3ProjectileLifecycleEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_PROJECTILE_VERSION_2;
    event.previous_state = previousState;
    event.reason = reason;
    event.snapshot = ProjectileSnapshotFromRecord(handle, removed);
    return PublishSystemEvent(
        WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED,
        &event,
        sizeof(event),
        0u);
}

WotbModV3Result PublishClientHostProjectileLifecycle(
    WotbModV3ProjectileHandle handle,
    uint32_t previousState,
    uint32_t reason) {
    HostProjectileRecord record = {};
    if (!GetHostProjectile(handle, &record)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    const WotbModV3ProjectileSnapshot snapshot =
        ProjectileSnapshotFromRecord(handle, record);
    WotbModV3ProjectileLifecycleEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_PROJECTILE_VERSION_2;
    event.previous_state = previousState;
    event.reason = reason;
    event.snapshot = snapshot;
    const char* topic = nullptr;
    switch (snapshot.lifecycle_state) {
        case WOTBMOD_V3_PROJECTILE_STATE_CREATED:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_CREATED;
            break;
        case WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_UPDATED;
            break;
        case WOTBMOD_V3_PROJECTILE_STATE_IMPACTED:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED;
            SpawnImpactVisuals(snapshot);
            break;
        case WOTBMOD_V3_PROJECTILE_STATE_DESTROYED:
            topic = WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED;
            break;
        default:
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return PublishSystemEvent(topic, &event, sizeof(event), 0u);
}

void RegisterClientServices() {
    {
        std::lock_guard<std::mutex> frameLock(
            g_clientFramePumpMutex);
        std::lock_guard<std::mutex> lock(g_clientMutex);
        g_clientServicesRunning = true;
        g_hostBackendAcceptingInvocations =
            g_hostBackendInstalled && g_hostBackend.invoke;
        g_acceptingHostReleaseRetries.store(
            true,
            std::memory_order_release);
    }
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_UI,
         WOTBMOD_V3_UI_VERSION,
         &kUiApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_UI,
         WOTBMOD_V3_UI_VERSION_3,
         &kUiApiV3,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_RENDER,
         WOTBMOD_V3_RENDER_VERSION,
         &kRenderApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_RENDER_NATIVE,
         WOTBMOD_V3_RENDER_NATIVE_VERSION,
         &kRenderNativeApi,
         WOTBMOD_V3_PERMISSION_UNSAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_CAMERA,
         WOTBMOD_V3_CAMERA_VERSION,
         &kCameraApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_HANGAR |
             WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_SCENE,
         WOTBMOD_V3_SCENE_VERSION,
         &kSceneApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_AUDIO,
         WOTBMOD_V3_AUDIO_VERSION,
         &kAudioApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
         WOTBMOD_V3_VEHICLE_VISUAL_VERSION,
         &kVehicleVisualApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_HANGAR |
             WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
         WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2,
         &kVehicleVisualApiV2,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_HANGAR |
             WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
         WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION,
         &kGameplayCameraApi,
         WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
         WOTBMOD_V3_CONTEXT_HANGAR |
             WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
         WOTBMOD_V3_GAMEPLAY_HUD_VERSION,
         &kGameplayHudApi,
         WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
         WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_GAMEPLAY_HANGAR,
         WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION,
         &kGameplayHangarApi,
         WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
         WOTBMOD_V3_CONTEXT_HANGAR,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY,
         WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION,
         &kGameplayReplayApi,
         WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
         WOTBMOD_V3_CONTEXT_REPLAY,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
         WOTBMOD_V3_ENTITY_PUBLIC_VERSION,
         &kEntityPublicApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_BIGWORLD_RPC,
         WOTBMOD_V3_BIGWORLD_RPC_VERSION,
         &kBigWorldRpcApi,
         WOTBMOD_V3_PERMISSION_REVIEWED,
         WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_PROJECTILE,
         WOTBMOD_V3_PROJECTILE_VERSION_2,
         &kProjectileApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_BATTLE |
             WOTBMOD_V3_CONTEXT_REPLAY |
             WOTBMOD_V3_CONTEXT_TRAINING,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_CLIENT,
         WOTBMOD_V3_CLIENT_VERSION,
         &kClientApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_DEVICE,
         WOTBMOD_V3_DEVICE_VERSION,
         &kDeviceApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    /*
     * The five interfaces declared 2026-08-16. Each registers at the version
     * of its own table, and each takes its implementation status from
     * kInterfaceAvailabilityDefaults in wotb_mod_v3_runtime.cpp -- which is
     * UNAVAILABLE, because in a build with no native backend every slot here
     * answers WOTBMOD_V3_E_NOT_SUPPORTED and query_interface must say so
     * before a mod ever holds the table. PublishDeclaredInterfaceAvailability
     * below raises a group to DEGRADED only once a real backend for it is
     * installed.
     *
     * The permission tier on each registration is the tier of the named
     * grant its slots enforce, so a mod below that tier cannot even obtain
     * the table. The named grant itself is enforced again inside every slot.
     */
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_UI_READ,
         WOTBMOD_V3_UI_VERSION_4,
         &kUiApiV4,
         WOTBMOD_V3_PERMISSION_SAFE,
         kUiReadContexts,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_CAMERA_STATE,
         WOTBMOD_V3_CAMERA_VERSION_2,
         &kCameraApiV2,
         WOTBMOD_V3_PERMISSION_REVIEWED,
         kCameraStateContexts,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_AUDIO_INTERCEPT,
         WOTBMOD_V3_AUDIO_VERSION_3,
         &kAudioApiV3,
         WOTBMOD_V3_PERMISSION_SAFE,
         kAudioInterceptContexts,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_SCENE_ENUMERATE,
         WOTBMOD_V3_SCENE_VERSION_2,
         &kSceneApiV2,
         WOTBMOD_V3_PERMISSION_REVIEWED,
         kSceneEnumerateContexts,
         nullptr});
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_TRACER,
         WOTBMOD_V3_TRACER_VERSION,
         &kTracerApi,
         WOTBMOD_V3_PERMISSION_REVIEWED,
         kTracerContexts,
         nullptr});
    GesSetDeclaredBackendAccessor(&GetClientHostDeclaredBackend);
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_GES,
         WOTBMOD_V3_GES_VERSION,
         &GesApi(),
         WOTBMOD_V3_PERMISSION_SAFE,
         kGesContexts,
         nullptr});
    SessionClusterSetDeclaredBackendAccessor(&GetClientHostDeclaredBackend);
    RegisterClientInterface(
        {WOTBMOD_V3_IFACE_SESSION_CLUSTER,
         WOTBMOD_V3_SESSION_CLUSTER_VERSION,
         &SessionClusterApi(),
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    PublishDeclaredInterfaceAvailability();
    const WotbModV3Result frameResult =
        RegisterFramePump(&ClientRuntimeFramePump);
    if (frameResult != WOTBMOD_V3_OK &&
        frameResult != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.client-services",
            "failed to register client render frame pump");
    }
    const WotbModV3Result shutdownResult =
        RegisterShutdownHook(&ClientServicesShutdown);
    if (shutdownResult != WOTBMOD_V3_OK &&
        shutdownResult != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.client-services",
            "failed to register client shutdown hook");
    }
    const WotbModV3Result ownerStoppingResult =
        RegisterOwnerStoppingHook(
            &ClientSubscriptionOwnerStopping);
    if (ownerStoppingResult != WOTBMOD_V3_OK &&
        ownerStoppingResult != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.client-services",
            "failed to register subscription owner stopping hook");
    }
    const WotbModV3Result lifecycleResult =
        RegisterLifecycleStateHook(
            &ClientSubscriptionLifecycleChanged);
    if (lifecycleResult != WOTBMOD_V3_OK &&
        lifecycleResult != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.client-services",
            "failed to register subscription lifecycle hook");
    }
}

}  // namespace v3
}  // namespace wotbmod
