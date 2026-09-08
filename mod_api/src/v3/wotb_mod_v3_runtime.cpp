#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../../include/wotbmod/capabilities_v1.h"
#include "../../include/wotbmod/core_v1.h"
#include "../../include/wotbmod/handles_v1.h"
#include "../../include/wotbmod/interface_ids.h"
#include "../../include/wotbmod/permissions_v1.h"
#include "wotb_mod_v3_internal.h"

namespace wotbmod {
namespace v3 {
namespace {

constexpr uint32_t kMaxMods = 128u;
constexpr uint32_t kMaxHandles = 4096u;
constexpr uint32_t kMaxInterfaces = 128u;
constexpr uint32_t kMaxCapabilities = 256u;
constexpr uint32_t kMaxFramePumps = 32u;
constexpr uint32_t kMaxShutdownHooks = 32u;
constexpr uint32_t kMaxOwnerStoppingHooks = 32u;
constexpr uint32_t kMaxLifecycleStateHooks = 32u;
constexpr uint32_t kDefaultCallbackBudgetMicroseconds = 4000u;
constexpr uint32_t kDefaultCallbacksPerFrame = 256u;
constexpr uint64_t kSlowCallback100ns = 10000u; /* 1 ms */
constexpr uint64_t kHandleMagic = 0xB3Dull << 52u;
constexpr uint64_t kHandleMagicMask = 0xFFFull << 52u;
constexpr uint64_t kHandleSlotMask = 0xFFFFFull;
constexpr uint64_t kHandleGenerationMask = 0xFFFFFFull;
constexpr uint32_t kHandleGenerationShift = 20u;
constexpr uint32_t kHandleTypeShift = 44u;

enum ModStateValue {
    MOD_STATE_CREATED = 0,
    MOD_STATE_LOADED = 1,
    MOD_STATE_ENABLED = 2,
    MOD_STATE_DISABLED = 3,
    MOD_STATE_UNLOADING = 4,
    MOD_STATE_FAULTED = 5
};

struct HandleRecord {
    bool alive = false;
    uint32_t generation = 1u;
    uint32_t type = WOTBMOD_V3_HANDLE_UNKNOWN;
    uint32_t references = 0u;
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    void* object = nullptr;
    HandleDestroyFn destroy = nullptr;
};

struct ModRecord {
    bool active = false;
    bool enabled = false;
    bool accepting_callbacks = false;
    bool named_permissions_restricted = false;
    uint32_t permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    uint32_t state = MOD_STATE_CREATED;
    uint32_t callbacks_in_flight = 0u;
    uint32_t callback_budget_microseconds =
        kDefaultCallbackBudgetMicroseconds;
    uint32_t max_callbacks_per_frame =
        kDefaultCallbacksPerFrame;
    uint64_t callback_profile_frame = UINT64_MAX;
    uint64_t frame_callbacks = 0u;
    uint64_t frame_cpu_100ns = 0u;
    uint64_t previous_frame_callbacks = 0u;
    uint64_t previous_frame_cpu_100ns = 0u;
    uint64_t total_callbacks = 0u;
    uint64_t total_callback_cpu_100ns = 0u;
    uint64_t max_callback_100ns = 0u;
    uint64_t slow_callbacks = 0u;
    uint64_t coalesced_callbacks = 0u;
    uint64_t over_budget_frames = 0u;
    bool frame_over_budget = false;
    /*
     * The enable-transition window. The host has to build a mod's package
     * mount *before* on_enable runs, because on_enable is expected to read
     * files out of that mount; mounting creates handles owned by the mod,
     * and until on_enable starts the mod is still disabled, so
     * CreateOwnedHandle would refuse it. These two fields are that gap,
     * held open explicitly and narrowly.
     *
     * Depth, not a bool, so nested opens by the same thread unwind in the
     * right order. Thread id, not just a flag, so the exemption belongs to
     * the caller that opened it: a disabled mod's own background thread
     * still gets refused, which is the whole point of the check.
     */
    uint32_t enable_transition_depth = 0u;
    std::thread::id enable_transition_thread;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    std::string module_path;
    std::string install_directory;
    std::string resource_directory;
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string data_directory;
    std::string cache_directory;
    std::string config_directory;
    std::vector<WotbModV3ManifestDependency> dependencies;
    std::vector<std::string> named_permission_grants;
    WotbModV3Info callbacks = {};
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
};

struct InterfaceRecord {
    std::string name;
    uint32_t version = 0u;
    const void* table = nullptr;
    uint32_t required_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    uint64_t allowed_contexts = WOTBMOD_V3_CONTEXT_ALL;
    std::string capability_name;
    uint32_t implementation_status = WOTBMOD_V3_CAPABILITY_DEGRADED;
    std::string implementation_reason;
};

struct CapabilityRecord {
    WotbModV3CapabilityInfo info = {};
    bool interface_derived = false;
};

struct CapabilitySubscription {
    WotbModV3CapabilityChangedCallback callback = nullptr;
    void* user_data = nullptr;
};

struct PermissionDefinition {
    const char* name;
    uint32_t tier;
};

const PermissionDefinition kPermissions[] = {
    {"core", WOTBMOD_V3_PERMISSION_SAFE},
    {"ui", WOTBMOD_V3_PERMISSION_SAFE},
    {"ui.create", WOTBMOD_V3_PERMISSION_SAFE},
    {"ui.modify.own", WOTBMOD_V3_PERMISSION_SAFE},
    {"localization", WOTBMOD_V3_PERMISSION_SAFE},
    {"audio", WOTBMOD_V3_PERMISSION_SAFE},
    {"audio.custom", WOTBMOD_V3_PERMISSION_SAFE},
    {"audio.events", WOTBMOD_V3_PERMISSION_SAFE},
    {"resources", WOTBMOD_V3_PERMISSION_SAFE},
    {"resources.mod", WOTBMOD_V3_PERMISSION_SAFE},
    {"filesystem.mod_data", WOTBMOD_V3_PERMISSION_SAFE},
    {"input", WOTBMOD_V3_PERMISSION_SAFE},
    {"content", WOTBMOD_V3_PERMISSION_SAFE},
    {"hangar.scene", WOTBMOD_V3_PERMISSION_SAFE},
    {"vehicle.local.cosmetic", WOTBMOD_V3_PERMISSION_SAFE},
    {"camera.hangar", WOTBMOD_V3_PERMISSION_SAFE},
    {"camera.replay", WOTBMOD_V3_PERMISSION_SAFE},
    {"network.http.allowlisted", WOTBMOD_V3_PERMISSION_SAFE},
    {"settings", WOTBMOD_V3_PERMISSION_SAFE},
    {"storage", WOTBMOD_V3_PERMISSION_SAFE},
    {"input.actions", WOTBMOD_V3_PERMISSION_SAFE},
    {"events.public", WOTBMOD_V3_PERMISSION_SAFE},
    {"entity.public.visible", WOTBMOD_V3_PERMISSION_SAFE},
    {"gameplay.tweak.camera", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"gameplay.tweak.hud", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"gameplay.tweak.hangar", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"gameplay.tweak.replay", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"gameplay.tweak.cosmetic", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"gameplay.tweak.vehicle", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"gameplay.tweak.projectile_visual", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"gameplay.tweak.freecam", WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK},
    {"ui.modify.game", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"battle.ui", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"resources.overlay.game", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"resources.write.mod_data", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"hooks.symbol", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"render.callbacks", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"battle.render.overlay", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"camera.battle.read", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"visible.projectile.events", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"ges.observe", WOTBMOD_V3_PERMISSION_SAFE},
    {"ges.publish", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"session.cluster.read", WOTBMOD_V3_PERMISSION_SAFE},
    {"session.cluster.change", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"game.entity.public", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"bigworld.observe", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"bigworld.rpc.observe", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"bigworld.rpc.metadata", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"client.leave_to_hangar", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"network.http", WOTBMOD_V3_PERMISSION_REVIEWED},
    {"native.memory", WOTBMOD_V3_PERMISSION_UNSAFE},
    {"native.memory_patch", WOTBMOD_V3_PERMISSION_UNSAFE},
    {"native.hook.address", WOTBMOD_V3_PERMISSION_UNSAFE},
    {"native.hooks", WOTBMOD_V3_PERMISSION_UNSAFE},
    {"render.native", WOTBMOD_V3_PERMISSION_UNSAFE},
    {"bigworld.rpc.modify", WOTBMOD_V3_PERMISSION_UNSAFE}
};

/*
 * Recursive, and at least one caller now depends on that rather than
 * merely tolerating it. EndModEnableTransition runs from a __finally in
 * the legacy facade's enable path, so it can be entered while this thread
 * is unwinding an access violation raised somewhere under the same lock.
 * Demoting this to a plain std::mutex would turn that unwind into a
 * self-deadlock -- and re-locking a plain std::mutex on the owning thread
 * makes MSVC throw, which during an unwind means terminate(). That is the
 * same failure mode already documented on OwnerAnchor in data_services.cpp.
 *
 * READ THE NEXT PARAGRAPH BEFORE CONCLUDING THAT CASE IS HANDLED. It is
 * not. Recursion buys exactly one thing: the __finally can re-enter this
 * lock without terminating the process. It does nothing about the state
 * the fault left behind. Under /EHsc MSVC runs no C++ destructor while
 * unwinding to an __except -- measured, see MountV3PackageDuringEnable in
 * wotb_mod_runtime.cpp -- so every std::lock_guard on this mutex between
 * the faulting instruction and that handler is skipped, and each one
 * leaves the recursion count one higher than it should be. The count never
 * returns to zero. This thread keeps working, because recursion lets it
 * re-acquire its own lock forever; every *other* thread blocks on
 * g_state_mutex the next time it wants the runtime, permanently. The mod
 * that faulted is marked faulted and the game appears to survive, which is
 * what makes this worth spelling out: the visible symptom is a hang some
 * time later on an unrelated thread, with nothing pointing back here.
 *
 * There is no fix at this declaration. std::recursive_mutex exposes no way
 * to ask how deep it is or to force it open, and a fault-time unwind
 * cannot know how many guards it skipped. Closing it means either not
 * faulting under this lock or a lock type that can be reconciled after an
 * unwind, and both are real work that is not this comment's to do.
 *
 * The size of the first option, re-counted at HEAD rather than inherited:
 * mod_api/src and mod_api/loader together hold 163 __except handler blocks
 * and 584 lock acquisitions, counting std::lock_guard, std::unique_lock
 * and std::scoped_lock declarations in .cpp and .h; 67 of those 584 are on
 * this mutex in this file. An earlier revision of this paragraph said
 * "169 lock sites under them". That matched neither count and no
 * derivation for it survives, so it is gone rather than adjusted. The
 * number that would actually size the work is how many of the 584 sit
 * under one of the 163, and nobody has computed it. What this comment owes
 * the next reader is that they keep looking.
 */
std::recursive_mutex g_state_mutex;
bool g_initialized = false;
DWORD g_main_thread_id = 0u;
std::string g_game_directory;
std::string g_mods_directory;
std::string g_cache_directory;
std::string g_config_directory;
std::string g_client_version;
std::string g_executable_sha256;
uint32_t g_binding_pack_version = 0u;
uint32_t g_process_architecture = 0u;
WotbModV3RuntimeLogSink g_log_sink = nullptr;
void* g_log_user_data = nullptr;
std::atomic<uint64_t> g_context(WOTBMOD_V3_CONTEXT_NONE);
std::atomic<uint64_t> g_frame_index(0u);
std::atomic<uint64_t> g_event_source_mask(0u);
HandleRecord g_handles[kMaxHandles];
ModRecord g_mods[kMaxMods];
std::vector<InterfaceRecord> g_interfaces;
std::vector<CapabilityRecord> g_capabilities;
std::vector<FramePumpFn> g_frame_pumps;
std::vector<ShutdownHookFn> g_shutdown_hooks;
std::vector<OwnerStoppingHookFn> g_owner_stopping_hooks;
std::vector<LifecycleStateHookFn> g_lifecycle_state_hooks;
WotbModV3Bootstrap g_bootstrap = {};

thread_local WotbModV3ErrorInfo g_last_error = {};
thread_local uint32_t g_thread_role = WOTBMOD_V3_THREAD_UNKNOWN;
thread_local WotbModV3Handle g_callback_mod = WOTBMOD_V3_INVALID_HANDLE;
thread_local uint32_t g_callback_nesting = 0u;

struct CallbackTimer {
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    uint64_t started_100ns = 0u;
};

thread_local CallbackTimer g_callback_timers[64] = {};
thread_local uint32_t g_callback_timer_depth = 0u;

uint64_t QueryTime100ns() {
    static const LARGE_INTEGER frequency = []() {
        LARGE_INTEGER value = {};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    if (frequency.QuadPart <= 0) return 0u;
    const long double value =
        static_cast<long double>(counter.QuadPart) *
        10000000.0L /
        static_cast<long double>(frequency.QuadPart);
    return value > 0.0L ? static_cast<uint64_t>(value) : 0u;
}

void AdvanceCallbackFrameLocked(
    ModRecord* record,
    uint64_t frame_index) {
    if (!record || record->callback_profile_frame == frame_index) {
        return;
    }
    if (record->callback_profile_frame != UINT64_MAX) {
        record->previous_frame_callbacks = record->frame_callbacks;
        record->previous_frame_cpu_100ns = record->frame_cpu_100ns;
    }
    record->callback_profile_frame = frame_index;
    record->frame_callbacks = 0u;
    record->frame_cpu_100ns = 0u;
    record->frame_over_budget = false;
}

void ResetCallbackProfileLocked(ModRecord* record) {
    if (!record) return;
    record->callback_profile_frame = UINT64_MAX;
    record->frame_callbacks = 0u;
    record->frame_cpu_100ns = 0u;
    record->previous_frame_callbacks = 0u;
    record->previous_frame_cpu_100ns = 0u;
    record->total_callbacks = 0u;
    record->total_callback_cpu_100ns = 0u;
    record->max_callback_100ns = 0u;
    record->slow_callbacks = 0u;
    record->coalesced_callbacks = 0u;
    record->over_budget_frames = 0u;
    record->frame_over_budget = false;
}

void CopyString(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0u) return;
    if (!source) source = "";
    strncpy_s(destination, capacity, source, _TRUNCATE);
}

std::string NormalizeDirectory(const char* path) {
    if (!path || !path[0]) return std::string();
    const DWORD required = GetFullPathNameA(path, 0u, nullptr, nullptr);
    if (required == 0u) return std::string(path);
    std::vector<char> absolute(
        static_cast<size_t>(required) + 1u, '\0');
    const DWORD length = GetFullPathNameA(
        path,
        static_cast<DWORD>(absolute.size()),
        absolute.data(),
        nullptr);
    if (length == 0u || length >= absolute.size()) {
        return std::string(path);
    }
    std::string result(absolute.data(), length);
    while (result.size() > 3u &&
           (result.back() == '\\' || result.back() == '/')) {
        result.pop_back();
    }
    return result;
}

std::string ParentDirectory(const std::string& path) {
    const size_t separator = path.find_last_of("\\/");
    if (separator == std::string::npos) return std::string();
    if (separator == 2u && path.size() >= 3u && path[1] == ':') {
        return path.substr(0u, 3u);
    }
    if (separator == 0u) return path.substr(0u, 1u);
    return path.substr(0u, separator);
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    std::string result(left);
    if (result.back() != '\\' && result.back() != '/') result.push_back('\\');
    result.append(right);
    return result;
}

bool EnsureDirectoryTree(const std::string& path) {
    if (path.empty()) return false;
    DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
    }
    const size_t separator = path.find_last_of("\\/");
    if (separator != std::string::npos && separator > 2u) {
        if (!EnsureDirectoryTree(path.substr(0u, separator))) return false;
    }
    if (CreateDirectoryA(path.c_str(), nullptr) != 0) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string FileStem(const char* module_path) {
    std::string path = module_path ? module_path : "";
    const size_t separator = path.find_last_of("\\/");
    std::string name =
        separator == std::string::npos ? path : path.substr(separator + 1u);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    if (name.empty()) name = "unnamed_mod";
    return name;
}

bool IsValidId(const char* id) {
    if (!id || !id[0]) return false;
    const size_t length = strlen(id);
    if (length >= WOTBMOD_V3_MAX_ID) return false;
    for (size_t index = 0u; index < length; ++index) {
        const unsigned char value = static_cast<unsigned char>(id[index]);
        const bool valid =
            (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') ||
            value == '.' || value == '_' || value == '-';
        if (!valid) return false;
    }
    return true;
}

bool HasTerminatedText(
    const char* text,
    size_t capacity,
    bool allow_empty = false) {
    if (!text || capacity == 0u) return false;
    size_t length = 0u;
    while (length < capacity && text[length] != '\0') ++length;
    return length < capacity && (allow_empty || length != 0u);
}

uint32_t NextGeneration(uint32_t current) {
    uint32_t next = (current + 1u) & static_cast<uint32_t>(
        kHandleGenerationMask);
    return next == 0u ? 1u : next;
}

WotbModV3Handle EncodeHandle(
    uint32_t slot,
    uint32_t generation,
    uint32_t type) {
    return kHandleMagic |
        (static_cast<uint64_t>(type & 0xFFu) << kHandleTypeShift) |
        (static_cast<uint64_t>(generation) << kHandleGenerationShift) |
        static_cast<uint64_t>(slot);
}

bool DecodeHandle(
    WotbModV3Handle handle,
    uint32_t* out_slot,
    uint32_t* out_generation,
    uint32_t* out_type) {
    if ((handle & kHandleMagicMask) != kHandleMagic) return false;
    const uint32_t slot =
        static_cast<uint32_t>(handle & kHandleSlotMask);
    const uint32_t generation = static_cast<uint32_t>(
        (handle >> kHandleGenerationShift) & kHandleGenerationMask);
    const uint32_t type =
        static_cast<uint32_t>((handle >> kHandleTypeShift) & 0xFFu);
    if (slot == 0u || slot >= kMaxHandles || generation == 0u ||
        type == WOTBMOD_V3_HANDLE_UNKNOWN) {
        return false;
    }
    if (out_slot) *out_slot = slot;
    if (out_generation) *out_generation = generation;
    if (out_type) *out_type = type;
    return true;
}

HandleRecord* FindHandleUnlocked(WotbModV3Handle handle) {
    uint32_t slot = 0u;
    uint32_t generation = 0u;
    uint32_t type = 0u;
    if (!DecodeHandle(handle, &slot, &generation, &type)) return nullptr;
    HandleRecord& record = g_handles[slot];
    if (!record.alive || record.generation != generation ||
        record.type != type) {
        return nullptr;
    }
    return &record;
}

ModRecord* FindModUnlocked(WotbModV3Handle mod) {
    HandleRecord* handle = FindHandleUnlocked(mod);
    if (!handle || handle->type != WOTBMOD_V3_HANDLE_MOD ||
        !handle->object) {
        return nullptr;
    }
    ModRecord* record = static_cast<ModRecord*>(handle->object);
    return record->active && record->handle == mod ? record : nullptr;
}

WotbModV3Result CopyOutString(
    WotbModV3Handle mod,
    const char* source,
    char* buffer,
    uint32_t* inout_size) {
    if (!inout_size) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "inout_size is required");
    }
    const uint32_t required =
        static_cast<uint32_t>(strlen(source ? source : "") + 1u);
    const uint32_t capacity = *inout_size;
    *inout_size = required;
    if (!buffer || capacity < required) {
        return SetError(
            mod,
            WOTBMOD_V3_E_BUFFER_TOO_SMALL,
            "output buffer is too small");
    }
    CopyString(buffer, capacity, source);
    return WOTBMOD_V3_OK;
}

CapabilityRecord* FindCapabilityUnlocked(const char* name) {
    if (!name) return nullptr;
    for (CapabilityRecord& record : g_capabilities) {
        if (_stricmp(record.info.name, name) == 0) return &record;
    }
    return nullptr;
}

InterfaceRecord* FindInterfaceUnlocked(
    const char* name,
    uint32_t minimum_version) {
    InterfaceRecord* best = nullptr;
    for (InterfaceRecord& record : g_interfaces) {
        if (_stricmp(record.name.c_str(), name) != 0 ||
            record.version < minimum_version) {
            continue;
        }
        if (!best || record.version > best->version) best = &record;
    }
    return best;
}

struct InterfaceAvailabilityDefault {
    const char* name;
    uint32_t status;
    const char* reason;
};

const InterfaceAvailabilityDefault kInterfaceAvailabilityDefaults[] = {
    {WOTBMOD_V3_IFACE_CORE,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_CAPABILITIES,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_PERMISSIONS,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_HANDLES,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_LIFECYCLE,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_EVENTS,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_SETTINGS,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_STORAGE,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "native ingress exposes local and confirmed-visible vehicles only; generic avatars, effects and replay entities are unavailable"},
    {WOTBMOD_V3_IFACE_BIGWORLD_RPC,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "incoming curated RPC metadata is available; payload access, sending, modification, dropping and replay are unavailable"},
    {WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "FOV operations are available when the verified host bridge is installed; zoom, transition, shake, post-processing and freecam remain unsupported"},
    {WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "stock battle controls (minimap, sixth-sense lamp, reticle, damage ribbons, damage counters) accept visibility, position and scale through the UI host bridge; opacity, textures, colours, sounds, markers, formats and delays remain unsupported"},
    {WOTBMOD_V3_IFACE_GAMEPLAY_HANGAR,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "gameplay hangar operations have no native backend"},
    {WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "gameplay replay operations have no native backend"},
    {WOTBMOD_V3_IFACE_HOOKS,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "hook metadata is available; native installation depends on the host backend"},
    {WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "unsafe native hooks require an explicitly enabled developer backend"},
    {WOTBMOD_V3_IFACE_ASYNC,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "core tasks and timers are available; host-thread dispatch depends on pumps"},
    {WOTBMOD_V3_IFACE_HTTP,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_INTERMOD,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "runtime-local exports and messages are available; cross-process transport is not provided"},
    {WOTBMOD_V3_IFACE_UI,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "native base controls, active-screen inspection, managed layout and input events are available; typed rendering and native style components are not"},
    {WOTBMOD_V3_IFACE_INPUT,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "native WndProc/DAVA ingress is binding-pack dependent; action queues and coalescing remain available"},
    {WOTBMOD_V3_IFACE_VFS,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "scoped mounts are available; native client interception is build-dependent"},
    {WOTBMOD_V3_IFACE_RESOURCES,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "generic resources are available; native client resource types are partial"},
    {WOTBMOD_V3_IFACE_YAML,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "generic YAML is available; the native DAVA YAML backend is not installed"},
    {WOTBMOD_V3_IFACE_ARCHIVE,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "directory archives are available; native DAVA ResourceArchive is not installed"},
    {WOTBMOD_V3_IFACE_LOADERS,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "generic and supported DVPL loaders are available; native DAVA loaders are partial"},
    {WOTBMOD_V3_IFACE_MANIFEST,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "manifest parsing and trusted ECDSA P-256/SHA-256 package verification are available"},
    {WOTBMOD_V3_IFACE_CATALOG,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "catalog policy and ECDSA P-256/SHA-256 trust verification are available; catalog review status remains independent of signer trust"},
    {WOTBMOD_V3_IFACE_CONTENT,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "content descriptors plus native-intercepted UI, texture and model file overlays are available; semantic audio, hangar and localization application is unavailable"},
    {WOTBMOD_V3_IFACE_RENDER,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "managed rendering is available; render phases and backends are partial"},
    {WOTBMOD_V3_IFACE_RENDER_NATIVE,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "native rendering is limited to verified host backends"},
    {WOTBMOD_V3_IFACE_CAMERA,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "transform, FOV, projection, transitions and shake are available; free/postmortem/cinematic ownership is not proven"},
    {WOTBMOD_V3_IFACE_SCENE,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "verified scene create/load/clone/hierarchy/transform and managed mesh attachment are available; live material internals remain partial"},
    {WOTBMOD_V3_IFACE_AUDIO,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "custom audio playback is available; native event coverage is partial"},
    {WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "exact-path mesh/material/texture skin packs support explicit LOD and rollback; active cached model hot swap is unavailable"},
    {WOTBMOD_V3_IFACE_PROJECTILE,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "stock shot/impact lifecycle and managed visuals are available; native stock tracer style-object mutation is unavailable"},
    {WOTBMOD_V3_IFACE_CLIENT,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "client operations require verified native bindings"},
    {WOTBMOD_V3_IFACE_DEVICE,
     WOTBMOD_V3_CAPABILITY_AVAILABLE,
     ""},
    {WOTBMOD_V3_IFACE_DIAGNOSTICS,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "core diagnostics are available; native diagnostics are partial"},
    {WOTBMOD_V3_IFACE_DEVTOOLS,
     WOTBMOD_V3_CAPABILITY_DEGRADED,
     "runtime markers are available; advanced developer tooling is partial"},
    /*
     * Declared 2026-08-16, backends not yet written. UNAVAILABLE, not
     * DEGRADED: DEGRADED is the token for an interface where a real subset
     * works, and every slot on these five answers
     * WOTBMOD_V3_E_NOT_SUPPORTED today. Calling that "degraded" would be the
     * same overstatement API_V3_RC1_FREEZE.md bans when it forbids returning
     * OK without performing the operation. These entries flip to DEGRADED or
     * AVAILABLE only when a backend actually lands, and only with the
     * evidence the freeze document requires.
     */
    {WOTBMOD_V3_IFACE_UI_READ,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "UI text, texture, font and style readback has no backend; every slot returns NOT_SUPPORTED"},
    {WOTBMOD_V3_IFACE_CAMERA_STATE,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "camera animation-state and arcade/sniper view-mode observation has no backend; every slot returns NOT_SUPPORTED"},
    {WOTBMOD_V3_IFACE_AUDIO_INTERCEPT,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "semantic sound-event interception has no backend; every slot returns NOT_SUPPORTED"},
    {WOTBMOD_V3_IFACE_SCENE_ENUMERATE,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "game-owned scene enumeration has no backend; every slot returns NOT_SUPPORTED"},
    {WOTBMOD_V3_IFACE_TRACER,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "stock tracer shell-type style lookup has no backend; every slot returns NOT_SUPPORTED"},
    {WOTBMOD_V3_IFACE_GES,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "GES event bus has no backend; every slot returns NOT_SUPPORTED"},
    {WOTBMOD_V3_IFACE_SESSION_CLUSTER,
     WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
     "login cluster switch has no native backend; every slot returns NOT_SUPPORTED"}
};

bool IsImplementationStatus(uint32_t status) {
    return status == WOTBMOD_V3_CAPABILITY_AVAILABLE ||
           status == WOTBMOD_V3_CAPABILITY_UNAVAILABLE ||
           status == WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH ||
           status == WOTBMOD_V3_CAPABILITY_DEGRADED;
}

InterfaceAvailabilityDefault DefaultInterfaceAvailability(
    const char* name) {
    for (const InterfaceAvailabilityDefault& availability :
         kInterfaceAvailabilityDefaults) {
        if (_stricmp(availability.name, name) == 0) {
            return availability;
        }
    }
    return {
        name,
        WOTBMOD_V3_CAPABILITY_DEGRADED,
        "implementation coverage has not been declared complete"};
}

void RefreshDerivedCapabilityUnlocked(const char* capability_name) {
    CapabilityRecord* capability =
        FindCapabilityUnlocked(capability_name);
    if (!capability || !capability->interface_derived) return;

    const InterfaceRecord* first = nullptr;
    bool all_same = true;
    bool reasons_same = true;
    uint32_t maximum_version = 0u;
    uint64_t allowed_contexts = 0u;
    uint32_t permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    for (const InterfaceRecord& interface_record : g_interfaces) {
        if (_stricmp(
                interface_record.capability_name.c_str(),
                capability_name) != 0) {
            continue;
        }
        if (!first) {
            first = &interface_record;
        } else if (
            interface_record.implementation_status !=
            first->implementation_status) {
            all_same = false;
        }
        if (first &&
            interface_record.implementation_reason !=
                first->implementation_reason) {
            reasons_same = false;
        }
        maximum_version = std::max(
            maximum_version,
            interface_record.version);
        allowed_contexts |= interface_record.allowed_contexts;
        permission_tier = std::max(
            permission_tier,
            interface_record.required_permission_tier);
    }
    if (!first) return;

    capability->info.interface_version = maximum_version;
    capability->info.allowed_contexts = allowed_contexts;
    capability->info.permission_tier = permission_tier;
    capability->info.status = all_same
        ? first->implementation_status
        : WOTBMOD_V3_CAPABILITY_DEGRADED;
    CopyString(
        capability->info.unavailable_reason,
        sizeof(capability->info.unavailable_reason),
        all_same
            ? (reasons_same
                   ? first->implementation_reason.c_str()
                   : "capability has partial coverage across registered interfaces")
            : "capability coverage differs across registered interfaces");
}

const PermissionDefinition* FindPermission(const char* name) {
    if (!name) return nullptr;
    for (const PermissionDefinition& permission : kPermissions) {
        if (_stricmp(permission.name, name) == 0) return &permission;
    }
    return nullptr;
}

bool IsValidNetworkPermissionName(
    const char* name,
    size_t length) {
    static const char kPrefix[] = "network:https://";
    const size_t prefix_length = sizeof(kPrefix) - 1u;
    if (length <= prefix_length ||
        _strnicmp(name, kPrefix, prefix_length) != 0) {
        return false;
    }
    const char* host = name + prefix_length;
    const size_t host_length = length - prefix_length;
    if (host[0] == '.' || host[host_length - 1u] == '.') {
        return false;
    }
    bool previous_dot = false;
    for (size_t index = 0u; index < host_length; ++index) {
        const unsigned char character =
            static_cast<unsigned char>(host[index]);
        const bool accepted =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '-' ||
            character == ':';
        if (!accepted || (character == '.' && previous_dot)) {
            return false;
        }
        previous_dot = character == '.';
    }
    return true;
}

bool IsValidPermissionGrantName(const char* name) {
    if (!name) return false;
    const size_t length =
        strnlen_s(name, WOTBMOD_V3_MAX_PERMISSION_NAME);
    if (length == 0u ||
        length >= WOTBMOD_V3_MAX_PERMISSION_NAME) {
        return false;
    }
    if (IsValidNetworkPermissionName(name, length)) {
        return true;
    }
    if (name[0] == '.' || name[length - 1u] == '.' ||
        name[0] == '/' || name[length - 1u] == '/') {
        return false;
    }
    bool previous_dot = false;
    bool previous_slash = false;
    size_t component_start = 0u;
    for (size_t index = 0u; index < length; ++index) {
        const unsigned char character =
            static_cast<unsigned char>(name[index]);
        const bool accepted =
            (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-' ||
            character == '.' || character == '/';
        if (!accepted ||
            (character == '.' && previous_dot) ||
            (character == '/' && previous_slash)) {
            return false;
        }
        if (character == '/') {
            const size_t component_length = index - component_start;
            if ((component_length == 1u &&
                 name[component_start] == '.') ||
                (component_length == 2u &&
                 name[component_start] == '.' &&
                 name[component_start + 1u] == '.')) {
                return false;
            }
            component_start = index + 1u;
        }
        previous_dot = character == '.';
        previous_slash = character == '/';
    }
    const size_t final_component_length = length - component_start;
    return !((final_component_length == 1u &&
              name[component_start] == '.') ||
             (final_component_length == 2u &&
              name[component_start] == '.' &&
              name[component_start + 1u] == '.'));
}

std::string NormalizePermissionGrantName(const char* name) {
    std::string normalized(name ? name : "");
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return normalized;
}

bool PermissionGrantCovers(
    const std::string& grant,
    const char* permission_name) {
    if (!permission_name || !permission_name[0]) return false;
    static const char kNetworkHostPrefix[] = "network:https://";
    const bool grant_is_network_host =
        grant.size() > sizeof(kNetworkHostPrefix) - 1u &&
        _strnicmp(
            grant.c_str(),
            kNetworkHostPrefix,
            sizeof(kNetworkHostPrefix) - 1u) == 0;
    const bool request_is_network_host =
        _strnicmp(
            permission_name,
            kNetworkHostPrefix,
            sizeof(kNetworkHostPrefix) - 1u) == 0;
    if (grant_is_network_host || request_is_network_host) {
        return _stricmp(grant.c_str(), permission_name) == 0;
    }
    const size_t requested_length = std::strlen(permission_name);
    if (grant.size() > requested_length ||
        _strnicmp(
            grant.c_str(),
            permission_name,
            grant.size()) != 0) {
        return false;
    }
    if (grant.size() == requested_length) return true;
    return permission_name[grant.size()] == '.';
}

bool HasNamedPermissionUnlocked(
    const ModRecord& record,
    const char* permission_name) {
    if (!record.named_permissions_restricted) return true;
    if (permission_name &&
        _stricmp(permission_name, "core") == 0) {
        return true;
    }
    for (const std::string& grant :
         record.named_permission_grants) {
        if (PermissionGrantCovers(grant, permission_name)) {
            return true;
        }
    }
    return false;
}

bool CapabilityNameRequiresNamedGrant(const char* capability_name) {
    if (!capability_name || !capability_name[0]) return false;
    if (FindPermission(capability_name)) return true;
    return _stricmp(
               capability_name,
               "filesystem.mod_data") == 0 ||
           _stricmp(capability_name, "resources") == 0 ||
           _stricmp(
               capability_name,
               "network.http.allowlisted") == 0 ||
           _strnicmp(
               capability_name,
               "network:https://",
               sizeof("network:https://") - 1u) == 0;
}

bool HasAnyNamedPermissionUnlocked(
    const ModRecord& record,
    const char* const* permission_names,
    size_t permission_count) {
    for (size_t index = 0u;
         index < permission_count;
         ++index) {
        if (HasNamedPermissionUnlocked(
                record,
                permission_names[index])) {
            return true;
        }
    }
    return false;
}

bool HasNetworkPermissionUnlocked(const ModRecord& record) {
    static const char* const kNetworkPermissions[] = {
        "network.http",
        "network.http.allowlisted"
    };
    if (HasAnyNamedPermissionUnlocked(
            record,
            kNetworkPermissions,
            sizeof(kNetworkPermissions) /
                sizeof(kNetworkPermissions[0]))) {
        return true;
    }
    static const char kDomainPrefix[] = "network:https://";
    for (const std::string& grant :
         record.named_permission_grants) {
        if (grant.compare(
                0u,
                sizeof(kDomainPrefix) - 1u,
                kDomainPrefix) == 0) {
            return true;
        }
    }
    return false;
}

bool CapabilityNamedPermissionGrantedUnlocked(
    const ModRecord& record,
    const char* capability_name) {
    if (_stricmp(capability_name, "resources") == 0) {
        static const char* const kResourcePermissions[] = {
            "resources",
            "resources.mod",
            "resources.overlay.game"
        };
        return HasAnyNamedPermissionUnlocked(
            record,
            kResourcePermissions,
            sizeof(kResourcePermissions) /
                sizeof(kResourcePermissions[0]));
    }
    if (_stricmp(capability_name, "content") == 0) {
        static const char* const kContentPermissions[] = {
            "content",
            "resources",
            "resources.mod",
            "resources.overlay.game"
        };
        return HasAnyNamedPermissionUnlocked(
            record,
            kContentPermissions,
            sizeof(kContentPermissions) /
                sizeof(kContentPermissions[0]));
    }
    if (_stricmp(
            capability_name,
            "network.http.allowlisted") == 0) {
        return HasNetworkPermissionUnlocked(record);
    }
    if (_stricmp(
            capability_name,
            "filesystem.mod_data") == 0) {
        static const char* const kStoragePermissions[] = {
            "filesystem.mod_data",
            "storage"
        };
        return HasAnyNamedPermissionUnlocked(
            record,
            kStoragePermissions,
            sizeof(kStoragePermissions) /
                sizeof(kStoragePermissions[0]));
    }
    return HasNamedPermissionUnlocked(record, capability_name);
}

bool InterfaceNamedPermissionGrantedUnlocked(
    const ModRecord& record,
    const InterfaceRecord& interface_record) {
    if (!record.named_permissions_restricted) return true;
    const char* name = interface_record.name.c_str();
    if (_stricmp(name, WOTBMOD_V3_IFACE_CORE) == 0 ||
        _stricmp(name, WOTBMOD_V3_IFACE_CAPABILITIES) == 0 ||
        _stricmp(name, WOTBMOD_V3_IFACE_PERMISSIONS) == 0 ||
        _stricmp(name, WOTBMOD_V3_IFACE_HANDLES) == 0) {
        return true;
    }

    static const char* const kCorePermissions[] = {"core"};
    static const char* const kEventPermissions[] = {
        "events.public"
    };
    static const char* const kHookPermissions[] = {
        "hooks.symbol",
        "native.hook.address",
        "native.hooks"
    };
    static const char* const kUnsafeNativePermissions[] = {
        "native.memory",
        "native.memory_patch",
        "native.hook.address",
        "native.hooks"
    };
    static const char* const kUiPermissions[] = {
        "ui",
        "ui.create",
        "ui.modify.own",
        "ui.modify.game",
        "battle.ui"
    };
    static const char* const kStoragePermissions[] = {
        "storage",
        "filesystem.mod_data"
    };
    static const char* const kInputPermissions[] = {
        "input",
        "input.actions"
    };
    static const char* const kResourcePermissions[] = {
        "resources",
        "resources.mod",
        "resources.overlay.game"
    };
    static const char* const kContentPermissions[] = {
        "resources",
        "resources.mod",
        "resources.overlay.game",
        "content"
    };
    static const char* const kRenderPermissions[] = {
        "render.callbacks",
        "battle.render.overlay"
    };
    static const char* const kRenderNativePermissions[] = {
        "render.native"
    };
    static const char* const kCameraPermissions[] = {
        "camera.battle.read",
        "camera.hangar",
        "camera.replay"
    };
    static const char* const kScenePermissions[] = {
        "hangar.scene",
        "resources",
        "resources.mod"
    };
    static const char* const kAudioPermissions[] = {
        "audio",
        "audio.custom",
        "audio.events"
    };
    static const char* const kVehicleVisualPermissions[] = {
        "vehicle.local.cosmetic",
        "gameplay.tweak.cosmetic",
        "gameplay.tweak.vehicle"
    };
    static const char* const kGameplayCameraPermissions[] = {
        "gameplay.tweak.camera",
        "gameplay.tweak.freecam"
    };
    static const char* const kGameplayHudPermissions[] = {
        "gameplay.tweak.hud",
        "battle.ui"
    };
    static const char* const kGameplayHangarPermissions[] = {
        "gameplay.tweak.hangar",
        "hangar.scene"
    };
    static const char* const kGameplayReplayPermissions[] = {
        "gameplay.tweak.replay"
    };
    static const char* const kEntityPermissions[] = {
        "entity.public.visible",
        "game.entity.public"
    };
    static const char* const kBigWorldPermissions[] = {
        "bigworld.observe",
        "bigworld.rpc.observe",
        "bigworld.rpc.metadata"
    };
    static const char* const kProjectilePermissions[] = {
        "visible.projectile.events",
        "gameplay.tweak.projectile_visual"
    };
    static const char* const kGesPermissions[] = {
        "ges.observe",
        "ges.publish"
    };
    static const char* const kSessionClusterPermissions[] = {
        "session.cluster.read",
        "session.cluster.change"
    };

#define WOTBMOD_V3_MATCH_PERMISSION_SET(interface_id, set_name)           \
    if (_stricmp(name, (interface_id)) == 0) {                            \
        return HasAnyNamedPermissionUnlocked(                             \
            record,                                                       \
            (set_name),                                                   \
            sizeof(set_name) / sizeof((set_name)[0]));                    \
    }

    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_LIFECYCLE,
        kCorePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_EVENTS,
        kEventPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_HOOKS,
        kHookPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_UNSAFE_NATIVE,
        kUnsafeNativePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_ASYNC,
        kCorePermissions)
    if (_stricmp(name, WOTBMOD_V3_IFACE_HTTP) == 0) {
        return HasNetworkPermissionUnlocked(record);
    }
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_INTERMOD,
        kCorePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_UI,
        kUiPermissions)
    if (_stricmp(name, WOTBMOD_V3_IFACE_SETTINGS) == 0) {
        return HasNamedPermissionUnlocked(record, "settings");
    }
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_STORAGE,
        kStoragePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_INPUT,
        kInputPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_VFS,
        kResourcePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_RESOURCES,
        kResourcePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_YAML,
        kResourcePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_ARCHIVE,
        kResourcePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_LOADERS,
        kResourcePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_MANIFEST,
        kCorePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_CATALOG,
        kCorePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_CONTENT,
        kContentPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_RENDER,
        kRenderPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_RENDER_NATIVE,
        kRenderNativePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_CAMERA,
        kCameraPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_SCENE,
        kScenePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_AUDIO,
        kAudioPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
        kVehicleVisualPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
        kGameplayCameraPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
        kGameplayHudPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_GAMEPLAY_HANGAR,
        kGameplayHangarPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY,
        kGameplayReplayPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
        kEntityPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_BIGWORLD_RPC,
        kBigWorldPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_PROJECTILE,
        kProjectilePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_CLIENT,
        kCorePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_DEVICE,
        kCorePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_DIAGNOSTICS,
        kCorePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_DEVTOOLS,
        kCorePermissions)

    /*
     * Post-RC1 contract, 2026-08-16. Each of the five reuses the permission
     * set of the frozen interface it sits beside, because it discloses
     * nothing that grant does not already cover: ui.read reads back the
     * loader's own mirror of what this API wrote, camera.state is the same
     * disclosure as the camera read grant, audio.intercept is governed by the
     * grant that already covers sound overrides, scene.enumerate discloses
     * identity and world placement of game-owned objects (the public-entity
     * disclosure class), and tracer is a read-only stock style lookup.
     *
     * Without an entry here a named-permission-restricted mod falls through
     * to the `return true` below and is not gated at query_interface at all.
     * Every slot still enforces its own named grant and the interface tier
     * still applies, so this was a defence-in-depth gap rather than an open
     * door - but the whole point of the restricted mode is that the refusal
     * happens once, at the door, rather than once per slot.
     */
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_UI_READ,
        kUiPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_CAMERA_STATE,
        kCameraPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_AUDIO_INTERCEPT,
        kAudioPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_SCENE_ENUMERATE,
        kEntityPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_TRACER,
        kProjectilePermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_GES,
        kGesPermissions)
    WOTBMOD_V3_MATCH_PERMISSION_SET(
        WOTBMOD_V3_IFACE_SESSION_CLUSTER,
        kSessionClusterPermissions)

#undef WOTBMOD_V3_MATCH_PERMISSION_SET

    if (!interface_record.capability_name.empty()) {
        return HasNamedPermissionUnlocked(
            record,
            interface_record.capability_name.c_str());
    }
    return true;
}

void FillCapabilityInfo(
    WotbModV3CapabilityInfo* destination,
    const WotbModV3CapabilityInfo& source) {
    if (!destination) return;
    const uint32_t requested_size = destination->struct_size;
    if (requested_size < sizeof(WotbModV3StructHeader)) return;
    const size_t copy_size = std::min<size_t>(
        requested_size,
        sizeof(WotbModV3CapabilityInfo));
    memcpy(destination, &source, copy_size);
    destination->struct_size = static_cast<uint32_t>(copy_size);
    destination->api_version = WOTBMOD_V3_CAPABILITIES_VERSION;
}

void DestroyCapabilitySubscription(void* object) {
    delete static_cast<CapabilitySubscription*>(object);
}

bool CallEntryProtected(
    WotbModLoadV3Fn entry,
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    WotbModV3Info* info,
    WotbModV3Result* out_result) {
    __try {
        *out_result = entry(bootstrap, mod, info);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallLifecycleProtected(
    WotbModV3LifecycleCallback callback,
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    __try {
        callback(bootstrap, mod);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallFrameProtected(
    WotbModV3FrameCallback callback,
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    uint64_t frame_index,
    double delta_seconds) {
    __try {
        callback(bootstrap, mod, frame_index, delta_seconds);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallCapabilityProtected(
    WotbModV3CapabilityChangedCallback callback,
    WotbModV3Handle mod,
    const WotbModV3CapabilityInfo* capability,
    void* user_data) {
    __try {
        callback(mod, capability, user_data);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallFramePumpProtected(
    FramePumpFn pump,
    uint64_t frame_index,
    double delta_seconds) {
    __try {
        pump(frame_index, delta_seconds);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallLifecycleStateHookProtected(
    LifecycleStateHookFn hook,
    WotbModV3Handle mod,
    uint32_t transition) {
    __try {
        hook(mod, transition);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FillModuleInfo(
    const ModRecord& record,
    WotbModV3RuntimeModuleInfo* out_info) {
    if (!out_info) return;
    const uint32_t capacity = out_info->struct_size;
    WotbModV3RuntimeModuleInfo value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_ABI_VERSION;
    value.mod = record.handle;
    value.permission_tier = record.permission_tier;
    value.state = record.state;
    CopyString(value.id, sizeof(value.id), record.id.c_str());
    CopyString(value.name, sizeof(value.name), record.name.c_str());
    CopyString(value.version, sizeof(value.version), record.version.c_str());
    CopyString(value.author, sizeof(value.author), record.author.c_str());
    CopyString(
        value.description,
        sizeof(value.description),
        record.description.c_str());
    const size_t copy_size =
        std::min<size_t>(capacity, sizeof(WotbModV3RuntimeModuleInfo));
    memcpy(out_info, &value, copy_size);
    out_info->struct_size = static_cast<uint32_t>(copy_size);
}

bool IsDuplicateIdUnlocked(const std::string& id, const ModRecord* self) {
    for (ModRecord& record : g_mods) {
        if (&record == self || !record.active || record.id.empty()) continue;
        if (_stricmp(record.id.c_str(), id.c_str()) == 0) return true;
    }
    return false;
}

void NotifyLifecycleTransition(
    WotbModV3Handle mod,
    uint32_t transition) {
    std::vector<LifecycleStateHookFn> hooks;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        hooks = g_lifecycle_state_hooks;
    }
    for (LifecycleStateHookFn hook : hooks) {
        if (!hook ||
            CallLifecycleStateHookProtected(hook, mod, transition)) {
            continue;
        }
        RuntimeLog(
            WOTBMOD_V3_LOG_ERROR,
            "runtime",
            "a lifecycle state hook raised an exception");
    }
}

WotbModV3Result QueryInterfaceImpl(
    WotbModV3Handle mod,
    const char* interface_name,
    uint32_t minimum_version,
    const void** out_interface) {
    if (!interface_name || !interface_name[0] || minimum_version == 0u ||
        !out_interface) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "interface name, version and output are required");
    }
    *out_interface = nullptr;
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    InterfaceRecord* record =
        FindInterfaceUnlocked(interface_name, minimum_version);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            "requested interface or version is unavailable");
    }
    const WotbModV3Result access = CheckAccess(
        mod,
        record->required_permission_tier,
        record->allowed_contexts,
        record->capability_name.empty()
            ? nullptr
            : record->capability_name.c_str());
    if (access != WOTBMOD_V3_OK) return access;
    ModRecord* caller = FindModUnlocked(mod);
    if (!caller ||
        !InterfaceNamedPermissionGrantedUnlocked(
            *caller,
            *record)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "the mod did not declare or receive a permission for this interface");
    }
    if (record->implementation_status ==
        WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CLIENT_MISMATCH,
            record->implementation_reason.empty()
                ? "interface bindings do not match this client"
                : record->implementation_reason.c_str());
    }
    if (record->implementation_status ==
        WOTBMOD_V3_CAPABILITY_UNAVAILABLE) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_SUPPORTED,
            record->implementation_reason.empty()
                ? "interface implementation is unavailable"
                : record->implementation_reason.c_str());
    }
    *out_interface = record->table;
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetInterfaceInfoImpl(
    WotbModV3Handle mod,
    const char* interface_name,
    WotbModV3InterfaceInfo* out_info) {
    if (!interface_name || !out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "valid interface name and output descriptor are required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    InterfaceRecord* record = FindInterfaceUnlocked(interface_name, 1u);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_FOUND,
            "interface is not registered");
    }
    WotbModV3InterfaceInfo value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_ABI_VERSION;
    value.interface_version = record->version;
    value.status = record->implementation_status;
    CopyString(value.name, sizeof(value.name), record->name.c_str());
    CopyString(
        value.unavailable_reason,
        sizeof(value.unavailable_reason),
        record->implementation_reason.c_str());
    const WotbModV3Result access = CheckAccess(
        mod,
        record->required_permission_tier,
        record->allowed_contexts,
        record->capability_name.empty()
            ? nullptr
            : record->capability_name.c_str());
    ModRecord* caller = FindModUnlocked(mod);
    const bool named_permission_granted =
        caller &&
        InterfaceNamedPermissionGrantedUnlocked(
            *caller,
            *record);
    if (access == WOTBMOD_V3_E_PERMISSION_DENIED) {
        value.status = WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED;
        CopyString(
            value.unavailable_reason,
            sizeof(value.unavailable_reason),
            "permission tier is not granted");
    } else if (access == WOTBMOD_V3_OK &&
               !named_permission_granted) {
        value.status = WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED;
        CopyString(
            value.unavailable_reason,
            sizeof(value.unavailable_reason),
            "named permission is not declared or granted");
    } else if (access == WOTBMOD_V3_E_NOT_SUPPORTED) {
        value.status = WOTBMOD_V3_CAPABILITY_UNAVAILABLE;
        CapabilityRecord* capability = record->capability_name.empty()
            ? nullptr
            : FindCapabilityUnlocked(record->capability_name.c_str());
        CopyString(
            value.unavailable_reason,
            sizeof(value.unavailable_reason),
            capability && capability->info.unavailable_reason[0]
                ? capability->info.unavailable_reason
                : "native or service capability is unavailable");
    } else if (access == WOTBMOD_V3_E_CLIENT_MISMATCH) {
        value.status = WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH;
        CapabilityRecord* capability = record->capability_name.empty()
            ? nullptr
            : FindCapabilityUnlocked(record->capability_name.c_str());
        CopyString(
            value.unavailable_reason,
            sizeof(value.unavailable_reason),
            capability && capability->info.unavailable_reason[0]
                ? capability->info.unavailable_reason
                : "native bindings do not match this client");
    } else if (access == WOTBMOD_V3_E_INCOMPATIBLE) {
        value.status = WOTBMOD_V3_CAPABILITY_CONTEXT_RESTRICTED;
        CopyString(
            value.unavailable_reason,
            sizeof(value.unavailable_reason),
            "interface is unavailable in the current game context");
    } else if (access == WOTBMOD_V3_OK &&
               !record->capability_name.empty()) {
        CapabilityRecord* capability =
            FindCapabilityUnlocked(record->capability_name.c_str());
        if (capability &&
            capability->info.status ==
                WOTBMOD_V3_CAPABILITY_DEGRADED &&
            value.status == WOTBMOD_V3_CAPABILITY_AVAILABLE) {
            value.status = WOTBMOD_V3_CAPABILITY_DEGRADED;
            CopyString(
                value.unavailable_reason,
                sizeof(value.unavailable_reason),
                capability->info.unavailable_reason[0]
                    ? capability->info.unavailable_reason
                    : "required capability is degraded");
        }
    }
    const size_t copy_size =
        std::min<size_t>(out_info->struct_size, sizeof(value));
    memcpy(out_info, &value, copy_size);
    out_info->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetLastErrorImpl(
    WotbModV3Handle mod,
    WotbModV3ErrorInfo* out_error) {
    if (!out_error ||
        out_error->struct_size < sizeof(WotbModV3StructHeader)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        if (!FindModUnlocked(mod)) return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    WotbModV3ErrorInfo value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_ABI_VERSION;
    value.thread_role = CurrentThreadRole();
    value.owner_mod = mod;
    if (g_last_error.owner_mod == mod) value = g_last_error;
    const size_t copy_size =
        std::min<size_t>(out_error->struct_size, sizeof(value));
    memcpy(out_error, &value, copy_size);
    out_error->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetClientInfoImpl(
    WotbModV3Handle mod,
    WotbModV3ClientInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "client info output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModV3ClientInfo value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_ABI_VERSION;
    value.supported = 1u;
    value.compatibility_state = WOTBMOD_V3_CAPABILITY_AVAILABLE;
    value.binding_pack_version = g_binding_pack_version;
    value.process_architecture = g_process_architecture;
    CopyString(
        value.client_version,
        sizeof(value.client_version),
        g_client_version.c_str());
    CopyString(
        value.executable_sha256,
        sizeof(value.executable_sha256),
        g_executable_sha256.c_str());
    const size_t copy_size =
        std::min<size_t>(out_info->struct_size, sizeof(value));
    memcpy(out_info, &value, copy_size);
    out_info->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CoreLog(
    WotbModV3Handle mod,
    uint32_t level,
    const char* category,
    const char* message) {
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    if (level > WOTBMOD_V3_LOG_FATAL || !message) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "invalid log level or message");
    }
    RuntimeLog(level, category ? category : "mod", message);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CoreGetContext(
    WotbModV3Handle mod,
    uint64_t* out_context_mask) {
    if (!out_context_mask) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "context output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_context_mask = CurrentContext();
    return WOTBMOD_V3_OK;
}

WotbModV3Result CoreGetThreadRole(
    WotbModV3Handle mod,
    uint32_t* out_thread_role) {
    if (!out_thread_role) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "thread role output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_thread_role = CurrentThreadRole();
    return WOTBMOD_V3_OK;
}

WotbModV3Result CoreGetFrameIndex(
    WotbModV3Handle mod,
    uint64_t* out_frame_index) {
    if (!out_frame_index) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "frame index output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_frame_index = CurrentFrameIndex();
    return WOTBMOD_V3_OK;
}

WotbModV3Result CoreGetGameDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    return CopyOutString(mod, GameDirectory(), buffer, inout_size);
}

WotbModV3Result CoreGetDataDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModDataDirectory(mod, buffer, inout_size);
}

WotbModV3Result CoreGetCacheDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModCacheDirectory(mod, buffer, inout_size);
}

WotbModV3Result CoreGetConfigDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    return GetModConfigDirectory(mod, buffer, inout_size);
}

WotbModV3Result CapabilitiesGetCount(
    WotbModV3Handle mod,
    uint32_t* out_count) {
    if (!out_count) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "capability count output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    *out_count = static_cast<uint32_t>(g_capabilities.size());
    return WOTBMOD_V3_OK;
}

WotbModV3Result CapabilitiesGetAt(
    WotbModV3Handle mod,
    uint32_t index,
    WotbModV3CapabilityInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "capability output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (index >= g_capabilities.size()) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_FOUND,
            "capability index is outside the registry");
    }
    FillCapabilityInfo(out_info, g_capabilities[index].info);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CapabilitiesQuery(
    WotbModV3Handle mod,
    const char* capability_name,
    WotbModV3CapabilityInfo* out_info) {
    if (!capability_name || !out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "capability name and output are required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    CapabilityRecord* record = FindCapabilityUnlocked(capability_name);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_FOUND,
            "capability is not registered");
    }
    FillCapabilityInfo(out_info, record->info);
    return WOTBMOD_V3_OK;
}

WotbModV3Result CapabilitiesSubscribe(
    WotbModV3Handle mod,
    WotbModV3CapabilityChangedCallback callback,
    void* user_data,
    WotbModV3Token* out_token) {
    if (!callback || !out_token) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "callback and token output are required");
    }
    CapabilitySubscription* subscription =
        new (std::nothrow) CapabilitySubscription();
    if (!subscription) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PLATFORM,
            "unable to allocate capability subscription");
    }
    subscription->callback = callback;
    subscription->user_data = user_data;
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result = CreateOwnedHandle(
        mod,
        WOTBMOD_V3_HANDLE_CAPABILITY_SUBSCRIPTION,
        subscription,
        DestroyCapabilitySubscription,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        delete subscription;
        return result;
    }
    *out_token = handle;
    return WOTBMOD_V3_OK;
}

WotbModV3Result CapabilitiesUnsubscribe(
    WotbModV3Handle mod,
    WotbModV3Token token) {
    void* object = nullptr;
    const WotbModV3Result inspect = InspectOwnedHandle(
        mod,
        token,
        WOTBMOD_V3_HANDLE_CAPABILITY_SUBSCRIPTION,
        &object,
        nullptr);
    if (inspect != WOTBMOD_V3_OK) return inspect;
    CapabilitySubscription* subscription =
        static_cast<CapabilitySubscription*>(object);
    if (!subscription || !subscription->callback) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "token is not a capability subscription");
    }
    return ReleaseOwnedHandle(mod, token);
}

WotbModV3Result PermissionsGetTier(
    WotbModV3Handle mod,
    uint32_t* out_tier) {
    if (!out_tier) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "permission tier output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_tier = ModPermissionTier(mod);
    return WOTBMOD_V3_OK;
}

void FillPermissionInfo(
    WotbModV3PermissionInfo* out_info,
    const PermissionDefinition& permission,
    uint32_t granted_tier,
    bool named_permission_granted) {
    WotbModV3PermissionInfo value = {};
    value.struct_size = sizeof(value);
    value.api_version = WOTBMOD_V3_PERMISSIONS_VERSION;
    value.tier = permission.tier;
    if (granted_tier < permission.tier) {
        value.state =
            permission.tier == WOTBMOD_V3_PERMISSION_UNSAFE
                ? WOTBMOD_V3_PERMISSION_STATE_DENIED
                : WOTBMOD_V3_PERMISSION_STATE_REVIEW_REQUIRED;
    } else {
        value.state = named_permission_granted
            ? WOTBMOD_V3_PERMISSION_STATE_GRANTED
            : WOTBMOD_V3_PERMISSION_STATE_DENIED;
    }
    CopyString(value.name, sizeof(value.name), permission.name);
    if (value.state != WOTBMOD_V3_PERMISSION_STATE_GRANTED) {
        CopyString(
            value.reason,
            sizeof(value.reason),
            granted_tier >= permission.tier &&
                    !named_permission_granted
                ? "permission was not declared and granted to this mod"
                : permission.tier == WOTBMOD_V3_PERMISSION_UNSAFE
                ? "unsafe permission is not granted to catalog mods"
                : "the installed mod grant does not include this tier");
    }
    const size_t copy_size =
        std::min<size_t>(out_info->struct_size, sizeof(value));
    memcpy(out_info, &value, copy_size);
    out_info->struct_size = static_cast<uint32_t>(copy_size);
}

WotbModV3Result PermissionsQuery(
    WotbModV3Handle mod,
    const char* permission_name,
    WotbModV3PermissionInfo* out_info) {
    if (!permission_name || !out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "permission name and output are required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    const PermissionDefinition* permission =
        FindPermission(permission_name);
    if (!permission) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_FOUND,
            "permission is not defined");
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid or stale");
    }
    FillPermissionInfo(
        out_info,
        *permission,
        record->permission_tier,
        HasNamedPermissionUnlocked(*record, permission->name));
    return WOTBMOD_V3_OK;
}

WotbModV3Result PermissionsGetCount(
    WotbModV3Handle mod,
    uint32_t* out_count) {
    if (!out_count) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "permission count output is required");
    }
    if (CheckMod(mod) != WOTBMOD_V3_OK) return WOTBMOD_V3_E_INVALID_HANDLE;
    *out_count = static_cast<uint32_t>(
        sizeof(kPermissions) / sizeof(kPermissions[0]));
    return WOTBMOD_V3_OK;
}

WotbModV3Result PermissionsGetAt(
    WotbModV3Handle mod,
    uint32_t index,
    WotbModV3PermissionInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "permission output is required");
    }
    const uint32_t count = static_cast<uint32_t>(
        sizeof(kPermissions) / sizeof(kPermissions[0]));
    if (index >= count) {
        return SetError(
            mod,
            WOTBMOD_V3_E_NOT_FOUND,
            "permission index is outside the registry");
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid or stale");
    }
    FillPermissionInfo(
        out_info,
        kPermissions[index],
        record->permission_tier,
        HasNamedPermissionUnlocked(
            *record,
            kPermissions[index].name));
    return WOTBMOD_V3_OK;
}

WotbModV3Result HandlesRetain(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    return RetainOwnedHandle(mod, handle);
}

WotbModV3Result HandlesRelease(
    WotbModV3Handle mod,
    WotbModV3Handle handle) {
    return ReleaseOwnedHandle(mod, handle);
}

WotbModV3Result HandlesGetInfo(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    WotbModV3HandleInfo* out_info) {
    if (!out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "handle info output is required");
    }
    return InspectOwnedHandle(mod, handle, 0u, nullptr, out_info);
}

WotbModV3Result HandlesIsAlive(
    WotbModV3Handle mod,
    WotbModV3Handle handle,
    uint32_t* out_alive) {
    if (!out_alive) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "alive output is required");
    }
    *out_alive = InspectOwnedHandle(
        mod,
        handle,
        0u,
        nullptr,
        nullptr) == WOTBMOD_V3_OK ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

const WotbModV3CoreApiV1 kCoreApi = {
    sizeof(WotbModV3CoreApiV1),
    WOTBMOD_V3_CORE_VERSION,
    CoreLog,
    CoreGetContext,
    CoreGetThreadRole,
    CoreGetFrameIndex,
    CoreGetGameDirectory,
    CoreGetDataDirectory,
    CoreGetCacheDirectory,
    CoreGetConfigDirectory
};

const WotbModV3CapabilitiesApiV1 kCapabilitiesApi = {
    sizeof(WotbModV3CapabilitiesApiV1),
    WOTBMOD_V3_CAPABILITIES_VERSION,
    CapabilitiesGetCount,
    CapabilitiesGetAt,
    CapabilitiesQuery,
    CapabilitiesSubscribe,
    CapabilitiesUnsubscribe
};

const WotbModV3PermissionsApiV1 kPermissionsApi = {
    sizeof(WotbModV3PermissionsApiV1),
    WOTBMOD_V3_PERMISSIONS_VERSION,
    PermissionsGetTier,
    PermissionsQuery,
    PermissionsGetCount,
    PermissionsGetAt
};

const WotbModV3HandlesApiV1 kHandlesApi = {
    sizeof(WotbModV3HandlesApiV1),
    WOTBMOD_V3_HANDLES_VERSION,
    HandlesRetain,
    HandlesRelease,
    HandlesGetInfo,
    HandlesIsAlive
};

}  // namespace

WotbModV3Result RegisterInterface(
    const InterfaceRegistration& registration) {
    if (!registration.name || !registration.name[0] ||
        registration.version == 0u || !registration.table ||
        registration.required_permission_tier >
            WOTBMOD_V3_PERMISSION_UNSAFE ||
        (registration.implementation_status != 0xFFFFFFFFu &&
         !IsImplementationStatus(
             registration.implementation_status))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (g_interfaces.size() >= kMaxInterfaces) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    for (const InterfaceRecord& existing : g_interfaces) {
        if (_stricmp(existing.name.c_str(), registration.name) == 0 &&
            existing.version == registration.version) {
            return WOTBMOD_V3_E_ALREADY_EXISTS;
        }
    }
    InterfaceRecord record;
    record.name = registration.name;
    record.version = registration.version;
    record.table = registration.table;
    record.required_permission_tier =
        registration.required_permission_tier;
    record.allowed_contexts = registration.allowed_contexts
        ? registration.allowed_contexts
        : WOTBMOD_V3_CONTEXT_ALL;
    if (registration.capability_name) {
        record.capability_name = registration.capability_name;
    }
    const InterfaceAvailabilityDefault availability =
        DefaultInterfaceAvailability(registration.name);
    record.implementation_status =
        registration.implementation_status == 0xFFFFFFFFu
            ? availability.status
            : registration.implementation_status;
    if (record.implementation_status ==
        WOTBMOD_V3_CAPABILITY_AVAILABLE) {
        record.implementation_reason.clear();
    } else if (registration.implementation_reason &&
               registration.implementation_reason[0]) {
        record.implementation_reason =
            registration.implementation_reason;
    } else if (registration.implementation_status == 0xFFFFFFFFu) {
        record.implementation_reason = availability.reason;
    } else if (record.implementation_status ==
               WOTBMOD_V3_CAPABILITY_DEGRADED) {
        record.implementation_reason =
            "interface implementation is degraded";
    } else if (record.implementation_status ==
               WOTBMOD_V3_CAPABILITY_UNAVAILABLE) {
        record.implementation_reason =
            "interface implementation is unavailable";
    } else if (record.implementation_status ==
               WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH) {
        record.implementation_reason =
            "interface bindings do not match this client";
    }
    g_interfaces.push_back(record);

    if (!record.capability_name.empty()) {
        CapabilityRecord* capability =
            FindCapabilityUnlocked(record.capability_name.c_str());
        if (!capability && g_capabilities.size() < kMaxCapabilities) {
            CapabilityRecord created;
            created.info.struct_size = sizeof(created.info);
            created.info.api_version =
                WOTBMOD_V3_CAPABILITIES_VERSION;
            created.interface_derived = true;
            CopyString(
                created.info.name,
                sizeof(created.info.name),
                record.capability_name.c_str());
            g_capabilities.push_back(created);
            capability = &g_capabilities.back();
        }
        if (capability && capability->interface_derived) {
            RefreshDerivedCapabilityUnlocked(
                record.capability_name.c_str());
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result SetInterfaceAvailability(
    const char* interface_name,
    uint32_t status,
    const char* reason) {
    if (!interface_name || !interface_name[0] ||
        !IsImplementationStatus(status)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    bool found = false;
    std::vector<std::string> derived_capabilities;
    for (InterfaceRecord& record : g_interfaces) {
        if (_stricmp(record.name.c_str(), interface_name) != 0) {
            continue;
        }
        record.implementation_status = status;
        if (status == WOTBMOD_V3_CAPABILITY_AVAILABLE) {
            record.implementation_reason.clear();
        } else if (reason && reason[0]) {
            record.implementation_reason = reason;
        } else if (status == WOTBMOD_V3_CAPABILITY_DEGRADED) {
            record.implementation_reason =
                "interface implementation is degraded";
        } else if (status == WOTBMOD_V3_CAPABILITY_UNAVAILABLE) {
            record.implementation_reason =
                "interface implementation is unavailable";
        } else if (status ==
                   WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH) {
            record.implementation_reason =
                "interface bindings do not match this client";
        }
        if (!record.capability_name.empty() &&
            std::find(
                derived_capabilities.begin(),
                derived_capabilities.end(),
                record.capability_name) ==
                derived_capabilities.end()) {
            derived_capabilities.push_back(
                record.capability_name);
        }
        found = true;
    }
    for (const std::string& capability_name :
         derived_capabilities) {
        RefreshDerivedCapabilityUnlocked(
            capability_name.c_str());
    }
    return found ? WOTBMOD_V3_OK : WOTBMOD_V3_E_NOT_FOUND;
}

WotbModV3Result SetError(
    WotbModV3Handle mod,
    WotbModV3Result code,
    const char* message,
    const char* context_json) {
    ZeroMemory(&g_last_error, sizeof(g_last_error));
    g_last_error.struct_size = sizeof(g_last_error);
    g_last_error.api_version = WOTBMOD_V3_ABI_VERSION;
    g_last_error.code = static_cast<uint32_t>(code);
    g_last_error.thread_role = CurrentThreadRole();
    g_last_error.owner_mod = mod;
    CopyString(
        g_last_error.message,
        sizeof(g_last_error.message),
        message ? message : "");
    CopyString(
        g_last_error.context_json,
        sizeof(g_last_error.context_json),
        context_json ? context_json : "");
    return code;
}

WotbModV3Result CheckMod(WotbModV3Handle mod) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (!g_initialized || !FindModUnlocked(mod)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid or stale");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result CheckAccess(
    WotbModV3Handle mod,
    uint32_t required_permission_tier,
    uint64_t allowed_contexts,
    const char* capability_name) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid or stale");
    }
    if (required_permission_tier > record->permission_tier) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "the mod has not been granted the required permission tier");
    }
    if (CapabilityNameRequiresNamedGrant(capability_name) &&
        !CapabilityNamedPermissionGrantedUnlocked(
            *record,
            capability_name)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "the mod did not declare or receive the named capability permission");
    }
    const uint64_t context = CurrentContext();
    if (allowed_contexts != 0u && context != WOTBMOD_V3_CONTEXT_NONE &&
        (allowed_contexts & context) == 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INCOMPATIBLE,
            "operation is unavailable in the current game context");
    }
    if (capability_name && capability_name[0]) {
        CapabilityRecord* capability =
            FindCapabilityUnlocked(capability_name);
        if (!capability) {
            return SetError(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                "required capability is not registered");
        }
        if (capability->info.status ==
            WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED) {
            return SetError(
                mod,
                WOTBMOD_V3_E_PERMISSION_DENIED,
                capability->info.unavailable_reason);
        }
        if (capability->info.status ==
            WOTBMOD_V3_CAPABILITY_CONTEXT_RESTRICTED) {
            return SetError(
                mod,
                WOTBMOD_V3_E_INCOMPATIBLE,
                capability->info.unavailable_reason);
        }
        if (capability->info.status ==
            WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH) {
            return SetError(
                mod,
                WOTBMOD_V3_E_CLIENT_MISMATCH,
                capability->info.unavailable_reason[0]
                    ? capability->info.unavailable_reason
                    : "required capability does not match this client");
        }
        if (capability->info.status != WOTBMOD_V3_CAPABILITY_AVAILABLE &&
            capability->info.status != WOTBMOD_V3_CAPABILITY_DEGRADED) {
            return SetError(
                mod,
                WOTBMOD_V3_E_NOT_SUPPORTED,
                capability->info.unavailable_reason[0]
                    ? capability->info.unavailable_reason
                    : "required capability is unavailable");
        }
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result CheckNamedPermission(
    WotbModV3Handle mod,
    const char* permission_name,
    uint32_t required_permission_tier) {
    if (!IsValidPermissionGrantName(permission_name) ||
        required_permission_tier >
            WOTBMOD_V3_PERMISSION_UNSAFE) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "a valid permission name and tier are required");
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid or stale");
    }
    if (required_permission_tier > record->permission_tier) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "the mod has not been granted the required permission tier");
    }
    if (!HasNamedPermissionUnlocked(*record, permission_name)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_PERMISSION_DENIED,
            "the mod did not declare or receive the named permission");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result CreateOwnedHandle(
    WotbModV3Handle owner,
    uint32_t type,
    void* object,
    HandleDestroyFn destroy,
    WotbModV3Handle* out_handle) {
    if (!out_handle || type == WOTBMOD_V3_HANDLE_UNKNOWN ||
        type > 0xFFu) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "valid handle type and output are required");
    }
    *out_handle = WOTBMOD_V3_INVALID_HANDLE;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (type != WOTBMOD_V3_HANDLE_MOD) {
        ModRecord* owner_record = FindModUnlocked(owner);
        if (!owner_record) {
            return SetError(
                owner,
                WOTBMOD_V3_E_INVALID_HANDLE,
                "handle owner is invalid");
        }
        const bool loading =
            owner_record->state == MOD_STATE_CREATED ||
            owner_record->state == MOD_STATE_LOADED;
        /*
         * Third and last exemption: this thread is the one that opened an
         * enable transition for this exact owner and has not closed it yet.
         * It is deliberately not "the owner is being enabled" -- another
         * thread, including one belonging to the still-disabled mod, sees
         * the refusal below unchanged.
         */
        const bool enabling =
            owner_record->enable_transition_depth > 0u &&
            owner_record->enable_transition_thread ==
                std::this_thread::get_id();
        if (!loading && !enabling &&
            !owner_record->accepting_callbacks) {
            return SetError(
                owner,
                WOTBMOD_V3_E_CANCELLED,
                "handle owner is stopping or disabled");
        }
    }
    for (uint32_t slot = 1u; slot < kMaxHandles; ++slot) {
        HandleRecord& record = g_handles[slot];
        if (record.alive) continue;
        if (record.generation == 0u) record.generation = 1u;
        record.alive = true;
        record.type = type;
        record.references = 1u;
        record.owner = owner;
        record.object = object;
        record.destroy = destroy;
        const WotbModV3Handle handle =
            EncodeHandle(slot, record.generation, type);
        if (type == WOTBMOD_V3_HANDLE_MOD) record.owner = handle;
        *out_handle = handle;
        return WOTBMOD_V3_OK;
    }
    return SetError(
        owner,
        WOTBMOD_V3_E_LIMIT_REACHED,
        "global handle registry is full");
}

WotbModV3Result RetainOwnedHandle(
    WotbModV3Handle owner,
    WotbModV3Handle handle) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (!FindModUnlocked(owner)) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle owner is invalid");
    }
    HandleRecord* record = FindHandleUnlocked(handle);
    if (!record || record->owner != owner ||
        record->type == WOTBMOD_V3_HANDLE_MOD) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is stale, foreign, or non-retainable");
    }
    if (record->references == UINT32_MAX) {
        return SetError(
            owner,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "handle reference count overflow");
    }
    ++record->references;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ReleaseOwnedHandle(
    WotbModV3Handle owner,
    WotbModV3Handle handle) {
    void* object = nullptr;
    HandleDestroyFn destroy = nullptr;
    {
        std::unique_lock<std::recursive_mutex> lock(g_state_mutex);
        if (!FindModUnlocked(owner)) {
            return SetError(
                owner,
                WOTBMOD_V3_E_INVALID_HANDLE,
                "handle owner is invalid");
        }
        HandleRecord* record = FindHandleUnlocked(handle);
        if (!record || record->owner != owner ||
            record->type == WOTBMOD_V3_HANDLE_MOD) {
            return SetError(
                owner,
                WOTBMOD_V3_E_INVALID_HANDLE,
                "handle is stale, foreign, or non-releasable");
        }
        if (--record->references != 0u) return WOTBMOD_V3_OK;
        object = record->object;
        destroy = record->destroy;
        record->alive = false;
        record->type = WOTBMOD_V3_HANDLE_UNKNOWN;
        record->owner = WOTBMOD_V3_INVALID_HANDLE;
        record->object = nullptr;
        record->destroy = nullptr;
        record->generation = NextGeneration(record->generation);
    }
    if (destroy) destroy(object);
    return WOTBMOD_V3_OK;
}

WotbModV3Result InspectOwnedHandle(
    WotbModV3Handle owner,
    WotbModV3Handle handle,
    uint32_t expected_type,
    void** out_object,
    WotbModV3HandleInfo* out_info) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (!FindModUnlocked(owner)) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle owner is invalid");
    }
    HandleRecord* record = FindHandleUnlocked(handle);
    if (!record || record->owner != owner ||
        (expected_type != 0u && record->type != expected_type)) {
        return SetError(
            owner,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "handle is stale, foreign, or has the wrong type");
    }
    if (out_object) *out_object = record->object;
    if (out_info) {
        WotbModV3HandleInfo value = {};
        value.struct_size = sizeof(value);
        value.api_version = WOTBMOD_V3_HANDLES_VERSION;
        value.type = record->type;
        value.reference_count = record->references;
        value.alive = record->alive ? 1u : 0u;
        value.owner_mod = record->owner;
        uint32_t generation = 0u;
        DecodeHandle(handle, nullptr, &generation, nullptr);
        value.generation = generation;
        const size_t copy_size =
            std::min<size_t>(out_info->struct_size, sizeof(value));
        memcpy(out_info, &value, copy_size);
        out_info->struct_size = static_cast<uint32_t>(copy_size);
    }
    return WOTBMOD_V3_OK;
}

void ReleaseAllOwnedHandles(WotbModV3Handle owner) {
    std::vector<std::pair<void*, HandleDestroyFn>> pending;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        for (uint32_t slot = 1u; slot < kMaxHandles; ++slot) {
            HandleRecord& record = g_handles[slot];
            if (!record.alive || record.owner != owner ||
                record.type == WOTBMOD_V3_HANDLE_MOD) {
                continue;
            }
            pending.push_back(std::make_pair(record.object, record.destroy));
            record.alive = false;
            record.type = WOTBMOD_V3_HANDLE_UNKNOWN;
            record.references = 0u;
            record.owner = WOTBMOD_V3_INVALID_HANDLE;
            record.object = nullptr;
            record.destroy = nullptr;
            record.generation = NextGeneration(record.generation);
        }
    }
    for (const auto& item : pending) {
        if (item.second) item.second(item.first);
    }
}

WotbModV3Result EnterModCallbackInternal(
    WotbModV3Handle mod,
    bool coalescible) {
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
    }
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "callback owner is invalid");
    }
    std::unique_lock<std::mutex> lock(record->callback_mutex);
    if (!record->accepting_callbacks) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CANCELLED,
            "mod is no longer accepting callbacks");
    }
    AdvanceCallbackFrameLocked(record, CurrentFrameIndex());
    const uint64_t budget_100ns =
        static_cast<uint64_t>(
            record->callback_budget_microseconds) * 10u;
    if (coalescible &&
        (record->frame_callbacks >=
             record->max_callbacks_per_frame ||
         record->frame_cpu_100ns >= budget_100ns)) {
        ++record->coalesced_callbacks;
        if (!record->frame_over_budget) {
            record->frame_over_budget = true;
            ++record->over_budget_frames;
        }
        return WOTBMOD_V3_E_BUSY;
    }
    ++record->callbacks_in_flight;
    ++record->frame_callbacks;
    if (g_callback_nesting == 0u) g_callback_mod = mod;
    ++g_callback_nesting;
    if (g_callback_timer_depth <
        static_cast<uint32_t>(
            sizeof(g_callback_timers) /
            sizeof(g_callback_timers[0]))) {
        CallbackTimer& timer =
            g_callback_timers[g_callback_timer_depth++];
        timer.mod = mod;
        timer.started_100ns = QueryTime100ns();
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result EnterModCallback(WotbModV3Handle mod) {
    return EnterModCallbackInternal(mod, false);
}

WotbModV3Result EnterModCallbackCoalescible(
    WotbModV3Handle mod) {
    return EnterModCallbackInternal(mod, true);
}

void LeaveModCallback(WotbModV3Handle mod) {
    uint64_t elapsed_100ns = 0u;
    if (g_callback_timer_depth > 0u) {
        CallbackTimer& timer =
            g_callback_timers[g_callback_timer_depth - 1u];
        if (timer.mod == mod) {
            const uint64_t finished = QueryTime100ns();
            elapsed_100ns = finished >= timer.started_100ns
                ? finished - timer.started_100ns
                : 0u;
            timer = {};
            --g_callback_timer_depth;
        }
    }
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
    }
    if (!record) return;
    {
        std::lock_guard<std::mutex> lock(record->callback_mutex);
        ++record->total_callbacks;
        record->total_callback_cpu_100ns += elapsed_100ns;
        record->frame_cpu_100ns += elapsed_100ns;
        record->max_callback_100ns = std::max(
            record->max_callback_100ns,
            elapsed_100ns);
        if (elapsed_100ns >= kSlowCallback100ns) {
            ++record->slow_callbacks;
        }
        const uint64_t budget_100ns =
            static_cast<uint64_t>(
                record->callback_budget_microseconds) * 10u;
        if (record->frame_cpu_100ns >= budget_100ns &&
            !record->frame_over_budget) {
            record->frame_over_budget = true;
            ++record->over_budget_frames;
        }
        if (record->callbacks_in_flight > 0u) {
            --record->callbacks_in_flight;
        }
        if (record->callbacks_in_flight == 0u) {
            record->callback_cv.notify_all();
        }
    }
    if (g_callback_nesting > 0u) --g_callback_nesting;
    if (g_callback_nesting == 0u) {
        g_callback_mod = WOTBMOD_V3_INVALID_HANDLE;
    }
}

WotbModV3Result GetModCallbackProfile(
    WotbModV3Handle mod,
    WotbModV3CallbackProfile* out_profile) {
    if (!out_profile ||
        out_profile->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "callback profile output is required");
    }
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
    }
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "callback profile owner is invalid");
    }
    WotbModV3CallbackProfile value = {};
    {
        std::lock_guard<std::mutex> lock(record->callback_mutex);
        AdvanceCallbackFrameLocked(record, CurrentFrameIndex());
        value.struct_size = sizeof(value);
        value.api_version = WOTBMOD_V3_DEVTOOLS_VERSION_3;
        value.budget_microseconds =
            record->callback_budget_microseconds;
        value.max_callbacks_per_frame =
            record->max_callbacks_per_frame;
        value.frame_index = record->callback_profile_frame;
        value.frame_callbacks = record->frame_callbacks;
        value.frame_cpu_100ns = record->frame_cpu_100ns;
        value.previous_frame_callbacks =
            record->previous_frame_callbacks;
        value.previous_frame_cpu_100ns =
            record->previous_frame_cpu_100ns;
        value.total_callbacks = record->total_callbacks;
        value.total_cpu_100ns =
            record->total_callback_cpu_100ns;
        value.max_callback_100ns = record->max_callback_100ns;
        value.slow_callbacks = record->slow_callbacks;
        value.coalesced_callbacks = record->coalesced_callbacks;
        value.over_budget_frames = record->over_budget_frames;
    }
    const size_t copy_size = std::min<size_t>(
        out_profile->struct_size, sizeof(value));
    std::memcpy(out_profile, &value, copy_size);
    out_profile->struct_size = static_cast<uint32_t>(copy_size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result SetModCallbackBudget(
    WotbModV3Handle mod,
    uint32_t budget_microseconds,
    uint32_t max_callbacks_per_frame) {
    if (budget_microseconds < 100u ||
        budget_microseconds > 8000u ||
        max_callbacks_per_frame == 0u ||
        max_callbacks_per_frame > 1024u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "callback budget must be 100..8000 us and 1..1024 callbacks");
    }
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
    }
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(record->callback_mutex);
    record->callback_budget_microseconds = budget_microseconds;
    record->max_callbacks_per_frame = max_callbacks_per_frame;
    return WOTBMOD_V3_OK;
}

WotbModV3Result ResetModCallbackProfile(WotbModV3Handle mod) {
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
    }
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    std::lock_guard<std::mutex> lock(record->callback_mutex);
    ResetCallbackProfileLocked(record);
    AdvanceCallbackFrameLocked(record, CurrentFrameIndex());
    return WOTBMOD_V3_OK;
}

bool IsModEnabled(WotbModV3Handle mod) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    return record && record->enabled;
}

uint32_t ModState(WotbModV3Handle mod) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    return record ? record->state : MOD_STATE_CREATED;
}

/*
 * Declares that the calling thread is in the middle of enabling `mod` and
 * is about to build resources the mod will own -- in practice, the package
 * mount that on_enable is going to read from. For as long as the window is
 * open, and only on this thread, CreateOwnedHandle treats the owner as if
 * it were already accepting callbacks.
 *
 * The mod's observable state is deliberately not touched. It stays
 * MOD_STATE_DISABLED (or MOD_STATE_LOADED on first load), so ModState()
 * keeps returning values the public ABI already defines, Enable() still
 * sees an enableable record, and a caller that forgets to close the window
 * leaves the mod disabled rather than half-enabled.
 */
WotbModV3Result BeginModEnableTransition(WotbModV3Handle mod) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    /*
     * Refuse only the states where handing out a handle-creation exemption
     * would be wrong: the record is going away. Everything else is either
     * a state that needs the window (DISABLED, the re-enable case this
     * exists for) or one where it is a harmless no-op -- CREATED and
     * LOADED are already exempt inside CreateOwnedHandle, and an ENABLED
     * record is already accepting callbacks.
     *
     * MOD_STATE_ENABLED in particular must NOT be refused, however
     * redundant a window looks for it. A mod that requests disable from
     * inside one of its own callbacks is turned away by the E_BUSY guard
     * in WotbModV3Runtime_Disable below, which returns before it touches
     * `state` or `accepting_callbacks` -- so the v3 record stays ENABLED
     * while the legacy facade has already written enabled=0 and
     * WOTBMOD_STATE_DISABLED (SetRecordEnabled does that before calling
     * CallDisable, and CallDisable only logs a failure). The next enable
     * then arrives here with a legacy record that is disabled and a v3
     * record that is ENABLED. That path is meant to work: see the
     * E_ALREADY_EXISTS -> OK branch in EnsureV3PackageMounted, whose
     * comment describes this exact desync and says re-enable "must not be
     * destructive". Refusing here would strand such a mod disabled until
     * the game restarts, because nothing afterwards moves the v3 record
     * out of ENABLED.
     *
     * Written as a refusal list rather than an allow-list of the other
     * four states on purpose. The property that matters is "the record is
     * being torn down", and naming it directly is what stops someone
     * reading the list as arbitrary and trimming it.
     */
    if (record->state == MOD_STATE_UNLOADING ||
        record->state == MOD_STATE_FAULTED) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "mod is not in an enableable state");
    }
    if (record->enable_transition_depth > 0u &&
        record->enable_transition_thread !=
            std::this_thread::get_id()) {
        return SetError(
            mod,
            WOTBMOD_V3_E_BUSY,
            "another thread is already enabling this mod");
    }
    record->enable_transition_thread = std::this_thread::get_id();
    ++record->enable_transition_depth;
    return WOTBMOD_V3_OK;
}

/*
 * Closes a window opened by BeginModEnableTransition. Only the thread that
 * opened it can close it, and a mismatch is ignored rather than reported:
 * this runs from a __finally on every exit path, including the failing
 * ones, and must not itself be able to fail.
 *
 * The lock below is the only one in this codebase acquired while an SEH
 * unwind is in progress -- MountV3PackageDuringEnable's __finally is a
 * termination handler, and it runs on the way out of an access violation
 * as well as on the way out of a return. That is unusual enough to be
 * worth arguing for in place rather than leaving to be rediscovered.
 *
 * It has to be taken. The two fields cleared below are guarded by
 * g_state_mutex and read from other threads -- BeginModEnableTransition's
 * "another thread is already enabling this mod", and CreateOwnedHandle's
 * exemption test. One of them is a std::thread::id, which is not lock-free
 * and has no atomic form, so clearing them unguarded is a data race on the
 * exact field whose whole purpose is to say which thread owns an
 * exemption. Getting that wrong hands handle creation to a thread that
 * never opened a window, which is the failure this mechanism exists to
 * prevent.
 *
 * It cannot self-deadlock or terminate on the unwinding thread. The
 * __finally runs on the thread that opened the window; if the fault was
 * raised under this same lock, the skipped lock_guards left the recursion
 * count non-zero and this thread still owns it, so a recursive re-entry is
 * a counter bump. std::lock_guard's own construction is EnterCriticalSection
 * underneath for a recursive mutex and does not throw here -- which matters,
 * because a C++ exception escaping a __finally during an unwind is a
 * terminate().
 *
 * It cannot hang. An earlier version of this paragraph justified that by
 * claiming that all 67 of g_state_mutex's critical sections in this file
 * are short, non-reentrant field access, with no call into foreign code,
 * no wait, no sleep and no join. That claim is false. It is recorded here
 * as false so that nobody re-derives it and reads it as permission to add
 * a blocking operation under this lock: the next SEH unwind through the
 * lock_guard below would then block the faulting thread inside a
 * termination handler, which is a hang with no crash dump. Two of the 67
 * break the claim and both are named below.
 *
 * The property that actually holds is narrower, and it is the one this
 * argument needs, because the only thing this __finally can do is wait
 * behind whoever holds the lock at that moment:
 *
 *     no critical section that can be concurrent with an open enable
 *     window blocks on anything this file does not bound.
 *
 * Exception one, and the only call out of this file's control anywhere
 * under this lock. WotbModV3Runtime_Initialize takes it at :3310 and
 * holds it to :3423. Inside that region it calls NormalizeDirectory four
 * times, EnsureDirectoryTree three times (:3328-3330 -- that is
 * GetFileAttributesA plus recursive CreateDirectoryA, :378-390),
 * RegisterCoreServices (:3405), SetNativeHookBackend (:3411), four
 * Register*Services (:3415-3418) and RuntimeLog (:3419), which calls
 * g_log_sink (:3155-3160) -- a host-supplied function pointer whose
 * runtime this file cannot bound at all. It is not concurrent with an
 * enable window today, but not for any reason the old claim gave: the
 * body past the g_initialized check only runs while g_initialized is
 * false, and a window can only be open on a ModRecord, which only exists
 * after WotbModV3Runtime_CreateMod. The gap that argument leaves is
 * Shutdown followed by a second Initialize while another thread still
 * holds a window on a destroyed record, so that is the sequence to check
 * before putting anything else blocking under this lock. Moving the
 * :3419 RuntimeLog past a lock.unlock(), exactly as :3557 already does
 * for NotifyLifecycleTransition, would delete the host callout from the
 * locked region; that is the enforcement to reach for if this ever has to
 * be more than an argument, and it was left undone here only because
 * reading g_log_sink outside the lock is its own question.
 *
 * Exception two. WotbModV3Runtime_CreateMod takes the lock at :3487 and
 * is reentrant on it -- CreateOwnedHandle at :3545 re-acquires at :2348
 * -- and holds it across three EnsureDirectoryTree calls at :3538-3540.
 * Unlike Initialize this one can be concurrent with an enable window on
 * some other mod. It is still bounded: recursive re-entry by the owning
 * thread is a counter bump and not a wait, and local directory creation
 * calls nothing outside this process. Bounded, not short, and that
 * distinction is the whole point.
 *
 * The rest of the accounting stands, re-counted at HEAD: 67 critical
 * sections on this mutex in this file, 64 std::lock_guard plus three
 * std::unique_lock (:2435, :2944 and the one below at :3487). The three
 * unique_locks are the strongest case rather than the exception -- :3487
 * takes one *in order to* lock.unlock() at :3557 before
 * NotifyLifecycleTransition calls out. The one condition_variable::wait
 * in this file (:3772) is on a ModRecord's own callback_mutex, not on
 * this lock.
 *
 * The one case where this waits forever is the case where the runtime is
 * already wedged by the leaked recursion count described at g_state_mutex's
 * declaration, and there this call is a symptom rather than the cause.
 * That leaked count is not fixable here. See the declaration.
 */
void EndModEnableTransition(WotbModV3Handle mod) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record || record->enable_transition_depth == 0u) return;
    if (record->enable_transition_thread !=
        std::this_thread::get_id()) {
        return;
    }
    if (--record->enable_transition_depth == 0u) {
        record->enable_transition_thread = std::thread::id();
    }
}

uint32_t ModPermissionTier(WotbModV3Handle mod) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    return record
        ? record->permission_tier
        : WOTBMOD_V3_PERMISSION_SAFE;
}

const char* ModId(WotbModV3Handle mod) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    return record ? record->id.c_str() : "";
}

WotbModV3Result FindModById(
    const char* id,
    WotbModV3Handle* out_mod) {
    if (!id || !id[0] || !out_mod) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_mod = WOTBMOD_V3_INVALID_HANDLE;
    std::unique_lock<std::recursive_mutex> lock(g_state_mutex);
    for (const ModRecord& record : g_mods) {
        if (!record.active || record.id.empty()) continue;
        if (_stricmp(record.id.c_str(), id) == 0) {
            *out_mod = record.handle;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

WotbModV3Result GetModVersion(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t buffer_size) {
    if (!buffer || buffer_size == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    const size_t required = record->version.size() + 1u;
    if (required > buffer_size) return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    CopyString(buffer, buffer_size, record->version.c_str());
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetModInstallDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    return CopyOutString(
        mod,
        record->install_directory.c_str(),
        buffer,
        inout_size);
}

WotbModV3Result GetModResourceDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    return CopyOutString(
        mod,
        record->resource_directory.c_str(),
        buffer,
        inout_size);
}

WotbModV3Result ResolveDeclaredDependency(
    WotbModV3Handle mod,
    const char* dependency_id,
    WotbModV3Handle* out_dependency,
    uint32_t* out_optional) {
    if (!dependency_id || !dependency_id[0] ||
        !out_dependency || !out_optional) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_dependency = WOTBMOD_V3_INVALID_HANDLE;
    *out_optional = 0u;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* caller = FindModUnlocked(mod);
    if (!caller) return WOTBMOD_V3_E_INVALID_HANDLE;
    const WotbModV3ManifestDependency* declared = nullptr;
    for (const WotbModV3ManifestDependency& dependency :
         caller->dependencies) {
        if ((dependency.kind == WOTBMOD_V3_DEPENDENCY_REQUIRED ||
             dependency.kind == WOTBMOD_V3_DEPENDENCY_OPTIONAL) &&
            _stricmp(dependency.id, dependency_id) == 0) {
            declared = &dependency;
            break;
        }
    }
    if (!declared) return WOTBMOD_V3_E_NOT_FOUND;
    *out_optional =
        declared->kind == WOTBMOD_V3_DEPENDENCY_OPTIONAL ? 1u : 0u;
    for (const ModRecord& candidate : g_mods) {
        if (!candidate.active || candidate.id.empty()) continue;
        if (_stricmp(candidate.id.c_str(), declared->id) == 0) {
            *out_dependency = candidate.handle;
            return WOTBMOD_V3_OK;
        }
    }
    return WOTBMOD_V3_E_NOT_FOUND;
}

uint64_t CurrentContext() {
    return g_context.load(std::memory_order_acquire);
}

uint64_t CurrentFrameIndex() {
    return g_frame_index.load(std::memory_order_acquire);
}

void ObserveExternalFrame(uint64_t frameIndex) {
    g_frame_index.store(frameIndex, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    for (ModRecord& record : g_mods) {
        if (!record.active) continue;
        std::lock_guard<std::mutex> callbackLock(
            record.callback_mutex);
        AdvanceCallbackFrameLocked(&record, frameIndex);
    }
}

uint32_t CurrentThreadRole() {
    if (g_thread_role != WOTBMOD_V3_THREAD_UNKNOWN) return g_thread_role;
    return GetCurrentThreadId() == g_main_thread_id
        ? WOTBMOD_V3_THREAD_MAIN
        : WOTBMOD_V3_THREAD_WORKER;
}

uint32_t SetCurrentThreadRole(uint32_t role) {
    const uint32_t previous = g_thread_role;
    g_thread_role = role;
    return previous;
}

const char* GameDirectory() {
    return g_game_directory.c_str();
}

const char* ModsDirectory() {
    return g_mods_directory.c_str();
}

const char* CacheDirectory() {
    return g_cache_directory.c_str();
}

const char* ConfigDirectory() {
    return g_config_directory.c_str();
}

WotbModV3Result GetModDataDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    return CopyOutString(
        mod,
        record->data_directory.c_str(),
        buffer,
        inout_size);
}

WotbModV3Result GetModCacheDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    return CopyOutString(
        mod,
        record->cache_directory.c_str(),
        buffer,
        inout_size);
}

WotbModV3Result GetModConfigDirectory(
    WotbModV3Handle mod,
    char* buffer,
    uint32_t* inout_size) {
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    return CopyOutString(
        mod,
        record->config_directory.c_str(),
        buffer,
        inout_size);
}

void RuntimeLog(
    uint32_t level,
    const char* category,
    const char* message) {
    if (g_log_sink) {
        g_log_sink(
            level,
            category ? category : "runtime",
            message ? message : "",
            g_log_user_data);
        return;
    }
    char line[2048] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[wotbmod.v3][%s] %s\n",
        category ? category : "runtime",
        message ? message : "");
    OutputDebugStringA(line);
}

WotbModV3Result RegisterFramePump(FramePumpFn pump) {
    if (!pump) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (g_frame_pumps.size() >= kMaxFramePumps) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (std::find(g_frame_pumps.begin(), g_frame_pumps.end(), pump) !=
        g_frame_pumps.end()) {
        return WOTBMOD_V3_E_ALREADY_EXISTS;
    }
    g_frame_pumps.push_back(pump);
    return WOTBMOD_V3_OK;
}

WotbModV3Result RegisterShutdownHook(ShutdownHookFn hook) {
    if (!hook) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (g_shutdown_hooks.size() >= kMaxShutdownHooks) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (std::find(g_shutdown_hooks.begin(), g_shutdown_hooks.end(), hook) !=
        g_shutdown_hooks.end()) {
        return WOTBMOD_V3_E_ALREADY_EXISTS;
    }
    g_shutdown_hooks.push_back(hook);
    return WOTBMOD_V3_OK;
}

WotbModV3Result RegisterOwnerStoppingHook(OwnerStoppingHookFn hook) {
    if (!hook) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (g_owner_stopping_hooks.size() >= kMaxOwnerStoppingHooks) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (std::find(
            g_owner_stopping_hooks.begin(),
            g_owner_stopping_hooks.end(),
            hook) != g_owner_stopping_hooks.end()) {
        return WOTBMOD_V3_E_ALREADY_EXISTS;
    }
    g_owner_stopping_hooks.push_back(hook);
    return WOTBMOD_V3_OK;
}

WotbModV3Result RegisterLifecycleStateHook(LifecycleStateHookFn hook) {
    if (!hook) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (g_lifecycle_state_hooks.size() >= kMaxLifecycleStateHooks) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    if (std::find(
            g_lifecycle_state_hooks.begin(),
            g_lifecycle_state_hooks.end(),
            hook) != g_lifecycle_state_hooks.end()) {
        return WOTBMOD_V3_E_ALREADY_EXISTS;
    }
    g_lifecycle_state_hooks.push_back(hook);
    return WOTBMOD_V3_OK;
}

void GetRuntimeStats(RuntimeStats* out_stats) {
    if (!out_stats) return;
    RuntimeStats value = {};
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    for (const ModRecord& record : g_mods) {
        if (!record.active) continue;
        ++value.loaded_mods;
        if (record.enabled) ++value.enabled_mods;
    }
    for (uint32_t slot = 1u; slot < kMaxHandles; ++slot) {
        if (g_handles[slot].alive) ++value.live_handles;
    }
    value.registered_interfaces =
        static_cast<uint32_t>(g_interfaces.size());
    value.registered_capabilities =
        static_cast<uint32_t>(g_capabilities.size());
    value.frame_index = CurrentFrameIndex();
    value.context_mask = CurrentContext();
    *out_stats = value;
}

void SetEventSourceMask(uint64_t source_mask) {
    g_event_source_mask.store(
        source_mask & WOTBMOD_V3_EVENT_SOURCE_ALL,
        std::memory_order_release);
}

uint64_t GetEventSourceMask() {
    return g_event_source_mask.load(std::memory_order_acquire);
}

void RegisterCoreServices() {
    RegisterInterface({
        WOTBMOD_V3_IFACE_CORE,
        WOTBMOD_V3_CORE_VERSION,
        &kCoreApi,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr});
    RegisterInterface({
        WOTBMOD_V3_IFACE_CAPABILITIES,
        WOTBMOD_V3_CAPABILITIES_VERSION,
        &kCapabilitiesApi,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr});
    RegisterInterface({
        WOTBMOD_V3_IFACE_PERMISSIONS,
        WOTBMOD_V3_PERMISSIONS_VERSION,
        &kPermissionsApi,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr});
    RegisterInterface({
        WOTBMOD_V3_IFACE_HANDLES,
        WOTBMOD_V3_HANDLES_VERSION,
        &kHandlesApi,
        WOTBMOD_V3_PERMISSION_SAFE,
        WOTBMOD_V3_CONTEXT_ALL,
        nullptr});
}

}  // namespace v3
}  // namespace wotbmod

extern "C" WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_Initialize(
    const WotbModV3RuntimeOptions* options) {
    using namespace wotbmod::v3;
    const size_t minimum_options_size =
        offsetof(WotbModV3RuntimeOptions, log_user_data) +
        sizeof(options->log_user_data);
    if (!options ||
        options->struct_size < minimum_options_size ||
        !options->game_directory) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    if (g_initialized) return WOTBMOD_V3_E_ALREADY_EXISTS;

    g_game_directory = NormalizeDirectory(options->game_directory);
    g_mods_directory = NormalizeDirectory(
        options->mods_directory
            ? options->mods_directory
            : JoinPath(g_game_directory, "mods").c_str());
    g_cache_directory = NormalizeDirectory(
        options->cache_directory
            ? options->cache_directory
            : JoinPath(g_mods_directory, "cache").c_str());
    g_config_directory = NormalizeDirectory(
        options->config_directory
            ? options->config_directory
            : JoinPath(g_mods_directory, "config").c_str());
    if (g_game_directory.empty() || g_mods_directory.empty() ||
        g_cache_directory.empty() || g_config_directory.empty() ||
        !EnsureDirectoryTree(g_mods_directory) ||
        !EnsureDirectoryTree(g_cache_directory) ||
        !EnsureDirectoryTree(g_config_directory)) {
        return WOTBMOD_V3_E_IO;
    }
    g_client_version = options->client_version
        ? options->client_version
        : "";
    g_executable_sha256 = options->executable_sha256
        ? options->executable_sha256
        : "";
    g_binding_pack_version = options->binding_pack_version;
    g_process_architecture = options->process_architecture;
    g_log_sink = options->log_sink;
    g_log_user_data = options->log_user_data;
    g_main_thread_id = GetCurrentThreadId();
    g_thread_role = WOTBMOD_V3_THREAD_MAIN;
    g_context.store(WOTBMOD_V3_CONTEXT_NONE, std::memory_order_release);
    g_frame_index.store(0u, std::memory_order_release);
    SetEventSourceMask(0u);
    g_interfaces.clear();
    g_capabilities.clear();
    g_frame_pumps.clear();
    g_shutdown_hooks.clear();
    g_owner_stopping_hooks.clear();
    g_lifecycle_state_hooks.clear();
    for (HandleRecord& record : g_handles) {
        record.alive = false;
        record.generation = NextGeneration(record.generation);
        record.type = WOTBMOD_V3_HANDLE_UNKNOWN;
        record.references = 0u;
        record.owner = WOTBMOD_V3_INVALID_HANDLE;
        record.object = nullptr;
        record.destroy = nullptr;
    }
    for (ModRecord& record : g_mods) {
        record.active = false;
        record.enabled = false;
        record.accepting_callbacks = false;
        record.enable_transition_depth = 0u;
        record.enable_transition_thread = std::thread::id();
        record.named_permissions_restricted = false;
        record.permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
        record.state = MOD_STATE_CREATED;
        record.callbacks_in_flight = 0u;
        record.callback_budget_microseconds =
            kDefaultCallbackBudgetMicroseconds;
        record.max_callbacks_per_frame =
            kDefaultCallbacksPerFrame;
        ResetCallbackProfileLocked(&record);
        record.handle = WOTBMOD_V3_INVALID_HANDLE;
        record.module_path.clear();
        record.install_directory.clear();
        record.resource_directory.clear();
        record.id.clear();
        record.name.clear();
        record.version.clear();
        record.author.clear();
        record.description.clear();
        record.data_directory.clear();
        record.cache_directory.clear();
        record.config_directory.clear();
        record.dependencies.clear();
        record.named_permission_grants.clear();
        ZeroMemory(&record.callbacks, sizeof(record.callbacks));
    }

    g_bootstrap.struct_size = sizeof(g_bootstrap);
    g_bootstrap.api_version = WOTBMOD_V3_ABI_VERSION;
    g_bootstrap.sdk_version = WOTBMOD_V3_SDK_VERSION;
    g_bootstrap.bootstrap_version = WOTBMOD_V3_BOOTSTRAP_VERSION;
    g_bootstrap.query_interface = QueryInterfaceImpl;
    g_bootstrap.get_interface_info = GetInterfaceInfoImpl;
    g_bootstrap.get_last_error = GetLastErrorImpl;
    g_bootstrap.get_client_info = GetClientInfoImpl;
    g_initialized = true;

    RegisterCoreServices();
    const size_t native_hook_backend_end =
        offsetof(
            WotbModV3RuntimeOptions,
            native_hook_backend) +
        sizeof(options->native_hook_backend);
    SetNativeHookBackend(
        options->struct_size >= native_hook_backend_end
            ? options->native_hook_backend
            : nullptr);
    RegisterRuntimeServices();
    RegisterDataServices();
    RegisterClientServices();
    RegisterToolingServices();
    RuntimeLog(
        WOTBMOD_V3_LOG_INFO,
        "runtime",
        "V3 modular runtime initialized");
    return WOTBMOD_V3_OK;
}

extern "C" void WOTBMOD_V3_CALL WotbModV3Runtime_Shutdown(void) {
    using namespace wotbmod::v3;
    std::vector<WotbModV3Handle> mods;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        if (!g_initialized) return;
        for (ModRecord& record : g_mods) {
            if (record.active) mods.push_back(record.handle);
        }
    }
    for (WotbModV3Handle mod : mods) {
        WotbModV3Runtime_DestroyMod(mod);
    }
    std::vector<ShutdownHookFn> hooks;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        hooks = g_shutdown_hooks;
    }
    for (auto iterator = hooks.rbegin(); iterator != hooks.rend(); ++iterator) {
        if (*iterator) (*iterator)();
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        g_interfaces.clear();
        g_capabilities.clear();
        g_frame_pumps.clear();
        g_shutdown_hooks.clear();
        g_owner_stopping_hooks.clear();
        g_lifecycle_state_hooks.clear();
        SetEventSourceMask(0u);
        g_initialized = false;
        g_log_sink = nullptr;
        g_log_user_data = nullptr;
        ZeroMemory(&g_bootstrap, sizeof(g_bootstrap));
    }
}

extern "C" const WotbModV3Bootstrap* WOTBMOD_V3_CALL
WotbModV3Runtime_GetBootstrap(void) {
    using namespace wotbmod::v3;
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    return g_initialized ? &g_bootstrap : nullptr;
}

extern "C" void WOTBMOD_V3_CALL
WotbModV3Runtime_SetEventSourceMask(uint64_t source_mask) {
    wotbmod::v3::SetEventSourceMask(source_mask);
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_CreateMod(
    const char* module_path,
    uint32_t granted_permission_tier,
    WotbModV3Handle* out_mod) {
    using namespace wotbmod::v3;
    if (!HasTerminatedText(
            module_path, WOTBMOD_V3_MAX_PATH) ||
        !out_mod ||
        granted_permission_tier > WOTBMOD_V3_PERMISSION_UNSAFE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_mod = WOTBMOD_V3_INVALID_HANDLE;
    std::unique_lock<std::recursive_mutex> lock(g_state_mutex);
    if (!g_initialized) return WOTBMOD_V3_E_NOT_SUPPORTED;
    ModRecord* available = nullptr;
    for (ModRecord& record : g_mods) {
        if (!record.active) {
            available = &record;
            break;
        }
    }
    if (!available) return WOTBMOD_V3_E_LIMIT_REACHED;
    available->active = true;
    available->enabled = false;
    available->accepting_callbacks = false;
    // A recycled slot must never inherit a previous mod's open window.
    available->enable_transition_depth = 0u;
    available->enable_transition_thread = std::thread::id();
    available->named_permissions_restricted = false;
    available->permission_tier = granted_permission_tier;
    available->state = MOD_STATE_CREATED;
    available->callbacks_in_flight = 0u;
    available->callback_budget_microseconds =
        kDefaultCallbackBudgetMicroseconds;
    available->max_callbacks_per_frame =
        kDefaultCallbacksPerFrame;
    ResetCallbackProfileLocked(available);
    available->module_path = NormalizeDirectory(module_path);
    if (available->module_path.empty()) {
        available->module_path = module_path;
    }
    available->install_directory =
        ParentDirectory(available->module_path);
    if (available->install_directory.empty()) {
        available->active = false;
        available->module_path.clear();
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    available->resource_directory = available->install_directory;
    available->id.clear();
    available->name.clear();
    available->version.clear();
    available->author.clear();
    available->description.clear();
    available->dependencies.clear();
    available->named_permission_grants.clear();
    ZeroMemory(&available->callbacks, sizeof(available->callbacks));
    const std::string stem = FileStem(module_path);
    available->data_directory = JoinPath(
        JoinPath(g_mods_directory, "data"),
        stem);
    available->cache_directory = JoinPath(g_cache_directory, stem);
    available->config_directory = JoinPath(g_config_directory, stem);
    if (!EnsureDirectoryTree(available->data_directory) ||
        !EnsureDirectoryTree(available->cache_directory) ||
        !EnsureDirectoryTree(available->config_directory)) {
        available->active = false;
        return WOTBMOD_V3_E_IO;
    }
    WotbModV3Handle handle = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Result result = CreateOwnedHandle(
        WOTBMOD_V3_INVALID_HANDLE,
        WOTBMOD_V3_HANDLE_MOD,
        available,
        nullptr,
        &handle);
    if (result != WOTBMOD_V3_OK) {
        available->active = false;
        return result;
    }
    available->handle = handle;
    *out_mod = handle;
    lock.unlock();
    NotifyLifecycleTransition(
        handle,
        LIFECYCLE_TRANSITION_PRELOAD);
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_InvokeEntry(
    WotbModV3Handle mod,
    WotbModLoadV3Fn entry,
    WotbModV3RuntimeModuleInfo* out_info) {
    using namespace wotbmod::v3;
    if (!entry || !out_info ||
        out_info->struct_size < sizeof(WotbModV3StructHeader)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "entry point and module info output are required");
    }
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
        if (!record || record->state != MOD_STATE_CREATED) {
            return SetError(
                mod,
                WOTBMOD_V3_E_CONFLICT,
                "mod entry has already been invoked or handle is invalid");
        }
    }
    WotbModV3Info info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_ABI_VERSION;
    WotbModV3Result entry_result = WOTBMOD_V3_E_CALLBACK_FAULT;
    bool call_ok = false;
    try {
        call_ok = CallEntryProtected(
            entry,
            &g_bootstrap,
            mod,
            &info,
            &entry_result);
    } catch (...) {
        call_ok = false;
    }
    if (!call_ok) {
        record->state = MOD_STATE_FAULTED;
        return SetError(
            mod,
            WOTBMOD_V3_E_CALLBACK_FAULT,
            "WotbModLoadV3 raised an exception");
    }
    if (entry_result != WOTBMOD_V3_OK) {
        return SetError(
            mod,
            entry_result,
            "WotbModLoadV3 rejected loading");
    }
    const size_t required_size =
        offsetof(WotbModV3Info, on_frame) + sizeof(info.on_frame);
    if (info.struct_size < required_size ||
        info.api_version != WOTBMOD_V3_ABI_VERSION ||
        !IsValidId(info.id) || !info.name[0] || !info.version[0] ||
        info.requested_permission_tier > record->permission_tier) {
        return SetError(
            mod,
            info.requested_permission_tier > record->permission_tier
                ? WOTBMOD_V3_E_PERMISSION_DENIED
                : WOTBMOD_V3_E_INCOMPATIBLE,
            "V3 mod descriptor, identity, ABI, or permission grant is invalid");
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        if (IsDuplicateIdUnlocked(info.id, record)) {
            return SetError(
                mod,
                WOTBMOD_V3_E_ALREADY_EXISTS,
                "another loaded V3 mod uses the same id");
        }
        record->id = info.id;
        record->name = info.name;
        record->version = info.version;
        record->author = info.author;
        record->description = info.description;
        record->callbacks = info;
        record->state = MOD_STATE_LOADED;
        FillModuleInfo(*record, out_info);
    }
    NotifyLifecycleTransition(
        mod,
        LIFECYCLE_TRANSITION_LOADED);
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_Enable(
    WotbModV3Handle mod) {
    using namespace wotbmod::v3;
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
        if (!record) {
            return SetError(
                mod,
                WOTBMOD_V3_E_INVALID_HANDLE,
                "mod handle is invalid");
        }
        if (record->enabled) return WOTBMOD_V3_OK;
        if (record->state != MOD_STATE_LOADED &&
            record->state != MOD_STATE_DISABLED) {
            return SetError(
                mod,
                WOTBMOD_V3_E_CONFLICT,
                "mod is not in an enableable state");
        }
        record->accepting_callbacks = true;
    }
    bool callback_ok = true;
    if (record->callbacks.on_enable) {
        {
            std::lock_guard<std::mutex> lock(record->callback_mutex);
            ++record->callbacks_in_flight;
        }
        const WotbModV3Handle previous_mod = g_callback_mod;
        const uint32_t previous_nesting = g_callback_nesting;
        g_callback_mod = mod;
        g_callback_nesting = previous_nesting + 1u;
        try {
            callback_ok = CallLifecycleProtected(
                record->callbacks.on_enable,
                &g_bootstrap,
                mod);
        } catch (...) {
            callback_ok = false;
        }
        g_callback_mod = previous_mod;
        g_callback_nesting = previous_nesting;
        {
            std::lock_guard<std::mutex> lock(record->callback_mutex);
            --record->callbacks_in_flight;
            record->callback_cv.notify_all();
        }
    }
    if (!callback_ok) {
        record->accepting_callbacks = false;
        record->state = MOD_STATE_FAULTED;
        ReleaseAllOwnedHandles(mod);
        return SetError(
            mod,
            WOTBMOD_V3_E_CALLBACK_FAULT,
            "mod on_enable callback raised an exception");
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record->enabled = true;
        record->state = MOD_STATE_ENABLED;
    }
    NotifyLifecycleTransition(
        mod,
        LIFECYCLE_TRANSITION_ENABLED);
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_Disable(
    WotbModV3Handle mod) {
    using namespace wotbmod::v3;
    ModRecord* record = nullptr;
    bool already_disabled = false;
    bool transitioned_to_disabled = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
        if (!record) {
            return SetError(
                mod,
                WOTBMOD_V3_E_INVALID_HANDLE,
                "mod handle is invalid");
        }
        if (!record->enabled) {
            if (record->state == MOD_STATE_LOADED) {
                record->state = MOD_STATE_DISABLED;
                transitioned_to_disabled = true;
            }
            already_disabled = true;
        }
    }
    if (already_disabled) {
        ReleaseAllOwnedHandles(mod);
        if (transitioned_to_disabled) {
            NotifyLifecycleTransition(
                mod,
                LIFECYCLE_TRANSITION_DISABLED);
        }
        return WOTBMOD_V3_OK;
    }
    if (g_callback_mod == mod && g_callback_nesting > 0u) {
        return SetError(
            mod,
            WOTBMOD_V3_E_BUSY,
            "a mod cannot synchronously disable itself from its callback");
    }
    std::vector<OwnerStoppingHookFn> stopping_hooks;
    {
        std::lock_guard<std::recursive_mutex> state_lock(g_state_mutex);
        stopping_hooks = g_owner_stopping_hooks;
    }
    {
        std::lock_guard<std::mutex> lock(record->callback_mutex);
        record->accepting_callbacks = false;
    }
    for (OwnerStoppingHookFn hook : stopping_hooks) {
        if (hook) hook(mod);
    }
    {
        std::unique_lock<std::mutex> lock(record->callback_mutex);
        record->callback_cv.wait(
            lock,
            [record]() { return record->callbacks_in_flight == 0u; });
    }
    bool callback_ok = true;
    if (record->callbacks.on_disable) {
        try {
            callback_ok = CallLifecycleProtected(
                record->callbacks.on_disable,
                &g_bootstrap,
                mod);
        } catch (...) {
            callback_ok = false;
        }
    }
    ReleaseAllOwnedHandles(mod);
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record->enabled = false;
        record->state = callback_ok
            ? MOD_STATE_DISABLED
            : MOD_STATE_FAULTED;
    }
    NotifyLifecycleTransition(
        mod,
        LIFECYCLE_TRANSITION_DISABLED);
    return callback_ok
        ? WOTBMOD_V3_OK
        : SetError(
            mod,
            WOTBMOD_V3_E_CALLBACK_FAULT,
            "mod on_disable callback raised an exception");
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_DestroyMod(
    WotbModV3Handle mod) {
    using namespace wotbmod::v3;
    ModRecord* record = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
        if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    const WotbModV3Result disable_result = WotbModV3Runtime_Disable(mod);
    if (disable_result == WOTBMOD_V3_E_BUSY) return disable_result;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        record = FindModUnlocked(mod);
        if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
        record->state = MOD_STATE_UNLOADING;
    }
    NotifyLifecycleTransition(
        mod,
        LIFECYCLE_TRANSITION_UNLOADING);
    bool unload_ok = true;
    if (record->callbacks.on_unload) {
        try {
            unload_ok = CallLifecycleProtected(
                record->callbacks.on_unload,
                &g_bootstrap,
                mod);
        } catch (...) {
            unload_ok = false;
        }
    }
    ReleaseAllOwnedHandles(mod);
    NotifyLifecycleTransition(
        mod,
        LIFECYCLE_TRANSITION_UNLOADED);
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        uint32_t slot = 0u;
        if (DecodeHandle(mod, &slot, nullptr, nullptr)) {
            HandleRecord& handle = g_handles[slot];
            handle.alive = false;
            handle.type = WOTBMOD_V3_HANDLE_UNKNOWN;
            handle.references = 0u;
            handle.owner = WOTBMOD_V3_INVALID_HANDLE;
            handle.object = nullptr;
            handle.destroy = nullptr;
            handle.generation = NextGeneration(handle.generation);
        }
        record->active = false;
        record->enabled = false;
        record->accepting_callbacks = false;
        /*
         * Same reason CreateMod clears these on a recycled slot, reached
         * from the other end. If another thread still has an enable window
         * open on this record, its EndModEnableTransition will fail
         * FindModUnlocked against the generation bumped just above and
         * return without decrementing, so nothing else will ever clear
         * them. Leaving {depth=1, thread=that thread} on a dead slot hands
         * CreateOwnedHandle's enabling exemption to whoever recycles it.
         * Clearing here and clearing in CreateMod are both needed: this one
         * closes the window, that one refuses to inherit one.
         */
        record->enable_transition_depth = 0u;
        record->enable_transition_thread = std::thread::id();
        record->named_permissions_restricted = false;
        record->handle = WOTBMOD_V3_INVALID_HANDLE;
        record->id.clear();
        record->name.clear();
        record->version.clear();
        record->author.clear();
        record->description.clear();
        record->module_path.clear();
        record->install_directory.clear();
        record->resource_directory.clear();
        record->data_directory.clear();
        record->cache_directory.clear();
        record->config_directory.clear();
        record->dependencies.clear();
        record->named_permission_grants.clear();
        record->callbacks = {};
        ResetCallbackProfileLocked(record);
    }
    return unload_ok
        ? WOTBMOD_V3_OK
        : WOTBMOD_V3_E_CALLBACK_FAULT;
}

extern "C" void WOTBMOD_V3_CALL WotbModV3Runtime_DispatchFrame(
    uint64_t frame_index,
    double delta_seconds) {
    using namespace wotbmod::v3;
    if (!g_initialized) return;
    g_frame_index.store(frame_index, std::memory_order_release);
    const uint32_t previous_role =
        SetCurrentThreadRole(WOTBMOD_V3_THREAD_RENDER);
    std::vector<FramePumpFn> pumps;
    std::vector<WotbModV3Handle> mods;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        pumps = g_frame_pumps;
        for (ModRecord& record : g_mods) {
            if (record.active) {
                std::lock_guard<std::mutex> callback_lock(
                    record.callback_mutex);
                AdvanceCallbackFrameLocked(&record, frame_index);
            }
            if (record.active && record.enabled &&
                record.callbacks.on_frame) {
                mods.push_back(record.handle);
            }
        }
    }
    for (FramePumpFn pump : pumps) {
        if (!CallFramePumpProtected(pump, frame_index, delta_seconds)) {
            RuntimeLog(
                WOTBMOD_V3_LOG_ERROR,
                "runtime",
                "a service frame pump raised an exception");
        }
    }
    for (WotbModV3Handle mod : mods) {
        if (EnterModCallbackCoalescible(mod) != WOTBMOD_V3_OK) {
            continue;
        }
        ModRecord* record = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
            record = FindModUnlocked(mod);
        }
        bool callback_ok = record && CallFrameProtected(
            record->callbacks.on_frame,
            &g_bootstrap,
            mod,
            frame_index,
            delta_seconds);
        LeaveModCallback(mod);
        if (!callback_ok) {
            RuntimeLog(
                WOTBMOD_V3_LOG_ERROR,
                "runtime",
                "a mod frame callback raised an exception and was disabled");
            WotbModV3Runtime_Disable(mod);
        }
    }
    SetCurrentThreadRole(previous_role);
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL WotbModV3Runtime_SetContext(
    uint64_t context_mask) {
    using namespace wotbmod::v3;
    if ((context_mask & ~static_cast<uint64_t>(
        WOTBMOD_V3_CONTEXT_ALL)) != 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!g_initialized) return WOTBMOD_V3_E_NOT_SUPPORTED;
    g_context.store(context_mask, std::memory_order_release);
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL
WotbModV3Runtime_SetCapability(
    const WotbModV3CapabilityInfo* capability) {
    using namespace wotbmod::v3;
    if (!capability ||
        capability->struct_size < sizeof(WotbModV3CapabilityInfo) ||
        !capability->name[0] ||
        capability->status > WOTBMOD_V3_CAPABILITY_DEGRADED ||
        capability->permission_tier > WOTBMOD_V3_PERMISSION_UNSAFE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::vector<std::pair<WotbModV3Handle, CapabilitySubscription*>>
        subscriptions;
    WotbModV3CapabilityInfo published = {};
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        if (!g_initialized) return WOTBMOD_V3_E_NOT_SUPPORTED;
        CapabilityRecord* record =
            FindCapabilityUnlocked(capability->name);
        if (!record) {
            if (g_capabilities.size() >= kMaxCapabilities) {
                return WOTBMOD_V3_E_LIMIT_REACHED;
            }
            CapabilityRecord created;
            created.info = *capability;
            created.info.struct_size = sizeof(created.info);
            created.info.api_version = WOTBMOD_V3_CAPABILITIES_VERSION;
            created.interface_derived = false;
            g_capabilities.push_back(created);
            record = &g_capabilities.back();
        } else {
            record->info = *capability;
            record->info.struct_size = sizeof(record->info);
            record->info.api_version = WOTBMOD_V3_CAPABILITIES_VERSION;
            record->interface_derived = false;
        }
        published = record->info;
        for (uint32_t slot = 1u; slot < kMaxHandles; ++slot) {
            HandleRecord& handle = g_handles[slot];
            if (!handle.alive ||
                handle.type !=
                    WOTBMOD_V3_HANDLE_CAPABILITY_SUBSCRIPTION ||
                !handle.object) {
                continue;
            }
            CapabilitySubscription* subscription =
                static_cast<CapabilitySubscription*>(handle.object);
            if (!subscription->callback) continue;
            subscriptions.push_back(
                std::make_pair(handle.owner, subscription));
        }
    }
    for (const auto& entry : subscriptions) {
        if (EnterModCallback(entry.first) != WOTBMOD_V3_OK) continue;
        const bool ok = CallCapabilityProtected(
            entry.second->callback,
            entry.first,
            &published,
            entry.second->user_data);
        LeaveModCallback(entry.first);
        if (!ok) {
            RuntimeLog(
                WOTBMOD_V3_LOG_ERROR,
                "capabilities",
                "a capability subscriber raised an exception");
        }
    }
    // Direct capabilities.subscribe callbacks were served above; mods that
    // watch the event bus instead get the same information here. Posted
    // outside the state lock, like those callbacks, so a subscriber cannot
    // deadlock the runtime by calling back into it.
    PublishSystemEvent(
        WOTBMOD_V3_EVENT_CAPABILITIES_CHANGED,
        &published,
        static_cast<uint32_t>(sizeof(published)),
        WOTBMOD_V3_EVENT_FLAG_NONE);
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL
WotbModV3Runtime_SetPermissionTier(
    WotbModV3Handle mod,
    uint32_t granted_permission_tier) {
    using namespace wotbmod::v3;
    if (granted_permission_tier > WOTBMOD_V3_PERMISSION_UNSAFE) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "permission tier is invalid");
    }
    uint32_t previous = WOTBMOD_V3_PERMISSION_SAFE;
    {
        std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
        ModRecord* record = FindModUnlocked(mod);
        if (!record) {
            return SetError(
                mod,
                WOTBMOD_V3_E_INVALID_HANDLE,
                "mod handle is invalid");
        }
        previous = record->permission_tier;
        record->permission_tier = granted_permission_tier;
    }
    if (granted_permission_tier < previous) {
        ReleaseAllOwnedHandles(mod);
    }
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL
WotbModV3Runtime_SetPermissionGrants(
    WotbModV3Handle mod,
    const char* const* permission_names,
    uint32_t permission_count,
    uint32_t restrict_to_names) {
    using namespace wotbmod::v3;
    if (restrict_to_names > 1u ||
        permission_count >
            WOTBMOD_V3_RUNTIME_MAX_PERMISSION_GRANTS ||
        (permission_count != 0u && !permission_names)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "permission grant list or restriction flag is invalid");
    }

    std::vector<std::string> grants;
    if (restrict_to_names != 0u) {
        grants.reserve(permission_count);
    }
    for (uint32_t index = 0u;
         index < permission_count;
         ++index) {
        const char* name = permission_names[index];
        if (!IsValidPermissionGrantName(name)) {
            return SetError(
                mod,
                WOTBMOD_V3_E_INVALID_ARGUMENT,
                "permission grant name is invalid or too long");
        }
        if (restrict_to_names == 0u) continue;
        grants.push_back(NormalizePermissionGrantName(name));
    }
    std::sort(grants.begin(), grants.end());
    grants.erase(
        std::unique(grants.begin(), grants.end()),
        grants.end());

    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    if (record->state != MOD_STATE_CREATED) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "permission grants must be installed before mod entry");
    }
    record->named_permissions_restricted =
        restrict_to_names != 0u;
    record->named_permission_grants.swap(grants);
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL
WotbModV3Runtime_SetPackageRoot(
    WotbModV3Handle mod,
    const char* package_root) {
    using namespace wotbmod::v3;
    if (!HasTerminatedText(
            package_root, WOTBMOD_V3_MAX_PATH)) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "package root is empty or exceeds the ABI path limit");
    }
    const std::string normalized =
        NormalizeDirectory(package_root);
    if (normalized.empty() ||
        normalized.size() >= WOTBMOD_V3_MAX_PATH) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "canonical package root exceeds the ABI path limit");
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    if (record->state != MOD_STATE_CREATED) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "package root must be installed before mod entry");
    }
    record->resource_directory = normalized;
    return WOTBMOD_V3_OK;
}

extern "C" WotbModV3Result WOTBMOD_V3_CALL
WotbModV3Runtime_SetDependencies(
    WotbModV3Handle mod,
    const WotbModV3ManifestDependency* dependencies,
    uint32_t dependency_count) {
    using namespace wotbmod::v3;
    if (dependency_count >
        WOTBMOD_V3_RUNTIME_MAX_DEPENDENCIES) {
        return SetError(
            mod,
            WOTBMOD_V3_E_LIMIT_REACHED,
            "manifest dependency count exceeds the runtime limit");
    }
    if (dependency_count != 0u && !dependencies) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_ARGUMENT,
            "dependency descriptors are required");
    }
    std::vector<WotbModV3ManifestDependency> copied;
    copied.reserve(dependency_count);
    for (uint32_t index = 0u;
         index < dependency_count; ++index) {
        const WotbModV3ManifestDependency& dependency =
            dependencies[index];
        if (dependency.struct_size <
                sizeof(WotbModV3ManifestDependency) ||
            dependency.api_version !=
                WOTBMOD_V3_MANIFEST_VERSION ||
            (dependency.kind !=
                 WOTBMOD_V3_DEPENDENCY_REQUIRED &&
             dependency.kind !=
                 WOTBMOD_V3_DEPENDENCY_OPTIONAL &&
             dependency.kind !=
                 WOTBMOD_V3_DEPENDENCY_INCOMPATIBLE) ||
            !HasTerminatedText(
                dependency.id, sizeof(dependency.id)) ||
            !IsValidId(dependency.id) ||
            !HasTerminatedText(
                dependency.version_range,
                sizeof(dependency.version_range))) {
            return SetError(
                mod,
                WOTBMOD_V3_E_INVALID_ARGUMENT,
                "manifest dependency descriptor is invalid");
        }
        for (const WotbModV3ManifestDependency& existing :
             copied) {
            if (_stricmp(existing.id, dependency.id) == 0) {
                return SetError(
                    mod,
                    WOTBMOD_V3_E_CONFLICT,
                    "dependency id appears more than once");
            }
        }
        WotbModV3ManifestDependency item = dependency;
        item.struct_size = sizeof(item);
        item.api_version = WOTBMOD_V3_MANIFEST_VERSION;
        item.reserved = 0u;
        copied.push_back(item);
    }
    std::lock_guard<std::recursive_mutex> lock(g_state_mutex);
    ModRecord* record = FindModUnlocked(mod);
    if (!record) {
        return SetError(
            mod,
            WOTBMOD_V3_E_INVALID_HANDLE,
            "mod handle is invalid");
    }
    if (record->state != MOD_STATE_CREATED) {
        return SetError(
            mod,
            WOTBMOD_V3_E_CONFLICT,
            "dependencies must be installed before mod entry");
    }
    record->dependencies.swap(copied);
    return WOTBMOD_V3_OK;
}
