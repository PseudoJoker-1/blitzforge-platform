#pragma once

#include <stdint.h>
#include <string>
#include <vector>

#include "../../include/wotb_mod_runtime_v3.h"

namespace wotbmod {
namespace v3 {

typedef void (*HandleDestroyFn)(void* object);
typedef void (*FramePumpFn)(uint64_t frame_index, double delta_seconds);
typedef void (*ShutdownHookFn)();
typedef void (*OwnerStoppingHookFn)(WotbModV3Handle owner);
typedef void (*LifecycleStateHookFn)(
    WotbModV3Handle mod,
    uint32_t transition);
typedef void (*OwnedWorkerFn)(void* user_data);

enum LifecycleTransition {
    LIFECYCLE_TRANSITION_PRELOAD = 0,
    LIFECYCLE_TRANSITION_LOADED = 1,
    LIFECYCLE_TRANSITION_ENABLED = 2,
    LIFECYCLE_TRANSITION_DISABLED = 3,
    LIFECYCLE_TRANSITION_UNLOADING = 4,
    LIFECYCLE_TRANSITION_UNLOADED = 5
};

struct InterfaceRegistration {
    const char* name;
    uint32_t version;
    const void* table;
    uint32_t required_permission_tier;
    uint64_t allowed_contexts;
    const char* capability_name;
    uint32_t implementation_status = 0xFFFFFFFFu;
    const char* implementation_reason = nullptr;
};

struct RuntimeStats {
    uint32_t loaded_mods;
    uint32_t enabled_mods;
    uint32_t live_handles;
    uint32_t registered_interfaces;
    uint32_t registered_capabilities;
    uint64_t frame_index;
    uint64_t context_mask;
};

/*
 * Portable, owner-filtered DevTools snapshots. These records intentionally
 * contain only stable public metadata and never retain callbacks, user-data,
 * native object pointers, detours, or original function pointers.
 */
struct DevtoolsHookSnapshot {
    WotbModV3HookHandle hook = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t creation_order = 0u;
    uint32_t mode = 0u;
    uint32_t status = 0u;
    int32_t priority = 0;
    uint32_t conflict_count = 0u;
    uint32_t native_created = 0u;
    uint32_t native_enabled = 0u;
    std::string target;
};

struct DevtoolsEventSnapshot {
    WotbModV3EventToken token = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t creation_order = 0u;
    int32_t priority = 0;
    uint32_t receive_system_events = 0u;
    uint32_t active = 0u;
    uint32_t accepting_callbacks = 0u;
    uint64_t callbacks_in_flight = 0u;
    std::string topic_pattern;
};

struct DevtoolsResourceSnapshot {
    WotbModV3ResourceHandle resource = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t type = 0u;
    uint32_t state = 0u;
    uint32_t backing = 0u;
    uint32_t operation_in_progress = 0u;
    uint64_t memory_bytes = 0u;
    std::string uri;
    std::string group;
    std::string sha256;
    std::string error;
};

WotbModV3Result RegisterInterface(
    const InterfaceRegistration& registration);
WotbModV3Result SetInterfaceAvailability(
    const char* interface_name,
    uint32_t status,
    const char* reason);
WotbModV3Result SetError(
    WotbModV3Handle mod,
    WotbModV3Result code,
    const char* message,
    const char* context_json = nullptr);
WotbModV3Result CheckMod(WotbModV3Handle mod);
WotbModV3Result CheckAccess(
    WotbModV3Handle mod,
    uint32_t required_permission_tier,
    uint64_t allowed_contexts,
    const char* capability_name);
WotbModV3Result CheckNamedPermission(
    WotbModV3Handle mod,
    const char* permission_name,
    uint32_t required_permission_tier);
WotbModV3Result CreateOwnedHandle(
    WotbModV3Handle owner,
    uint32_t type,
    void* object,
    HandleDestroyFn destroy,
    WotbModV3Handle* out_handle);
WotbModV3Result RetainOwnedHandle(
    WotbModV3Handle owner,
    WotbModV3Handle handle);
WotbModV3Result ReleaseOwnedHandle(
    WotbModV3Handle owner,
    WotbModV3Handle handle);
WotbModV3Result InspectOwnedHandle(
    WotbModV3Handle owner,
    WotbModV3Handle handle,
    uint32_t expected_type,
    void** out_object,
    WotbModV3HandleInfo* out_info);
void ReleaseAllOwnedHandles(WotbModV3Handle owner);
WotbModV3Result EnterModCallback(WotbModV3Handle mod);
WotbModV3Result EnterModCallbackCoalescible(WotbModV3Handle mod);
void LeaveModCallback(WotbModV3Handle mod);
WotbModV3Result GetModCallbackProfile(
    WotbModV3Handle mod,
    WotbModV3CallbackProfile* out_profile);
WotbModV3Result SetModCallbackBudget(
    WotbModV3Handle mod,
    uint32_t budget_microseconds,
    uint32_t max_callbacks_per_frame);
WotbModV3Result ResetModCallbackProfile(WotbModV3Handle mod);
bool IsModEnabled(WotbModV3Handle mod);
uint32_t ModState(WotbModV3Handle mod);
/*
 * Enable-transition window. The host must create some of a mod's owned
 * resources -- its package mount -- before on_enable runs, because
 * on_enable reads from that mount. Between the two the mod is still
 * disabled, and CreateOwnedHandle refuses disabled owners, so the host
 * opens this window for exactly the span between them.
 *
 * The exemption is scoped to the calling thread and to the one mod, and
 * does not change the mod's state: a disabled mod's own threads keep
 * getting WOTBMOD_V3_E_CANCELLED. Always pair Begin with End, on the same
 * thread, ideally through a scope guard.
 */
WotbModV3Result BeginModEnableTransition(WotbModV3Handle mod);
void EndModEnableTransition(WotbModV3Handle mod);
uint32_t ModPermissionTier(WotbModV3Handle mod);
const char* ModId(WotbModV3Handle mod);
WotbModV3Result FindModById(
    const char* id,
    WotbModV3Handle* out_mod);
WotbModV3Result GetModVersion(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t buffer_size);
WotbModV3Result GetModInstallDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size);
WotbModV3Result GetModResourceDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size);
WotbModV3Result ResolveDeclaredDependency(
    WotbModV3Handle mod,
    const char* dependency_id,
    WotbModV3Handle* out_dependency,
    uint32_t* out_optional);

uint64_t CurrentContext();
uint64_t CurrentFrameIndex();
void ObserveExternalFrame(uint64_t frame_index);
uint32_t CurrentThreadRole();
uint32_t SetCurrentThreadRole(uint32_t role);
const char* GameDirectory();
const char* ModsDirectory();
const char* CacheDirectory();
const char* ConfigDirectory();
WotbModV3Result GetModDataDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size);
WotbModV3Result GetModCacheDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size);
WotbModV3Result GetModConfigDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size);
void RuntimeLog(
    uint32_t level,
    const char* category,
    const char* message);
WotbModV3Result RegisterFramePump(FramePumpFn pump);
/*
 * MAIN-thread async ingress. Off until the loader installs a per-frame
 * main-thread hook and declares it online; while off, dispatch_to_main_thread
 * refuses honestly instead of queueing work that would never run.
 */
void SetMainIngressOnline(bool online);
uint32_t PumpMainThread(uint32_t max_callbacks);
typedef WotbModV3Result (WOTBMOD_V3_CALL* MainThreadCallFn)(
    void* user_data);
typedef void (WOTBMOD_V3_CALL* MainThreadCancelFn)(
    void* user_data);
/*
 * Loader-private synchronous MAIN ingress. The callback runs inline when the
 * caller is already inside the proven MAIN pump; otherwise it is queued and
 * the caller blocks until completion, cancellation, or timeout. A callback
 * which has started is always joined before this function returns so its
 * user_data may safely refer to caller-owned stack storage.
 */
WotbModV3Result InvokeMainThreadBlocking(
    MainThreadCallFn callback,
    void* user_data,
    uint32_t timeout_ms);
/*
 * Loader-private non-blocking MAIN ingress. The callback is always queued,
 * including when the caller already has the MAIN role, and may therefore run
 * after this function returns. user_data must remain valid until the callback
 * runs or the async runtime shuts down. Shutdown cancels a queued callback
 * without invoking it.
 */
WotbModV3Result PostMainThread(
    MainThreadCallFn callback,
    void* user_data);
/*
 * Variant for heap-backed jobs. cancel is called exactly once when a queued
 * callback is discarded during runtime shutdown; it is not called after the
 * callback starts. This lets private bridges release job storage without
 * leaking when the client exits between enqueue and pump.
 */
WotbModV3Result PostMainThreadEx(
    MainThreadCallFn callback,
    void* user_data,
    MainThreadCancelFn cancel);
WotbModV3Result RegisterShutdownHook(ShutdownHookFn hook);
WotbModV3Result RegisterOwnerStoppingHook(OwnerStoppingHookFn hook);
WotbModV3Result RegisterLifecycleStateHook(LifecycleStateHookFn hook);
WotbModV3Result EnqueueOwnedWorker(
    WotbModV3Handle owner,
    OwnedWorkerFn work,
    OwnedWorkerFn cancel,
    void* user_data);
WotbModV3Result PublishSystemEvent(
    const char* topic,
    const void* payload,
    uint32_t payload_size,
    uint32_t flags);
void GetRuntimeStats(RuntimeStats* out_stats);
WotbModV3Result SnapshotOwnedHooksForDevtools(
    WotbModV3Handle owner,
    const char* selector,
    std::vector<DevtoolsHookSnapshot>* out_snapshots);
WotbModV3Result SnapshotOwnedEventsForDevtools(
    WotbModV3Handle owner,
    const char* selector,
    std::vector<DevtoolsEventSnapshot>* out_snapshots);
WotbModV3Result SnapshotOwnedResourcesForDevtools(
    WotbModV3Handle owner,
    const char* selector,
    std::vector<DevtoolsResourceSnapshot>* out_snapshots);

void RegisterCoreServices();
void SetNativeHookBackend(
    const WotbModV3NativeHookBackend* backend);
void SetEventSourceMask(uint64_t source_mask);
uint64_t GetEventSourceMask();
void RegisterRuntimeServices();
void RegisterDataServices();
void RegisterClientServices();
void RegisterToolingServices();

}  // namespace v3
}  // namespace wotbmod
