#include "wotb_mod_runtime_v3.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_assertions = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_assertions;
    if (condition) return;
    ++g_failures;
    std::fprintf(stderr, "V3 CORE FAIL line %d: %s\n", line, expression);
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

template <typename T>
const T* Query(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    uint32_t version) {
    const void* table = nullptr;
    const WotbModV3Result result =
        bootstrap->query_interface(mod, name, version, &table);
    CHECK(result == WOTBMOD_V3_OK);
    CHECK(table != nullptr);
    return static_cast<const T*>(table);
}

void CopyText(char* destination, size_t capacity, const char* text) {
    if (!destination || capacity == 0u) return;
    destination[0] = '\0';
    if (!text) return;
    strncpy_s(destination, capacity, text, _TRUNCATE);
}

struct FrameBarrier {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
};

FrameBarrier g_frame_barrier;
std::atomic<bool> g_frame_active(false);
std::atomic<int> g_enable_calls(0);
std::atomic<int> g_disable_calls(0);
std::atomic<int> g_unload_calls(0);
std::atomic<bool> g_disable_observed_frame_exit(false);
std::atomic<int> g_cleanup_calls(0);

void WOTBMOD_V3_CALL SubjectEnable(
    const WotbModV3Bootstrap*,
    WotbModV3Handle) {
    g_enable_calls.fetch_add(1);
}

void WOTBMOD_V3_CALL SubjectDisable(
    const WotbModV3Bootstrap*,
    WotbModV3Handle) {
    g_disable_observed_frame_exit.store(!g_frame_active.load());
    g_disable_calls.fetch_add(1);
}

void WOTBMOD_V3_CALL SubjectUnload(
    const WotbModV3Bootstrap*,
    WotbModV3Handle) {
    g_unload_calls.fetch_add(1);
}

void WOTBMOD_V3_CALL SubjectFrame(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    uint64_t,
    double) {
    g_frame_active.store(true);
    std::unique_lock<std::mutex> lock(g_frame_barrier.mutex);
    g_frame_barrier.entered = true;
    g_frame_barrier.condition.notify_all();
    g_frame_barrier.condition.wait(
        lock,
        []() { return g_frame_barrier.release; });
    g_frame_active.store(false);
}

void WOTBMOD_V3_CALL CleanupCallback(
    WotbModV3Handle,
    uint32_t,
    void*) {
    g_cleanup_calls.fetch_add(1);
}

WotbModV3Result FillModInfo(
    WotbModV3Info* out_info,
    const char* id,
    const char* name,
    WotbModV3LifecycleCallback on_enable,
    WotbModV3LifecycleCallback on_disable,
    WotbModV3LifecycleCallback on_unload,
    WotbModV3FrameCallback on_frame) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    CopyText(out_info->id, sizeof(out_info->id), id);
    CopyText(out_info->name, sizeof(out_info->name), name);
    CopyText(out_info->version, sizeof(out_info->version), "1.0.0");
    CopyText(out_info->author, sizeof(out_info->author), "V3 core tests");
    CopyText(
        out_info->description,
        sizeof(out_info->description),
        "Strict ABI, ownership, lifecycle, and unload-barrier probe");
    out_info->on_enable = on_enable;
    out_info->on_disable = on_disable;
    out_info->on_unload = on_unload;
    out_info->on_frame = on_frame;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ObserverEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* out_info) {
    return FillModInfo(
        out_info,
        "v3_core_observer",
        "V3 Core Observer",
        nullptr,
        nullptr,
        nullptr,
        nullptr);
}

WotbModV3Result WOTBMOD_V3_CALL SubjectEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* out_info) {
    return FillModInfo(
        out_info,
        "v3_core_subject",
        "V3 Core Subject",
        SubjectEnable,
        SubjectDisable,
        SubjectUnload,
        SubjectFrame);
}

struct LifecycleTrace {
    std::mutex mutex;
    std::vector<uint32_t> transitions;
    WotbModV3Handle target = WOTBMOD_V3_INVALID_HANDLE;
};

void WOTBMOD_V3_CALL LifecycleEventCallback(
    WotbModV3Handle,
    WotbModV3Event* event,
    void* user_data) {
    LifecycleTrace* trace = static_cast<LifecycleTrace*>(user_data);
    if (!trace || !event || !event->payload ||
        event->payload_size < sizeof(WotbModV3LifecycleEvent)) {
        return;
    }
    const WotbModV3LifecycleEvent* lifecycle =
        static_cast<const WotbModV3LifecycleEvent*>(event->payload);
    std::lock_guard<std::mutex> lock(trace->mutex);
    if (trace->target == WOTBMOD_V3_INVALID_HANDLE ||
        lifecycle->mod == trace->target) {
        trace->transitions.push_back(lifecycle->transition);
    }
}

void ResetGlobals() {
    g_enable_calls.store(0);
    g_disable_calls.store(0);
    g_unload_calls.store(0);
    g_cleanup_calls.store(0);
    g_frame_active.store(false);
    g_disable_observed_frame_exit.store(false);
    std::lock_guard<std::mutex> lock(g_frame_barrier.mutex);
    g_frame_barrier.entered = false;
    g_frame_barrier.release = false;
}

void TestUnknownInterfacesAndPermissions(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle safe_mod) {
    const void* table = reinterpret_cast<const void*>(1);
    CHECK(
        bootstrap->query_interface(
            safe_mod, "wotbmod.does.not.exist", 1u, &table) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(table == nullptr);

    table = reinterpret_cast<const void*>(1);
    CHECK(
        bootstrap->query_interface(
            safe_mod,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION + 1u,
            &table) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(table == nullptr);

    WotbModV3ErrorInfo error = {};
    error.struct_size = sizeof(error);
    CHECK(bootstrap->get_last_error(safe_mod, &error) == WOTBMOD_V3_OK);
    CHECK(error.code == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(error.owner_mod == safe_mod);
    CHECK(error.message[0] != '\0');

    WotbModV3InterfaceInfo interface_info = {};
    interface_info.struct_size = sizeof(interface_info);
    CHECK(
        bootstrap->get_interface_info(
            safe_mod,
            "wotbmod.does.not.exist",
            &interface_info) == WOTBMOD_V3_E_NOT_FOUND);

    table = nullptr;
    CHECK(
        bootstrap->query_interface(
            safe_mod,
            WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
            WOTBMOD_V3_UNSAFE_NATIVE_VERSION,
            &table) == WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(table == nullptr);

    interface_info = {};
    interface_info.struct_size = sizeof(interface_info);
    CHECK(
        bootstrap->get_interface_info(
            safe_mod,
            WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
            &interface_info) == WOTBMOD_V3_OK);
    CHECK(
        interface_info.status ==
        WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED);

    const WotbModV3PermissionsApiV1* permissions =
        Query<WotbModV3PermissionsApiV1>(
            bootstrap,
            safe_mod,
            WOTBMOD_V3_IFACE_PERMISSIONS,
            WOTBMOD_V3_PERMISSIONS_VERSION);
    uint32_t tier = UINT32_MAX;
    CHECK(permissions->get_granted_tier(safe_mod, &tier) == WOTBMOD_V3_OK);
    CHECK(tier == WOTBMOD_V3_PERMISSION_SAFE);

    WotbModV3PermissionInfo permission = {};
    permission.struct_size = sizeof(permission);
    CHECK(
        permissions->query(safe_mod, "native.memory", &permission) ==
        WOTBMOD_V3_OK);
    CHECK(permission.tier == WOTBMOD_V3_PERMISSION_UNSAFE);
    CHECK(permission.state == WOTBMOD_V3_PERMISSION_STATE_DENIED);

    permission = {};
    permission.struct_size = sizeof(permission);
    CHECK(
        permissions->query(safe_mod, "core", &permission) ==
        WOTBMOD_V3_OK);
    CHECK(permission.state == WOTBMOD_V3_PERMISSION_STATE_GRANTED);

    permission = {};
    permission.struct_size = sizeof(permission);
    CHECK(
        permissions->query(safe_mod, "not.a.permission", &permission) ==
        WOTBMOD_V3_E_NOT_FOUND);
}

void TestHandlesAndDynamicPermission(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle owner,
    WotbModV3Handle foreign_owner,
    const WotbModV3LifecycleApiV1* lifecycle) {
    const WotbModV3HandlesApiV1* handles =
        Query<WotbModV3HandlesApiV1>(
            bootstrap,
            owner,
            WOTBMOD_V3_IFACE_HANDLES,
            WOTBMOD_V3_HANDLES_VERSION);

    WotbModV3Token first = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        lifecycle->register_cleanup(
            owner, CleanupCallback, nullptr, &first) == WOTBMOD_V3_OK);
    CHECK(first != WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3HandleInfo info = {};
    info.struct_size = sizeof(info);
    CHECK(handles->get_info(owner, first, &info) == WOTBMOD_V3_OK);
    CHECK(info.type == WOTBMOD_V3_HANDLE_LIFECYCLE_CLEANUP);
    CHECK(info.owner_mod == owner);
    CHECK(info.reference_count == 1u);
    const uint32_t first_generation = info.generation;

    info = {};
    info.struct_size = sizeof(info);
    CHECK(
        handles->get_info(foreign_owner, first, &info) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        handles->retain(foreign_owner, first) ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    CHECK(handles->retain(owner, first) == WOTBMOD_V3_OK);
    info = {};
    info.struct_size = sizeof(info);
    CHECK(handles->get_info(owner, first, &info) == WOTBMOD_V3_OK);
    CHECK(info.reference_count == 2u);
    CHECK(handles->release(owner, first) == WOTBMOD_V3_OK);
    CHECK(handles->release(owner, first) == WOTBMOD_V3_OK);
    CHECK(g_cleanup_calls.load() == 1);

    uint32_t alive = 1u;
    CHECK(handles->is_alive(owner, first, &alive) == WOTBMOD_V3_OK);
    CHECK(alive == 0u);
    info = {};
    info.struct_size = sizeof(info);
    CHECK(
        handles->get_info(owner, first, &info) ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    WotbModV3Token replacement = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        lifecycle->register_cleanup(
            owner, CleanupCallback, nullptr, &replacement) ==
        WOTBMOD_V3_OK);
    CHECK(replacement != first);
    info = {};
    info.struct_size = sizeof(info);
    CHECK(handles->get_info(owner, replacement, &info) == WOTBMOD_V3_OK);
    CHECK(info.generation != first_generation);
    CHECK(
        handles->retain(owner, first) ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    const void* unsafe_api = nullptr;
    CHECK(
        bootstrap->query_interface(
            owner,
            WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
            WOTBMOD_V3_UNSAFE_NATIVE_VERSION,
            &unsafe_api) == WOTBMOD_V3_OK);
    CHECK(unsafe_api != nullptr);

    CHECK(
        WotbModV3Runtime_SetPermissionTier(
            owner, WOTBMOD_V3_PERMISSION_SAFE) == WOTBMOD_V3_OK);
    alive = 1u;
    CHECK(handles->is_alive(owner, replacement, &alive) == WOTBMOD_V3_OK);
    CHECK(alive == 0u);
    CHECK(g_cleanup_calls.load() == 2);

    unsafe_api = reinterpret_cast<const void*>(1);
    CHECK(
        bootstrap->query_interface(
            owner,
            WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
            WOTBMOD_V3_UNSAFE_NATIVE_VERSION,
            &unsafe_api) == WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(unsafe_api == nullptr);
}

void TestDisableBarrier(WotbModV3Handle subject) {
    CHECK(WotbModV3Runtime_Enable(subject) == WOTBMOD_V3_OK);
    CHECK(g_enable_calls.load() == 1);

    std::thread frame_thread([]() {
        WotbModV3Runtime_DispatchFrame(77u, 1.0 / 60.0);
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(g_frame_barrier.mutex);
        entered = g_frame_barrier.condition.wait_for(
            lock,
            std::chrono::seconds(3),
            []() { return g_frame_barrier.entered; });
    }
    CHECK(entered);

    std::atomic<bool> disable_started(false);
    std::atomic<bool> disable_finished(false);
    WotbModV3Result disable_result = WOTBMOD_V3_E_TIMEOUT;
    std::thread disable_thread([&]() {
        disable_started.store(true);
        disable_result = WotbModV3Runtime_Disable(subject);
        disable_finished.store(true);
    });

    const auto start_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!disable_started.load() &&
           std::chrono::steady_clock::now() < start_deadline) {
        std::this_thread::yield();
    }
    CHECK(disable_started.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!disable_finished.load());

    {
        std::lock_guard<std::mutex> lock(g_frame_barrier.mutex);
        g_frame_barrier.release = true;
    }
    g_frame_barrier.condition.notify_all();
    frame_thread.join();
    disable_thread.join();

    CHECK(disable_result == WOTBMOD_V3_OK);
    CHECK(disable_finished.load());
    CHECK(g_disable_calls.load() == 1);
    CHECK(g_disable_observed_frame_exit.load());
}

}  // namespace

int main(int argc, char** argv) {
    const char* root = argc > 1 ? argv[1] : "build\\v3_core_env";
    ResetGlobals();

    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = root;
    options.client_version = "11.19.0.834-test";
    options.executable_sha256 =
        "41960DBD8D1ACE21F24EBCCBEC8C093E61AFD5DDB9A04AD398198F5B3162E0AD";
    options.binding_pack_version = 1u;
    options.process_architecture = 32u;

    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);
    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);
    CHECK(bootstrap->api_version == WOTBMOD_V3_ABI_VERSION);
    CHECK(bootstrap->sdk_version == WOTBMOD_V3_SDK_VERSION);
    CHECK(
        bootstrap->bootstrap_version ==
        WOTBMOD_V3_BOOTSTRAP_VERSION);

    WotbModV3Handle observer = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "observer.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &observer) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo observer_info = {};
    observer_info.struct_size = sizeof(observer_info);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            observer, ObserverEntry, &observer_info) == WOTBMOD_V3_OK);
    CHECK(observer_info.state == WOTBMOD_V3_MOD_STATE_LOADED);
    CHECK(WotbModV3Runtime_Enable(observer) == WOTBMOD_V3_OK);

    TestUnknownInterfacesAndPermissions(bootstrap, observer);

    const WotbModV3EventsApiV1* events =
        Query<WotbModV3EventsApiV1>(
            bootstrap,
            observer,
            WOTBMOD_V3_IFACE_EVENTS,
            WOTBMOD_V3_EVENTS_VERSION);
    LifecycleTrace trace;
    WotbModV3EventSubscriptionInfo subscription_info = {};
    subscription_info.struct_size = sizeof(subscription_info);
    subscription_info.api_version = WOTBMOD_V3_EVENTS_VERSION;
    subscription_info.topic_pattern = "wotbmod.mod.*";
    subscription_info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    subscription_info.receive_system_events = 1u;
    WotbModV3EventToken lifecycle_subscription =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        events->subscribe(
            observer,
            &subscription_info,
            LifecycleEventCallback,
            &trace,
            &lifecycle_subscription) == WOTBMOD_V3_OK);

    WotbModV3Handle subject = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "subject.dll",
            WOTBMOD_V3_PERMISSION_UNSAFE,
            &subject) == WOTBMOD_V3_OK);
    {
        std::lock_guard<std::mutex> lock(trace.mutex);
        trace.target = subject;
    }

    WotbModV3RuntimeModuleInfo subject_info = {};
    subject_info.struct_size = sizeof(subject_info);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            subject, SubjectEntry, &subject_info) == WOTBMOD_V3_OK);
    CHECK(subject_info.mod == subject);
    CHECK(subject_info.permission_tier == WOTBMOD_V3_PERMISSION_UNSAFE);
    CHECK(subject_info.state == WOTBMOD_V3_MOD_STATE_LOADED);
    CHECK(std::strcmp(subject_info.id, "v3_core_subject") == 0);

    const WotbModV3LifecycleApiV1* lifecycle =
        Query<WotbModV3LifecycleApiV1>(
            bootstrap,
            subject,
            WOTBMOD_V3_IFACE_LIFECYCLE,
            WOTBMOD_V3_LIFECYCLE_VERSION);
    WotbModV3LifecycleInfo lifecycle_info = {};
    lifecycle_info.struct_size = sizeof(lifecycle_info);
    lifecycle_info.api_version = WOTBMOD_V3_LIFECYCLE_VERSION;
    CHECK(
        lifecycle->get_info(subject, &lifecycle_info) ==
        WOTBMOD_V3_OK);
    CHECK(lifecycle_info.state == WOTBMOD_V3_MOD_STATE_LOADED);

    TestHandlesAndDynamicPermission(
        bootstrap, subject, observer, lifecycle);
    TestDisableBarrier(subject);

    lifecycle_info = {};
    lifecycle_info.struct_size = sizeof(lifecycle_info);
    lifecycle_info.api_version = WOTBMOD_V3_LIFECYCLE_VERSION;
    CHECK(
        lifecycle->get_info(subject, &lifecycle_info) ==
        WOTBMOD_V3_OK);
    CHECK(lifecycle_info.state == WOTBMOD_V3_MOD_STATE_DISABLED);

    CHECK(WotbModV3Runtime_DestroyMod(subject) == WOTBMOD_V3_OK);
    CHECK(g_unload_calls.load() == 1);
    CHECK(
        bootstrap->query_interface(
            subject,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION,
            reinterpret_cast<const void**>(&lifecycle)) ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    std::vector<uint32_t> transitions;
    {
        std::lock_guard<std::mutex> lock(trace.mutex);
        transitions = trace.transitions;
    }
    const uint32_t expected[] = {
        WOTBMOD_V3_MOD_TRANSITION_PRELOAD,
        WOTBMOD_V3_MOD_TRANSITION_LOADED,
        WOTBMOD_V3_MOD_TRANSITION_ENABLED,
        WOTBMOD_V3_MOD_TRANSITION_DISABLED,
        WOTBMOD_V3_MOD_TRANSITION_UNLOADING,
        WOTBMOD_V3_MOD_TRANSITION_UNLOADED};
    CHECK(transitions.size() == sizeof(expected) / sizeof(expected[0]));
    if (transitions.size() == sizeof(expected) / sizeof(expected[0])) {
        for (size_t index = 0u;
             index < sizeof(expected) / sizeof(expected[0]);
             ++index) {
            CHECK(transitions[index] == expected[index]);
        }
    }

    WotbModV3Handle replacement_mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "replacement.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &replacement_mod) == WOTBMOD_V3_OK);
    CHECK(replacement_mod != subject);
    CHECK(WotbModV3Runtime_DestroyMod(replacement_mod) == WOTBMOD_V3_OK);

    CHECK(
        events->unsubscribe(observer, lifecycle_subscription) ==
        WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Disable(observer) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(observer) == WOTBMOD_V3_OK);
    WotbModV3Runtime_Shutdown();
    CHECK(WotbModV3Runtime_GetBootstrap() == nullptr);

    if (g_failures != 0) {
        std::fprintf(
            stderr,
            "V3 CORE FAILED: assertions=%d failures=%d\n",
            g_assertions,
            g_failures);
        return 1;
    }
    std::printf(
        "V3 CORE OK: assertions=%d failures=0\n",
        g_assertions);
    return 0;
}
