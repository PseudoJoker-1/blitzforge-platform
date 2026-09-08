#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/audio_v2.h"
#include "../include/wotbmod/bigworld_rpc_v1.h"
#include "../include/wotbmod/client_v1.h"
#include "../include/wotbmod/device_v1.h"
#include "../include/wotbmod/events_v1.h"
#include "../include/wotbmod/ges_v1.h"
#include "../include/wotbmod/gameplay_camera_v1.h"
#include "../include/wotbmod/handles_v1.h"
#include "../include/wotbmod/projectile_v2.h"
#include "../include/wotbmod/render_v1.h"
#include "../include/wotbmod/vehicle_visual_v1.h"
#include "../src/v3/client_services_backend.h"
#include "../src/v3/wotb_mod_v3_internal.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace wotbmod {
namespace v3 {

void SetAudioLifecycleBeforeStateUpdateHookForTesting(
    void (*hook)());

}  // namespace v3
}  // namespace wotbmod

namespace {

using wotbmod::v3::ClientHostBackend;
using wotbmod::v3::ClientHostFrame;
using wotbmod::v3::ClientHostObjectRequest;
using wotbmod::v3::ClientHostObjectResponse;
using wotbmod::v3::ClientHostPublicEntity;
using wotbmod::v3::ClientHostRpcObserverRequest;

constexpr uint64_t kCameraObject = UINT64_C(0xC0DEC0DE);
constexpr float kDefaultFov = 70.0f;

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;
uint32_t g_cameraGetActiveCalls = 0u;
uint32_t g_cameraGetFovCalls = 0u;
uint32_t g_cameraSetFovCalls = 0u;
uint32_t g_cameraResetFovCalls = 0u;
std::atomic<uint32_t> g_rpcSubscribeCalls{0u};
std::atomic<uint32_t> g_rpcUnsubscribeCalls{0u};
std::atomic<uint32_t> g_rpcUnsubscribeAttempts{0u};
std::atomic<bool> g_failNextRpcUnsubscribe{false};
std::atomic<uint32_t> g_nextRpcUnsubscribeResult{WOTBMOD_V3_OK};
std::atomic<bool> g_failAllRpcUnsubscribes{false};
std::atomic<uint64_t> g_nextRpcSubscription{UINT64_C(0xB100)};
constexpr uint64_t kAudioObject = UINT64_C(0xA0D10001);
std::atomic<uint32_t> g_audioDestroyCalls{0u};
struct BackendProbe {
    uint64_t tokenPrefix = 0u;
    std::atomic<uint32_t> unsubscribeAttempts{0u};
    std::atomic<uint32_t> unsubscribeCalls{0u};
    std::atomic<uint32_t> wrongGenerationCalls{0u};
};
struct BackendInvocationGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool blockClientLeave = false;
    bool clientLeaveEntered = false;
    bool releaseClientLeave = false;
    bool pumpFrameBeforeClientLeaveReturns = false;
    bool blockRpcSubscribe = false;
    bool rpcSubscribeEntered = false;
    bool releaseRpcSubscribe = false;
    bool blockRpcUnsubscribe = false;
    bool rpcUnsubscribeEntered = false;
    bool releaseRpcUnsubscribe = false;
};
BackendInvocationGate g_backendGate;
const WotbModV3HandlesApiV1* g_reentrantHandles = nullptr;
WotbModV3Token g_reentrantReleaseToken =
    WOTBMOD_V3_INVALID_HANDLE;
std::atomic<bool> g_reentrantReleaseOnUnsubscribe{false};
struct LifecycleGate {
    std::mutex mutex;
    std::condition_variable cv;
    WotbModV3Handle target = WOTBMOD_V3_INVALID_HANDLE;
    bool unloadedEntered = false;
    bool releaseUnloaded = false;
};
LifecycleGate g_lifecycleGate;
struct AudioLifecycleGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
};
AudioLifecycleGate g_audioLifecycleGate;
uint32_t g_renderCalls = 0u;
uint32_t g_lastRenderPhase = UINT32_MAX;
uint64_t g_lastRenderFrame = 0u;
constexpr uint32_t kMaxRenderLifecycleEvents = 16u;
uint32_t g_renderLifecycleCount = 0u;
bool g_renderLifecyclePayloadValid = true;
char g_renderLifecycleTopics
    [kMaxRenderLifecycleEvents][WOTBMOD_V3_MAX_EVENT_TOPIC] = {};
WotbModV3RenderLifecycleEvent
    g_renderLifecyclePayloads[kMaxRenderLifecycleEvents] = {};
uint32_t
    g_renderLifecycleThreadRoles[kMaxRenderLifecycleEvents] = {};
enum : uint32_t {
    kSerializedLifecycleObservation = 1u,
    kSerializedRenderObservation = 2u
};
struct SerializedFrameObservation {
    uint64_t frameIndex = 0u;
    uint32_t kind = 0u;
    float viewportWidth = 0.0f;
};
constexpr uint32_t kMaxSerializedFrameObservations = 8u;
std::mutex g_serializedFrameMutex;
std::condition_variable g_serializedFrameCv;
SerializedFrameObservation
    g_serializedFrameObservations
        [kMaxSerializedFrameObservations] = {};
uint32_t g_serializedFrameObservationCount = 0u;
uint32_t g_serializedRenderCallbacksActive = 0u;
bool g_firstSerializedRenderEntered = false;
bool g_releaseFirstSerializedRender = false;
bool g_secondSerializedRenderEntered = false;
bool g_serializedRenderOverlap = false;
struct BlockingCallbackGate {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    uint32_t calls = 0u;
};
struct SelfRenderUnsubscribeContext {
    const WotbModV3RenderApiV1* api = nullptr;
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    uint32_t calls = 0u;
};
struct SelfRpcUnsubscribeContext {
    const WotbModV3BigWorldRpcApiV1* api = nullptr;
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    uint32_t calls = 0u;
};
struct ConcurrentSelfRpcUnsubscribeContext {
    const WotbModV3BigWorldRpcApiV1* api = nullptr;
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    uint32_t calls = 0u;
};
struct ConcurrentRenderSelfOperationContext {
    const WotbModV3RenderApiV1* api = nullptr;
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool externalUnregisterStarted = false;
    uint32_t calls = 0u;
};
uint32_t g_leaveToHangarCalls = 0u;
float g_cameraFov = kDefaultFov;
bool g_supportCameraReset = false;
uint32_t g_missingBindingCount = 0u;
bool g_sawAdvancedCameraGap = false;
bool g_sawTracerStyleGap = false;
bool g_sawInputIngressGap = false;
bool g_sawEntityOptionalFieldsGap = false;
uint32_t g_entityPropertyCalls = 0u;
int64_t g_lastEntityPropertyInteger = 0;
uint32_t g_partialEnumerationSeen = 0u;
bool g_partialEnumerationSanitized = false;

#define CHECK(expression)                                                \
    do {                                                                 \
        ++g_checks;                                                      \
        if (!(expression)) {                                             \
            ++g_failures;                                                \
            std::fprintf(                                                \
                stderr,                                                  \
                "check failed at line %d: %s\n",                       \
                __LINE__,                                                \
                #expression);                                            \
        }                                                                \
    } while (0)

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.001f;
}

void Copy(char* output, size_t capacity, const char* value) {
#if defined(_MSC_VER)
    strncpy_s(output, capacity, value, _TRUNCATE);
#else
    std::strncpy(output, value, capacity - 1u);
    output[capacity - 1u] = '\0';
#endif
}

bool ValidRequest(
    const void* request,
    uint32_t requestSize,
    const ClientHostObjectRequest** output) {
    if (!request || requestSize < sizeof(ClientHostObjectRequest) ||
        !output) {
        return false;
    }
    const ClientHostObjectRequest* typed =
        static_cast<const ClientHostObjectRequest*>(request);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *output = typed;
    return true;
}

bool ValidResponse(
    void* response,
    uint32_t responseSize,
    ClientHostObjectResponse** output) {
    if (!response || responseSize < sizeof(ClientHostObjectResponse) ||
        !output) {
        return false;
    }
    ClientHostObjectResponse* typed =
        static_cast<ClientHostObjectResponse*>(response);
    if (typed->struct_size < sizeof(*typed) ||
        typed->api_version !=
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION) {
        return false;
    }
    *output = typed;
    return true;
}

WotbModV3Result WOTBMOD_V3_CALL InvokeClientHost(
    void* userData,
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t requestSize,
    void* response,
    uint32_t responseSize) {
    if (!operation) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    BackendProbe* const probe =
        static_cast<BackendProbe*>(userData);
    if (std::strcmp(operation, "client_leave_to_hangar") == 0) {
        if (request || requestSize != 0u ||
            response || responseSize != 0u) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        bool pumpFrame = false;
        {
            std::unique_lock<std::mutex> lock(
                g_backendGate.mutex);
            if (g_backendGate.blockClientLeave) {
                g_backendGate.clientLeaveEntered = true;
                g_backendGate.cv.notify_all();
                g_backendGate.cv.wait(
                    lock,
                    []() {
                        return g_backendGate.releaseClientLeave;
                    });
            }
            pumpFrame =
                g_backendGate.pumpFrameBeforeClientLeaveReturns;
            g_backendGate.pumpFrameBeforeClientLeaveReturns = false;
        }
        if (pumpFrame) {
            ClientHostFrame frame = {};
            frame.struct_size = sizeof(frame);
            frame.api_version =
                WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
            frame.render_backend =
                WOTBMOD_V3_RENDER_BACKEND_NONE;
            frame.frame_index = 77u;
            frame.delta_seconds = 0.016;
            wotbmod::v3::PumpClientHostFrame(&frame);
        }
        ++g_leaveToHangarCalls;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "rpc_subscribe_observed") == 0) {
        if (!request ||
            requestSize < sizeof(ClientHostRpcObserverRequest)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const ClientHostRpcObserverRequest* observer =
            static_cast<const ClientHostRpcObserverRequest*>(
                request);
        ClientHostObjectResponse* objectResponse = nullptr;
        if (observer->struct_size < sizeof(*observer) ||
            observer->api_version !=
                WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION ||
            observer->direction_mask !=
                WOTBMOD_V3_RPC_INCOMING ||
            !ValidResponse(
                response,
                responseSize,
                &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ++g_rpcSubscribeCalls;
        {
            std::unique_lock<std::mutex> lock(
                g_backendGate.mutex);
            if (g_backendGate.blockRpcSubscribe) {
                g_backendGate.rpcSubscribeEntered = true;
                g_backendGate.cv.notify_all();
                g_backendGate.cv.wait(
                    lock,
                    []() {
                        return g_backendGate.releaseRpcSubscribe;
                    });
            }
        }
        const uint64_t sequence =
            g_nextRpcSubscription.fetch_add(
                1u,
                std::memory_order_relaxed) +
            1u;
        objectResponse->object =
            probe
                ? (probe->tokenPrefix |
                   (sequence & UINT64_C(0x0000FFFFFFFFFFFF)))
                : sequence;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "audio_create") == 0 ||
        std::strcmp(operation, "audio_create_stream") == 0) {
        const ClientHostObjectRequest* audioRequest = nullptr;
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidRequest(
                request,
                requestSize,
                &audioRequest) ||
            !ValidResponse(
                response,
                responseSize,
                &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        objectResponse->object = kAudioObject;
        return WOTBMOD_V3_OK;
    }
    const ClientHostObjectRequest* objectRequest = nullptr;
    if (!ValidRequest(request, requestSize, &objectRequest)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (std::strcmp(operation, "rpc_unsubscribe_observed") == 0) {
        if (objectRequest->object == 0u ||
            response || responseSize != 0u) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ++g_rpcUnsubscribeAttempts;
        if (probe) {
            ++probe->unsubscribeAttempts;
            if ((objectRequest->object &
                 UINT64_C(0xFFFF000000000000)) !=
                probe->tokenPrefix) {
                ++probe->wrongGenerationCalls;
            }
        }
        {
            std::unique_lock<std::mutex> lock(
                g_backendGate.mutex);
            if (g_backendGate.blockRpcUnsubscribe) {
                g_backendGate.rpcUnsubscribeEntered = true;
                g_backendGate.cv.notify_all();
                g_backendGate.cv.wait(
                    lock,
                    []() {
                        return g_backendGate.releaseRpcUnsubscribe;
                    });
            }
        }
        if (g_reentrantReleaseOnUnsubscribe.exchange(
                false,
                std::memory_order_acq_rel)) {
            if (g_reentrantHandles &&
                g_reentrantReleaseToken !=
                    WOTBMOD_V3_INVALID_HANDLE) {
                g_reentrantHandles->release(
                    mod,
                    g_reentrantReleaseToken);
            }
            return WOTBMOD_V3_E_PLATFORM;
        }
        if (g_failAllRpcUnsubscribes.load(
                std::memory_order_acquire)) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        const WotbModV3Result configuredResult =
            static_cast<WotbModV3Result>(
                g_nextRpcUnsubscribeResult.exchange(
                    WOTBMOD_V3_OK,
                    std::memory_order_acq_rel));
        if (configuredResult != WOTBMOD_V3_OK) {
            return configuredResult;
        }
        if (g_failNextRpcUnsubscribe.exchange(
                false,
                std::memory_order_acq_rel)) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        ++g_rpcUnsubscribeCalls;
        if (probe) {
            ++probe->unsubscribeCalls;
        }
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "audio_destroy") == 0) {
        if (objectRequest->object != kAudioObject ||
            response || responseSize != 0u) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ++g_audioDestroyCalls;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_get_active") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(
                response, responseSize, &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ++g_cameraGetActiveCalls;
        objectResponse->object = kCameraObject;
        return WOTBMOD_V3_OK;
    }
    if (objectRequest->object != kCameraObject) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    if (std::strcmp(operation, "camera_get_fov") == 0) {
        ClientHostObjectResponse* objectResponse = nullptr;
        if (!ValidResponse(
                response, responseSize, &objectResponse)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        ++g_cameraGetFovCalls;
        objectResponse->value_f64 = g_cameraFov;
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_set_fov") == 0) {
        ++g_cameraSetFovCalls;
        g_cameraFov = static_cast<float>(objectRequest->scalar0);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(operation, "camera_reset_fov") == 0) {
        ++g_cameraResetFovCalls;
        if (!g_supportCameraReset) {
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        }
        g_cameraFov = kDefaultFov;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(info->id, sizeof(info->id), "tests.client-truthful-slices");
    Copy(info->name, sizeof(info->name), "Client truthful slices test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL OwnerRaceEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(
        info->id,
        sizeof(info->id),
        "tests.client-owner-race");
    Copy(
        info->name,
        sizeof(info->name),
        "Client owner race test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

void TestLifecycleHook(
    WotbModV3Handle mod,
    uint32_t transition) {
    std::unique_lock<std::mutex> lock(g_lifecycleGate.mutex);
    if (mod != g_lifecycleGate.target ||
        transition !=
            wotbmod::v3::LIFECYCLE_TRANSITION_UNLOADED) {
        return;
    }
    g_lifecycleGate.unloadedEntered = true;
    g_lifecycleGate.cv.notify_all();
    g_lifecycleGate.cv.wait(
        lock,
        []() {
            return g_lifecycleGate.releaseUnloaded;
        });
}

void AudioLifecyclePauseHook() {
    std::unique_lock<std::mutex> lock(
        g_audioLifecycleGate.mutex);
    g_audioLifecycleGate.entered = true;
    g_audioLifecycleGate.cv.notify_all();
    g_audioLifecycleGate.cv.wait(
        lock,
        []() {
            return g_audioLifecycleGate.release;
        });
}

ClientHostPublicEntity PublicEntity(
    uint64_t nativeToken,
    uint32_t publicId,
    uint32_t type,
    uint32_t visible,
    uint32_t local,
    uint32_t team,
    const char* displayName,
    WotbModV3Vec3 position) {
    ClientHostPublicEntity entity = {};
    entity.struct_size = sizeof(entity);
    entity.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    entity.native_token = nativeToken;
    entity.valid_fields =
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELDS_ALL;
    WOTBMOD_V3_INIT_STRUCT(
        entity.snapshot,
        WOTBMOD_V3_ENTITY_PUBLIC_VERSION);
    entity.snapshot.public_id = publicId;
    entity.snapshot.type = type;
    entity.snapshot.visible_to_player = visible;
    entity.snapshot.local_player = local;
    entity.snapshot.team = team;
    entity.snapshot.health = 1000;
    entity.snapshot.max_health = 1000;
    entity.snapshot.position = position;
    Copy(
        entity.snapshot.public_type,
        sizeof(entity.snapshot.public_type),
        type == WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE
            ? "vehicle"
            : "effect");
    Copy(
        entity.snapshot.display_name,
        sizeof(entity.snapshot.display_name),
        displayName ? displayName : "");
    return entity;
}

void WOTBMOD_V3_CALL RenderCallback(
    WotbModV3Handle,
    const WotbModV3RenderFrameInfo* frame,
    void*) {
    if (!frame) {
        return;
    }
    ++g_renderCalls;
    g_lastRenderPhase = frame->phase;
    g_lastRenderFrame = frame->frame_index;
}

void RecordSerializedFrameObservationLocked(
    uint64_t frameIndex,
    uint32_t kind,
    float viewportWidth) {
    if (g_serializedFrameObservationCount >=
        kMaxSerializedFrameObservations) {
        return;
    }
    SerializedFrameObservation& observation =
        g_serializedFrameObservations[
            g_serializedFrameObservationCount++];
    observation.frameIndex = frameIndex;
    observation.kind = kind;
    observation.viewportWidth = viewportWidth;
}

void WOTBMOD_V3_CALL SerializedRenderCallback(
    WotbModV3Handle,
    const WotbModV3RenderFrameInfo* frame,
    void*) {
    if (!frame) {
        return;
    }
    std::unique_lock<std::mutex> lock(
        g_serializedFrameMutex);
    RecordSerializedFrameObservationLocked(
        frame->frame_index,
        kSerializedRenderObservation,
        frame->viewport.width);
    ++g_serializedRenderCallbacksActive;
    if (g_serializedRenderCallbacksActive > 1u) {
        g_serializedRenderOverlap = true;
    }
    if (frame->frame_index == 200u) {
        g_firstSerializedRenderEntered = true;
        g_serializedFrameCv.notify_all();
        g_serializedFrameCv.wait(
            lock,
            []() {
                return g_releaseFirstSerializedRender;
            });
    } else if (frame->frame_index == 201u) {
        g_secondSerializedRenderEntered = true;
        g_serializedFrameCv.notify_all();
    }
    --g_serializedRenderCallbacksActive;
}

void WOTBMOD_V3_CALL BlockingRenderCallback(
    WotbModV3Handle,
    const WotbModV3RenderFrameInfo*,
    void* userData) {
    BlockingCallbackGate* gate =
        static_cast<BlockingCallbackGate*>(userData);
    if (!gate) {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    ++gate->calls;
    gate->entered = true;
    gate->cv.notify_all();
    gate->cv.wait(
        lock,
        [&]() {
            return gate->release;
        });
}

void WOTBMOD_V3_CALL SelfUnregisterRenderCallback(
    WotbModV3Handle mod,
    const WotbModV3RenderFrameInfo*,
    void* userData) {
    SelfRenderUnsubscribeContext* context =
        static_cast<SelfRenderUnsubscribeContext*>(userData);
    if (!context || !context->api) {
        return;
    }
    ++context->calls;
    context->result = context->api->unregister_callback(
        mod,
        context->token);
}

void WOTBMOD_V3_CALL ConcurrentRenderSelfOperationCallback(
    WotbModV3Handle mod,
    const WotbModV3RenderFrameInfo*,
    void* userData) {
    ConcurrentRenderSelfOperationContext* context =
        static_cast<ConcurrentRenderSelfOperationContext*>(
            userData);
    if (!context || !context->api) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(context->mutex);
        ++context->calls;
        context->entered = true;
        context->cv.notify_all();
        context->cv.wait(
            lock,
            [&]() {
                return context->externalUnregisterStarted;
            });
    }
    do {
        context->result =
            context->api->set_callback_priority(
                mod,
                context->token,
                11);
        if (context->result == WOTBMOD_V3_OK) {
            std::this_thread::yield();
        }
    } while (context->result == WOTBMOD_V3_OK);
}

void WOTBMOD_V3_CALL RenderLifecycleCallback(
    WotbModV3Handle,
    WotbModV3Event* event,
    void*) {
    if (!event ||
        g_renderLifecycleCount >=
            kMaxRenderLifecycleEvents) {
        g_renderLifecyclePayloadValid = false;
        return;
    }
    const uint32_t index = g_renderLifecycleCount++;
    Copy(
        g_renderLifecycleTopics[index],
        sizeof(g_renderLifecycleTopics[index]),
        event->topic);
    g_renderLifecycleThreadRoles[index] =
        event->thread_role;
    if (!event->payload ||
        event->payload_size <
            sizeof(WotbModV3RenderLifecycleEvent)) {
        g_renderLifecyclePayloadValid = false;
        return;
    }
    const WotbModV3RenderLifecycleEvent* payload =
        static_cast<const WotbModV3RenderLifecycleEvent*>(
            event->payload);
    if (payload->struct_size <
            sizeof(WotbModV3RenderLifecycleEvent) ||
        payload->api_version != WOTBMOD_V3_RENDER_VERSION) {
        g_renderLifecyclePayloadValid = false;
        return;
    }
    g_renderLifecyclePayloads[index] = *payload;
    if (payload->frame_index == 200u ||
        payload->frame_index == 201u) {
        std::lock_guard<std::mutex> lock(
            g_serializedFrameMutex);
        RecordSerializedFrameObservationLocked(
            payload->frame_index,
            kSerializedLifecycleObservation,
            payload->viewport.width);
    }
}

/*
 * PROJECTILE DELIVERY, END TO END.
 *
 * Everything the loader does with a shot ends in
 * PublishClientHostProjectileLifecycle, and everything a mod does to hear it
 * ends in events->subscribe. Until this recorder existed nothing in the suite
 * linked both halves at once: the gameplay-bridge tests stub the projectile
 * backend, and the runtime-services tests do not link client_services. That
 * gap is why the four topics could be missing from SystemEventTopics() for a
 * whole release cycle while every test stayed green.
 */
constexpr uint32_t kMaxProjectileEvents = 16u;
uint32_t g_projectileEventCount = 0u;
bool g_projectileEventPayloadValid = true;
char g_projectileEventTopics[kMaxProjectileEvents]
                            [WOTBMOD_V3_MAX_EVENT_TOPIC] = {};
WotbModV3ProjectileLifecycleEvent
    g_projectileEventPayloads[kMaxProjectileEvents] = {};

void WOTBMOD_V3_CALL ProjectileLifecycleCallback(
    WotbModV3Handle,
    WotbModV3Event* event,
    void*) {
    if (!event || g_projectileEventCount >= kMaxProjectileEvents) {
        g_projectileEventPayloadValid = false;
        return;
    }
    const uint32_t index = g_projectileEventCount++;
    Copy(
        g_projectileEventTopics[index],
        sizeof(g_projectileEventTopics[index]),
        event->topic);
    if (!event->payload ||
        event->payload_size <
            sizeof(WotbModV3ProjectileLifecycleEvent)) {
        g_projectileEventPayloadValid = false;
        return;
    }
    const WotbModV3ProjectileLifecycleEvent* payload =
        static_cast<const WotbModV3ProjectileLifecycleEvent*>(
            event->payload);
    if (payload->struct_size <
            sizeof(WotbModV3ProjectileLifecycleEvent) ||
        payload->api_version != WOTBMOD_V3_PROJECTILE_VERSION_2) {
        g_projectileEventPayloadValid = false;
        return;
    }
    g_projectileEventPayloads[index] = *payload;
}

void WOTBMOD_V3_CALL RpcCallback(
    WotbModV3Handle,
    const WotbModV3ObservedRpc*,
    void*) {
}

void WOTBMOD_V3_CALL BlockingRpcCallback(
    WotbModV3Handle,
    const WotbModV3ObservedRpc*,
    void* userData) {
    BlockingCallbackGate* gate =
        static_cast<BlockingCallbackGate*>(userData);
    if (!gate) {
        return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    ++gate->calls;
    gate->entered = true;
    gate->cv.notify_all();
    gate->cv.wait(
        lock,
        [&]() {
            return gate->release;
        });
}

void WOTBMOD_V3_CALL SelfUnregisterRpcCallback(
    WotbModV3Handle mod,
    const WotbModV3ObservedRpc*,
    void* userData) {
    SelfRpcUnsubscribeContext* context =
        static_cast<SelfRpcUnsubscribeContext*>(userData);
    if (!context || !context->api) {
        return;
    }
    ++context->calls;
    context->result = context->api->unsubscribe_observed(
        mod,
        context->token);
}

void WOTBMOD_V3_CALL ConcurrentSelfUnregisterRpcCallback(
    WotbModV3Handle mod,
    const WotbModV3ObservedRpc*,
    void* userData) {
    ConcurrentSelfRpcUnsubscribeContext* context =
        static_cast<ConcurrentSelfRpcUnsubscribeContext*>(
            userData);
    if (!context || !context->api) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(context->mutex);
        ++context->calls;
        context->entered = true;
        context->cv.notify_all();
        context->cv.wait(
            lock,
            [&]() {
                return context->release;
            });
    }
    context->result = context->api->unsubscribe_observed(
        mod,
        context->token);
}

void WOTBMOD_V3_CALL EntityPropertyCallback(
    WotbModV3Handle,
    WotbModV3EntityHandle,
    const char*,
    const WotbModV3PublicValue* value,
    void*) {
    if (!value) {
        return;
    }
    ++g_entityPropertyCalls;
    g_lastEntityPropertyInteger = value->value.integer;
}

WotbModV3Result WOTBMOD_V3_CALL PublicEntityVisitor(
    WotbModV3Handle,
    const WotbModV3PublicEntitySnapshot* snapshot,
    void*) {
    if (!snapshot) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (snapshot->public_id == 15u) {
        ++g_partialEnumerationSeen;
        g_partialEnumerationSanitized =
            snapshot->team == 0u &&
            Near(snapshot->position.x, 0.0f) &&
            Near(snapshot->position.y, 0.0f) &&
            Near(snapshot->position.z, 0.0f) &&
            Near(snapshot->direction.x, 0.0f) &&
            Near(snapshot->direction.y, 0.0f) &&
            Near(snapshot->direction.z, 0.0f) &&
            snapshot->display_name[0] == '\0';
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL MissingBindingVisitor(
    WotbModV3Handle,
    const WotbModV3MissingBinding* binding,
    void*) {
    if (!binding ||
        binding->struct_size < sizeof(*binding) ||
        binding->api_version != WOTBMOD_V3_CLIENT_VERSION) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ++g_missingBindingCount;
    if (std::strcmp(
            binding->symbol,
            "native_gameplay_camera_advanced") == 0) {
        g_sawAdvancedCameraGap = true;
    } else if (std::strcmp(
                   binding->symbol,
                   "native_tracer_style") == 0) {
        g_sawTracerStyleGap = true;
    } else if (std::strcmp(
                   binding->symbol,
                   "native_input_event_ingress") == 0) {
        g_sawInputIngressGap = true;
    } else if (std::strcmp(
                   binding->symbol,
                   "native_public_entity_optional_fields") == 0) {
        g_sawEntityOptionalFieldsGap = true;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result FakeGesListTypes(
    void*,
    wotbmod::v3::ClientHostGesTypeVisitFn visit,
    void* data) {
    visit(data, "Avatar::CameraModeChanged");
    visit(data, "Lobby::Survey::Accepted");
    return WOTBMOD_V3_OK;
}

WotbModV3Result FakeGesObserve(void*, const char*, uint32_t) {
    return WOTBMOD_V3_OK;
}

WotbModV3Result FakeGesPublish(
    void*, const char*, const void*, uint32_t, uint32_t) {
    return WOTBMOD_V3_OK;
}

template <typename T>
const T* Query(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    uint32_t version) {
    const void* table = nullptr;
    const WotbModV3Result result = bootstrap->query_interface(
        mod,
        name,
        version,
        &table);
    CHECK(result == WOTBMOD_V3_OK);
    CHECK(table != nullptr);
    return static_cast<const T*>(table);
}

}  // namespace

int main() {
    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory =
        "build\\v3_client_truthful_tests\\mods";
    options.cache_directory =
        "build\\v3_client_truthful_tests\\cache";
    options.config_directory =
        "build\\v3_client_truthful_tests\\config";
    options.client_version = "client-truthful-test";
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::RegisterLifecycleStateHook(
            &TestLifecycleHook) == WOTBMOD_V3_OK);

    ClientHostBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    backend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    backend.invoke = &InvokeClientHost;
    wotbmod::v3::SetClientHostBackend(&backend);

    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "client_truthful_slices_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            mod, &TestEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK);

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);
    const WotbModV3HandlesApiV1* handles =
        Query<WotbModV3HandlesApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_HANDLES,
            WOTBMOD_V3_HANDLES_VERSION);
    const WotbModV3VehicleVisualApiV1* vehicle =
        Query<WotbModV3VehicleVisualApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
            WOTBMOD_V3_VEHICLE_VISUAL_VERSION);
    const WotbModV3GameplayCameraApiV1* camera =
        Query<WotbModV3GameplayCameraApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
            WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION);
    const WotbModV3RenderApiV1* render =
        Query<WotbModV3RenderApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_RENDER,
            WOTBMOD_V3_RENDER_VERSION);
    const WotbModV3EventsApiV1* events =
        Query<WotbModV3EventsApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_EVENTS,
            WOTBMOD_V3_EVENTS_VERSION);
    const WotbModV3ClientApiV1* client =
        Query<WotbModV3ClientApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_CLIENT,
            WOTBMOD_V3_CLIENT_VERSION);
    const WotbModV3BigWorldRpcApiV1* rpc =
        Query<WotbModV3BigWorldRpcApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_BIGWORLD_RPC,
            WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    const WotbModV3DeviceApiV1* device =
        Query<WotbModV3DeviceApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_DEVICE,
            WOTBMOD_V3_DEVICE_VERSION);
    const WotbModV3AudioApiV2* audio =
        Query<WotbModV3AudioApiV2>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_AUDIO,
            WOTBMOD_V3_AUDIO_VERSION);
    const WotbModV3EntityPublicApiV1* entityPublic =
        Query<WotbModV3EntityPublicApiV1>(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
            WOTBMOD_V3_ENTITY_PUBLIC_VERSION);

    uint32_t supported = 0u;
    CHECK(client->is_supported(mod, &supported) == WOTBMOD_V3_OK);
    CHECK(supported == 1u);
    uint32_t compatibility =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN;
    CHECK(
        client->get_compatibility_state(
            mod, &compatibility) == WOTBMOD_V3_OK);
    CHECK(
        compatibility ==
        WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED);
    CHECK(
        client->enumerate_missing_bindings(
            mod, &MissingBindingVisitor, nullptr) ==
        WOTBMOD_V3_OK);
    CHECK(g_missingBindingCount >= 10u);
    CHECK(g_sawAdvancedCameraGap);
    CHECK(g_sawTracerStyleGap);
    CHECK(g_sawInputIngressGap);
    CHECK(g_sawEntityOptionalFieldsGap);

    WotbModV3Token outgoingRpcToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_OUTGOING,
            nullptr,
            &RpcCallback,
            nullptr,
            &outgoingRpcToken) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(outgoingRpcToken == WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3Token mixedRpcToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING |
                WOTBMOD_V3_RPC_OUTGOING,
            nullptr,
            &RpcCallback,
            nullptr,
            &mixedRpcToken) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(mixedRpcToken == WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3DeviceInfo invalidDeviceInfo = {};
    CHECK(
        device->get_info(mod, &invalidDeviceInfo) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    WotbModV3DeviceInfo deviceInfo = {};
    WOTBMOD_V3_INIT_STRUCT(
        deviceInfo,
        WOTBMOD_V3_DEVICE_VERSION);
    CHECK(device->get_info(mod, &deviceInfo) == WOTBMOD_V3_OK);
    CHECK(deviceInfo.struct_size == sizeof(deviceInfo));
    CHECK(
        deviceInfo.api_version ==
        WOTBMOD_V3_DEVICE_VERSION);
    CHECK(deviceInfo.logical_processor_count > 0u);
    CHECK(deviceInfo.operating_system[0] != '\0');
    CHECK(deviceInfo.processor[0] != '\0');
    uint32_t adapterCount = UINT32_MAX;
    CHECK(
        device->get_graphics_adapter_count(
            mod, &adapterCount) == WOTBMOD_V3_OK);
    CHECK(adapterCount == deviceInfo.graphics_adapter_count);
    if (adapterCount > 0u) {
        WotbModV3GraphicsAdapterInfo adapter = {};
        WOTBMOD_V3_INIT_STRUCT(
            adapter,
            WOTBMOD_V3_DEVICE_VERSION);
        CHECK(
            device->get_graphics_adapter_at(
                mod, 0u, &adapter) == WOTBMOD_V3_OK);
        CHECK(adapter.index == 0u);
        CHECK(adapter.name[0] != '\0');
    }
    WotbModV3GraphicsAdapterInfo missingAdapter = {};
    WOTBMOD_V3_INIT_STRUCT(
        missingAdapter,
        WOTBMOD_V3_DEVICE_VERSION);
    CHECK(
        device->get_graphics_adapter_at(
            mod,
            adapterCount,
            &missingAdapter) == WOTBMOD_V3_E_NOT_FOUND);

    CHECK(
        WotbModV3Runtime_SetContext(
            WOTBMOD_V3_CONTEXT_HANGAR) == WOTBMOD_V3_OK);
    CHECK(
        client->leave_to_hangar(mod) ==
        WOTBMOD_V3_E_INCOMPATIBLE);
    CHECK(g_leaveToHangarCalls == 0u);
    CHECK(
        WotbModV3Runtime_SetContext(
            WOTBMOD_V3_CONTEXT_BATTLE) == WOTBMOD_V3_OK);
    CHECK(client->leave_to_hangar(mod) == WOTBMOD_V3_OK);
    CHECK(g_leaveToHangarCalls == 1u);

    BackendProbe pumpReplacementProbe = {};
    pumpReplacementProbe.tokenPrefix =
        UINT64_C(0x7700000000000000);
    ClientHostBackend pumpReplacementBackend = backend;
    pumpReplacementBackend.user_data = &pumpReplacementProbe;
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockClientLeave = true;
        g_backendGate.clientLeaveEntered = false;
        g_backendGate.releaseClientLeave = false;
        g_backendGate.pumpFrameBeforeClientLeaveReturns = true;
    }
    WotbModV3Result concurrentLeaveResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread concurrentLeaveThread([&]() {
        const uint32_t previousRole =
            wotbmod::v3::SetCurrentThreadRole(
                WOTBMOD_V3_THREAD_MAIN);
        concurrentLeaveResult =
            client->leave_to_hangar(mod);
        wotbmod::v3::SetCurrentThreadRole(previousRole);
    });
    bool concurrentLeaveEntered = false;
    {
        std::unique_lock<std::mutex> lock(g_backendGate.mutex);
        concurrentLeaveEntered =
            g_backendGate.cv.wait_for(
                lock,
                std::chrono::seconds(2),
                []() {
                    return g_backendGate.clientLeaveEntered;
                });
    }
    CHECK(concurrentLeaveEntered);
    std::atomic<bool> backendReplacementReturned{false};
    std::thread backendReplacementThread([&]() {
        wotbmod::v3::SetClientHostBackend(
            &pumpReplacementBackend);
        backendReplacementReturned.store(
            true,
            std::memory_order_release);
    });
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    CHECK(
        !backendReplacementReturned.load(
            std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.releaseClientLeave = true;
    }
    g_backendGate.cv.notify_all();
    concurrentLeaveThread.join();
    backendReplacementThread.join();
    CHECK(concurrentLeaveResult == WOTBMOD_V3_OK);
    CHECK(
        backendReplacementReturned.load(
            std::memory_order_acquire));
    CHECK(g_leaveToHangarCalls == 2u);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockClientLeave = false;
        g_backendGate.clientLeaveEntered = false;
        g_backendGate.releaseClientLeave = false;
        g_backendGate.pumpFrameBeforeClientLeaveReturns = false;
    }
    wotbmod::v3::SetClientHostBackend(&backend);

    WotbModV3BigWorldRpcPolicy unavailablePolicy = {};
    WOTBMOD_V3_INIT_STRUCT(
        unavailablePolicy,
        WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    wotbmod::v3::SetClientHostBackend(nullptr);
    CHECK(
        rpc->get_policy(mod, &unavailablePolicy) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailablePolicy.metadata_observation == 0u);
    wotbmod::v3::SetClientHostBackend(&backend);

    WotbModV3EntityHandle hiddenHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity hidden = PublicEntity(
        10u,
        10u,
        WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE,
        0u,
        0u,
        2u,
        "hidden",
        {99.0f, 99.0f, 99.0f});
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &hidden, &hiddenHandle) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(hiddenHandle == WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3EntityHandle localHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity local = PublicEntity(
        11u,
        11u,
        WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE,
        1u,
        1u,
        1u,
        "local",
        {1.0f, 2.0f, 3.0f});
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &local, &localHandle) == WOTBMOD_V3_OK);

    WotbModV3EntityHandle allyHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity ally = PublicEntity(
        12u,
        12u,
        WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE,
        1u,
        0u,
        1u,
        "ally",
        {4.0f, 5.0f, 6.0f});
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &ally, &allyHandle) == WOTBMOD_V3_OK);

    WotbModV3EntityHandle enemyHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity enemy = PublicEntity(
        13u,
        13u,
        WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE,
        1u,
        0u,
        2u,
        "visible-enemy",
        {7.0f, 8.0f, 9.0f});
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &enemy, &enemyHandle) == WOTBMOD_V3_OK);

    WotbModV3EntityHandle effectHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity effect = PublicEntity(
        14u,
        14u,
        WOTBMOD_V3_PUBLIC_ENTITY_EFFECT,
        1u,
        0u,
        0u,
        "effect",
        {10.0f, 11.0f, 12.0f});
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &effect, &effectHandle) == WOTBMOD_V3_OK);

    WotbModV3EntityHandle partialHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    ClientHostPublicEntity partial = PublicEntity(
        15u,
        15u,
        WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE,
        1u,
        0u,
        9u,
        "must-not-leak",
        {40.0f, 41.0f, 42.0f});
    partial.valid_fields =
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_ID |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_TYPE |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_VISIBILITY |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_LOCAL |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_HEALTH |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_MAX_HEALTH |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_PUBLIC_TYPE;
    partial.snapshot.direction = {43.0f, 44.0f, 45.0f};
    CHECK(
        wotbmod::v3::RegisterClientHostPublicEntity(
            &partial, &partialHandle) == WOTBMOD_V3_OK);

    WotbModV3PublicValue publicValue = {};
    CHECK(
        entityPublic->get_public_property(
            mod,
            partialHandle,
            "team",
            &publicValue) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        entityPublic->get_public_property(
            mod,
            partialHandle,
            "position",
            &publicValue) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        entityPublic->get_public_property(
            mod,
            partialHandle,
            "display_name",
            &publicValue) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        entityPublic->get_public_property(
            mod,
            partialHandle,
            "unknown_field",
            &publicValue) == WOTBMOD_V3_E_NOT_FOUND);
    CHECK(
        entityPublic->get_public_property(
            mod,
            partialHandle,
            "health",
            &publicValue) == WOTBMOD_V3_OK);
    CHECK(
        publicValue.type == WOTBMOD_V3_PUBLIC_VALUE_INT64 &&
        publicValue.value.integer == 1000);

    WotbModV3Token unavailablePropertyToken =
        UINT64_C(0xFFFFFFFFFFFFFFFE);
    CHECK(
        entityPublic->subscribe_public_property(
            mod,
            partialHandle,
            "team",
            &EntityPropertyCallback,
            nullptr,
            &unavailablePropertyToken) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        unavailablePropertyToken ==
        WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3Token unknownPropertyToken =
        UINT64_C(0xFFFFFFFFFFFFFFFE);
    CHECK(
        entityPublic->subscribe_public_property(
            mod,
            partialHandle,
            "unknown_field",
            &EntityPropertyCallback,
            nullptr,
            &unknownPropertyToken) ==
        WOTBMOD_V3_E_NOT_FOUND);
    CHECK(unknownPropertyToken == WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3Token healthToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        entityPublic->subscribe_public_property(
            mod,
            partialHandle,
            "health",
            &EntityPropertyCallback,
            nullptr,
            &healthToken) == WOTBMOD_V3_OK);
    CHECK(healthToken != WOTBMOD_V3_INVALID_HANDLE);
    partial.snapshot.health = 900;
    partial.snapshot.team = 7u;
    partial.snapshot.position = {70.0f, 71.0f, 72.0f};
    CHECK(
        wotbmod::v3::UpdateClientHostPublicEntity(
            partialHandle,
            &partial) == WOTBMOD_V3_OK);
    CHECK(g_entityPropertyCalls == 1u);
    CHECK(g_lastEntityPropertyInteger == 900);
    CHECK(
        entityPublic->unsubscribe_public_property(
            mod,
            healthToken) == WOTBMOD_V3_OK);

    WotbModV3PublicEntitySnapshot partialSnapshot = {};
    CHECK(
        entityPublic->get_snapshot(
            mod,
            partialHandle,
            &partialSnapshot) == WOTBMOD_V3_OK);
    CHECK(partialSnapshot.health == 900);
    CHECK(partialSnapshot.team == 0u);
    CHECK(
        Near(partialSnapshot.position.x, 0.0f) &&
        Near(partialSnapshot.position.y, 0.0f) &&
        Near(partialSnapshot.position.z, 0.0f));
    CHECK(
        Near(partialSnapshot.direction.x, 0.0f) &&
        Near(partialSnapshot.direction.y, 0.0f) &&
        Near(partialSnapshot.direction.z, 0.0f));
    CHECK(partialSnapshot.display_name[0] == '\0');
    CHECK(
        entityPublic->enumerate_visible(
            mod,
            &PublicEntityVisitor,
            nullptr) == WOTBMOD_V3_OK);
    CHECK(g_partialEnumerationSeen == 1u);
    CHECK(g_partialEnumerationSanitized);

    /* roster extras (clan_tag, account_id, kills, vehicle_name) live beside
     * the snapshot: refused without their bits, served by name with them,
     * and sanitized back to nothing when a later update drops the bits */
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "clan_tag", &publicValue) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "account_id", &publicValue) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "kills", &publicValue) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "vehicle_name", &publicValue) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    partial.valid_fields |=
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_CLAN_TAG |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_ACCOUNT_ID |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_KILLS |
        wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_VEHICLE_NAME;
    strcpy_s(partial.extras.clan_tag, "PIGGG");
    partial.extras.account_id = INT64_C(598289265);
    partial.extras.kills = 3;
    strcpy_s(partial.extras.vehicle_name, "france:F127_ELC_AMX_901_Proto");
    CHECK(
        wotbmod::v3::UpdateClientHostPublicEntity(
            partialHandle,
            &partial) == WOTBMOD_V3_OK);
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "clan_tag", &publicValue) == WOTBMOD_V3_OK &&
        publicValue.type == WOTBMOD_V3_PUBLIC_VALUE_STRING &&
        std::strcmp(publicValue.value.string_value, "PIGGG") == 0);
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "account_id", &publicValue) == WOTBMOD_V3_OK &&
        publicValue.type == WOTBMOD_V3_PUBLIC_VALUE_INT64 &&
        publicValue.value.integer == INT64_C(598289265));
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "frags", &publicValue) == WOTBMOD_V3_OK &&
        publicValue.value.integer == 3);
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "vehicle_name", &publicValue) == WOTBMOD_V3_OK &&
        std::strcmp(
            publicValue.value.string_value,
            "france:F127_ELC_AMX_901_Proto") == 0);
    partial.valid_fields &=
        ~static_cast<uint64_t>(
            wotbmod::v3::CLIENT_HOST_PUBLIC_ENTITY_FIELD_KILLS);
    CHECK(
        wotbmod::v3::UpdateClientHostPublicEntity(
            partialHandle,
            &partial) == WOTBMOD_V3_OK);
    CHECK(
        entityPublic->get_public_property(
            mod, partialHandle, "kills", &publicValue) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);

    CHECK(
        entityPublic->get_public_property(
            mod,
            allyHandle,
            "team",
            &publicValue) == WOTBMOD_V3_OK);
    CHECK(
        publicValue.type == WOTBMOD_V3_PUBLIC_VALUE_INT64 &&
        publicValue.value.integer == 1);

    WotbModV3EntityHandle returnedLocal =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        vehicle->get_local_player_vehicle(
            mod, &returnedLocal) == WOTBMOD_V3_OK);
    CHECK(returnedLocal == localHandle);

    uint32_t flag = UINT32_MAX;
    CHECK(
        vehicle->is_local_player(
            mod, localHandle, &flag) == WOTBMOD_V3_OK);
    CHECK(flag == 1u);
    CHECK(
        vehicle->is_local_player(
            mod, enemyHandle, &flag) == WOTBMOD_V3_OK);
    CHECK(flag == 0u);
    CHECK(
        vehicle->get_enemy(
            mod, allyHandle, &flag) == WOTBMOD_V3_OK);
    CHECK(flag == 0u);
    CHECK(
        vehicle->get_enemy(
            mod, enemyHandle, &flag) == WOTBMOD_V3_OK);
    CHECK(flag == 1u);
    CHECK(
        vehicle->get_enemy(
            mod, localHandle, &flag) == WOTBMOD_V3_OK);
    CHECK(flag == 0u);

    WotbModV3Vec3 position = {};
    CHECK(
        vehicle->get_position(
            mod, enemyHandle, &position) == WOTBMOD_V3_OK);
    CHECK(
        Near(position.x, 7.0f) &&
        Near(position.y, 8.0f) &&
        Near(position.z, 9.0f));
    CHECK(
        vehicle->get_position(
            mod, effectHandle, &position) ==
        WOTBMOD_V3_E_NOT_FOUND);

    uint32_t nameSize = 0u;
    CHECK(
        vehicle->get_enemy_name(
            mod, enemyHandle, nullptr, &nameSize) ==
        WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(nameSize == std::strlen("visible-enemy") + 1u);
    char name[WOTBMOD_V3_MAX_NAME] = {};
    uint32_t nameCapacity = sizeof(name);
    CHECK(
        vehicle->get_enemy_name(
            mod, enemyHandle, name, &nameCapacity) ==
        WOTBMOD_V3_OK);
    CHECK(std::strcmp(name, "visible-enemy") == 0);
    uint32_t appearance = UINT32_MAX;
    CHECK(
        vehicle->get_appearance_state(
            mod, enemyHandle, &appearance) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);

    CHECK(
        wotbmod::v3::RemoveClientHostPublicEntity(
            enemyHandle) == WOTBMOD_V3_OK);
    CHECK(
        vehicle->get_position(
            mod, enemyHandle, &position) ==
        WOTBMOD_V3_E_NOT_FOUND);
    CHECK(
        vehicle->get_enemy(
            mod, enemyHandle, &flag) ==
        WOTBMOD_V3_E_NOT_FOUND);

    CHECK(
        camera->set_fov(mod, 95.0f) == WOTBMOD_V3_OK);
    CHECK(g_cameraSetFovCalls == 1u);
    CHECK(Near(g_cameraFov, 95.0f));
    float fov = 0.0f;
    CHECK(camera->get_fov(mod, &fov) == WOTBMOD_V3_OK);
    CHECK(Near(fov, 95.0f));
    CHECK(g_cameraGetActiveCalls == 2u);
    CHECK(g_cameraGetFovCalls == 1u);
    CHECK(
        camera->set_fov(mod, 20.0f) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(g_cameraSetFovCalls == 1u);
    CHECK(
        camera->reset_fov(mod) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(g_cameraResetFovCalls == 1u);
    CHECK(Near(g_cameraFov, 95.0f));
    g_supportCameraReset = true;
    CHECK(camera->reset_fov(mod) == WOTBMOD_V3_OK);
    CHECK(g_cameraResetFovCalls == 2u);
    CHECK(Near(g_cameraFov, kDefaultFov));
    g_cameraFov = 200.0f;
    CHECK(camera->get_fov(mod, &fov) == WOTBMOD_V3_E_PLATFORM);
    g_cameraFov = kDefaultFov;

    uint32_t renderBackend = UINT32_MAX;
    CHECK(
        render->get_backend(mod, &renderBackend) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(renderBackend == WOTBMOD_V3_RENDER_BACKEND_NONE);

    WotbModV3EventSubscriptionInfo renderEvents = {};
    renderEvents.struct_size = sizeof(renderEvents);
    renderEvents.api_version = WOTBMOD_V3_EVENTS_VERSION;
    renderEvents.topic_pattern = "wotbmod.render.*";
    renderEvents.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    renderEvents.receive_system_events = 1u;
    WotbModV3EventToken renderEventsToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        events->subscribe(
            mod,
            &renderEvents,
            &RenderLifecycleCallback,
            nullptr,
            &renderEventsToken) == WOTBMOD_V3_OK);
    CHECK(renderEventsToken != WOTBMOD_V3_INVALID_HANDLE);

    ClientHostFrame unavailableFrame = {};
    unavailableFrame.struct_size = sizeof(unavailableFrame);
    unavailableFrame.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    unavailableFrame.render_backend =
        WOTBMOD_V3_RENDER_BACKEND_NONE;
    unavailableFrame.frame_index = 76u;
    unavailableFrame.delta_seconds = 1.0 / 60.0;
    unavailableFrame.viewport =
        {0.0f, 0.0f, 1920.0f, 1080.0f};

    ClientHostFrame unsupportedFrame = unavailableFrame;
    unsupportedFrame.render_backend =
        WOTBMOD_V3_RENDER_BACKEND_D3D12;
    unsupportedFrame.native_device =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x1000u));
    unsupportedFrame.native_context =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x2000u));
    unsupportedFrame.native_swapchain =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x3000u));
    wotbmod::v3::PumpClientHostFrame(&unsupportedFrame);
    CHECK(g_renderLifecycleCount == 0u);

    ClientHostFrame incompleteFrame = unavailableFrame;
    incompleteFrame.render_backend =
        WOTBMOD_V3_RENDER_BACKEND_D3D11;
    wotbmod::v3::PumpClientHostFrame(&incompleteFrame);
    CHECK(g_renderLifecycleCount == 0u);

    wotbmod::v3::PumpClientHostFrame(&unavailableFrame);
    CHECK(g_renderLifecycleCount == 0u);
    renderBackend = UINT32_MAX;
    CHECK(
        render->get_backend(mod, &renderBackend) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(renderBackend == WOTBMOD_V3_RENDER_BACKEND_NONE);

    WotbModV3Token beforeToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_BEFORE_UI,
            0,
            &RenderCallback,
            nullptr,
            &beforeToken) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(beforeToken == WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3Token afterToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_AFTER_UI,
            0,
            &RenderCallback,
            nullptr,
            &afterToken) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(afterToken == WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3Token invalidToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT + 1u,
            0,
            &RenderCallback,
            nullptr,
            &invalidToken) == WOTBMOD_V3_E_INVALID_ARGUMENT);

    WotbModV3Token presentToken = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT,
            10,
            &RenderCallback,
            nullptr,
            &presentToken) == WOTBMOD_V3_OK);
    CHECK(presentToken != WOTBMOD_V3_INVALID_HANDLE);
    ClientHostFrame frame = {};
    frame.struct_size = sizeof(frame);
    frame.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    frame.render_backend = WOTBMOD_V3_RENDER_BACKEND_D3D11;
    frame.frame_index = 77u;
    frame.delta_seconds = 1.0 / 60.0;
    frame.viewport = {0.0f, 0.0f, 1920.0f, 1080.0f};
    frame.native_device =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x1000u));
    frame.native_context =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x2000u));
    frame.native_swapchain =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x3000u));
    wotbmod::v3::PumpClientHostFrame(&frame);
    CHECK(
        render->get_backend(mod, &renderBackend) ==
        WOTBMOD_V3_OK);
    CHECK(renderBackend == WOTBMOD_V3_RENDER_BACKEND_D3D11);
    CHECK(g_renderCalls == 1u);
    CHECK(g_lastRenderPhase == WOTBMOD_V3_RENDER_PHASE_PRESENT);
    CHECK(g_lastRenderFrame == 77u);
    CHECK(
        render->unregister_callback(
            mod, presentToken) == WOTBMOD_V3_OK);
    frame.frame_index = 78u;
    wotbmod::v3::PumpClientHostFrame(&frame);
    CHECK(g_renderCalls == 1u);

    frame.frame_index = 79u;
    frame.viewport =
        {0.0f, 0.0f, 1600.0f, 900.0f};
    wotbmod::v3::PumpClientHostFrame(&frame);

    frame.frame_index = 80u;
    frame.native_device =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x4000u));
    frame.native_context =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x5000u));
    frame.native_swapchain =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x6000u));
    wotbmod::v3::PumpClientHostFrame(&frame);

    ClientHostFrame lostFrame = frame;
    lostFrame.frame_index = 81u;
    lostFrame.render_backend =
        WOTBMOD_V3_RENDER_BACKEND_NONE;
    lostFrame.native_device = nullptr;
    lostFrame.native_context = nullptr;
    lostFrame.native_swapchain = nullptr;
    wotbmod::v3::PumpClientHostFrame(&lostFrame);
    renderBackend = UINT32_MAX;
    CHECK(
        render->get_backend(mod, &renderBackend) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(renderBackend == WOTBMOD_V3_RENDER_BACKEND_NONE);
    lostFrame.frame_index = 82u;
    wotbmod::v3::PumpClientHostFrame(&lostFrame);

    frame.frame_index = 83u;
    wotbmod::v3::PumpClientHostFrame(&frame);
    CHECK(
        render->get_backend(mod, &renderBackend) ==
        WOTBMOD_V3_OK);
    CHECK(renderBackend == WOTBMOD_V3_RENDER_BACKEND_D3D11);

    frame.frame_index = 84u;
    frame.native_swapchain =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x7000u));
    wotbmod::v3::PumpClientHostFrame(&frame);

    frame.frame_index = 85u;
    frame.native_context =
        reinterpret_cast<void*>(
            static_cast<std::uintptr_t>(0x8000u));
    wotbmod::v3::PumpClientHostFrame(&frame);

    CHECK(g_renderLifecyclePayloadValid);
    CHECK(g_renderLifecycleCount == 13u);
    for (uint32_t index = 0u;
         index < g_renderLifecycleCount;
         ++index) {
        CHECK(
            g_renderLifecycleThreadRoles[index] ==
            WOTBMOD_V3_THREAD_RENDER);
    }
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[0],
            WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED) == 0);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[1],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED) == 0);
    CHECK(
        g_renderLifecyclePayloads[0].previous_backend ==
        WOTBMOD_V3_RENDER_BACKEND_NONE);
    CHECK(
        g_renderLifecyclePayloads[0].backend ==
        WOTBMOD_V3_RENDER_BACKEND_D3D11);
    CHECK(
        g_renderLifecyclePayloads[0].frame_index == 77u);
    CHECK(
        g_renderLifecyclePayloads[0].previous_device_available ==
        0u);
    CHECK(
        g_renderLifecyclePayloads[0].device_available == 1u);

    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[2],
            WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED) == 0);
    CHECK(
        g_renderLifecyclePayloads[2].frame_index == 79u);
    CHECK(
        Near(
            g_renderLifecyclePayloads[2]
                .previous_viewport.width,
            1920.0f));
    CHECK(
        Near(
            g_renderLifecyclePayloads[2].viewport.width,
            1600.0f));
    CHECK(
        g_renderLifecyclePayloads[2].component_changes ==
        WOTBMOD_V3_RENDER_LIFECYCLE_VIEWPORT);

    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[3],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST) == 0);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[4],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED) == 0);
    CHECK(
        g_renderLifecyclePayloads[3].frame_index == 80u);
    CHECK(
        (g_renderLifecyclePayloads[3].component_changes &
         WOTBMOD_V3_RENDER_LIFECYCLE_DEVICE) != 0u);
    CHECK(
        g_renderLifecyclePayloads[3].previous_device_available ==
        1u);
    CHECK(
        g_renderLifecyclePayloads[3].device_available == 1u);

    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[5],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST) == 0);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[6],
            WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED) == 0);
    CHECK(
        g_renderLifecyclePayloads[5].frame_index == 81u);
    CHECK(
        g_renderLifecyclePayloads[5].previous_device_available ==
        1u);
    CHECK(
        g_renderLifecyclePayloads[5].device_available == 0u);

    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[7],
            WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED) == 0);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[8],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED) == 0);
    CHECK(
        g_renderLifecyclePayloads[8].frame_index == 83u);
    CHECK(
        g_renderLifecyclePayloads[8].previous_device_available ==
        0u);
    CHECK(
        g_renderLifecyclePayloads[8].device_available == 1u);

    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[9],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST) == 0);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[10],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED) == 0);
    CHECK(
        g_renderLifecyclePayloads[9].frame_index == 84u);
    CHECK(
        g_renderLifecyclePayloads[9].component_changes ==
        WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN);
    CHECK(
        Near(
            g_renderLifecyclePayloads[9].previous_viewport.width,
            g_renderLifecyclePayloads[9].viewport.width));

    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[11],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST) == 0);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[12],
            WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED) == 0);
    CHECK(
        g_renderLifecyclePayloads[11].frame_index == 85u);
    CHECK(
        g_renderLifecyclePayloads[11].component_changes ==
        WOTBMOD_V3_RENDER_LIFECYCLE_CONTEXT);

    WotbModV3Token serializedRenderToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT,
            10,
            &SerializedRenderCallback,
            nullptr,
            &serializedRenderToken) == WOTBMOD_V3_OK);
    CHECK(
        serializedRenderToken !=
        WOTBMOD_V3_INVALID_HANDLE);

    ClientHostFrame firstConcurrentFrame = frame;
    firstConcurrentFrame.frame_index = 200u;
    firstConcurrentFrame.viewport =
        {0.0f, 0.0f, 1111.0f, 777.0f};
    ClientHostFrame secondConcurrentFrame = frame;
    secondConcurrentFrame.frame_index = 201u;
    secondConcurrentFrame.viewport =
        {0.0f, 0.0f, 2222.0f, 888.0f};

    std::thread firstFrameThread([&]() {
        wotbmod::v3::PumpClientHostFrame(
            &firstConcurrentFrame);
    });
    bool firstRenderEntered = false;
    {
        std::unique_lock<std::mutex> lock(
            g_serializedFrameMutex);
        firstRenderEntered = g_serializedFrameCv.wait_for(
            lock,
            std::chrono::seconds(2),
            []() {
                return g_firstSerializedRenderEntered;
            });
    }
    CHECK(firstRenderEntered);

    std::atomic<bool> secondFrameStarted{false};
    std::atomic<bool> secondFrameReturned{false};
    std::thread secondFrameThread([&]() {
        secondFrameStarted.store(
            true,
            std::memory_order_release);
        wotbmod::v3::PumpClientHostFrame(
            &secondConcurrentFrame);
        secondFrameReturned.store(
            true,
            std::memory_order_release);
    });
    while (!secondFrameStarted.load(
        std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    bool secondRenderEnteredBeforeRelease = false;
    {
        std::unique_lock<std::mutex> lock(
            g_serializedFrameMutex);
        secondRenderEnteredBeforeRelease =
            g_serializedFrameCv.wait_for(
                lock,
                std::chrono::milliseconds(100),
                []() {
                    return g_secondSerializedRenderEntered;
                });
        CHECK(!secondRenderEnteredBeforeRelease);
        CHECK(
            !secondFrameReturned.load(
                std::memory_order_acquire));
        g_releaseFirstSerializedRender = true;
    }
    g_serializedFrameCv.notify_all();
    firstFrameThread.join();
    secondFrameThread.join();

    CHECK(!g_serializedRenderOverlap);
    CHECK(g_serializedRenderCallbacksActive == 0u);
    CHECK(g_serializedFrameObservationCount == 4u);
    CHECK(
        g_serializedFrameObservations[0].frameIndex == 200u &&
        g_serializedFrameObservations[0].kind ==
            kSerializedLifecycleObservation &&
        Near(
            g_serializedFrameObservations[0].viewportWidth,
            1111.0f));
    CHECK(
        g_serializedFrameObservations[1].frameIndex == 200u &&
        g_serializedFrameObservations[1].kind ==
            kSerializedRenderObservation &&
        Near(
            g_serializedFrameObservations[1].viewportWidth,
            1111.0f));
    CHECK(
        g_serializedFrameObservations[2].frameIndex == 201u &&
        g_serializedFrameObservations[2].kind ==
            kSerializedLifecycleObservation &&
        Near(
            g_serializedFrameObservations[2].viewportWidth,
            2222.0f));
    CHECK(
        g_serializedFrameObservations[3].frameIndex == 201u &&
        g_serializedFrameObservations[3].kind ==
            kSerializedRenderObservation &&
        Near(
            g_serializedFrameObservations[3].viewportWidth,
            2222.0f));
    CHECK(g_renderLifecycleCount == 15u);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[13],
            WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED) == 0);
    CHECK(
        std::strcmp(
            g_renderLifecycleTopics[14],
            WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED) == 0);
    CHECK(
        g_renderLifecyclePayloads[13].frame_index == 200u);
    CHECK(
        g_renderLifecyclePayloads[14].frame_index == 201u);
    CHECK(
        g_renderLifecycleThreadRoles[13] ==
            WOTBMOD_V3_THREAD_RENDER &&
        g_renderLifecycleThreadRoles[14] ==
            WOTBMOD_V3_THREAD_RENDER);
    CHECK(
        render->unregister_callback(
            mod,
            serializedRenderToken) == WOTBMOD_V3_OK);

    ClientHostFrame quiescenceFrame = secondConcurrentFrame;
    SelfRenderUnsubscribeContext selfRender = {};
    selfRender.api = render;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT,
            10,
            &SelfUnregisterRenderCallback,
            &selfRender,
            &selfRender.token) == WOTBMOD_V3_OK);
    quiescenceFrame.frame_index = 202u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(selfRender.calls == 1u);
    CHECK(selfRender.result == WOTBMOD_V3_OK);
    quiescenceFrame.frame_index = 203u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(selfRender.calls == 1u);

    BlockingCallbackGate renderGate = {};
    WotbModV3Token blockingRenderToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT,
            10,
            &BlockingRenderCallback,
            &renderGate,
            &blockingRenderToken) == WOTBMOD_V3_OK);
    quiescenceFrame.frame_index = 204u;
    std::thread blockingRenderThread([&]() {
        wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    });
    bool blockingRenderEntered = false;
    {
        std::unique_lock<std::mutex> lock(renderGate.mutex);
        blockingRenderEntered = renderGate.cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [&]() {
                return renderGate.entered;
            });
    }
    CHECK(blockingRenderEntered);
    std::atomic<bool> renderUnregisterStarted{false};
    std::atomic<bool> renderUnregisterReturned{false};
    WotbModV3Result renderUnregisterResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread renderUnregisterThread([&]() {
        renderUnregisterStarted.store(
            true,
            std::memory_order_release);
        renderUnregisterResult =
            render->unregister_callback(
                mod,
                blockingRenderToken);
        renderUnregisterReturned.store(
            true,
            std::memory_order_release);
    });
    while (!renderUnregisterStarted.load(
        std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    CHECK(
        !renderUnregisterReturned.load(
            std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(renderGate.mutex);
        renderGate.release = true;
    }
    renderGate.cv.notify_all();
    blockingRenderThread.join();
    renderUnregisterThread.join();
    CHECK(renderUnregisterResult == WOTBMOD_V3_OK);
    CHECK(renderGate.calls == 1u);
    quiescenceFrame.frame_index = 205u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(renderGate.calls == 1u);

    ConcurrentRenderSelfOperationContext concurrentRenderSelf = {};
    concurrentRenderSelf.api = render;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT,
            10,
            &ConcurrentRenderSelfOperationCallback,
            &concurrentRenderSelf,
            &concurrentRenderSelf.token) == WOTBMOD_V3_OK);
    quiescenceFrame.frame_index = 206u;
    std::thread concurrentRenderPump([&]() {
        wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    });
    bool concurrentRenderEntered = false;
    {
        std::unique_lock<std::mutex> lock(
            concurrentRenderSelf.mutex);
        concurrentRenderEntered =
            concurrentRenderSelf.cv.wait_for(
                lock,
                std::chrono::seconds(2),
                [&]() {
                    return concurrentRenderSelf.entered;
                });
    }
    CHECK(concurrentRenderEntered);
    WotbModV3Result concurrentRenderUnregisterResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread concurrentRenderUnregister([&]() {
        {
            std::lock_guard<std::mutex> lock(
                concurrentRenderSelf.mutex);
            concurrentRenderSelf.externalUnregisterStarted = true;
        }
        concurrentRenderSelf.cv.notify_all();
        concurrentRenderUnregisterResult =
            render->unregister_callback(
                mod,
                concurrentRenderSelf.token);
    });
    concurrentRenderPump.join();
    concurrentRenderUnregister.join();
    CHECK(concurrentRenderSelf.calls == 1u);
    CHECK(
        concurrentRenderSelf.result ==
        WOTBMOD_V3_E_OBJECT_DESTROYED);
    CHECK(concurrentRenderUnregisterResult == WOTBMOD_V3_OK);

    WotbModV3Token doubleRenderToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        render->register_callback(
            mod,
            WOTBMOD_V3_RENDER_PHASE_PRESENT,
            10,
            &RenderCallback,
            nullptr,
            &doubleRenderToken) == WOTBMOD_V3_OK);
    std::atomic<uint32_t> doubleRenderReady{0u};
    std::atomic<bool> releaseDoubleRender{false};
    WotbModV3Result doubleRenderResults[2] = {
        WOTBMOD_V3_E_PLATFORM,
        WOTBMOD_V3_E_PLATFORM};
    std::thread doubleRenderThreads[2];
    for (uint32_t index = 0u; index < 2u; ++index) {
        doubleRenderThreads[index] = std::thread(
            [&, index]() {
                doubleRenderReady.fetch_add(
                    1u,
                    std::memory_order_release);
                while (!releaseDoubleRender.load(
                    std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                doubleRenderResults[index] =
                    render->unregister_callback(
                        mod,
                        doubleRenderToken);
            });
    }
    while (doubleRenderReady.load(
               std::memory_order_acquire) != 2u) {
        std::this_thread::yield();
    }
    releaseDoubleRender.store(
        true,
        std::memory_order_release);
    for (std::thread& thread : doubleRenderThreads) {
        thread.join();
    }
    const uint32_t doubleRenderSuccesses =
        (doubleRenderResults[0] == WOTBMOD_V3_OK ? 1u : 0u) +
        (doubleRenderResults[1] == WOTBMOD_V3_OK ? 1u : 0u);
    CHECK(doubleRenderSuccesses == 1u);
    CHECK(
        doubleRenderResults[0] == WOTBMOD_V3_OK ||
        doubleRenderResults[0] ==
            WOTBMOD_V3_E_OBJECT_DESTROYED ||
        doubleRenderResults[0] ==
            WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        doubleRenderResults[1] == WOTBMOD_V3_OK ||
        doubleRenderResults[1] ==
            WOTBMOD_V3_E_OBJECT_DESTROYED ||
        doubleRenderResults[1] ==
            WOTBMOD_V3_E_INVALID_HANDLE);

    /* The preceding blocked render callback deliberately exceeds the
     * coalescible per-frame CPU budget on slower or loaded builders. Advance
     * the modeled frame before the independent RPC callback contract. */
    quiescenceFrame.frame_index = 207u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);

    WotbModV3ObservedRpc observedRpc = {};
    WOTBMOD_V3_INIT_STRUCT(
        observedRpc,
        WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    observedRpc.direction = WOTBMOD_V3_RPC_INCOMING;
    observedRpc.public_entity_id = 13u;
    observedRpc.sequence = 1u;
    observedRpc.timestamp_microseconds = 1000u;
    Copy(
        observedRpc.entity_type,
        sizeof(observedRpc.entity_type),
        "Avatar");
    Copy(
        observedRpc.method_name,
        sizeof(observedRpc.method_name),
        "onHealthChanged");

    SelfRpcUnsubscribeContext selfRpc = {};
    selfRpc.api = rpc;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &SelfUnregisterRpcCallback,
            &selfRpc,
            &selfRpc.token) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyClientHostObservedRpc(
            &observedRpc) == WOTBMOD_V3_OK);
    CHECK(selfRpc.calls == 1u);
    CHECK(selfRpc.result == WOTBMOD_V3_OK);
    ++observedRpc.sequence;
    CHECK(
        wotbmod::v3::NotifyClientHostObservedRpc(
            &observedRpc) == WOTBMOD_V3_OK);
    CHECK(selfRpc.calls == 1u);

    ConcurrentSelfRpcUnsubscribeContext concurrentSelfRpc = {};
    concurrentSelfRpc.api = rpc;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &ConcurrentSelfUnregisterRpcCallback,
            &concurrentSelfRpc,
            &concurrentSelfRpc.token) == WOTBMOD_V3_OK);
    WotbModV3Result concurrentRpcNotifyResults[2] = {
        WOTBMOD_V3_E_PLATFORM,
        WOTBMOD_V3_E_PLATFORM};
    std::thread firstConcurrentRpcThread([&]() {
        concurrentRpcNotifyResults[0] =
            wotbmod::v3::NotifyClientHostObservedRpc(
                &observedRpc);
    });
    bool concurrentSelfRpcEntered = false;
    {
        std::unique_lock<std::mutex> lock(
            concurrentSelfRpc.mutex);
        concurrentSelfRpcEntered =
            concurrentSelfRpc.cv.wait_for(
                lock,
                std::chrono::seconds(2),
                [&]() {
                    return concurrentSelfRpc.entered;
                });
    }
    CHECK(concurrentSelfRpcEntered);
    std::atomic<bool> secondConcurrentRpcStarted{false};
    std::thread secondConcurrentRpcThread([&]() {
        secondConcurrentRpcStarted.store(
            true,
            std::memory_order_release);
        concurrentRpcNotifyResults[1] =
            wotbmod::v3::NotifyClientHostObservedRpc(
                &observedRpc);
    });
    while (!secondConcurrentRpcStarted.load(
        std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(50));
    {
        std::lock_guard<std::mutex> lock(
            concurrentSelfRpc.mutex);
        concurrentSelfRpc.release = true;
    }
    concurrentSelfRpc.cv.notify_all();
    firstConcurrentRpcThread.join();
    secondConcurrentRpcThread.join();
    CHECK(concurrentSelfRpc.calls == 1u);
    CHECK(concurrentSelfRpc.result == WOTBMOD_V3_OK);
    CHECK(
        concurrentRpcNotifyResults[0] == WOTBMOD_V3_OK &&
        concurrentRpcNotifyResults[1] == WOTBMOD_V3_OK);

    /* The previous deliberately blocked callback consumed this mod's
     * per-frame CPU budget. A real client advances the budget at Present;
     * model that boundary before testing the next independent callback. */
    quiescenceFrame.frame_index = 208u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);

    BlockingCallbackGate rpcGate = {};
    WotbModV3Token blockingRpcToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &BlockingRpcCallback,
            &rpcGate,
            &blockingRpcToken) == WOTBMOD_V3_OK);
    WotbModV3Result rpcNotifyResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread blockingRpcThread([&]() {
        rpcNotifyResult =
            wotbmod::v3::NotifyClientHostObservedRpc(
                &observedRpc);
    });
    bool blockingRpcEntered = false;
    {
        std::unique_lock<std::mutex> lock(rpcGate.mutex);
        blockingRpcEntered = rpcGate.cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [&]() {
                return rpcGate.entered;
            });
    }
    CHECK(blockingRpcEntered);
    std::atomic<bool> rpcUnregisterStarted{false};
    std::atomic<bool> rpcUnregisterReturned{false};
    WotbModV3Result rpcUnregisterResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread rpcUnregisterThread([&]() {
        rpcUnregisterStarted.store(
            true,
            std::memory_order_release);
        rpcUnregisterResult =
            rpc->unsubscribe_observed(
                mod,
                blockingRpcToken);
        rpcUnregisterReturned.store(
            true,
            std::memory_order_release);
    });
    while (!rpcUnregisterStarted.load(
        std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    CHECK(
        !rpcUnregisterReturned.load(
            std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(rpcGate.mutex);
        rpcGate.release = true;
    }
    rpcGate.cv.notify_all();
    blockingRpcThread.join();
    rpcUnregisterThread.join();
    CHECK(rpcNotifyResult == WOTBMOD_V3_OK);
    CHECK(rpcUnregisterResult == WOTBMOD_V3_OK);
    CHECK(rpcGate.calls == 1u);
    ++observedRpc.sequence;
    CHECK(
        wotbmod::v3::NotifyClientHostObservedRpc(
            &observedRpc) == WOTBMOD_V3_OK);
    CHECK(rpcGate.calls == 1u);
    CHECK(g_rpcSubscribeCalls == 3u);
    CHECK(g_rpcUnsubscribeCalls == 3u);

    WotbModV3Token retryReleaseRpcToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &retryReleaseRpcToken) == WOTBMOD_V3_OK);
    const uint32_t releaseAttemptsBefore =
        g_rpcUnsubscribeAttempts;
    const uint32_t successfulReleasesBefore =
        g_rpcUnsubscribeCalls;
    g_failNextRpcUnsubscribe.store(
        true,
        std::memory_order_release);
    CHECK(
        handles->release(mod, retryReleaseRpcToken) ==
        WOTBMOD_V3_OK);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        releaseAttemptsBefore + 1u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        successfulReleasesBefore);
    quiescenceFrame.frame_index = 206u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        releaseAttemptsBefore + 2u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        successfulReleasesBefore + 1u);

    const uint32_t policyAttemptsBefore =
        g_rpcUnsubscribeAttempts.load(std::memory_order_acquire);
    const uint32_t policySuccessesBefore =
        g_rpcUnsubscribeCalls.load(std::memory_order_acquire);
    g_nextRpcUnsubscribeResult.store(
        WOTBMOD_V3_E_PLATFORM,
        std::memory_order_release);
    WotbModV3BigWorldRpcPolicy failedProbePolicy = {};
    WOTBMOD_V3_INIT_STRUCT(
        failedProbePolicy,
        WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    CHECK(
        rpc->get_policy(mod, &failedProbePolicy) ==
        WOTBMOD_V3_E_PLATFORM);
    CHECK(failedProbePolicy.metadata_observation == 0u);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        policyAttemptsBefore + 1u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        policySuccessesBefore);
    quiescenceFrame.frame_index = 207u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        policyAttemptsBefore + 2u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        policySuccessesBefore + 1u);

    WotbModV3Token reentrantRpcToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &reentrantRpcToken) == WOTBMOD_V3_OK);
    const uint32_t reentrantAttemptsBefore =
        g_rpcUnsubscribeAttempts.load(std::memory_order_acquire);
    const uint32_t reentrantSuccessesBefore =
        g_rpcUnsubscribeCalls.load(std::memory_order_acquire);
    g_reentrantHandles = handles;
    g_reentrantReleaseToken = reentrantRpcToken;
    g_reentrantReleaseOnUnsubscribe.store(
        true,
        std::memory_order_release);
    CHECK(
        rpc->unsubscribe_observed(
            mod,
            reentrantRpcToken) == WOTBMOD_V3_E_PLATFORM);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        reentrantAttemptsBefore + 1u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        reentrantSuccessesBefore);
    quiescenceFrame.frame_index = 208u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        reentrantAttemptsBefore + 2u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        reentrantSuccessesBefore + 1u);
    g_reentrantHandles = nullptr;
    g_reentrantReleaseToken = WOTBMOD_V3_INVALID_HANDLE;

    /*
     * A SHOT MUST REACH A SUBSCRIBER, NOT MERELY BE PUBLISHED.
     *
     * On 11.19.0.834 the loader half of this was already correct - the
     * showShooting detour resolved the owner and PublishNewProjectile returned
     * 0 - yet 110k traced events across 82 sessions held not one projectile
     * event. The four topics were missing from SystemEventTopics(), so
     * EventsSubscribe refused every subscription and the publishes had no
     * listeners. Both halves are asserted here, in the one binary that links
     * client_services and runtime_services together.
     */
    WotbModV3EventSubscriptionInfo projectileEvents = {};
    projectileEvents.struct_size = sizeof(projectileEvents);
    projectileEvents.api_version = WOTBMOD_V3_EVENTS_VERSION;
    projectileEvents.topic_pattern = "wotbmod.gameplay.projectile.*";
    projectileEvents.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    projectileEvents.receive_system_events = 1u;
    WotbModV3EventToken projectileEventsToken =
        WOTBMOD_V3_INVALID_HANDLE;

    // Disarmed, the pattern has no publisher and must be refused - the same
    // answer the missing table entries used to give unconditionally.
    const uint64_t restoreSourceMask =
        wotbmod::v3::GetEventSourceMask();
    wotbmod::v3::SetEventSourceMask(0u);
    CHECK(
        events->subscribe(
            mod,
            &projectileEvents,
            &ProjectileLifecycleCallback,
            nullptr,
            &projectileEventsToken) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(projectileEventsToken == WOTBMOD_V3_INVALID_HANDLE);

    wotbmod::v3::SetEventSourceMask(
        WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING |
        WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT);
    CHECK(
        events->subscribe(
            mod,
            &projectileEvents,
            &ProjectileLifecycleCallback,
            nullptr,
            &projectileEventsToken) == WOTBMOD_V3_OK);
    CHECK(projectileEventsToken != WOTBMOD_V3_INVALID_HANDLE);

    // The exact shape ObserveShot builds from a showShooting detour.
    wotbmod::v3::ClientHostProjectile shot = {};
    shot.struct_size = sizeof(shot);
    shot.api_version = WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    shot.native_token = UINT64_C(0x5407000000000001);
    shot.owner_scope = WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER;
    shot.source = WOTBMOD_V3_PROJECTILE_SOURCE_STOCK_SHOT;
    shot.lifecycle_state = WOTBMOD_V3_PROJECTILE_STATE_CREATED;
    shot.primary_entity_id = 538748761u;
    shot.valid_fields =
        WOTBMOD_V3_PROJECTILE_FIELD_PRIMARY_ENTITY;
    WotbModV3ProjectileHandle shotHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        wotbmod::v3::RegisterClientHostProjectile(
            &shot, &shotHandle) == WOTBMOD_V3_OK);
    CHECK(shotHandle != WOTBMOD_V3_INVALID_HANDLE);

    g_projectileEventCount = 0u;
    g_projectileEventPayloadValid = true;
    CHECK(
        wotbmod::v3::PublishClientHostProjectileLifecycle(
            shotHandle, 0u, 0u) == WOTBMOD_V3_OK);
    CHECK(g_projectileEventCount == 1u);
    CHECK(
        std::strcmp(
            g_projectileEventTopics[0],
            WOTBMOD_V3_EVENT_PROJECTILE_CREATED) == 0);
    CHECK(
        g_projectileEventPayloads[0].snapshot.lifecycle_state ==
        WOTBMOD_V3_PROJECTILE_STATE_CREATED);
    CHECK(
        g_projectileEventPayloads[0].snapshot.owner_scope ==
        WOTBMOD_V3_PROJECTILE_OWNER_LOCAL_PLAYER);
    CHECK(
        g_projectileEventPayloads[0].snapshot.primary_entity_id ==
        538748761u);

    shot.lifecycle_state = WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT;
    CHECK(
        wotbmod::v3::UpdateClientHostProjectile(
            shotHandle, &shot) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::PublishClientHostProjectileLifecycle(
            shotHandle,
            WOTBMOD_V3_PROJECTILE_STATE_CREATED,
            0u) == WOTBMOD_V3_OK);
    CHECK(g_projectileEventCount == 2u);
    CHECK(
        std::strcmp(
            g_projectileEventTopics[1],
            WOTBMOD_V3_EVENT_PROJECTILE_UPDATED) == 0);

    shot.lifecycle_state = WOTBMOD_V3_PROJECTILE_STATE_IMPACTED;
    CHECK(
        wotbmod::v3::UpdateClientHostProjectile(
            shotHandle, &shot) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::PublishClientHostProjectileLifecycle(
            shotHandle,
            WOTBMOD_V3_PROJECTILE_STATE_IN_FLIGHT,
            0u) == WOTBMOD_V3_OK);
    CHECK(g_projectileEventCount == 3u);
    CHECK(
        std::strcmp(
            g_projectileEventTopics[2],
            WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED) == 0);

    CHECK(
        wotbmod::v3::RemoveClientHostProjectile(
            shotHandle,
            WOTBMOD_V3_PROJECTILE_DESTROY_IMPACT_COMPLETE) ==
        WOTBMOD_V3_OK);
    CHECK(g_projectileEventCount == 4u);
    CHECK(
        std::strcmp(
            g_projectileEventTopics[3],
            WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED) == 0);
    CHECK(
        g_projectileEventPayloads[3].reason ==
        WOTBMOD_V3_PROJECTILE_DESTROY_IMPACT_COMPLETE);
    CHECK(g_projectileEventPayloadValid);

    CHECK(
        events->unsubscribe(mod, projectileEventsToken) ==
        WOTBMOD_V3_OK);
    {
        /*
         * wotbmod.ges: registered with the other declared interfaces. Without
         * a backend the interface is UNAVAILABLE and query_interface refuses
         * with NOT_SUPPORTED rather than handing out a table whose every slot
         * would fail; with the three ges_* slots installed it answers and
         * list_types goes through the backend. Its topics are not in
         * SystemEventTopics() - the GES source bit alone decides.
         */
        const void* gesTable = nullptr;
        CHECK(
            bootstrap->query_interface(
                mod,
                WOTBMOD_V3_IFACE_GES,
                WOTBMOD_V3_GES_VERSION,
                &gesTable) == WOTBMOD_V3_E_NOT_SUPPORTED);

        wotbmod::v3::ClientHostDeclaredBackend gesBackend = {};
        gesBackend.struct_size = sizeof(gesBackend);
        gesBackend.api_version =
            WOTBMOD_V3_CLIENT_DECLARED_BACKEND_VERSION;
        gesBackend.compatibility_state =
            WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
        gesBackend.ges_list_types = &FakeGesListTypes;
        gesBackend.ges_observe = &FakeGesObserve;
        gesBackend.ges_publish = &FakeGesPublish;
        wotbmod::v3::SetClientHostDeclaredBackend(&gesBackend);
        CHECK(
            bootstrap->query_interface(
                mod,
                WOTBMOD_V3_IFACE_GES,
                WOTBMOD_V3_GES_VERSION,
                &gesTable) == WOTBMOD_V3_OK);
        const WotbModV3GesApiV1* ges =
            static_cast<const WotbModV3GesApiV1*>(gesTable);
        CHECK(ges != nullptr && ges->struct_size == sizeof(WotbModV3GesApiV1));
        uint32_t gesTypeCount = 0u;
        CHECK(ges->list_types(mod, nullptr, 0u, &gesTypeCount) == WOTBMOD_V3_OK);
        CHECK(gesTypeCount == 2u);

        WotbModV3EventSubscriptionInfo gesEvents = {};
        gesEvents.struct_size = sizeof(gesEvents);
        gesEvents.api_version = WOTBMOD_V3_EVENTS_VERSION;
        gesEvents.topic_pattern = "wotbmod.ges.*";
        gesEvents.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
        gesEvents.receive_system_events = 1u;
        WotbModV3EventToken gesToken = WOTBMOD_V3_INVALID_HANDLE;
        wotbmod::v3::SetEventSourceMask(0u);
        CHECK(
            events->subscribe(
                mod,
                &gesEvents,
                &ProjectileLifecycleCallback,
                nullptr,
                &gesToken) == WOTBMOD_V3_E_NOT_SUPPORTED);
        wotbmod::v3::SetEventSourceMask(WOTBMOD_V3_EVENT_SOURCE_GES);
        CHECK(
            events->subscribe(
                mod,
                &gesEvents,
                &ProjectileLifecycleCallback,
                nullptr,
                &gesToken) == WOTBMOD_V3_OK);
        CHECK(events->unsubscribe(mod, gesToken) == WOTBMOD_V3_OK);
        wotbmod::v3::SetClientHostDeclaredBackend(nullptr);
        CHECK(
            bootstrap->query_interface(
                mod,
                WOTBMOD_V3_IFACE_GES,
                WOTBMOD_V3_GES_VERSION,
                &gesTable) == WOTBMOD_V3_E_NOT_SUPPORTED);
    }
    wotbmod::v3::SetEventSourceMask(restoreSourceMask);

    CHECK(
        events->unsubscribe(mod, renderEventsToken) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::RemoveClientHostPublicEntity(
            localHandle) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::RemoveClientHostPublicEntity(
            allyHandle) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::RemoveClientHostPublicEntity(
            effectHandle) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::RemoveClientHostPublicEntity(
            partialHandle) == WOTBMOD_V3_OK);

    BackendProbe backendAProbe = {};
    backendAProbe.tokenPrefix =
        UINT64_C(0xA100000000000000);
    ClientHostBackend backendA = backend;
    backendA.user_data = &backendAProbe;
    wotbmod::v3::SetClientHostBackend(&backendA);
    WotbModV3Token backendAToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &backendAToken) == WOTBMOD_V3_OK);
    g_nextRpcUnsubscribeResult.store(
        WOTBMOD_V3_E_PLATFORM,
        std::memory_order_release);
    CHECK(
        handles->release(mod, backendAToken) ==
        WOTBMOD_V3_OK);
    CHECK(backendAProbe.unsubscribeAttempts == 1u);
    CHECK(backendAProbe.unsubscribeCalls == 0u);

    BackendProbe backendBProbe = {};
    backendBProbe.tokenPrefix =
        UINT64_C(0xB200000000000000);
    ClientHostBackend backendB = backend;
    backendB.user_data = &backendBProbe;
    wotbmod::v3::SetClientHostBackend(&backendB);
    CHECK(backendBProbe.unsubscribeAttempts == 0u);
    CHECK(backendBProbe.wrongGenerationCalls == 0u);
    quiescenceFrame.frame_index = 209u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(backendBProbe.unsubscribeAttempts == 0u);

    WotbModV3Token backendBToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &backendBToken) == WOTBMOD_V3_OK);
    CHECK(
        handles->release(mod, backendBToken) ==
        WOTBMOD_V3_OK);
    CHECK(backendBProbe.unsubscribeAttempts == 1u);
    CHECK(backendBProbe.unsubscribeCalls == 1u);
    CHECK(backendBProbe.wrongGenerationCalls == 0u);

    WotbModV3Token terminalReleaseToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &terminalReleaseToken) == WOTBMOD_V3_OK);
    g_nextRpcUnsubscribeResult.store(
        WOTBMOD_V3_E_NOT_SUPPORTED,
        std::memory_order_release);
    CHECK(
        handles->release(mod, terminalReleaseToken) ==
        WOTBMOD_V3_OK);
    const uint32_t terminalAttempts =
        backendBProbe.unsubscribeAttempts.load(
            std::memory_order_acquire);
    CHECK(terminalAttempts == 2u);
    quiescenceFrame.frame_index = 210u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    quiescenceFrame.frame_index = 211u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(backendBProbe.unsubscribeAttempts == terminalAttempts);
    CHECK(backendBProbe.unsubscribeCalls == 1u);
    CHECK(backendBProbe.wrongGenerationCalls == 0u);
    wotbmod::v3::SetClientHostBackend(&backend);

    const uint32_t boundedAttemptsBefore =
        g_rpcUnsubscribeAttempts.load(std::memory_order_acquire);
    const uint32_t boundedSuccessesBefore =
        g_rpcUnsubscribeCalls.load(std::memory_order_acquire);
    g_failAllRpcUnsubscribes.store(
        true,
        std::memory_order_release);
    uint32_t boundedFailures = 0u;
    for (uint32_t index = 0u; index < 4097u; ++index) {
        WotbModV3BigWorldRpcPolicy boundedPolicy = {};
        WOTBMOD_V3_INIT_STRUCT(
            boundedPolicy,
            WOTBMOD_V3_BIGWORLD_RPC_VERSION);
        if (rpc->get_policy(mod, &boundedPolicy) ==
                WOTBMOD_V3_E_PLATFORM &&
            boundedPolicy.metadata_observation == 0u) {
            ++boundedFailures;
        }
    }
    g_failAllRpcUnsubscribes.store(
        false,
        std::memory_order_release);
    CHECK(boundedFailures == 4097u);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        boundedAttemptsBefore + 4097u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        boundedSuccessesBefore);
    quiescenceFrame.frame_index = 212u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(
        g_rpcUnsubscribeAttempts ==
        boundedAttemptsBefore + 8193u);
    CHECK(
        g_rpcUnsubscribeCalls ==
        boundedSuccessesBefore + 4096u);
    const uint32_t drainedBoundedAttempts =
        g_rpcUnsubscribeAttempts.load(std::memory_order_acquire);
    quiescenceFrame.frame_index = 213u;
    wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    CHECK(g_rpcUnsubscribeAttempts == drainedBoundedAttempts);

    WotbModV3AudioDescriptor audioDescriptor = {};
    WOTBMOD_V3_INIT_STRUCT(
        audioDescriptor,
        WOTBMOD_V3_AUDIO_VERSION);
    audioDescriptor.uri = "audio-lifetime.wav";
    audioDescriptor.volume = 1.0f;
    audioDescriptor.pitch = 1.0f;
    WotbModV3AudioHandle audioHandle =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        audio->create(
            mod,
            &audioDescriptor,
            &audioHandle) == WOTBMOD_V3_OK);
    const uint32_t audioDestroysBefore =
        g_audioDestroyCalls.load(std::memory_order_acquire);
    wotbmod::v3::SetAudioLifecycleBeforeStateUpdateHookForTesting(
        &AudioLifecyclePauseHook);
    WotbModV3Result audioNotifyResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread audioNotifyThread([&]() {
        audioNotifyResult =
            wotbmod::v3::NotifyClientHostAudioLifecycle(
                kAudioObject,
                wotbmod::v3::CLIENT_HOST_AUDIO_STARTED,
                0u,
                nullptr);
    });
    bool audioLifecycleEntered = false;
    {
        std::unique_lock<std::mutex> lock(
            g_audioLifecycleGate.mutex);
        audioLifecycleEntered =
            g_audioLifecycleGate.cv.wait_for(
                lock,
                std::chrono::seconds(2),
                []() {
                    return g_audioLifecycleGate.entered;
                });
    }
    CHECK(audioLifecycleEntered);
    WotbModV3Result audioDestroyResult =
        WOTBMOD_V3_E_PLATFORM;
    std::atomic<bool> audioDestroyReturned{false};
    std::thread audioDestroyThread([&]() {
        audioDestroyResult =
            audio->destroy(mod, audioHandle);
        audioDestroyReturned.store(
            true,
            std::memory_order_release);
    });
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    CHECK(
        !audioDestroyReturned.load(
            std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(
            g_audioLifecycleGate.mutex);
        g_audioLifecycleGate.release = true;
    }
    g_audioLifecycleGate.cv.notify_all();
    audioNotifyThread.join();
    audioDestroyThread.join();
    wotbmod::v3::SetAudioLifecycleBeforeStateUpdateHookForTesting(
        nullptr);
    CHECK(audioNotifyResult == WOTBMOD_V3_OK);
    CHECK(audioDestroyResult == WOTBMOD_V3_OK);
    CHECK(
        g_audioDestroyCalls ==
        audioDestroysBefore + 1u);

    WotbModV3Handle unrelatedOwnerMod =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "client_unrelated_owner_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &unrelatedOwnerMod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo unrelatedOwnerModule = {};
    unrelatedOwnerModule.struct_size = sizeof(unrelatedOwnerModule);
    unrelatedOwnerModule.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            unrelatedOwnerMod,
            &OwnerRaceEntry,
            &unrelatedOwnerModule) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_Enable(unrelatedOwnerMod) ==
        WOTBMOD_V3_OK);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockRpcSubscribe = true;
        g_backendGate.rpcSubscribeEntered = false;
        g_backendGate.releaseRpcSubscribe = false;
    }
    WotbModV3Token unaffectedOwnerToken =
        WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result unaffectedOwnerSubscribeResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread unaffectedOwnerSubscribeThread([&]() {
        unaffectedOwnerSubscribeResult =
            rpc->subscribe_observed(
                mod,
                WOTBMOD_V3_RPC_INCOMING,
                nullptr,
                &RpcCallback,
                nullptr,
                &unaffectedOwnerToken);
    });
    bool unaffectedSubscribeEntered = false;
    {
        std::unique_lock<std::mutex> lock(g_backendGate.mutex);
        unaffectedSubscribeEntered =
            g_backendGate.cv.wait_for(
                lock,
                std::chrono::seconds(2),
                []() {
                    return g_backendGate.rpcSubscribeEntered;
                });
    }
    CHECK(unaffectedSubscribeEntered);
    CHECK(
        WotbModV3Runtime_DestroyMod(unrelatedOwnerMod) ==
        WOTBMOD_V3_OK);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.releaseRpcSubscribe = true;
    }
    g_backendGate.cv.notify_all();
    unaffectedOwnerSubscribeThread.join();
    CHECK(unaffectedOwnerSubscribeResult == WOTBMOD_V3_OK);
    CHECK(
        unaffectedOwnerToken != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        rpc->unsubscribe_observed(
            mod,
            unaffectedOwnerToken) == WOTBMOD_V3_OK);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockRpcSubscribe = false;
        g_backendGate.rpcSubscribeEntered = false;
        g_backendGate.releaseRpcSubscribe = false;
    }

    WotbModV3Handle ownerRaceMod =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "client_owner_race_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &ownerRaceMod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo ownerRaceModule = {};
    ownerRaceModule.struct_size = sizeof(ownerRaceModule);
    ownerRaceModule.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            ownerRaceMod,
            &OwnerRaceEntry,
            &ownerRaceModule) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_Enable(ownerRaceMod) ==
        WOTBMOD_V3_OK);
    const WotbModV3BigWorldRpcApiV1* ownerRaceRpc =
        Query<WotbModV3BigWorldRpcApiV1>(
            bootstrap,
            ownerRaceMod,
            WOTBMOD_V3_IFACE_BIGWORLD_RPC,
            WOTBMOD_V3_BIGWORLD_RPC_VERSION);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockRpcSubscribe = true;
        g_backendGate.rpcSubscribeEntered = false;
        g_backendGate.releaseRpcSubscribe = false;
    }
    WotbModV3Token ownerRaceToken =
        WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result ownerRaceSubscribeResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread ownerRaceSubscribeThread([&]() {
        ownerRaceSubscribeResult =
            ownerRaceRpc->subscribe_observed(
                ownerRaceMod,
                WOTBMOD_V3_RPC_INCOMING,
                nullptr,
                &RpcCallback,
                nullptr,
                &ownerRaceToken);
    });
    bool ownerSubscribeEntered = false;
    {
        std::unique_lock<std::mutex> lock(g_backendGate.mutex);
        ownerSubscribeEntered = g_backendGate.cv.wait_for(
            lock,
            std::chrono::seconds(2),
            []() {
                return g_backendGate.rpcSubscribeEntered;
            });
    }
    CHECK(ownerSubscribeEntered);
    {
        std::lock_guard<std::mutex> lock(g_lifecycleGate.mutex);
        g_lifecycleGate.target = ownerRaceMod;
        g_lifecycleGate.unloadedEntered = false;
        g_lifecycleGate.releaseUnloaded = false;
    }
    WotbModV3Result ownerRaceDestroyResult =
        WOTBMOD_V3_E_PLATFORM;
    std::thread ownerRaceDestroyThread([&]() {
        ownerRaceDestroyResult =
            WotbModV3Runtime_DestroyMod(ownerRaceMod);
    });
    bool ownerUnloadedEntered = false;
    {
        std::unique_lock<std::mutex> lock(g_lifecycleGate.mutex);
        ownerUnloadedEntered = g_lifecycleGate.cv.wait_for(
            lock,
            std::chrono::seconds(2),
            []() {
                return g_lifecycleGate.unloadedEntered;
            });
    }
    CHECK(ownerUnloadedEntered);
    WotbModV3Token lateOwnerRaceToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        ownerRaceRpc->subscribe_observed(
            ownerRaceMod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &lateOwnerRaceToken) ==
        WOTBMOD_V3_E_OBJECT_DESTROYED);
    CHECK(
        lateOwnerRaceToken ==
        WOTBMOD_V3_INVALID_HANDLE);
    const uint32_t ownerCleanupBefore =
        g_rpcUnsubscribeCalls.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.releaseRpcSubscribe = true;
    }
    g_backendGate.cv.notify_all();
    ownerRaceSubscribeThread.join();
    CHECK(
        ownerRaceSubscribeResult ==
        WOTBMOD_V3_E_OBJECT_DESTROYED);
    CHECK(ownerRaceToken == WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_rpcUnsubscribeCalls ==
        ownerCleanupBefore + 1u);
    {
        std::lock_guard<std::mutex> lock(g_lifecycleGate.mutex);
        g_lifecycleGate.releaseUnloaded = true;
    }
    g_lifecycleGate.cv.notify_all();
    ownerRaceDestroyThread.join();
    CHECK(ownerRaceDestroyResult == WOTBMOD_V3_OK);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockRpcSubscribe = false;
        g_backendGate.rpcSubscribeEntered = false;
        g_backendGate.releaseRpcSubscribe = false;
    }

    WotbModV3Token shutdownRetryToken =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        rpc->subscribe_observed(
            mod,
            WOTBMOD_V3_RPC_INCOMING,
            nullptr,
            &RpcCallback,
            nullptr,
            &shutdownRetryToken) == WOTBMOD_V3_OK);
    g_nextRpcUnsubscribeResult.store(
        WOTBMOD_V3_E_PLATFORM,
        std::memory_order_release);
    CHECK(
        handles->release(mod, shutdownRetryToken) ==
        WOTBMOD_V3_OK);
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockRpcUnsubscribe = true;
        g_backendGate.rpcUnsubscribeEntered = false;
        g_backendGate.releaseRpcUnsubscribe = false;
    }
    quiescenceFrame.frame_index = 500u;
    std::thread shutdownPumpThread([&]() {
        wotbmod::v3::PumpClientHostFrame(&quiescenceFrame);
    });
    bool shutdownRetryEntered = false;
    {
        std::unique_lock<std::mutex> lock(g_backendGate.mutex);
        shutdownRetryEntered = g_backendGate.cv.wait_for(
            lock,
            std::chrono::seconds(2),
            []() {
                return g_backendGate.rpcUnsubscribeEntered;
            });
    }
    CHECK(shutdownRetryEntered);
    std::atomic<bool> shutdownReturned{false};
    std::thread shutdownThread([&]() {
        WotbModV3Runtime_Shutdown();
        shutdownReturned.store(
            true,
            std::memory_order_release);
    });
    std::this_thread::sleep_for(
        std::chrono::milliseconds(100));
    CHECK(
        !shutdownReturned.load(
            std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.releaseRpcUnsubscribe = true;
    }
    g_backendGate.cv.notify_all();
    shutdownPumpThread.join();
    shutdownThread.join();
    CHECK(
        shutdownReturned.load(
            std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(g_backendGate.mutex);
        g_backendGate.blockRpcUnsubscribe = false;
        g_backendGate.rpcUnsubscribeEntered = false;
        g_backendGate.releaseRpcUnsubscribe = false;
    }

    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);
    WotbModV3Handle postShutdownMod =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "client_post_shutdown_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &postShutdownMod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo postShutdownModule = {};
    postShutdownModule.struct_size = sizeof(postShutdownModule);
    postShutdownModule.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            postShutdownMod,
            &TestEntry,
            &postShutdownModule) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_Enable(postShutdownMod) ==
        WOTBMOD_V3_OK);
    const WotbModV3Bootstrap* postShutdownBootstrap =
        WotbModV3Runtime_GetBootstrap();
    const WotbModV3RenderApiV1* postShutdownRender =
        Query<WotbModV3RenderApiV1>(
            postShutdownBootstrap,
            postShutdownMod,
            WOTBMOD_V3_IFACE_RENDER,
            WOTBMOD_V3_RENDER_VERSION);
    uint32_t postShutdownBackend = UINT32_MAX;
    CHECK(
        postShutdownRender->get_backend(
            postShutdownMod,
            &postShutdownBackend) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        postShutdownBackend ==
        WOTBMOD_V3_RENDER_BACKEND_NONE);
    CHECK(
        WotbModV3Runtime_DestroyMod(postShutdownMod) ==
        WOTBMOD_V3_OK);
    wotbmod::v3::SetClientHostBackend(nullptr);
    WotbModV3Runtime_Shutdown();

    std::printf(
        "v3 truthful client slices checks: %u, failures: %u\n",
        g_checks,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
