#include "../include/wotbmod/async_v1.h"
#include "../include/wotbmod/events_v1.h"
#include "../include/wotbmod/hooks_v1.h"
#include "../include/wotbmod/http_v1.h"
#include "../include/wotbmod/intermod_v1.h"
#include "../include/wotbmod/lifecycle_v1.h"
#include "../include/wotbmod/projectile_v2.h"
#include "../include/wotbmod/unsafe_native_v1.h"
#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotbmod/ges_v1.h"
#include "../include/wotbmod/session_cluster_v1.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>

#define WOTBMOD_ASSERT_ABI_HEADER(type)                                  \
    static_assert(std::is_standard_layout<type>::value, #type);          \
    static_assert(offsetof(type, struct_size) == 0u, #type);             \
    static_assert(offsetof(type, api_version) == sizeof(uint32_t), #type)

WOTBMOD_ASSERT_ABI_HEADER(WotbModV3LifecycleInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3LifecycleEvent);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3LifecycleApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3Event);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3EventSubscriptionInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3EventDispatchInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3EventsApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3HookCreateInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3HookInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3HookConflictInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3HooksApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3UnsafeNativeApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3TaskInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3TaskSubmitInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3TimerCreateInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3TimerInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3AsyncApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3HttpRequestInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3HttpApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ExportedInterfaceInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ImportedInterface);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3IntermodMessage);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3IntermodApiV1);

namespace wotbmod {
namespace v3 {
#if defined(WOTBMOD_V3_RUNTIME_SERVICES_STANDALONE_TEST)
void RegisterDataServices() {}
void RegisterClientServices() {}
void HudMainThreadTick() {}
void RegisterToolingServices() {}
namespace testing {
void ShutdownEventSubscriptionsForTesting();
void ArmEventSubscribePublishBarrierForTesting();
bool WaitEventSubscribePublishBarrierForTesting(uint32_t timeout_ms);
void ReleaseEventSubscribePublishBarrierForTesting();
bool EventSubscriptionsAcceptingForTesting();
}
#endif
void SetMainIngressOnline(bool online);
uint32_t PumpMainThread(uint32_t max_callbacks);
typedef WotbModV3Result (WOTBMOD_V3_CALL* MainThreadCallFn)(
    void* user_data);
typedef void (WOTBMOD_V3_CALL* MainThreadCancelFn)(void* user_data);
WotbModV3Result InvokeMainThreadBlocking(
    MainThreadCallFn callback,
    void* user_data,
    uint32_t timeout_ms);
WotbModV3Result PostMainThread(
    MainThreadCallFn callback,
    void* user_data);
WotbModV3Result PostMainThreadEx(
    MainThreadCallFn callback,
    void* user_data,
    MainThreadCancelFn cancel);
uint32_t CurrentThreadRole();
uint32_t SetCurrentThreadRole(uint32_t role);
}  // namespace v3
}  // namespace wotbmod

namespace {

const WotbModV3EventsApiV1* g_events = nullptr;
const WotbModV3AsyncApiV1* g_async = nullptr;
std::atomic<uint32_t> g_high_event_count{0u};
std::atomic<uint32_t> g_low_event_count{0u};
std::atomic<uint32_t> g_event_info_count{0u};
std::atomic<uint32_t> g_task_completion_count{0u};
std::atomic<uint32_t> g_dispatch_count{0u};
std::atomic<uint32_t> g_main_dispatch_count{0u};
std::atomic<uint32_t> g_main_ingress_dispatch_count{0u};
std::atomic<uint32_t> g_main_ingress_reentrant_count{0u};
std::atomic<uint32_t> g_main_ingress_reentrant_pump_result{
    0xFFFFFFFFu};
std::atomic<uint32_t> g_main_ingress_follower_count{0u};
// Set if the follower dispatch ever runs while the first one is still on
// the stack, i.e. if the reentrancy guard failed to stop a nested pump.
std::atomic<uint32_t> g_main_ingress_follower_ran_nested{0u};
std::atomic<uint32_t> g_main_ingress_in_reentrant_callback{0u};
// Monotonic tick stamped by each callback so ordering can be asserted.
std::atomic<uint32_t> g_main_ingress_sequence{0u};
std::atomic<uint32_t> g_main_ingress_reentrant_exit_seq{0u};
std::atomic<uint32_t> g_main_ingress_follower_seq{0u};
struct BlockingMainCallTestState {
    std::atomic<uint32_t> calls{0u};
    std::atomic<uint32_t> cancellations{0u};
    std::atomic<uint32_t> observed_role{WOTBMOD_V3_THREAD_UNKNOWN};
    WotbModV3Result callback_result = WOTBMOD_V3_OK;
};
std::atomic<uint32_t> g_timer_count{0u};
std::atomic<uint32_t> g_message_count{0u};
std::atomic<uint32_t> g_slow_task_started{0u};
std::atomic<uint32_t> g_slow_task_cancelled{0u};
std::atomic<uint32_t> g_cleanup_count{0u};
std::atomic<uint32_t> g_lifecycle_transition_mask{0u};
std::atomic<uint32_t> g_lifecycle_payload_error{0u};
std::atomic<uint32_t> g_native_hook_create_count{0u};
std::atomic<uint32_t> g_native_hook_enable_count{0u};
std::atomic<uint32_t> g_native_hook_disable_count{0u};
std::atomic<uint32_t> g_native_hook_remove_count{0u};
std::atomic<uint32_t> g_capability_event_count{0u};
WotbModV3CapabilityInfo g_capability_event_payload = {};

struct BlockingEventState {
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false;
    bool release = false;
    std::atomic<bool> alive{true};
    std::atomic<bool> callback_after_unsubscribe{false};
    std::atomic<uint32_t> calls{0u};
};

struct SelfUnsubscribeEventState {
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3EventToken token = WOTBMOD_V3_INVALID_HANDLE;
    std::atomic<int32_t> result{WOTBMOD_V3_E_PLATFORM};
    std::atomic<uint32_t> calls{0u};
};

struct ConcurrentSelfUnsubscribeEventState {
    std::mutex mutex;
    std::condition_variable cv;
    bool first_entered = false;
    bool release_first = false;
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3EventToken token = WOTBMOD_V3_INVALID_HANDLE;
    std::atomic<bool> self_unsubscribe_started{false};
    std::atomic<bool> self_unsubscribe_returned{false};
    std::atomic<int32_t> result{WOTBMOD_V3_E_PLATFORM};
    std::atomic<uint32_t> calls{0u};
};

struct DualSelfUnsubscribeEventState {
    std::mutex mutex;
    std::condition_variable cv;
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3EventToken token = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t arrived = 0u;
    std::atomic<uint32_t> returned{0u};
    std::atomic<int32_t> results[2] = {
        WOTBMOD_V3_E_PLATFORM,
        WOTBMOD_V3_E_PLATFORM};
};

void WOTBMOD_V3_CALL NativeHookTarget() {}
void WOTBMOD_V3_CALL NativeHookDetour() {}
// The one target the version 2 fake describes for OBSERVE/AFTER. Its body
// differs from NativeHookTarget on purpose: /OPT:ICF folds identical empty
// functions into one address, which would make every symbol "described".
volatile int g_observed_target_sink = 0;
void WOTBMOD_V3_CALL ObservedTarget() { g_observed_target_sink += 3; }
// Resolvable and plain reviewed, but described by nobody. Its own address:
// sharing NativeHookTarget would make it an alias of Camera::setFOV and
// charge gameplay.tweak.camera instead of hooks.symbol.
void WOTBMOD_V3_CALL UndescribedTarget() { g_observed_target_sink += 5; }
std::atomic<uint32_t> g_native_attach_count{0u};
std::atomic<uint32_t> g_native_detach_count{0u};
std::atomic<uint32_t> g_native_set_enabled_count{0u};
uint32_t g_last_attach_mode = 99u;
int32_t g_last_attach_priority = -1;
uint64_t g_last_detached_token = 0u;

WotbModV3Result WOTBMOD_V3_CALL NativeHookResolve(
    void*,
    const char* symbol,
    void** out_target) {
    if (!symbol || !out_target) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_target = nullptr;
    // Both spellings resolve to one address, mirroring the shipping binding
    // pack where "Camera::setFOV" and "DAVA::Camera::SetFovY" are the same
    // anchor (kGameCameraSetFovRva). The permission checks below depend on
    // that: a mod must not be able to pick the cheaper name.
    if (std::strcmp(symbol, "Vehicle::set_health") == 0) {
        *out_target = reinterpret_cast<void*>(&ObservedTarget);
        return WOTBMOD_V3_OK;
    }
    // Resolvable, plain reviewed, and NOT described by the version 2 fake.
    if (std::strcmp(symbol, "Vehicle::showShooting") == 0) {
        *out_target = reinterpret_cast<void*>(&UndescribedTarget);
        return WOTBMOD_V3_OK;
    }
    if (std::strcmp(symbol, "Camera::setFOV") != 0 &&
        std::strcmp(symbol, "DAVA::Camera::SetFovY") != 0) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    *out_target = reinterpret_cast<void*>(&NativeHookTarget);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookDescribe(
    void*,
    void* target,
    uint32_t* out_mask) {
    if (!target || !out_mask) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_mask = target == reinterpret_cast<void*>(&ObservedTarget)
        ? ((1u << WOTBMOD_V3_HOOK_OBSERVE) | (1u << WOTBMOD_V3_HOOK_AFTER))
        : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookAttach(
    void*,
    void* target,
    uint32_t mode,
    int32_t priority,
    void* detour,
    uint64_t* out_token) {
    if (!target || !detour || !out_token) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (target != reinterpret_cast<void*>(&ObservedTarget)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    g_last_attach_mode = mode;
    g_last_attach_priority = priority;
    *out_token = 1000u + g_native_attach_count.fetch_add(1u);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookDetach(void*, uint64_t token) {
    g_last_detached_token = token;
    ++g_native_detach_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookSetAttachedEnabled(
    void*,
    uint64_t,
    uint32_t) {
    ++g_native_set_enabled_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookCreate(
    void*,
    void* target,
    void* detour,
    void** out_original) {
    if (!target || !detour || !out_original) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ++g_native_hook_create_count;
    *out_original = target;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookEnable(
    void*,
    void* target) {
    if (!target) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    ++g_native_hook_enable_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookDisable(
    void*,
    void* target) {
    if (!target) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    ++g_native_hook_disable_count;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeHookRemove(
    void*,
    void* target) {
    if (!target) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    ++g_native_hook_remove_count;
    return WOTBMOD_V3_OK;
}

void Copy(char* output, size_t capacity, const char* value) {
#if defined(_MSC_VER)
    strncpy_s(output, capacity, value, _TRUNCATE);
#else
    std::strncpy(output, value, capacity - 1u);
    output[capacity - 1u] = '\0';
#endif
}

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    Copy(info->id, sizeof(info->id), "tests.runtime-services");
    Copy(info->name, sizeof(info->name), "Runtime services test");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL DependencyEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.runtime-dependency");
    Copy(info->name, sizeof(info->name), "Runtime dependency");
    Copy(info->version, sizeof(info->version), "1.4.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SubjectEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.lifecycle-subject");
    Copy(info->name, sizeof(info->name), "Lifecycle subject");
    Copy(info->version, sizeof(info->version), "2.3.4");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

void WOTBMOD_V3_CALL LifecycleEvent(
    WotbModV3Handle,
    WotbModV3Event* event,
    void*) {
    if (!event ||
        event->payload_size != sizeof(WotbModV3LifecycleEvent) ||
        !event->payload ||
        (event->flags & WOTBMOD_V3_EVENT_FLAG_SYSTEM) == 0u) {
        ++g_lifecycle_payload_error;
        return;
    }
    const WotbModV3LifecycleEvent* payload =
        static_cast<const WotbModV3LifecycleEvent*>(event->payload);
    if (payload->struct_size != sizeof(*payload) ||
        payload->api_version != WOTBMOD_V3_LIFECYCLE_VERSION ||
        payload->transition > WOTBMOD_V3_MOD_TRANSITION_UNLOADED ||
        payload->state > WOTBMOD_V3_MOD_STATE_UNLOADED) {
        ++g_lifecycle_payload_error;
        return;
    }
    if (payload->transition != WOTBMOD_V3_MOD_TRANSITION_PRELOAD &&
        std::strcmp(payload->id, "tests.lifecycle-subject") != 0) {
        ++g_lifecycle_payload_error;
        return;
    }
    g_lifecycle_transition_mask.fetch_or(
        1u << payload->transition);
}

void WOTBMOD_V3_CALL HighEvent(
    WotbModV3Handle mod,
    WotbModV3Event* event,
    void*) {
    ++g_high_event_count;
    if (g_events && event) {
        uint32_t thread_role = WOTBMOD_V3_THREAD_UNKNOWN;
        uint64_t timestamp = 0u;
        uint64_t context = 0u;
        if (g_events->get_thread(
                mod, event->dispatch_token, &thread_role) ==
                WOTBMOD_V3_OK &&
            g_events->get_timestamp(
                mod, event->dispatch_token, &timestamp) ==
                WOTBMOD_V3_OK &&
            g_events->get_context(
                mod, event->dispatch_token, &context) ==
                WOTBMOD_V3_OK &&
            thread_role == WOTBMOD_V3_THREAD_MAIN &&
            timestamp != 0u) {
            ++g_event_info_count;
        }
        g_events->stop_propagation(mod, event->dispatch_token);
    }
}

void WOTBMOD_V3_CALL LowEvent(
    WotbModV3Handle,
    WotbModV3Event*,
    void*) {
    ++g_low_event_count;
}

void WOTBMOD_V3_CALL CapabilityChangedEvent(
    WotbModV3Handle,
    WotbModV3Event* event,
    void*) {
    ++g_capability_event_count;
    if (event && event->payload &&
        event->payload_size >= sizeof(WotbModV3CapabilityInfo)) {
        g_capability_event_payload =
            *static_cast<const WotbModV3CapabilityInfo*>(event->payload);
    }
}

void WOTBMOD_V3_CALL BlockingEvent(
    WotbModV3Handle,
    WotbModV3Event*,
    void* user_data) {
    BlockingEventState* state =
        static_cast<BlockingEventState*>(user_data);
    if (!state) return;
    ++state->calls;
    if (!state->alive.load()) {
        state->callback_after_unsubscribe.store(true);
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->cv.notify_all();
    state->cv.wait(lock, [&]() { return state->release; });
    if (!state->alive.load()) {
        state->callback_after_unsubscribe.store(true);
    }
}

void WOTBMOD_V3_CALL SelfUnsubscribeEvent(
    WotbModV3Handle,
    WotbModV3Event*,
    void* user_data) {
    SelfUnsubscribeEventState* state =
        static_cast<SelfUnsubscribeEventState*>(user_data);
    if (!state || !g_events) return;
    ++state->calls;
    state->result.store(
        g_events->unsubscribe(state->mod, state->token));
}

void WOTBMOD_V3_CALL ConcurrentSelfUnsubscribeEvent(
    WotbModV3Handle,
    WotbModV3Event*,
    void* user_data) {
    ConcurrentSelfUnsubscribeEventState* state =
        static_cast<ConcurrentSelfUnsubscribeEventState*>(user_data);
    if (!state || !g_events) return;
    const uint32_t invocation = state->calls.fetch_add(1u);
    if (invocation == 0u) {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->first_entered = true;
        state->cv.notify_all();
        state->cv.wait(
            lock, [&]() { return state->release_first; });
        return;
    }
    state->self_unsubscribe_started.store(true);
    state->result.store(
        g_events->unsubscribe(state->mod, state->token));
    state->self_unsubscribe_returned.store(true);
}

void WOTBMOD_V3_CALL DualSelfUnsubscribeEvent(
    WotbModV3Handle,
    WotbModV3Event*,
    void* user_data) {
    DualSelfUnsubscribeEventState* state =
        static_cast<DualSelfUnsubscribeEventState*>(user_data);
    if (!state || !g_events) return;
    uint32_t invocation = 0u;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        invocation = state->arrived++;
        state->cv.notify_all();
        state->cv.wait(
            lock,
            [&]() {
                return state->arrived >= 2u;
            });
    }
    if (invocation < 2u) {
        state->results[invocation].store(
            g_events->unsubscribe(state->mod, state->token));
    }
    state->returned.fetch_add(1u);
}

WotbModV3Result WOTBMOD_V3_CALL TaskWork(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task,
    void*) {
    static const char result[] = "worker-result";
    if (!g_async) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const WotbModV3Result progress =
        g_async->task_set_progress(mod, task, 0.5f);
    if (progress != WOTBMOD_V3_OK) return progress;
    return g_async->task_set_result(
        mod, task, result, sizeof(result));
}

void WOTBMOD_V3_CALL TaskCompletion(
    WotbModV3Handle,
    WotbModV3TaskHandle,
    WotbModV3Result result,
    void*) {
    if (result == WOTBMOD_V3_OK) ++g_task_completion_count;
}

WotbModV3Result WOTBMOD_V3_CALL SlowTaskWork(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task,
    void*) {
    ++g_slow_task_started;
    for (uint32_t attempt = 0u; attempt < 5000u; ++attempt) {
        uint32_t cancelled = 0u;
        const WotbModV3Result result =
            g_async->task_is_cancellation_requested(
                mod, task, &cancelled);
        if (result != WOTBMOD_V3_OK) return result;
        if (cancelled != 0u) {
            ++g_slow_task_cancelled;
            return WOTBMOD_V3_E_CANCELLED;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return WOTBMOD_V3_E_TIMEOUT;
}

void WOTBMOD_V3_CALL DispatchCallback(WotbModV3Handle, void*) {
    ++g_dispatch_count;
}

void WOTBMOD_V3_CALL MainDispatchCallback(WotbModV3Handle, void*) {
    ++g_main_dispatch_count;
}

void WOTBMOD_V3_CALL MainIngressDispatchCallback(WotbModV3Handle, void*) {
    ++g_main_ingress_dispatch_count;
}

// Reentrancy: pumping from inside a pumped callback must execute nothing
// rather than recursing. Results are recorded rather than asserted here
// because the callback type returns void, so CHECK cannot be used inline.
//
// A second dispatch (the follower, below) is deliberately queued behind
// this one before the outer pump runs. With the guard in place the nested
// pump must find the queue closed and return 0, leaving the follower for
// the outer pump loop to run after this callback returns. With the guard
// removed the nested pump drains the follower right here, inside this
// callback -- which the follower detects and the assertions reject.
void WOTBMOD_V3_CALL MainIngressReentrantDispatchCallback(
    WotbModV3Handle, void*) {
    ++g_main_ingress_reentrant_count;
    g_main_ingress_in_reentrant_callback.store(1u);
    g_main_ingress_reentrant_pump_result.store(
        wotbmod::v3::PumpMainThread(16u));
    g_main_ingress_in_reentrant_callback.store(0u);
    g_main_ingress_reentrant_exit_seq.store(++g_main_ingress_sequence);
}

void WOTBMOD_V3_CALL MainIngressFollowerDispatchCallback(
    WotbModV3Handle, void*) {
    ++g_main_ingress_follower_count;
    if (g_main_ingress_in_reentrant_callback.load() != 0u) {
        g_main_ingress_follower_ran_nested.store(1u);
    }
    g_main_ingress_follower_seq.store(++g_main_ingress_sequence);
}

WotbModV3Result WOTBMOD_V3_CALL BlockingMainCallCallback(void* user_data) {
    BlockingMainCallTestState* state =
        static_cast<BlockingMainCallTestState*>(user_data);
    if (!state) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    state->observed_role.store(wotbmod::v3::CurrentThreadRole());
    ++state->calls;
    return state->callback_result;
}

void WOTBMOD_V3_CALL BlockingMainCallCancel(void* user_data) {
    BlockingMainCallTestState* state =
        static_cast<BlockingMainCallTestState*>(user_data);
    if (state) ++state->cancellations;
}

void WOTBMOD_V3_CALL TimerCallback(
    WotbModV3Handle,
    WotbModV3TimerHandle,
    uint64_t,
    void*) {
    ++g_timer_count;
}

void WOTBMOD_V3_CALL MessageCallback(
    WotbModV3Handle,
    const WotbModV3IntermodMessage*,
    void*) {
    ++g_message_count;
}

void WOTBMOD_V3_CALL CleanupCallback(
    WotbModV3Handle,
    uint32_t,
    void*) {
    ++g_cleanup_count;
}

int ExportedFunction(int value) {
    return value + 1;
}

struct ExportedTable {
    uint32_t struct_size;
    uint32_t api_version;
    int (*increment)(int);
};

int g_pending_detour_marker = 0;

}  // namespace

#define CHECK(expression)                                                \
    do {                                                                 \
        if (!(expression)) {                                             \
            std::fprintf(                                                \
                stderr, "check failed at line %d: %s\n",                \
                __LINE__, #expression);                                  \
            WotbModV3Runtime_Shutdown();                                 \
            return 1;                                                    \
        }                                                                \
    } while (0)

int main() {
    WotbModV3TaskSubmitInfo task = {};
    WOTBMOD_V3_INIT_STRUCT(task, WOTBMOD_V3_ASYNC_VERSION);
    WotbModV3EventSubscriptionInfo event = {};
    WOTBMOD_V3_INIT_STRUCT(event, WOTBMOD_V3_EVENTS_VERSION);
    WotbModV3HookCreateInfo hook = {};
    WOTBMOD_V3_INIT_STRUCT(hook, WOTBMOD_V3_HOOKS_VERSION);
    CHECK(
        task.struct_size != 0u &&
        event.struct_size != 0u &&
        hook.struct_size != 0u);
    WotbModV3GesEvent ges_event = {};
    WOTBMOD_V3_INIT_STRUCT(ges_event, WOTBMOD_V3_GES_VERSION);
    CHECK(ges_event.struct_size == sizeof(WotbModV3GesEvent));
    static_assert(sizeof(ges_event.type_name) == WOTBMOD_V3_GES_TYPE_NAME_SIZE,
                  "ges type name buffer");
    static_assert((WOTBMOD_V3_EVENT_SOURCE_ALL & WOTBMOD_V3_EVENT_SOURCE_GES) != 0u,
                  "GES source bit must be inside SOURCE_ALL");
    CHECK(std::strcmp(WOTBMOD_V3_IFACE_GES, "wotbmod.ges") == 0);

    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_runtime_test\\mods";
    options.cache_directory = "build\\v3_runtime_test\\cache";
    options.config_directory = "build\\v3_runtime_test\\config";
    options.client_version = "test";
    WotbModV3NativeHookBackend native_hook_backend = {};
    native_hook_backend.struct_size = sizeof(native_hook_backend);
    native_hook_backend.api_version =
        WOTBMOD_V3_NATIVE_HOOK_BACKEND_VERSION;
    native_hook_backend.resolve_symbol = &NativeHookResolve;
    native_hook_backend.create = &NativeHookCreate;
    native_hook_backend.enable = &NativeHookEnable;
    native_hook_backend.disable = &NativeHookDisable;
    native_hook_backend.remove = &NativeHookRemove;
    native_hook_backend.describe_target = &NativeHookDescribe;
    native_hook_backend.attach = &NativeHookAttach;
    native_hook_backend.detach = &NativeHookDetach;
    native_hook_backend.set_attached_enabled =
        &NativeHookSetAttachedEnabled;
    options.native_hook_backend = &native_hook_backend;
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);

    WotbModV3Handle dependency_mod =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "runtime_dependency.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &dependency_mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo dependency_module = {};
    dependency_module.struct_size = sizeof(dependency_module);
    dependency_module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            dependency_mod,
            &DependencyEntry,
            &dependency_module) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_Enable(dependency_mod) ==
        WOTBMOD_V3_OK);

    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "build\\v3_runtime_test\\package\\bin\\..\\bin\\"
            "runtime_services_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &mod) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_SetPackageRoot(
            mod,
            "build\\v3_runtime_test") == WOTBMOD_V3_OK);
    WotbModV3ManifestDependency dependencies[2] = {};
    dependencies[0].struct_size = sizeof(dependencies[0]);
    dependencies[0].api_version = WOTBMOD_V3_MANIFEST_VERSION;
    dependencies[0].kind = WOTBMOD_V3_DEPENDENCY_REQUIRED;
    Copy(
        dependencies[0].id,
        sizeof(dependencies[0].id),
        "tests.runtime-dependency");
    Copy(
        dependencies[0].version_range,
        sizeof(dependencies[0].version_range),
        "^1.0");
    dependencies[1].struct_size = sizeof(dependencies[1]);
    dependencies[1].api_version = WOTBMOD_V3_MANIFEST_VERSION;
    dependencies[1].kind = WOTBMOD_V3_DEPENDENCY_OPTIONAL;
    Copy(
        dependencies[1].id,
        sizeof(dependencies[1].id),
        "tests.optional-missing");
    Copy(
        dependencies[1].version_range,
        sizeof(dependencies[1].version_range),
        ">=1.0");
    CHECK(
        WotbModV3Runtime_SetDependencies(
            mod,
            nullptr,
            WOTBMOD_V3_RUNTIME_MAX_DEPENDENCIES + 1u) ==
        WOTBMOD_V3_E_LIMIT_REACHED);
    WotbModV3ManifestDependency invalid_dependency =
        dependencies[0];
    invalid_dependency.struct_size =
        sizeof(WotbModV3StructHeader);
    CHECK(
        WotbModV3Runtime_SetDependencies(
            mod,
            &invalid_dependency,
            1u) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(
        WotbModV3Runtime_SetDependencies(
            mod,
            dependencies,
            2u) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            mod, &TestEntry, &module) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_SetPackageRoot(mod, ".") ==
        WOTBMOD_V3_E_CONFLICT);
    CHECK(
        WotbModV3Runtime_SetDependencies(mod, nullptr, 0u) ==
        WOTBMOD_V3_E_CONFLICT);
    CHECK(WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK);
    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);

    const void* table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_LIFECYCLE,
            WOTBMOD_V3_LIFECYCLE_VERSION,
            &table) == WOTBMOD_V3_OK);
    const WotbModV3LifecycleApiV1* lifecycle =
        static_cast<const WotbModV3LifecycleApiV1*>(table);
    WotbModV3LifecycleInfo lifecycle_info = {};
    lifecycle_info.struct_size = sizeof(lifecycle_info);
    lifecycle_info.api_version = WOTBMOD_V3_LIFECYCLE_VERSION;
    CHECK(
        lifecycle->get_info(mod, &lifecycle_info) ==
        WOTBMOD_V3_OK);
    CHECK(lifecycle_info.state == WOTBMOD_V3_MOD_STATE_ENABLED);
    uint32_t install_size = 0u;
    CHECK(
        lifecycle->get_install_path(
            mod, nullptr, &install_size) ==
        WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(install_size > 1u);
    char tiny_install[1] = {};
    uint32_t tiny_install_size = sizeof(tiny_install);
    CHECK(
        lifecycle->get_install_path(
            mod,
            tiny_install,
            &tiny_install_size) ==
        WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(tiny_install_size == install_size);
    char install_path[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t install_capacity = sizeof(install_path);
    CHECK(
        lifecycle->get_install_path(
            mod,
            install_path,
            &install_capacity) == WOTBMOD_V3_OK);
    CHECK(install_capacity == install_size);
    CHECK(std::strstr(install_path, "\\..\\") == nullptr);
    const char install_suffix[] =
        "\\build\\v3_runtime_test\\package\\bin";
    CHECK(
        std::strlen(install_path) >=
        std::strlen(install_suffix));
    CHECK(
        _stricmp(
            install_path + std::strlen(install_path) -
                std::strlen(install_suffix),
            install_suffix) == 0);

    uint32_t resource_size = 0u;
    CHECK(
        lifecycle->get_resource_path(
            mod, nullptr, &resource_size) ==
        WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    char resource_path[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t resource_capacity = sizeof(resource_path);
    CHECK(
        lifecycle->get_resource_path(
            mod,
            resource_path,
            &resource_capacity) == WOTBMOD_V3_OK);
    CHECK(resource_capacity == resource_size);
    const char resource_suffix[] =
        "\\build\\v3_runtime_test";
    CHECK(
        std::strlen(resource_path) >=
        std::strlen(resource_suffix));
    CHECK(
        _stricmp(
            resource_path + std::strlen(resource_path) -
                std::strlen(resource_suffix),
            resource_suffix) == 0);
    CHECK(_stricmp(install_path, resource_path) != 0);
    char loose_install[WOTBMOD_V3_MAX_PATH] = {};
    char loose_resource[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t loose_install_size = sizeof(loose_install);
    uint32_t loose_resource_size = sizeof(loose_resource);
    CHECK(
        lifecycle->get_install_path(
            dependency_mod,
            loose_install,
            &loose_install_size) == WOTBMOD_V3_OK);
    CHECK(
        lifecycle->get_resource_path(
            dependency_mod,
            loose_resource,
            &loose_resource_size) == WOTBMOD_V3_OK);
    CHECK(_stricmp(loose_install, loose_resource) == 0);
    CHECK(lifecycle->request_enable(mod, mod) == WOTBMOD_V3_OK);
    uint32_t can_hot_reload = 1u;
    char hot_reload_reason[WOTBMOD_V3_MAX_MESSAGE] = {};
    uint32_t hot_reload_reason_size = sizeof(hot_reload_reason);
    CHECK(
        lifecycle->can_hot_reload(
            mod,
            mod,
            &can_hot_reload,
            hot_reload_reason,
            &hot_reload_reason_size) == WOTBMOD_V3_OK);
    CHECK(can_hot_reload == 0u);
    CHECK(hot_reload_reason[0] != '\0');
    WotbModV3Token cleanup_token = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        lifecycle->register_cleanup(
            mod,
            &CleanupCallback,
            nullptr,
            &cleanup_token) == WOTBMOD_V3_OK);

    table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_EVENTS,
            WOTBMOD_V3_EVENTS_VERSION,
            &table) == WOTBMOD_V3_OK);
    g_events = static_cast<const WotbModV3EventsApiV1*>(table);
    CHECK(g_events != nullptr);

    WotbModV3EventSubscriptionInfo event_availability = {};
    event_availability.struct_size = sizeof(event_availability);
    event_availability.api_version = WOTBMOD_V3_EVENTS_VERSION;
    event_availability.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    event_availability.receive_system_events = 1u;
    event_availability.topic_pattern = WOTBMOD_V3_EVENT_FOV_CHANGED;
    WotbModV3EventToken unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    event_availability.topic_pattern =
        "wotbmod.experimental.no_publisher";
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    event_availability.topic_pattern = "wotbmod.replay.*";
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    event_availability.topic_pattern = WOTBMOD_V3_EVENT_FRAME_UPDATE;
    WotbModV3EventToken available_event =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern =
        WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern = "wotbmod.render.*";
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern = WOTBMOD_V3_EVENT_SHOT_FIRED;
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    event_availability.topic_pattern = "wotbmod.gameplay.*";
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3Runtime_SetEventSourceMask(
        WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING |
        WOTBMOD_V3_EVENT_SOURCE_UI_INPUT);
    event_availability.topic_pattern = WOTBMOD_V3_EVENT_SHOT_FIRED;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern = WOTBMOD_V3_EVENT_UI_INPUT;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern = WOTBMOD_V3_EVENT_SHELL_HIT;
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    event_availability.topic_pattern = "wotbmod.gameplay.*";
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    // THE PROJECTILE V2 TOPICS, WHICH THIS TABLE ONCE OMITTED ENTIRELY.
    //
    // A topic missing from SystemEventTopics() cannot be subscribed to at all,
    // whatever the source mask says - and all four `wotbmod.projectile.*`
    // topics were missing, so the interface published into a room nobody was
    // allowed to enter. Live on 11.19.0.834 the loader did its half correctly
    // (`PublishNewProjectile -> 0`) and 110k traced events still held zero
    // projectile events. `created` rides the showShooting hook, which is armed
    // here; `impacted` rides the vehicle-hit hook, which is not.
    event_availability.topic_pattern =
        WOTBMOD_V3_EVENT_PROJECTILE_CREATED;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern = "wotbmod.gameplay.projectile.*";
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern =
        WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED;
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3Runtime_SetEventSourceMask(
        WOTBMOD_V3_EVENT_SOURCE_ALL);

    event_availability.topic_pattern =
        WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);
    event_availability.topic_pattern = WOTBMOD_V3_EVENT_SHELL_HIT;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern =
        WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern =
        WOTBMOD_V3_EVENT_SCENE_ACTIVATED;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern =
        WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED;
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    WotbModV3Runtime_SetEventSourceMask(0u);
    event_availability.topic_pattern = WOTBMOD_V3_EVENT_SHOT_FIRED;
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    event_availability.topic_pattern = "wotbmod.*";
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern = WOTBMOD_V3_EVENT_FRAME_UPDATE;
    event_availability.receive_system_events = 0u;
    unavailable_event =
        static_cast<WotbModV3EventToken>(0xBADC0FFEu);
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &unavailable_event) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(unavailable_event == WOTBMOD_V3_INVALID_HANDLE);

    event_availability.topic_pattern =
        "mod.tests.runtime-services.*";
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    event_availability.topic_pattern = "*";
    available_event = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &LowEvent,
            nullptr,
            &available_event) == WOTBMOD_V3_OK);
    CHECK(available_event != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(
        g_events->unsubscribe(mod, available_event) ==
        WOTBMOD_V3_OK);

    // capabilities.changed must be subscribable AND actually fire. Both
    // halves matter: a subscribable topic that never fires is
    // indistinguishable from a broken runtime.
    g_capability_event_count = 0u;
    g_capability_event_payload = WotbModV3CapabilityInfo{};
    event_availability.topic_pattern = WOTBMOD_V3_EVENT_CAPABILITIES_CHANGED;
    event_availability.receive_system_events = 1u;
    WotbModV3EventToken capability_event_token = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &event_availability,
            &CapabilityChangedEvent,
            nullptr,
            &capability_event_token) == WOTBMOD_V3_OK);
    CHECK(capability_event_token != WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3CapabilityInfo announced_capability = {};
    WOTBMOD_V3_INIT_STRUCT(
        announced_capability, WOTBMOD_V3_CAPABILITIES_VERSION);
    Copy(
        announced_capability.name,
        sizeof(announced_capability.name),
        "test.capability.event");
    announced_capability.status = WOTBMOD_V3_CAPABILITY_AVAILABLE;
    announced_capability.permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    CHECK(
        WotbModV3Runtime_SetCapability(&announced_capability) ==
        WOTBMOD_V3_OK);

    CHECK(g_capability_event_count.load() == 1u);
    CHECK(
        std::strcmp(
            g_capability_event_payload.name, "test.capability.event") == 0);
    CHECK(
        g_capability_event_payload.status ==
        WOTBMOD_V3_CAPABILITY_AVAILABLE);

    CHECK(
        g_events->unsubscribe(mod, capability_event_token) ==
        WOTBMOD_V3_OK);

    table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_ASYNC,
            WOTBMOD_V3_ASYNC_VERSION,
            &table) == WOTBMOD_V3_OK);
    g_async = static_cast<const WotbModV3AsyncApiV1*>(table);
    CHECK(g_async != nullptr);
    WotbModV3TaskHandle main_dispatch_task =
        static_cast<WotbModV3TaskHandle>(123u);
    CHECK(
        g_async->dispatch_to_main_thread(
            mod,
            &MainDispatchCallback,
            nullptr,
            &main_dispatch_task) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(main_dispatch_task == WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3TaskHandle audio_dispatch_task =
        static_cast<WotbModV3TaskHandle>(456u);
    CHECK(
        g_async->dispatch_to_audio_thread(
            mod,
            &MainDispatchCallback,
            nullptr,
            &audio_dispatch_task) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(audio_dispatch_task == WOTBMOD_V3_INVALID_HANDLE);
    uint32_t pumped = 0u;
    CHECK(
        g_async->pump_current_thread(mod, 8u, &pumped) ==
        WOTBMOD_V3_OK);
    CHECK(pumped == 0u);
    CHECK(g_main_dispatch_count.load() == 0u);

    // MAIN dispatch must refuse while the ingress is offline and start
    // working the moment it is declared online, without a rebuild. The
    // offline half is the one that matters: it keeps a mismatched client
    // from queueing work that nothing would ever run.
    using wotbmod::v3::PumpMainThread;
    using wotbmod::v3::PostMainThread;
    using wotbmod::v3::PostMainThreadEx;
    using wotbmod::v3::SetMainIngressOnline;
    using wotbmod::v3::InvokeMainThreadBlocking;

    SetMainIngressOnline(false);
    const uint32_t role_before_blocking_tests =
        wotbmod::v3::SetCurrentThreadRole(WOTBMOD_V3_THREAD_RENDER);
    BlockingMainCallTestState offline_blocking_call;
    CHECK(
        InvokeMainThreadBlocking(
            &BlockingMainCallCallback,
            &offline_blocking_call,
            100u) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(offline_blocking_call.calls.load() == 0u);
    BlockingMainCallTestState offline_posted_call;
    CHECK(
        PostMainThread(
            &BlockingMainCallCallback,
            &offline_posted_call) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(offline_posted_call.calls.load() == 0u);

    BlockingMainCallTestState direct_blocking_call;
    direct_blocking_call.callback_result = WOTBMOD_V3_E_BUSY;
    const uint32_t role_before_direct =
        wotbmod::v3::SetCurrentThreadRole(WOTBMOD_V3_THREAD_MAIN);
    CHECK(
        InvokeMainThreadBlocking(
            &BlockingMainCallCallback,
            &direct_blocking_call,
            100u) == WOTBMOD_V3_E_BUSY);
    wotbmod::v3::SetCurrentThreadRole(role_before_direct);
    CHECK(direct_blocking_call.calls.load() == 1u);
    CHECK(
        direct_blocking_call.observed_role.load() ==
        WOTBMOD_V3_THREAD_MAIN);

    WotbModV3TaskHandle main_ingress_task = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_async->dispatch_to_main_thread(
            mod,
            &MainIngressDispatchCallback,
            nullptr,
            &main_ingress_task) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(main_ingress_task == WOTBMOD_V3_INVALID_HANDLE);
    CHECK(PumpMainThread(16u) == 0u);
    CHECK(g_main_ingress_dispatch_count.load() == 0u);

    SetMainIngressOnline(true);
    BlockingMainCallTestState posted_main_call;
    posted_main_call.callback_result = WOTBMOD_V3_E_BUSY;
    CHECK(
        PostMainThread(
            &BlockingMainCallCallback,
            &posted_main_call) == WOTBMOD_V3_OK);
    CHECK(posted_main_call.calls.load() == 0u);
    CHECK(PumpMainThread(16u) == 1u);
    CHECK(posted_main_call.calls.load() == 1u);
    CHECK(posted_main_call.cancellations.load() == 0u);
    CHECK(
        posted_main_call.observed_role.load() ==
        WOTBMOD_V3_THREAD_MAIN);

    BlockingMainCallTestState queued_blocking_call;
    queued_blocking_call.callback_result = WOTBMOD_V3_E_BUSY;
    std::atomic<int32_t> queued_blocking_result{WOTBMOD_V3_E_PLATFORM};
    std::thread blocking_worker([&]() {
        queued_blocking_result.store(
            InvokeMainThreadBlocking(
                &BlockingMainCallCallback,
                &queued_blocking_call,
                1000u));
    });
    const auto blocking_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (queued_blocking_call.calls.load() == 0u &&
           std::chrono::steady_clock::now() < blocking_deadline) {
        (void)PumpMainThread(16u);
        std::this_thread::yield();
    }
    blocking_worker.join();
    CHECK(queued_blocking_result.load() == WOTBMOD_V3_E_BUSY);
    CHECK(queued_blocking_call.calls.load() == 1u);
    CHECK(
        queued_blocking_call.observed_role.load() ==
        WOTBMOD_V3_THREAD_MAIN);

    BlockingMainCallTestState chained_blocking_call;
    std::atomic<int32_t> chained_first_result{WOTBMOD_V3_E_PLATFORM};
    std::atomic<int32_t> chained_second_result{WOTBMOD_V3_E_PLATFORM};
    std::atomic<uint32_t> chained_worker_ready{0u};
    std::thread chained_worker([&]() {
        chained_worker_ready.store(1u);
        chained_first_result.store(
            InvokeMainThreadBlocking(
                &BlockingMainCallCallback,
                &chained_blocking_call,
                1000u));
        chained_second_result.store(
            InvokeMainThreadBlocking(
                &BlockingMainCallCallback,
                &chained_blocking_call,
                1000u));
    });
    while (chained_worker_ready.load() == 0u) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const uint32_t chained_first_pump = PumpMainThread(16u);
    if (chained_first_pump < 2u) {
        /* Avoid hanging the test process if the one-pump contract regresses. */
        (void)PumpMainThread(16u);
    }
    chained_worker.join();
    CHECK(chained_first_pump == 2u);
    CHECK(chained_blocking_call.calls.load() == 2u);
    CHECK(chained_first_result.load() == WOTBMOD_V3_OK);
    CHECK(chained_second_result.load() == WOTBMOD_V3_OK);

    BlockingMainCallTestState timed_out_blocking_call;
    const uint32_t role_before_timeout =
        wotbmod::v3::SetCurrentThreadRole(WOTBMOD_V3_THREAD_RENDER);
    CHECK(
        InvokeMainThreadBlocking(
            &BlockingMainCallCallback,
            &timed_out_blocking_call,
            10u) == WOTBMOD_V3_E_TIMEOUT);
    wotbmod::v3::SetCurrentThreadRole(role_before_timeout);
    CHECK(timed_out_blocking_call.calls.load() == 0u);
    CHECK(PumpMainThread(16u) == 1u);
    CHECK(timed_out_blocking_call.calls.load() == 0u);
    wotbmod::v3::SetCurrentThreadRole(role_before_blocking_tests);

    CHECK(
        g_async->dispatch_to_main_thread(
            mod,
            &MainIngressDispatchCallback,
            nullptr,
            &main_ingress_task) == WOTBMOD_V3_OK);
    CHECK(g_main_ingress_dispatch_count.load() == 0u);  // queued, not run
    // The pump must hand the thread back with the role it borrowed it with.
    // Its eventual host is a game callback: if it leaked MAIN while running
    // inside DispatchFrame's window, the rest of that frame would be labelled
    // MAIN, and a mod calling pump_current_thread from its frame callback
    // would drain the MAIN queue instead of RENDER. RENDER is set here
    // because it is the role that window actually uses.
    const uint32_t role_before_pump =
        wotbmod::v3::SetCurrentThreadRole(WOTBMOD_V3_THREAD_RENDER);
    CHECK(PumpMainThread(16u) == 1u);
    CHECK(
        wotbmod::v3::CurrentThreadRole() == WOTBMOD_V3_THREAD_RENDER);
    wotbmod::v3::SetCurrentThreadRole(role_before_pump);
    CHECK(g_main_ingress_dispatch_count.load() == 1u);
    CHECK(PumpMainThread(16u) == 0u);  // drained

    // Reentrancy: pumping from inside a pumped callback must not recurse.
    // Two dispatches are queued before the outer pump so that the nested
    // pump has real work available to steal. A single queued dispatch
    // would prove nothing: PumpRole pops it under lock before invoking it,
    // so the queue would be empty by the time the nested pump ran and it
    // would return 0 whether or not the guard existed.
    WotbModV3TaskHandle reentrant_ingress_task =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_async->dispatch_to_main_thread(
            mod,
            &MainIngressReentrantDispatchCallback,
            nullptr,
            &reentrant_ingress_task) == WOTBMOD_V3_OK);
    WotbModV3TaskHandle follower_ingress_task =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_async->dispatch_to_main_thread(
            mod,
            &MainIngressFollowerDispatchCallback,
            nullptr,
            &follower_ingress_task) == WOTBMOD_V3_OK);
    CHECK(g_main_ingress_reentrant_count.load() == 0u);
    CHECK(g_main_ingress_follower_count.load() == 0u);

    // The outer pump must run both, one after the other. Without the
    // guard it returns 1, because the nested pump consumes the follower
    // and the outer loop then finds an empty queue.
    CHECK(PumpMainThread(16u) == 2u);
    CHECK(g_main_ingress_reentrant_count.load() == 1u);
    CHECK(g_main_ingress_follower_count.load() == 1u);
    // The nested pump ran nothing.
    CHECK(g_main_ingress_reentrant_pump_result.load() == 0u);
    // The follower did not run inside the first callback.
    CHECK(g_main_ingress_follower_ran_nested.load() == 0u);
    // Ordering: the first callback returned before the follower started.
    CHECK(g_main_ingress_reentrant_exit_seq.load() == 1u);
    CHECK(g_main_ingress_follower_seq.load() == 2u);
    CHECK(PumpMainThread(16u) == 0u);  // drained

    SetMainIngressOnline(false);

    WotbModV3EventSubscriptionInfo high = {};
    high.struct_size = sizeof(high);
    high.api_version = WOTBMOD_V3_EVENTS_VERSION;
    high.topic_pattern = "mod.tests.runtime-services.test";
    high.priority = WOTBMOD_V3_EVENT_PRIORITY_HIGH;
    WotbModV3EventToken high_token = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod, &high, &HighEvent, nullptr, &high_token) ==
        WOTBMOD_V3_OK);
    WotbModV3EventSubscriptionInfo low = high;
    low.priority = WOTBMOD_V3_EVENT_PRIORITY_LOW;
    WotbModV3EventToken low_token = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod, &low, &LowEvent, nullptr, &low_token) ==
        WOTBMOD_V3_OK);
    const uint32_t payload = 42u;
    CHECK(
        g_events->post(
            mod,
            high.topic_pattern,
            &payload,
            sizeof(payload),
            WOTBMOD_V3_EVENT_FLAG_STOPPABLE) == WOTBMOD_V3_OK);
    CHECK(g_high_event_count.load() == 1u);
    CHECK(g_low_event_count.load() == 0u);
    CHECK(g_event_info_count.load() == 1u);

    WotbModV3EventSubscriptionInfo blocking_info = {};
    blocking_info.struct_size = sizeof(blocking_info);
    blocking_info.api_version = WOTBMOD_V3_EVENTS_VERSION;
    blocking_info.topic_pattern =
        "mod.tests.runtime-services.quiescence";
    blocking_info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    WotbModV3EventToken blocking_token =
        WOTBMOD_V3_INVALID_HANDLE;
    BlockingEventState blocking_state;
    CHECK(
        g_events->subscribe(
            mod,
            &blocking_info,
            &BlockingEvent,
            &blocking_state,
            &blocking_token) == WOTBMOD_V3_OK);
    WotbModV3Result blocking_post_result =
        WOTBMOD_V3_E_PLATFORM;
    std::thread blocking_poster([&]() {
        blocking_post_result = g_events->post(
            mod,
            blocking_info.topic_pattern,
            nullptr,
            0u,
            0u);
    });
    bool blocking_entered = false;
    {
        std::unique_lock<std::mutex> lock(blocking_state.mutex);
        blocking_entered = blocking_state.cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [&]() { return blocking_state.entered; });
    }
    std::atomic<bool> unsubscribe_started{false};
    std::atomic<bool> unsubscribe_returned{false};
    WotbModV3Result blocking_unsubscribe_result =
        WOTBMOD_V3_E_PLATFORM;
    std::thread blocking_unsubscriber;
    bool unsubscribe_returned_before_release = false;
    std::atomic<bool> duplicate_unsubscribe_returned{false};
    WotbModV3Result duplicate_unsubscribe_result =
        WOTBMOD_V3_E_PLATFORM;
    std::thread duplicate_unsubscriber;
    WotbModV3Result priority_during_unsubscribe =
        WOTBMOD_V3_E_PLATFORM;
    if (blocking_entered) {
        blocking_unsubscriber = std::thread([&]() {
            unsubscribe_started.store(true);
            blocking_unsubscribe_result =
                g_events->unsubscribe(mod, blocking_token);
            unsubscribe_returned.store(true);
        });
        const std::chrono::steady_clock::time_point start_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (!unsubscribe_started.load() &&
               std::chrono::steady_clock::now() < start_deadline) {
            std::this_thread::yield();
        }
        const std::chrono::steady_clock::time_point claim_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        do {
            priority_during_unsubscribe =
                g_events->set_priority(
                    mod,
                    blocking_token,
                    WOTBMOD_V3_EVENT_PRIORITY_HIGH);
            if (priority_during_unsubscribe != WOTBMOD_V3_OK) {
                break;
            }
            std::this_thread::yield();
        } while (
            std::chrono::steady_clock::now() < claim_deadline);
        duplicate_unsubscriber = std::thread([&]() {
            duplicate_unsubscribe_result =
                g_events->unsubscribe(mod, blocking_token);
            duplicate_unsubscribe_returned.store(true);
        });
        const std::chrono::steady_clock::time_point duplicate_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (!duplicate_unsubscribe_returned.load() &&
               std::chrono::steady_clock::now() <
                   duplicate_deadline) {
            std::this_thread::yield();
        }
        unsubscribe_returned_before_release =
            unsubscribe_returned.load();
    }
    {
        std::lock_guard<std::mutex> lock(blocking_state.mutex);
        blocking_state.release = true;
    }
    blocking_state.cv.notify_all();
    if (blocking_unsubscriber.joinable()) {
        blocking_unsubscriber.join();
    }
    if (duplicate_unsubscriber.joinable()) {
        duplicate_unsubscriber.join();
    }
    blocking_poster.join();
    CHECK(blocking_entered);
    CHECK(unsubscribe_started.load());
    CHECK(!unsubscribe_returned_before_release);
    CHECK(unsubscribe_returned.load());
    CHECK(blocking_unsubscribe_result == WOTBMOD_V3_OK);
    CHECK(duplicate_unsubscribe_returned.load());
    CHECK(
        duplicate_unsubscribe_result ==
            WOTBMOD_V3_E_OBJECT_DESTROYED ||
        duplicate_unsubscribe_result ==
            WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        priority_during_unsubscribe ==
            WOTBMOD_V3_E_OBJECT_DESTROYED ||
        priority_during_unsubscribe ==
            WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(blocking_post_result == WOTBMOD_V3_OK);
    CHECK(blocking_state.calls.load() == 1u);
    blocking_state.alive.store(false);
    CHECK(
        g_events->post(
            mod,
            blocking_info.topic_pattern,
            nullptr,
            0u,
            0u) == WOTBMOD_V3_OK);
    CHECK(blocking_state.calls.load() == 1u);
    CHECK(!blocking_state.callback_after_unsubscribe.load());

    WotbModV3EventSubscriptionInfo self_info = {};
    self_info.struct_size = sizeof(self_info);
    self_info.api_version = WOTBMOD_V3_EVENTS_VERSION;
    self_info.topic_pattern =
        "mod.tests.runtime-services.self-unsubscribe";
    self_info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    SelfUnsubscribeEventState self_state;
    self_state.mod = mod;
    CHECK(
        g_events->subscribe(
            mod,
            &self_info,
            &SelfUnsubscribeEvent,
            &self_state,
            &self_state.token) == WOTBMOD_V3_OK);
    std::atomic<bool> self_post_done{false};
    WotbModV3Result self_post_result = WOTBMOD_V3_E_PLATFORM;
    std::thread self_poster([&]() {
        self_post_result = g_events->post(
            mod,
            self_info.topic_pattern,
            nullptr,
            0u,
            0u);
        self_post_done.store(true);
    });
    const std::chrono::steady_clock::time_point self_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!self_post_done.load() &&
           std::chrono::steady_clock::now() < self_deadline) {
        std::this_thread::yield();
    }
    if (!self_post_done.load()) {
        std::fputs(
            "self-unsubscribe event callback deadlocked\n",
            stderr);
        std::abort();
    }
    self_poster.join();
    CHECK(self_post_result == WOTBMOD_V3_OK);
    CHECK(self_state.result.load() == WOTBMOD_V3_OK);
    CHECK(self_state.calls.load() == 1u);
    CHECK(
        g_events->post(
            mod,
            self_info.topic_pattern,
            nullptr,
            0u,
            0u) == WOTBMOD_V3_OK);
    CHECK(self_state.calls.load() == 1u);

    WotbModV3EventSubscriptionInfo concurrent_self_info = {};
    concurrent_self_info.struct_size = sizeof(concurrent_self_info);
    concurrent_self_info.api_version = WOTBMOD_V3_EVENTS_VERSION;
    concurrent_self_info.topic_pattern =
        "mod.tests.runtime-services.self-unsubscribe-foreign";
    concurrent_self_info.priority =
        WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    ConcurrentSelfUnsubscribeEventState concurrent_self_state;
    concurrent_self_state.mod = mod;
    CHECK(
        g_events->subscribe(
            mod,
            &concurrent_self_info,
            &ConcurrentSelfUnsubscribeEvent,
            &concurrent_self_state,
            &concurrent_self_state.token) == WOTBMOD_V3_OK);
    WotbModV3Result first_concurrent_post_result =
        WOTBMOD_V3_E_PLATFORM;
    std::thread first_concurrent_poster([&]() {
        first_concurrent_post_result = g_events->post(
            mod,
            concurrent_self_info.topic_pattern,
            nullptr,
            0u,
            0u);
    });
    bool first_concurrent_entered = false;
    {
        std::unique_lock<std::mutex> lock(
            concurrent_self_state.mutex);
        first_concurrent_entered =
            concurrent_self_state.cv.wait_for(
                lock,
                std::chrono::seconds(2),
                [&]() {
                    return concurrent_self_state.first_entered;
                });
    }
    WotbModV3Result second_concurrent_post_result =
        WOTBMOD_V3_E_PLATFORM;
    std::atomic<bool> second_concurrent_post_done{false};
    std::thread second_concurrent_poster;
    bool concurrent_self_returned_before_release = false;
    if (first_concurrent_entered) {
        second_concurrent_poster = std::thread([&]() {
            second_concurrent_post_result = g_events->post(
                mod,
                concurrent_self_info.topic_pattern,
                nullptr,
                0u,
                0u);
            second_concurrent_post_done.store(true);
        });
        const std::chrono::steady_clock::time_point start_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (!concurrent_self_state.self_unsubscribe_started.load() &&
               std::chrono::steady_clock::now() < start_deadline) {
            std::this_thread::yield();
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
        concurrent_self_returned_before_release =
            concurrent_self_state.self_unsubscribe_returned.load();
    }
    {
        std::lock_guard<std::mutex> lock(
            concurrent_self_state.mutex);
        concurrent_self_state.release_first = true;
    }
    concurrent_self_state.cv.notify_all();
    first_concurrent_poster.join();
    const std::chrono::steady_clock::time_point finish_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (second_concurrent_poster.joinable() &&
           !second_concurrent_post_done.load() &&
           std::chrono::steady_clock::now() < finish_deadline) {
        std::this_thread::yield();
    }
    if (second_concurrent_poster.joinable() &&
        !second_concurrent_post_done.load()) {
        std::fputs(
            "self-unsubscribe did not quiesce foreign callbacks\n",
            stderr);
        std::abort();
    }
    if (second_concurrent_poster.joinable()) {
        second_concurrent_poster.join();
    }
    CHECK(first_concurrent_entered);
    CHECK(
        concurrent_self_state.self_unsubscribe_started.load());
    CHECK(!concurrent_self_returned_before_release);
    CHECK(
        concurrent_self_state.self_unsubscribe_returned.load());
    CHECK(concurrent_self_state.result.load() == WOTBMOD_V3_OK);
    CHECK(first_concurrent_post_result == WOTBMOD_V3_OK);
    CHECK(second_concurrent_post_result == WOTBMOD_V3_OK);
    CHECK(concurrent_self_state.calls.load() == 2u);
    CHECK(
        g_events->post(
            mod,
            concurrent_self_info.topic_pattern,
            nullptr,
            0u,
        0u) == WOTBMOD_V3_OK);
    CHECK(concurrent_self_state.calls.load() == 2u);

    WotbModV3EventSubscriptionInfo dual_self_info = {};
    dual_self_info.struct_size = sizeof(dual_self_info);
    dual_self_info.api_version = WOTBMOD_V3_EVENTS_VERSION;
    dual_self_info.topic_pattern =
        "mod.tests.runtime-services.dual-self-unsubscribe";
    dual_self_info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    DualSelfUnsubscribeEventState dual_self_state;
    dual_self_state.mod = mod;
    CHECK(
        g_events->subscribe(
            mod,
            &dual_self_info,
            &DualSelfUnsubscribeEvent,
            &dual_self_state,
            &dual_self_state.token) == WOTBMOD_V3_OK);
    WotbModV3Result dual_post_results[2] = {
        WOTBMOD_V3_E_PLATFORM,
        WOTBMOD_V3_E_PLATFORM};
    std::thread dual_posters[2] = {
        std::thread([&]() {
            dual_post_results[0] = g_events->post(
                mod,
                dual_self_info.topic_pattern,
                nullptr,
                0u,
                0u);
        }),
        std::thread([&]() {
            dual_post_results[1] = g_events->post(
                mod,
                dual_self_info.topic_pattern,
                nullptr,
                0u,
                0u);
        })};
    const std::chrono::steady_clock::time_point dual_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (dual_self_state.returned.load() != 2u &&
           std::chrono::steady_clock::now() < dual_deadline) {
        std::this_thread::yield();
    }
    if (dual_self_state.returned.load() != 2u) {
        std::fputs(
            "concurrent self-unsubscribe callbacks deadlocked\n",
            stderr);
        std::abort();
    }
    dual_posters[0].join();
    dual_posters[1].join();
    CHECK(dual_post_results[0] == WOTBMOD_V3_OK);
    CHECK(dual_post_results[1] == WOTBMOD_V3_OK);
    const int32_t dual_result_0 =
        dual_self_state.results[0].load();
    const int32_t dual_result_1 =
        dual_self_state.results[1].load();
    CHECK(
        (dual_result_0 == WOTBMOD_V3_OK &&
         (dual_result_1 == WOTBMOD_V3_E_OBJECT_DESTROYED ||
          dual_result_1 == WOTBMOD_V3_E_INVALID_HANDLE)) ||
        (dual_result_1 == WOTBMOD_V3_OK &&
         (dual_result_0 == WOTBMOD_V3_E_OBJECT_DESTROYED ||
          dual_result_0 == WOTBMOD_V3_E_INVALID_HANDLE)));
    CHECK(
        g_events->post(
            mod,
            dual_self_info.topic_pattern,
            nullptr,
            0u,
            0u) == WOTBMOD_V3_OK);
    CHECK(dual_self_state.returned.load() == 2u);

    WotbModV3TaskSubmitInfo submit = {};
    submit.struct_size = sizeof(submit);
    submit.api_version = WOTBMOD_V3_ASYNC_VERSION;
    submit.description = "worker test";
    submit.work = &TaskWork;
    submit.completion = &TaskCompletion;
    submit.completion_thread_role = WOTBMOD_V3_THREAD_RENDER;
    WotbModV3TaskSubmitInfo unsupported_submit = submit;
    unsupported_submit.completion_thread_role =
        WOTBMOD_V3_THREAD_MAIN;
    WotbModV3TaskHandle unsupported_completion_task =
        static_cast<WotbModV3TaskHandle>(789u);
    CHECK(
        g_async->task_submit(
            mod,
            &unsupported_submit,
            &unsupported_completion_task) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        unsupported_completion_task ==
        WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3TaskHandle worker_task = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_async->task_submit(mod, &submit, &worker_task) ==
        WOTBMOD_V3_OK);
    WotbModV3TaskInfo task_info = {};
    task_info.struct_size = sizeof(task_info);
    task_info.api_version = WOTBMOD_V3_ASYNC_VERSION;
    for (uint32_t attempt = 0u; attempt < 200u; ++attempt) {
        CHECK(
            g_async->task_get_info(
                mod, worker_task, &task_info) == WOTBMOD_V3_OK);
        if (task_info.state == WOTBMOD_V3_TASK_COMPLETED) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(task_info.state == WOTBMOD_V3_TASK_COMPLETED);
    uint32_t direct_task_state = WOTBMOD_V3_TASK_QUEUED;
    float direct_task_progress = 0.0f;
    CHECK(
        g_async->task_get_state(
            mod, worker_task, &direct_task_state) == WOTBMOD_V3_OK);
    CHECK(
        g_async->task_get_progress(
            mod, worker_task, &direct_task_progress) ==
        WOTBMOD_V3_OK);
    CHECK(direct_task_state == WOTBMOD_V3_TASK_COMPLETED);
    CHECK(direct_task_progress == 1.0f);
    char result_data[32] = {};
    WotbModV3Buffer result_buffer = {};
    result_buffer.struct_size = sizeof(result_buffer);
    result_buffer.api_version = WOTBMOD_V3_ASYNC_VERSION;
    result_buffer.data = result_data;
    result_buffer.capacity = sizeof(result_data);
    CHECK(
        g_async->task_get_result(
            mod, worker_task, &result_buffer) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(result_data, "worker-result") == 0);

    WotbModV3TaskHandle dispatch_task = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_async->dispatch_to_render_thread(
            mod, &DispatchCallback, nullptr, &dispatch_task) ==
        WOTBMOD_V3_OK);
    WotbModV3TimerCreateInfo timer_info = {};
    timer_info.struct_size = sizeof(timer_info);
    timer_info.api_version = WOTBMOD_V3_ASYNC_VERSION;
    timer_info.delay_ms = 1u;
    timer_info.callback_thread_role = WOTBMOD_V3_THREAD_RENDER;
    timer_info.callback = &TimerCallback;
    WotbModV3TimerCreateInfo unsupported_timer = timer_info;
    unsupported_timer.callback_thread_role =
        WOTBMOD_V3_THREAD_AUDIO;
    WotbModV3TimerHandle unsupported_timer_handle =
        static_cast<WotbModV3TimerHandle>(987u);
    CHECK(
        g_async->timer_create(
            mod,
            &unsupported_timer,
            &unsupported_timer_handle) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        unsupported_timer_handle ==
        WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3TimerHandle timer_handle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_async->timer_create(mod, &timer_info, &timer_handle) ==
        WOTBMOD_V3_OK);
    for (uint32_t attempt = 0u;
         attempt < 200u && g_timer_count.load() == 0u;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        WotbModV3Runtime_DispatchFrame(attempt + 1u, 0.001);
    }
    CHECK(g_dispatch_count.load() == 1u);
    CHECK(g_task_completion_count.load() == 1u);
    CHECK(g_timer_count.load() == 1u);

    table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_INTERMOD,
            WOTBMOD_V3_INTERMOD_VERSION,
            &table) == WOTBMOD_V3_OK);
    const WotbModV3IntermodApiV1* intermod =
        static_cast<const WotbModV3IntermodApiV1*>(table);
    ExportedTable exported = {
        sizeof(ExportedTable), 1u, &ExportedFunction};
    WotbModV3Token export_token = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        intermod->export_interface(
            mod,
            "tests.runtime-services/increment",
            1u,
            &exported,
            sizeof(exported),
            &export_token) == WOTBMOD_V3_OK);
    WotbModV3ImportedInterface imported = {};
    imported.struct_size = sizeof(imported);
    imported.api_version = WOTBMOD_V3_INTERMOD_VERSION;
    CHECK(
        intermod->import_interface(
            mod,
            "tests.runtime-services/increment",
            1u,
            &imported) == WOTBMOD_V3_OK);
    const ExportedTable* imported_table =
        static_cast<const ExportedTable*>(imported.table);
    CHECK(imported_table && imported_table->increment(4) == 5);
    WotbModV3Handle found_mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        intermod->mod_find(
            mod, "tests.runtime-services", &found_mod) ==
        WOTBMOD_V3_OK);
    CHECK(found_mod == mod);
    char found_version[WOTBMOD_V3_MAX_VERSION] = {};
    uint32_t found_version_size = sizeof(found_version);
    CHECK(
        intermod->mod_get_version(
            mod,
            found_mod,
            found_version,
            &found_version_size) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(found_version, "1.0.0") == 0);
    uint32_t dependency_optional = 99u;
    CHECK(
        intermod->mod_get_dependency(
            mod,
            "tests.runtime-dependency",
            &found_mod,
            &dependency_optional) == WOTBMOD_V3_OK);
    CHECK(found_mod == dependency_mod);
    CHECK(dependency_optional == 0u);
    found_mod = static_cast<WotbModV3Handle>(123u);
    dependency_optional = 0u;
    CHECK(
        intermod->mod_get_dependency(
            mod,
            "tests.optional-missing",
            &found_mod,
            &dependency_optional) == WOTBMOD_V3_E_NOT_FOUND);
    CHECK(found_mod == WOTBMOD_V3_INVALID_HANDLE);
    CHECK(dependency_optional == 1u);
    found_mod = static_cast<WotbModV3Handle>(123u);
    dependency_optional = 99u;
    CHECK(
        intermod->mod_get_dependency(
            mod,
            "tests.runtime-services",
            &found_mod,
            &dependency_optional) == WOTBMOD_V3_E_NOT_FOUND);
    CHECK(found_mod == WOTBMOD_V3_INVALID_HANDLE);
    CHECK(dependency_optional == 0u);
    WotbModV3Token message_token = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        intermod->message_subscribe(
            mod,
            "mod.tests.runtime-services.*",
            0,
            &MessageCallback,
            nullptr,
            &message_token) == WOTBMOD_V3_OK);
    CHECK(
        intermod->message_publish(
            mod,
            "mod.tests.runtime-services.hello",
            &payload,
            sizeof(payload)) == WOTBMOD_V3_OK);
    CHECK(g_message_count.load() == 1u);

    table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_HOOKS,
            WOTBMOD_V3_HOOKS_VERSION,
            &table) == WOTBMOD_V3_OK);
    const WotbModV3HooksApiV1* hooks =
        static_cast<const WotbModV3HooksApiV1*>(table);
    WotbModV3HookCreateInfo hook_info = {};
    hook_info.struct_size = sizeof(hook_info);
    hook_info.api_version = WOTBMOD_V3_HOOKS_VERSION;
    hook_info.mode = WOTBMOD_V3_HOOK_OBSERVE;
    hook_info.flags = WOTBMOD_V3_HOOK_CREATE_ALLOW_PENDING;
    hook_info.detour = &g_pending_detour_marker;
    WotbModV3HookHandle hook_handle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            mod,
            "Camera::update",
            &hook_info,
            &hook_handle) == WOTBMOD_V3_OK);
    WotbModV3HookInfo pending = {};
    pending.struct_size = sizeof(pending);
    pending.api_version = WOTBMOD_V3_HOOKS_VERSION;
    CHECK(
        hooks->get_info(mod, hook_handle, &pending) ==
        WOTBMOD_V3_OK);
    CHECK(pending.status == WOTBMOD_V3_HOOK_STATUS_PENDING_BACKEND);
    WotbModV3Handle hook_owner = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t hook_status = WOTBMOD_V3_HOOK_STATUS_FAILED;
    CHECK(
        hooks->get_owner(mod, hook_handle, &hook_owner) ==
        WOTBMOD_V3_OK);
    CHECK(
        hooks->get_status(mod, hook_handle, &hook_status) ==
        WOTBMOD_V3_OK);
    CHECK(hook_owner == mod);
    CHECK(hook_status == WOTBMOD_V3_HOOK_STATUS_PENDING_BACKEND);
    CHECK(
        hooks->set_priority(mod, hook_handle, 123) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    pending.struct_size = sizeof(pending);
    pending.api_version = WOTBMOD_V3_HOOKS_VERSION;
    CHECK(
        hooks->get_info(mod, hook_handle, &pending) ==
        WOTBMOD_V3_OK);
    CHECK(pending.priority == 0);
    WotbModV3HookHandle related_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            mod,
            "Camera::update",
            &hook_info,
            &related_hook) == WOTBMOD_V3_OK);
    CHECK(
        hooks->run_before(mod, hook_handle, related_hook) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(
        hooks->run_after(mod, hook_handle, related_hook) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    uint32_t chain_count = 0u;
    CHECK(
        hooks->get_chain(
            mod,
            "Camera::update",
            nullptr,
            &chain_count) == WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(chain_count == 2u);
    WotbModV3HookInfo chain[2] = {};
    for (WotbModV3HookInfo& item : chain) {
        item.struct_size = sizeof(item);
        item.api_version = WOTBMOD_V3_HOOKS_VERSION;
    }
    CHECK(
        hooks->get_chain(
            mod,
            "Camera::update",
            chain,
            &chain_count) == WOTBMOD_V3_OK);
    CHECK(chain_count == 2u);
    CHECK(chain[0].hook == hook_handle);
    CHECK(chain[1].hook == related_hook);
    CHECK(
        hooks->enable(mod, hook_handle) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    void* vtable_entries[] = {
        reinterpret_cast<void*>(&NativeHookTarget)};
    struct VtableFixture {
        void** vtable;
    };
    VtableFixture vtable_fixture = {vtable_entries};
    WotbModV3HookHandle denied_vtable =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_vtable(
            mod,
            &vtable_fixture,
            0u,
            &hook_info,
            &denied_vtable) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);

    WotbModV3Handle restricted_hook_mod =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "restricted_hook_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &restricted_hook_mod) == WOTBMOD_V3_OK);
    const char* restricted_hook_grants[] = {"core"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            restricted_hook_mod,
            restricted_hook_grants,
            1u,
            1u) == WOTBMOD_V3_OK);
    WotbModV3HookHandle restricted_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            restricted_hook_mod,
            "Camera::update",
            &hook_info,
            &restricted_hook) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        WotbModV3Runtime_DestroyMod(restricted_hook_mod) ==
        WOTBMOD_V3_OK);

    WotbModV3Handle camera_hook_mod =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "camera_hook_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &camera_hook_mod) == WOTBMOD_V3_OK);
    const char* camera_hook_grants[] = {
        "gameplay.tweak.camera"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            camera_hook_mod,
            camera_hook_grants,
            1u,
            1u) == WOTBMOD_V3_OK);
    WotbModV3HookHandle camera_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            camera_hook_mod,
            "Camera::setFOV",
            &hook_info,
            &camera_hook) == WOTBMOD_V3_OK);

    // Two names for one function must cost the same. "Camera::setFOV" and
    // "DAVA::Camera::SetFovY" resolve to a single address, so the second
    // spelling must not be a way around gameplay.tweak.camera. This mod holds
    // only hooks.symbol; both spellings must refuse it, and a genuinely plain
    // reviewed symbol must still work, because pricing aliases is not a
    // licence to raise the other reviewed targets.
    WotbModV3Handle alias_hook_mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "alias_hook_test.dll",
            WOTBMOD_V3_PERMISSION_REVIEWED,
            &alias_hook_mod) == WOTBMOD_V3_OK);
    const char* alias_hook_grants[] = {"hooks.symbol"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            alias_hook_mod,
            alias_hook_grants,
            1u,
            1u) == WOTBMOD_V3_OK);
    WotbModV3HookHandle alias_denied_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            alias_hook_mod,
            "Camera::setFOV",
            &hook_info,
            &alias_denied_hook) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        hooks->create_symbol(
            alias_hook_mod,
            "DAVA::Camera::SetFovY",
            &hook_info,
            &alias_denied_hook) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(alias_denied_hook == WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3HookHandle alias_plain_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            alias_hook_mod,
            "Camera::update",
            &hook_info,
            &alias_plain_hook) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_DestroyMod(alias_hook_mod) ==
        WOTBMOD_V3_OK);
    // The camera grant opens the alias exactly as it opens the canonical
    // name, so the two spellings really are interchangeable, not merely both
    // expensive.
    WotbModV3HookHandle camera_alias_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            camera_hook_mod,
            "DAVA::Camera::SetFovY",
            &hook_info,
            &camera_alias_hook) == WOTBMOD_V3_OK);

    WotbModV3HookCreateInfo native_hook_info = hook_info;
    native_hook_info.mode = WOTBMOD_V3_HOOK_AROUND;
    native_hook_info.flags = WOTBMOD_V3_HOOK_CREATE_NONE;
    native_hook_info.detour =
        reinterpret_cast<void*>(&NativeHookDetour);
    WotbModV3HookHandle native_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            camera_hook_mod,
            "Camera::setFOV",
            &native_hook_info,
            &native_hook) == WOTBMOD_V3_OK);
    CHECK(g_native_hook_create_count.load() == 1u);
    void* original = nullptr;
    CHECK(
        hooks->get_original(
            camera_hook_mod,
            native_hook,
            &original) == WOTBMOD_V3_OK);
    CHECK(original == reinterpret_cast<void*>(&NativeHookTarget));
    CHECK(
        hooks->enable(camera_hook_mod, native_hook) ==
        WOTBMOD_V3_OK);
    CHECK(g_native_hook_enable_count.load() == 1u);
    CHECK(
        hooks->call_next(
            camera_hook_mod,
            native_hook,
            nullptr,
            nullptr) == WOTBMOD_V3_E_NOT_SUPPORTED);
    WotbModV3ErrorInfo call_next_error = {};
    WOTBMOD_V3_INIT_STRUCT(
        call_next_error, WOTBMOD_V3_ABI_VERSION);
    CHECK(
        bootstrap->get_last_error(
            camera_hook_mod,
            &call_next_error) == WOTBMOD_V3_OK);
    CHECK(call_next_error.code == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(std::strstr(call_next_error.message, "hook chain") != nullptr);
    CHECK(std::strstr(call_next_error.message, "backend") == nullptr);
    WotbModV3HookHandle conflicting_native_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            camera_hook_mod,
            "Camera::setFOV",
            &native_hook_info,
            &conflicting_native_hook) ==
        WOTBMOD_V3_E_CONFLICT);
    CHECK(
        hooks->disable(camera_hook_mod, native_hook) ==
        WOTBMOD_V3_OK);
    CHECK(g_native_hook_disable_count.load() == 1u);
    CHECK(
        hooks->remove(camera_hook_mod, native_hook) ==
        WOTBMOD_V3_OK);
    CHECK(g_native_hook_remove_count.load() == 1u);
    WotbModV3HookHandle generic_hook =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_symbol(
            camera_hook_mod,
            "Camera::update",
            &hook_info,
            &generic_hook) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    // OBSERVE/AFTER through the version 2 backend, which describes exactly
    // one target: they attach as observers, never touch create(), do not
    // conflict with each other, and refuse get_original. A fresh mod holding
    // hooks.symbol, which is what a plain reviewed symbol costs.
    {
        WotbModV3Handle observer_mod = WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            WotbModV3Runtime_CreateMod(
                "observer_hook_test.dll",
                WOTBMOD_V3_PERMISSION_REVIEWED,
                &observer_mod) == WOTBMOD_V3_OK);
        const char* observer_grants[] = {"hooks.symbol"};
        CHECK(
            WotbModV3Runtime_SetPermissionGrants(
                observer_mod, observer_grants, 1u, 1u) == WOTBMOD_V3_OK);
        WotbModV3HookCreateInfo observe = {};
        WOTBMOD_V3_INIT_STRUCT(observe, WOTBMOD_V3_HOOKS_VERSION);
        observe.mode = WOTBMOD_V3_HOOK_OBSERVE;
        observe.priority = 7;
        observe.detour = reinterpret_cast<void*>(&NativeHookDetour);
        const uint32_t creates_before = g_native_hook_create_count.load();
        const uint32_t attaches_before = g_native_attach_count.load();
        const uint32_t detaches_before = g_native_detach_count.load();
        const uint32_t toggles_before = g_native_set_enabled_count.load();
        WotbModV3HookHandle first = WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            hooks->create_symbol(
                observer_mod, "Vehicle::set_health", &observe, &first) ==
            WOTBMOD_V3_OK);
        CHECK(g_native_attach_count.load() == attaches_before + 1u);
        const uint64_t first_token = 1000u + attaches_before;
        CHECK(g_last_attach_mode == WOTBMOD_V3_HOOK_OBSERVE);
        CHECK(g_last_attach_priority == 7);
        CHECK(g_native_hook_create_count.load() == creates_before);
        uint32_t observe_status = 0u;
        CHECK(
            hooks->get_status(observer_mod, first, &observe_status) ==
            WOTBMOD_V3_OK);
        CHECK(observe_status == WOTBMOD_V3_HOOK_STATUS_DISABLED);
        CHECK(hooks->enable(observer_mod, first) == WOTBMOD_V3_OK);
        CHECK(g_native_set_enabled_count.load() == toggles_before + 1u);
        CHECK(
            hooks->get_status(observer_mod, first, &observe_status) ==
            WOTBMOD_V3_OK);
        CHECK(observe_status == WOTBMOD_V3_HOOK_STATUS_ENABLED);
        void* observe_original = reinterpret_cast<void*>(1);
        CHECK(
            hooks->get_original(observer_mod, first, &observe_original) ==
            WOTBMOD_V3_E_NOT_SUPPORTED);
        CHECK(observe_original == nullptr);
        WotbModV3HookHandle second = WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            hooks->create_symbol(
                observer_mod, "Vehicle::set_health", &observe, &second) ==
            WOTBMOD_V3_OK);
        observe.mode = WOTBMOD_V3_HOOK_AFTER;
        WotbModV3HookHandle after = WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            hooks->create_symbol(
                observer_mod, "Vehicle::set_health", &observe, &after) ==
            WOTBMOD_V3_OK);
        CHECK(g_native_attach_count.load() == attaches_before + 3u);
        CHECK(g_last_attach_mode == WOTBMOD_V3_HOOK_AFTER);
        // AROUND beside observers still goes through create()
        observe.mode = WOTBMOD_V3_HOOK_AROUND;
        WotbModV3HookHandle around = WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            hooks->create_symbol(
                observer_mod, "Vehicle::set_health", &observe, &around) ==
            WOTBMOD_V3_OK);
        CHECK(g_native_hook_create_count.load() == creates_before + 1u);
        // BEFORE has its own refusal
        observe.mode = WOTBMOD_V3_HOOK_BEFORE;
        WotbModV3HookHandle before = WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            hooks->create_symbol(
                observer_mod, "Vehicle::set_health", &observe, &before) ==
            WOTBMOD_V3_E_NOT_SUPPORTED);
        WotbModV3ErrorInfo observe_error = {};
        WOTBMOD_V3_INIT_STRUCT(observe_error, WOTBMOD_V3_ABI_VERSION);
        CHECK(
            bootstrap->get_last_error(observer_mod, &observe_error) ==
            WOTBMOD_V3_OK);
        CHECK(
            std::strstr(
                observe_error.message,
                "BEFORE has no cancel protocol yet") != nullptr);
        // OBSERVE on a resolvable target the backend does not describe
        observe.mode = WOTBMOD_V3_HOOK_OBSERVE;
        WotbModV3HookHandle undescribed = WOTBMOD_V3_INVALID_HANDLE;
        CHECK(
            hooks->create_symbol(
                observer_mod, "Vehicle::showShooting", &observe, &undescribed) ==
            WOTBMOD_V3_E_NOT_SUPPORTED);
        WOTBMOD_V3_INIT_STRUCT(observe_error, WOTBMOD_V3_ABI_VERSION);
        CHECK(
            bootstrap->get_last_error(observer_mod, &observe_error) ==
            WOTBMOD_V3_OK);
        CHECK(
            std::strstr(
                observe_error.message,
                "signature of this target is not described") != nullptr);
        // disable and remove go through the token slots
        CHECK(hooks->disable(observer_mod, first) == WOTBMOD_V3_OK);
        CHECK(g_native_set_enabled_count.load() == toggles_before + 2u);
        CHECK(hooks->remove(observer_mod, first) == WOTBMOD_V3_OK);
        CHECK(g_native_detach_count.load() == detaches_before + 1u);
        CHECK(g_last_detached_token == first_token);
        CHECK(hooks->remove(observer_mod, second) == WOTBMOD_V3_OK);
        CHECK(hooks->remove(observer_mod, after) == WOTBMOD_V3_OK);
        CHECK(hooks->remove(observer_mod, around) == WOTBMOD_V3_OK);
        CHECK(g_native_detach_count.load() == detaches_before + 3u);
        CHECK(WotbModV3Runtime_DestroyMod(observer_mod) == WOTBMOD_V3_OK);
    }

    CHECK(
        WotbModV3Runtime_DestroyMod(camera_hook_mod) ==
        WOTBMOD_V3_OK);

    WotbModV3Handle unsafe_hook_mod =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "unsafe_hook_test.dll",
            WOTBMOD_V3_PERMISSION_UNSAFE,
            &unsafe_hook_mod) == WOTBMOD_V3_OK);
    const char* unsafe_hook_grants[] = {
        "native.hook.address"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            unsafe_hook_mod,
            unsafe_hook_grants,
            1u,
            1u) == WOTBMOD_V3_OK);
    WotbModV3HookHandle unsafe_vtable =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        hooks->create_vtable(
            unsafe_hook_mod,
            &vtable_fixture,
            0u,
            &hook_info,
            &unsafe_vtable) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_DestroyMod(unsafe_hook_mod) ==
        WOTBMOD_V3_OK);

    table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_HTTP,
            WOTBMOD_V3_HTTP_VERSION,
            &table) == WOTBMOD_V3_OK);
    const WotbModV3HttpApiV1* http =
        static_cast<const WotbModV3HttpApiV1*>(table);
    WotbModV3HttpHandle request = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(http->request_create(mod, &request) == WOTBMOD_V3_OK);
    CHECK(
        http->request_send_async(mod, request, nullptr, nullptr) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(
        http->request_set_url(
            mod, request, "https://api.example.com/test") ==
        WOTBMOD_V3_OK);
    uint32_t response_status = 0u;
    CHECK(
        http->response_get_status(mod, request, &response_status) ==
        WOTBMOD_V3_E_BUSY);

    WotbModV3EventSubscriptionInfo lifecycle_events = {};
    lifecycle_events.struct_size = sizeof(lifecycle_events);
    lifecycle_events.api_version = WOTBMOD_V3_EVENTS_VERSION;
    lifecycle_events.topic_pattern = "wotbmod.mod.*";
    lifecycle_events.receive_system_events = 1u;
    WotbModV3EventToken lifecycle_event_token =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_events->subscribe(
            mod,
            &lifecycle_events,
            &LifecycleEvent,
            nullptr,
            &lifecycle_event_token) == WOTBMOD_V3_OK);
    WotbModV3Handle subject = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "lifecycle_subject.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &subject) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo subject_module = {};
    subject_module.struct_size = sizeof(subject_module);
    subject_module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            subject, &SubjectEntry, &subject_module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(subject) == WOTBMOD_V3_OK);
    found_mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        intermod->mod_find(
            mod, "tests.lifecycle-subject", &found_mod) ==
        WOTBMOD_V3_OK);
    CHECK(found_mod == subject);
    found_version_size = sizeof(found_version);
    CHECK(
        intermod->mod_get_version(
            mod,
            subject,
            found_version,
            &found_version_size) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(found_version, "2.3.4") == 0);
    CHECK(WotbModV3Runtime_Disable(subject) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(subject) == WOTBMOD_V3_OK);
    CHECK(g_lifecycle_payload_error.load() == 0u);
    CHECK(g_lifecycle_transition_mask.load() == 0x3fu);

#if defined(WOTBMOD_V3_RUNTIME_SERVICES_STANDALONE_TEST)
    WotbModV3EventSubscriptionInfo shutdown_info = {};
    shutdown_info.struct_size = sizeof(shutdown_info);
    shutdown_info.api_version = WOTBMOD_V3_EVENTS_VERSION;
    shutdown_info.topic_pattern =
        "mod.tests.runtime-services.shutdown-quiescence";
    shutdown_info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    WotbModV3EventToken shutdown_token =
        WOTBMOD_V3_INVALID_HANDLE;
    BlockingEventState shutdown_state;
    CHECK(
        g_events->subscribe(
            mod,
            &shutdown_info,
            &BlockingEvent,
            &shutdown_state,
            &shutdown_token) == WOTBMOD_V3_OK);

    WotbModV3EventSubscriptionInfo pending_subscribe_info = {};
    pending_subscribe_info.struct_size =
        sizeof(pending_subscribe_info);
    pending_subscribe_info.api_version =
        WOTBMOD_V3_EVENTS_VERSION;
    pending_subscribe_info.topic_pattern =
        "mod.tests.runtime-services.subscribe-during-shutdown";
    pending_subscribe_info.priority =
        WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    WotbModV3EventToken pending_subscribe_token =
        WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result pending_subscribe_result =
        WOTBMOD_V3_E_PLATFORM;
    wotbmod::v3::testing::
        ArmEventSubscribePublishBarrierForTesting();
    std::thread pending_subscriber([&]() {
        pending_subscribe_result = g_events->subscribe(
            mod,
            &pending_subscribe_info,
            &LowEvent,
            nullptr,
            &pending_subscribe_token);
    });
    const bool pending_subscribe_reached =
        wotbmod::v3::testing::
            WaitEventSubscribePublishBarrierForTesting(2000u);

    WotbModV3Result shutdown_post_result =
        WOTBMOD_V3_E_PLATFORM;
    std::thread shutdown_poster([&]() {
        shutdown_post_result = g_events->post(
            mod,
            shutdown_info.topic_pattern,
            nullptr,
            0u,
            0u);
    });
    bool shutdown_callback_entered = false;
    {
        std::unique_lock<std::mutex> lock(shutdown_state.mutex);
        shutdown_callback_entered = shutdown_state.cv.wait_for(
            lock,
            std::chrono::seconds(2),
            [&]() { return shutdown_state.entered; });
    }
    std::atomic<bool> event_shutdown_started{false};
    std::atomic<bool> event_shutdown_returned{false};
    std::thread event_shutdown;
    bool event_shutdown_returned_before_release = false;
    if (shutdown_callback_entered &&
        pending_subscribe_reached) {
        event_shutdown = std::thread([&]() {
            event_shutdown_started.store(true);
            wotbmod::v3::testing::
                ShutdownEventSubscriptionsForTesting();
            event_shutdown_returned.store(true);
        });
        const std::chrono::steady_clock::time_point start_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (!event_shutdown_started.load() &&
               std::chrono::steady_clock::now() < start_deadline) {
            std::this_thread::yield();
        }
        const std::chrono::steady_clock::time_point close_deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(2);
        while (wotbmod::v3::testing::
                   EventSubscriptionsAcceptingForTesting() &&
               std::chrono::steady_clock::now() < close_deadline) {
            std::this_thread::yield();
        }
        event_shutdown_returned_before_release =
            event_shutdown_returned.load();
    }
    {
        std::lock_guard<std::mutex> lock(shutdown_state.mutex);
        shutdown_state.release = true;
    }
    shutdown_state.cv.notify_all();
    if (event_shutdown.joinable()) {
        event_shutdown.join();
    }
    wotbmod::v3::testing::
        ReleaseEventSubscribePublishBarrierForTesting();
    pending_subscriber.join();
    shutdown_poster.join();
    CHECK(shutdown_callback_entered);
    CHECK(pending_subscribe_reached);
    CHECK(event_shutdown_started.load());
    CHECK(
        !wotbmod::v3::testing::
             EventSubscriptionsAcceptingForTesting());
    CHECK(!event_shutdown_returned_before_release);
    CHECK(event_shutdown_returned.load());
    CHECK(pending_subscribe_result == WOTBMOD_V3_E_CANCELLED);
    CHECK(
        pending_subscribe_token ==
        WOTBMOD_V3_INVALID_HANDLE);
    CHECK(shutdown_post_result == WOTBMOD_V3_OK);
    CHECK(shutdown_state.calls.load() == 1u);
    shutdown_state.alive.store(false);
    CHECK(
        g_events->post(
            mod,
            shutdown_info.topic_pattern,
            nullptr,
            0u,
            0u) == WOTBMOD_V3_OK);
    CHECK(shutdown_state.calls.load() == 1u);
    CHECK(!shutdown_state.callback_after_unsubscribe.load());
#endif

    WotbModV3Handle race_mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "build\\v3_runtime_test\\race\\old.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &race_mod) == WOTBMOD_V3_OK);
    std::atomic<bool> race_query_started{false};
    std::atomic<bool> race_saw_destroy{false};
    std::atomic<bool> race_stop{false};
    std::atomic<bool> race_unexpected{false};
    std::thread lifecycle_query([&]() {
        while (!race_stop.load()) {
            char path[WOTBMOD_V3_MAX_PATH] = {};
            uint32_t size = sizeof(path);
            const WotbModV3Result result =
                lifecycle->get_install_path(
                    race_mod,
                    path,
                    &size);
            if (result == WOTBMOD_V3_OK) {
                if (path[0] == '\0' ||
                    size != std::strlen(path) + 1u) {
                    race_unexpected.store(true);
                }
                race_query_started.store(true);
            } else if (
                result == WOTBMOD_V3_E_INVALID_HANDLE) {
                race_saw_destroy.store(true);
            } else {
                race_unexpected.store(true);
            }
            std::this_thread::yield();
        }
    });
    const auto race_query_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!race_query_started.load() &&
           std::chrono::steady_clock::now() <
               race_query_deadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    const WotbModV3Result race_destroy_result =
        WotbModV3Runtime_DestroyMod(race_mod);
    WotbModV3Handle replacement_mod =
        WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result replacement_create_result =
        WotbModV3Runtime_CreateMod(
            "build\\v3_runtime_test\\race\\new.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &replacement_mod);
    const auto race_destroy_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(2);
    while (!race_saw_destroy.load() &&
           std::chrono::steady_clock::now() <
               race_destroy_deadline) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    race_stop.store(true);
    lifecycle_query.join();
    CHECK(race_query_started.load());
    CHECK(race_destroy_result == WOTBMOD_V3_OK);
    CHECK(replacement_create_result == WOTBMOD_V3_OK);
    CHECK(replacement_mod != race_mod);
    CHECK(race_saw_destroy.load());
    CHECK(!race_unexpected.load());
    char replacement_path[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t replacement_path_size =
        sizeof(replacement_path);
    CHECK(
        lifecycle->get_install_path(
            replacement_mod,
            replacement_path,
            &replacement_path_size) ==
        WOTBMOD_V3_OK);
    CHECK(
        std::strstr(replacement_path, "\\race") != nullptr);
    CHECK(
        WotbModV3Runtime_DestroyMod(replacement_mod) ==
        WOTBMOD_V3_OK);

    WotbModV3TaskSubmitInfo slow = {};
    slow.struct_size = sizeof(slow);
    slow.api_version = WOTBMOD_V3_ASYNC_VERSION;
    slow.description = "cooperative unload cancellation";
    slow.work = &SlowTaskWork;
    slow.completion_thread_role = WOTBMOD_V3_THREAD_WORKER;
    WotbModV3TaskHandle slow_task = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        g_async->task_submit(mod, &slow, &slow_task) ==
        WOTBMOD_V3_OK);
    for (uint32_t attempt = 0u;
         attempt < 500u && g_slow_task_started.load() == 0u;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(g_slow_task_started.load() == 1u);
    CHECK(lifecycle->request_disable(mod, mod) == WOTBMOD_V3_OK);
    CHECK(g_slow_task_cancelled.load() == 1u);
    CHECK(g_cleanup_count.load() == 1u);
    CHECK(WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK);
    install_capacity = sizeof(install_path);
    CHECK(
        lifecycle->get_install_path(
            mod,
            install_path,
            &install_capacity) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        WotbModV3Runtime_DestroyMod(dependency_mod) ==
        WOTBMOD_V3_OK);
    BlockingMainCallTestState shutdown_cancelled_post;
    SetMainIngressOnline(true);
    CHECK(
        PostMainThreadEx(
            &BlockingMainCallCallback,
            &shutdown_cancelled_post,
            &BlockingMainCallCancel) == WOTBMOD_V3_OK);
    CHECK(shutdown_cancelled_post.calls.load() == 0u);
    CHECK(shutdown_cancelled_post.cancellations.load() == 0u);
    WotbModV3Runtime_Shutdown();
    CHECK(shutdown_cancelled_post.calls.load() == 0u);
    CHECK(shutdown_cancelled_post.cancellations.load() == 1u);
    CHECK(PumpMainThread(16u) == 0u);
    std::puts("V3 RUNTIME SERVICES OK");
    return 0;
}
