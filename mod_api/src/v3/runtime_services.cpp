#include "wotb_mod_v3_internal.h"
#include "client_services_backend.h"
#include "ges_services.h"
#include "http_transport_test.h"

#include "../../include/wotbmod/async_v1.h"
#include "../../include/wotbmod/core_v1.h"
#include "../../include/wotbmod/events_v1.h"
#include "../../include/wotbmod/hooks_v1.h"
#include "../../include/wotbmod/http_v1.h"
#include "../../include/wotbmod/intermod_v1.h"
#include "../../include/wotbmod/interface_ids.h"
#include "../../include/wotbmod/lifecycle_v1.h"
#include "../../include/wotbmod/projectile_v2.h"
#include "../../include/wotbmod/resources_v1.h"
#include "../../include/wotbmod/unsafe_native_v1.h"
#include "../../include/wotbmod/vfs_v1.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

/*
 * Legacy-runtime bridge for hot reload. The HMODULE that backs a mod is owned by
 * the legacy loader (src/wotb_mod_runtime.cpp), not by this V3 service. Rather
 * than link against it directly (which would leave every unit-test binary that
 * compiles this file without the loader with two unresolved symbols), the loader
 * installs its reload handlers at startup via WotbModV3Runtime_SetReloadHost.
 * When the handlers are null - as in the standalone service tests, which have no
 * mod loader at all - reload correctly reports "unsupported".
 *
 * RequestModReload only queues the reload (returns OK immediately; the actual
 * unload/reload happens at the frame boundary). CanReloadMod reports whether a
 * reload is possible right now.
 */
extern "C" {
typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3ReloadRequestFn)(
    WotbModV3Handle target);
typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3ReloadQueryFn)(
    WotbModV3Handle target);
}

static WotbModV3ReloadRequestFn g_reload_request_host = nullptr;
static WotbModV3ReloadQueryFn g_reload_query_host = nullptr;

extern "C" void WOTBMOD_V3_CALL WotbModV3Runtime_SetReloadHost(
    WotbModV3ReloadRequestFn request_host,
    WotbModV3ReloadQueryFn query_host) {
    g_reload_request_host = request_host;
    g_reload_query_host = query_host;
}

namespace wotbmod {
namespace v3 {
namespace {

using Clock = std::chrono::steady_clock;

const size_t kMaxEventSubscriptions = 4096u;
const size_t kMaxIntermodSubscriptions = 2048u;
const size_t kMaxExports = 1024u;
const size_t kMaxHooks = 1024u;
const size_t kMaxWorkerQueue = 256u;
const size_t kMaxRoleQueue = 1024u;
const size_t kWorkerCount = 4u;
const uint32_t kDefaultPumpLimit = 64u;

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now().time_since_epoch())
            .count());
}

bool HasText(const char* text, size_t maximum) {
    if (!text || text[0] == '\0') return false;
    size_t length = 0u;
    while (length <= maximum && text[length] != '\0') ++length;
    return length <= maximum;
}

bool StartsWith(const std::string& value, const char* prefix) {
    const size_t size = std::strlen(prefix);
    return value.size() >= size &&
           value.compare(0u, size, prefix) == 0;
}

bool EndsWith(const std::string& value, char suffix) {
    return !value.empty() && value[value.size() - 1u] == suffix;
}

void CopyText(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    destination[0] = '\0';
    if (!source) return;
#if defined(_MSC_VER)
    strncpy_s(destination, capacity, source, _TRUNCATE);
#else
    std::strncpy(destination, source, capacity - 1u);
    destination[capacity - 1u] = '\0';
#endif
}

WotbModV3Result CopyStringResult(
    WotbModV3Handle mod,
    const std::string& value,
    char* buffer,
    uint32_t* inout_size) {
    if (!inout_size) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inout_size is required");
    }
    const uint32_t required = static_cast<uint32_t>(value.size() + 1u);
    if (!buffer || *inout_size < required) {
        *inout_size = required;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer, value.c_str(), required);
    *inout_size = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result CopyBytesResult(
    WotbModV3Handle mod,
    const std::vector<uint8_t>& value,
    WotbModV3Buffer* output) {
    if (!output ||
        output->struct_size < sizeof(WotbModV3Buffer) ||
        output->api_version != WOTBMOD_V3_ASYNC_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "output buffer has an invalid structure header");
    }
    output->size = static_cast<uint32_t>(value.size());
    if (value.empty()) return WOTBMOD_V3_OK;
    if (!output->data || output->capacity < value.size()) {
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(output->data, value.data(), value.size());
    return WOTBMOD_V3_OK;
}

bool ValidPriority(int32_t priority) {
    return priority >= -1000000 && priority <= 1000000;
}

bool ValidThreadRole(uint32_t role, bool allow_worker) {
    return role == WOTBMOD_V3_THREAD_MAIN ||
           role == WOTBMOD_V3_THREAD_RENDER ||
           role == WOTBMOD_V3_THREAD_AUDIO ||
           (allow_worker && role == WOTBMOD_V3_THREAD_WORKER);
}

std::atomic<bool> g_main_ingress_online{false};

bool HasGuaranteedAsyncIngress(uint32_t role) {
    if (role == WOTBMOD_V3_THREAD_MAIN) {
        return g_main_ingress_online.load(std::memory_order_acquire);
    }
    return role == WOTBMOD_V3_THREAD_RENDER ||
           role == WOTBMOD_V3_THREAD_WORKER;
}

class CallbackScope {
public:
    explicit CallbackScope(WotbModV3Handle mod)
        : mod_(mod), entered_(EnterModCallback(mod) == WOTBMOD_V3_OK) {}

    ~CallbackScope() {
        if (entered_) LeaveModCallback(mod_);
    }

    bool entered() const { return entered_; }

private:
    WotbModV3Handle mod_;
    bool entered_;
};

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

struct CleanupRegistration {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3CleanupCallback callback = nullptr;
    void* user_data = nullptr;
    std::atomic<bool> invoke_on_destroy{true};
};

void DestroyCleanupRegistration(void* object) {
    std::unique_ptr<CleanupRegistration> registration(
        static_cast<CleanupRegistration*>(object));
    if (!registration || !registration->callback ||
        !registration->invoke_on_destroy.exchange(false)) {
        return;
    }
    try {
        registration->callback(
            registration->owner,
            WOTBMOD_V3_CLEANUP_MOD_UNLOADING,
            registration->user_data);
    } catch (...) {
        SetError(
            registration->owner,
            WOTBMOD_V3_E_CALLBACK_FAULT,
            "cleanup callback raised an exception");
    }
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleGetCurrent(
    WotbModV3Handle mod,
    WotbModV3Handle* out_current_mod) {
    if (!out_current_mod) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_current_mod is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    *out_current_mod = mod;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleGetInfo(
    WotbModV3Handle mod,
    WotbModV3LifecycleInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3LifecycleInfo) ||
        out_info->api_version != WOTBMOD_V3_LIFECYCLE_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "lifecycle info has an invalid structure header");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    const uint32_t size = out_info->struct_size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = size;
    out_info->api_version = WOTBMOD_V3_LIFECYCLE_VERSION;
    out_info->mod = mod;
    const uint32_t runtime_state = ModState(mod);
    switch (runtime_state) {
        case WOTBMOD_V3_MOD_STATE_LOADED:
        case WOTBMOD_V3_MOD_STATE_ENABLED:
        case WOTBMOD_V3_MOD_STATE_DISABLED:
        case WOTBMOD_V3_MOD_STATE_UNLOADING:
            out_info->state = runtime_state;
            break;
        default:
            out_info->state = WOTBMOD_V3_MOD_STATE_UNKNOWN;
            break;
    }
    out_info->permission_tier = ModPermissionTier(mod);
    // Three-way on purpose: "cannot reload" has two very different causes, and
    // reporting "already queued" when the loader is simply absent would be a
    // lie the caller cannot act on.
    const WotbModV3Result reload_state = g_reload_query_host
        ? g_reload_query_host(mod)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
    const bool can_reload = reload_state == WOTBMOD_V3_OK;
    out_info->hot_reload_supported = can_reload ? 1u : 0u;
    CopyText(out_info->id, sizeof(out_info->id), ModId(mod));
    CopyText(
        out_info->hot_reload_reason,
        sizeof(out_info->hot_reload_reason),
        can_reload
            ? "request_reload() unloads and reloads the module at the frame "
              "boundary"
            : (g_reload_query_host
                   ? "a reload is already queued for this module"
                   : "request_reload requires the loader runtime and is "
                     "unavailable"));
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleInstallPath(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModInstallDirectory(mod, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleResourcePath(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModResourceDirectory(mod, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleDataPath(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModDataDirectory(mod, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleCachePath(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModCacheDirectory(mod, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleConfigPath(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModConfigDirectory(mod, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleRegisterCleanup(
    WotbModV3Handle mod,
    WotbModV3CleanupCallback callback,
    void* user_data,
    WotbModV3Token* out_token) {
    if (!callback || !out_token) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "callback and out_token are required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::unique_ptr<CleanupRegistration> registration(
        new (std::nothrow) CleanupRegistration());
    if (!registration) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "cleanup registration allocation failed");
    }
    registration->owner = mod;
    registration->callback = callback;
    registration->user_data = user_data;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_LIFECYCLE_CLEANUP,
        registration.get(),
        &DestroyCleanupRegistration,
        &handle);
    if (result != WOTBMOD_V3_OK) return result;
    registration.release();
    *out_token = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleUnregisterCleanup(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        token,
        WOTBMOD_V3_HANDLE_LIFECYCLE_CLEANUP,
        &object,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    CleanupRegistration* registration =
        static_cast<CleanupRegistration*>(object);
    registration->invoke_on_destroy.store(false);
    return ReleaseOwnedHandle(mod, token);
}

WotbModV3Result LifecycleUnsupportedMutation(
    WotbModV3Handle mod,
    WotbModV3Handle target_mod,
    const char* operation) {
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    if (target_mod != mod) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "a mod may only request its own lifecycle transition");
    }
    char message[WOTBMOD_V3_MAX_MESSAGE] = {};
#if defined(_MSC_VER)
    sprintf_s(
        message, "%s requires loader integration and is unavailable", operation);
#else
    std::snprintf(
        message, sizeof(message),
        "%s requires loader integration and is unavailable", operation);
#endif
    return SetError(mod, WOTBMOD_V3_E_NOT_SUPPORTED, message);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleRequestEnable(
    WotbModV3Handle mod,
    WotbModV3Handle target_mod) {
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    if (target_mod != mod) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "a mod may only request its own lifecycle transition");
    }
    return WotbModV3Runtime_Enable(target_mod);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleRequestDisable(
    WotbModV3Handle mod,
    WotbModV3Handle target_mod) {
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    if (target_mod != mod) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "a mod may only request its own lifecycle transition");
    }
    return WotbModV3Runtime_Disable(target_mod);
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleRequestReload(
    WotbModV3Handle mod,
    WotbModV3Handle target_mod) {
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    if (target_mod != mod) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "a mod may only request its own lifecycle transition");
    }
    if (!g_reload_request_host) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "request_reload requires the loader runtime and is unavailable");
    }
    // The unload/reload is deferred to the frame boundary, so this returns as
    // soon as the request is queued; the calling mod keeps running until then.
    const WotbModV3Result result = g_reload_request_host(target_mod);
    if (result != WOTBMOD_V3_OK) {
        return SetError(
            mod, result,
            "request_reload could not be queued for this module");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL LifecycleCanHotReload(
    WotbModV3Handle mod,
    WotbModV3Handle target_mod,
    uint32_t* out_can_hot_reload,
    char* reason,
    uint32_t* inout_reason_size) {
    if (!out_can_hot_reload) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_can_hot_reload is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    if (target_mod != mod) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "a mod may only inspect its own hot-reload readiness");
    }
    const WotbModV3Result can = g_reload_query_host
        ? g_reload_query_host(target_mod)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
    *out_can_hot_reload = (can == WOTBMOD_V3_OK) ? 1u : 0u;
    const std::string text =
        can == WOTBMOD_V3_OK
            ? "request_reload() unloads and reloads the module at the frame "
              "boundary"
            : (g_reload_query_host
                   ? "a reload is already queued for this module"
                   : "request_reload requires the loader runtime and is "
                     "unavailable");
    if (!inout_reason_size) return WOTBMOD_V3_OK;
    return CopyStringResult(mod, text, reason, inout_reason_size);
}

/* ------------------------------------------------------------------------- */
/* Event bus                                                                 */
/* ------------------------------------------------------------------------- */

struct EventSubscription {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3EventToken handle = WOTBMOD_V3_INVALID_HANDLE;
    std::string pattern;
    int32_t priority = 0;
    bool receive_system = false;
    /* Set once this subscription asked the GES bus to observe its pattern;
     * cleared by whichever deactivation path runs first. */
    std::atomic<bool> ges_observed{false};
    WotbModV3EventCallback callback = nullptr;
    void* user_data = nullptr;
    std::atomic<bool> active{true};
    uint64_t order = 0u;
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool accepting_callbacks = true;
    size_t callbacks_in_flight = 0u;
};

struct EventSubscriptionHandle {
    std::shared_ptr<EventSubscription> subscription;
};

struct EventCallbackFrame {
    const EventSubscription* subscription = nullptr;
    EventCallbackFrame* previous = nullptr;
};

thread_local EventCallbackFrame* g_event_callback_frame = nullptr;

size_t CurrentEventCallbackDepth(
    const EventSubscription* subscription) {
    size_t depth = 0u;
    for (EventCallbackFrame* frame = g_event_callback_frame;
         frame;
         frame = frame->previous) {
        if (frame->subscription == subscription) {
            ++depth;
        }
    }
    return depth;
}

void RemoveEventSubscriptionIfDrained(
    const std::shared_ptr<EventSubscription>& subscription);

class EventCallbackScope {
public:
    explicit EventCallbackScope(
        const std::shared_ptr<EventSubscription>& subscription)
        : subscription_(subscription) {
        if (!subscription_) return;
        {
            std::lock_guard<std::mutex> lock(
                subscription_->callback_mutex);
            if (!subscription_->active.load() ||
                !subscription_->accepting_callbacks) {
                return;
            }
            ++subscription_->callbacks_in_flight;
        }
        frame_.subscription = subscription_.get();
        frame_.previous = g_event_callback_frame;
        g_event_callback_frame = &frame_;
        entered_ = true;
    }

    ~EventCallbackScope() {
        if (!entered_) return;
        g_event_callback_frame = frame_.previous;
        bool remove_inactive_subscription = false;
        {
            std::lock_guard<std::mutex> lock(
                subscription_->callback_mutex);
            if (subscription_->callbacks_in_flight != 0u) {
                --subscription_->callbacks_in_flight;
            }
            remove_inactive_subscription =
                subscription_->callbacks_in_flight == 0u &&
                !subscription_->active.load();
        }
        subscription_->callback_cv.notify_all();
        if (remove_inactive_subscription) {
            RemoveEventSubscriptionIfDrained(subscription_);
        }
    }

    EventCallbackScope(const EventCallbackScope&) = delete;
    EventCallbackScope& operator=(const EventCallbackScope&) = delete;

    bool entered() const { return entered_; }

private:
    std::shared_ptr<EventSubscription> subscription_;
    EventCallbackFrame frame_;
    bool entered_ = false;
};

struct EventDispatchControl {
    WotbModV3Token token = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle current_listener = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t timestamp_ns = 0u;
    uint64_t context_mask = 0u;
    uint32_t thread_role = WOTBMOD_V3_THREAD_UNKNOWN;
    uint32_t flags = 0u;
    bool stopped = false;
};

std::mutex g_event_mutex;
std::vector<std::shared_ptr<EventSubscription>> g_event_subscriptions;
std::unordered_map<WotbModV3Token, std::shared_ptr<EventDispatchControl>>
    g_event_dispatches;
bool g_event_accepting_subscriptions = true;
std::atomic<uint64_t> g_event_order{1u};
std::atomic<uint64_t> g_event_dispatch_token{1u};

#if defined(WOTBMOD_V3_RUNTIME_SERVICES_STANDALONE_TEST)
std::mutex g_event_subscribe_test_mutex;
std::condition_variable g_event_subscribe_test_cv;
bool g_event_subscribe_test_barrier_armed = false;
bool g_event_subscribe_test_barrier_reached = false;
bool g_event_subscribe_test_barrier_released = false;

void WaitAtEventSubscribePublishBarrierForTesting() {
    std::unique_lock<std::mutex> lock(g_event_subscribe_test_mutex);
    if (!g_event_subscribe_test_barrier_armed) return;
    g_event_subscribe_test_barrier_reached = true;
    g_event_subscribe_test_cv.notify_all();
    g_event_subscribe_test_cv.wait(
        lock,
        []() {
            return g_event_subscribe_test_barrier_released;
        });
    g_event_subscribe_test_barrier_armed = false;
    g_event_subscribe_test_barrier_reached = false;
    g_event_subscribe_test_barrier_released = false;
}
#endif

bool MatchesPattern(const std::string& pattern, const std::string& topic) {
    if (EndsWith(pattern, '*')) {
        const std::string prefix = pattern.substr(0u, pattern.size() - 1u);
        return topic.compare(0u, prefix.size(), prefix) == 0;
    }
    return pattern == topic;
}

struct SystemEventTopic {
    const char* topic;
    uint64_t required_sources;
};

const std::array<SystemEventTopic, 57u>& SystemEventTopics() {
    static const std::array<SystemEventTopic, 57u> topics = {{
        {WOTBMOD_V3_EVENT_CLIENT_SHUTTING_DOWN, 0u},
        {WOTBMOD_V3_EVENT_FRAME_UPDATE, 0u},
        {WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_UI_SCREEN},
        {WOTBMOD_V3_EVENT_UI_INPUT,
         WOTBMOD_V3_EVENT_SOURCE_UI_INPUT},
        {WOTBMOD_V3_EVENT_BATTLE_ENTERED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD},
        {WOTBMOD_V3_EVENT_BATTLE_STARTED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD},
        {WOTBMOD_V3_EVENT_BATTLE_ENDED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD},
        {WOTBMOD_V3_EVENT_BATTLE_LEFT,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD},
        {WOTBMOD_V3_EVENT_SCENE_ACTIVATED,
         WOTBMOD_V3_EVENT_SOURCE_SCENE_ACTIVATED},
        {WOTBMOD_V3_EVENT_SCENE_DEACTIVATED,
         WOTBMOD_V3_EVENT_SOURCE_SCENE_DEACTIVATED},
        {WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD |
             WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE},
        {WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CREATED,
         WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE},
        {WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED,
         WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH},
        {WOTBMOD_V3_EVENT_VEHICLE_SPAWNED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD},
        {WOTBMOD_V3_EVENT_VEHICLE_DESPAWNED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD},
        {WOTBMOD_V3_EVENT_SHOT_FIRED,
         WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING},
        {WOTBMOD_V3_EVENT_SHELL_HIT,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT},
        {WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH},
        {WOTBMOD_V3_EVENT_VEHICLE_DAMAGED,
         WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH},
        {WOTBMOD_V3_EVENT_DAMAGE_RECEIVED,
         WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH},
        {WOTBMOD_V3_EVENT_VEHICLE_DESTROYED,
         WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH},
        {WOTBMOD_V3_EVENT_RELOAD_STATE_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_RELOAD_STATE},
        {WOTBMOD_V3_EVENT_AMMO_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_AMMO_CHANGED},
        {WOTBMOD_V3_EVENT_AIM_TARGET_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_AIM_TARGET},
        {WOTBMOD_V3_EVENT_VEHICLE_SPOTTED,
         WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS},
        {WOTBMOD_V3_EVENT_VEHICLE_UNSPOTTED,
         WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS},
        {WOTBMOD_V3_EVENT_VEHICLE_KILLED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_KILLED},
        {WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_SESSION_CLUSTER},
        {WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED,
         WOTBMOD_V3_EVENT_SOURCE_CAMERA_MODE},
        {WOTBMOD_V3_EVENT_SNIPER_ENTERED,
         WOTBMOD_V3_EVENT_SOURCE_CAMERA_MODE},
        {WOTBMOD_V3_EVENT_SNIPER_EXITED,
         WOTBMOD_V3_EVENT_SOURCE_CAMERA_MODE},
        {WOTBMOD_V3_EVENT_MOD_PRELOAD, 0u},
        {WOTBMOD_V3_EVENT_MOD_LOADED, 0u},
        // Runtime-internal: published by WotbModV3Runtime_SetCapability, so
        // it needs no native event source and works on any client build.
        {WOTBMOD_V3_EVENT_CAPABILITIES_CHANGED, 0u},
        {WOTBMOD_V3_EVENT_MOD_ENABLED, 0u},
        {WOTBMOD_V3_EVENT_MOD_DISABLED, 0u},
        {WOTBMOD_V3_EVENT_MOD_UNLOADING, 0u},
        {WOTBMOD_V3_EVENT_MOD_UNLOADED, 0u},
        {WOTBMOD_V3_EVENT_LOCAL_SHELL_FIRED,
         WOTBMOD_V3_EVENT_SOURCE_LOCAL_SHELL},
        // THE PROJECTILE V2 TOPICS, WHICH THIS TABLE FORGOT.
        //
        // A topic that is absent here can never be subscribed to:
        // `PatternCanMatchPublishedSystemEvent` only walks this array, so
        // `EventsSubscribe` answers NOT_SUPPORTED with "event subscription
        // pattern has no available publisher" for anything it does not list.
        // The four `wotbmod.gameplay.projectile.*` topics ship in
        // `projectile_v2.h` and are among the 45 frozen RC1 interface IDs, but
        // the table was written for projectile v1 and never grew when v2
        // landed.
        //
        // So the loader was right all along. Measured on 11.19.0.834: the
        // showShooting detour reaches `ObserveShot`, the owner resolves, and
        // `PublishNewProjectile` returns 0 - the event is published and then
        // has nowhere to go, because no mod was ever allowed to listen. Four
        // shots gave four `gameplay.shot_fired` and zero projectile events;
        // 110k traced events across 82 sessions hold not one.
        //
        // Sources are the detours that actually produce each state:
        // `ObserveShot` runs from the showShooting hook, `ObserveImpact` from
        // GameSceneController::OnVehicleHitDamage.
        {WOTBMOD_V3_EVENT_PROJECTILE_CREATED,
         WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING |
             WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT},
        {WOTBMOD_V3_EVENT_PROJECTILE_UPDATED,
         WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING |
             WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT},
        {WOTBMOD_V3_EVENT_PROJECTILE_IMPACTED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT},
        {WOTBMOD_V3_EVENT_PROJECTILE_DESTROYED,
         WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING |
             WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT},
        {WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD |
             WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH |
             WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE |
             WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS},
        {WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD |
             WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH |
             WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE |
             WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS},
        {WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD |
             WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS},
        {WOTBMOD_V3_EVENT_RPC_OBSERVED,
         WOTBMOD_V3_EVENT_SOURCE_VEHICLE_ENTER_WORLD |
             WOTBMOD_V3_EVENT_SOURCE_VEHICLE_LEAVE_WORLD |
             WOTBMOD_V3_EVENT_SOURCE_SHOW_SHOOTING |
             WOTBMOD_V3_EVENT_SOURCE_SET_HEALTH |
             WOTBMOD_V3_EVENT_SOURCE_UPDATE_LOCAL_VEHICLE |
             WOTBMOD_V3_EVENT_SOURCE_OBSERVED_STATUS |
             WOTBMOD_V3_EVENT_SOURCE_VEHICLE_HIT},
        {WOTBMOD_V3_EVENT_RENDER_DEVICE_CREATED, 0u},
        {WOTBMOD_V3_EVENT_RENDER_DEVICE_LOST, 0u},
        {WOTBMOD_V3_EVENT_RENDER_DEVICE_RESTORED, 0u},
        {WOTBMOD_V3_EVENT_SWAPCHAIN_RESIZED, 0u},
        {WOTBMOD_V3_EVENT_RENDER_BACKEND_CHANGED, 0u},
        {WOTBMOD_V3_EVENT_VFS_INVALIDATED, 0u},
        {WOTBMOD_V3_EVENT_RESOURCE_INVALIDATED, 0u},
        {WOTBMOD_V3_EVENT_RESOURCE_RELOADED, 0u},
        /*
         * The visible-tracer pair. The source bit means the TracerManager
         * ShowTracer hook actually installed on this client build - nothing
         * else can announce a tracer - so when the hook is absent the bit stays
         * clear and a subscription is refused instead of hanging on a topic
         * that would never fire.
         */
        {WOTBMOD_V3_EVENT_VISIBLE_TRACER_CREATED,
         WOTBMOD_V3_EVENT_SOURCE_VISIBLE_TRACER},
        {WOTBMOD_V3_EVENT_VISIBLE_TRACER_DESTROYED,
         WOTBMOD_V3_EVENT_SOURCE_VISIBLE_TRACER},
    }};
    return topics;
}

bool PrefixesOverlap(
    const std::string& left,
    const char* right_text) {
    const std::string right(right_text);
    const size_t common =
        left.size() < right.size() ? left.size() : right.size();
    return left.compare(0u, common, right, 0u, common) == 0;
}

bool PatternCanMatchModEvent(const std::string& pattern) {
    if (EndsWith(pattern, '*')) {
        return PrefixesOverlap(
            pattern.substr(0u, pattern.size() - 1u),
            "mod.");
    }
    if (!StartsWith(pattern, "mod.")) return false;
    const size_t separator = pattern.find('.', 4u);
    return separator != std::string::npos && separator > 4u;
}

bool PatternCanMatchPublishedSystemEvent(
    const std::string& pattern) {
    const uint64_t source_mask = GetEventSourceMask();
    /* The GES topics are not in the static table: 601 of them, spelled from
     * the client's RTTI at runtime. One source bit covers them all. */
    if (GesPatternCanMatch(pattern.c_str(), source_mask)) return true;
    for (const SystemEventTopic& candidate : SystemEventTopics()) {
        const bool available =
            candidate.required_sources == 0u ||
            (source_mask & candidate.required_sources) != 0u;
        if (available &&
            MatchesPattern(pattern, candidate.topic)) {
            return true;
        }
    }
    return false;
}

bool EventPatternHasAvailableSource(
    const std::string& pattern,
    bool receive_system_events) {
    if (PatternCanMatchModEvent(pattern)) return true;
    return receive_system_events &&
           PatternCanMatchPublishedSystemEvent(pattern);
}

void DeactivateEventSubscription(
    const std::shared_ptr<EventSubscription>& subscription) {
    if (!subscription) return;
    if (subscription->ges_observed.exchange(false)) {
        GesOnPatternUnsubscribed(subscription->pattern.c_str());
    }
    subscription->active.store(false);
    std::lock_guard<std::mutex> lock(subscription->callback_mutex);
    subscription->accepting_callbacks = false;
}

void RemoveEventSubscription(
    const std::shared_ptr<EventSubscription>& subscription) {
    if (!subscription) return;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    g_event_subscriptions.erase(
        std::remove_if(
            g_event_subscriptions.begin(),
            g_event_subscriptions.end(),
            [&](const std::shared_ptr<EventSubscription>& item) {
                return item.get() == subscription.get();
            }),
        g_event_subscriptions.end());
}

void RemoveEventSubscriptionIfDrained(
    const std::shared_ptr<EventSubscription>& subscription) {
    if (!subscription || subscription->active.load()) return;
    {
        std::lock_guard<std::mutex> lock(
            subscription->callback_mutex);
        if (subscription->callbacks_in_flight != 0u) return;
    }
    RemoveEventSubscription(subscription);
}

std::shared_ptr<EventSubscription> FindEventSubscriptionLocked(
    WotbModV3Handle mod,
    WotbModV3EventToken token) {
    const auto found = std::find_if(
        g_event_subscriptions.begin(),
        g_event_subscriptions.end(),
        [&](const std::shared_ptr<EventSubscription>& item) {
            return item && item->owner == mod &&
                   item->handle == token;
        });
    return found == g_event_subscriptions.end()
               ? std::shared_ptr<EventSubscription>()
               : *found;
}

WotbModV3Result InactiveEventSubscriptionResult(
    WotbModV3Handle mod,
    WotbModV3EventToken token) {
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        token,
        WOTBMOD_V3_HANDLE_EVENT_SUBSCRIPTION,
        nullptr,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    return SetError(
        mod,
        WOTBMOD_V3_E_OBJECT_DESTROYED,
        "event subscription is inactive");
}

void WaitForEventSubscriptionCallbacks(
    const std::shared_ptr<EventSubscription>& subscription) {
    if (!subscription) return;
    const size_t current_thread_depth =
        CurrentEventCallbackDepth(subscription.get());
    std::unique_lock<std::mutex> lock(subscription->callback_mutex);
    subscription->callback_cv.wait(
        lock,
        [&]() {
            return subscription->callbacks_in_flight <=
                   current_thread_depth;
        });
}

void QuiesceEventSubscription(
    const std::shared_ptr<EventSubscription>& subscription) {
    DeactivateEventSubscription(subscription);
    WaitForEventSubscriptionCallbacks(subscription);
    RemoveEventSubscriptionIfDrained(subscription);
}

void ShutdownEventSubscriptions() {
    std::vector<std::shared_ptr<EventSubscription>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_event_accepting_subscriptions = false;
        subscriptions = g_event_subscriptions;
        for (const auto& subscription : subscriptions) {
            if (subscription) subscription->active.store(false);
        }
        g_event_subscriptions.clear();
    }
    for (const auto& subscription : subscriptions) {
        DeactivateEventSubscription(subscription);
    }
    for (const auto& subscription : subscriptions) {
        WaitForEventSubscriptionCallbacks(subscription);
    }
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_event_dispatches.clear();
    }
}

void DestroyEventSubscription(void* object) {
    std::unique_ptr<EventSubscriptionHandle> holder(
        static_cast<EventSubscriptionHandle*>(object));
    if (!holder || !holder->subscription) return;
    QuiesceEventSubscription(holder->subscription);
}

WotbModV3Result WOTBMOD_V3_CALL EventsSubscribe(
    WotbModV3Handle mod,
    const WotbModV3EventSubscriptionInfo* info,
    WotbModV3EventCallback callback,
    void* user_data,
    WotbModV3EventToken* out_token) {
    if (!info ||
        info->struct_size < sizeof(WotbModV3EventSubscriptionInfo) ||
        info->api_version != WOTBMOD_V3_EVENTS_VERSION ||
        !HasText(info->topic_pattern, WOTBMOD_V3_MAX_EVENT_TOPIC - 1u) ||
        !callback || !out_token || !ValidPriority(info->priority)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "event subscription arguments are invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    *out_token = WOTBMOD_V3_INVALID_HANDLE;
    const std::string pattern(info->topic_pattern);
    if (!EventPatternHasAvailableSource(
            pattern,
            info->receive_system_events != 0u)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "event subscription pattern has no available publisher");
    }

    std::shared_ptr<EventSubscription> subscription(
        new (std::nothrow) EventSubscription());
    std::unique_ptr<EventSubscriptionHandle> holder(
        new (std::nothrow) EventSubscriptionHandle());
    if (!subscription || !holder) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "event subscription allocation failed");
    }
    subscription->owner = mod;
    subscription->pattern = pattern;
    subscription->priority = info->priority;
    subscription->receive_system = info->receive_system_events != 0u;
    subscription->callback = callback;
    subscription->user_data = user_data;
    subscription->order = g_event_order.fetch_add(1u);
    holder->subscription = subscription;

    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        if (!g_event_accepting_subscriptions) {
            return SetError(
                mod,
                WOTBMOD_V3_E_CANCELLED,
                "event bus is shutting down");
        }
        if (g_event_subscriptions.size() >= kMaxEventSubscriptions) {
            return SetError(
                mod, WOTBMOD_V3_E_LIMIT_REACHED,
                "event subscription limit reached");
        }
    }

    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_EVENT_SUBSCRIPTION,
        holder.get(),
        &DestroyEventSubscription,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    subscription->handle = handle;
    holder.release();

#if defined(WOTBMOD_V3_RUNTIME_SERVICES_STANDALONE_TEST)
    WaitAtEventSubscribePublishBarrierForTesting();
#endif

    WotbModV3Result publish_result = WOTBMOD_V3_OK;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        if (!g_event_accepting_subscriptions) {
            publish_result = WOTBMOD_V3_E_CANCELLED;
        } else if (
            g_event_subscriptions.size() >= kMaxEventSubscriptions) {
            publish_result = WOTBMOD_V3_E_LIMIT_REACHED;
        } else {
            g_event_subscriptions.push_back(subscription);
        }
    }
    if (publish_result != WOTBMOD_V3_OK) {
        ReleaseOwnedHandle(mod, handle);
        return SetError(
            mod,
            publish_result,
            publish_result == WOTBMOD_V3_E_CANCELLED
                ? "event bus shut down before subscription publication"
                : "event subscription limit reached");
    }
    *out_token = handle;
    if (subscription->receive_system) {
        subscription->ges_observed.store(true);
        GesOnPatternSubscribed(pattern.c_str());
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventsUnsubscribe(
    WotbModV3Handle mod,
    WotbModV3EventToken token) {
    std::shared_ptr<EventSubscription> subscription;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        subscription = FindEventSubscriptionLocked(mod, token);
        if (subscription &&
            !subscription->active.exchange(false)) {
            subscription.reset();
        }
    }
    if (!subscription) {
        return InactiveEventSubscriptionResult(mod, token);
    }
    DeactivateEventSubscription(subscription);
    WaitForEventSubscriptionCallbacks(subscription);
    RemoveEventSubscriptionIfDrained(subscription);
    return ReleaseOwnedHandle(mod, token);
}

WotbModV3Result WOTBMOD_V3_CALL EventsSetPriority(
    WotbModV3Handle mod,
    WotbModV3EventToken token,
    int32_t priority) {
    if (!ValidPriority(priority)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "event priority is outside the supported range");
    }
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        const std::shared_ptr<EventSubscription> subscription =
            FindEventSubscriptionLocked(mod, token);
        if (subscription && subscription->active.load()) {
            subscription->priority = priority;
            return WOTBMOD_V3_OK;
        }
    }
    return InactiveEventSubscriptionResult(mod, token);
}

WotbModV3Result PostEventInternal(
    WotbModV3Handle publisher,
    const char* topic_text,
    const void* payload,
    uint32_t payload_size,
    uint32_t flags,
    bool system_event) {
    if (!HasText(topic_text, WOTBMOD_V3_MAX_EVENT_TOPIC - 1u) ||
        (payload_size != 0u && !payload) ||
        payload_size > WOTBMOD_V3_MAX_EVENT_PAYLOAD) {
        return SetError(
            publisher, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "event topic or payload is invalid");
    }
    const std::string topic(topic_text);
    if (!system_event) {
        const WotbModV3Result access = CheckAccess(
            publisher,
            WOTBMOD_V3_PERMISSION_SAFE,
            WOTBMOD_V3_CONTEXT_ALL,
            nullptr);
        if (access != WOTBMOD_V3_OK) return access;
        const char* id = ModId(publisher);
        const std::string required_prefix =
            std::string("mod.") + (id ? id : "") + ".";
        if (StartsWith(topic, "wotbmod.") ||
            topic.compare(0u, required_prefix.size(), required_prefix) != 0) {
            return SetError(
                publisher, WOTBMOD_V3_E_PERMISSION_DENIED,
                "mods may only post topics in mod.<own-id>.*");
        }
    }

    std::vector<uint8_t> payload_copy(payload_size);
    if (payload_size != 0u) {
        std::memcpy(payload_copy.data(), payload, payload_size);
    }
    std::vector<std::shared_ptr<EventSubscription>> listeners;
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        for (const auto& listener : g_event_subscriptions) {
            if (!listener || !listener->active.load()) continue;
            if (system_event && !listener->receive_system) continue;
            if (MatchesPattern(listener->pattern, topic)) {
                listeners.push_back(listener);
            }
        }
        std::stable_sort(
            listeners.begin(),
            listeners.end(),
            [](const std::shared_ptr<EventSubscription>& left,
               const std::shared_ptr<EventSubscription>& right) {
                if (left->priority != right->priority) {
                    return left->priority > right->priority;
                }
                return left->order < right->order;
            });
    }

    std::shared_ptr<EventDispatchControl> control(
        new (std::nothrow) EventDispatchControl());
    if (!control) {
        return SetError(
            publisher, WOTBMOD_V3_E_LIMIT_REACHED,
            "event dispatch allocation failed");
    }
    control->token = g_event_dispatch_token.fetch_add(1u);
    if (control->token == WOTBMOD_V3_INVALID_HANDLE) {
        control->token = g_event_dispatch_token.fetch_add(1u);
    }
    control->timestamp_ns = NowNs();
    control->context_mask = CurrentContext();
    control->thread_role = CurrentThreadRole();
    control->flags = flags |
                     (system_event ? WOTBMOD_V3_EVENT_FLAG_SYSTEM : 0u);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_event_dispatches[control->token] = control;
    }

    WotbModV3Event event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_EVENTS_VERSION;
    event.dispatch_token = control->token;
    event.publisher_mod = publisher;
    event.timestamp_ns = control->timestamp_ns;
    event.context_mask = control->context_mask;
    event.thread_role = control->thread_role;
    event.flags = control->flags;
    CopyText(event.topic, sizeof(event.topic), topic.c_str());
    event.payload = payload_copy.empty() ? nullptr : payload_copy.data();
    event.payload_size = payload_size;

    for (const auto& listener : listeners) {
        EventCallbackScope subscription_scope(listener);
        if (!subscription_scope.entered()) continue;
        {
            std::lock_guard<std::mutex> lock(g_event_mutex);
            if (control->stopped) break;
            control->current_listener = listener->owner;
        }
        const WotbModV3Result callback_entry =
            (event.flags & WOTBMOD_V3_EVENT_FLAG_COALESCIBLE) != 0u
                ? EnterModCallbackCoalescible(listener->owner)
                : EnterModCallback(listener->owner);
        if (callback_entry != WOTBMOD_V3_OK) continue;
        try {
            listener->callback(
                listener->owner, &event, listener->user_data);
        } catch (...) {
            SetError(
                listener->owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "event callback raised an exception");
        }
        LeaveModCallback(listener->owner);
        {
            std::lock_guard<std::mutex> lock(g_event_mutex);
            event.propagation_stopped = control->stopped ? 1u : 0u;
            control->current_listener = WOTBMOD_V3_INVALID_HANDLE;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_event_dispatches.erase(control->token);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventsPost(
    WotbModV3Handle mod,
    const char* topic,
    const void* payload,
    uint32_t payload_size,
    uint32_t flags) {
    if ((flags & WOTBMOD_V3_EVENT_FLAG_SYSTEM) != 0u) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "system event flag is reserved for the runtime");
    }
    return PostEventInternal(
        mod, topic, payload, payload_size, flags, false);
}

WotbModV3Result WOTBMOD_V3_CALL EventsStopPropagation(
    WotbModV3Handle mod,
    WotbModV3Token dispatch_token) {
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    const auto found = g_event_dispatches.find(dispatch_token);
    if (found == g_event_dispatches.end()) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_FOUND,
            "event dispatch is no longer active");
    }
    EventDispatchControl& control = *found->second;
    if (control.current_listener != mod) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "only the currently executing listener may stop propagation");
    }
    if ((control.flags & WOTBMOD_V3_EVENT_FLAG_STOPPABLE) == 0u) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_SUPPORTED,
            "this event does not allow propagation to be stopped");
    }
    control.stopped = true;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventsGetDispatchInfo(
    WotbModV3Handle mod,
    WotbModV3Token dispatch_token,
    WotbModV3EventDispatchInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3EventDispatchInfo) ||
        out_info->api_version != WOTBMOD_V3_EVENTS_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "dispatch info has an invalid structure header");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    const auto found = g_event_dispatches.find(dispatch_token);
    if (found == g_event_dispatches.end()) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_FOUND,
            "event dispatch is no longer active");
    }
    const EventDispatchControl& control = *found->second;
    const uint32_t size = out_info->struct_size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = size;
    out_info->api_version = WOTBMOD_V3_EVENTS_VERSION;
    out_info->dispatch_token = control.token;
    out_info->timestamp_ns = control.timestamp_ns;
    out_info->context_mask = control.context_mask;
    out_info->thread_role = control.thread_role;
    out_info->flags = control.flags;
    out_info->propagation_stopped = control.stopped ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetDispatchControl(
    WotbModV3Handle mod,
    WotbModV3Token dispatch_token,
    std::shared_ptr<EventDispatchControl>* out_control) {
    if (!out_control) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "dispatch output is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_event_mutex);
    const auto found = g_event_dispatches.find(dispatch_token);
    if (found == g_event_dispatches.end()) {
        return SetError(
            mod, WOTBMOD_V3_E_NOT_FOUND,
            "event dispatch is no longer active");
    }
    *out_control = found->second;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventsGetThread(
    WotbModV3Handle mod,
    WotbModV3Token dispatch_token,
    uint32_t* out_thread_role) {
    if (!out_thread_role) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "thread role output is required");
    }
    std::shared_ptr<EventDispatchControl> control;
    const WotbModV3Result result =
        GetDispatchControl(mod, dispatch_token, &control);
    if (result != WOTBMOD_V3_OK) return result;
    *out_thread_role = control->thread_role;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventsGetTimestamp(
    WotbModV3Handle mod,
    WotbModV3Token dispatch_token,
    uint64_t* out_timestamp_ns) {
    if (!out_timestamp_ns) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "timestamp output is required");
    }
    std::shared_ptr<EventDispatchControl> control;
    const WotbModV3Result result =
        GetDispatchControl(mod, dispatch_token, &control);
    if (result != WOTBMOD_V3_OK) return result;
    *out_timestamp_ns = control->timestamp_ns;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL EventsGetContext(
    WotbModV3Handle mod,
    WotbModV3Token dispatch_token,
    uint64_t* out_context_mask) {
    if (!out_context_mask) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "context output is required");
    }
    std::shared_ptr<EventDispatchControl> control;
    const WotbModV3Result result =
        GetDispatchControl(mod, dispatch_token, &control);
    if (result != WOTBMOD_V3_OK) return result;
    *out_context_mask = control->context_mask;
    return WOTBMOD_V3_OK;
}

/* ------------------------------------------------------------------------- */
/* Hook broker metadata and conflict policy                                  */
/* ------------------------------------------------------------------------- */

struct HookRecord {
    WotbModV3HookHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    std::string target;
    void* target_address = nullptr;
    void* detour = nullptr;
    void* original = nullptr;
    uint64_t native_token = 0u;   // observer attachment (OBSERVE/AFTER)
    uint32_t mode = WOTBMOD_V3_HOOK_OBSERVE;
    uint32_t status = WOTBMOD_V3_HOOK_STATUS_PENDING_BACKEND;
    int32_t priority = 0;
    uint32_t flags = 0u;
    uint64_t order = 0u;
    std::vector<WotbModV3HookHandle> conflicts;
    bool native_created = false;
    bool native_enabled = false;
    std::atomic<bool> active{true};
};

struct HookHandleObject {
    std::shared_ptr<HookRecord> hook;
};

std::mutex g_hook_mutex;
std::vector<std::shared_ptr<HookRecord>> g_hooks;
std::atomic<uint64_t> g_hook_order{1u};
WotbModV3NativeHookBackend g_native_hook_backend = {};

bool NativeHookBackendReady() {
    return (g_native_hook_backend.api_version == 1u ||
            g_native_hook_backend.api_version == 2u) &&
           g_native_hook_backend.create &&
           g_native_hook_backend.enable &&
           g_native_hook_backend.disable &&
           g_native_hook_backend.remove;
}

// Version 2 adds the observer slots; SetNativeHookBackend rejects a version
// 2 table that leaves any of them null, so one check here is enough.
bool NativeHookBackendV2() {
    return NativeHookBackendReady() &&
           g_native_hook_backend.api_version >= 2u &&
           g_native_hook_backend.describe_target != nullptr;
}

// OBSERVE and AFTER never own a trampoline: they are callbacks the loader
// runs from inside its own detour on the target.
bool ObserverMode(uint32_t mode) {
    return mode == WOTBMOD_V3_HOOK_OBSERVE ||
           mode == WOTBMOD_V3_HOOK_AFTER;
}

uint32_t DescribedModes(void* target) {
    if (!target || !NativeHookBackendV2()) return 0u;
    uint32_t mask = 0u;
    const WotbModV3Result result =
        g_native_hook_backend.describe_target(
            g_native_hook_backend.user_data, target, &mask);
    return result == WOTBMOD_V3_OK ? mask : 0u;
}

// A function of the pair (mode, target): AROUND/REPLACE are the raw
// trampoline and work anywhere the backend can hook; OBSERVE/AFTER only
// where the loader describes an observer point; BEFORE nowhere yet (no
// cancel protocol).
bool NativeModeSupported(uint32_t mode, void* target) {
    if (mode == WOTBMOD_V3_HOOK_AROUND ||
        mode == WOTBMOD_V3_HOOK_REPLACE) {
        return true;
    }
    if (!ObserverMode(mode)) return false;
    return (DescribedModes(target) & (1u << mode)) != 0u;
}

bool IsReadableMemory(const void* address, size_t bytes) {
    if (!address || bytes == 0u) return false;
    MEMORY_BASIC_INFORMATION information = {};
    if (VirtualQuery(
            address,
            &information,
            sizeof(information)) != sizeof(information)) {
        return false;
    }
    const DWORD protection =
        information.Protect & 0xFFu;
    if (information.State != MEM_COMMIT ||
        protection == PAGE_NOACCESS ||
        (information.Protect & PAGE_GUARD) != 0u) {
        return false;
    }
    const uintptr_t begin =
        reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = begin + bytes;
    const uintptr_t region_end =
        reinterpret_cast<uintptr_t>(information.BaseAddress) +
        information.RegionSize;
    return end >= begin && end <= region_end;
}

bool ResolveVtableTarget(
    void* object,
    uint32_t slot,
    void** out_target) {
    if (!out_target) return false;
    *out_target = nullptr;
    if (!IsReadableMemory(object, sizeof(void*))) return false;
    void** vtable = *static_cast<void***>(object);
    if (!IsReadableMemory(
            vtable + slot,
            sizeof(void*))) {
        return false;
    }
    void* target = vtable[slot];
    MEMORY_BASIC_INFORMATION information = {};
    if (!target ||
        VirtualQuery(
            target,
            &information,
            sizeof(information)) != sizeof(information) ||
        information.State != MEM_COMMIT) {
        return false;
    }
    const DWORD protection = information.Protect & 0xFFu;
    const bool executable =
        protection == PAGE_EXECUTE ||
        protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
    if (!executable) return false;
    *out_target = target;
    return true;
}

// Targets a mod may only hook with a gameplay-tweak grant.
//
// Only the canonical spelling of each one is listed. The binding pack
// publishes several of its 35 anchors under more than one name -- for
// instance "Camera::setFOV" and "DAVA::Camera::SetFovY" are both
// kGameCameraSetFovRva, one function -- and classifying by string would price
// the two spellings differently, so a mod would simply name the cheap one.
// FindManagedHookTarget below therefore falls back to comparing the address a
// symbol resolves to, which makes "same function, same price" hold by
// construction: publishing a new alias in the binding pack requires no edit
// here, and there is no second list to keep in sync.
struct ManagedHookTarget {
    const char* canonical_symbol;
    const char* permission;
};

const ManagedHookTarget kManagedHookTargets[] = {
    {"Camera::setFOV", "gameplay.tweak.camera"},
    {"SniperCamera::exit", "gameplay.tweak.camera"},
    {"PostProcess::apply", "gameplay.tweak.camera"},
    {"Minimap::render", "gameplay.tweak.hud"},
    {"Reticle::draw", "gameplay.tweak.hud"},
    {"SixthSense::activate", "gameplay.tweak.hud"},
    {"DamageLog::addEntry", "gameplay.tweak.hud"}};

// The address the reviewed binding pack maps this name to, or nullptr when
// there is no backend, the name is not published, or the client fingerprint
// does not match. Pure: the backend only reads its own table.
void* ResolveReviewedSymbol(const char* symbol) {
    if (!symbol || !NativeHookBackendReady() ||
        !g_native_hook_backend.resolve_symbol) {
        return nullptr;
    }
    void* address = nullptr;
    if (g_native_hook_backend.resolve_symbol(
            g_native_hook_backend.user_data,
            symbol,
            &address) != WOTBMOD_V3_OK) {
        return nullptr;
    }
    return address;
}

const ManagedHookTarget* FindManagedHookTarget(
    const std::string& symbol) {
    // Canonical spelling first, so a managed target keeps its price even
    // when no backend is present to resolve anything.
    for (const ManagedHookTarget& target : kManagedHookTargets) {
        if (symbol == target.canonical_symbol) return &target;
    }
    const void* const address = ResolveReviewedSymbol(symbol.c_str());
    if (!address) return nullptr;
    for (const ManagedHookTarget& target : kManagedHookTargets) {
        if (ResolveReviewedSymbol(target.canonical_symbol) == address) {
            return &target;
        }
    }
    return nullptr;
}

uint32_t RequiredSymbolHookTier(const ManagedHookTarget* managed) {
    return managed != nullptr
               ? WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK
               : WOTBMOD_V3_PERMISSION_REVIEWED;
}

const char* RequiredSymbolHookPermission(
    const ManagedHookTarget* managed) {
    return managed != nullptr ? managed->permission : "hooks.symbol";
}

bool HookModesConflict(uint32_t left, uint32_t right) {
    return left == WOTBMOD_V3_HOOK_REPLACE ||
           right == WOTBMOD_V3_HOOK_REPLACE;
}

void DestroyHook(void* object) {
    std::unique_ptr<HookHandleObject> holder(
        static_cast<HookHandleObject*>(object));
    if (!holder || !holder->hook) return;
    if (holder->hook->native_created && NativeHookBackendReady() &&
        ObserverMode(holder->hook->mode)) {
        const WotbModV3Result detached = NativeHookBackendV2()
            ? g_native_hook_backend.detach(
                  g_native_hook_backend.user_data,
                  holder->hook->native_token)
            : WOTBMOD_V3_E_NOT_SUPPORTED;
        if (detached != WOTBMOD_V3_OK) {
            RuntimeLog(
                WOTBMOD_V3_LOG_ERROR,
                "v3.hooks",
                "native observer detach failed during owned cleanup");
        }
        holder->hook->native_enabled = false;
        holder->hook->native_created = false;
        holder->hook->native_token = 0u;
    } else if (holder->hook->native_created && NativeHookBackendReady()) {
        if (holder->hook->native_enabled) {
            const WotbModV3Result disabled =
                g_native_hook_backend.disable(
                    g_native_hook_backend.user_data,
                    holder->hook->target_address);
            if (disabled != WOTBMOD_V3_OK) {
                RuntimeLog(
                    WOTBMOD_V3_LOG_ERROR,
                    "v3.hooks",
                    "native hook disable failed during owned cleanup");
            }
        }
        const WotbModV3Result removed =
            g_native_hook_backend.remove(
                g_native_hook_backend.user_data,
                holder->hook->target_address);
        if (removed != WOTBMOD_V3_OK) {
            RuntimeLog(
                WOTBMOD_V3_LOG_ERROR,
                "v3.hooks",
                "native hook removal failed during owned cleanup");
        }
        holder->hook->native_enabled = false;
        holder->hook->native_created = false;
        holder->hook->original = nullptr;
    }
    holder->hook->active.store(false);
    holder->hook->status = WOTBMOD_V3_HOOK_STATUS_REMOVED;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_hooks.erase(
        std::remove_if(
            g_hooks.begin(),
            g_hooks.end(),
            [&](const std::shared_ptr<HookRecord>& item) {
                return item.get() == holder->hook.get();
            }),
        g_hooks.end());
    for (const auto& hook : g_hooks) {
        if (!hook) continue;
        hook->conflicts.erase(
            std::remove(
                hook->conflicts.begin(),
                hook->conflicts.end(),
                holder->hook->handle),
            hook->conflicts.end());
    }
}

std::shared_ptr<HookRecord> FindHookLocked(WotbModV3HookHandle handle) {
    for (const auto& hook : g_hooks) {
        if (hook && hook->active.load() && hook->handle == handle) {
            return hook;
        }
    }
    return std::shared_ptr<HookRecord>();
}

WotbModV3Result InspectOwnedHook(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook,
    std::shared_ptr<HookRecord>* out_hook) {
    if (!out_hook) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_hook is required");
    }
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        hook,
        WOTBMOD_V3_HANDLE_HOOK,
        &object,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    HookHandleObject* holder = static_cast<HookHandleObject*>(object);
    if (!holder->hook || !holder->hook->active.load()) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "hook registration is destroyed");
    }
    *out_hook = holder->hook;
    return WOTBMOD_V3_OK;
}

void FillHookInfo(
    const HookRecord& hook,
    WotbModV3HookInfo* out_info) {
    const uint32_t size = out_info->struct_size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = size;
    out_info->api_version = WOTBMOD_V3_HOOKS_VERSION;
    out_info->hook = hook.handle;
    out_info->owner_mod = hook.owner;
    out_info->creation_order = hook.order;
    out_info->mode = hook.mode;
    out_info->status = hook.status;
    out_info->priority = hook.priority;
    out_info->conflict_count =
        static_cast<uint32_t>(hook.conflicts.size());
    CopyText(
        out_info->target,
        sizeof(out_info->target),
        hook.target.c_str());
}

WotbModV3Result CreatePendingHook(
    WotbModV3Handle mod,
    const std::string& target,
    void* target_address,
    const WotbModV3HookCreateInfo* info,
    WotbModV3HookHandle* out_hook) {
    if (!info ||
        info->struct_size < sizeof(WotbModV3HookCreateInfo) ||
        info->api_version != WOTBMOD_V3_HOOKS_VERSION ||
        info->mode > WOTBMOD_V3_HOOK_OBSERVE ||
        !ValidPriority(info->priority) || !info->detour || !out_hook) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "hook create arguments are invalid");
    }
    *out_hook = WOTBMOD_V3_INVALID_HANDLE;

    const bool backend_ready = NativeHookBackendReady();
    WotbModV3Result resolve_result = WOTBMOD_V3_E_NOT_SUPPORTED;
    if (!target_address &&
        backend_ready &&
        g_native_hook_backend.resolve_symbol) {
        resolve_result =
            g_native_hook_backend.resolve_symbol(
                g_native_hook_backend.user_data,
                target.c_str(),
                &target_address);
        if (resolve_result == WOTBMOD_V3_OK && !target_address) {
            resolve_result = WOTBMOD_V3_E_PLATFORM;
        }
        if (resolve_result != WOTBMOD_V3_OK &&
            resolve_result != WOTBMOD_V3_E_NOT_FOUND &&
            resolve_result != WOTBMOD_V3_E_NOT_SUPPORTED) {
            return SetError(
                mod,
                resolve_result,
                "native hook symbol resolution failed");
        }
    }
    const bool native_mode_supported =
        NativeModeSupported(info->mode, target_address);
    const bool can_create_native =
        backend_ready &&
        target_address &&
        native_mode_supported;
    if (!can_create_native &&
        (info->flags & WOTBMOD_V3_HOOK_CREATE_ALLOW_PENDING) == 0u) {
        const char* reason = !backend_ready
            ? "native hook backend is unavailable"
            : !target_address
                ? "symbol is not present in the reviewed binding pack"
                : info->mode == WOTBMOD_V3_HOOK_BEFORE
                    ? "BEFORE has no cancel protocol yet"
                    : !NativeHookBackendV2()
                        ? "native backend supports only AROUND and REPLACE modes"
                        : "signature of this target is not described; "
                          "OBSERVE and AFTER need a loader-side observer point";
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            reason);
    }

    std::shared_ptr<HookRecord> hook(new (std::nothrow) HookRecord());
    std::unique_ptr<HookHandleObject> holder(
        new (std::nothrow) HookHandleObject());
    if (!hook || !holder) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "hook registration allocation failed");
    }
    hook->owner = mod;
    hook->target = target;
    hook->target_address = target_address;
    hook->detour = info->detour;
    hook->mode = info->mode;
    hook->priority = info->priority;
    hook->flags = info->flags;
    hook->order = g_hook_order.fetch_add(1u);
    holder->hook = hook;

    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_HOOK,
        holder.get(),
        &DestroyHook,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    hook->handle = handle;
    holder.release();

    WotbModV3Result native_result = WOTBMOD_V3_OK;
    bool rejected_conflict = false;
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        if (g_hooks.size() >= kMaxHooks) {
            native_result = WOTBMOD_V3_E_LIMIT_REACHED;
        }
        if (native_result == WOTBMOD_V3_OK) {
            for (const auto& existing : g_hooks) {
                if (!existing || !existing->active.load()) continue;
                const bool same_resolved_target =
                    target_address &&
                    existing->target_address == target_address;
                const bool metadata_conflict =
                    existing->target == target &&
                    HookModesConflict(existing->mode, hook->mode);
                // Observers never own the trampoline, so they neither take
                // a native target nor block one.
                const bool native_conflict =
                    same_resolved_target &&
                    existing->native_created &&
                    native_mode_supported &&
                    !ObserverMode(existing->mode) &&
                    !ObserverMode(hook->mode);
                if (!metadata_conflict && !native_conflict) continue;
                hook->conflicts.push_back(existing->handle);
                if ((info->flags &
                     WOTBMOD_V3_HOOK_CREATE_ALLOW_CONFLICT) == 0u) {
                    rejected_conflict = true;
                    break;
                }
            }
        }
        if (native_result == WOTBMOD_V3_OK &&
            !rejected_conflict &&
            hook->conflicts.empty() &&
            can_create_native) {
            if (ObserverMode(hook->mode)) {
                native_result =
                    g_native_hook_backend.attach(
                        g_native_hook_backend.user_data,
                        hook->target_address,
                        hook->mode,
                        hook->priority,
                        hook->detour,
                        &hook->native_token);
                if (native_result == WOTBMOD_V3_OK &&
                    hook->native_token == 0u) {
                    native_result = WOTBMOD_V3_E_PLATFORM;
                }
                if (native_result == WOTBMOD_V3_OK) {
                    hook->native_created = true;
                    hook->status = WOTBMOD_V3_HOOK_STATUS_DISABLED;
                }
            } else {
            native_result =
                g_native_hook_backend.create(
                    g_native_hook_backend.user_data,
                    hook->target_address,
                    hook->detour,
                    &hook->original);
            }
            if (ObserverMode(hook->mode)) {
                // handled above
            } else if (native_result == WOTBMOD_V3_OK &&
                hook->original) {
                hook->native_created = true;
                hook->status = WOTBMOD_V3_HOOK_STATUS_DISABLED;
            } else if (native_result == WOTBMOD_V3_OK) {
                g_native_hook_backend.remove(
                    g_native_hook_backend.user_data,
                    hook->target_address);
                native_result = WOTBMOD_V3_E_PLATFORM;
            }
        }
        if (native_result == WOTBMOD_V3_OK && !rejected_conflict) {
            for (WotbModV3HookHandle conflict : hook->conflicts) {
                const auto existing = FindHookLocked(conflict);
                if (existing) existing->conflicts.push_back(handle);
            }
            if (!hook->conflicts.empty()) {
                hook->status = WOTBMOD_V3_HOOK_STATUS_CONFLICT;
            }
            g_hooks.push_back(hook);
        }
    }
    if (rejected_conflict || native_result != WOTBMOD_V3_OK) {
        ReleaseOwnedHandle(mod, handle);
        return SetError(
            mod,
            rejected_conflict
                ? WOTBMOD_V3_E_CONFLICT
                : native_result,
            rejected_conflict
                ? "native target conflicts with an existing hook"
                : "native hook creation failed");
    }

    *out_hook = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HooksCreateSymbol(
    WotbModV3Handle mod,
    const char* symbol,
    const WotbModV3HookCreateInfo* info,
    WotbModV3HookHandle* out_hook) {
    if (!HasText(symbol, WOTBMOD_V3_MAX_HOOK_TARGET - 1u)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "symbol is empty or too long");
    }
    const std::string target(symbol);
    // Classified once, by resolved address, so every spelling of one
    // function is charged the same tier and the same named grant.
    const ManagedHookTarget* const managed =
        FindManagedHookTarget(target);
    const uint32_t tier = RequiredSymbolHookTier(managed);
    const WotbModV3Result access = CheckAccess(
        mod,
        tier,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    const WotbModV3Result named_permission = CheckNamedPermission(
        mod,
        RequiredSymbolHookPermission(managed),
        tier);
    if (named_permission != WOTBMOD_V3_OK) {
        return named_permission;
    }
    return CreatePendingHook(mod, target, nullptr, info, out_hook);
}

WotbModV3Result WOTBMOD_V3_CALL HooksCreateVtable(
    WotbModV3Handle mod,
    void* object,
    uint32_t slot,
    const WotbModV3HookCreateInfo* info,
    WotbModV3HookHandle* out_hook) {
    if (!object || slot >= 4096u) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "vtable object or slot is invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_UNSAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    const WotbModV3Result named_permission = CheckNamedPermission(
        mod,
        "native.hook.address",
        WOTBMOD_V3_PERMISSION_UNSAFE);
    if (named_permission != WOTBMOD_V3_OK) {
        return named_permission;
    }
    void* target_address = nullptr;
    if (!ResolveVtableTarget(object, slot, &target_address)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "vtable object, slot, or executable target is invalid");
    }
    char target[WOTBMOD_V3_MAX_HOOK_TARGET] = {};
#if defined(_MSC_VER)
    sprintf_s(target, "vtable:%p:%u", target_address, slot);
#else
    std::snprintf(
        target,
        sizeof(target),
        "vtable:%p:%u",
        target_address,
        slot);
#endif
    return CreatePendingHook(
        mod,
        target,
        target_address,
        info,
        out_hook);
}

WotbModV3Result WOTBMOD_V3_CALL HooksEnable(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle) {
    std::shared_ptr<HookRecord> hook;
    const WotbModV3Result inspect =
        InspectOwnedHook(mod, hook_handle, &hook);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (hook->status == WOTBMOD_V3_HOOK_STATUS_ENABLED) {
        return WOTBMOD_V3_OK;
    }
    if (hook->status == WOTBMOD_V3_HOOK_STATUS_CONFLICT) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "hook has an unresolved native target conflict");
    }
    if (!hook->native_created || !NativeHookBackendReady()) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "pending hook has no installed native backend target");
    }
    const WotbModV3Result result = ObserverMode(hook->mode)
        ? (NativeHookBackendV2()
               ? g_native_hook_backend.set_attached_enabled(
                     g_native_hook_backend.user_data,
                     hook->native_token,
                     1u)
               : WOTBMOD_V3_E_NOT_SUPPORTED)
        : g_native_hook_backend.enable(
              g_native_hook_backend.user_data,
              hook->target_address);
    if (result != WOTBMOD_V3_OK) {
        hook->status = WOTBMOD_V3_HOOK_STATUS_FAILED;
        return SetError(
            mod,
            result,
            "native hook enable failed");
    }
    hook->native_enabled = true;
    hook->status = WOTBMOD_V3_HOOK_STATUS_ENABLED;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HooksDisable(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle) {
    std::shared_ptr<HookRecord> hook;
    const WotbModV3Result inspect =
        InspectOwnedHook(mod, hook_handle, &hook);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (hook->status == WOTBMOD_V3_HOOK_STATUS_ENABLED) {
        if (!hook->native_created || !NativeHookBackendReady()) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PLATFORM,
                "enabled hook lost its native backend");
        }
        const WotbModV3Result result = ObserverMode(hook->mode)
            ? (NativeHookBackendV2()
                   ? g_native_hook_backend.set_attached_enabled(
                         g_native_hook_backend.user_data,
                         hook->native_token,
                         0u)
                   : WOTBMOD_V3_E_NOT_SUPPORTED)
            : g_native_hook_backend.disable(
                  g_native_hook_backend.user_data,
                  hook->target_address);
        if (result != WOTBMOD_V3_OK) {
            hook->status = WOTBMOD_V3_HOOK_STATUS_FAILED;
            return SetError(
                mod,
                result,
                "native hook disable failed");
        }
        hook->native_enabled = false;
        hook->status = WOTBMOD_V3_HOOK_STATUS_DISABLED;
        return WOTBMOD_V3_OK;
    }
    if (hook->status == WOTBMOD_V3_HOOK_STATUS_DISABLED) {
        return WOTBMOD_V3_OK;
    }
    return SetError(
        mod,
        hook->status == WOTBMOD_V3_HOOK_STATUS_CONFLICT
            ? WOTBMOD_V3_E_CONFLICT
            : WOTBMOD_V3_E_NOT_SUPPORTED,
        hook->status == WOTBMOD_V3_HOOK_STATUS_CONFLICT
            ? "hook has an unresolved native target conflict"
            : "pending hook has no installed native backend target");
}

WotbModV3Result WOTBMOD_V3_CALL HooksRemove(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook) {
    std::shared_ptr<HookRecord> owned;
    const WotbModV3Result inspect = InspectOwnedHook(mod, hook, &owned);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    return ReleaseOwnedHandle(mod, hook);
}

WotbModV3Result WOTBMOD_V3_CALL HooksSetPriority(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    int32_t priority) {
    if (!ValidPriority(priority)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "hook priority is outside the supported range");
    }
    std::shared_ptr<HookRecord> hook;
    const WotbModV3Result inspect =
        InspectOwnedHook(mod, hook_handle, &hook);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "native hook chain priority mutation is unavailable");
}

WotbModV3Result SetHookRelationship(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    WotbModV3HookHandle other_handle,
    bool before) {
    if (hook_handle == other_handle) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "a hook cannot be ordered relative to itself");
    }
    std::shared_ptr<HookRecord> hook;
    const WotbModV3Result inspect =
        InspectOwnedHook(mod, hook_handle, &hook);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    const std::shared_ptr<HookRecord> other = FindHookLocked(other_handle);
    if (!other) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_HANDLE,
            "related hook does not exist");
    }
    if (hook->target != other->target) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "hook ordering can only relate hooks in the same target chain");
    }
    (void)before;
    return SetError(
        mod,
        WOTBMOD_V3_E_NOT_SUPPORTED,
        "native hook chain ordering is unavailable");
}

WotbModV3Result WOTBMOD_V3_CALL HooksRunBefore(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook,
    WotbModV3HookHandle other_hook) {
    return SetHookRelationship(mod, hook, other_hook, true);
}

WotbModV3Result WOTBMOD_V3_CALL HooksRunAfter(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook,
    WotbModV3HookHandle other_hook) {
    return SetHookRelationship(mod, hook, other_hook, false);
}

WotbModV3Result WOTBMOD_V3_CALL HooksGetOriginal(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    void** out_original) {
    if (!out_original) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_original is required");
    }
    *out_original = nullptr;
    std::shared_ptr<HookRecord> hook;
    const WotbModV3Result inspect =
        InspectOwnedHook(mod, hook_handle, &hook);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (ObserverMode(hook->mode)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "observer hooks do not own a trampoline");
    }
    if (!hook->native_created || !hook->original) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "original function is unavailable for a pending metadata hook");
    }
    *out_original = hook->original;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HooksCallNext(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    const WotbModV3ConstBuffer*,
    WotbModV3Buffer*) {
    std::shared_ptr<HookRecord> hook;
    const WotbModV3Result inspect =
        InspectOwnedHook(mod, hook_handle, &hook);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    return SetError(
        mod, WOTBMOD_V3_E_NOT_SUPPORTED,
        "call_next is unavailable because no typed native hook chain "
        "continuation dispatcher is implemented");
}

WotbModV3Result WOTBMOD_V3_CALL HooksGetInfo(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    WotbModV3HookInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3HookInfo) ||
        out_info->api_version != WOTBMOD_V3_HOOKS_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "hook info has an invalid structure header");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    const std::shared_ptr<HookRecord> hook = FindHookLocked(hook_handle);
    if (!hook) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_HANDLE,
            "hook does not exist");
    }
    FillHookInfo(*hook, out_info);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HooksGetOwner(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    WotbModV3Handle* out_owner) {
    if (!out_owner) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "hook owner output is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    const std::shared_ptr<HookRecord> hook = FindHookLocked(hook_handle);
    if (!hook) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_HANDLE,
            "hook does not exist");
    }
    *out_owner = hook->owner;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HooksGetStatus(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    uint32_t* out_status) {
    if (!out_status) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "hook status output is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    const std::shared_ptr<HookRecord> hook = FindHookLocked(hook_handle);
    if (!hook) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_HANDLE,
            "hook does not exist");
    }
    *out_status = hook->status;
    return WOTBMOD_V3_OK;
}

std::vector<std::shared_ptr<HookRecord>> OrderedHookChainLocked(
    const char* target) {
    std::vector<std::shared_ptr<HookRecord>> chain;
    for (const auto& hook : g_hooks) {
        if (hook && hook->active.load() && hook->target == target) {
            chain.push_back(hook);
        }
    }
    std::stable_sort(
        chain.begin(),
        chain.end(),
        [](const std::shared_ptr<HookRecord>& left,
           const std::shared_ptr<HookRecord>& right) {
            if (left->priority != right->priority) {
                return left->priority > right->priority;
            }
            return left->order < right->order;
        });
    return chain;
}

WotbModV3Result CopyHookList(
    WotbModV3Handle mod,
    const std::vector<std::shared_ptr<HookRecord>>& source,
    WotbModV3HookInfo* hooks,
    uint32_t* inout_count) {
    if (!inout_count) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inout_count is required");
    }
    const uint32_t required = static_cast<uint32_t>(source.size());
    if (!hooks || *inout_count < required) {
        *inout_count = required;
        return required == 0u
                   ? WOTBMOD_V3_OK
                   : WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    for (uint32_t index = 0u; index < required; ++index) {
        if (hooks[index].struct_size < sizeof(WotbModV3HookInfo) ||
            hooks[index].api_version != WOTBMOD_V3_HOOKS_VERSION) {
            return SetError(
                mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                "hook output entry has an invalid structure header");
        }
        FillHookInfo(*source[index], &hooks[index]);
    }
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HooksGetChain(
    WotbModV3Handle mod,
    const char* target,
    WotbModV3HookInfo* hooks,
    uint32_t* inout_count) {
    if (!HasText(target, WOTBMOD_V3_MAX_HOOK_TARGET - 1u)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "target is empty or too long");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    return CopyHookList(
        mod, OrderedHookChainLocked(target), hooks, inout_count);
}

WotbModV3Result WOTBMOD_V3_CALL HooksGetConflicts(
    WotbModV3Handle mod,
    WotbModV3HookHandle hook_handle,
    WotbModV3HookConflictInfo* conflicts,
    uint32_t* inout_count) {
    if (!inout_count) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inout_count is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    const std::shared_ptr<HookRecord> hook = FindHookLocked(hook_handle);
    if (!hook) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_HANDLE,
            "hook does not exist");
    }
    const uint32_t required =
        static_cast<uint32_t>(hook->conflicts.size());
    if (!conflicts || *inout_count < required) {
        *inout_count = required;
        return required == 0u
                   ? WOTBMOD_V3_OK
                   : WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    for (uint32_t index = 0u; index < required; ++index) {
        if (conflicts[index].struct_size <
                sizeof(WotbModV3HookConflictInfo) ||
            conflicts[index].api_version != WOTBMOD_V3_HOOKS_VERSION) {
            return SetError(
                mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                "conflict output entry has an invalid structure header");
        }
        const std::shared_ptr<HookRecord> other =
            FindHookLocked(hook->conflicts[index]);
        const uint32_t size = conflicts[index].struct_size;
        std::memset(&conflicts[index], 0, sizeof(conflicts[index]));
        conflicts[index].struct_size = size;
        conflicts[index].api_version = WOTBMOD_V3_HOOKS_VERSION;
        conflicts[index].hook = hook_handle;
        conflicts[index].conflicting_hook = hook->conflicts[index];
        if (other) {
            conflicts[index].conflicting_owner = other->owner;
            conflicts[index].conflicting_mode = other->mode;
        }
        CopyText(
            conflicts[index].reason,
            sizeof(conflicts[index].reason),
            "REPLACE interception overlaps another hook in the target chain");
    }
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HooksEnumerate(
    WotbModV3Handle mod,
    WotbModV3HookInfo* hooks,
    uint32_t* inout_count) {
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    std::vector<std::shared_ptr<HookRecord>> copy;
    for (const auto& hook : g_hooks) {
        if (hook && hook->active.load()) copy.push_back(hook);
    }
    std::stable_sort(
        copy.begin(),
        copy.end(),
        [](const std::shared_ptr<HookRecord>& left,
           const std::shared_ptr<HookRecord>& right) {
            if (left->target != right->target) {
                return left->target < right->target;
            }
            return left->order < right->order;
        });
    return CopyHookList(mod, copy, hooks, inout_count);
}

WotbModV3Result WOTBMOD_V3_CALL UnsafeCreateAddressHook(
    WotbModV3Handle mod,
    void* target,
    const WotbModV3HookCreateInfo* info,
    WotbModV3HookHandle* out_hook) {
    if (out_hook) *out_hook = WOTBMOD_V3_INVALID_HANDLE;
    if (!target || !info || !out_hook) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "raw address hook arguments are invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_UNSAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        "native.hook.address");
    if (access != WOTBMOD_V3_OK) return access;
    const WotbModV3Result named_permission = CheckNamedPermission(
        mod,
        "native.hook.address",
        WOTBMOD_V3_PERMISSION_UNSAFE);
    if (named_permission != WOTBMOD_V3_OK) {
        return named_permission;
    }
    char target_name[WOTBMOD_V3_MAX_HOOK_TARGET] = {};
#if defined(_MSC_VER)
    sprintf_s(target_name, "address:%p", target);
#else
    std::snprintf(
        target_name,
        sizeof(target_name),
        "address:%p",
        target);
#endif
    return CreatePendingHook(
        mod,
        target_name,
        target,
        info,
        out_hook);
}

/* ------------------------------------------------------------------------- */
/* Shared async runtime, role dispatch, and timers                           */
/* ------------------------------------------------------------------------- */

struct TaskState {
    std::mutex mutex;
    std::condition_variable finished_cv;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3TaskHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    std::string description;
    WotbModV3TaskWorkCallback work = nullptr;
    WotbModV3TaskCompletionCallback completion = nullptr;
    WotbModV3DispatchCallback dispatch = nullptr;
    void* user_data = nullptr;
    uint32_t completion_role = WOTBMOD_V3_THREAD_MAIN;
    uint32_t state = WOTBMOD_V3_TASK_QUEUED;
    WotbModV3Result result_code = WOTBMOD_V3_OK;
    float progress = 0.0f;
    bool cancellation_requested = false;
    bool running = false;
    bool completion_running = false;
    std::thread::id execution_thread;
    std::vector<uint8_t> result;
};

struct TaskHandleObject {
    std::shared_ptr<TaskState> task;
};

struct TimerState {
    std::mutex mutex;
    std::condition_variable finished_cv;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3TimerHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3TimerCallback callback = nullptr;
    void* user_data = nullptr;
    uint64_t interval_ms = 0u;
    uint64_t fire_count = 0u;
    uint64_t paused_remaining_ms = 0u;
    uint32_t callback_role = WOTBMOD_V3_THREAD_MAIN;
    bool repeating = false;
    bool paused = false;
    bool cancelled = false;
    bool armed = true;
    uint32_t queued_callbacks = 0u;
    uint32_t active_callbacks = 0u;
    std::thread::id execution_thread;
    Clock::time_point next_fire;
};

struct TimerHandleObject {
    std::shared_ptr<TimerState> timer;
};

struct QueuedCall {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    std::function<void()> invoke;
    std::function<void()> cancel;
};

enum class BlockingMainCallPhase {
    QUEUED,
    RUNNING,
    COMPLETED,
    CANCELLED
};

struct BlockingMainCallState {
    std::mutex mutex;
    std::condition_variable cv;
    MainThreadCallFn callback = nullptr;
    void* user_data = nullptr;
    BlockingMainCallPhase phase = BlockingMainCallPhase::QUEUED;
    WotbModV3Result result = WOTBMOD_V3_E_CANCELLED;
};

std::mutex g_async_mutex;
std::condition_variable g_worker_cv;
std::condition_variable g_timer_cv;
std::condition_variable g_main_role_cv;
std::deque<QueuedCall> g_worker_queue;
std::array<std::deque<QueuedCall>, 6u> g_role_queues;
std::vector<std::weak_ptr<TaskState>> g_tasks;
std::vector<std::weak_ptr<TimerState>> g_timers;
std::vector<std::thread> g_workers;
std::thread g_timer_thread;
bool g_async_started = false;
bool g_async_stopping = false;
std::atomic<uint64_t> g_blocking_main_completion_epoch{0u};

bool EnqueueWorker(QueuedCall call) {
    std::lock_guard<std::mutex> lock(g_async_mutex);
    if (!g_async_started || g_async_stopping ||
        g_worker_queue.size() >= kMaxWorkerQueue) {
        return false;
    }
    g_worker_queue.push_back(std::move(call));
    g_worker_cv.notify_one();
    return true;
}

bool EnqueueRole(uint32_t role, QueuedCall call) {
    if (role >= g_role_queues.size() ||
        !ValidThreadRole(role, false)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_async_mutex);
        if (!g_async_started || g_async_stopping ||
            g_role_queues[role].size() >= kMaxRoleQueue) {
            return false;
        }
        g_role_queues[role].push_back(std::move(call));
    }
    if (role == WOTBMOD_V3_THREAD_MAIN) g_main_role_cv.notify_one();
    return true;
}

void WorkerLoop() {
    const uint32_t previous_role =
        SetCurrentThreadRole(WOTBMOD_V3_THREAD_WORKER);
    for (;;) {
        QueuedCall call;
        {
            std::unique_lock<std::mutex> lock(g_async_mutex);
            g_worker_cv.wait(
                lock,
                []() {
                    return g_async_stopping || !g_worker_queue.empty();
                });
            if (g_async_stopping && g_worker_queue.empty()) break;
            call = std::move(g_worker_queue.front());
            g_worker_queue.pop_front();
        }
        if (call.invoke) call.invoke();
    }
    SetCurrentThreadRole(previous_role);
}

void ExecuteTimerCallback(const std::shared_ptr<TimerState>& timer) {
    uint64_t fire_count = 0u;
    {
        std::lock_guard<std::mutex> lock(timer->mutex);
        if (timer->queued_callbacks != 0u) --timer->queued_callbacks;
        if (timer->cancelled) {
            timer->finished_cv.notify_all();
            return;
        }
        ++timer->active_callbacks;
        timer->execution_thread = std::this_thread::get_id();
        fire_count = timer->fire_count;
    }
    CallbackScope scope(timer->owner);
    if (scope.entered()) {
        try {
            timer->callback(
                timer->owner,
                timer->handle,
                fire_count,
                timer->user_data);
        } catch (...) {
            SetError(
                timer->owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "timer callback raised an exception");
        }
    }
    {
        std::lock_guard<std::mutex> lock(timer->mutex);
        if (timer->active_callbacks != 0u) --timer->active_callbacks;
        timer->execution_thread = std::thread::id();
        timer->finished_cv.notify_all();
    }
}

void ScheduleTimerCallback(const std::shared_ptr<TimerState>& timer) {
    QueuedCall call;
    call.owner = timer->owner;
    call.invoke = [timer]() { ExecuteTimerCallback(timer); };
    bool queued = false;
    if (timer->callback_role == WOTBMOD_V3_THREAD_WORKER) {
        queued = EnqueueWorker(std::move(call));
    } else {
        queued = EnqueueRole(timer->callback_role, std::move(call));
    }
    if (!queued) {
        std::lock_guard<std::mutex> lock(timer->mutex);
        if (timer->queued_callbacks != 0u) --timer->queued_callbacks;
        if (!timer->repeating) timer->cancelled = true;
        timer->finished_cv.notify_all();
        SetError(
            timer->owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "timer callback queue is full");
    }
}

void TimerLoop() {
    const uint32_t previous_role =
        SetCurrentThreadRole(WOTBMOD_V3_THREAD_WORKER);
    for (;;) {
        std::vector<std::shared_ptr<TimerState>> timers;
        {
            std::unique_lock<std::mutex> lock(g_async_mutex);
            g_timer_cv.wait_for(
                lock,
                std::chrono::milliseconds(5),
                []() { return g_async_stopping; });
            if (g_async_stopping) break;
            g_timers.erase(
                std::remove_if(
                    g_timers.begin(),
                    g_timers.end(),
                    [](const std::weak_ptr<TimerState>& timer) {
                        return timer.expired();
                    }),
                g_timers.end());
            for (const auto& weak : g_timers) {
                const auto timer = weak.lock();
                if (timer) timers.push_back(timer);
            }
        }
        const Clock::time_point now = Clock::now();
        for (const auto& timer : timers) {
            bool fire = false;
            {
                std::lock_guard<std::mutex> lock(timer->mutex);
                if (timer->cancelled || timer->paused ||
                    !timer->armed || now < timer->next_fire) {
                    continue;
                }
                if (timer->queued_callbacks != 0u ||
                    timer->active_callbacks != 0u) {
                    if (timer->repeating) {
                        timer->next_fire =
                            now +
                            std::chrono::milliseconds(timer->interval_ms);
                    }
                    continue;
                }
                ++timer->fire_count;
                ++timer->queued_callbacks;
                fire = true;
                if (timer->repeating) {
                    timer->next_fire =
                        now + std::chrono::milliseconds(timer->interval_ms);
                } else {
                    timer->armed = false;
                }
            }
            if (fire) ScheduleTimerCallback(timer);
        }
    }
    SetCurrentThreadRole(previous_role);
}

void StartAsyncRuntime() {
    std::lock_guard<std::mutex> lock(g_async_mutex);
    if (g_async_started) return;
    g_async_stopping = false;
    g_async_started = true;
    for (size_t index = 0u; index < kWorkerCount; ++index) {
        g_workers.emplace_back(&WorkerLoop);
    }
    g_timer_thread = std::thread(&TimerLoop);
}

void StopAsyncRuntime() {
    std::vector<std::shared_ptr<TimerState>> timers;
    {
        std::lock_guard<std::mutex> lock(g_async_mutex);
        if (!g_async_started) return;
        g_async_stopping = true;
        for (const auto& weak : g_timers) {
            const auto timer = weak.lock();
            if (timer) timers.push_back(timer);
        }
        g_timers.clear();
        g_tasks.clear();
        for (QueuedCall& call : g_worker_queue) {
            if (call.cancel) call.cancel();
        }
        g_worker_queue.clear();
        for (auto& queue : g_role_queues) {
            for (QueuedCall& call : queue) {
                if (call.cancel) call.cancel();
            }
            queue.clear();
        }
    }
    for (const auto& timer : timers) {
        std::lock_guard<std::mutex> timer_lock(timer->mutex);
        timer->cancelled = true;
        timer->armed = false;
        timer->finished_cv.notify_all();
    }
    g_worker_cv.notify_all();
    g_timer_cv.notify_all();
    g_main_role_cv.notify_all();
    if (g_timer_thread.joinable()) g_timer_thread.join();
    for (std::thread& worker : g_workers) {
        if (worker.joinable()) worker.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_async_mutex);
        g_workers.clear();
        g_worker_queue.clear();
        g_async_started = false;
    }
}

void CompleteTask(
    const std::shared_ptr<TaskState>& task,
    WotbModV3Result result) {
    WotbModV3TaskCompletionCallback completion = nullptr;
    uint32_t completion_role = WOTBMOD_V3_THREAD_MAIN;
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->running = false;
        task->execution_thread = std::thread::id();
        if (task->cancellation_requested ||
            result == WOTBMOD_V3_E_CANCELLED) {
            task->state = WOTBMOD_V3_TASK_CANCELLED;
            task->result_code = WOTBMOD_V3_E_CANCELLED;
        } else if (result == WOTBMOD_V3_OK) {
            task->state = WOTBMOD_V3_TASK_COMPLETED;
            task->result_code = WOTBMOD_V3_OK;
            task->progress = 1.0f;
        } else {
            task->state = WOTBMOD_V3_TASK_FAILED;
            task->result_code = result;
        }
        completion = task->completion;
        completion_role = task->completion_role;
        task->finished_cv.notify_all();
    }
    if (!completion) return;

    const auto invoke_completion = [task]() {
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            if (task->cancellation_requested &&
                task->state != WOTBMOD_V3_TASK_CANCELLED) {
                return;
            }
            task->completion_running = true;
            task->execution_thread = std::this_thread::get_id();
        }
        CallbackScope scope(task->owner);
        if (scope.entered()) {
            try {
                task->completion(
                    task->owner,
                    task->handle,
                    task->result_code,
                    task->user_data);
            } catch (...) {
                SetError(
                    task->owner,
                    WOTBMOD_V3_E_CALLBACK_FAULT,
                    "task completion callback raised an exception");
            }
        }
        {
            std::lock_guard<std::mutex> lock(task->mutex);
            task->completion_running = false;
            task->execution_thread = std::thread::id();
            task->finished_cv.notify_all();
        }
    };

    QueuedCall call;
    call.owner = task->owner;
    call.invoke = invoke_completion;
    const bool queued =
        completion_role == WOTBMOD_V3_THREAD_WORKER
            ? EnqueueWorker(std::move(call))
            : EnqueueRole(completion_role, std::move(call));
    if (!queued) {
        SetError(
            task->owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "task completion queue is full");
    }
}

void ExecuteTaskWork(const std::shared_ptr<TaskState>& task) {
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->cancellation_requested) {
            task->state = WOTBMOD_V3_TASK_CANCELLED;
            task->result_code = WOTBMOD_V3_E_CANCELLED;
            task->finished_cv.notify_all();
            return;
        }
        task->state = WOTBMOD_V3_TASK_RUNNING;
        task->running = true;
        task->execution_thread = std::this_thread::get_id();
    }
    WotbModV3Result result = WOTBMOD_V3_E_CANCELLED;
    CallbackScope scope(task->owner);
    if (scope.entered()) {
        try {
            result = task->work(
                task->owner, task->handle, task->user_data);
        } catch (...) {
            result = WOTBMOD_V3_E_CALLBACK_FAULT;
            SetError(
                task->owner,
                result,
                "task work callback raised an exception");
        }
    }
    CompleteTask(task, result);
}

void ExecuteDispatch(const std::shared_ptr<TaskState>& task) {
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        if (task->cancellation_requested) {
            task->state = WOTBMOD_V3_TASK_CANCELLED;
            task->result_code = WOTBMOD_V3_E_CANCELLED;
            task->finished_cv.notify_all();
            return;
        }
        task->state = WOTBMOD_V3_TASK_RUNNING;
        task->running = true;
        task->execution_thread = std::this_thread::get_id();
    }
    WotbModV3Result result = WOTBMOD_V3_OK;
    CallbackScope scope(task->owner);
    if (!scope.entered()) {
        result = WOTBMOD_V3_E_CANCELLED;
    } else {
        try {
            task->dispatch(task->owner, task->user_data);
        } catch (...) {
            result = WOTBMOD_V3_E_CALLBACK_FAULT;
            SetError(
                task->owner,
                result,
                "thread dispatch callback raised an exception");
        }
    }
    CompleteTask(task, result);
}

void DestroyTask(void* object) {
    std::unique_ptr<TaskHandleObject> holder(
        static_cast<TaskHandleObject*>(object));
    if (!holder || !holder->task) return;
    std::shared_ptr<TaskState> task = holder->task;
    std::unique_lock<std::mutex> lock(task->mutex);
    task->cancellation_requested = true;
    if (task->state == WOTBMOD_V3_TASK_QUEUED) {
        task->state = WOTBMOD_V3_TASK_CANCELLED;
        task->result_code = WOTBMOD_V3_E_CANCELLED;
        task->finished_cv.notify_all();
    }
    if (task->execution_thread != std::this_thread::get_id()) {
        task->finished_cv.wait(
            lock,
            [&]() {
                return !task->running && !task->completion_running;
            });
    }
}

WotbModV3Result GetOwnedTask(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    std::shared_ptr<TaskState>* out_task) {
    if (!out_task) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_task is required");
    }
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        task_handle,
        WOTBMOD_V3_HANDLE_TASK,
        &object,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    TaskHandleObject* holder = static_cast<TaskHandleObject*>(object);
    if (!holder->task) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "task is destroyed");
    }
    *out_task = holder->task;
    return WOTBMOD_V3_OK;
}

WotbModV3Result CreateTaskHandle(
    WotbModV3Handle mod,
    const std::shared_ptr<TaskState>& task,
    WotbModV3TaskHandle* out_task) {
    std::unique_ptr<TaskHandleObject> holder(
        new (std::nothrow) TaskHandleObject());
    if (!holder) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "task handle allocation failed");
    }
    holder->task = task;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_TASK,
        holder.get(),
        &DestroyTask,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    holder.release();
    task->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_async_mutex);
        g_tasks.erase(
            std::remove_if(
                g_tasks.begin(),
                g_tasks.end(),
                [](const std::weak_ptr<TaskState>& item) {
                    return item.expired();
                }),
            g_tasks.end());
        g_tasks.push_back(task);
    }
    *out_task = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskSubmit(
    WotbModV3Handle mod,
    const WotbModV3TaskSubmitInfo* info,
    WotbModV3TaskHandle* out_task) {
    if (!info ||
        info->struct_size < sizeof(WotbModV3TaskSubmitInfo) ||
        info->api_version != WOTBMOD_V3_ASYNC_VERSION ||
        !info->work || !out_task ||
        !ValidThreadRole(info->completion_thread_role, true) ||
        (info->description &&
         std::strlen(info->description) >= WOTBMOD_V3_MAX_NAME)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "task submission arguments are invalid");
    }
    *out_task = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    if (info->completion &&
        !HasGuaranteedAsyncIngress(info->completion_thread_role)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "task completion requires the RENDER or WORKER ingress");
    }
    std::shared_ptr<TaskState> task(new (std::nothrow) TaskState());
    if (!task) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "task allocation failed");
    }
    task->owner = mod;
    task->description = info->description ? info->description : "";
    task->work = info->work;
    task->completion = info->completion;
    task->user_data = info->user_data;
    task->completion_role = info->completion_thread_role;
    WotbModV3Result created = CreateTaskHandle(mod, task, out_task);
    if (created != WOTBMOD_V3_OK) return created;

    QueuedCall call;
    call.owner = mod;
    call.invoke = [task]() { ExecuteTaskWork(task); };
    call.cancel = [task]() {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->cancellation_requested = true;
        if (task->state == WOTBMOD_V3_TASK_QUEUED) {
            task->state = WOTBMOD_V3_TASK_CANCELLED;
            task->result_code = WOTBMOD_V3_E_CANCELLED;
            task->finished_cv.notify_all();
        }
    };
    if (!EnqueueWorker(std::move(call))) {
        ReleaseOwnedHandle(mod, *out_task);
        *out_task = WOTBMOD_V3_INVALID_HANDLE;
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "worker queue is full or shutting down");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskCancel(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle) {
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    task->cancellation_requested = true;
    if (task->state == WOTBMOD_V3_TASK_QUEUED) {
        task->state = WOTBMOD_V3_TASK_CANCELLED;
        task->result_code = WOTBMOD_V3_E_CANCELLED;
        task->finished_cv.notify_all();
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskGetInfo(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    WotbModV3TaskInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3TaskInfo) ||
        out_info->api_version != WOTBMOD_V3_ASYNC_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "task info has an invalid structure header");
    }
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    const uint32_t size = out_info->struct_size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = size;
    out_info->api_version = WOTBMOD_V3_ASYNC_VERSION;
    out_info->state = task->state;
    out_info->completion_thread_role = task->completion_role;
    out_info->progress = task->progress;
    out_info->result_code = task->result_code;
    out_info->result_size = static_cast<uint32_t>(task->result.size());
    out_info->cancellation_requested =
        task->cancellation_requested ? 1u : 0u;
    CopyText(
        out_info->description,
        sizeof(out_info->description),
        task->description.c_str());
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskGetState(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    uint32_t* out_state) {
    if (!out_state) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "task state output is required");
    }
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    *out_state = task->state;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskGetProgress(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    float* out_progress) {
    if (!out_progress) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "task progress output is required");
    }
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    *out_progress = task->progress;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskSetProgress(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    float progress) {
    if (!(progress >= 0.0f && progress <= 1.0f)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "task progress must be between 0 and 1");
    }
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    if (task->state != WOTBMOD_V3_TASK_RUNNING) {
        return SetError(
            mod, WOTBMOD_V3_E_BUSY,
            "task progress can only be changed while work is running");
    }
    task->progress = progress;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskSetResult(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    const void* data,
    uint32_t size) {
    if ((size != 0u && !data) || size > WOTBMOD_V3_MAX_TASK_RESULT) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "task result is null or exceeds the one-megabyte limit");
    }
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    if (task->state != WOTBMOD_V3_TASK_RUNNING) {
        return SetError(
            mod, WOTBMOD_V3_E_BUSY,
            "task result can only be changed while work is running");
    }
    task->result.resize(size);
    if (size != 0u) std::memcpy(task->result.data(), data, size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskGetResult(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    WotbModV3Buffer* out_result) {
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    if (task->state == WOTBMOD_V3_TASK_QUEUED ||
        task->state == WOTBMOD_V3_TASK_RUNNING) {
        return SetError(
            mod, WOTBMOD_V3_E_BUSY,
            "task has not completed");
    }
    if (task->state == WOTBMOD_V3_TASK_CANCELLED) {
        return WOTBMOD_V3_E_CANCELLED;
    }
    return CopyBytesResult(mod, task->result, out_result);
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTaskIsCancellationRequested(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task_handle,
    uint32_t* out_requested) {
    if (!out_requested) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_requested is required");
    }
    std::shared_ptr<TaskState> task;
    const WotbModV3Result result =
        GetOwnedTask(mod, task_handle, &task);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(task->mutex);
    *out_requested = task->cancellation_requested ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result DispatchToRole(
    WotbModV3Handle mod,
    uint32_t role,
    WotbModV3DispatchCallback callback,
    void* user_data,
    WotbModV3TaskHandle* out_task) {
    if (!callback || !out_task || !ValidThreadRole(role, false)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "thread dispatch arguments are invalid");
    }
    *out_task = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    if (!HasGuaranteedAsyncIngress(role)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            role == WOTBMOD_V3_THREAD_MAIN
                ? "the MAIN dispatch ingress is not installed on this client"
                : "this dispatch ingress is not connected to the host");
    }
    std::shared_ptr<TaskState> task(new (std::nothrow) TaskState());
    if (!task) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "thread dispatch allocation failed");
    }
    task->owner = mod;
    task->description = "thread dispatch";
    task->dispatch = callback;
    task->user_data = user_data;
    task->completion_role = role;
    WotbModV3Result created = CreateTaskHandle(mod, task, out_task);
    if (created != WOTBMOD_V3_OK) return created;
    QueuedCall call;
    call.owner = mod;
    call.invoke = [task]() { ExecuteDispatch(task); };
    call.cancel = [task]() {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->cancellation_requested = true;
        if (task->state == WOTBMOD_V3_TASK_QUEUED) {
            task->state = WOTBMOD_V3_TASK_CANCELLED;
            task->result_code = WOTBMOD_V3_E_CANCELLED;
            task->finished_cv.notify_all();
        }
    };
    if (!EnqueueRole(role, std::move(call))) {
        ReleaseOwnedHandle(mod, *out_task);
        *out_task = WOTBMOD_V3_INVALID_HANDLE;
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "target thread queue is full or shutting down");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncDispatchMain(
    WotbModV3Handle mod,
    WotbModV3DispatchCallback callback,
    void* user_data,
    WotbModV3TaskHandle* out_task) {
    return DispatchToRole(
        mod,
        WOTBMOD_V3_THREAD_MAIN,
        callback,
        user_data,
        out_task);
}

WotbModV3Result WOTBMOD_V3_CALL AsyncDispatchRender(
    WotbModV3Handle mod,
    WotbModV3DispatchCallback callback,
    void* user_data,
    WotbModV3TaskHandle* out_task) {
    return DispatchToRole(
        mod,
        WOTBMOD_V3_THREAD_RENDER,
        callback,
        user_data,
        out_task);
}

WotbModV3Result WOTBMOD_V3_CALL AsyncDispatchAudio(
    WotbModV3Handle mod,
    WotbModV3DispatchCallback callback,
    void* user_data,
    WotbModV3TaskHandle* out_task) {
    return DispatchToRole(
        mod,
        WOTBMOD_V3_THREAD_AUDIO,
        callback,
        user_data,
        out_task);
}

uint32_t PumpRole(
    uint32_t role,
    WotbModV3Handle owner_filter,
    uint32_t maximum) {
    uint32_t executed = 0u;
    const uint32_t limit = maximum == 0u ? kDefaultPumpLimit : maximum;
    while (executed < limit) {
        QueuedCall call;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_async_mutex);
            if (role >= g_role_queues.size()) break;
            auto& queue = g_role_queues[role];
            if (owner_filter == WOTBMOD_V3_INVALID_HANDLE) {
                if (!queue.empty()) {
                    call = std::move(queue.front());
                    queue.pop_front();
                    found = true;
                }
            } else {
                const auto item = std::find_if(
                    queue.begin(),
                    queue.end(),
                    [&](const QueuedCall& queued) {
                        return queued.owner == owner_filter;
                    });
                if (item != queue.end()) {
                    call = std::move(*item);
                    queue.erase(item);
                    found = true;
                }
            }
        }
        if (!found) break;
        if (call.invoke) call.invoke();
        ++executed;
    }
    return executed;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncPumpCurrentThread(
    WotbModV3Handle mod,
    uint32_t max_callbacks,
    uint32_t* out_executed) {
    if (!out_executed || max_callbacks > 4096u) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "pump output or callback limit is invalid");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    const uint32_t role = CurrentThreadRole();
    if (!ValidThreadRole(role, false)) {
        return SetError(
            mod, WOTBMOD_V3_E_WRONG_THREAD,
            "current thread has no dispatchable runtime role");
    }
    *out_executed = PumpRole(role, mod, max_callbacks);
    return WOTBMOD_V3_OK;
}

void DestroyTimer(void* object) {
    std::unique_ptr<TimerHandleObject> holder(
        static_cast<TimerHandleObject*>(object));
    if (!holder || !holder->timer) return;
    std::shared_ptr<TimerState> timer = holder->timer;
    std::unique_lock<std::mutex> lock(timer->mutex);
    timer->cancelled = true;
    timer->armed = false;
    if (timer->execution_thread != std::this_thread::get_id()) {
        timer->finished_cv.wait(
            lock,
            [&]() { return timer->active_callbacks == 0u; });
    }
    g_timer_cv.notify_all();
}

WotbModV3Result GetOwnedTimer(
    WotbModV3Handle mod,
    WotbModV3TimerHandle timer_handle,
    std::shared_ptr<TimerState>* out_timer) {
    if (!out_timer) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_timer is required");
    }
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        timer_handle,
        WOTBMOD_V3_HANDLE_TIMER,
        &object,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    TimerHandleObject* holder = static_cast<TimerHandleObject*>(object);
    if (!holder->timer) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "timer is destroyed");
    }
    *out_timer = holder->timer;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTimerCreate(
    WotbModV3Handle mod,
    const WotbModV3TimerCreateInfo* info,
    WotbModV3TimerHandle* out_timer) {
    const uint64_t kMaxDelayMs = 7ull * 24ull * 60ull * 60ull * 1000ull;
    if (!info ||
        info->struct_size < sizeof(WotbModV3TimerCreateInfo) ||
        info->api_version != WOTBMOD_V3_ASYNC_VERSION ||
        !info->callback || !out_timer ||
        info->delay_ms > kMaxDelayMs ||
        info->interval_ms > kMaxDelayMs ||
        (info->repeating != 0u && info->interval_ms == 0u) ||
        !ValidThreadRole(info->callback_thread_role, true)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "timer create arguments are invalid");
    }
    *out_timer = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    if (!HasGuaranteedAsyncIngress(info->callback_thread_role)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "timer callbacks require the RENDER or WORKER ingress");
    }
    std::shared_ptr<TimerState> timer(new (std::nothrow) TimerState());
    std::unique_ptr<TimerHandleObject> holder(
        new (std::nothrow) TimerHandleObject());
    if (!timer || !holder) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "timer allocation failed");
    }
    timer->owner = mod;
    timer->callback = info->callback;
    timer->user_data = info->user_data;
    timer->interval_ms = info->interval_ms;
    timer->repeating = info->repeating != 0u;
    timer->callback_role = info->callback_thread_role;
    timer->next_fire =
        Clock::now() + std::chrono::milliseconds(info->delay_ms);
    holder->timer = timer;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_TIMER,
        holder.get(),
        &DestroyTimer,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    holder.release();
    timer->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_async_mutex);
        if (!g_async_started || g_async_stopping) {
            ReleaseOwnedHandle(mod, handle);
            return SetError(
                mod, WOTBMOD_V3_E_BUSY,
                "async runtime is shutting down");
        }
        g_timers.push_back(timer);
    }
    *out_timer = handle;
    g_timer_cv.notify_all();
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTimerCancel(
    WotbModV3Handle mod,
    WotbModV3TimerHandle timer_handle) {
    std::shared_ptr<TimerState> timer;
    const WotbModV3Result result =
        GetOwnedTimer(mod, timer_handle, &timer);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(timer->mutex);
    timer->cancelled = true;
    timer->armed = false;
    timer->finished_cv.notify_all();
    g_timer_cv.notify_all();
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTimerPause(
    WotbModV3Handle mod,
    WotbModV3TimerHandle timer_handle) {
    std::shared_ptr<TimerState> timer;
    const WotbModV3Result result =
        GetOwnedTimer(mod, timer_handle, &timer);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(timer->mutex);
    if (timer->cancelled || !timer->armed) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "timer is cancelled or expired");
    }
    if (timer->paused) return WOTBMOD_V3_OK;
    const Clock::time_point now = Clock::now();
    timer->paused_remaining_ms =
        timer->next_fire > now
            ? static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      timer->next_fire - now)
                      .count())
            : 0u;
    timer->paused = true;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTimerResume(
    WotbModV3Handle mod,
    WotbModV3TimerHandle timer_handle) {
    std::shared_ptr<TimerState> timer;
    const WotbModV3Result result =
        GetOwnedTimer(mod, timer_handle, &timer);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(timer->mutex);
    if (timer->cancelled || !timer->armed) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "timer is cancelled or expired");
    }
    if (!timer->paused) return WOTBMOD_V3_OK;
    timer->next_fire =
        Clock::now() +
        std::chrono::milliseconds(timer->paused_remaining_ms);
    timer->paused = false;
    g_timer_cv.notify_all();
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncTimerGetInfo(
    WotbModV3Handle mod,
    WotbModV3TimerHandle timer_handle,
    WotbModV3TimerInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3TimerInfo) ||
        out_info->api_version != WOTBMOD_V3_ASYNC_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "timer info has an invalid structure header");
    }
    std::shared_ptr<TimerState> timer;
    const WotbModV3Result result =
        GetOwnedTimer(mod, timer_handle, &timer);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(timer->mutex);
    uint64_t remaining = 0u;
    if (timer->paused) {
        remaining = timer->paused_remaining_ms;
    } else if (timer->armed && !timer->cancelled &&
               timer->next_fire > Clock::now()) {
        remaining = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                timer->next_fire - Clock::now())
                .count());
    }
    const uint32_t size = out_info->struct_size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = size;
    out_info->api_version = WOTBMOD_V3_ASYNC_VERSION;
    out_info->interval_ms = timer->interval_ms;
    out_info->fire_count = timer->fire_count;
    out_info->remaining_ms = remaining;
    out_info->repeating = timer->repeating ? 1u : 0u;
    out_info->paused = timer->paused ? 1u : 0u;
    out_info->cancelled = timer->cancelled ? 1u : 0u;
    out_info->callback_thread_role = timer->callback_role;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncGetThreadRole(
    WotbModV3Handle mod,
    uint32_t* out_role) {
    if (!out_role) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_role is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    *out_role = CurrentThreadRole();
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL AsyncIsMainThread(
    WotbModV3Handle mod,
    uint32_t* out_is_main) {
    if (!out_is_main) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_is_main is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    *out_is_main =
        CurrentThreadRole() == WOTBMOD_V3_THREAD_MAIN ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

/* ------------------------------------------------------------------------- */
/* Permission-gated, public-HTTPS-only asynchronous HTTP transport            */
/* ------------------------------------------------------------------------- */

const size_t kMaxHttpRequestHeaderBytes = 64u * 1024u;
const DWORD kMaxHttpResponseHeaderBytes = 64u * 1024u;
const DWORD kHttpReadChunkBytes = 64u * 1024u;

struct ParsedHttpUrl {
    std::wstring host;
    std::wstring object_name;
};

struct HttpRequestState {
    std::mutex mutex;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3HttpHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t state = WOTBMOD_V3_HTTP_CREATED;
    uint32_t timeout_ms = 30000u;
    uint32_t max_response_size = WOTBMOD_V3_HTTP_DEFAULT_MAX_RESPONSE;
    uint32_t response_status = 0u;
    WotbModV3Result result_code = WOTBMOD_V3_OK;
    WotbModV3Result completion_result = WOTBMOD_V3_OK;
    std::string method = "GET";
    std::string url;
    std::unordered_map<std::string, std::string> headers;
    std::unordered_map<std::string, std::string> response_headers;
    std::vector<uint8_t> body;
    std::vector<uint8_t> response_body;
    WotbModV3HttpCompletionCallback completion = nullptr;
    void* completion_user_data = nullptr;
    bool handle_alive = true;
    bool completion_queued = false;
    bool completion_running = false;
    std::atomic<bool> cancellation_requested{false};
    std::atomic<void*> active_native_request{nullptr};
};

struct HttpRequestHandleObject {
    std::shared_ptr<HttpRequestState> request;
};

class ScopedWinHttpHandle {
public:
    explicit ScopedWinHttpHandle(HINTERNET handle = nullptr)
        : handle_(handle) {}

    ~ScopedWinHttpHandle() {
        if (handle_) WinHttpCloseHandle(handle_);
    }

    ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
    ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;

    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_;
};

WotbModV3Result WinHttpTransport(
    const testing::HttpTransportRequest& request,
    const testing::HttpTransportContext& context,
    testing::HttpTransportResponse* response);

std::atomic<testing::HttpTransportFn> g_http_transport{
    &WinHttpTransport};

void CloseActiveHttpRequest(std::atomic<void*>* active_request) {
    if (!active_request) return;
    void* handle = active_request->exchange(nullptr);
    if (handle) WinHttpCloseHandle(static_cast<HINTERNET>(handle));
}

void CancelHttpRequestState(
    const std::shared_ptr<HttpRequestState>& request,
    bool destroy_handle) {
    if (!request) return;
    request->cancellation_requested.store(true);
    CloseActiveHttpRequest(&request->active_native_request);
    std::lock_guard<std::mutex> lock(request->mutex);
    if (destroy_handle) {
        request->handle_alive = false;
        request->completion = nullptr;
        request->completion_queued = false;
    }
    if (request->state != WOTBMOD_V3_HTTP_COMPLETED &&
        request->state != WOTBMOD_V3_HTTP_FAILED) {
        request->state = WOTBMOD_V3_HTTP_CANCELLED;
        request->result_code = WOTBMOD_V3_E_CANCELLED;
        request->completion_result = WOTBMOD_V3_E_CANCELLED;
    }
}

void DestroyHttpRequest(void* object) {
    std::unique_ptr<HttpRequestHandleObject> holder(
        static_cast<HttpRequestHandleObject*>(object));
    if (holder) CancelHttpRequestState(holder->request, true);
}

WotbModV3Result GetHttpRequest(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    std::shared_ptr<HttpRequestState>* out_request) {
    if (!out_request) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_request is required");
    }
    const WotbModV3Result retained = RetainOwnedHandle(mod, handle);
    if (retained != WOTBMOD_V3_OK) return retained;
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        handle,
        WOTBMOD_V3_HANDLE_HTTP_REQUEST,
        &object,
        nullptr);
    if (inspect == WOTBMOD_V3_OK) {
        HttpRequestHandleObject* holder =
            static_cast<HttpRequestHandleObject*>(object);
        if (holder) *out_request = holder->request;
    }
    const WotbModV3Result released = ReleaseOwnedHandle(mod, handle);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    if (released != WOTBMOD_V3_OK) return released;
    if (!*out_request) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "HTTP request is destroyed");
    }
    return WOTBMOD_V3_OK;
}

std::string LowerAscii(const char* value) {
    std::string output = value ? value : "";
    std::transform(
        output.begin(),
        output.end(),
        output.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return output;
}

bool Utf8ToWide(const std::string& input, std::wstring* output) {
    if (!output) return false;
    output->clear();
    if (input.empty()) return true;
    if (input.size() >
        static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0);
    if (required <= 0) return false;
    output->resize(static_cast<size_t>(required));
    return MultiByteToWideChar(
               CP_UTF8,
               MB_ERR_INVALID_CHARS,
               input.data(),
               static_cast<int>(input.size()),
               &(*output)[0],
               required) == required;
}

bool WideToUtf8(const wchar_t* input, size_t size, std::string* output) {
    if (!output) return false;
    output->clear();
    if (!input || size == 0u) return true;
    if (size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int required = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input,
        static_cast<int>(size),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) return false;
    output->resize(static_cast<size_t>(required));
    return WideCharToMultiByte(
               CP_UTF8,
               WC_ERR_INVALID_CHARS,
               input,
               static_cast<int>(size),
               &(*output)[0],
               required,
               nullptr,
               nullptr) == required;
}

bool ValidHttpHeaderName(const char* name) {
    if (!HasText(name, 128u)) return false;
    for (const unsigned char character : std::string(name)) {
        if (!(std::isalnum(character) || character == '-' ||
              character == '_')) {
            return false;
        }
    }
    return true;
}

bool ValidHttpHeaderValue(const char* value) {
    if (!value || std::strlen(value) > 8192u) return false;
    for (const unsigned char character : std::string(value)) {
        if ((character < 0x20u && character != '\t') ||
            character == 0x7Fu) {
            return false;
        }
    }
    std::wstring converted;
    return Utf8ToWide(value, &converted);
}

bool ValidHttpMethod(const char* method) {
    if (!method) return false;
    static const char* const kMethods[] = {
        "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};
    for (const char* candidate : kMethods) {
        if (std::strcmp(method, candidate) == 0) return true;
    }
    return false;
}

bool IsPublicIpv4(const IN_ADDR& address) {
    const uint8_t* bytes =
        reinterpret_cast<const uint8_t*>(&address.S_un.S_addr);
    const uint8_t first = bytes[0];
    const uint8_t second = bytes[1];
    const uint8_t third = bytes[2];
    if (first == 0u || first == 10u || first == 127u ||
        first >= 224u) {
        return false;
    }
    if (first == 100u && (second & 0xC0u) == 0x40u) return false;
    if (first == 169u && second == 254u) return false;
    if (first == 172u && second >= 16u && second <= 31u) return false;
    if (first == 192u &&
        (second == 168u ||
         (second == 0u && (third == 0u || third == 2u)) ||
         (second == 88u && third == 99u))) {
        return false;
    }
    if (first == 198u &&
        ((second == 18u || second == 19u) ||
         (second == 51u && third == 100u))) {
        return false;
    }
    if (first == 203u && second == 0u && third == 113u) return false;
    return true;
}

bool IsPublicIpv6(const IN6_ADDR& address) {
    const uint8_t* bytes = address.u.Byte;
    static const uint8_t kMappedPrefix[12] = {
        0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0xFFu, 0xFFu};
    if (std::memcmp(bytes, kMappedPrefix, sizeof(kMappedPrefix)) == 0) {
        IN_ADDR mapped = {};
        std::memcpy(&mapped, bytes + 12u, sizeof(mapped));
        return IsPublicIpv4(mapped);
    }
    if ((bytes[0] & 0xE0u) != 0x20u) return false;
    if (bytes[0] == 0x20u && bytes[1] == 0x01u) {
        if ((bytes[2] == 0u && bytes[3] == 0u) ||
            (bytes[2] == 0x0Du && bytes[3] == 0xB8u)) {
            return false;
        }
    }
    if (bytes[0] == 0x20u && bytes[1] == 0x02u) {
        IN_ADDR embedded = {};
        std::memcpy(&embedded, bytes + 2u, sizeof(embedded));
        if (!IsPublicIpv4(embedded)) return false;
    }
    return true;
}

bool IsPublicSockaddr(const SOCKADDR* address) {
    if (!address) return false;
    if (address->sa_family == AF_INET) {
        return IsPublicIpv4(
            reinterpret_cast<const SOCKADDR_IN*>(address)->sin_addr);
    }
    if (address->sa_family == AF_INET6) {
        return IsPublicIpv6(
            reinterpret_cast<const SOCKADDR_IN6*>(address)->sin6_addr);
    }
    return false;
}

bool SameSockaddrAddress(
    const SOCKADDR_STORAGE& left,
    const SOCKADDR_STORAGE& right) {
    if (left.ss_family != right.ss_family) return false;
    if (left.ss_family == AF_INET) {
        const SOCKADDR_IN* left4 =
            reinterpret_cast<const SOCKADDR_IN*>(&left);
        const SOCKADDR_IN* right4 =
            reinterpret_cast<const SOCKADDR_IN*>(&right);
        return left4->sin_addr.S_un.S_addr ==
               right4->sin_addr.S_un.S_addr;
    }
    if (left.ss_family == AF_INET6) {
        const SOCKADDR_IN6* left6 =
            reinterpret_cast<const SOCKADDR_IN6*>(&left);
        const SOCKADDR_IN6* right6 =
            reinterpret_cast<const SOCKADDR_IN6*>(&right);
        return std::memcmp(
                   &left6->sin6_addr,
                   &right6->sin6_addr,
                   sizeof(left6->sin6_addr)) == 0;
    }
    return false;
}

bool ParsePublicHttpsUrl(
    const std::string& url,
    ParsedHttpUrl* parsed) {
    if (!parsed || url.find('#') != std::string::npos) return false;
    std::wstring wide_url;
    if (!Utf8ToWide(url, &wide_url) || wide_url.empty()) return false;
    URL_COMPONENTS components = {};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(
            wide_url.c_str(),
            static_cast<DWORD>(wide_url.size()),
            0u,
            &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        components.dwHostNameLength == 0u ||
        components.dwUserNameLength != 0u ||
        components.dwPasswordLength != 0u) {
        return false;
    }
    parsed->host.assign(
        components.lpszHostName,
        components.dwHostNameLength);
    parsed->object_name.assign(
        components.lpszUrlPath,
        components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0u) {
        parsed->object_name.append(
            components.lpszExtraInfo,
            components.dwExtraInfoLength);
    }
    if (parsed->object_name.empty()) parsed->object_name = L"/";

    IN_ADDR address4 = {};
    IN6_ADDR address6 = {};
    if (InetPtonW(AF_INET, parsed->host.c_str(), &address4) == 1 ||
        InetPtonW(AF_INET6, parsed->host.c_str(), &address6) == 1) {
        return false;
    }
    std::string host;
    if (!WideToUtf8(parsed->host.data(), parsed->host.size(), &host)) {
        return false;
    }
    host = LowerAscii(host.c_str());
    if (host == "localhost" || host.find('.') == std::string::npos ||
        (host.size() >= 6u &&
         host.compare(host.size() - 6u, 6u, ".local") == 0) ||
        (host.size() >= 7u &&
         host.compare(host.size() - 7u, 7u, ".local.") == 0)) {
        return false;
    }
    return true;
}

bool ValidPublicHttpsUrl(const std::string& url) {
    ParsedHttpUrl parsed;
    return ParsePublicHttpsUrl(url, &parsed);
}

bool HttpHostPermissionName(
    const std::string& url,
    std::string* permission_name) {
    if (!permission_name) return false;
    ParsedHttpUrl parsed;
    if (!ParsePublicHttpsUrl(url, &parsed)) return false;
    std::string host;
    if (!WideToUtf8(parsed.host.data(), parsed.host.size(), &host)) {
        return false;
    }
    host = LowerAscii(host.c_str());
    while (!host.empty() && host.back() == '.') host.pop_back();
    *permission_name = "network:https://" + host;
    return permission_name->size() <
           WOTBMOD_V3_MAX_PERMISSION_NAME;
}

WotbModV3Result ResolvePinnedPublicAddress(
    const std::wstring& host,
    uint32_t timeout_ms,
    const std::atomic<bool>* cancellation_requested,
    SOCKADDR_STORAGE* pinned_address,
    std::wstring* pinned_name) {
    if (!pinned_address || !pinned_name) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (cancellation_requested &&
        cancellation_requested->load()) {
        return WOTBMOD_V3_E_CANCELLED;
    }
    WSADATA data = {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    ADDRINFOEXW hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOEXW* result = nullptr;
    OVERLAPPED overlapped = {};
    const HANDLE completion_event =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completion_event) {
        WSACleanup();
        return WOTBMOD_V3_E_PLATFORM;
    }
    overlapped.hEvent = completion_event;
    HANDLE cancel_handle = nullptr;
    int resolved = GetAddrInfoExW(
        host.c_str(),
        L"443",
        NS_DNS,
        nullptr,
        &hints,
        &result,
        nullptr,
        &overlapped,
        nullptr,
        &cancel_handle);
    if (resolved == WSA_IO_PENDING) {
        const Clock::time_point deadline =
            Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const bool cancelled =
                cancellation_requested &&
                cancellation_requested->load();
            const Clock::time_point now = Clock::now();
            if (cancelled || now >= deadline) {
                if (cancel_handle) {
                    GetAddrInfoExCancel(&cancel_handle);
                }
                WaitForSingleObject(completion_event, INFINITE);
                GetAddrInfoExOverlappedResult(&overlapped);
                CloseHandle(completion_event);
                if (result) FreeAddrInfoExW(result);
                WSACleanup();
                return cancelled
                    ? WOTBMOD_V3_E_CANCELLED
                    : WOTBMOD_V3_E_TIMEOUT;
            }
            const uint64_t remaining =
                static_cast<uint64_t>(
                    std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                        deadline - now)
                        .count());
            const DWORD wait_ms = static_cast<DWORD>(
                std::max<uint64_t>(
                    1u, std::min<uint64_t>(remaining, 25u)));
            const DWORD wait_result =
                WaitForSingleObject(completion_event, wait_ms);
            if (wait_result == WAIT_OBJECT_0) {
                resolved =
                    GetAddrInfoExOverlappedResult(&overlapped);
                break;
            }
            if (wait_result == WAIT_FAILED) {
                if (cancel_handle) {
                    GetAddrInfoExCancel(&cancel_handle);
                }
                WaitForSingleObject(completion_event, INFINITE);
                GetAddrInfoExOverlappedResult(&overlapped);
                CloseHandle(completion_event);
                if (result) FreeAddrInfoExW(result);
                WSACleanup();
                return WOTBMOD_V3_E_PLATFORM;
            }
        }
    }
    CloseHandle(completion_event);
    if (resolved != 0) {
#if defined(WOTBMOD_V3_HTTP_STANDALONE_TEST)
        std::fprintf(
            stderr,
            "GetAddrInfoExW native error: %d (0x%08X)\n",
            resolved,
            static_cast<unsigned int>(resolved));
#endif
        WSACleanup();
        return resolved == WSAETIMEDOUT
            ? WOTBMOD_V3_E_TIMEOUT
            : WOTBMOD_V3_E_IO;
    }

    const ADDRINFOEXW* selected = nullptr;
    bool saw_address = false;
    bool all_public = true;
    for (const ADDRINFOEXW* item = result;
         item;
         item = item->ai_next) {
        if (!item->ai_addr ||
            (item->ai_family != AF_INET &&
             item->ai_family != AF_INET6)) {
            continue;
        }
        saw_address = true;
        if (!IsPublicSockaddr(item->ai_addr)) {
            all_public = false;
            break;
        }
        if (!selected ||
            (selected->ai_family == AF_INET6 &&
             item->ai_family == AF_INET)) {
            selected = item;
        }
    }
    WotbModV3Result final_result = WOTBMOD_V3_OK;
    if (!saw_address || !selected) {
        final_result = WOTBMOD_V3_E_IO;
    } else if (!all_public) {
        final_result = WOTBMOD_V3_E_PERMISSION_DENIED;
    } else if (cancellation_requested &&
               cancellation_requested->load()) {
        final_result = WOTBMOD_V3_E_CANCELLED;
    } else if (selected->ai_addrlen >
               sizeof(*pinned_address)) {
        final_result = WOTBMOD_V3_E_PLATFORM;
    } else {
        std::memset(pinned_address, 0, sizeof(*pinned_address));
        std::memcpy(
            pinned_address,
            selected->ai_addr,
            selected->ai_addrlen);
        wchar_t text[INET6_ADDRSTRLEN] = {};
        const void* binary = selected->ai_family == AF_INET
            ? static_cast<const void*>(
                  &reinterpret_cast<const SOCKADDR_IN*>(
                       selected->ai_addr)->sin_addr)
            : static_cast<const void*>(
                  &reinterpret_cast<const SOCKADDR_IN6*>(
                       selected->ai_addr)->sin6_addr);
        if (!InetNtopW(
                selected->ai_family,
                const_cast<void*>(binary),
                text,
                static_cast<DWORD>(
                    sizeof(text) / sizeof(text[0])))) {
            final_result = WOTBMOD_V3_E_PLATFORM;
        } else {
            *pinned_name = text;
        }
    }
    FreeAddrInfoExW(result);
    WSACleanup();
    return final_result;
}

WotbModV3Result WinHttpErrorResult(
    const testing::HttpTransportContext& context,
    DWORD error) {
#if defined(WOTBMOD_V3_HTTP_STANDALONE_TEST)
    std::fprintf(
        stderr,
        "WinHTTP native error: %lu (0x%08lX)\n",
        static_cast<unsigned long>(error),
        static_cast<unsigned long>(error));
#endif
    if ((context.cancellation_requested &&
         context.cancellation_requested->load()) ||
        error == ERROR_WINHTTP_OPERATION_CANCELLED ||
        error == ERROR_OPERATION_ABORTED) {
        return WOTBMOD_V3_E_CANCELLED;
    }
    if (error == ERROR_WINHTTP_TIMEOUT ||
        error == ERROR_TIMEOUT) {
        return WOTBMOD_V3_E_TIMEOUT;
    }
    if (error == ERROR_WINHTTP_INVALID_OPTION) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_E_IO;
}

uint32_t RemainingHttpTime(Clock::time_point deadline) {
    const Clock::time_point now = Clock::now();
    if (now >= deadline) return 0u;
    const uint64_t remaining = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now)
            .count());
    if (remaining == 0u) return 1u;
    return static_cast<uint32_t>(
        std::min<uint64_t>(
            remaining,
            std::numeric_limits<uint32_t>::max()));
}

bool AddResponseHeaders(
    const std::vector<wchar_t>& raw_headers,
    testing::HttpTransportResponse* response) {
    if (!response || raw_headers.empty()) return false;
    const wchar_t* cursor = raw_headers.data();
    const wchar_t* end = cursor + raw_headers.size();
    while (cursor < end && *cursor != L'\0') {
        const wchar_t* line_end = cursor;
        while (line_end < end &&
               *line_end != L'\0' &&
               *line_end != L'\r' &&
               *line_end != L'\n') {
            ++line_end;
        }
        const wchar_t* colon = std::find(cursor, line_end, L':');
        if (colon != line_end) {
            const wchar_t* value = colon + 1;
            while (value < line_end &&
                   (*value == L' ' || *value == L'\t')) {
                ++value;
            }
            const wchar_t* value_end = line_end;
            while (value_end > value &&
                   (value_end[-1] == L' ' ||
                    value_end[-1] == L'\t')) {
                --value_end;
            }
            std::string name;
            std::string header_value;
            if (!WideToUtf8(
                    cursor,
                    static_cast<size_t>(colon - cursor),
                    &name) ||
                !WideToUtf8(
                    value,
                    static_cast<size_t>(value_end - value),
                    &header_value)) {
                return false;
            }
            name = LowerAscii(name.c_str());
            const auto existing = response->headers.find(name);
            if (existing == response->headers.end()) {
                response->headers[name] = header_value;
            } else {
                if (existing->second.size() + header_value.size() + 2u >
                    kMaxHttpResponseHeaderBytes) {
                    return false;
                }
                existing->second.append(", ");
                existing->second.append(header_value);
            }
        }
        cursor = line_end;
        while (cursor < end &&
               (*cursor == L'\r' || *cursor == L'\n')) {
            ++cursor;
        }
    }
    return true;
}

WotbModV3Result WinHttpTransport(
    const testing::HttpTransportRequest& transport_request,
    const testing::HttpTransportContext& context,
    testing::HttpTransportResponse* response) {
    if (!response || !context.cancellation_requested ||
        !context.active_native_request) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    response->status = 0u;
    response->headers.clear();
    response->body.clear();
    const Clock::time_point deadline =
        Clock::now() +
        std::chrono::milliseconds(transport_request.timeout_ms);
    ParsedHttpUrl parsed;
    if (!ParsePublicHttpsUrl(transport_request.url, &parsed)) {
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }

    SOCKADDR_STORAGE pinned_address = {};
    std::wstring pinned_name;
    WotbModV3Result result = ResolvePinnedPublicAddress(
        parsed.host,
        RemainingHttpTime(deadline),
        context.cancellation_requested,
        &pinned_address,
        &pinned_name);
    if (result != WOTBMOD_V3_OK) return result;
    uint32_t remaining = RemainingHttpTime(deadline);
    if (remaining == 0u) return WOTBMOD_V3_E_TIMEOUT;

    ScopedWinHttpHandle session(WinHttpOpen(
        L"WotbModRuntime/3",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        WINHTTP_FLAG_SECURE_DEFAULTS));
    if (!session.get()) {
        return WinHttpErrorResult(context, GetLastError());
    }
    if (!WinHttpSetTimeouts(
            session.get(),
            static_cast<int>(remaining),
            static_cast<int>(remaining),
            static_cast<int>(remaining),
            static_cast<int>(remaining))) {
        return WinHttpErrorResult(context, GetLastError());
    }
    DWORD header_limit = kMaxHttpResponseHeaderBytes;
    WinHttpSetOption(
        session.get(),
        WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,
        &header_limit,
        sizeof(header_limit));
    ScopedWinHttpHandle connection(WinHttpConnect(
        session.get(),
        parsed.host.c_str(),
        INTERNET_DEFAULT_HTTPS_PORT,
        0u));
    if (!connection.get()) {
        return WinHttpErrorResult(context, GetLastError());
    }
    std::wstring method;
    if (!Utf8ToWide(transport_request.method, &method)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    HINTERNET native_request = WinHttpOpenRequest(
        connection.get(),
        method.c_str(),
        parsed.object_name.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!native_request) {
        return WinHttpErrorResult(context, GetLastError());
    }
    context.active_native_request->store(native_request);
    if (context.cancellation_requested->load()) {
        CloseActiveHttpRequest(context.active_native_request);
        return WOTBMOD_V3_E_CANCELLED;
    }

    const DWORD pinned_name_bytes = static_cast<DWORD>(
        (pinned_name.size() + 1u) * sizeof(wchar_t));
    if (!WinHttpSetOption(
            native_request,
            WINHTTP_OPTION_RESOLUTION_HOSTNAME,
            &pinned_name[0],
            pinned_name_bytes)) {
        result = WinHttpErrorResult(context, GetLastError());
        CloseActiveHttpRequest(context.active_native_request);
        return result;
    }
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    DWORD disabled_features =
        WINHTTP_DISABLE_AUTHENTICATION |
        WINHTTP_DISABLE_COOKIES |
        WINHTTP_DISABLE_KEEP_ALIVE |
        WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(
            native_request,
            WINHTTP_OPTION_REDIRECT_POLICY,
            &redirect_policy,
            sizeof(redirect_policy)) ||
        !WinHttpSetOption(
            native_request,
            WINHTTP_OPTION_DISABLE_FEATURE,
            &disabled_features,
            sizeof(disabled_features)) ||
        !WinHttpSetOption(
            native_request,
            WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,
            &header_limit,
            sizeof(header_limit))) {
        result = WinHttpErrorResult(context, GetLastError());
        CloseActiveHttpRequest(context.active_native_request);
        return result;
    }

    for (const auto& header : transport_request.headers) {
        std::wstring wide_name;
        std::wstring wide_value;
        if (!Utf8ToWide(header.first, &wide_name) ||
            !Utf8ToWide(header.second, &wide_value)) {
            CloseActiveHttpRequest(context.active_native_request);
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        const std::wstring line =
            wide_name + L": " + wide_value;
        if (!WinHttpAddRequestHeaders(
                native_request,
                line.c_str(),
                static_cast<DWORD>(line.size()),
                WINHTTP_ADDREQ_FLAG_ADD |
                    WINHTTP_ADDREQ_FLAG_REPLACE)) {
            result = WinHttpErrorResult(context, GetLastError());
            CloseActiveHttpRequest(context.active_native_request);
            return result;
        }
    }

    remaining = RemainingHttpTime(deadline);
    if (remaining == 0u) {
        CloseActiveHttpRequest(context.active_native_request);
        return WOTBMOD_V3_E_TIMEOUT;
    }
    WinHttpSetTimeouts(
        session.get(),
        static_cast<int>(remaining),
        static_cast<int>(remaining),
        static_cast<int>(remaining),
        static_cast<int>(remaining));
    LPVOID body = transport_request.body.empty()
        ? WINHTTP_NO_REQUEST_DATA
        : static_cast<LPVOID>(
              const_cast<uint8_t*>(transport_request.body.data()));
    const DWORD body_size =
        static_cast<DWORD>(transport_request.body.size());
    if (!WinHttpSendRequest(
            native_request,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0u,
            body,
            body_size,
            body_size,
            0u) ||
        !WinHttpReceiveResponse(native_request, nullptr)) {
        result = WinHttpErrorResult(context, GetLastError());
        CloseActiveHttpRequest(context.active_native_request);
        return result;
    }
    WINHTTP_CONNECTION_INFO connection_info = {};
    connection_info.cbSize = sizeof(connection_info);
    DWORD connection_info_size = sizeof(connection_info);
    if (!WinHttpQueryOption(
            native_request,
            WINHTTP_OPTION_CONNECTION_INFO,
            &connection_info,
            &connection_info_size) ||
        !IsPublicSockaddr(
            reinterpret_cast<const SOCKADDR*>(
                &connection_info.RemoteAddress)) ||
        !SameSockaddrAddress(
            pinned_address,
            connection_info.RemoteAddress)) {
        CloseActiveHttpRequest(context.active_native_request);
        return WOTBMOD_V3_E_PERMISSION_DENIED;
    }
    if (context.cancellation_requested->load()) {
        CloseActiveHttpRequest(context.active_native_request);
        return WOTBMOD_V3_E_CANCELLED;
    }

    DWORD status = 0u;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(
            native_request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status,
            &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
        result = WinHttpErrorResult(context, GetLastError());
        CloseActiveHttpRequest(context.active_native_request);
        return result;
    }
    response->status = status;

    DWORD raw_size = 0u;
    WinHttpQueryHeaders(
        native_request,
        WINHTTP_QUERY_RAW_HEADERS_CRLF,
        WINHTTP_HEADER_NAME_BY_INDEX,
        nullptr,
        &raw_size,
        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        raw_size == 0u ||
        raw_size > kMaxHttpResponseHeaderBytes) {
        CloseActiveHttpRequest(context.active_native_request);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    std::vector<wchar_t> raw_headers(
        static_cast<size_t>(
            (raw_size + sizeof(wchar_t) - 1u) /
            sizeof(wchar_t)) +
        1u,
        L'\0');
    if (!WinHttpQueryHeaders(
            native_request,
            WINHTTP_QUERY_RAW_HEADERS_CRLF,
            WINHTTP_HEADER_NAME_BY_INDEX,
            raw_headers.data(),
            &raw_size,
            WINHTTP_NO_HEADER_INDEX) ||
        !AddResponseHeaders(raw_headers, response)) {
        result = WinHttpErrorResult(context, GetLastError());
        CloseActiveHttpRequest(context.active_native_request);
        return result == WOTBMOD_V3_E_CANCELLED
            ? result
            : WOTBMOD_V3_E_PARSE;
    }

    DWORD content_length = 0u;
    DWORD content_length_size = sizeof(content_length);
    if (WinHttpQueryHeaders(
            native_request,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &content_length,
            &content_length_size,
            WINHTTP_NO_HEADER_INDEX) &&
        content_length > transport_request.max_response_size) {
        CloseActiveHttpRequest(context.active_native_request);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }

    for (;;) {
        if (context.cancellation_requested->load()) {
            CloseActiveHttpRequest(context.active_native_request);
            return WOTBMOD_V3_E_CANCELLED;
        }
        remaining = RemainingHttpTime(deadline);
        if (remaining == 0u) {
            CloseActiveHttpRequest(context.active_native_request);
            return WOTBMOD_V3_E_TIMEOUT;
        }
        DWORD receive_timeout = remaining;
        WinHttpSetOption(
            native_request,
            WINHTTP_OPTION_RECEIVE_TIMEOUT,
            &receive_timeout,
            sizeof(receive_timeout));
        DWORD available = 0u;
        if (!WinHttpQueryDataAvailable(native_request, &available)) {
            result = WinHttpErrorResult(context, GetLastError());
            CloseActiveHttpRequest(context.active_native_request);
            return result;
        }
        if (available == 0u) break;
        if (response->body.size() >
                transport_request.max_response_size ||
            available >
                transport_request.max_response_size -
                    response->body.size()) {
            CloseActiveHttpRequest(context.active_native_request);
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        const DWORD chunk_size =
            std::min<DWORD>(available, kHttpReadChunkBytes);
        const size_t offset = response->body.size();
        response->body.resize(offset + chunk_size);
        DWORD read = 0u;
        if (!WinHttpReadData(
                native_request,
                response->body.data() + offset,
                chunk_size,
                &read)) {
            response->body.resize(offset);
            result = WinHttpErrorResult(context, GetLastError());
            CloseActiveHttpRequest(context.active_native_request);
            return result;
        }
        response->body.resize(offset + read);
        if (read == 0u) break;
    }
    CloseActiveHttpRequest(context.active_native_request);
    return WOTBMOD_V3_OK;
}

WotbModV3Result EnsureHttpMutable(
    WotbModV3Handle mod,
    const HttpRequestState& request) {
    if (request.cancellation_requested.load() ||
        request.state == WOTBMOD_V3_HTTP_CANCELLED) {
        return SetError(
            mod, WOTBMOD_V3_E_CANCELLED,
            "HTTP request is cancelled");
    }
    if (request.state == WOTBMOD_V3_HTTP_SENDING ||
        request.state == WOTBMOD_V3_HTTP_COMPLETED ||
        request.state == WOTBMOD_V3_HTTP_FAILED) {
        return SetError(
            mod, WOTBMOD_V3_E_BUSY,
            "HTTP request can no longer be changed");
    }
    return WOTBMOD_V3_OK;
}

void InvokeHttpCompletion(
    const std::shared_ptr<HttpRequestState>& request) {
    WotbModV3HttpCompletionCallback completion = nullptr;
    void* user_data = nullptr;
    WotbModV3Result result = WOTBMOD_V3_E_CANCELLED;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3HttpHandle handle = WOTBMOD_V3_INVALID_HANDLE;
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->completion_queued = false;
        if (!request->handle_alive || !request->completion) return;
        request->completion_running = true;
        completion = request->completion;
        user_data = request->completion_user_data;
        result = request->completion_result;
        owner = request->owner;
        handle = request->handle;
    }
    CallbackScope scope(owner);
    if (scope.entered()) {
        try {
            completion(owner, handle, result, user_data);
        } catch (...) {
            SetError(
                owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "HTTP completion callback raised an exception");
        }
    }
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->completion_running = false;
        request->completion = nullptr;
        request->completion_user_data = nullptr;
    }
}

void FinishHttpRequest(
    const std::shared_ptr<HttpRequestState>& request,
    WotbModV3Result result,
    testing::HttpTransportResponse response) {
    bool queue_completion = false;
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        if (request->cancellation_requested.load()) {
            result = WOTBMOD_V3_E_CANCELLED;
        }
        request->result_code = result;
        request->completion_result = result;
        if (result == WOTBMOD_V3_OK) {
            request->state = WOTBMOD_V3_HTTP_COMPLETED;
            request->response_status = response.status;
            request->response_headers = std::move(response.headers);
            request->response_body = std::move(response.body);
        } else if (result == WOTBMOD_V3_E_CANCELLED) {
            request->state = WOTBMOD_V3_HTTP_CANCELLED;
            request->response_status = 0u;
            request->response_headers.clear();
            request->response_body.clear();
        } else {
            request->state = WOTBMOD_V3_HTTP_FAILED;
            request->response_status = 0u;
            request->response_headers.clear();
            request->response_body.clear();
        }
        if (request->handle_alive && request->completion) {
            request->completion_queued = true;
            queue_completion = true;
        }
    }
    if (!queue_completion) return;
    QueuedCall call;
    call.owner = request->owner;
    call.invoke = [request]() { InvokeHttpCompletion(request); };
    call.cancel = [request]() {
        CancelHttpRequestState(request, false);
        std::lock_guard<std::mutex> lock(request->mutex);
        request->completion = nullptr;
        request->completion_user_data = nullptr;
        request->completion_queued = false;
    };
    if (!EnqueueRole(WOTBMOD_V3_THREAD_RENDER, std::move(call))) {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->completion_queued = false;
        request->completion = nullptr;
        request->completion_user_data = nullptr;
        request->state = WOTBMOD_V3_HTTP_FAILED;
        request->result_code = WOTBMOD_V3_E_LIMIT_REACHED;
        request->completion_result = WOTBMOD_V3_E_LIMIT_REACHED;
        request->response_status = 0u;
        request->response_headers.clear();
        request->response_body.clear();
        SetError(
            request->owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "HTTP render-thread completion queue is full");
    }
}

void ExecuteHttpRequest(
    const std::shared_ptr<HttpRequestState>& request,
    testing::HttpTransportRequest transport_request) {
    testing::HttpTransportResponse response;
    WotbModV3Result result = WOTBMOD_V3_E_CANCELLED;
    if (!request->cancellation_requested.load()) {
        testing::HttpTransportContext context;
        context.cancellation_requested =
            &request->cancellation_requested;
        context.active_native_request =
            &request->active_native_request;
        const testing::HttpTransportFn transport =
            g_http_transport.load();
        result = transport
            ? transport(transport_request, context, &response)
            : WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (result == WOTBMOD_V3_OK) {
        size_t response_header_bytes = 0u;
        for (const auto& header : response.headers) {
            response_header_bytes +=
                header.first.size() + header.second.size() + 4u;
        }
        if (response.body.size() >
                transport_request.max_response_size ||
            response_header_bytes > kMaxHttpResponseHeaderBytes) {
            response.status = 0u;
            response.headers.clear();
            response.body.clear();
            result = WOTBMOD_V3_E_LIMIT_REACHED;
        }
    }
    CloseActiveHttpRequest(&request->active_native_request);
    FinishHttpRequest(request, result, std::move(response));
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestCreate(
    WotbModV3Handle mod,
    WotbModV3HttpHandle* out_request) {
    if (!out_request) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_request is required");
    }
    *out_request = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    std::shared_ptr<HttpRequestState> request(
        new (std::nothrow) HttpRequestState());
    std::unique_ptr<HttpRequestHandleObject> holder(
        new (std::nothrow) HttpRequestHandleObject());
    if (!request || !holder) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "HTTP request allocation failed");
    }
    request->owner = mod;
    holder->request = request;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_HTTP_REQUEST,
        holder.get(),
        &DestroyHttpRequest,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    holder.release();
    request->handle = handle;
    *out_request = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestSetMethod(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    const char* method) {
    if (!ValidHttpMethod(method)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "HTTP method is not supported");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result mutable_result =
        EnsureHttpMutable(mod, *request);
    if (mutable_result != WOTBMOD_V3_OK) return mutable_result;
    request->method = method;
    request->state = WOTBMOD_V3_HTTP_CONFIGURED;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestSetUrl(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    const char* url) {
    if (!HasText(url, WOTBMOD_V3_MAX_PATH - 1u) ||
        !ValidPublicHttpsUrl(url)) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "only public HTTPS hostnames on port 443 are accepted");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result mutable_result =
        EnsureHttpMutable(mod, *request);
    if (mutable_result != WOTBMOD_V3_OK) return mutable_result;
    request->url = url;
    request->state = WOTBMOD_V3_HTTP_CONFIGURED;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestSetHeader(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    const char* name,
    const char* value) {
    if (!ValidHttpHeaderName(name) ||
        !ValidHttpHeaderValue(value)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "HTTP header name or UTF-8 value is invalid");
    }
    const std::string lower_name = LowerAscii(name);
    if (lower_name == "host" || lower_name == "connection" ||
        lower_name == "cookie" ||
        lower_name == "proxy-authorization" ||
        lower_name == "proxy-connection" ||
        lower_name == "content-length" ||
        lower_name == "transfer-encoding" ||
        lower_name == "upgrade" ||
        lower_name == "trailer") {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "restricted transport header cannot be set by a mod");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result mutable_result =
        EnsureHttpMutable(mod, *request);
    if (mutable_result != WOTBMOD_V3_OK) return mutable_result;
    const auto existing = request->headers.find(lower_name);
    if (request->headers.size() >= 64u &&
        existing == request->headers.end()) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "HTTP header count limit reached");
    }
    size_t total = 0u;
    for (const auto& header : request->headers) {
        if (header.first != lower_name) {
            total += header.first.size() + header.second.size() + 4u;
        }
    }
    total += lower_name.size() + std::strlen(value) + 4u;
    if (total > kMaxHttpRequestHeaderBytes) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "HTTP request headers exceed the 64 KiB limit");
    }
    request->headers[lower_name] = value;
    request->state = WOTBMOD_V3_HTTP_CONFIGURED;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestSetBody(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    const void* data,
    uint32_t size) {
    if ((size != 0u && !data) || size > 8u * 1024u * 1024u) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "HTTP body is null or exceeds the eight-megabyte limit");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result mutable_result =
        EnsureHttpMutable(mod, *request);
    if (mutable_result != WOTBMOD_V3_OK) return mutable_result;
    request->body.resize(size);
    if (size != 0u) std::memcpy(request->body.data(), data, size);
    request->state = WOTBMOD_V3_HTTP_CONFIGURED;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestSetTimeout(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    uint32_t timeout_ms) {
    if (timeout_ms < 100u || timeout_ms > 300000u) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "HTTP timeout must be between 100 ms and five minutes");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result mutable_result =
        EnsureHttpMutable(mod, *request);
    if (mutable_result != WOTBMOD_V3_OK) return mutable_result;
    request->timeout_ms = timeout_ms;
    request->state = WOTBMOD_V3_HTTP_CONFIGURED;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestSetMaxResponse(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    uint32_t maximum) {
    if (maximum == 0u ||
        maximum > WOTBMOD_V3_HTTP_ABSOLUTE_MAX_RESPONSE) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "HTTP response limit is zero or exceeds 64 MiB");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result mutable_result =
        EnsureHttpMutable(mod, *request);
    if (mutable_result != WOTBMOD_V3_OK) return mutable_result;
    request->max_response_size = maximum;
    request->state = WOTBMOD_V3_HTTP_CONFIGURED;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestSendAsync(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    WotbModV3HttpCompletionCallback completion,
    void* user_data) {
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    testing::HttpTransportRequest transport_request;
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        const WotbModV3Result mutable_result =
            EnsureHttpMutable(mod, *request);
        if (mutable_result != WOTBMOD_V3_OK) return mutable_result;
        if (request->url.empty()) {
            return SetError(
                mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                "HTTP URL must be configured before sending");
        }
        std::string host_permission;
        if (!HttpHostPermissionName(
                request->url, &host_permission)) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PERMISSION_DENIED,
                "HTTP hostname cannot be represented by a permission");
        }
        const WotbModV3Result broad_permission =
            CheckNamedPermission(
                mod,
                "network.http",
                WOTBMOD_V3_PERMISSION_REVIEWED);
        const WotbModV3Result host_permission_result =
            CheckNamedPermission(
                mod,
                host_permission.c_str(),
                WOTBMOD_V3_PERMISSION_REVIEWED);
        if (broad_permission != WOTBMOD_V3_OK &&
            host_permission_result != WOTBMOD_V3_OK) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PERMISSION_DENIED,
                "HTTP send requires REVIEWED network.http or the exact "
                "network:https://host permission");
        }
        request->state = WOTBMOD_V3_HTTP_SENDING;
        request->result_code = WOTBMOD_V3_OK;
        request->completion_result = WOTBMOD_V3_OK;
        request->completion = completion;
        request->completion_user_data = user_data;
        request->response_status = 0u;
        request->response_headers.clear();
        request->response_body.clear();
        transport_request.method = request->method;
        transport_request.url = request->url;
        transport_request.headers = request->headers;
        transport_request.body = request->body;
        transport_request.timeout_ms = request->timeout_ms;
        transport_request.max_response_size =
            request->max_response_size;
    }
    QueuedCall call;
    call.owner = mod;
    call.invoke = [request, transport_request]() mutable {
        ExecuteHttpRequest(request, std::move(transport_request));
    };
    call.cancel = [request]() {
        CancelHttpRequestState(request, false);
    };
    if (!EnqueueWorker(std::move(call))) {
        std::lock_guard<std::mutex> lock(request->mutex);
        request->state = WOTBMOD_V3_HTTP_CONFIGURED;
        request->completion = nullptr;
        request->completion_user_data = nullptr;
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "HTTP worker queue is full or shutting down");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestCancel(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle) {
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    {
        std::lock_guard<std::mutex> lock(request->mutex);
        if (request->state == WOTBMOD_V3_HTTP_COMPLETED ||
            request->state == WOTBMOD_V3_HTTP_FAILED) {
            return SetError(
                mod,
                WOTBMOD_V3_E_BUSY,
                "HTTP request has already finished");
        }
        if (request->state == WOTBMOD_V3_HTTP_CANCELLED) {
            return WOTBMOD_V3_OK;
        }
    }
    CancelHttpRequestState(request, false);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpRequestGetInfo(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    WotbModV3HttpRequestInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3HttpRequestInfo) ||
        out_info->api_version != WOTBMOD_V3_HTTP_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "HTTP request info has an invalid structure header");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const uint32_t size = out_info->struct_size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = size;
    out_info->api_version = WOTBMOD_V3_HTTP_VERSION;
    out_info->state = request->state;
    out_info->timeout_ms = request->timeout_ms;
    out_info->max_response_size = request->max_response_size;
    out_info->response_status = request->response_status;
    out_info->response_size =
        static_cast<uint32_t>(request->response_body.size());
    CopyText(
        out_info->method,
        sizeof(out_info->method),
        request->method.c_str());
    CopyText(out_info->url, sizeof(out_info->url), request->url.c_str());
    return WOTBMOD_V3_OK;
}

WotbModV3Result HttpResponseAvailability(
    WotbModV3Handle mod,
    const HttpRequestState& request) {
    if (request.state == WOTBMOD_V3_HTTP_COMPLETED) {
        return WOTBMOD_V3_OK;
    }
    if (request.state == WOTBMOD_V3_HTTP_CANCELLED) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CANCELLED,
            "HTTP request was cancelled");
    }
    if (request.state == WOTBMOD_V3_HTTP_FAILED) {
        return SetError(
            mod,
            request.result_code == WOTBMOD_V3_OK
                ? WOTBMOD_V3_E_IO
                : request.result_code,
            "HTTP request failed before producing a response");
    }
    return SetError(
        mod,
        WOTBMOD_V3_E_BUSY,
        "HTTP response is not ready");
}

WotbModV3Result WOTBMOD_V3_CALL HttpResponseGetStatus(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    uint32_t* out_status) {
    if (!out_status) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "HTTP status output is required");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result available =
        HttpResponseAvailability(mod, *request);
    if (available != WOTBMOD_V3_OK) return available;
    *out_status = request->response_status;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL HttpResponseGetHeader(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    const char* name,
    char* buffer,
    uint32_t* inout_size) {
    if (!ValidHttpHeaderName(name)) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "response header name is invalid");
    }
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result available =
        HttpResponseAvailability(mod, *request);
    if (available != WOTBMOD_V3_OK) return available;
    const auto found = request->response_headers.find(LowerAscii(name));
    if (found == request->response_headers.end()) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    return CopyStringResult(mod, found->second, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL HttpResponseGetBody(
    WotbModV3Handle mod,
    WotbModV3HttpHandle handle,
    WotbModV3Buffer* out_body) {
    std::shared_ptr<HttpRequestState> request;
    const WotbModV3Result result =
        GetHttpRequest(mod, handle, &request);
    if (result != WOTBMOD_V3_OK) return result;
    std::lock_guard<std::mutex> lock(request->mutex);
    const WotbModV3Result available =
        HttpResponseAvailability(mod, *request);
    if (available != WOTBMOD_V3_OK) return available;
    return CopyBytesResult(mod, request->response_body, out_body);
}

/* ------------------------------------------------------------------------- */
/* Inter-mod service registry and synchronous message bus                    */
/* ------------------------------------------------------------------------- */

struct ExportRecord {
    WotbModV3Token handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    std::string service_id;
    uint32_t version = 0u;
    const void* table = nullptr;
    uint32_t table_size = 0u;
    uint64_t order = 0u;
    std::atomic<bool> active{true};
};

struct ExportHandleObject {
    std::shared_ptr<ExportRecord> record;
};

struct MessageSubscription {
    WotbModV3Token handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    std::string pattern;
    int32_t priority = 0;
    WotbModV3IntermodMessageCallback callback = nullptr;
    void* user_data = nullptr;
    uint64_t order = 0u;
    std::atomic<bool> active{true};
};

struct MessageSubscriptionHandle {
    std::shared_ptr<MessageSubscription> subscription;
};

std::mutex g_intermod_mutex;
std::vector<std::shared_ptr<ExportRecord>> g_exports;
std::vector<std::shared_ptr<MessageSubscription>> g_message_subscriptions;
std::unordered_map<std::string, WotbModV3Handle> g_known_mods;
std::atomic<uint64_t> g_intermod_order{1u};

void RememberModLocked(WotbModV3Handle mod) {
    const char* id = ModId(mod);
    if (id && id[0] != '\0') g_known_mods[id] = mod;
}

void DestroyExport(void* object) {
    std::unique_ptr<ExportHandleObject> holder(
        static_cast<ExportHandleObject*>(object));
    if (!holder || !holder->record) return;
    holder->record->active.store(false);
    std::lock_guard<std::mutex> lock(g_intermod_mutex);
    g_exports.erase(
        std::remove_if(
            g_exports.begin(),
            g_exports.end(),
            [&](const std::shared_ptr<ExportRecord>& item) {
                return item.get() == holder->record.get();
            }),
        g_exports.end());
}

void DestroyMessageSubscription(void* object) {
    std::unique_ptr<MessageSubscriptionHandle> holder(
        static_cast<MessageSubscriptionHandle*>(object));
    if (!holder || !holder->subscription) return;
    holder->subscription->active.store(false);
    std::lock_guard<std::mutex> lock(g_intermod_mutex);
    g_message_subscriptions.erase(
        std::remove_if(
            g_message_subscriptions.begin(),
            g_message_subscriptions.end(),
            [&](const std::shared_ptr<MessageSubscription>& item) {
                return item.get() == holder->subscription.get();
            }),
        g_message_subscriptions.end());
}

WotbModV3Result WOTBMOD_V3_CALL IntermodFind(
    WotbModV3Handle mod,
    const char* mod_id,
    WotbModV3Handle* out_mod) {
    if (!HasText(mod_id, WOTBMOD_V3_MAX_ID - 1u) || !out_mod) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "mod_id and out_mod are required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    *out_mod = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result found = FindModById(mod_id, out_mod);
    if (found != WOTBMOD_V3_OK) {
        *out_mod = WOTBMOD_V3_INVALID_HANDLE;
        if (found == WOTBMOD_V3_E_NOT_FOUND) {
            return found;
        }
        return SetError(
            mod,
            found,
            "the runtime could not resolve the requested mod");
    }
    {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        RememberModLocked(mod);
        RememberModLocked(*out_mod);
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL IntermodIsLoaded(
    WotbModV3Handle mod,
    const char* mod_id,
    uint32_t* out_loaded) {
    if (!out_loaded) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "out_loaded is required");
    }
    WotbModV3Handle target = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result =
        IntermodFind(mod, mod_id, &target);
    if (result == WOTBMOD_V3_E_NOT_FOUND) {
        *out_loaded = 0u;
        return WOTBMOD_V3_OK;
    }
    if (result != WOTBMOD_V3_OK) return result;
    *out_loaded = 1u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL IntermodGetVersion(
    WotbModV3Handle mod,
    WotbModV3Handle target_mod,
    char* buffer,
    uint32_t* inout_size) {
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    if (!inout_size) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inout_size is required");
    }
    char version[WOTBMOD_V3_MAX_VERSION] = {};
    const WotbModV3Result version_result =
        GetModVersion(target_mod, version, sizeof(version));
    if (version_result == WOTBMOD_V3_E_INVALID_HANDLE) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_HANDLE,
            "target mod does not exist");
    }
    if (version_result != WOTBMOD_V3_OK) {
        return SetError(
            mod,
            version_result,
            "the runtime could not read target mod version metadata");
    }
    return CopyStringResult(mod, version, buffer, inout_size);
}

WotbModV3Result WOTBMOD_V3_CALL IntermodGetDependency(
    WotbModV3Handle mod,
    const char* dependency_id,
    WotbModV3Handle* out_dependency,
    uint32_t* out_optional) {
    if (!HasText(dependency_id, WOTBMOD_V3_MAX_ID - 1u) ||
        !out_dependency || !out_optional) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "dependency id and outputs are required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    *out_dependency = WOTBMOD_V3_INVALID_HANDLE;
    *out_optional = 0u;
    const WotbModV3Result resolved =
        ResolveDeclaredDependency(
            mod,
            dependency_id,
            out_dependency,
            out_optional);
    if (resolved == WOTBMOD_V3_OK) {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        RememberModLocked(mod);
        RememberModLocked(*out_dependency);
        return WOTBMOD_V3_OK;
    }
    *out_dependency = WOTBMOD_V3_INVALID_HANDLE;
    if (resolved == WOTBMOD_V3_E_NOT_FOUND) {
        return SetError(
            mod,
            resolved,
            *out_optional != 0u
                ? "optional manifest dependency is not loaded"
                : "dependency is undeclared or not loaded");
    }
    return SetError(
        mod,
        resolved,
        "the runtime could not resolve manifest dependency metadata");
}

WotbModV3Result WOTBMOD_V3_CALL IntermodExport(
    WotbModV3Handle mod,
    const char* service_id,
    uint32_t interface_version,
    const void* table,
    uint32_t table_size,
    WotbModV3Token* out_token) {
    if (!HasText(service_id, WOTBMOD_V3_MAX_SERVICE_ID - 1u) ||
        interface_version == 0u || !table || table_size < 8u ||
        table_size > 64u * 1024u || !out_token) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "exported interface arguments are invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    std::shared_ptr<ExportRecord> record(
        new (std::nothrow) ExportRecord());
    std::unique_ptr<ExportHandleObject> holder(
        new (std::nothrow) ExportHandleObject());
    if (!record || !holder) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "exported interface allocation failed");
    }
    record->owner = mod;
    record->service_id = service_id;
    record->version = interface_version;
    record->table = table;
    record->table_size = table_size;
    record->order = g_intermod_order.fetch_add(1u);
    holder->record = record;
    {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        RememberModLocked(mod);
        if (g_exports.size() >= kMaxExports) {
            return SetError(
                mod, WOTBMOD_V3_E_LIMIT_REACHED,
                "exported interface limit reached");
        }
        for (const auto& existing : g_exports) {
            if (existing && existing->active.load() &&
                existing->service_id == record->service_id &&
                existing->version == record->version) {
                return SetError(
                    mod, WOTBMOD_V3_E_ALREADY_EXISTS,
                    "service id and version are already exported");
            }
        }
    }
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_INTERMOD_EXPORT,
        holder.get(),
        &DestroyExport,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    holder.release();
    record->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        g_exports.push_back(record);
    }
    *out_token = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL IntermodUnexport(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        token,
        WOTBMOD_V3_HANDLE_INTERMOD_EXPORT,
        &object,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    ExportHandleObject* holder = static_cast<ExportHandleObject*>(object);
    if (!holder->record) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "exported interface is destroyed");
    }
    holder->record->active.store(false);
    return ReleaseOwnedHandle(mod, token);
}

WotbModV3Result WOTBMOD_V3_CALL IntermodImport(
    WotbModV3Handle mod,
    const char* service_id,
    uint32_t minimum_version,
    WotbModV3ImportedInterface* out_interface) {
    if (!HasText(service_id, WOTBMOD_V3_MAX_SERVICE_ID - 1u) ||
        minimum_version == 0u || !out_interface ||
        out_interface->struct_size <
            sizeof(WotbModV3ImportedInterface) ||
        out_interface->api_version != WOTBMOD_V3_INTERMOD_VERSION) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "imported interface arguments are invalid");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_intermod_mutex);
    RememberModLocked(mod);
    std::shared_ptr<ExportRecord> best;
    for (const auto& record : g_exports) {
        if (!record || !record->active.load() ||
            record->service_id != service_id ||
            record->version < minimum_version) {
            continue;
        }
        if (!best || record->version > best->version) best = record;
    }
    if (!best) return WOTBMOD_V3_E_NOT_FOUND;
    const uint32_t size = out_interface->struct_size;
    std::memset(out_interface, 0, sizeof(*out_interface));
    out_interface->struct_size = size;
    out_interface->api_version = WOTBMOD_V3_INTERMOD_VERSION;
    out_interface->provider_mod = best->owner;
    out_interface->interface_version = best->version;
    out_interface->table_size = best->table_size;
    out_interface->table = best->table;
    CopyText(
        out_interface->service_id,
        sizeof(out_interface->service_id),
        best->service_id.c_str());
    return WOTBMOD_V3_OK;
}

void FillExportInfo(
    const ExportRecord& record,
    WotbModV3ExportedInterfaceInfo* output) {
    const uint32_t size = output->struct_size;
    std::memset(output, 0, sizeof(*output));
    output->struct_size = size;
    output->api_version = WOTBMOD_V3_INTERMOD_VERSION;
    output->export_token = record.handle;
    output->provider_mod = record.owner;
    output->interface_version = record.version;
    output->table_size = record.table_size;
    CopyText(
        output->service_id,
        sizeof(output->service_id),
        record.service_id.c_str());
}

WotbModV3Result WOTBMOD_V3_CALL IntermodEnumerate(
    WotbModV3Handle mod,
    WotbModV3ExportedInterfaceInfo* interfaces,
    uint32_t* inout_count) {
    if (!inout_count) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inout_count is required");
    }
    const WotbModV3Result check = CheckMod(mod);
    if (check != WOTBMOD_V3_OK) return check;
    std::lock_guard<std::mutex> lock(g_intermod_mutex);
    std::vector<std::shared_ptr<ExportRecord>> active;
    for (const auto& record : g_exports) {
        if (record && record->active.load()) active.push_back(record);
    }
    const uint32_t required = static_cast<uint32_t>(active.size());
    if (!interfaces || *inout_count < required) {
        *inout_count = required;
        return required == 0u
                   ? WOTBMOD_V3_OK
                   : WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    for (uint32_t index = 0u; index < required; ++index) {
        if (interfaces[index].struct_size <
                sizeof(WotbModV3ExportedInterfaceInfo) ||
            interfaces[index].api_version !=
                WOTBMOD_V3_INTERMOD_VERSION) {
            return SetError(
                mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
                "export info output has an invalid structure header");
        }
        FillExportInfo(*active[index], &interfaces[index]);
    }
    *inout_count = required;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL IntermodMessagePublish(
    WotbModV3Handle mod,
    const char* topic_text,
    const void* payload,
    uint32_t payload_size) {
    if (!HasText(topic_text, WOTBMOD_V3_MAX_MESSAGE_TOPIC - 1u) ||
        (payload_size != 0u && !payload) ||
        payload_size > WOTBMOD_V3_MAX_INTERMOD_PAYLOAD) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inter-mod message topic or payload is invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    const std::string topic(topic_text);
    const char* id = ModId(mod);
    const std::string own_prefix =
        std::string("mod.") + (id ? id : "") + ".";
    if (topic.compare(0u, own_prefix.size(), own_prefix) != 0) {
        return SetError(
            mod, WOTBMOD_V3_E_PERMISSION_DENIED,
            "inter-mod messages must use mod.<own-id>.* topics");
    }
    std::vector<uint8_t> data(payload_size);
    if (payload_size != 0u) {
        std::memcpy(data.data(), payload, payload_size);
    }
    std::vector<std::shared_ptr<MessageSubscription>> listeners;
    {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        RememberModLocked(mod);
        for (const auto& listener : g_message_subscriptions) {
            if (listener && listener->active.load() &&
                MatchesPattern(listener->pattern, topic)) {
                listeners.push_back(listener);
            }
        }
    }
    std::stable_sort(
        listeners.begin(),
        listeners.end(),
        [](const std::shared_ptr<MessageSubscription>& left,
           const std::shared_ptr<MessageSubscription>& right) {
            if (left->priority != right->priority) {
                return left->priority > right->priority;
            }
            return left->order < right->order;
        });
    WotbModV3IntermodMessage message = {};
    message.struct_size = sizeof(message);
    message.api_version = WOTBMOD_V3_INTERMOD_VERSION;
    message.publisher_mod = mod;
    message.timestamp_ns = NowNs();
    CopyText(message.topic, sizeof(message.topic), topic.c_str());
    message.payload = data.empty() ? nullptr : data.data();
    message.payload_size = payload_size;
    for (const auto& listener : listeners) {
        if (!listener->active.load()) continue;
        CallbackScope scope(listener->owner);
        if (!scope.entered()) continue;
        try {
            listener->callback(
                listener->owner, &message, listener->user_data);
        } catch (...) {
            SetError(
                listener->owner,
                WOTBMOD_V3_E_CALLBACK_FAULT,
                "inter-mod message callback raised an exception");
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL IntermodMessageSubscribe(
    WotbModV3Handle mod,
    const char* pattern,
    int32_t priority,
    WotbModV3IntermodMessageCallback callback,
    void* user_data,
    WotbModV3Token* out_token) {
    if (!HasText(pattern, WOTBMOD_V3_MAX_MESSAGE_TOPIC - 1u) ||
        !ValidPriority(priority) || !callback || !out_token) {
        return SetError(
            mod, WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inter-mod subscription arguments are invalid");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    std::shared_ptr<MessageSubscription> subscription(
        new (std::nothrow) MessageSubscription());
    std::unique_ptr<MessageSubscriptionHandle> holder(
        new (std::nothrow) MessageSubscriptionHandle());
    if (!subscription || !holder) {
        return SetError(
            mod, WOTBMOD_V3_E_LIMIT_REACHED,
            "inter-mod subscription allocation failed");
    }
    subscription->owner = mod;
    subscription->pattern = pattern;
    subscription->priority = priority;
    subscription->callback = callback;
    subscription->user_data = user_data;
    subscription->order = g_intermod_order.fetch_add(1u);
    holder->subscription = subscription;
    {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        RememberModLocked(mod);
        if (g_message_subscriptions.size() >=
            kMaxIntermodSubscriptions) {
            return SetError(
                mod, WOTBMOD_V3_E_LIMIT_REACHED,
                "inter-mod subscription limit reached");
        }
    }
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result created = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_INTERMOD_SUBSCRIPTION,
        holder.get(),
        &DestroyMessageSubscription,
        &handle);
    if (created != WOTBMOD_V3_OK) return created;
    holder.release();
    subscription->handle = handle;
    {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        g_message_subscriptions.push_back(subscription);
    }
    *out_token = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL IntermodMessageUnsubscribe(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        token,
        WOTBMOD_V3_HANDLE_INTERMOD_SUBSCRIPTION,
        &object,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    MessageSubscriptionHandle* holder =
        static_cast<MessageSubscriptionHandle*>(object);
    if (!holder->subscription) {
        return SetError(
            mod, WOTBMOD_V3_E_OBJECT_DESTROYED,
            "inter-mod subscription is destroyed");
    }
    holder->subscription->active.store(false);
    return ReleaseOwnedHandle(mod, token);
}

/* ------------------------------------------------------------------------- */
/* Runtime integration and interface registration                           */
/* ------------------------------------------------------------------------- */

void RuntimeServicesOwnerStopping(WotbModV3Handle owner) {
    std::vector<std::shared_ptr<TaskState>> tasks;
    std::vector<std::shared_ptr<TimerState>> timers;
    {
        std::lock_guard<std::mutex> lock(g_async_mutex);
        for (auto iterator = g_worker_queue.begin();
             iterator != g_worker_queue.end();) {
            if (iterator->owner == owner) {
                if (iterator->cancel) iterator->cancel();
                iterator = g_worker_queue.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto& queue : g_role_queues) {
            for (auto iterator = queue.begin(); iterator != queue.end();) {
                if (iterator->owner == owner) {
                    if (iterator->cancel) iterator->cancel();
                    iterator = queue.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }
        g_tasks.erase(
            std::remove_if(
                g_tasks.begin(),
                g_tasks.end(),
                [](const std::weak_ptr<TaskState>& item) {
                    return item.expired();
                }),
            g_tasks.end());
        for (const auto& weak : g_tasks) {
            const auto task = weak.lock();
            if (task && task->owner == owner) tasks.push_back(task);
        }
        g_timers.erase(
            std::remove_if(
                g_timers.begin(),
                g_timers.end(),
                [](const std::weak_ptr<TimerState>& item) {
                    return item.expired();
                }),
            g_timers.end());
        for (const auto& weak : g_timers) {
            const auto timer = weak.lock();
            if (timer && timer->owner == owner) timers.push_back(timer);
        }
    }
    for (const auto& task : tasks) {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->cancellation_requested = true;
        if (task->state == WOTBMOD_V3_TASK_QUEUED) {
            task->state = WOTBMOD_V3_TASK_CANCELLED;
            task->result_code = WOTBMOD_V3_E_CANCELLED;
            task->finished_cv.notify_all();
        }
    }
    for (const auto& timer : timers) {
        std::lock_guard<std::mutex> lock(timer->mutex);
        timer->cancelled = true;
        timer->armed = false;
        timer->finished_cv.notify_all();
    }
    g_timer_cv.notify_all();
}

void RuntimeServicesLifecycleState(
    WotbModV3Handle mod,
    uint32_t transition) {
    const char* topic = nullptr;
    uint32_t public_transition = WOTBMOD_V3_MOD_TRANSITION_PRELOAD;
    uint32_t state = WOTBMOD_V3_MOD_STATE_UNKNOWN;
    switch (transition) {
        case LIFECYCLE_TRANSITION_PRELOAD:
            topic = WOTBMOD_V3_EVENT_MOD_PRELOAD;
            public_transition = WOTBMOD_V3_MOD_TRANSITION_PRELOAD;
            break;
        case LIFECYCLE_TRANSITION_LOADED:
            topic = WOTBMOD_V3_EVENT_MOD_LOADED;
            public_transition = WOTBMOD_V3_MOD_TRANSITION_LOADED;
            state = WOTBMOD_V3_MOD_STATE_LOADED;
            break;
        case LIFECYCLE_TRANSITION_ENABLED:
            topic = WOTBMOD_V3_EVENT_MOD_ENABLED;
            public_transition = WOTBMOD_V3_MOD_TRANSITION_ENABLED;
            state = WOTBMOD_V3_MOD_STATE_ENABLED;
            break;
        case LIFECYCLE_TRANSITION_DISABLED:
            topic = WOTBMOD_V3_EVENT_MOD_DISABLED;
            public_transition = WOTBMOD_V3_MOD_TRANSITION_DISABLED;
            state = WOTBMOD_V3_MOD_STATE_DISABLED;
            break;
        case LIFECYCLE_TRANSITION_UNLOADING:
            topic = WOTBMOD_V3_EVENT_MOD_UNLOADING;
            public_transition = WOTBMOD_V3_MOD_TRANSITION_UNLOADING;
            state = WOTBMOD_V3_MOD_STATE_UNLOADING;
            break;
        case LIFECYCLE_TRANSITION_UNLOADED:
            topic = WOTBMOD_V3_EVENT_MOD_UNLOADED;
            public_transition = WOTBMOD_V3_MOD_TRANSITION_UNLOADED;
            state = WOTBMOD_V3_MOD_STATE_UNLOADED;
            break;
        default:
            RuntimeLog(
                WOTBMOD_V3_LOG_WARNING,
                "v3.runtime-services",
                "ignored an unknown lifecycle transition");
            return;
    }

    WotbModV3LifecycleEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_LIFECYCLE_VERSION;
    event.mod = mod;
    event.transition = public_transition;
    event.state = state;
    CopyText(event.id, sizeof(event.id), ModId(mod));
    const WotbModV3Result result = PostEventInternal(
        WOTBMOD_V3_INVALID_HANDLE,
        topic,
        &event,
        sizeof(event),
        WOTBMOD_V3_EVENT_FLAG_NONE,
        true);
    if (result != WOTBMOD_V3_OK) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.runtime-services",
            "failed to publish a lifecycle event");
    }
}

void RuntimeServicesFramePump(uint64_t, double) {
    const uint32_t role = CurrentThreadRole();
    if (ValidThreadRole(role, false)) {
        PumpRole(role, WOTBMOD_V3_INVALID_HANDLE, 4096u);
    }
    PostEventInternal(
        WOTBMOD_V3_INVALID_HANDLE,
        WOTBMOD_V3_EVENT_FRAME_UPDATE,
        nullptr,
        0u,
        0u,
        true);
}

void RuntimeServicesShutdown() {
    PostEventInternal(
        WOTBMOD_V3_INVALID_HANDLE,
        WOTBMOD_V3_EVENT_CLIENT_SHUTTING_DOWN,
        nullptr,
        0u,
        0u,
        true);
    SetEventSourceMask(0u);
    StopAsyncRuntime();
    ShutdownEventSubscriptions();
    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        for (const auto& hook : g_hooks) {
            if (hook) hook->active.store(false);
        }
        g_hooks.clear();
        std::memset(
            &g_native_hook_backend,
            0,
            sizeof(g_native_hook_backend));
    }
    {
        std::lock_guard<std::mutex> lock(g_intermod_mutex);
        for (const auto& record : g_exports) {
            if (record) record->active.store(false);
        }
        for (const auto& subscription : g_message_subscriptions) {
            if (subscription) subscription->active.store(false);
        }
        g_exports.clear();
        g_message_subscriptions.clear();
        g_known_mods.clear();
    }
}

const WotbModV3LifecycleApiV1 kLifecycleApi = {
    sizeof(WotbModV3LifecycleApiV1),
    WOTBMOD_V3_LIFECYCLE_VERSION,
    &LifecycleGetCurrent,
    &LifecycleGetInfo,
    &LifecycleInstallPath,
    &LifecycleResourcePath,
    &LifecycleDataPath,
    &LifecycleCachePath,
    &LifecycleConfigPath,
    &LifecycleRegisterCleanup,
    &LifecycleUnregisterCleanup,
    &LifecycleRequestEnable,
    &LifecycleRequestDisable,
    &LifecycleRequestReload,
    &LifecycleCanHotReload};

const WotbModV3EventsApiV1 kEventsApi = {
    sizeof(WotbModV3EventsApiV1),
    WOTBMOD_V3_EVENTS_VERSION,
    &EventsSubscribe,
    &EventsUnsubscribe,
    &EventsSetPriority,
    &EventsPost,
    &EventsStopPropagation,
    &EventsGetDispatchInfo,
    &EventsGetThread,
    &EventsGetTimestamp,
    &EventsGetContext};

const WotbModV3HooksApiV1 kHooksApi = {
    sizeof(WotbModV3HooksApiV1),
    WOTBMOD_V3_HOOKS_VERSION,
    &HooksCreateSymbol,
    &HooksCreateVtable,
    &HooksEnable,
    &HooksDisable,
    &HooksRemove,
    &HooksSetPriority,
    &HooksRunBefore,
    &HooksRunAfter,
    &HooksGetOriginal,
    &HooksCallNext,
    &HooksGetInfo,
    &HooksGetOwner,
    &HooksGetStatus,
    &HooksGetChain,
    &HooksGetConflicts,
    &HooksEnumerate};

const WotbModV3UnsafeNativeApiV1 kUnsafeNativeApi = {
    sizeof(WotbModV3UnsafeNativeApiV1),
    WOTBMOD_V3_UNSAFE_NATIVE_VERSION,
    &UnsafeCreateAddressHook};

const WotbModV3AsyncApiV1 kAsyncApi = {
    sizeof(WotbModV3AsyncApiV1),
    WOTBMOD_V3_ASYNC_VERSION,
    &AsyncTaskSubmit,
    &AsyncTaskCancel,
    &AsyncTaskGetInfo,
    &AsyncTaskGetState,
    &AsyncTaskGetProgress,
    &AsyncTaskSetProgress,
    &AsyncTaskSetResult,
    &AsyncTaskGetResult,
    &AsyncTaskIsCancellationRequested,
    &AsyncDispatchMain,
    &AsyncDispatchRender,
    &AsyncDispatchAudio,
    &AsyncPumpCurrentThread,
    &AsyncTimerCreate,
    &AsyncTimerCancel,
    &AsyncTimerPause,
    &AsyncTimerResume,
    &AsyncTimerGetInfo,
    &AsyncGetThreadRole,
    &AsyncIsMainThread};

const WotbModV3HttpApiV1 kHttpApi = {
    sizeof(WotbModV3HttpApiV1),
    WOTBMOD_V3_HTTP_VERSION,
    &HttpRequestCreate,
    &HttpRequestSetMethod,
    &HttpRequestSetUrl,
    &HttpRequestSetHeader,
    &HttpRequestSetBody,
    &HttpRequestSetTimeout,
    &HttpRequestSetMaxResponse,
    &HttpRequestSendAsync,
    &HttpRequestCancel,
    &HttpRequestGetInfo,
    &HttpResponseGetStatus,
    &HttpResponseGetHeader,
    &HttpResponseGetBody};

const WotbModV3IntermodApiV1 kIntermodApi = {
    sizeof(WotbModV3IntermodApiV1),
    WOTBMOD_V3_INTERMOD_VERSION,
    &IntermodFind,
    &IntermodIsLoaded,
    &IntermodGetVersion,
    &IntermodGetDependency,
    &IntermodExport,
    &IntermodUnexport,
    &IntermodImport,
    &IntermodEnumerate,
    &IntermodMessagePublish,
    &IntermodMessageSubscribe,
    &IntermodMessageUnsubscribe};

void RegisterChecked(const InterfaceRegistration& registration) {
    const WotbModV3Result result = RegisterInterface(registration);
    if (result != WOTBMOD_V3_OK &&
        result != WOTBMOD_V3_E_ALREADY_EXISTS) {
        char message[WOTBMOD_V3_MAX_MESSAGE] = {};
#if defined(_MSC_VER)
        sprintf_s(
            message,
            "failed to register interface %s v%u (result=%u)",
            registration.name,
            registration.version,
            static_cast<unsigned>(result));
#else
        std::snprintf(
            message,
            sizeof(message),
            "failed to register interface %s v%u (result=%u)",
            registration.name,
            registration.version,
            static_cast<unsigned>(result));
#endif
        RuntimeLog(WOTBMOD_V3_LOG_ERROR, "v3.runtime-services", message);
    }
}

}  // namespace

WotbModV3Result SnapshotOwnedHooksForDevtools(
    WotbModV3Handle owner,
    const char* selector,
    std::vector<DevtoolsHookSnapshot>* out_snapshots) {
    if (!out_snapshots) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "hook inspector snapshot output is required");
    }
    out_snapshots->clear();
    const WotbModV3Result access = CheckAccess(
        owner,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    try {
        const std::string filter = selector ? selector : "";
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        for (const std::shared_ptr<HookRecord>& hook : g_hooks) {
            if (!hook || !hook->active.load() ||
                hook->owner != owner ||
                (!filter.empty() && hook->target != filter)) {
                continue;
            }
            DevtoolsHookSnapshot snapshot;
            snapshot.hook = hook->handle;
            snapshot.creation_order = hook->order;
            snapshot.mode = hook->mode;
            snapshot.status = hook->status;
            snapshot.priority = hook->priority;
            snapshot.conflict_count =
                static_cast<uint32_t>(hook->conflicts.size());
            snapshot.native_created = hook->native_created ? 1u : 0u;
            snapshot.native_enabled = hook->native_enabled ? 1u : 0u;
            snapshot.target = hook->target;
            out_snapshots->push_back(std::move(snapshot));
        }
        std::stable_sort(
            out_snapshots->begin(),
            out_snapshots->end(),
            [](const DevtoolsHookSnapshot& left,
               const DevtoolsHookSnapshot& right) {
                if (left.target != right.target) {
                    return left.target < right.target;
                }
                if (left.priority != right.priority) {
                    return left.priority > right.priority;
                }
                return left.creation_order < right.creation_order;
            });
        return WOTBMOD_V3_OK;
    } catch (const std::bad_alloc&) {
        out_snapshots->clear();
        return SetError(
            owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate hook inspector snapshot");
    }
}

WotbModV3Result SnapshotOwnedEventsForDevtools(
    WotbModV3Handle owner,
    const char* selector,
    std::vector<DevtoolsEventSnapshot>* out_snapshots) {
    if (!out_snapshots) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "event inspector snapshot output is required");
    }
    out_snapshots->clear();
    const WotbModV3Result access = CheckAccess(
        owner,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr);
    if (access != WOTBMOD_V3_OK) return access;
    try {
        const std::string filter = selector ? selector : "";
        std::lock_guard<std::mutex> registry_lock(g_event_mutex);
        for (const std::shared_ptr<EventSubscription>& subscription :
             g_event_subscriptions) {
            if (!subscription || subscription->owner != owner ||
                (!filter.empty() &&
                 subscription->pattern != filter)) {
                continue;
            }
            DevtoolsEventSnapshot snapshot;
            snapshot.token = subscription->handle;
            snapshot.creation_order = subscription->order;
            snapshot.priority = subscription->priority;
            snapshot.receive_system_events =
                subscription->receive_system ? 1u : 0u;
            snapshot.topic_pattern = subscription->pattern;
            {
                std::lock_guard<std::mutex> callback_lock(
                    subscription->callback_mutex);
                snapshot.active =
                    subscription->active.load() ? 1u : 0u;
                snapshot.accepting_callbacks =
                    subscription->accepting_callbacks ? 1u : 0u;
                snapshot.callbacks_in_flight =
                    static_cast<uint64_t>(
                        subscription->callbacks_in_flight);
            }
            out_snapshots->push_back(std::move(snapshot));
        }
        std::stable_sort(
            out_snapshots->begin(),
            out_snapshots->end(),
            [](const DevtoolsEventSnapshot& left,
               const DevtoolsEventSnapshot& right) {
                return left.creation_order < right.creation_order;
            });
        return WOTBMOD_V3_OK;
    } catch (const std::bad_alloc&) {
        out_snapshots->clear();
        return SetError(
            owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "unable to allocate event inspector snapshot");
    }
}

namespace testing {

#if defined(WOTBMOD_V3_RUNTIME_SERVICES_STANDALONE_TEST)
void ShutdownEventSubscriptionsForTesting() {
    ShutdownEventSubscriptions();
}

void ArmEventSubscribePublishBarrierForTesting() {
    std::lock_guard<std::mutex> lock(g_event_subscribe_test_mutex);
    g_event_subscribe_test_barrier_armed = true;
    g_event_subscribe_test_barrier_reached = false;
    g_event_subscribe_test_barrier_released = false;
}

bool WaitEventSubscribePublishBarrierForTesting(
    uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(g_event_subscribe_test_mutex);
    return g_event_subscribe_test_cv.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        []() {
            return g_event_subscribe_test_barrier_reached;
        });
}

void ReleaseEventSubscribePublishBarrierForTesting() {
    {
        std::lock_guard<std::mutex> lock(g_event_subscribe_test_mutex);
        g_event_subscribe_test_barrier_released = true;
    }
    g_event_subscribe_test_cv.notify_all();
}

bool EventSubscriptionsAcceptingForTesting() {
    std::lock_guard<std::mutex> lock(g_event_mutex);
    return g_event_accepting_subscriptions;
}
#endif

void SetHttpTransportForTesting(HttpTransportFn transport) {
    g_http_transport.store(transport ? transport : &WinHttpTransport);
}

void ResetHttpTransportForTesting() {
    g_http_transport.store(&WinHttpTransport);
}

bool IsPublicIpLiteralForTesting(const char* address) {
    if (!address || address[0] == '\0') return false;
    IN_ADDR address4 = {};
    if (InetPtonA(AF_INET, address, &address4) == 1) {
        return IsPublicIpv4(address4);
    }
    IN6_ADDR address6 = {};
    if (InetPtonA(AF_INET6, address, &address6) == 1) {
        return IsPublicIpv6(address6);
    }
    return false;
}

}  // namespace testing

WotbModV3Result EnqueueOwnedWorker(
    WotbModV3Handle owner,
    OwnedWorkerFn work,
    OwnedWorkerFn cancel,
    void* user_data) {
    if (!work) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "owned worker callback is required");
    }
    const WotbModV3Result check = CheckMod(owner);
    if (check != WOTBMOD_V3_OK) return check;

    QueuedCall call;
    call.owner = owner;
    call.invoke = [owner, work, cancel, user_data]() {
        CallbackScope scope(owner);
        if (!scope.entered()) {
            if (cancel) cancel(user_data);
            return;
        }
        work(user_data);
    };
    call.cancel = [cancel, user_data]() {
        if (cancel) cancel(user_data);
    };
    if (!EnqueueWorker(std::move(call))) {
        return SetError(
            owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "worker queue is full or shutting down");
    }
    return WOTBMOD_V3_OK;
}

void SetNativeHookBackend(
    const WotbModV3NativeHookBackend* backend) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    std::memset(
        &g_native_hook_backend,
        0,
        sizeof(g_native_hook_backend));
    const size_t required =
        offsetof(WotbModV3NativeHookBackend, remove) +
        sizeof(backend->remove);
    if (!backend ||
        backend->struct_size < required ||
        (backend->api_version != 1u &&
         backend->api_version !=
             WOTBMOD_V3_NATIVE_HOOK_BACKEND_VERSION)) {
        return;
    }
    // A version 2 table must carry every observer slot: a partial table
    // would let OBSERVE be promised and then fail at attach.
    if (backend->api_version >= 2u &&
        (backend->struct_size < sizeof(WotbModV3NativeHookBackend) ||
         !backend->describe_target || !backend->attach ||
         !backend->detach || !backend->set_attached_enabled)) {
        return;
    }
    const size_t copy_size =
        backend->struct_size < sizeof(g_native_hook_backend)
            ? backend->struct_size
            : sizeof(g_native_hook_backend);
    std::memcpy(
        &g_native_hook_backend,
        backend,
        copy_size);
    g_native_hook_backend.struct_size =
        sizeof(g_native_hook_backend);
}

WotbModV3Result PublishSystemEvent(
    const char* topic,
    const void* payload,
    uint32_t payload_size,
    uint32_t flags) {
    return PostEventInternal(
        WOTBMOD_V3_INVALID_HANDLE,
        topic,
        payload,
        payload_size,
        flags & ~static_cast<uint32_t>(WOTBMOD_V3_EVENT_FLAG_SYSTEM),
        true);
}

void SetMainIngressOnline(bool online) {
    g_main_ingress_online.store(online, std::memory_order_release);
    if (!online) g_main_role_cv.notify_all();
}

WotbModV3Result InvokeMainThreadBlocking(
    MainThreadCallFn callback,
    void* user_data,
    uint32_t timeout_ms) {
    if (!callback || timeout_ms == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (CurrentThreadRole() == WOTBMOD_V3_THREAD_MAIN) {
        try {
            return callback(user_data);
        } catch (...) {
            return WOTBMOD_V3_E_CALLBACK_FAULT;
        }
    }
    if (!g_main_ingress_online.load(std::memory_order_acquire)) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    const auto state = std::make_shared<BlockingMainCallState>();
    state->callback = callback;
    state->user_data = user_data;

    QueuedCall call;
    call.invoke = [state]() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->phase != BlockingMainCallPhase::QUEUED) {
                state->cv.notify_all();
                return;
            }
            state->phase = BlockingMainCallPhase::RUNNING;
        }

        WotbModV3Result result = WOTBMOD_V3_E_CALLBACK_FAULT;
        try {
            result = state->callback(state->user_data);
        } catch (...) {
            result = WOTBMOD_V3_E_CALLBACK_FAULT;
        }

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = result;
            state->phase = BlockingMainCallPhase::COMPLETED;
        }
        g_blocking_main_completion_epoch.fetch_add(
            1u, std::memory_order_release);
        state->cv.notify_all();
    };
    call.cancel = [state]() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->phase != BlockingMainCallPhase::QUEUED) return;
            state->result = WOTBMOD_V3_E_CANCELLED;
            state->phase = BlockingMainCallPhase::CANCELLED;
        }
        state->cv.notify_all();
    };

    if (!EnqueueRole(WOTBMOD_V3_THREAD_MAIN, std::move(call))) {
        return g_main_ingress_online.load(std::memory_order_acquire)
            ? WOTBMOD_V3_E_LIMIT_REACHED
            : WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    std::unique_lock<std::mutex> lock(state->mutex);
    const auto completed = [&state]() {
        return state->phase == BlockingMainCallPhase::COMPLETED ||
               state->phase == BlockingMainCallPhase::CANCELLED;
    };
    if (!state->cv.wait_for(
            lock, std::chrono::milliseconds(timeout_ms), completed)) {
        if (state->phase == BlockingMainCallPhase::QUEUED) {
            state->result = WOTBMOD_V3_E_TIMEOUT;
            state->phase = BlockingMainCallPhase::CANCELLED;
            return WOTBMOD_V3_E_TIMEOUT;
        }
        /*
         * The callback won the timeout race and already holds user_data.
         * Join it before returning so stack-backed call data cannot be used
         * after its owner has gone out of scope.
         */
        state->cv.wait(lock, completed);
    }
    return state->result;
}

WotbModV3Result PostMainThread(
    MainThreadCallFn callback,
    void* user_data) {
    return PostMainThreadEx(callback, user_data, nullptr);
}

WotbModV3Result PostMainThreadEx(
    MainThreadCallFn callback,
    void* user_data,
    MainThreadCancelFn cancel) {
    if (!callback) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!g_main_ingress_online.load(std::memory_order_acquire)) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    QueuedCall call;
    call.invoke = [callback, user_data]() {
        try {
            (void)callback(user_data);
        } catch (...) {
            RuntimeLog(
                WOTBMOD_V3_LOG_ERROR,
                "v3.runtime-services",
                "loader-private MAIN callback raised an exception");
        }
    };
    if (cancel) {
        call.cancel = [cancel, user_data]() {
            try {
                cancel(user_data);
            } catch (...) {
                RuntimeLog(
                    WOTBMOD_V3_LOG_ERROR,
                    "v3.runtime-services",
                    "loader-private MAIN cancellation raised an exception");
            }
        };
    }
    if (!EnqueueRole(WOTBMOD_V3_THREAD_MAIN, std::move(call))) {
        return g_main_ingress_online.load(std::memory_order_acquire)
            ? WOTBMOD_V3_E_LIMIT_REACHED
            : WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return WOTBMOD_V3_OK;
}

// Holds the MAIN pump's two pieces of per-thread state for the duration of
// one pump, and restores both on the way out however that happens.
//
// The guard has to survive an exception: a mod callback that throws through
// PumpRole would otherwise leave `pumping` stuck true and kill MAIN ingress
// on this thread permanently, silently. The role has to be restored for the
// same reason every other role-setting site restores it (WorkerLoop,
// TimerLoop, DispatchFrame): the pump's eventual host is a game callback, and
// if it is ever reached from inside DispatchFrame's window, leaking MAIN
// would leave the rest of that frame mis-labelled -- a mod calling
// pump_current_thread from its frame callback would drain MAIN instead of
// RENDER.
class MainPumpScope {
public:
    explicit MainPumpScope(bool* flag)
        : flag_(flag),
          previous_role_(
              SetCurrentThreadRole(WOTBMOD_V3_THREAD_MAIN)) {
        *flag_ = true;
    }

    ~MainPumpScope() {
        *flag_ = false;
        SetCurrentThreadRole(previous_role_);
    }

    MainPumpScope(const MainPumpScope&) = delete;
    MainPumpScope& operator=(const MainPumpScope&) = delete;

private:
    bool* flag_;
    uint32_t previous_role_;
};

uint32_t PumpMainThread(uint32_t max_callbacks) {
    if (!g_main_ingress_online.load(std::memory_order_acquire)) return 0u;
    /* The stock HUD extensions ride the same main-thread frame: resolve,
     * apply and re-assert before the queued callbacks run. */
    HudMainThreadTick();
    // Guards a pumped callback pumping again: the queue is not reentrant,
    // and the host site is a game callback we must return from promptly.
    static thread_local bool pumping = false;
    if (pumping) return 0u;
    MainPumpScope scope(&pumping);
    const uint32_t limit =
        max_callbacks == 0u ? kDefaultPumpLimit : max_callbacks;
    uint32_t executed = 0u;
    uint64_t completionEpoch =
        g_blocking_main_completion_epoch.load(std::memory_order_acquire);
    while (executed < limit) {
        executed += PumpRole(
            WOTBMOD_V3_THREAD_MAIN,
            WOTBMOD_V3_INVALID_HANDLE,
            limit - executed);
        const uint64_t currentEpoch =
            g_blocking_main_completion_epoch.load(
                std::memory_order_acquire);
        if (currentEpoch == completionEpoch || executed >= limit) break;
        completionEpoch = currentEpoch;

        /*
         * A render/Lua caller commonly performs a short synchronous chain
         * (create -> mutate -> release). The first completion wakes that
         * caller, but without a grace window this pump can observe an empty
         * queue in the scheduling gap and return, leaving the next call to
         * deadlock against the current Present. Wait on a CV, not a spin, and
         * only after a blocking callback actually completed.
         */
        std::unique_lock<std::mutex> lock(g_async_mutex);
        const bool followupReady = g_main_role_cv.wait_for(
            lock,
            std::chrono::milliseconds(4),
            []() {
                return g_async_stopping ||
                       !g_main_ingress_online.load(
                           std::memory_order_acquire) ||
                       !g_role_queues[WOTBMOD_V3_THREAD_MAIN].empty();
            });
        if (!followupReady || g_async_stopping ||
            !g_main_ingress_online.load(std::memory_order_acquire) ||
            g_role_queues[WOTBMOD_V3_THREAD_MAIN].empty()) {
            break;
        }
    }
    return executed;
}

void RegisterRuntimeServices() {
    SetEventSourceMask(0u);
    {
        std::lock_guard<std::mutex> lock(g_event_mutex);
        g_event_accepting_subscriptions = true;
    }
    StartAsyncRuntime();
    RegisterChecked(
        {WOTBMOD_V3_IFACE_LIFECYCLE,
         WOTBMOD_V3_LIFECYCLE_VERSION,
         &kLifecycleApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterChecked(
        {WOTBMOD_V3_IFACE_EVENTS,
         WOTBMOD_V3_EVENTS_VERSION,
         &kEventsApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterChecked(
        {WOTBMOD_V3_IFACE_HOOKS,
         WOTBMOD_V3_HOOKS_VERSION,
         &kHooksApi,
         WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterChecked(
        {WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
         WOTBMOD_V3_UNSAFE_NATIVE_VERSION,
         &kUnsafeNativeApi,
         WOTBMOD_V3_PERMISSION_UNSAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterChecked(
        {WOTBMOD_V3_IFACE_ASYNC,
         WOTBMOD_V3_ASYNC_VERSION,
         &kAsyncApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterChecked(
        {WOTBMOD_V3_IFACE_HTTP,
         WOTBMOD_V3_HTTP_VERSION,
         &kHttpApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    RegisterChecked(
        {WOTBMOD_V3_IFACE_INTERMOD,
         WOTBMOD_V3_INTERMOD_VERSION,
         &kIntermodApi,
         WOTBMOD_V3_PERMISSION_SAFE,
         WOTBMOD_V3_CONTEXT_ALL,
         nullptr});
    const WotbModV3Result frame_result =
        RegisterFramePump(&RuntimeServicesFramePump);
    if (frame_result != WOTBMOD_V3_OK &&
        frame_result != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.runtime-services",
            "failed to register runtime-services frame pump");
    }
    const WotbModV3Result shutdown_result =
        RegisterShutdownHook(&RuntimeServicesShutdown);
    if (shutdown_result != WOTBMOD_V3_OK &&
        shutdown_result != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.runtime-services",
            "failed to register runtime-services shutdown hook");
    }
    const WotbModV3Result owner_stopping_result =
        RegisterOwnerStoppingHook(&RuntimeServicesOwnerStopping);
    if (owner_stopping_result != WOTBMOD_V3_OK &&
        owner_stopping_result != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.runtime-services",
            "failed to register runtime-services owner stopping hook");
    }
    const WotbModV3Result lifecycle_state_result =
        RegisterLifecycleStateHook(&RuntimeServicesLifecycleState);
    if (lifecycle_state_result != WOTBMOD_V3_OK &&
        lifecycle_state_result != WOTBMOD_V3_E_ALREADY_EXISTS) {
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "v3.runtime-services",
            "failed to register runtime-services lifecycle state hook");
    }
}

}  // namespace v3
}  // namespace wotbmod
