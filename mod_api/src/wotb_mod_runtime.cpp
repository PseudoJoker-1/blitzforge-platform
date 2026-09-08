#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WOTBMOD_RUNTIME_BUILD
#include "../include/wotb_mod_runtime.h"
#include "../include/wotb_mod_runtime_v3.h"
#include "v3/dava_native_registry.h"
#include "../include/wotbmod/camera_v1.h"
#include "v3/client_services_backend.h"
#include "v3/data_services_backend.h"
#include "v3/package_loader.h"
#include "v3/wotb_mod_v3_internal.h"

namespace {

const uint32_t kModRecordMagic = 0x4D425457u; /* WTBM */
const uint32_t kMaxMods = 128u;
const uint32_t kMaxHooksPerMod = 64u;
const uint32_t kMaxResourceMounts = 256u;
const uint32_t kMaxResourceMountsPerMod = 32u;
const uint32_t kMaxResourcesPerMod = 64u;
const uint32_t kMaxAudioPlaybacksPerMod = 64u;
const uint32_t kMaxSoundEventsPerMod = 64u;
const uint32_t kMaxMainThreadWorkItems = 256u;
const uint32_t kMaxMainThreadWorkItemsPerMod = 64u;
const uint32_t kMaxEventSubscriptionsPerMod = 16u;
const uint32_t kMaxPendingClientEvents = 256u;
const uint32_t kMaxBorrowedEventResources = 2u;
const uint32_t kMaxVehiclesPerMod = 64u;
const uint32_t kMaxBorrowedEventVehicles = 2u;
const uint32_t kMaxVehicleSkinsPerMod = 16u;
const uint32_t kMaxVehicleSkinAssets = 32u;
uint32_t g_v3LocalVehicleId = 0u;

struct HookRecord {
    void* target;
    LONG active;
};

struct ModRecord;

enum MainThreadWorkKind {
    MAIN_THREAD_WORK_CALLBACK = 0,
    MAIN_THREAD_WORK_UI_SET_GEOMETRY = 1,
    MAIN_THREAD_WORK_UI_SET_VISIBLE = 2,
    MAIN_THREAD_WORK_UI_ADD_CHILD = 3,
    MAIN_THREAD_WORK_UI_REMOVE_CHILD = 4,
    MAIN_THREAD_WORK_SCENE_SET_TRANSFORM = 5,
    MAIN_THREAD_WORK_SCENE_ADD_CHILD = 6,
    MAIN_THREAD_WORK_SCENE_REMOVE_CHILD = 7,
    MAIN_THREAD_WORK_UI_SET_INPUT_ENABLED = 8,
    MAIN_THREAD_WORK_UI_SET_DISABLED = 9
};

struct MainThreadWorkItem {
    ModRecord* owner;
    uint64_t sequence;
    MainThreadWorkKind kind;
    WotbModMainThreadCallback callback;
    void* user_data;
    WotbModResourceHandle first_resource;
    WotbModResourceHandle second_resource;
    WotbModUiControlGeometry geometry;
    WotbModSceneTransform transform;
    int32_t int_value;
    int32_t second_int_value;
    LONG active;
};

struct ResourceMountRecord {
    ModRecord* owner;
    WotbModResourceMountId id;
    int32_t priority;
    uint32_t flags;
    LONG active;
    char virtual_root[WOTBMOD_MAX_RESOURCE_PATH];
    char source_directory[WOTBMOD_MAX_RESOURCE_PATH];
};

enum ResourceBackendKind {
    RESOURCE_BACKEND_NONE = 0,
    RESOURCE_BACKEND_GENERIC = 1,
    RESOURCE_BACKEND_AUDIO_FILE = 2
};

enum ResourceOwnershipKind {
    RESOURCE_OWNED = 0,
    RESOURCE_BORROWED_EVENT = 1
};

struct ResourceHandleRecord {
    void* native_resource;
    WotbModResourceType type;
    ResourceBackendKind backend_kind;
    ResourceOwnershipKind ownership_kind;
    uintptr_t public_id;
    LONG active;
};

struct EventSubscriptionRecord {
    WotbModEventSubscriptionId id;
    uint32_t event_mask;
    WotbModClientEventCallback callback;
    void* user_data;
    LONG active;
};

struct PendingClientEvent {
    uint64_t sequence;
    WotbModClientEventType type;
    WotbModResourceType resource_type;
    uint32_t flags;
    uint32_t payload_size;
    uint32_t primary_entity_id;
    uint32_t other_entity_id;
    void* previous_native_resource;
    void* native_resource;
    WotbModClientEventPayload payload;
    LONG active;
};

enum VehicleOwnershipKind {
    VEHICLE_OWNED = 0,
    VEHICLE_BORROWED_EVENT = 1
};

struct VehicleHandleRecord {
    void* token;
    uintptr_t public_id;
    VehicleOwnershipKind ownership_kind;
    LONG active;
};

struct VehicleSkinAssetRecord {
    uint32_t kind;
    char stock_virtual_path[WOTBMOD_MAX_RESOURCE_PATH];
    char replacement_virtual_path[WOTBMOD_MAX_RESOURCE_PATH];
};

struct VehicleSkinHandleRecord {
    uintptr_t public_id;
    uint64_t sequence;
    int32_t priority;
    uint32_t flags;
    uint32_t asset_count;
    VehicleSkinAssetRecord* assets;
    LONG enabled;
    LONG active;
    char skin_id[WOTBMOD_MAX_SKIN_ID];
    char vehicle_name[WOTBMOD_MAX_VEHICLE_NAME];
};

struct AudioPlaybackRecord {
    void* native_playback;
    ResourceHandleRecord* source_resource;
    uintptr_t public_id;
    volatile LONG state;
    LONG active;
};

struct SoundEventRecord {
    void* native_event;
    uintptr_t public_id;
    volatile LONG state;
    LONG active;
    char event_name[WOTBMOD_MAX_SOUND_EVENT_NAME];
};

struct ModCallbacks {
    WotbModEnableCallback on_enable;
    WotbModDisableCallback on_disable;
    WotbModUnloadCallback on_unload;
    WotbModFrameCallback on_frame;
};

struct ModRecord {
    uint32_t magic;
    HMODULE module;
    char module_name[MAX_PATH];
    char module_path[MAX_PATH];
    char data_path[MAX_PATH];
    char config_path[MAX_PATH];
    char settings_key[MAX_PATH];
    WotbModPublicInfo public_info;
    ModCallbacks callbacks;
    uint32_t api_generation;
    WotbModV3Handle v3_handle;
    WotbModV3Handle v3_package_mount;
    WotbModV3Handle v3_content_handle;
    volatile LONG v3_content_apply_result;
    char v3_package_root[MAX_PATH];
    char v3_package_provider[WOTBMOD_V3_MAX_ID];
    char v3_content_descriptor[WOTBMOD_V3_MAX_PATH];
    HookRecord hooks[kMaxHooksPerMod];
    ResourceHandleRecord resources[kMaxResourcesPerMod];
    ResourceHandleRecord borrowed_event_resources[
        kMaxBorrowedEventResources];
    AudioPlaybackRecord audio_playbacks[kMaxAudioPlaybacksPerMod];
    SoundEventRecord sound_events[kMaxSoundEventsPerMod];
    EventSubscriptionRecord event_subscriptions[
        kMaxEventSubscriptionsPerMod];
    VehicleHandleRecord vehicles[kMaxVehiclesPerMod];
    VehicleHandleRecord borrowed_event_vehicles[
        kMaxBorrowedEventVehicles];
    VehicleSkinHandleRecord vehicle_skins[
        kMaxVehicleSkinsPerMod];
    LONG hook_count;
    volatile LONG enabled;
    volatile LONG state;
    volatile LONG fault_count;
    /*
     * Hot-reload plumbing. request_reload() only sets pending_reload; the actual
     * teardown+reload runs from DrainPendingReloads() at the top of the frame
     * pump, where no code of this module is on the stack. is_package records
     * whether the mod was loaded from a signed package (so the reload knows to
     * re-run the package preflight) versus a loose DLL.
     */
    volatile LONG pending_reload;
    volatile LONG is_package;
};

static ModRecord g_mods[kMaxMods] = {};
static volatile LONG g_modCount = 0;

/*
 * Pick the slot a newly loaded mod goes into. A hot reload tears its record down
 * in place, which zeroes the slot and leaves a hole below g_modCount; without
 * reuse the table would only ever grow, and a development session that reloads a
 * mod repeatedly - the entire point of hot reload - would walk into
 * kMaxMods and the mod would silently stop coming back. Cold boot has no holes,
 * so the scan finds nothing and this behaves exactly as the old
 * "slot = g_modCount" did.
 *
 * Returns -1 when the table is genuinely full. *out_appended tells the caller
 * whether it grew the table, because only an append may bump g_modCount.
 */
static LONG AcquireModSlot(bool* out_appended) {
    if (out_appended) *out_appended = false;
    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        if (g_mods[index].magic == 0u) return index;
    }
    if (count < 0 || count >= static_cast<LONG>(kMaxMods)) return -1;
    if (out_appended) *out_appended = true;
    return count;
}
static volatile LONG g_initialized = 0;
static volatile LONG g_loadedAll = 0;
static volatile LONG64 g_frameIndex = 0;
static HMODULE g_gameModule = nullptr;
static char g_gameDirectory[MAX_PATH] = {};
static char g_modsDirectory[MAX_PATH] = {};
static char g_dataDirectory[MAX_PATH] = {};
static char g_configDirectory[MAX_PATH] = {};
static char g_settingsPath[MAX_PATH] = {};
static char g_sessionMarkerPath[MAX_PATH] = {};
static char g_sessionMarkerTempPath[MAX_PATH] = {};
static char g_sessionMarkerToken[80] = {};
static char g_safeModeLastPhase[96] = {};
static char g_safeModeLastMod[MAX_PATH] = {};
static char g_safeModeLastCallbackOwner[WOTBMOD_MAX_ID] = {};
static char g_safeModeLastCallback[96] = {};
static char g_safeModeLastNativeBinding[128] = {};
static char g_safeModeLastEvent[96] = {};
static char g_safeModeLastAsync[96] = {};
static char g_safeModeLastResourceTransaction[128] = {};
static char g_crashHistoryPath[MAX_PATH] = {};
static char g_autoDisabledPath[MAX_PATH] = {};
static char g_autoDisabledMod[WOTBMOD_MAX_ID] = {};
static LARGE_INTEGER g_qpcFrequency = {};
static LARGE_INTEGER g_lastFrameTime = {};
static WotbModRuntimeLogSink g_logSink = nullptr;
static void* g_logUserData = nullptr;
static WotbModRuntimeHookBackend g_hookBackend = {};
static WotbModRuntimeResourceBackend g_resourceBackend = {};
static WotbModRuntimeAudioBackend g_audioBackend = {};
static WotbModRuntimeSoundBackend g_soundBackend = {};
static WotbModRuntimeGameplayBackend g_gameplayBackend = {};
static WotbModRuntimeV3ClientBackend g_v3ClientBackend = {};
static uint32_t g_runtimeOptionFlags = 0u;
static char g_clientBuild[WOTBMOD_V3_MAX_NAME] = {};
static char g_clientExecutableSha256[65] = {};
static ResourceMountRecord g_resourceMounts[kMaxResourceMounts] = {};
static SRWLOCK g_resourceMountLock = SRWLOCK_INIT;
static volatile LONG64 g_nextResourceMountId = 0;
static volatile LONG g_nextResourceHandleId = 0;
static volatile LONG g_nextAudioPlaybackId = 0;
static volatile LONG g_nextSoundEventId = 0;
static volatile LONG g_nextVehicleHandleId = 0;
static volatile LONG g_nextVehicleSkinHandleId = 0;
static volatile LONG64 g_nextVehicleSkinSequence = 0;
static volatile LONG64 g_resourceGeneration = 0;
static MainThreadWorkItem g_mainThreadWork[kMaxMainThreadWorkItems] = {};
static SRWLOCK g_mainThreadWorkLock = SRWLOCK_INIT;
static volatile LONG64 g_nextMainThreadSequence = 0;
static volatile LONG g_dispatchThreadId = 0;
static PendingClientEvent g_pendingClientEvents[
    kMaxPendingClientEvents] = {};
static SRWLOCK g_clientEventLock = SRWLOCK_INIT;
static volatile LONG64 g_nextClientEventSequence = 0;
static volatile LONG64 g_nextEventSubscriptionId = 0;
static SRWLOCK g_vehicleSkinLock = SRWLOCK_INIT;
static volatile LONG g_safeMode = 0;
static volatile LONG g_sessionMarkerOwned = 0;
static volatile LONG g_exitHandlerRegistered = 0;
static volatile LONG g_crashLoopGuardPrepared = 0;
static volatile LONG64 g_sessionMarkerTokenSequence = 0;
static SRWLOCK g_crashLoopGuardLock = SRWLOCK_INIT;
static SRWLOCK g_sessionMarkerIoLock = SRWLOCK_INIT;

extern WotbModHostApi g_hostApi;
static const char* RecordLogId(const ModRecord* record);
static void MarkFaulted(
    ModRecord* record,
    const char* callbackName,
    DWORD exceptionCode);

static WotbModResult V3ResultToLegacy(WotbModV3Result result) {
    switch (result) {
        case WOTBMOD_V3_OK:
            return WOTBMOD_OK;
        case WOTBMOD_V3_E_INVALID_ARGUMENT:
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        case WOTBMOD_V3_E_NOT_FOUND:
        case WOTBMOD_V3_E_INVALID_HANDLE:
        case WOTBMOD_V3_E_OBJECT_DESTROYED:
            return WOTBMOD_ERROR_NOT_FOUND;
        case WOTBMOD_V3_E_ALREADY_EXISTS:
            return WOTBMOD_ERROR_ALREADY_EXISTS;
        case WOTBMOD_V3_E_PERMISSION_DENIED:
            return WOTBMOD_ERROR_ACCESS_DENIED;
        case WOTBMOD_V3_E_BUFFER_TOO_SMALL:
            return WOTBMOD_ERROR_BUFFER_TOO_SMALL;
        case WOTBMOD_V3_E_LIMIT_REACHED:
            return WOTBMOD_ERROR_LIMIT_REACHED;
        case WOTBMOD_V3_E_CALLBACK_FAULT:
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        case WOTBMOD_V3_E_WRONG_THREAD:
            return WOTBMOD_ERROR_WRONG_THREAD;
        case WOTBMOD_V3_E_NOT_SUPPORTED:
        case WOTBMOD_V3_E_CLIENT_MISMATCH:
        case WOTBMOD_V3_E_INCOMPATIBLE:
            return WOTBMOD_ERROR_UNSUPPORTED_ABI;
        default:
            return WOTBMOD_ERROR_PLATFORM;
    }
}

static void CopyString(char* destination, size_t capacity, const char* source) {
    if (!destination || capacity == 0) return;
    if (!source) source = "";
#if defined(_MSC_VER)
    strncpy_s(destination, capacity, source, _TRUNCATE);
#else
    strncpy(destination, source, capacity - 1);
    destination[capacity - 1] = '\0';
#endif
}

static bool JoinPath(
    char* destination,
    size_t capacity,
    const char* left,
    const char* right) {
    if (!destination || capacity == 0 || !left || !right) return false;
    const size_t leftLength = strlen(left);
    const char separator =
        leftLength > 0 && (left[leftLength - 1] == '\\' || left[leftLength - 1] == '/')
            ? '\0'
            : '\\';
    int written = 0;
    if (separator) {
        written = _snprintf_s(
            destination, capacity, _TRUNCATE, "%s\\%s", left, right);
    } else {
        written = _snprintf_s(
            destination, capacity, _TRUNCATE, "%s%s", left, right);
    }
    return written >= 0;
}

static bool NormalizeAbsolutePath(
    const char* input,
    char* output,
    size_t outputCapacity) {
    if (!input || !input[0] || !output || outputCapacity == 0) return false;
    DWORD length = GetFullPathNameA(
        input, (DWORD)outputCapacity, output, nullptr);
    if (length == 0 || length >= outputCapacity) return false;

    size_t current = strlen(output);
    while (current > 3 &&
           (output[current - 1] == '\\' || output[current - 1] == '/')) {
        output[--current] = '\0';
    }
    return true;
}

static bool DeriveExecutableDirectory(char* output, size_t outputCapacity) {
    if (!output || outputCapacity == 0) return false;
    DWORD length = GetModuleFileNameA(nullptr, output, (DWORD)outputCapacity);
    if (length == 0 || length >= outputCapacity) return false;
    char* slash = strrchr(output, '\\');
    if (!slash) slash = strrchr(output, '/');
    if (!slash) return false;
    *slash = '\0';
    return true;
}

static bool EnsureDirectory(const char* path) {
    if (!path || !path[0]) return false;
    if (CreateDirectoryA(path, nullptr)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static const char* LogLevelName(WotbModLogLevel level) {
    switch (level) {
        case WOTBMOD_LOG_TRACE: return "trace";
        case WOTBMOD_LOG_WARNING: return "warning";
        case WOTBMOD_LOG_ERROR: return "error";
        case WOTBMOD_LOG_INFO:
        default: return "info";
    }
}

static void RuntimeLogV(
    WotbModLogLevel level,
    const char* format,
    va_list arguments) {
    char message[1400] = {};
    _vsnprintf_s(message, sizeof(message), _TRUNCATE, format, arguments);
    if (g_logSink) {
        g_logSink(level, message, g_logUserData);
        return;
    }

    char debugLine[1500] = {};
    _snprintf_s(
        debugLine,
        sizeof(debugLine),
        _TRUNCATE,
        "[wotb-mod-api/%s] %s\n",
        LogLevelName(level),
        message);
    OutputDebugStringA(debugLine);
}

static void RuntimeLog(WotbModLogLevel level, const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    RuntimeLogV(level, format, arguments);
    va_end(arguments);
}

static bool EnvironmentFlagEnabled(const char* name) {
    char value[16] = {};
    const DWORD length = GetEnvironmentVariableA(
        name,
        value,
        static_cast<DWORD>(sizeof(value)));
    if (length == 0 || length >= sizeof(value)) return false;
    return strcmp(value, "1") == 0 ||
           _stricmp(value, "true") == 0 ||
           _stricmp(value, "yes") == 0;
}

static void CopySessionMarkerField(
    char* destination,
    size_t capacity,
    const char* source) {
    CopyString(destination, capacity, source && source[0] ? source : "-");
    for (char* current = destination; *current; ++current) {
        const unsigned char value =
            static_cast<unsigned char>(*current);
        if (value < 0x20u || *current == '=') *current = ' ';
    }
}

static bool WriteSessionMarkerFile(
    const char* path,
    DWORD creationDisposition,
    const char* phase,
    const char* mod) {
    if (!path || !path[0]) return false;
    char safePhase[96] = {};
    char safeMod[MAX_PATH] = {};
    char safeCallbackOwner[WOTBMOD_MAX_ID] = {};
    char safeCallback[96] = {};
    char safeNativeBinding[128] = {};
    char safeEvent[96] = {};
    char safeAsync[96] = {};
    char safeResourceTransaction[128] = {};
    CopySessionMarkerField(
        safePhase, sizeof(safePhase), phase);
    CopySessionMarkerField(safeMod, sizeof(safeMod), mod);
    CopySessionMarkerField(
        safeCallbackOwner,
        sizeof(safeCallbackOwner),
        g_safeModeLastCallbackOwner);
    CopySessionMarkerField(
        safeCallback, sizeof(safeCallback), g_safeModeLastCallback);
    CopySessionMarkerField(
        safeNativeBinding,
        sizeof(safeNativeBinding),
        g_safeModeLastNativeBinding);
    CopySessionMarkerField(
        safeEvent, sizeof(safeEvent), g_safeModeLastEvent);
    CopySessionMarkerField(
        safeAsync, sizeof(safeAsync), g_safeModeLastAsync);
    CopySessionMarkerField(
        safeResourceTransaction,
        sizeof(safeResourceTransaction),
        g_safeModeLastResourceTransaction);

    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER timestamp = {};
    timestamp.LowPart = now.dwLowDateTime;
    timestamp.HighPart = now.dwHighDateTime;
    char contents[1536] = {};
    const int length = _snprintf_s(
        contents,
        sizeof(contents),
        _TRUNCATE,
        "[session]\r\n"
        "format=2\r\n"
        "process_id=%lu\r\n"
        "session_token=%s\r\n"
        "updated_filetime=%llu\r\n"
        "phase=%s\r\n"
        "mod=%s\r\n"
        "last_callback_owner=%s\r\n"
        "last_callback=%s\r\n"
        "last_native_binding=%s\r\n"
        "last_event=%s\r\n"
        "last_async_operation=%s\r\n"
        "last_resource_transaction=%s\r\n",
        GetCurrentProcessId(),
        g_sessionMarkerToken,
        static_cast<unsigned long long>(timestamp.QuadPart),
        safePhase,
        safeMod,
        safeCallbackOwner,
        safeCallback,
        safeNativeBinding,
        safeEvent,
        safeAsync,
        safeResourceTransaction);
    if (length < 0) return false;

    HANDLE file = CreateFileA(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        creationDisposition,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL writeOk = WriteFile(
        file,
        contents,
        static_cast<DWORD>(length),
        &written,
        nullptr);
    const BOOL flushOk = writeOk ? FlushFileBuffers(file) : FALSE;
    CloseHandle(file);
    if (!writeOk ||
        written != static_cast<DWORD>(length) ||
        !flushOk) {
        if (creationDisposition == CREATE_NEW) {
            DeleteFileA(path);
        }
        return false;
    }
    return true;
}

static void ReadSessionMarkerValue(
    const char* contents,
    const char* key,
    char* output,
    size_t outputCapacity) {
    if (!output || outputCapacity == 0) return;
    output[0] = '\0';
    if (!contents || !key || !key[0]) return;
    const size_t keyLength = strlen(key);
    const char* cursor = contents;
    while (*cursor) {
        const char* lineEnd = strpbrk(cursor, "\r\n");
        const size_t lineLength =
            lineEnd ? static_cast<size_t>(lineEnd - cursor) : strlen(cursor);
        if (lineLength > keyLength + 1u &&
            _strnicmp(cursor, key, keyLength) == 0 &&
            cursor[keyLength] == '=') {
            const size_t valueLength = lineLength - keyLength - 1u;
            const size_t copyLength =
                valueLength < outputCapacity - 1u
                    ? valueLength
                    : outputCapacity - 1u;
            memcpy(output, cursor + keyLength + 1u, copyLength);
            output[copyLength] = '\0';
            return;
        }
        if (!lineEnd) return;
        cursor = lineEnd + 1;
        if (*lineEnd == '\r' && *cursor == '\n') ++cursor;
    }
}

static void ReadSessionMarkerDetails() {
    CopyString(
        g_safeModeLastPhase,
        sizeof(g_safeModeLastPhase),
        "unknown");
    CopyString(
        g_safeModeLastMod,
        sizeof(g_safeModeLastMod),
        "unknown");
    ZeroMemory(
        g_safeModeLastCallbackOwner,
        sizeof(g_safeModeLastCallbackOwner));
    ZeroMemory(g_safeModeLastCallback, sizeof(g_safeModeLastCallback));
    ZeroMemory(
        g_safeModeLastNativeBinding,
        sizeof(g_safeModeLastNativeBinding));
    ZeroMemory(g_safeModeLastEvent, sizeof(g_safeModeLastEvent));
    ZeroMemory(g_safeModeLastAsync, sizeof(g_safeModeLastAsync));
    ZeroMemory(
        g_safeModeLastResourceTransaction,
        sizeof(g_safeModeLastResourceTransaction));
    HANDLE file = CreateFileA(
        g_sessionMarkerPath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char contents[4096] = {};
    DWORD bytesRead = 0;
    const BOOL readOk = ReadFile(
        file,
        contents,
        static_cast<DWORD>(sizeof(contents) - 1u),
        &bytesRead,
        nullptr);
    CloseHandle(file);
    if (!readOk) return;
    contents[bytesRead] = '\0';
    ReadSessionMarkerValue(
        contents,
        "phase",
        g_safeModeLastPhase,
        sizeof(g_safeModeLastPhase));
    ReadSessionMarkerValue(
        contents,
        "mod",
        g_safeModeLastMod,
        sizeof(g_safeModeLastMod));
    ReadSessionMarkerValue(
        contents,
        "last_callback_owner",
        g_safeModeLastCallbackOwner,
        sizeof(g_safeModeLastCallbackOwner));
    ReadSessionMarkerValue(
        contents,
        "last_callback",
        g_safeModeLastCallback,
        sizeof(g_safeModeLastCallback));
    ReadSessionMarkerValue(
        contents,
        "last_native_binding",
        g_safeModeLastNativeBinding,
        sizeof(g_safeModeLastNativeBinding));
    ReadSessionMarkerValue(
        contents,
        "last_event",
        g_safeModeLastEvent,
        sizeof(g_safeModeLastEvent));
    ReadSessionMarkerValue(
        contents,
        "last_async_operation",
        g_safeModeLastAsync,
        sizeof(g_safeModeLastAsync));
    ReadSessionMarkerValue(
        contents,
        "last_resource_transaction",
        g_safeModeLastResourceTransaction,
        sizeof(g_safeModeLastResourceTransaction));
    if (!g_safeModeLastPhase[0]) {
        CopyString(
            g_safeModeLastPhase,
            sizeof(g_safeModeLastPhase),
            "unknown");
    }
    if (!g_safeModeLastMod[0]) {
        CopyString(
            g_safeModeLastMod,
            sizeof(g_safeModeLastMod),
            "unknown");
    }
}

static void SetCrashAttribution(
    char* field,
    size_t capacity,
    const char* value) {
    CopyString(field, capacity, value && value[0] ? value : "-");
}

static void SetCallbackAttribution(
    const ModRecord* record,
    const char* callback) {
    SetCrashAttribution(
        g_safeModeLastCallbackOwner,
        sizeof(g_safeModeLastCallbackOwner),
        RecordLogId(record));
    SetCrashAttribution(
        g_safeModeLastCallback,
        sizeof(g_safeModeLastCallback),
        callback);
}

static bool ReadAutoDisabledMod() {
    ZeroMemory(g_autoDisabledMod, sizeof(g_autoDisabledMod));
    if (!g_autoDisabledPath[0]) return false;
    GetPrivateProfileStringA(
        "auto_disable",
        "id",
        "",
        g_autoDisabledMod,
        static_cast<DWORD>(sizeof(g_autoDisabledMod)),
        g_autoDisabledPath);
    return g_autoDisabledMod[0] != '\0';
}

static bool IsAutoDisabledMod(const char* id) {
    return id && id[0] && g_autoDisabledMod[0] &&
           _stricmp(id, g_autoDisabledMod) == 0;
}

static void RecordStaleSessionCrash() {
    if (!g_crashHistoryPath[0] || !g_autoDisabledPath[0] ||
        !g_safeModeLastMod[0] ||
        _stricmp(g_safeModeLastMod, "unknown") == 0 ||
        strcmp(g_safeModeLastMod, "-") == 0) {
        return;
    }
    char markerToken[sizeof(g_sessionMarkerToken)] = {};
    char previousToken[sizeof(g_sessionMarkerToken)] = {};
    GetPrivateProfileStringA(
        "session",
        "session_token",
        "",
        markerToken,
        static_cast<DWORD>(sizeof(markerToken)),
        g_sessionMarkerPath);
    GetPrivateProfileStringA(
        "crash_history",
        "last_session_token",
        "",
        previousToken,
        static_cast<DWORD>(sizeof(previousToken)),
        g_crashHistoryPath);
    if (markerToken[0] && previousToken[0] &&
        strcmp(markerToken, previousToken) == 0) {
        return;
    }
    char previous[WOTBMOD_MAX_ID] = {};
    GetPrivateProfileStringA(
        "crash_history",
        "last_mod",
        "",
        previous,
        static_cast<DWORD>(sizeof(previous)),
        g_crashHistoryPath);
    int count = GetPrivateProfileIntA(
        "crash_history", "count", 0, g_crashHistoryPath);
    count = previous[0] &&
            _stricmp(previous, g_safeModeLastMod) == 0
        ? count + 1
        : 1;
    char countText[16] = {};
    _snprintf_s(
        countText, sizeof(countText), _TRUNCATE, "%d", count);
    WritePrivateProfileStringA(
        "crash_history", "last_mod", g_safeModeLastMod,
        g_crashHistoryPath);
    WritePrivateProfileStringA(
        "crash_history", "last_session_token", markerToken,
        g_crashHistoryPath);
    WritePrivateProfileStringA(
        "crash_history", "count", countText, g_crashHistoryPath);
    if (count < 2) return;
    WritePrivateProfileStringA(
        "auto_disable", "id", g_safeModeLastMod,
        g_autoDisabledPath);
    WritePrivateProfileStringA(
        "auto_disable", "reason",
        "repeated_crash_attribution", g_autoDisabledPath);
    CopyString(
        g_autoDisabledMod,
        sizeof(g_autoDisabledMod),
        g_safeModeLastMod);
    RuntimeLog(
        WOTBMOD_LOG_ERROR,
        "SAFE MODE: automatically disabled repeatedly crashing mod %s; "
        "remove %s only after diagnosis to re-enable it",
        g_autoDisabledMod,
        g_autoDisabledPath);
}

static bool ReadSessionMarkerToken(
    const char* path,
    char* output,
    size_t outputCapacity) {
    if (!path || !path[0] || !output || outputCapacity == 0u) {
        return false;
    }
    output[0] = '\0';
    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char contents[4096] = {};
    DWORD bytesRead = 0;
    const BOOL readOk = ReadFile(
        file,
        contents,
        static_cast<DWORD>(sizeof(contents) - 1u),
        &bytesRead,
        nullptr);
    CloseHandle(file);
    if (!readOk) return false;
    contents[bytesRead] = '\0';
    ReadSessionMarkerValue(
        contents,
        "session_token",
        output,
        outputCapacity);
    return output[0] != '\0';
}

static void DeleteOwnedSessionMarker() {
    AcquireSRWLockExclusive(&g_sessionMarkerIoLock);
    const bool ownershipFlagWasSet =
        InterlockedExchange(&g_sessionMarkerOwned, 0) == 1;
    char markerPath[MAX_PATH] = {};
    char markerTempPath[MAX_PATH] = {};
    char sessionToken[sizeof(g_sessionMarkerToken)] = {};
    char markerToken[sizeof(g_sessionMarkerToken)] = {};
    CopyString(markerPath, sizeof(markerPath), g_sessionMarkerPath);
    CopyString(
        markerTempPath,
        sizeof(markerTempPath),
        g_sessionMarkerTempPath);
    CopyString(
        sessionToken,
        sizeof(sessionToken),
        g_sessionMarkerToken);

    bool markerInspected = false;
    bool markerTokenRead = false;
    bool ownershipTokenChanged = false;
    bool recoveredOwnership = false;
    bool deleteAttempted = false;
    BOOL deleteOk = FALSE;
    DWORD inspectError = ERROR_SUCCESS;
    DWORD deleteError = ERROR_SUCCESS;
    if (markerPath[0] && sessionToken[0]) {
        const DWORD attributes = GetFileAttributesA(markerPath);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            inspectError = GetLastError();
        } else {
            markerInspected = true;
            markerTokenRead = ReadSessionMarkerToken(
                markerPath,
                markerToken,
                sizeof(markerToken));
            if (!markerTokenRead ||
                strcmp(markerToken, sessionToken) != 0) {
                ownershipTokenChanged = ownershipFlagWasSet;
            } else {
                recoveredOwnership = !ownershipFlagWasSet;
                deleteAttempted = true;
                deleteOk = DeleteFileA(markerPath);
                deleteError =
                    deleteOk ? ERROR_SUCCESS : GetLastError();
                if (markerTempPath[0]) {
                    DeleteFileA(markerTempPath);
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&g_sessionMarkerIoLock);

    if (!markerPath[0] || !sessionToken[0]) return;
    if (!markerInspected) {
        if (inspectError != ERROR_FILE_NOT_FOUND &&
            inspectError != ERROR_PATH_NOT_FOUND &&
            ownershipFlagWasSet) {
            RuntimeLog(
                WOTBMOD_LOG_WARNING,
                "could not inspect clean session marker %s (win32=%lu)",
                markerPath,
                inspectError);
        }
        return;
    }

    if (ownershipTokenChanged) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "clean shutdown retained session marker because its "
            "ownership token changed");
        return;
    }
    if (!markerTokenRead ||
        strcmp(markerToken, sessionToken) != 0) {
        return;
    }
    if (recoveredOwnership) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "recovering clean session-marker ownership from its token");
    }
    if (deleteAttempted &&
        !deleteOk &&
        deleteError != ERROR_FILE_NOT_FOUND) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "could not remove clean session marker %s (win32=%lu)",
            markerPath,
            deleteError);
    }
}

static void CleanSessionMarkerAtProcessExit() {
    /*
     * This callback intentionally performs only kernel32 file deletion. It is
     * registered with the loader DLL's CRT and therefore runs for an orderly
     * process/DLL teardown, but not for a crash or TerminateProcess.
     */
    if (InterlockedCompareExchange(
            &g_sessionMarkerOwned, 0, 0) != 1) {
        return;
    }
    AcquireSRWLockExclusive(&g_sessionMarkerIoLock);
    if (InterlockedExchange(&g_sessionMarkerOwned, 0) == 1 &&
        g_sessionMarkerPath[0]) {
        DeleteFileA(g_sessionMarkerPath);
    }
    if (g_sessionMarkerTempPath[0]) {
        DeleteFileA(g_sessionMarkerTempPath);
    }
    ReleaseSRWLockExclusive(&g_sessionMarkerIoLock);
}

static void CleanSessionMarkerAtProcessDetach() {
    /*
     * ExitProcess has already terminated the other process threads before
     * DLL_PROCESS_DETACH. Do not acquire an SRW lock here: a terminated thread
     * could have owned it. Keep this path limited to interlocked state and
     * kernel32 file deletion so it remains safe under the loader lock.
     */
    if (InterlockedExchange(&g_sessionMarkerOwned, 0) == 1 &&
        g_sessionMarkerPath[0]) {
        DeleteFileA(g_sessionMarkerPath);
    }
    if (g_sessionMarkerTempPath[0]) {
        DeleteFileA(g_sessionMarkerTempPath);
    }
}

static bool EnsureExitHandlerRegistered() {
    if (InterlockedCompareExchange(
            &g_exitHandlerRegistered, 1, 0) != 0) {
        return true;
    }
    if (atexit(&CleanSessionMarkerAtProcessExit) == 0) return true;
    InterlockedExchange(&g_exitHandlerRegistered, 0);
    return false;
}

static bool GenerateSessionMarkerToken() {
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER timestamp = {};
    timestamp.LowPart = now.dwLowDateTime;
    timestamp.HighPart = now.dwHighDateTime;
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    const LONG64 sequence =
        InterlockedIncrement64(&g_sessionMarkerTokenSequence);
    return _snprintf_s(
               g_sessionMarkerToken,
               sizeof(g_sessionMarkerToken),
               _TRUNCATE,
               "%08lX-%08lX-%016llX-%016llX",
               GetCurrentProcessId(),
               GetCurrentThreadId(),
               static_cast<unsigned long long>(timestamp.QuadPart),
               static_cast<unsigned long long>(
                   counter.QuadPart ^
                   sequence ^
                   static_cast<LONG64>(
                       reinterpret_cast<uintptr_t>(
                           &g_sessionMarkerOwned)))) >= 0;
}

static bool ClaimSessionMarker() {
    AcquireSRWLockExclusive(&g_sessionMarkerIoLock);
    const bool claimed =
        GenerateSessionMarkerToken() &&
        WriteSessionMarkerFile(
            g_sessionMarkerPath,
            CREATE_NEW,
            "runtime_initialize",
            nullptr);
    if (claimed) {
        InterlockedExchange(&g_sessionMarkerOwned, 1);
    }
    ReleaseSRWLockExclusive(&g_sessionMarkerIoLock);
    if (!claimed) return false;
    if (EnsureExitHandlerRegistered()) return true;
    DeleteOwnedSessionMarker();
    return false;
}

static void UpdateSessionMarker(const char* phase, const char* mod) {
    AcquireSRWLockExclusive(&g_sessionMarkerIoLock);
    if (InterlockedCompareExchange(
            &g_sessionMarkerOwned, 0, 0) != 1) {
        ReleaseSRWLockExclusive(&g_sessionMarkerIoLock);
        return;
    }
    /*
     * Keep one marker file identity for the whole session. Replacing it with
     * temp-file renames can leave older file generations visible while a
     * scanner holds shared handles, even after DeleteFile reports success.
     * A crash during this in-place rewrite still leaves a marker (possibly
     * partial), which is sufficient to enter safe mode on the next startup.
     */
    const bool updated =
        WriteSessionMarkerFile(
            g_sessionMarkerPath,
            CREATE_ALWAYS,
            phase,
            mod);
    const DWORD error = updated ? ERROR_SUCCESS : GetLastError();
    ReleaseSRWLockExclusive(&g_sessionMarkerIoLock);

    if (!updated) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "could not update crash-loop session marker "
            "(phase=%s mod=%s win32=%lu)",
            phase ? phase : "-",
            mod ? mod : "-",
            error);
    }
}

static bool PrepareCrashLoopGuard(const char* cacheDirectory) {
    InterlockedExchange(&g_safeMode, 0);
    InterlockedExchange(&g_sessionMarkerOwned, 0);
    ZeroMemory(g_sessionMarkerPath, sizeof(g_sessionMarkerPath));
    ZeroMemory(g_sessionMarkerTempPath, sizeof(g_sessionMarkerTempPath));
    ZeroMemory(g_sessionMarkerToken, sizeof(g_sessionMarkerToken));
    ZeroMemory(g_safeModeLastPhase, sizeof(g_safeModeLastPhase));
    ZeroMemory(g_safeModeLastMod, sizeof(g_safeModeLastMod));
    ZeroMemory(
        g_safeModeLastCallbackOwner,
        sizeof(g_safeModeLastCallbackOwner));
    ZeroMemory(g_safeModeLastCallback, sizeof(g_safeModeLastCallback));
    ZeroMemory(
        g_safeModeLastNativeBinding,
        sizeof(g_safeModeLastNativeBinding));
    ZeroMemory(g_safeModeLastEvent, sizeof(g_safeModeLastEvent));
    ZeroMemory(g_safeModeLastAsync, sizeof(g_safeModeLastAsync));
    ZeroMemory(
        g_safeModeLastResourceTransaction,
        sizeof(g_safeModeLastResourceTransaction));
    ZeroMemory(g_crashHistoryPath, sizeof(g_crashHistoryPath));
    ZeroMemory(g_autoDisabledPath, sizeof(g_autoDisabledPath));
    ZeroMemory(g_autoDisabledMod, sizeof(g_autoDisabledMod));
    if (!JoinPath(
            g_sessionMarkerPath,
            sizeof(g_sessionMarkerPath),
            cacheDirectory,
            "runtime_session.marker") ||
        !JoinPath(
            g_sessionMarkerTempPath,
            sizeof(g_sessionMarkerTempPath),
            cacheDirectory,
            "runtime_session.marker.tmp") ||
        !JoinPath(
            g_crashHistoryPath,
            sizeof(g_crashHistoryPath),
            cacheDirectory,
            "crash_history.ini") ||
        !JoinPath(
            g_autoDisabledPath,
            sizeof(g_autoDisabledPath),
            cacheDirectory,
            "auto_disabled_mod.ini")) {
        return false;
    }
    ReadAutoDisabledMod();

    const DWORD attributes = GetFileAttributesA(g_sessionMarkerPath);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        ReadSessionMarkerDetails();
        RecordStaleSessionCrash();
        if (EnvironmentFlagEnabled("WOTBMOD_SAFE_MODE_OVERRIDE")) {
            if (DeleteFileA(g_sessionMarkerPath)) {
                RuntimeLog(
                    WOTBMOD_LOG_WARNING,
                    "explicit WOTBMOD_SAFE_MODE_OVERRIDE bypassed stale "
                    "session marker (last_phase=%s last_mod=%s)",
                    g_safeModeLastPhase,
                    g_safeModeLastMod);
            } else {
                RuntimeLog(
                    WOTBMOD_LOG_ERROR,
                    "safe-mode override could not remove %s "
                    "(win32=%lu); third-party mods remain blocked",
                    g_sessionMarkerPath,
                    GetLastError());
                InterlockedExchange(&g_safeMode, 1);
            }
        } else {
            InterlockedExchange(
                &g_safeMode,
                EnvironmentFlagEnabled(
                    "WOTBMOD_SAFE_MODE_PORTABLE_ONLY")
                    ? 2
                    : 1);
        }
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND &&
               GetLastError() != ERROR_PATH_NOT_FOUND) {
        return false;
    }

    const LONG safeMode =
        InterlockedCompareExchange(&g_safeMode, 0, 0);
    if (safeMode != 0) {
        RuntimeLog(
            safeMode == 1 ? WOTBMOD_LOG_ERROR : WOTBMOD_LOG_WARNING,
            "SAFE MODE%s: stale session marker detected at %s "
            "(last_phase=%s last_mod=%s last_callback_owner=%s "
            "last_callback=%s last_native_binding=%s last_event=%s "
            "last_async=%s last_resource_transaction=%s); %s. "
            "Exit the game, inspect the log, then "
            "delete this marker or set WOTBMOD_SAFE_MODE_OVERRIDE=1 "
            "for one explicit recovery startup",
            safeMode == 2 ? " PORTABLE-ONLY" : "",
            g_sessionMarkerPath,
            g_safeModeLastPhase[0]
                ? g_safeModeLastPhase
                : "unknown",
            g_safeModeLastMod[0]
                ? g_safeModeLastMod
                : "unknown",
            g_safeModeLastCallbackOwner[0]
                ? g_safeModeLastCallbackOwner
                : "unknown",
            g_safeModeLastCallback[0]
                ? g_safeModeLastCallback
                : "unknown",
            g_safeModeLastNativeBinding[0]
                ? g_safeModeLastNativeBinding
                : "unknown",
            g_safeModeLastEvent[0]
                ? g_safeModeLastEvent
                : "unknown",
            g_safeModeLastAsync[0]
                ? g_safeModeLastAsync
                : "unknown",
            g_safeModeLastResourceTransaction[0]
                ? g_safeModeLastResourceTransaction
                : "unknown",
            safeMode == 2
                ? "only content-only portable packages may load"
                : "third-party mod loading and enable are blocked");
        return true;
    }
    return ClaimSessionMarker();
}

static bool EnsureCrashLoopGuardPrepared(
    const char* cacheDirectory) {
    if (!cacheDirectory || !cacheDirectory[0]) return false;
    char expectedMarkerPath[MAX_PATH] = {};
    if (!JoinPath(
            expectedMarkerPath,
            sizeof(expectedMarkerPath),
            cacheDirectory,
            "runtime_session.marker")) {
        return false;
    }

    AcquireSRWLockExclusive(&g_crashLoopGuardLock);
    if (InterlockedCompareExchange(
            &g_crashLoopGuardPrepared, 0, 0) == 1) {
        const bool sameDirectory =
            g_sessionMarkerPath[0] &&
            _stricmp(g_sessionMarkerPath, expectedMarkerPath) == 0;
        ReleaseSRWLockExclusive(&g_crashLoopGuardLock);
        return sameDirectory;
    }
    const bool prepared = PrepareCrashLoopGuard(cacheDirectory);
    if (prepared) {
        InterlockedExchange(&g_crashLoopGuardPrepared, 1);
    }
    ReleaseSRWLockExclusive(&g_crashLoopGuardLock);
    return prepared;
}

static bool StartsWithInsensitive(const char* value, const char* prefix) {
    if (!value || !prefix) return false;
    const size_t prefixLength = strlen(prefix);
    return _strnicmp(value, prefix, prefixLength) == 0;
}

static bool CopyForwardSlashPath(
    const char* input,
    char* output,
    size_t outputCapacity) {
    if (!input || !output || outputCapacity == 0) return false;
    const size_t inputLength = strlen(input);
    if (inputLength >= outputCapacity) return false;
    for (size_t index = 0; index <= inputLength; ++index) {
        const char c = input[index];
        output[index] = c == '\\' ? '/' : c;
    }
    return true;
}

static bool IsSafeRelativeDirectory(const char* path) {
    if (!path || !path[0]) return false;
    if (path[0] == '\\' || path[0] == '/' || strchr(path, ':')) return false;

    const char* segment = path;
    while (*segment) {
        while (*segment == '\\' || *segment == '/') ++segment;
        const char* end = segment;
        while (*end && *end != '\\' && *end != '/') ++end;
        const size_t length = (size_t)(end - segment);
        if (length == 2 && segment[0] == '.' && segment[1] == '.') {
            return false;
        }
        segment = end;
    }
    return true;
}

static bool IsPathWithinDirectory(
    const char* directory,
    const char* candidate) {
    if (!directory || !candidate) return false;
    const size_t directoryLength = strlen(directory);
    if (_strnicmp(directory, candidate, directoryLength) != 0) return false;
    const char boundary = candidate[directoryLength];
    return boundary == '\0' || boundary == '\\' || boundary == '/';
}

static bool CanonicalizeResourcePath(
    const char* input,
    bool asRoot,
    char* output,
    size_t outputCapacity) {
    if (!input || !input[0] || !output || outputCapacity < 7) return false;

    char normalized[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CopyForwardSlashPath(input, normalized, sizeof(normalized))) {
        return false;
    }

    const char* relative = normalized;
    if (StartsWithInsensitive(relative, "~res:/")) {
        relative += 6;
    } else if (StartsWithInsensitive(relative, "res:/")) {
        relative += 5;
    } else if (StartsWithInsensitive(relative, "Data/")) {
        relative += 5;
    } else {
        char gameData[WOTBMOD_MAX_RESOURCE_PATH] = {};
        char gameDirectory[WOTBMOD_MAX_RESOURCE_PATH] = {};
        if (g_gameDirectory[0] &&
            CopyForwardSlashPath(
                g_gameDirectory, gameDirectory, sizeof(gameDirectory))) {
            const int written = _snprintf_s(
                gameData,
                sizeof(gameData),
                _TRUNCATE,
                "%s/Data/",
                gameDirectory);
            if (written >= 0 && StartsWithInsensitive(relative, gameData)) {
                relative += strlen(gameData);
            } else if (strchr(relative, ':')) {
                return false;
            }
        } else if (strchr(relative, ':')) {
            return false;
        }
    }

    while (*relative == '/') ++relative;
    CopyString(output, outputCapacity, "~res:/");
    size_t used = strlen(output);
    bool wroteSegment = false;

    while (*relative) {
        while (*relative == '/') ++relative;
        if (!*relative) break;
        const char* end = strchr(relative, '/');
        if (!end) end = relative + strlen(relative);
        const size_t segmentLength = (size_t)(end - relative);
        if ((segmentLength == 1 && relative[0] == '.') ||
            (segmentLength == 2 &&
             relative[0] == '.' &&
             relative[1] == '.') ||
            memchr(relative, ':', segmentLength)) {
            return false;
        }

        if (wroteSegment) {
            if (used + 1 >= outputCapacity) return false;
            output[used++] = '/';
        }
        if (used + segmentLength >= outputCapacity) return false;
        memcpy(output + used, relative, segmentLength);
        used += segmentLength;
        output[used] = '\0';
        wroteSegment = true;
        relative = end;
    }

    if (asRoot && output[used - 1] != '/') {
        if (used + 1 >= outputCapacity) return false;
        output[used++] = '/';
        output[used] = '\0';
    }
    return true;
}

static bool IsRegularFile(const char* path) {
    if (!path || !path[0]) return false;
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool StockGameResourceExists(const char* canonicalPath) {
    if (!canonicalPath ||
        !StartsWithInsensitive(canonicalPath, "~res:/") ||
        !g_gameDirectory[0]) {
        return false;
    }

    char dataDirectory[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!JoinPath(
            dataDirectory,
            sizeof(dataDirectory),
            g_gameDirectory,
            "Data")) {
        return false;
    }

    char relative[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CopyForwardSlashPath(
            canonicalPath + 6,
            relative,
            sizeof(relative))) {
        return false;
    }
    for (char* current = relative; *current; ++current) {
        if (*current == '/') *current = '\\';
    }

    char candidate[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!JoinPath(
            candidate,
            sizeof(candidate),
            dataDirectory,
            relative)) {
        return false;
    }
    if (IsRegularFile(candidate)) return true;

    const size_t length = strlen(candidate);
    if (length + 5 >= sizeof(candidate) ||
        (length >= 5 &&
         _stricmp(candidate + length - 5, ".dvpl") == 0)) {
        return false;
    }
    strcat_s(candidate, sizeof(candidate), ".dvpl");
    return IsRegularFile(candidate);
}

static bool IsDirectory(const char* path) {
    if (!path || !path[0]) return false;
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static void NotifyResourceRegistryChanged() {
    const uint64_t generation =
        (uint64_t)InterlockedIncrement64(&g_resourceGeneration);
    if (!g_resourceBackend.registry_changed) return;
    __try {
        g_resourceBackend.registry_changed(
            g_resourceBackend.user_data, generation);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "resource registry callback faulted with SEH 0x%08lX",
            GetExceptionCode());
    }
}

static ModRecord* RecordFromHandle(WotbModHandle handle) {
    if (!handle) return nullptr;
    ModRecord* record = static_cast<ModRecord*>(handle);
    if (record < &g_mods[0] || record >= &g_mods[kMaxMods]) return nullptr;
    return record->magic == kModRecordMagic ? record : nullptr;
}

static void SyncPublicState(ModRecord* record) {
    if (!record) return;
    record->public_info.state = (uint32_t)record->state;
    record->public_info.enabled = record->enabled ? 1u : 0u;
    record->public_info.fault_count = (uint32_t)record->fault_count;
}

static const char* RecordLogId(const ModRecord* record) {
    if (!record) return "host";
    if (record->public_info.id[0]) return record->public_info.id;
    if (record->settings_key[0]) return record->settings_key;
    return "unknown";
}

static void WOTBMOD_CALL HostLog(
    WotbModHandle handle,
    WotbModLogLevel level,
    const char* message) {
    ModRecord* record = RecordFromHandle(handle);
    RuntimeLog(
        level,
        "[%s] %s",
        RecordLogId(record),
        message ? message : "");
}

static WotbModResult CopyPathToCaller(
    const char* path,
    char* buffer,
    uint32_t* inoutSize) {
    if (!path || !inoutSize) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    const uint32_t required = (uint32_t)strlen(path) + 1u;
    const uint32_t capacity = *inoutSize;
    *inoutSize = required;
    if (!buffer || capacity < required) return WOTBMOD_ERROR_BUFFER_TOO_SMALL;
    memcpy(buffer, path, required);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostGetPath(
    WotbModHandle handle,
    WotbModPath path,
    char* buffer,
    uint32_t* inoutSize) {
    ModRecord* record = RecordFromHandle(handle);
    switch (path) {
        case WOTBMOD_PATH_GAME:
            return CopyPathToCaller(g_gameDirectory, buffer, inoutSize);
        case WOTBMOD_PATH_MODS:
            return CopyPathToCaller(g_modsDirectory, buffer, inoutSize);
        case WOTBMOD_PATH_MODULE:
            if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
            return CopyPathToCaller(record->module_path, buffer, inoutSize);
        case WOTBMOD_PATH_DATA:
            if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
            return CopyPathToCaller(record->data_path, buffer, inoutSize);
        case WOTBMOD_PATH_CONFIG:
            if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
            return CopyPathToCaller(record->config_path, buffer, inoutSize);
        default:
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
}

static size_t GetModuleImageSize(HMODULE module) {
    if (!module) return 0;
    __try {
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool LegacyRawProcessApiAllowed() {
    return InterlockedCompareExchange(&g_initialized, 0, 0) != 0 &&
           (g_runtimeOptionFlags &
            WOTBMOD_RUNTIME_OPTION_ALLOW_LEGACY_RAW_PROCESS_API) != 0u;
}

static void* WOTBMOD_CALL HostGetGameModule() {
    if (!LegacyRawProcessApiAllowed()) return nullptr;
    return g_gameModule;
}

static void* WOTBMOD_CALL HostResolveRva(uint32_t rva) {
    if (!LegacyRawProcessApiAllowed()) return nullptr;
    const size_t imageSize = GetModuleImageSize(g_gameModule);
    if (!g_gameModule || imageSize == 0 || rva >= imageSize) return nullptr;
    return reinterpret_cast<uint8_t*>(g_gameModule) + rva;
}

static void* WOTBMOD_CALL HostGetProcAddress(
    const char* loadedModuleName,
    const char* exportName) {
    if (!LegacyRawProcessApiAllowed()) return nullptr;
    if (!exportName || !exportName[0]) return nullptr;
    HMODULE module = loadedModuleName && loadedModuleName[0]
                         ? GetModuleHandleA(loadedModuleName)
                         : g_gameModule;
    if (!module) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(module, exportName));
}

static bool IsReadableProtection(DWORD protection) {
    if ((protection & PAGE_GUARD) || (protection & PAGE_NOACCESS)) return false;
    const DWORD baseProtection = protection & 0xFFu;
    return baseProtection == PAGE_READONLY ||
           baseProtection == PAGE_READWRITE ||
           baseProtection == PAGE_WRITECOPY ||
           baseProtection == PAGE_EXECUTE ||
           baseProtection == PAGE_EXECUTE_READ ||
           baseProtection == PAGE_EXECUTE_READWRITE ||
           baseProtection == PAGE_EXECUTE_WRITECOPY;
}

static bool PatternMatches(
    const uint8_t* candidate,
    const uint8_t* pattern,
    const char* mask,
    size_t length) {
    __try {
        for (size_t index = 0; index < length; ++index) {
            if (mask[index] == 'x' && candidate[index] != pattern[index]) {
                return false;
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void* WOTBMOD_CALL HostFindPattern(
    const char* loadedModuleName,
    const uint8_t* pattern,
    const char* mask) {
    if (!LegacyRawProcessApiAllowed()) return nullptr;
    if (!pattern || !mask || !mask[0]) return nullptr;
    for (const char* current = mask; *current; ++current) {
        if (*current != 'x' && *current != '?') return nullptr;
    }
    HMODULE module = loadedModuleName && loadedModuleName[0]
                         ? GetModuleHandleA(loadedModuleName)
                         : g_gameModule;
    const size_t imageSize = GetModuleImageSize(module);
    const size_t patternLength = strlen(mask);
    if (!module || imageSize == 0 || patternLength > imageSize) return nullptr;

    uint8_t* imageStart = reinterpret_cast<uint8_t*>(module);
    uint8_t* imageEnd = imageStart + imageSize;
    uint8_t* cursor = imageStart;
    while (cursor < imageEnd) {
        MEMORY_BASIC_INFORMATION memory = {};
        if (!VirtualQuery(cursor, &memory, sizeof(memory))) break;

        uint8_t* regionStart =
            static_cast<uint8_t*>(memory.BaseAddress) > imageStart
                ? static_cast<uint8_t*>(memory.BaseAddress)
                : imageStart;
        uint8_t* rawRegionEnd =
            static_cast<uint8_t*>(memory.BaseAddress) + memory.RegionSize;
        uint8_t* regionEnd = rawRegionEnd < imageEnd ? rawRegionEnd : imageEnd;

        if (memory.State == MEM_COMMIT &&
            IsReadableProtection(memory.Protect) &&
            regionEnd > regionStart &&
            (size_t)(regionEnd - regionStart) >= patternLength) {
            uint8_t* last = regionEnd - patternLength;
            for (uint8_t* candidate = regionStart; candidate <= last; ++candidate) {
                if (PatternMatches(candidate, pattern, mask, patternLength)) {
                    return candidate;
                }
            }
        }

        if (rawRegionEnd <= cursor) break;
        cursor = rawRegionEnd;
    }
    return nullptr;
}

static int FindOwnedHook(const ModRecord* record, void* target) {
    if (!record || !target) return -1;
    const LONG count = record->hook_count;
    for (LONG index = 0; index < count && index < (LONG)kMaxHooksPerMod; ++index) {
        if (record->hooks[index].active &&
            record->hooks[index].target == target) {
            return (int)index;
        }
    }
    return -1;
}

static WotbModResult RemoveOwnedHooks(ModRecord* record) {
    if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    WotbModResult cleanupResult = WOTBMOD_OK;
    for (LONG index = record->hook_count - 1; index >= 0; --index) {
        HookRecord* hook = &record->hooks[index];
        if (!hook->active || !hook->target) continue;
        if (g_hookBackend.disable) {
            const WotbModResult result =
                g_hookBackend.disable(
                    g_hookBackend.user_data, hook->target);
            if (result != WOTBMOD_OK && cleanupResult == WOTBMOD_OK) {
                cleanupResult = result;
            }
        }
        WotbModResult removeResult = WOTBMOD_ERROR_PLATFORM;
        if (g_hookBackend.remove) {
            removeResult =
                g_hookBackend.remove(
                    g_hookBackend.user_data, hook->target);
        }
        if (removeResult == WOTBMOD_OK) {
            hook->active = 0;
            hook->target = nullptr;
        } else if (cleanupResult == WOTBMOD_OK) {
            cleanupResult = removeResult;
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] failed to remove owned hook %p (result=%d)",
                RecordLogId(record),
                hook->target,
                (int)removeResult);
        }
    }
    while (record->hook_count > 0 &&
           !record->hooks[record->hook_count - 1].active) {
        --record->hook_count;
    }
    return cleanupResult;
}

static bool HookBackendReady() {
    return g_hookBackend.create &&
           g_hookBackend.enable &&
           g_hookBackend.disable &&
           g_hookBackend.remove;
}

static WotbModResult WOTBMOD_CALL HostHookCreate(
    WotbModHandle handle,
    void* target,
    void* detour,
    void** original) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target || !detour || !original) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!LegacyRawProcessApiAllowed()) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    if (FindOwnedHook(record, target) >= 0) return WOTBMOD_ERROR_ALREADY_EXISTS;
    LONG slotIndex = -1;
    for (LONG index = 0; index < record->hook_count; ++index) {
        if (!record->hooks[index].active) {
            slotIndex = index;
            break;
        }
    }
    const bool appendSlot = slotIndex < 0;
    if (appendSlot) {
        if (record->hook_count >= (LONG)kMaxHooksPerMod) {
            return WOTBMOD_ERROR_LIMIT_REACHED;
        }
        slotIndex = record->hook_count;
    }

    WotbModResult result = g_hookBackend.create(
        g_hookBackend.user_data, target, detour, original);
    if (result != WOTBMOD_OK) return result;

    if (appendSlot) ++record->hook_count;
    HookRecord* hook = &record->hooks[slotIndex];
    hook->target = target;
    hook->active = 1;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostHookEnable(
    WotbModHandle handle,
    void* target) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!LegacyRawProcessApiAllowed()) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    if (FindOwnedHook(record, target) < 0) return WOTBMOD_ERROR_ACCESS_DENIED;
    return g_hookBackend.enable(g_hookBackend.user_data, target);
}

static WotbModResult WOTBMOD_CALL HostHookDisable(
    WotbModHandle handle,
    void* target) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!LegacyRawProcessApiAllowed()) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    if (FindOwnedHook(record, target) < 0) return WOTBMOD_ERROR_ACCESS_DENIED;
    return g_hookBackend.disable(g_hookBackend.user_data, target);
}

static WotbModResult WOTBMOD_CALL HostHookRemove(
    WotbModHandle handle,
    void* target) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !target) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!LegacyRawProcessApiAllowed()) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (!HookBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    const int hookIndex = FindOwnedHook(record, target);
    if (hookIndex < 0) return WOTBMOD_ERROR_ACCESS_DENIED;

    WotbModResult result =
        g_hookBackend.remove(g_hookBackend.user_data, target);
    if (result == WOTBMOD_OK) {
        record->hooks[hookIndex].active = 0;
        record->hooks[hookIndex].target = nullptr;
        while (record->hook_count > 0 &&
               !record->hooks[record->hook_count - 1].active) {
            --record->hook_count;
        }
    }
    return result;
}

static bool ResourceBackendReady() {
    return g_resourceBackend.load && g_resourceBackend.release;
}

static bool AudioClipBackendReady() {
    return g_audioBackend.load_clip && g_audioBackend.release_clip;
}

static WotbModResult CallResourceBackendOperation(
    WotbModRuntimeResourceOperation operation,
    void* nativeResource) {
    if (!operation || !nativeResource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = operation(g_resourceBackend.user_data, nativeResource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "resource backend operation faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static WotbModResult CallResourceBackendClone(
    void* nativeResource,
    void** outNativeResource) {
    if (!g_resourceBackend.clone ||
        !nativeResource ||
        !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_resourceBackend.clone(
            g_resourceBackend.user_data,
            nativeResource,
            outNativeResource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "resource backend clone faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result == WOTBMOD_OK && !*outNativeResource) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    return result;
}

static WotbModResult CallAudioClipBackendOperation(
    WotbModRuntimeAudioClipOperation operation,
    void* nativeResource,
    const char* operationName) {
    if (!operation || !nativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = operation(g_audioBackend.user_data, nativeResource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "audio clip backend %s faulted with SEH 0x%08lX",
            operationName ? operationName : "operation",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static bool BuildMountedCandidate(
    const ResourceMountRecord* mount,
    const char* canonicalPath,
    char* output,
    size_t outputCapacity) {
    if (!mount || !canonicalPath || !output || outputCapacity == 0) {
        return false;
    }
    const size_t rootLength = strlen(mount->virtual_root);
    if (_strnicmp(canonicalPath, mount->virtual_root, rootLength) != 0) {
        return false;
    }

    const char* suffix = canonicalPath + rootLength;
    if (!suffix[0]) return false;
    char relative[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CopyForwardSlashPath(suffix, relative, sizeof(relative))) return false;
    for (char* current = relative; *current; ++current) {
        if (*current == '/') *current = '\\';
    }

    const int written = _snprintf_s(
        output,
        outputCapacity,
        _TRUNCATE,
        "%s\\%s",
        mount->source_directory,
        relative);
    if (written < 0) return false;
    if (IsRegularFile(output)) return true;

    if ((mount->flags & WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL) == 0) {
        return false;
    }

    const size_t outputLength = strlen(output);
    if (outputLength >= 5 &&
        _stricmp(output + outputLength - 5, ".dvpl") == 0) {
        output[outputLength - 5] = '\0';
        return IsRegularFile(output);
    }
    if (outputLength + 5 >= outputCapacity) return false;
    strcat_s(output, outputCapacity, ".dvpl");
    return IsRegularFile(output);
}

static WotbModResult ResolveResourcePathForOwnerInternal(
    const char* requestedPath,
    const ModRecord* requiredOwner,
    char* buffer,
    uint32_t* inoutSize) {
    if (!requestedPath || !inoutSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    if (!requiredOwner) {
        const WotbModV3Result v3Overlay =
            wotbmod::v3::ResolveActiveGameOverlayPath(
                requestedPath,
                buffer,
                inoutSize);
        if (v3Overlay == WOTBMOD_V3_OK ||
            v3Overlay == WOTBMOD_V3_E_BUFFER_TOO_SMALL) {
            return V3ResultToLegacy(v3Overlay);
        }
        if (v3Overlay != WOTBMOD_V3_E_NOT_FOUND) {
            return V3ResultToLegacy(v3Overlay);
        }
    }

    char canonical[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CanonicalizeResourcePath(
            requestedPath, false, canonical, sizeof(canonical))) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char bestPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    int32_t bestPriority = INT32_MIN;
    WotbModResourceMountId bestId = 0;
    AcquireSRWLockShared(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        const ResourceMountRecord* mount = &g_resourceMounts[index];
        if (!mount->active ||
            !mount->owner ||
            (requiredOwner && mount->owner != requiredOwner) ||
            !mount->owner->enabled ||
            mount->owner->state != WOTBMOD_STATE_ENABLED) {
            continue;
        }

        char candidate[WOTBMOD_MAX_RESOURCE_PATH] = {};
        if (!BuildMountedCandidate(
                mount, canonical, candidate, sizeof(candidate))) {
            continue;
        }
        if (mount->priority > bestPriority ||
            (mount->priority == bestPriority && mount->id > bestId)) {
            bestPriority = mount->priority;
            bestId = mount->id;
            CopyString(bestPath, sizeof(bestPath), candidate);
        }
    }
    ReleaseSRWLockShared(&g_resourceMountLock);

    return bestPath[0]
               ? CopyPathToCaller(bestPath, buffer, inoutSize)
               : WOTBMOD_ERROR_NOT_FOUND;
}

static WotbModResult ResolveResourcePathInternal(
    const char* requestedPath,
    char* buffer,
    uint32_t* inoutSize) {
    return ResolveResourcePathForOwnerInternal(
        requestedPath, nullptr, buffer, inoutSize);
}

static WotbModResult WOTBMOD_CALL HostResourceMount(
    WotbModHandle handle,
    const WotbModResourceMountInfo* mountInfo,
    WotbModResourceMountId* outMountId) {
    ModRecord* record = RecordFromHandle(handle);
    const size_t requiredSize =
        offsetof(WotbModResourceMountInfo, flags) +
        sizeof(mountInfo->flags);
    if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!mountInfo ||
        mountInfo->struct_size < requiredSize ||
        !mountInfo->virtual_root ||
        !mountInfo->source_directory ||
        !outMountId ||
        (mountInfo->flags & ~WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL) != 0) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char virtualRoot[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CanonicalizeResourcePath(
            mountInfo->virtual_root,
            true,
            virtualRoot,
            sizeof(virtualRoot)) ||
        !IsSafeRelativeDirectory(mountInfo->source_directory)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char joined[WOTBMOD_MAX_RESOURCE_PATH] = {};
    char sourceDirectory[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!JoinPath(
            joined,
            sizeof(joined),
            record->data_path,
            mountInfo->source_directory) ||
        !NormalizeAbsolutePath(
            joined, sourceDirectory, sizeof(sourceDirectory)) ||
        !IsPathWithinDirectory(record->data_path, sourceDirectory)) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (!IsDirectory(sourceDirectory)) return WOTBMOD_ERROR_NOT_FOUND;

    ResourceMountRecord* available = nullptr;
    uint32_t ownedCount = 0;
    AcquireSRWLockExclusive(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        ResourceMountRecord* current = &g_resourceMounts[index];
        if (current->active && current->owner == record) ++ownedCount;
        if (!current->active && !available) available = current;
    }
    if (!available || ownedCount >= kMaxResourceMountsPerMod) {
        ReleaseSRWLockExclusive(&g_resourceMountLock);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    ZeroMemory(available, sizeof(*available));
    available->owner = record;
    available->id =
        (WotbModResourceMountId)InterlockedIncrement64(
            &g_nextResourceMountId);
    available->priority = mountInfo->priority;
    available->flags = mountInfo->flags;
    CopyString(
        available->virtual_root,
        sizeof(available->virtual_root),
        virtualRoot);
    CopyString(
        available->source_directory,
        sizeof(available->source_directory),
        sourceDirectory);
    InterlockedExchange(&available->active, 1);
    *outMountId = available->id;
    ReleaseSRWLockExclusive(&g_resourceMountLock);

    NotifyResourceRegistryChanged();
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] mounted %s -> %s (priority=%ld)",
        RecordLogId(record),
        virtualRoot,
        sourceDirectory,
        (long)mountInfo->priority);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostResourceUnmount(
    WotbModHandle handle,
    WotbModResourceMountId mountId) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || mountId == 0) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    bool removed = false;
    bool denied = false;
    AcquireSRWLockExclusive(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        ResourceMountRecord* mount = &g_resourceMounts[index];
        if (!mount->active || mount->id != mountId) continue;
        if (mount->owner != record) {
            denied = true;
            break;
        }
        ZeroMemory(mount, sizeof(*mount));
        removed = true;
        break;
    }
    ReleaseSRWLockExclusive(&g_resourceMountLock);
    if (denied) return WOTBMOD_ERROR_ACCESS_DENIED;
    if (!removed) return WOTBMOD_ERROR_NOT_FOUND;
    NotifyResourceRegistryChanged();
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostResourceResolve(
    WotbModHandle handle,
    const char* virtualPath,
    char* buffer,
    uint32_t* inoutSize) {
    if (!RecordFromHandle(handle)) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    return ResolveResourcePathInternal(virtualPath, buffer, inoutSize);
}

static VehicleSkinHandleRecord* VehicleSkinFromHandle(
    ModRecord* record,
    WotbModVehicleSkinHandle handle) {
    if (!record || !handle) return nullptr;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    for (uint32_t index = 0;
         index < kMaxVehicleSkinsPerMod;
         ++index) {
        VehicleSkinHandleRecord* skin =
            &record->vehicle_skins[index];
        if (skin->public_id == value) return skin;
    }
    return nullptr;
}

static bool IsValidVehicleSkinId(const char* id) {
    if (!id || !id[0] || strlen(id) >= WOTBMOD_MAX_SKIN_ID) {
        return false;
    }
    for (const char* current = id; *current; ++current) {
        const char c = *current;
        if (!((c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

static void FreeVehicleSkinRecord(
    VehicleSkinHandleRecord* skin) {
    if (!skin) return;
    if (skin->assets) {
        HeapFree(GetProcessHeap(), 0, skin->assets);
    }
    ZeroMemory(skin, sizeof(*skin));
}

static WotbModResult WOTBMOD_CALL HostVehicleSkinRegister(
    WotbModHandle handle,
    const WotbModVehicleSkinDescriptor* descriptor,
    WotbModVehicleSkinHandle* outSkin) {
    ModRecord* record = RecordFromHandle(handle);
    const size_t descriptorSize =
        offsetof(WotbModVehicleSkinDescriptor, flags) +
        sizeof(descriptor->flags);
    const size_t assetSize =
        offsetof(WotbModVehicleSkinAsset, replacement_virtual_path) +
        sizeof(descriptor->assets[0].replacement_virtual_path);
    if (!record || !descriptor || !outSkin ||
        descriptor->struct_size < descriptorSize ||
        !IsValidVehicleSkinId(descriptor->skin_id) ||
        !descriptor->vehicle_name ||
        strlen(descriptor->vehicle_name) >=
            WOTBMOD_MAX_VEHICLE_NAME ||
        !descriptor->assets ||
        descriptor->asset_count == 0 ||
        descriptor->asset_count > kMaxVehicleSkinAssets ||
        (descriptor->flags & ~WOTBMOD_VEHICLE_SKIN_ENABLED) != 0) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outSkin = nullptr;
    if (!record->enabled ||
        record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }

    VehicleSkinAssetRecord* assets =
        static_cast<VehicleSkinAssetRecord*>(HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(VehicleSkinAssetRecord) *
                descriptor->asset_count));
    if (!assets) return WOTBMOD_ERROR_LIMIT_REACHED;

    for (uint32_t index = 0;
         index < descriptor->asset_count;
         ++index) {
        const WotbModVehicleSkinAsset* source =
            &descriptor->assets[index];
        if (source->struct_size < assetSize ||
            source->kind > WOTBMOD_SKIN_ASSET_TEXTURE ||
            !source->stock_virtual_path ||
            !source->replacement_virtual_path ||
            !CanonicalizeResourcePath(
                source->stock_virtual_path,
                false,
                assets[index].stock_virtual_path,
                sizeof(assets[index].stock_virtual_path)) ||
            !CanonicalizeResourcePath(
                source->replacement_virtual_path,
                false,
                assets[index].replacement_virtual_path,
                sizeof(assets[index].replacement_virtual_path))) {
            HeapFree(GetProcessHeap(), 0, assets);
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
        const size_t stockLength =
            strlen(assets[index].stock_virtual_path);
        if (stockLength >= 5u &&
            _stricmp(
                assets[index].stock_virtual_path +
                    stockLength - 5u,
                ".dvpl") == 0) {
            assets[index].stock_virtual_path[
                stockLength - 5u] = '\0';
        }
        assets[index].kind = source->kind;
        for (uint32_t previous = 0;
             previous < index;
             ++previous) {
            if (_stricmp(
                    assets[previous].stock_virtual_path,
                    assets[index].stock_virtual_path) == 0) {
                HeapFree(GetProcessHeap(), 0, assets);
                return WOTBMOD_ERROR_ALREADY_EXISTS;
            }
        }
    }

    VehicleSkinHandleRecord* available = nullptr;
    AcquireSRWLockExclusive(&g_vehicleSkinLock);
    for (uint32_t index = 0;
         index < kMaxVehicleSkinsPerMod;
         ++index) {
        VehicleSkinHandleRecord* skin =
            &record->vehicle_skins[index];
        if (skin->active &&
            _stricmp(skin->skin_id, descriptor->skin_id) == 0) {
            ReleaseSRWLockExclusive(&g_vehicleSkinLock);
            HeapFree(GetProcessHeap(), 0, assets);
            return WOTBMOD_ERROR_ALREADY_EXISTS;
        }
        if (!skin->active && !available) available = skin;
    }
    if (!available) {
        ReleaseSRWLockExclusive(&g_vehicleSkinLock);
        HeapFree(GetProcessHeap(), 0, assets);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    ZeroMemory(available, sizeof(*available));
    uintptr_t publicId =
        (uintptr_t)(uint32_t)InterlockedIncrement(
            &g_nextVehicleSkinHandleId);
    if (publicId == 0) {
        publicId =
            (uintptr_t)(uint32_t)InterlockedIncrement(
                &g_nextVehicleSkinHandleId);
    }
    available->public_id = publicId;
    available->sequence =
        (uint64_t)InterlockedIncrement64(
            &g_nextVehicleSkinSequence);
    available->priority = descriptor->priority;
    available->flags = descriptor->flags;
    available->asset_count = descriptor->asset_count;
    available->assets = assets;
    CopyString(
        available->skin_id,
        sizeof(available->skin_id),
        descriptor->skin_id);
    CopyString(
        available->vehicle_name,
        sizeof(available->vehicle_name),
        descriptor->vehicle_name);
    available->enabled =
        (descriptor->flags & WOTBMOD_VEHICLE_SKIN_ENABLED)
            ? 1
            : 0;
    available->active = 1;
    *outSkin =
        reinterpret_cast<WotbModVehicleSkinHandle>(publicId);
    ReleaseSRWLockExclusive(&g_vehicleSkinLock);
    NotifyResourceRegistryChanged();
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostVehicleSkinSetEnabled(
    WotbModHandle handle,
    WotbModVehicleSkinHandle skinHandle,
    int32_t enabled) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !skinHandle) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled ||
        record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    AcquireSRWLockExclusive(&g_vehicleSkinLock);
    VehicleSkinHandleRecord* skin =
        VehicleSkinFromHandle(record, skinHandle);
    if (!skin || skin->active != 1) {
        ReleaseSRWLockExclusive(&g_vehicleSkinLock);
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    const LONG requested = enabled ? 1 : 0;
    const bool changed = skin->enabled != requested;
    skin->enabled = requested;
    ReleaseSRWLockExclusive(&g_vehicleSkinLock);
    if (changed) NotifyResourceRegistryChanged();
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostVehicleSkinGetInfo(
    WotbModHandle handle,
    WotbModVehicleSkinHandle skinHandle,
    WotbModVehicleSkinInfo* outInfo) {
    ModRecord* record = RecordFromHandle(handle);
    const size_t requiredSize =
        offsetof(WotbModVehicleSkinInfo, priority) +
        sizeof(outInfo->priority);
    if (!record || !skinHandle || !outInfo ||
        outInfo->struct_size < requiredSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t callerSize = outInfo->struct_size;
    WotbModVehicleSkinInfo info = {};
    info.struct_size = sizeof(info);
    AcquireSRWLockShared(&g_vehicleSkinLock);
    VehicleSkinHandleRecord* skin =
        VehicleSkinFromHandle(record, skinHandle);
    if (!skin || skin->active != 1) {
        ReleaseSRWLockShared(&g_vehicleSkinLock);
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    info.enabled = skin->enabled ? 1u : 0u;
    info.asset_count = skin->asset_count;
    info.priority = skin->priority;
    info.flags = skin->flags;
    CopyString(
        info.skin_id, sizeof(info.skin_id), skin->skin_id);
    CopyString(
        info.vehicle_name,
        sizeof(info.vehicle_name),
        skin->vehicle_name);
    ReleaseSRWLockShared(&g_vehicleSkinLock);
    const uint32_t copySize =
        callerSize < sizeof(info)
            ? callerSize
            : static_cast<uint32_t>(sizeof(info));
    memcpy(outInfo, &info, copySize);
    return callerSize < sizeof(info)
               ? WOTBMOD_ERROR_BUFFER_TOO_SMALL
               : WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostVehicleSkinRelease(
    WotbModHandle handle,
    WotbModVehicleSkinHandle skinHandle) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !skinHandle) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&g_vehicleSkinLock);
    VehicleSkinHandleRecord* skin =
        VehicleSkinFromHandle(record, skinHandle);
    if (!skin || skin->active != 1) {
        ReleaseSRWLockExclusive(&g_vehicleSkinLock);
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    FreeVehicleSkinRecord(skin);
    ReleaseSRWLockExclusive(&g_vehicleSkinLock);
    NotifyResourceRegistryChanged();
    return WOTBMOD_OK;
}

static void RemoveOwnedVehicleSkins(ModRecord* record) {
    if (!record) return;
    bool removed = false;
    AcquireSRWLockExclusive(&g_vehicleSkinLock);
    for (uint32_t index = 0;
         index < kMaxVehicleSkinsPerMod;
         ++index) {
        VehicleSkinHandleRecord* skin =
            &record->vehicle_skins[index];
        if (skin->active) {
            FreeVehicleSkinRecord(skin);
            removed = true;
        }
    }
    ReleaseSRWLockExclusive(&g_vehicleSkinLock);
    if (removed) NotifyResourceRegistryChanged();
}

static WotbModResult ResolveVehicleSkinPathInternal(
    const char* requestedPath,
    char* buffer,
    uint32_t* inoutSize) {
    if (!requestedPath || !inoutSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    char canonical[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CanonicalizeResourcePath(
            requestedPath,
            false,
            canonical,
            sizeof(canonical))) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const size_t canonicalLength = strlen(canonical);
    if (canonicalLength >= 5u &&
        _stricmp(
            canonical + canonicalLength - 5u,
            ".dvpl") == 0) {
        canonical[canonicalLength - 5u] = '\0';
    }

    char replacement[WOTBMOD_MAX_RESOURCE_PATH] = {};
    ModRecord* owner = nullptr;
    int32_t bestPriority = INT32_MIN;
    uint64_t bestSequence = 0;
    AcquireSRWLockShared(&g_vehicleSkinLock);
    const LONG modCount = g_modCount;
    for (LONG modIndex = 0;
         modIndex < modCount;
         ++modIndex) {
        ModRecord* record = &g_mods[modIndex];
        if (!record->enabled ||
            record->state != WOTBMOD_STATE_ENABLED) {
            continue;
        }
        for (uint32_t skinIndex = 0;
             skinIndex < kMaxVehicleSkinsPerMod;
             ++skinIndex) {
            const VehicleSkinHandleRecord* skin =
                &record->vehicle_skins[skinIndex];
            if (!skin->active || !skin->enabled ||
                !skin->assets) {
                continue;
            }
            if (skin->priority < bestPriority ||
                (skin->priority == bestPriority &&
                 skin->sequence <= bestSequence)) {
                continue;
            }
            for (uint32_t assetIndex = 0;
                 assetIndex < skin->asset_count;
                 ++assetIndex) {
                const VehicleSkinAssetRecord* asset =
                    &skin->assets[assetIndex];
                if (_stricmp(
                        asset->stock_virtual_path,
                        canonical) != 0) {
                    continue;
                }
                owner = record;
                bestPriority = skin->priority;
                bestSequence = skin->sequence;
                CopyString(
                    replacement,
                    sizeof(replacement),
                    asset->replacement_virtual_path);
                break;
            }
        }
    }
    ReleaseSRWLockShared(&g_vehicleSkinLock);
    if (!owner || !replacement[0]) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    const WotbModResult mountedResult =
        ResolveResourcePathForOwnerInternal(
            replacement, owner, buffer, inoutSize);
    if (mountedResult != WOTBMOD_ERROR_NOT_FOUND) {
        return mountedResult;
    }
    return StockGameResourceExists(replacement)
               ? CopyPathToCaller(replacement, buffer, inoutSize)
               : WOTBMOD_ERROR_NOT_FOUND;
}

static ResourceHandleRecord* ResourceFromHandle(
    ModRecord* record,
    WotbModResourceHandle handle) {
    if (!record || !handle) return nullptr;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    for (uint32_t index = 0; index < kMaxResourcesPerMod; ++index) {
        ResourceHandleRecord* resource = &record->resources[index];
        if (resource->public_id == value) return resource;
    }
    for (uint32_t index = 0;
         index < kMaxBorrowedEventResources;
         ++index) {
        ResourceHandleRecord* resource =
            &record->borrowed_event_resources[index];
        if (resource->public_id == value) return resource;
    }
    return nullptr;
}

static ResourceHandleRecord* ReserveResourceSlot(ModRecord* record) {
    if (!record) return nullptr;
    for (uint32_t index = 0; index < kMaxResourcesPerMod; ++index) {
        if (InterlockedCompareExchange(
                &record->resources[index].active, -1, 0) == 0) {
            return &record->resources[index];
        }
    }
    return nullptr;
}

static void AbandonResourceSlot(ResourceHandleRecord* slot) {
    if (!slot) return;
    slot->native_resource = nullptr;
    slot->type = WOTBMOD_RESOURCE_GENERIC;
    slot->backend_kind = RESOURCE_BACKEND_NONE;
    slot->ownership_kind = RESOURCE_OWNED;
    slot->public_id = 0;
    InterlockedExchange(&slot->active, 0);
}

static WotbModResult PublishResourceSlot(
    ResourceHandleRecord* slot,
    void* nativeResource,
    WotbModResourceType type,
    ResourceBackendKind backendKind,
    WotbModResourceHandle* outResource) {
    if (!slot || !nativeResource || !outResource || slot->active != -1) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t publicId =
        (uintptr_t)(uint32_t)InterlockedIncrement(&g_nextResourceHandleId);
    if (publicId == 0) {
        publicId =
            (uintptr_t)(uint32_t)InterlockedIncrement(
                &g_nextResourceHandleId);
    }
    slot->native_resource = nativeResource;
    slot->type = type;
    slot->backend_kind = backendKind;
    slot->ownership_kind = RESOURCE_OWNED;
    slot->public_id = publicId;
    InterlockedExchange(&slot->active, 1);
    *outResource = reinterpret_cast<WotbModResourceHandle>(publicId);
    return WOTBMOD_OK;
}

static WotbModResourceHandle PublishBorrowedEventResource(
    ResourceHandleRecord* slot,
    void* nativeResource,
    WotbModResourceType type) {
    if (!slot || !nativeResource || slot->active != 0) return nullptr;
    uintptr_t publicId =
        (uintptr_t)(uint32_t)InterlockedIncrement(
            &g_nextResourceHandleId);
    if (publicId == 0) {
        publicId =
            (uintptr_t)(uint32_t)InterlockedIncrement(
                &g_nextResourceHandleId);
    }
    slot->native_resource = nativeResource;
    slot->type = type;
    slot->backend_kind = RESOURCE_BACKEND_GENERIC;
    slot->ownership_kind = RESOURCE_BORROWED_EVENT;
    slot->public_id = publicId;
    InterlockedExchange(&slot->active, 1);
    return reinterpret_cast<WotbModResourceHandle>(publicId);
}

static void InvalidateBorrowedEventResources(ModRecord* record) {
    if (!record) return;
    for (uint32_t index = 0;
         index < kMaxBorrowedEventResources;
         ++index) {
        ZeroMemory(
            &record->borrowed_event_resources[index],
            sizeof(record->borrowed_event_resources[index]));
    }
}

static bool GameplayBackendReady() {
    return g_gameplayBackend.vehicle_get_by_entity_id &&
           g_gameplayBackend.vehicle_clone &&
           g_gameplayBackend.vehicle_release &&
           g_gameplayBackend.vehicle_get_info;
}

static WotbModResult CallGameplayRelease(void* token) {
    if (!token || !g_gameplayBackend.vehicle_release) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        return g_gameplayBackend.vehicle_release(
            g_gameplayBackend.user_data, token);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "gameplay backend release faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static VehicleHandleRecord* VehicleFromHandle(
    ModRecord* record,
    WotbModVehicleHandle handle) {
    if (!record || !handle) return nullptr;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    for (uint32_t index = 0; index < kMaxVehiclesPerMod; ++index) {
        VehicleHandleRecord* vehicle = &record->vehicles[index];
        if (vehicle->public_id == value) return vehicle;
    }
    for (uint32_t index = 0;
         index < kMaxBorrowedEventVehicles;
         ++index) {
        VehicleHandleRecord* vehicle =
            &record->borrowed_event_vehicles[index];
        if (vehicle->public_id == value) return vehicle;
    }
    return nullptr;
}

static VehicleHandleRecord* ReserveVehicleSlot(ModRecord* record) {
    if (!record) return nullptr;
    for (uint32_t index = 0; index < kMaxVehiclesPerMod; ++index) {
        if (InterlockedCompareExchange(
                &record->vehicles[index].active, -1, 0) == 0) {
            return &record->vehicles[index];
        }
    }
    return nullptr;
}

static void AbandonVehicleSlot(VehicleHandleRecord* slot) {
    if (!slot) return;
    ZeroMemory(slot, sizeof(*slot));
}

static WotbModResult PublishVehicleSlot(
    VehicleHandleRecord* slot,
    void* token,
    WotbModVehicleHandle* outVehicle) {
    if (!slot || !token || !outVehicle || slot->active != -1) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    uintptr_t publicId =
        (uintptr_t)(uint32_t)InterlockedIncrement(
            &g_nextVehicleHandleId);
    if (publicId == 0) {
        publicId =
            (uintptr_t)(uint32_t)InterlockedIncrement(
                &g_nextVehicleHandleId);
    }
    slot->token = token;
    slot->public_id = publicId;
    slot->ownership_kind = VEHICLE_OWNED;
    InterlockedExchange(&slot->active, 1);
    *outVehicle = reinterpret_cast<WotbModVehicleHandle>(publicId);
    return WOTBMOD_OK;
}

static WotbModVehicleHandle PublishBorrowedEventVehicle(
    VehicleHandleRecord* slot,
    void* token) {
    if (!slot || !token || slot->active != 0) return nullptr;
    uintptr_t publicId =
        (uintptr_t)(uint32_t)InterlockedIncrement(
            &g_nextVehicleHandleId);
    if (publicId == 0) {
        publicId =
            (uintptr_t)(uint32_t)InterlockedIncrement(
                &g_nextVehicleHandleId);
    }
    slot->token = token;
    slot->public_id = publicId;
    slot->ownership_kind = VEHICLE_BORROWED_EVENT;
    InterlockedExchange(&slot->active, 1);
    return reinterpret_cast<WotbModVehicleHandle>(publicId);
}

static void InvalidateBorrowedEventVehicles(ModRecord* record) {
    if (!record) return;
    for (uint32_t index = 0;
         index < kMaxBorrowedEventVehicles;
         ++index) {
        VehicleHandleRecord* vehicle =
            &record->borrowed_event_vehicles[index];
        if (vehicle->active == 1 && vehicle->token) {
            CallGameplayRelease(vehicle->token);
        }
        ZeroMemory(vehicle, sizeof(*vehicle));
    }
}

static void RemoveOwnedVehicles(ModRecord* record) {
    if (!record) return;
    InvalidateBorrowedEventVehicles(record);
    for (uint32_t index = 0; index < kMaxVehiclesPerMod; ++index) {
        VehicleHandleRecord* vehicle = &record->vehicles[index];
        if (InterlockedCompareExchange(
                &vehicle->active, -1, 1) != 1) {
            continue;
        }
        if (vehicle->token) CallGameplayRelease(vehicle->token);
        ZeroMemory(vehicle, sizeof(*vehicle));
    }
}

static void RemoveOwnedMainThreadWork(ModRecord* record) {
    if (!record) return;
    AcquireSRWLockExclusive(&g_mainThreadWorkLock);
    for (uint32_t index = 0; index < kMaxMainThreadWorkItems; ++index) {
        MainThreadWorkItem* work = &g_mainThreadWork[index];
        if (work->active && work->owner == record) {
            ZeroMemory(work, sizeof(*work));
        }
    }
    ReleaseSRWLockExclusive(&g_mainThreadWorkLock);
}

static WotbModResult EnqueueMainThreadWork(
    const MainThreadWorkItem* requested) {
    if (!requested || !requested->owner) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    MainThreadWorkItem* available = nullptr;
    uint32_t ownedCount = 0;
    AcquireSRWLockExclusive(&g_mainThreadWorkLock);
    for (uint32_t index = 0; index < kMaxMainThreadWorkItems; ++index) {
        MainThreadWorkItem* work = &g_mainThreadWork[index];
        if (work->active && work->owner == requested->owner) {
            ++ownedCount;
        } else if (!work->active && !available) {
            available = work;
        }
    }
    if (!available || ownedCount >= kMaxMainThreadWorkItemsPerMod) {
        ReleaseSRWLockExclusive(&g_mainThreadWorkLock);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    *available = *requested;
    available->sequence =
        (uint64_t)InterlockedIncrement64(&g_nextMainThreadSequence);
    available->active = 1;
    ReleaseSRWLockExclusive(&g_mainThreadWorkLock);
    return WOTBMOD_OK;
}

static bool HasActiveAudioForResource(
    const ModRecord* record,
    const ResourceHandleRecord* resource) {
    if (!record || !resource) return false;
    for (uint32_t index = 0; index < kMaxAudioPlaybacksPerMod; ++index) {
        const AudioPlaybackRecord* playback =
            &record->audio_playbacks[index];
        if (playback->active == 1 &&
            playback->source_resource == resource) {
            return true;
        }
    }
    return false;
}

static WotbModResult WOTBMOD_CALL HostResourceLoad(
    WotbModHandle handle,
    const WotbModResourceLoadRequest* request,
    WotbModResourceHandle* outResource) {
    ModRecord* record = RecordFromHandle(handle);
    const size_t requiredSize =
        offsetof(WotbModResourceLoadRequest, flags) +
        sizeof(request->flags);
    if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!request ||
        request->struct_size < requiredSize ||
        !request->virtual_path ||
        !outResource ||
        request->type < WOTBMOD_RESOURCE_GENERIC ||
        request->type > WOTBMOD_RESOURCE_AUDIO_CLIP) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outResource = nullptr;
    const bool useAudioFileBackend =
        request->type == WOTBMOD_RESOURCE_AUDIO_CLIP &&
        AudioClipBackendReady();
    if (!useAudioFileBackend && !ResourceBackendReady()) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    ResourceHandleRecord* slot = ReserveResourceSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;

    char canonical[WOTBMOD_MAX_RESOURCE_PATH] = {};
    if (!CanonicalizeResourcePath(
            request->virtual_path, false, canonical, sizeof(canonical))) {
        AbandonResourceSlot(slot);
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    WotbModResourceLoadRequest normalized = *request;
    normalized.struct_size = sizeof(normalized);
    normalized.virtual_path = canonical;
    void* nativeResource = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    ResourceBackendKind backendKind = RESOURCE_BACKEND_GENERIC;
    if (useAudioFileBackend) {
        char resolvedPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
        uint32_t resolvedSize = sizeof(resolvedPath);
        result = ResolveResourcePathInternal(
            canonical, resolvedPath, &resolvedSize);
        if (result == WOTBMOD_OK) {
            __try {
                result = g_audioBackend.load_clip(
                    g_audioBackend.user_data,
                    resolvedPath,
                    &nativeResource);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                RuntimeLog(
                    WOTBMOD_LOG_ERROR,
                    "[%s] audio clip load backend faulted with SEH 0x%08lX",
                    RecordLogId(record),
                    GetExceptionCode());
                result = WOTBMOD_ERROR_CALLBACK_FAULT;
            }
        }
        backendKind = RESOURCE_BACKEND_AUDIO_FILE;
    } else {
        __try {
            result = g_resourceBackend.load(
                g_resourceBackend.user_data,
                &normalized,
                &nativeResource);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] resource load backend faulted with SEH 0x%08lX",
                RecordLogId(record),
                GetExceptionCode());
            result = WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }
    if (result != WOTBMOD_OK || !nativeResource) {
        AbandonResourceSlot(slot);
        return result == WOTBMOD_OK ? WOTBMOD_ERROR_PLATFORM : result;
    }
    return PublishResourceSlot(
        slot,
        nativeResource,
        request->type,
        backendKind,
        outResource);
}

static WotbModResult WOTBMOD_CALL HostResourceReload(
    WotbModHandle handle,
    WotbModResourceHandle resourceHandle) {
    ModRecord* record = RecordFromHandle(handle);
    ResourceHandleRecord* resource =
        ResourceFromHandle(record, resourceHandle);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (resource->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (resource->ownership_kind == RESOURCE_BORROWED_EVENT) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (HasActiveAudioForResource(record, resource)) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (resource->backend_kind == RESOURCE_BACKEND_AUDIO_FILE) {
        if (!g_audioBackend.reload_clip) return WOTBMOD_ERROR_PLATFORM;
        return CallAudioClipBackendOperation(
            g_audioBackend.reload_clip,
            resource->native_resource,
            "reload");
    }
    if (!g_resourceBackend.reload) return WOTBMOD_ERROR_PLATFORM;
    return CallResourceBackendOperation(
        g_resourceBackend.reload, resource->native_resource);
}

static WotbModResult WOTBMOD_CALL HostResourceRelease(
    WotbModHandle handle,
    WotbModResourceHandle resourceHandle) {
    ModRecord* record = RecordFromHandle(handle);
    ResourceHandleRecord* resource =
        ResourceFromHandle(record, resourceHandle);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (resource->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (resource->ownership_kind == RESOURCE_BORROWED_EVENT) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (HasActiveAudioForResource(record, resource)) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (InterlockedCompareExchange(&resource->active, -1, 1) != 1) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    void* nativeResource = resource->native_resource;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    if (resource->backend_kind == RESOURCE_BACKEND_AUDIO_FILE) {
        result = CallAudioClipBackendOperation(
            g_audioBackend.release_clip,
            nativeResource,
            "release");
    } else {
        result = CallResourceBackendOperation(
            g_resourceBackend.release, nativeResource);
    }
    if (result == WOTBMOD_OK) {
        ZeroMemory(resource, sizeof(*resource));
    } else {
        InterlockedExchange(&resource->active, 1);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostResourceClone(
    WotbModHandle handle,
    WotbModResourceHandle resourceHandle,
    WotbModResourceHandle* outResource) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !resourceHandle || !outResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outResource = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    ResourceHandleRecord* resource =
        ResourceFromHandle(record, resourceHandle);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (resource->active != 1 || !resource->native_resource) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    if (resource->backend_kind != RESOURCE_BACKEND_GENERIC ||
        !g_resourceBackend.clone ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    ResourceHandleRecord* slot = ReserveResourceSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;
    void* clonedNativeResource = nullptr;
    const WotbModResult cloneResult =
        CallResourceBackendClone(
            resource->native_resource,
            &clonedNativeResource);
    if (cloneResult != WOTBMOD_OK) {
        AbandonResourceSlot(slot);
        return cloneResult;
    }
    const WotbModResult publishResult = PublishResourceSlot(
        slot,
        clonedNativeResource,
        resource->type,
        RESOURCE_BACKEND_GENERIC,
        outResource);
    if (publishResult != WOTBMOD_OK) {
        CallResourceBackendOperation(
            g_resourceBackend.release,
            clonedNativeResource);
        AbandonResourceSlot(slot);
    }
    return publishResult;
}

static bool IsValidClientEventMask(uint32_t eventMask) {
    return eventMask != 0 &&
           (eventMask & ~static_cast<uint32_t>(WOTBMOD_EVENT_ALL)) == 0;
}

static void RemoveOwnedEventSubscriptions(ModRecord* record) {
    if (!record) return;
    AcquireSRWLockExclusive(&g_clientEventLock);
    ZeroMemory(
        record->event_subscriptions,
        sizeof(record->event_subscriptions));
    ReleaseSRWLockExclusive(&g_clientEventLock);
    InvalidateBorrowedEventResources(record);
    InvalidateBorrowedEventVehicles(record);
}

static WotbModResult WOTBMOD_CALL HostEventSubscribe(
    WotbModHandle handle,
    uint32_t eventMask,
    WotbModClientEventCallback callback,
    void* userData,
    WotbModEventSubscriptionId* outSubscriptionId) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record ||
        !IsValidClientEventMask(eventMask) ||
        !callback ||
        !outSubscriptionId) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outSubscriptionId = 0;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }

    EventSubscriptionRecord* available = nullptr;
    AcquireSRWLockExclusive(&g_clientEventLock);
    for (uint32_t index = 0;
         index < kMaxEventSubscriptionsPerMod;
         ++index) {
        EventSubscriptionRecord* subscription =
            &record->event_subscriptions[index];
        if (!subscription->active && !available) {
            available = subscription;
        }
    }
    if (!available) {
        ReleaseSRWLockExclusive(&g_clientEventLock);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    WotbModEventSubscriptionId id =
        static_cast<WotbModEventSubscriptionId>(
            InterlockedIncrement64(&g_nextEventSubscriptionId));
    if (id == 0) {
        id = static_cast<WotbModEventSubscriptionId>(
            InterlockedIncrement64(&g_nextEventSubscriptionId));
    }
    available->id = id;
    available->event_mask = eventMask;
    available->callback = callback;
    available->user_data = userData;
    InterlockedExchange(&available->active, 1);
    ReleaseSRWLockExclusive(&g_clientEventLock);
    *outSubscriptionId = id;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostEventUnsubscribe(
    WotbModHandle handle,
    WotbModEventSubscriptionId subscriptionId) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || subscriptionId == 0) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    bool removed = false;
    bool ownedByOtherMod = false;
    AcquireSRWLockExclusive(&g_clientEventLock);
    const LONG count = g_modCount;
    for (LONG modIndex = 0; modIndex < count; ++modIndex) {
        ModRecord* candidate = &g_mods[modIndex];
        for (uint32_t index = 0;
             index < kMaxEventSubscriptionsPerMod;
             ++index) {
            EventSubscriptionRecord* subscription =
                &candidate->event_subscriptions[index];
            if (!subscription->active ||
                subscription->id != subscriptionId) {
                continue;
            }
            if (candidate != record) {
                ownedByOtherMod = true;
            } else {
                ZeroMemory(subscription, sizeof(*subscription));
                removed = true;
            }
            break;
        }
        if (removed || ownedByOtherMod) break;
    }
    ReleaseSRWLockExclusive(&g_clientEventLock);
    if (ownedByOtherMod) return WOTBMOD_ERROR_ACCESS_DENIED;
    return removed ? WOTBMOD_OK : WOTBMOD_ERROR_NOT_FOUND;
}

static void ReleasePendingClientEventResources(
    PendingClientEvent* event) {
    if (!event) return;
    if (event->previous_native_resource &&
        g_resourceBackend.release) {
        CallResourceBackendOperation(
            g_resourceBackend.release,
            event->previous_native_resource);
    }
    if (event->native_resource &&
        g_resourceBackend.release) {
        CallResourceBackendOperation(
            g_resourceBackend.release,
            event->native_resource);
    }
    event->previous_native_resource = nullptr;
    event->native_resource = nullptr;
}

static bool IsCoalescibleClientEvent(
    WotbModClientEventType type) {
    switch (type) {
        case WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED:
        case WOTBMOD_EVENT_RELOAD_STATE_CHANGED:
        case WOTBMOD_EVENT_AMMO_CHANGED:
        case WOTBMOD_EVENT_AIM_TARGET_CHANGED:
        case WOTBMOD_EVENT_CAMERA_MODE_CHANGED:
            return true;
        default:
            return false;
    }
}

static uint32_t RequiredClientEventPayloadSize(
    WotbModClientEventType type) {
    switch (type) {
        case WOTBMOD_EVENT_UI_SCREEN_CHANGED:
        case WOTBMOD_EVENT_SCENE_ACTIVATED:
        case WOTBMOD_EVENT_SCENE_DEACTIVATED:
            return 0u;
        case WOTBMOD_EVENT_UI_INPUT:
            return sizeof(WotbModUiInputEventData);
        case WOTBMOD_EVENT_BATTLE_ENTERED:
        case WOTBMOD_EVENT_BATTLE_STARTED:
        case WOTBMOD_EVENT_BATTLE_ENDED:
        case WOTBMOD_EVENT_BATTLE_LEFT:
            return sizeof(WotbModBattleEventData);
        case WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED:
        case WOTBMOD_EVENT_VEHICLE_SPAWNED:
        case WOTBMOD_EVENT_VEHICLE_DESPAWNED:
        case WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED:
        case WOTBMOD_EVENT_VEHICLE_DESTROYED:
        case WOTBMOD_EVENT_AIM_TARGET_CHANGED:
        case WOTBMOD_EVENT_VEHICLE_SPOTTED:
        case WOTBMOD_EVENT_VEHICLE_UNSPOTTED:
            return sizeof(WotbModVehicleEventData);
        case WOTBMOD_EVENT_SHOT_FIRED:
            return sizeof(WotbModShotEventData);
        case WOTBMOD_EVENT_SHELL_HIT:
            return sizeof(WotbModHitEventData);
        case WOTBMOD_EVENT_VEHICLE_DAMAGED:
            return sizeof(WotbModDamageEventData);
        case WOTBMOD_EVENT_RELOAD_STATE_CHANGED:
            return sizeof(WotbModReloadEventData);
        case WOTBMOD_EVENT_AMMO_CHANGED:
            return sizeof(WotbModAmmoEventData);
        case WOTBMOD_EVENT_CAMERA_MODE_CHANGED:
            return sizeof(WotbModCameraEventData);
        case WOTBMOD_EVENT_VEHICLE_KILLED:
            return sizeof(WotbModVehicleKillEventData);
        default:
            return UINT32_MAX;
    }
}

static bool SameClientEventCoalescingKey(
    const PendingClientEvent& queued,
    const PendingClientEvent& requested) {
    return queued.active &&
           queued.type == requested.type &&
           queued.primary_entity_id == requested.primary_entity_id;
}

static WotbModResult QueueClientEvent(
    WotbModClientEventType type,
    WotbModResourceType resourceType,
    void* previousNativeResource,
    void* nativeResource,
    uint32_t flags,
    uint32_t primaryEntityId,
    uint32_t otherEntityId,
    uint32_t payloadSize,
    const WotbModClientEventPayload* payload) {
    if (!g_initialized) return WOTBMOD_ERROR_DISABLED;
    if ((previousNativeResource || nativeResource) &&
        (!g_resourceBackend.clone || !g_resourceBackend.release)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    if ((static_cast<uint32_t>(type) &
         ~static_cast<uint32_t>(WOTBMOD_EVENT_ALL)) != 0 ||
        static_cast<uint32_t>(type) == 0 ||
        (static_cast<uint32_t>(type) &
         (static_cast<uint32_t>(type) - 1u)) != 0 ||
        payloadSize < RequiredClientEventPayloadSize(type) ||
        payloadSize > sizeof(WotbModClientEventPayload) ||
        (payloadSize != 0 && !payload)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (type == WOTBMOD_EVENT_UI_SCREEN_CHANGED) {
        if (resourceType != WOTBMOD_RESOURCE_UI_CONTROL ||
            (!previousNativeResource && !nativeResource)) {
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    } else if (type == WOTBMOD_EVENT_SCENE_ACTIVATED) {
        if (resourceType != WOTBMOD_RESOURCE_SCENE ||
            !nativeResource) {
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    } else if (type == WOTBMOD_EVENT_SCENE_DEACTIVATED) {
        if (resourceType != WOTBMOD_RESOURCE_SCENE ||
            !previousNativeResource) {
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    } else if (previousNativeResource || nativeResource) {
        if (resourceType < WOTBMOD_RESOURCE_GENERIC ||
            resourceType > WOTBMOD_RESOURCE_AUDIO_CLIP) {
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    }

    PendingClientEvent requested = {};
    requested.type = type;
    requested.resource_type = resourceType;
    requested.flags = flags;
    requested.primary_entity_id = primaryEntityId;
    requested.other_entity_id = otherEntityId;
    requested.payload_size = payloadSize;
    if (payload && payloadSize) {
        memcpy(&requested.payload, payload, payloadSize);
    }
    WotbModResult result = WOTBMOD_OK;
    if (previousNativeResource) {
        result = CallResourceBackendClone(
            previousNativeResource,
            &requested.previous_native_resource);
    }
    if (result == WOTBMOD_OK && nativeResource) {
        result = CallResourceBackendClone(
            nativeResource,
            &requested.native_resource);
    }
    if (result != WOTBMOD_OK) {
        ReleasePendingClientEventResources(&requested);
        return result;
    }

    requested.sequence =
        static_cast<uint64_t>(
            InterlockedIncrement64(&g_nextClientEventSequence));
    requested.active = 1;
    PendingClientEvent* available = nullptr;
    PendingClientEvent displaced = {};
    AcquireSRWLockExclusive(&g_clientEventLock);
    if (IsCoalescibleClientEvent(type)) {
        for (uint32_t index = 0;
             index < kMaxPendingClientEvents;
             ++index) {
            if (SameClientEventCoalescingKey(
                    g_pendingClientEvents[index], requested)) {
                available = &g_pendingClientEvents[index];
                displaced = *available;
                break;
            }
        }
    }
    if (!available) {
    for (uint32_t index = 0;
         index < kMaxPendingClientEvents;
         ++index) {
        if (!g_pendingClientEvents[index].active) {
            available = &g_pendingClientEvents[index];
            break;
        }
    }
    }
    if (available) {
        *available = requested;
    }
    ReleaseSRWLockExclusive(&g_clientEventLock);
    ReleasePendingClientEventResources(&displaced);
    if (!available) {
        ReleasePendingClientEventResources(&requested);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    return WOTBMOD_OK;
}

static bool AudioBackendReady() {
    return g_audioBackend.play && g_audioBackend.release;
}

static bool SoundBackendReady() {
    return g_soundBackend.create && g_soundBackend.release;
}

static WotbModResult CallAudioBackendOperation(
    WotbModRuntimeAudioOperation operation,
    void* nativePlayback,
    const char* operationName) {
    if (!operation || !nativePlayback) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = operation(g_audioBackend.user_data, nativePlayback);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "audio backend %s faulted with SEH 0x%08lX",
            operationName ? operationName : "operation",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static WotbModAudioPlayInfo DefaultAudioPlayInfo() {
    WotbModAudioPlayInfo info = {};
    info.struct_size = sizeof(info);
    info.flags = WOTBMOD_AUDIO_PLAY_NONE;
    info.volume = 1.0f;
    info.pitch = 1.0f;
    info.pan = 0.0f;
    info.min_distance = 1.0f;
    info.max_distance = 100.0f;
    return info;
}

static bool IsFiniteAudioValue(float value) {
    return _finite((double)value) != 0;
}

static bool IsValidAudioPlayInfo(
    const WotbModAudioPlayInfo* info,
    bool allowStartPaused) {
    const size_t requiredSize =
        offsetof(WotbModAudioPlayInfo, max_distance) +
        sizeof(info->max_distance);
    const uint32_t allowedFlags =
        WOTBMOD_AUDIO_PLAY_LOOP |
        WOTBMOD_AUDIO_PLAY_SPATIAL |
        (allowStartPaused ? WOTBMOD_AUDIO_PLAY_START_PAUSED : 0u);
    if (!info ||
        info->struct_size < requiredSize ||
        (info->flags & ~allowedFlags) != 0 ||
        !IsFiniteAudioValue(info->volume) ||
        !IsFiniteAudioValue(info->pitch) ||
        !IsFiniteAudioValue(info->pan) ||
        !IsFiniteAudioValue(info->position_x) ||
        !IsFiniteAudioValue(info->position_y) ||
        !IsFiniteAudioValue(info->position_z) ||
        !IsFiniteAudioValue(info->min_distance) ||
        !IsFiniteAudioValue(info->max_distance)) {
        return false;
    }
    return info->volume >= 0.0f &&
           info->pitch > 0.0f &&
           info->pan >= -1.0f &&
           info->pan <= 1.0f &&
           info->min_distance >= 0.0f &&
           info->max_distance >= info->min_distance;
}

static AudioPlaybackRecord* AudioPlaybackFromHandle(
    ModRecord* record,
    WotbModAudioPlaybackHandle handle) {
    if (!record || !handle) return nullptr;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    for (uint32_t index = 0; index < kMaxAudioPlaybacksPerMod; ++index) {
        AudioPlaybackRecord* playback =
            &record->audio_playbacks[index];
        if (playback->public_id == value) return playback;
    }
    return nullptr;
}

static WotbModResult WOTBMOD_CALL HostAudioPlay(
    WotbModHandle handle,
    WotbModResourceHandle audioClipHandle,
    const WotbModAudioPlayInfo* playInfo,
    WotbModAudioPlaybackHandle* outPlayback) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !audioClipHandle || !outPlayback) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outPlayback = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    ResourceHandleRecord* audioClip =
        ResourceFromHandle(record, audioClipHandle);
    if (!audioClip ||
        audioClip->active != 1 ||
        audioClip->type != WOTBMOD_RESOURCE_AUDIO_CLIP) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!AudioBackendReady()) return WOTBMOD_ERROR_PLATFORM;

    if (playInfo && !IsValidAudioPlayInfo(playInfo, true)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    WotbModAudioPlayInfo normalized =
        playInfo ? *playInfo : DefaultAudioPlayInfo();
    normalized.struct_size = sizeof(normalized);

    AudioPlaybackRecord* slot = nullptr;
    for (uint32_t index = 0; index < kMaxAudioPlaybacksPerMod; ++index) {
        if (InterlockedCompareExchange(
                &record->audio_playbacks[index].active, -1, 0) == 0) {
            slot = &record->audio_playbacks[index];
            break;
        }
    }
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;

    void* nativePlayback = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_audioBackend.play(
            g_audioBackend.user_data,
            audioClip->native_resource,
            &normalized,
            &nativePlayback);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] audio play backend faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativePlayback) {
        ZeroMemory(slot, sizeof(*slot));
        return result == WOTBMOD_OK ? WOTBMOD_ERROR_PLATFORM : result;
    }

    uintptr_t publicId =
        (uintptr_t)(uint32_t)InterlockedIncrement(
            &g_nextAudioPlaybackId);
    if (publicId == 0) {
        publicId =
            (uintptr_t)(uint32_t)InterlockedIncrement(
                &g_nextAudioPlaybackId);
    }
    slot->native_playback = nativePlayback;
    slot->source_resource = audioClip;
    slot->public_id = publicId;
    slot->state =
        (normalized.flags & WOTBMOD_AUDIO_PLAY_START_PAUSED)
            ? WOTBMOD_AUDIO_PAUSED
            : WOTBMOD_AUDIO_PLAYING;
    InterlockedExchange(&slot->active, 1);
    *outPlayback =
        reinterpret_cast<WotbModAudioPlaybackHandle>(publicId);
    return WOTBMOD_OK;
}

static WotbModResult HostAudioSimpleOperation(
    WotbModHandle handle,
    WotbModAudioPlaybackHandle playbackHandle,
    WotbModRuntimeAudioOperation operation,
    WotbModAudioState resultingState,
    const char* operationName,
    bool requireEnabled) {
    ModRecord* record = RecordFromHandle(handle);
    AudioPlaybackRecord* playback =
        AudioPlaybackFromHandle(record, playbackHandle);
    if (!record || !playback) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (playback->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (requireEnabled &&
        (!record->enabled ||
         record->state != WOTBMOD_STATE_ENABLED)) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!operation) return WOTBMOD_ERROR_PLATFORM;
    const WotbModResult result = CallAudioBackendOperation(
        operation, playback->native_playback, operationName);
    if (result == WOTBMOD_OK) {
        InterlockedExchange(
            &playback->state, (LONG)resultingState);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostAudioPause(
    WotbModHandle handle,
    WotbModAudioPlaybackHandle playback) {
    return HostAudioSimpleOperation(
        handle,
        playback,
        g_audioBackend.pause,
        WOTBMOD_AUDIO_PAUSED,
        "pause",
        false);
}

static WotbModResult WOTBMOD_CALL HostAudioResume(
    WotbModHandle handle,
    WotbModAudioPlaybackHandle playback) {
    return HostAudioSimpleOperation(
        handle,
        playback,
        g_audioBackend.resume,
        WOTBMOD_AUDIO_PLAYING,
        "resume",
        true);
}

static WotbModResult WOTBMOD_CALL HostAudioStop(
    WotbModHandle handle,
    WotbModAudioPlaybackHandle playback) {
    return HostAudioSimpleOperation(
        handle,
        playback,
        g_audioBackend.stop,
        WOTBMOD_AUDIO_STOPPED,
        "stop",
        false);
}

static WotbModResult WOTBMOD_CALL HostAudioSetParameters(
    WotbModHandle handle,
    WotbModAudioPlaybackHandle playbackHandle,
    const WotbModAudioPlayInfo* parameters) {
    ModRecord* record = RecordFromHandle(handle);
    AudioPlaybackRecord* playback =
        AudioPlaybackFromHandle(record, playbackHandle);
    if (!record || !playback || !parameters) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (playback->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsValidAudioPlayInfo(parameters, false)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!g_audioBackend.set_parameters) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    WotbModAudioPlayInfo normalized = *parameters;
    normalized.struct_size = sizeof(normalized);
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_audioBackend.set_parameters(
            g_audioBackend.user_data,
            playback->native_playback,
            &normalized);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] audio parameter backend faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostAudioGetState(
    WotbModHandle handle,
    WotbModAudioPlaybackHandle playbackHandle,
    WotbModAudioState* outState) {
    ModRecord* record = RecordFromHandle(handle);
    AudioPlaybackRecord* playback =
        AudioPlaybackFromHandle(record, playbackHandle);
    if (!record || !playback || !outState) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (playback->active != 1) return WOTBMOD_ERROR_NOT_FOUND;

    if (!g_audioBackend.get_state) {
        *outState = (WotbModAudioState)InterlockedCompareExchange(
            &playback->state, 0, 0);
        return WOTBMOD_OK;
    }

    WotbModAudioState backendState = WOTBMOD_AUDIO_STOPPED;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_audioBackend.get_state(
            g_audioBackend.user_data,
            playback->native_playback,
            &backendState);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] audio state backend faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK) return result;
    if (backendState < WOTBMOD_AUDIO_STOPPED ||
        backendState > WOTBMOD_AUDIO_PAUSED) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedExchange(&playback->state, (LONG)backendState);
    *outState = backendState;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostAudioRelease(
    WotbModHandle handle,
    WotbModAudioPlaybackHandle playbackHandle) {
    ModRecord* record = RecordFromHandle(handle);
    AudioPlaybackRecord* playback =
        AudioPlaybackFromHandle(record, playbackHandle);
    if (!record || !playback) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (InterlockedCompareExchange(&playback->active, -1, 1) != 1) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    const WotbModResult result = CallAudioBackendOperation(
        g_audioBackend.release,
        playback->native_playback,
        "release");
    if (result == WOTBMOD_OK) {
        ZeroMemory(playback, sizeof(*playback));
    } else {
        InterlockedExchange(&playback->active, 1);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostAudioClipLoad(
    WotbModHandle handle,
    const char* virtualPath,
    WotbModResourceHandle* outAudioClip) {
    WotbModResourceLoadRequest request = {};
    request.struct_size = sizeof(request);
    request.type = WOTBMOD_RESOURCE_AUDIO_CLIP;
    request.virtual_path = virtualPath;
    return HostResourceLoad(handle, &request, outAudioClip);
}

static SoundEventRecord* SoundEventFromHandle(
    ModRecord* record,
    WotbModSoundEventHandle handle) {
    if (!record || !handle) return nullptr;
    const uintptr_t value = reinterpret_cast<uintptr_t>(handle);
    for (uint32_t index = 0; index < kMaxSoundEventsPerMod; ++index) {
        SoundEventRecord* event = &record->sound_events[index];
        if (event->public_id == value) return event;
    }
    return nullptr;
}

static WotbModResult CallSoundEventOperation(
    WotbModRuntimeSoundEventOperation operation,
    void* nativeEvent,
    const char* operationName) {
    if (!operation || !nativeEvent) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = operation(g_soundBackend.user_data, nativeEvent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend %s faulted with SEH 0x%08lX",
            operationName ? operationName : "operation",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static bool IsValidSoundName(const char* name) {
    if (!name || !name[0]) return false;
    const size_t length = strlen(name);
    return length < WOTBMOD_MAX_SOUND_EVENT_NAME &&
           strchr(name, '\r') == nullptr &&
           strchr(name, '\n') == nullptr;
}

static WotbModResult WOTBMOD_CALL HostSoundEventCreate(
    WotbModHandle handle,
    const char* eventName,
    WotbModSoundEventHandle* outEvent) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !IsValidSoundName(eventName) || !outEvent) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outEvent = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!SoundBackendReady()) return WOTBMOD_ERROR_PLATFORM;

    SoundEventRecord* slot = nullptr;
    for (uint32_t index = 0; index < kMaxSoundEventsPerMod; ++index) {
        if (InterlockedCompareExchange(
                &record->sound_events[index].active, -1, 0) == 0) {
            slot = &record->sound_events[index];
            break;
        }
    }
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;

    void* nativeEvent = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.create(
            g_soundBackend.user_data, eventName, &nativeEvent);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] sound event create faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativeEvent) {
        ZeroMemory(slot, sizeof(*slot));
        return result == WOTBMOD_OK ? WOTBMOD_ERROR_PLATFORM : result;
    }

    uintptr_t publicId =
        (uintptr_t)(uint32_t)InterlockedIncrement(&g_nextSoundEventId);
    if (publicId == 0) {
        publicId =
            (uintptr_t)(uint32_t)InterlockedIncrement(&g_nextSoundEventId);
    }
    slot->native_event = nativeEvent;
    slot->public_id = publicId;
    slot->state = WOTBMOD_AUDIO_STOPPED;
    CopyString(slot->event_name, sizeof(slot->event_name), eventName);
    InterlockedExchange(&slot->active, 1);
    *outEvent = reinterpret_cast<WotbModSoundEventHandle>(publicId);
    return WOTBMOD_OK;
}

static WotbModResult HostSoundSimpleOperation(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    WotbModRuntimeSoundEventOperation operation,
    WotbModAudioState resultingState,
    const char* operationName,
    bool requireEnabled) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record || !event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (requireEnabled &&
        (!record->enabled || record->state != WOTBMOD_STATE_ENABLED)) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!operation) return WOTBMOD_ERROR_PLATFORM;
    const WotbModResult result = CallSoundEventOperation(
        operation, event->native_event, operationName);
    if (result == WOTBMOD_OK) {
        InterlockedExchange(&event->state, (LONG)resultingState);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostSoundEventTrigger(
    WotbModHandle handle,
    WotbModSoundEventHandle event) {
    return HostSoundSimpleOperation(
        handle,
        event,
        g_soundBackend.trigger,
        WOTBMOD_AUDIO_PLAYING,
        "trigger",
        true);
}

static WotbModResult WOTBMOD_CALL HostSoundEventStop(
    WotbModHandle handle,
    WotbModSoundEventHandle event) {
    return HostSoundSimpleOperation(
        handle,
        event,
        g_soundBackend.stop,
        WOTBMOD_AUDIO_STOPPED,
        "stop",
        false);
}

static WotbModResult WOTBMOD_CALL HostSoundEventSetPaused(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    int32_t paused) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record || !event || (paused != 0 && paused != 1)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!g_soundBackend.set_paused) return WOTBMOD_ERROR_PLATFORM;

    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.set_paused(
            g_soundBackend.user_data,
            event->native_event,
            paused);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend set-paused faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result == WOTBMOD_OK) {
        InterlockedExchange(
            &event->state,
            paused ? WOTBMOD_AUDIO_PAUSED : WOTBMOD_AUDIO_PLAYING);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostSoundEventSetVolume(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    float volume) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record || !event || !IsFiniteAudioValue(volume) || volume < 0.0f) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!g_soundBackend.set_volume) return WOTBMOD_ERROR_PLATFORM;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.set_volume(
            g_soundBackend.user_data, event->native_event, volume);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend set-volume faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostSoundEventSetPosition(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    float x,
    float y,
    float z) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record ||
        !event ||
        !IsFiniteAudioValue(x) ||
        !IsFiniteAudioValue(y) ||
        !IsFiniteAudioValue(z)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!g_soundBackend.set_position) return WOTBMOD_ERROR_PLATFORM;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.set_position(
            g_soundBackend.user_data, event->native_event, x, y, z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend set-position faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostSoundEventGetState(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    WotbModAudioState* outState) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record || !event || !outState) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!g_soundBackend.get_state) {
        *outState = (WotbModAudioState)InterlockedCompareExchange(
            &event->state, 0, 0);
        return WOTBMOD_OK;
    }

    WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.get_state(
            g_soundBackend.user_data, event->native_event, &state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend get-state faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK) return result;
    if (state < WOTBMOD_AUDIO_STOPPED || state > WOTBMOD_AUDIO_PAUSED) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedExchange(&event->state, (LONG)state);
    *outState = state;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostSoundEventSetParameter(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    const char* parameterName,
    float value) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record ||
        !event ||
        !IsValidSoundName(parameterName) ||
        !IsFiniteAudioValue(value)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!g_soundBackend.set_parameter) return WOTBMOD_ERROR_PLATFORM;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.set_parameter(
            g_soundBackend.user_data,
            event->native_event,
            parameterName,
            value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend set-parameter faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostSoundEventGetParameter(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    const char* parameterName,
    float* outValue) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record ||
        !event ||
        !IsValidSoundName(parameterName) ||
        !outValue) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!g_soundBackend.get_parameter) return WOTBMOD_ERROR_PLATFORM;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.get_parameter(
            g_soundBackend.user_data,
            event->native_event,
            parameterName,
            outValue);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend get-parameter faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostSoundEventHasParameter(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    const char* parameterName,
    int32_t* outHasParameter) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record ||
        !event ||
        !IsValidSoundName(parameterName) ||
        !outHasParameter) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (!g_soundBackend.has_parameter) return WOTBMOD_ERROR_PLATFORM;
    int32_t hasParameter = 0;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_soundBackend.has_parameter(
            g_soundBackend.user_data,
            event->native_event,
            parameterName,
            &hasParameter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "sound backend has-parameter faulted with SEH 0x%08lX",
            GetExceptionCode());
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result == WOTBMOD_OK) {
        *outHasParameter = hasParameter ? 1 : 0;
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostSoundEventGetName(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle,
    char* buffer,
    uint32_t* inoutSize) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record || !event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (event->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    return CopyPathToCaller(event->event_name, buffer, inoutSize);
}

static WotbModResult WOTBMOD_CALL HostSoundEventRelease(
    WotbModHandle handle,
    WotbModSoundEventHandle eventHandle) {
    ModRecord* record = RecordFromHandle(handle);
    SoundEventRecord* event = SoundEventFromHandle(record, eventHandle);
    if (!record || !event) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (InterlockedCompareExchange(&event->active, -1, 1) != 1) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    const WotbModResult result = CallSoundEventOperation(
        g_soundBackend.release, event->native_event, "release");
    if (result == WOTBMOD_OK) {
        ZeroMemory(event, sizeof(*event));
    } else {
        InterlockedExchange(&event->active, 1);
    }
    return result;
}

static WotbModResult RemoveOwnedSoundEvents(ModRecord* record) {
    if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    WotbModResult firstFailure = WOTBMOD_OK;
    for (uint32_t index = 0; index < kMaxSoundEventsPerMod; ++index) {
        SoundEventRecord* event = &record->sound_events[index];
        if (InterlockedCompareExchange(&event->active, -1, 1) != 1) {
            continue;
        }
        if (g_soundBackend.stop &&
            event->state != WOTBMOD_AUDIO_STOPPED) {
            const WotbModResult stopResult = CallSoundEventOperation(
                g_soundBackend.stop, event->native_event, "cleanup-stop");
            if (stopResult != WOTBMOD_OK && firstFailure == WOTBMOD_OK) {
                firstFailure = stopResult;
            }
        }
        const WotbModResult releaseResult = CallSoundEventOperation(
            g_soundBackend.release, event->native_event, "cleanup-release");
        if (releaseResult == WOTBMOD_OK) {
            ZeroMemory(event, sizeof(*event));
        } else {
            InterlockedExchange(&event->active, 1);
            if (firstFailure == WOTBMOD_OK) firstFailure = releaseResult;
        }
    }
    return firstFailure;
}

static WotbModResult RemoveOwnedAudio(ModRecord* record) {
    if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    WotbModResult firstFailure = WOTBMOD_OK;
    for (uint32_t index = 0; index < kMaxAudioPlaybacksPerMod; ++index) {
        AudioPlaybackRecord* playback =
            &record->audio_playbacks[index];
        if (InterlockedCompareExchange(
                &playback->active, -1, 1) != 1) {
            continue;
        }

        if (g_audioBackend.stop &&
            playback->state != WOTBMOD_AUDIO_STOPPED) {
            const WotbModResult stopResult =
                CallAudioBackendOperation(
                    g_audioBackend.stop,
                    playback->native_playback,
                    "cleanup-stop");
            if (stopResult != WOTBMOD_OK &&
                firstFailure == WOTBMOD_OK) {
                firstFailure = stopResult;
            }
        }
        const WotbModResult releaseResult =
            CallAudioBackendOperation(
                g_audioBackend.release,
                playback->native_playback,
                "cleanup-release");
        if (releaseResult == WOTBMOD_OK) {
            ZeroMemory(playback, sizeof(*playback));
        } else {
            InterlockedExchange(&playback->active, 1);
            if (firstFailure == WOTBMOD_OK) {
                firstFailure = releaseResult;
            }
        }
    }
    return firstFailure;
}

static void RemoveOwnedResources(ModRecord* record) {
    if (!record) return;
    for (uint32_t index = 0; index < kMaxResourcesPerMod; ++index) {
        ResourceHandleRecord* resource = &record->resources[index];
        if (HasActiveAudioForResource(record, resource)) {
            continue;
        }
        if (InterlockedCompareExchange(&resource->active, -1, 1) != 1) {
            continue;
        }
        void* nativeResource = resource->native_resource;
        const ResourceBackendKind backendKind = resource->backend_kind;
        if (nativeResource) {
            if (backendKind == RESOURCE_BACKEND_AUDIO_FILE &&
                g_audioBackend.release_clip) {
                CallAudioClipBackendOperation(
                    g_audioBackend.release_clip,
                    nativeResource,
                    "cleanup-release");
            } else if (
                backendKind == RESOURCE_BACKEND_GENERIC &&
                g_resourceBackend.release) {
                CallResourceBackendOperation(
                    g_resourceBackend.release,
                    nativeResource);
            }
        }
        ZeroMemory(resource, sizeof(*resource));
    }

    bool removedMount = false;
    AcquireSRWLockExclusive(&g_resourceMountLock);
    for (uint32_t index = 0; index < kMaxResourceMounts; ++index) {
        ResourceMountRecord* mount = &g_resourceMounts[index];
        if (mount->active && mount->owner == record) {
            ZeroMemory(mount, sizeof(*mount));
            removedMount = true;
        }
    }
    ReleaseSRWLockExclusive(&g_resourceMountLock);
    if (removedMount) NotifyResourceRegistryChanged();
}

static int32_t WOTBMOD_CALL HostConfigGetInt(
    WotbModHandle handle,
    const char* section,
    const char* key,
    int32_t defaultValue) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key) return defaultValue;
    return (int32_t)GetPrivateProfileIntA(
        section, key, defaultValue, record->config_path);
}

static WotbModResult WOTBMOD_CALL HostConfigSetInt(
    WotbModHandle handle,
    const char* section,
    const char* key,
    int32_t value) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    char text[32] = {};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%ld", (long)value);
    return WritePrivateProfileStringA(
               section, key, text, record->config_path)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

static WotbModResult WOTBMOD_CALL HostConfigGetString(
    WotbModHandle handle,
    const char* section,
    const char* key,
    const char* defaultValue,
    char* buffer,
    uint32_t bufferSize) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key || !buffer || bufferSize == 0) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    char value[32768] = {};
    DWORD copied = GetPrivateProfileStringA(
        section,
        key,
        defaultValue ? defaultValue : "",
        value,
        (DWORD)sizeof(value),
        record->config_path);
    const uint32_t required = (uint32_t)copied + 1u;
    if (copied >= sizeof(value) - 1u || bufferSize < required) {
        const uint32_t copySize =
            bufferSize > 0 && bufferSize - 1u < copied
                ? bufferSize - 1u
                : (uint32_t)copied;
        if (copySize) memcpy(buffer, value, copySize);
        buffer[copySize] = '\0';
        return WOTBMOD_ERROR_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, value, required);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostConfigSetString(
    WotbModHandle handle,
    const char* section,
    const char* key,
    const char* value) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !section || !key || !value) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    return WritePrivateProfileStringA(
               section, key, value, record->config_path)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

static uint32_t WOTBMOD_CALL HostGetModCount() {
    LONG count = g_modCount;
    return count > 0 ? (uint32_t)count : 0u;
}

static WotbModResult WOTBMOD_CALL HostGetModInfo(
    uint32_t index,
    WotbModPublicInfo* outInfo) {
    const LONG count = g_modCount;
    if (!outInfo || index >= (uint32_t)count) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const uint32_t callerSize = outInfo->struct_size;
    if (callerSize < sizeof(uint32_t)) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    ModRecord* record = &g_mods[index];
    SyncPublicState(record);
    WotbModPublicInfo snapshot = record->public_info;
    snapshot.struct_size = sizeof(snapshot);
    const uint32_t copySize =
        callerSize < sizeof(snapshot) ? callerSize : (uint32_t)sizeof(snapshot);
    memcpy(outInfo, &snapshot, copySize);
    return callerSize < sizeof(snapshot)
               ? WOTBMOD_ERROR_BUFFER_TOO_SMALL
               : WOTBMOD_OK;
}

static void SaveEnabledState(const ModRecord* record, bool enabled) {
    if (!record || !record->settings_key[0]) return;
    WritePrivateProfileStringA(
        "mods",
        record->settings_key,
        enabled ? "1" : "0",
        g_settingsPath);
}

static void MarkFaulted(
    ModRecord* record,
    const char* callbackName,
    DWORD exceptionCode) {
    if (!record) return;
    SetCallbackAttribution(record, callbackName);
    UpdateSessionMarker("callback_fault", RecordLogId(record));
    InterlockedIncrement(&record->fault_count);
    InterlockedExchange(&record->enabled, 0);
    InterlockedExchange(&record->state, WOTBMOD_STATE_FAULTED);
    SyncPublicState(record);
    RuntimeLog(
        WOTBMOD_LOG_ERROR,
        "[%s] callback %s faulted with SEH 0x%08lX; mod disabled",
        RecordLogId(record),
        callbackName ? callbackName : "unknown",
        exceptionCode);
    RemoveOwnedEventSubscriptions(record);
    RemoveOwnedMainThreadWork(record);
    RemoveOwnedVehicles(record);
    RemoveOwnedHooks(record);
    RemoveOwnedSoundEvents(record);
    RemoveOwnedAudio(record);
    RemoveOwnedVehicleSkins(record);
    RemoveOwnedResources(record);
}

static bool CallEnable(ModRecord* record) {
    SetCallbackAttribution(record, "on_enable");
    if (record && record->api_generation == 3u) {
        const WotbModV3Result result =
            WotbModV3Runtime_Enable(record->v3_handle);
        if (result == WOTBMOD_V3_OK) return true;
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] V3 on_enable failed (result=%d)",
            RecordLogId(record),
            static_cast<int>(result));
        InterlockedExchange(&record->enabled, 0);
        InterlockedExchange(&record->state, WOTBMOD_STATE_FAULTED);
        SyncPublicState(record);
        return false;
    }
    if (!record || !record->callbacks.on_enable) return true;
    __try {
        record->callbacks.on_enable(&g_hostApi, record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_enable", GetExceptionCode());
        return false;
    }
}

static bool CallDisable(ModRecord* record) {
    SetCallbackAttribution(record, "on_disable");
    if (record && record->api_generation == 3u) {
        const WotbModV3Result result =
            WotbModV3Runtime_Disable(record->v3_handle);
        if (result == WOTBMOD_V3_OK) return true;
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] V3 on_disable failed (result=%d)",
            RecordLogId(record),
            static_cast<int>(result));
        return false;
    }
    if (!record || !record->callbacks.on_disable) return true;
    __try {
        record->callbacks.on_disable(&g_hostApi, record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_disable", GetExceptionCode());
        return false;
    }
}

static bool CallUnload(ModRecord* record) {
    SetCallbackAttribution(record, "on_unload");
    if (record && record->api_generation == 3u) {
        const WotbModV3Result result =
            WotbModV3Runtime_DestroyMod(record->v3_handle);
        if (result == WOTBMOD_V3_OK) {
            record->v3_handle = WOTBMOD_V3_INVALID_HANDLE;
            return true;
        }
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] V3 on_unload failed (result=%d)",
            RecordLogId(record),
            static_cast<int>(result));
        return false;
    }
    if (!record || !record->callbacks.on_unload) return true;
    __try {
        record->callbacks.on_unload(&g_hostApi, record);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_unload", GetExceptionCode());
        return false;
    }
}

static bool CallFrame(
    ModRecord* record,
    const WotbModFrameInfo* frame) {
    if (record && record->api_generation == 3u) return true;
    if (!record || !record->callbacks.on_frame) return true;
    SetCallbackAttribution(record, "on_frame");
    __try {
        record->callbacks.on_frame(&g_hostApi, record, frame);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        MarkFaulted(record, "on_frame", GetExceptionCode());
        return false;
    }
}

static bool IsFiniteFloat(float value) {
    return _finite(static_cast<double>(value)) != 0;
}

static bool IsValidUiGeometry(
    const WotbModUiControlGeometry* geometry) {
    const size_t requiredSize =
        offsetof(WotbModUiControlGeometry, height) +
        sizeof(float);
    return geometry &&
           geometry->struct_size >= requiredSize &&
           IsFiniteFloat(geometry->x) &&
           IsFiniteFloat(geometry->y) &&
           IsFiniteFloat(geometry->width) &&
           IsFiniteFloat(geometry->height) &&
           geometry->width >= 0.0f &&
           geometry->height >= 0.0f;
}

static bool IsValidSceneTransform(
    const WotbModSceneTransform* transform) {
    const size_t requiredSize =
        offsetof(WotbModSceneTransform, scale_z) +
        sizeof(float);
    if (!transform || transform->struct_size < requiredSize) return false;
    const float values[] = {
        transform->position_x,
        transform->position_y,
        transform->position_z,
        transform->rotation_x,
        transform->rotation_y,
        transform->rotation_z,
        transform->rotation_w,
        transform->scale_x,
        transform->scale_y,
        transform->scale_z,
    };
    for (size_t index = 0;
         index < sizeof(values) / sizeof(values[0]);
         ++index) {
        if (!IsFiniteFloat(values[index])) return false;
    }
    const bool hasRotation =
        transform->rotation_x != 0.0f ||
        transform->rotation_y != 0.0f ||
        transform->rotation_z != 0.0f ||
        transform->rotation_w != 0.0f;
    return hasRotation &&
           transform->scale_x != 0.0f &&
           transform->scale_y != 0.0f &&
           transform->scale_z != 0.0f;
}

static WotbModResult ValidateObjectResource(
    ModRecord* record,
    WotbModResourceHandle handle,
    WotbModResourceType type) {
    ResourceHandleRecord* resource =
        ResourceFromHandle(record, handle);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (resource->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (resource->backend_kind != RESOURCE_BACKEND_GENERIC) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    return resource->type == type
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_INVALID_ARGUMENT;
}

static WotbModResult ExecuteMainThreadWork(
    const MainThreadWorkItem* work) {
    ModRecord* record = work ? work->owner : nullptr;
    if (!record || record->magic != kModRecordMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }

    if (work->kind == MAIN_THREAD_WORK_CALLBACK) {
        if (!work->callback) return WOTBMOD_ERROR_INVALID_ARGUMENT;
        SetCallbackAttribution(record, "main_thread_callback");
        SetCrashAttribution(
            g_safeModeLastAsync,
            sizeof(g_safeModeLastAsync),
            "main_thread_callback");
        __try {
            work->callback(&g_hostApi, record, work->user_data);
            return WOTBMOD_OK;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            MarkFaulted(
                record,
                "main_thread_callback",
                GetExceptionCode());
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }

    ResourceHandleRecord* first = ResourceFromHandle(
        record, work->first_resource);
    if (!first || first->active != 1 || !first->native_resource) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    ResourceHandleRecord* second = nullptr;
    if (work->second_resource) {
        second = ResourceFromHandle(record, work->second_resource);
        if (!second || second->active != 1 || !second->native_resource) {
            return WOTBMOD_ERROR_NOT_FOUND;
        }
    }

    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        switch (work->kind) {
            case MAIN_THREAD_WORK_UI_SET_GEOMETRY:
                if (first->type != WOTBMOD_RESOURCE_UI_CONTROL ||
                    !g_resourceBackend.ui_set_geometry) {
                    break;
                }
                result = g_resourceBackend.ui_set_geometry(
                    g_resourceBackend.user_data,
                    first->native_resource,
                    &work->geometry);
                break;
            case MAIN_THREAD_WORK_UI_SET_VISIBLE:
                if (first->type != WOTBMOD_RESOURCE_UI_CONTROL ||
                    !g_resourceBackend.ui_set_visible) {
                    break;
                }
                result = g_resourceBackend.ui_set_visible(
                    g_resourceBackend.user_data,
                    first->native_resource,
                    work->int_value);
                break;
            case MAIN_THREAD_WORK_UI_SET_INPUT_ENABLED:
            case MAIN_THREAD_WORK_UI_SET_DISABLED: {
                if (first->type != WOTBMOD_RESOURCE_UI_CONTROL) {
                    break;
                }
                WotbModRuntimeUiSetFlag operation =
                    work->kind ==
                            MAIN_THREAD_WORK_UI_SET_INPUT_ENABLED
                        ? g_resourceBackend.ui_set_input_enabled
                        : g_resourceBackend.ui_set_disabled;
                if (!operation) break;
                result = operation(
                    g_resourceBackend.user_data,
                    first->native_resource,
                    work->int_value,
                    work->second_int_value);
                break;
            }
            case MAIN_THREAD_WORK_UI_ADD_CHILD:
            case MAIN_THREAD_WORK_UI_REMOVE_CHILD: {
                if (!second ||
                    first->type != WOTBMOD_RESOURCE_UI_CONTROL ||
                    second->type != WOTBMOD_RESOURCE_UI_CONTROL) {
                    break;
                }
                WotbModRuntimeResourcePairOperation operation =
                    work->kind == MAIN_THREAD_WORK_UI_ADD_CHILD
                        ? g_resourceBackend.ui_add_child
                        : g_resourceBackend.ui_remove_child;
                if (!operation) break;
                result = operation(
                    g_resourceBackend.user_data,
                    first->native_resource,
                    second->native_resource);
                break;
            }
            case MAIN_THREAD_WORK_SCENE_SET_TRANSFORM:
                if (first->type != WOTBMOD_RESOURCE_SCENE ||
                    !g_resourceBackend.scene_set_transform) {
                    break;
                }
                result = g_resourceBackend.scene_set_transform(
                    g_resourceBackend.user_data,
                    first->native_resource,
                    &work->transform);
                break;
            case MAIN_THREAD_WORK_SCENE_ADD_CHILD:
            case MAIN_THREAD_WORK_SCENE_REMOVE_CHILD: {
                if (!second ||
                    first->type != WOTBMOD_RESOURCE_SCENE ||
                    second->type != WOTBMOD_RESOURCE_SCENE) {
                    break;
                }
                WotbModRuntimeResourcePairOperation operation =
                    work->kind == MAIN_THREAD_WORK_SCENE_ADD_CHILD
                        ? g_resourceBackend.scene_add_child
                        : g_resourceBackend.scene_remove_child;
                if (!operation) break;
                result = operation(
                    g_resourceBackend.user_data,
                    first->native_resource,
                    second->native_resource);
                break;
            }
            default:
                result = WOTBMOD_ERROR_INVALID_ARGUMENT;
                break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] main-thread resource backend faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    return result;
}

static bool IsDispatchThread() {
    const LONG threadId =
        InterlockedCompareExchange(&g_dispatchThreadId, 0, 0);
    return threadId != 0 &&
           static_cast<DWORD>(threadId) == GetCurrentThreadId();
}

static WotbModResult PublishSynchronousResource(
    ModRecord* record,
    WotbModResourceType type,
    WotbModRuntimeResourceCreate create,
    const char* operationName,
    WotbModResourceHandle* outResource) {
    if (!record || !create || !outResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    ResourceHandleRecord* slot = ReserveResourceSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;

    void* nativeResource = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = create(
            g_resourceBackend.user_data,
            &nativeResource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] resource backend %s faulted with SEH 0x%08lX",
            RecordLogId(record),
            operationName ? operationName : "create",
            GetExceptionCode());
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativeResource) {
        AbandonResourceSlot(slot);
        return result == WOTBMOD_OK ? WOTBMOD_ERROR_PLATFORM : result;
    }

    result = PublishResourceSlot(
        slot,
        nativeResource,
        type,
        RESOURCE_BACKEND_GENERIC,
        outResource);
    if (result != WOTBMOD_OK) {
        CallResourceBackendOperation(
            g_resourceBackend.release, nativeResource);
        AbandonResourceSlot(slot);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostUiControlCreate(
    WotbModHandle handle,
    const WotbModUiControlGeometry* geometry,
    WotbModResourceHandle* outControl) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !outControl ||
        (geometry && !IsValidUiGeometry(geometry))) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outControl = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsDispatchThread()) return WOTBMOD_ERROR_WRONG_THREAD;
    if (!g_resourceBackend.ui_create ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    ResourceHandleRecord* slot = ReserveResourceSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;
    void* nativeResource = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_resourceBackend.ui_create(
            g_resourceBackend.user_data,
            geometry,
            &nativeResource);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] resource backend ui_create faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativeResource) {
        AbandonResourceSlot(slot);
        return result == WOTBMOD_OK ? WOTBMOD_ERROR_PLATFORM : result;
    }
    result = PublishResourceSlot(
        slot,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL,
        RESOURCE_BACKEND_GENERIC,
        outControl);
    if (result != WOTBMOD_OK) {
        CallResourceBackendOperation(
            g_resourceBackend.release, nativeResource);
        AbandonResourceSlot(slot);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostUiGetActiveScreen(
    WotbModHandle handle,
    WotbModResourceHandle* outScreen) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !outScreen) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outScreen = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsDispatchThread()) return WOTBMOD_ERROR_WRONG_THREAD;
    if (!g_resourceBackend.ui_get_active_screen ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    return PublishSynchronousResource(
        record,
        WOTBMOD_RESOURCE_UI_CONTROL,
        g_resourceBackend.ui_get_active_screen,
        "ui_get_active_screen",
        outScreen);
}

static WotbModResult WOTBMOD_CALL HostSceneEntityCreate(
    WotbModHandle handle,
    WotbModResourceHandle* outEntity) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !outEntity) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outEntity = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsDispatchThread()) return WOTBMOD_ERROR_WRONG_THREAD;
    if (!g_resourceBackend.scene_entity_create ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    return PublishSynchronousResource(
        record,
        WOTBMOD_RESOURCE_SCENE,
        g_resourceBackend.scene_entity_create,
        "scene_entity_create",
        outEntity);
}

static WotbModResult WOTBMOD_CALL HostSceneGetActive(
    WotbModHandle handle,
    WotbModResourceHandle* outScene) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !outScene) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outScene = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsDispatchThread()) return WOTBMOD_ERROR_WRONG_THREAD;
    if (!g_resourceBackend.scene_get_active ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    return PublishSynchronousResource(
        record,
        WOTBMOD_RESOURCE_SCENE,
        g_resourceBackend.scene_get_active,
        "scene_get_active",
        outScene);
}

static WotbModResult ScheduleOrExecuteObjectWork(
    const MainThreadWorkItem* work) {
    return IsDispatchThread()
               ? ExecuteMainThreadWork(work)
               : EnqueueMainThreadWork(work);
}

static WotbModResult WOTBMOD_CALL HostMainThreadEnqueue(
    WotbModHandle handle,
    WotbModMainThreadCallback callback,
    void* userData) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !callback) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    MainThreadWorkItem work = {};
    work.owner = record;
    work.kind = MAIN_THREAD_WORK_CALLBACK;
    work.callback = callback;
    work.user_data = userData;
    return EnqueueMainThreadWork(&work);
}

static WotbModResult WOTBMOD_CALL HostUiControlSetGeometry(
    WotbModHandle handle,
    WotbModResourceHandle control,
    const WotbModUiControlGeometry* geometry) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !IsValidUiGeometry(geometry)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    WotbModResult validation = ValidateObjectResource(
        record, control, WOTBMOD_RESOURCE_UI_CONTROL);
    if (validation != WOTBMOD_OK) return validation;
    if (!g_resourceBackend.ui_set_geometry) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    MainThreadWorkItem work = {};
    work.owner = record;
    work.kind = MAIN_THREAD_WORK_UI_SET_GEOMETRY;
    work.first_resource = control;
    work.geometry = *geometry;
    work.geometry.struct_size = sizeof(work.geometry);
    return ScheduleOrExecuteObjectWork(&work);
}

static WotbModResult WOTBMOD_CALL HostUiControlSetVisible(
    WotbModHandle handle,
    WotbModResourceHandle control,
    int32_t visible) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    WotbModResult validation = ValidateObjectResource(
        record, control, WOTBMOD_RESOURCE_UI_CONTROL);
    if (validation != WOTBMOD_OK) return validation;
    if (!g_resourceBackend.ui_set_visible) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    MainThreadWorkItem work = {};
    work.owner = record;
    work.kind = MAIN_THREAD_WORK_UI_SET_VISIBLE;
    work.first_resource = control;
    work.int_value = visible != 0;
    return ScheduleOrExecuteObjectWork(&work);
}

static WotbModResult HostUiControlSetFlag(
    WotbModHandle handle,
    WotbModResourceHandle control,
    int32_t value,
    int32_t hierarchical,
    MainThreadWorkKind kind) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    WotbModResult validation = ValidateObjectResource(
        record, control, WOTBMOD_RESOURCE_UI_CONTROL);
    if (validation != WOTBMOD_OK) return validation;
    WotbModRuntimeUiSetFlag operation =
        kind == MAIN_THREAD_WORK_UI_SET_INPUT_ENABLED
            ? g_resourceBackend.ui_set_input_enabled
            : g_resourceBackend.ui_set_disabled;
    if (!operation) return WOTBMOD_ERROR_PLATFORM;

    MainThreadWorkItem work = {};
    work.owner = record;
    work.kind = kind;
    work.first_resource = control;
    work.int_value = value != 0;
    work.second_int_value = hierarchical != 0;
    return ScheduleOrExecuteObjectWork(&work);
}

static WotbModResult WOTBMOD_CALL HostUiControlSetInputEnabled(
    WotbModHandle handle,
    WotbModResourceHandle control,
    int32_t enabled,
    int32_t hierarchical) {
    return HostUiControlSetFlag(
        handle,
        control,
        enabled,
        hierarchical,
        MAIN_THREAD_WORK_UI_SET_INPUT_ENABLED);
}

static WotbModResult WOTBMOD_CALL HostUiControlSetDisabled(
    WotbModHandle handle,
    WotbModResourceHandle control,
    int32_t disabled,
    int32_t hierarchical) {
    return HostUiControlSetFlag(
        handle,
        control,
        disabled,
        hierarchical,
        MAIN_THREAD_WORK_UI_SET_DISABLED);
}

static WotbModResult HostUiControlPair(
    WotbModHandle handle,
    WotbModResourceHandle parent,
    WotbModResourceHandle child,
    MainThreadWorkKind kind) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !parent || !child || parent == child) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    WotbModResult validation = ValidateObjectResource(
        record, parent, WOTBMOD_RESOURCE_UI_CONTROL);
    if (validation != WOTBMOD_OK) return validation;
    validation = ValidateObjectResource(
        record, child, WOTBMOD_RESOURCE_UI_CONTROL);
    if (validation != WOTBMOD_OK) return validation;
    WotbModRuntimeResourcePairOperation operation =
        kind == MAIN_THREAD_WORK_UI_ADD_CHILD
            ? g_resourceBackend.ui_add_child
            : g_resourceBackend.ui_remove_child;
    if (!operation) return WOTBMOD_ERROR_PLATFORM;

    MainThreadWorkItem work = {};
    work.owner = record;
    work.kind = kind;
    work.first_resource = parent;
    work.second_resource = child;
    return ScheduleOrExecuteObjectWork(&work);
}

static WotbModResult WOTBMOD_CALL HostUiControlAddChild(
    WotbModHandle handle,
    WotbModResourceHandle parent,
    WotbModResourceHandle child) {
    return HostUiControlPair(
        handle, parent, child, MAIN_THREAD_WORK_UI_ADD_CHILD);
}

static WotbModResult WOTBMOD_CALL HostUiControlRemoveChild(
    WotbModHandle handle,
    WotbModResourceHandle parent,
    WotbModResourceHandle child) {
    return HostUiControlPair(
        handle, parent, child, MAIN_THREAD_WORK_UI_REMOVE_CHILD);
}

static WotbModResult WOTBMOD_CALL HostSceneSetTransform(
    WotbModHandle handle,
    WotbModResourceHandle scene,
    const WotbModSceneTransform* transform) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !IsValidSceneTransform(transform)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    WotbModResult validation = ValidateObjectResource(
        record, scene, WOTBMOD_RESOURCE_SCENE);
    if (validation != WOTBMOD_OK) return validation;
    if (!g_resourceBackend.scene_set_transform) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    MainThreadWorkItem work = {};
    work.owner = record;
    work.kind = MAIN_THREAD_WORK_SCENE_SET_TRANSFORM;
    work.first_resource = scene;
    work.transform = *transform;
    work.transform.struct_size = sizeof(work.transform);
    return ScheduleOrExecuteObjectWork(&work);
}

static WotbModResult HostScenePair(
    WotbModHandle handle,
    WotbModResourceHandle parent,
    WotbModResourceHandle child,
    MainThreadWorkKind kind) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !parent || !child || parent == child) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    WotbModResult validation = ValidateObjectResource(
        record, parent, WOTBMOD_RESOURCE_SCENE);
    if (validation != WOTBMOD_OK) return validation;
    validation = ValidateObjectResource(
        record, child, WOTBMOD_RESOURCE_SCENE);
    if (validation != WOTBMOD_OK) return validation;
    WotbModRuntimeResourcePairOperation operation =
        kind == MAIN_THREAD_WORK_SCENE_ADD_CHILD
            ? g_resourceBackend.scene_add_child
            : g_resourceBackend.scene_remove_child;
    if (!operation) return WOTBMOD_ERROR_PLATFORM;

    MainThreadWorkItem work = {};
    work.owner = record;
    work.kind = kind;
    work.first_resource = parent;
    work.second_resource = child;
    return ScheduleOrExecuteObjectWork(&work);
}

static WotbModResult WOTBMOD_CALL HostSceneAddChild(
    WotbModHandle handle,
    WotbModResourceHandle parent,
    WotbModResourceHandle child) {
    return HostScenePair(
        handle, parent, child, MAIN_THREAD_WORK_SCENE_ADD_CHILD);
}

static WotbModResult WOTBMOD_CALL HostSceneRemoveChild(
    WotbModHandle handle,
    WotbModResourceHandle parent,
    WotbModResourceHandle child) {
    return HostScenePair(
        handle, parent, child, MAIN_THREAD_WORK_SCENE_REMOVE_CHILD);
}

static WotbModResult PublishUiQueryResult(
    ModRecord* record,
    void* nativeResource,
    WotbModResourceHandle* outControl) {
    if (!record || !nativeResource || !outControl) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    ResourceHandleRecord* slot = ReserveResourceSlot(record);
    if (!slot) {
        CallResourceBackendOperation(
            g_resourceBackend.release, nativeResource);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    const WotbModResult result = PublishResourceSlot(
        slot,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL,
        RESOURCE_BACKEND_GENERIC,
        outControl);
    if (result != WOTBMOD_OK) {
        CallResourceBackendOperation(
            g_resourceBackend.release, nativeResource);
        AbandonResourceSlot(slot);
    }
    return result;
}

static WotbModResult ValidateUiQuery(
    WotbModHandle handle,
    WotbModResourceHandle control,
    ModRecord** outRecord,
    ResourceHandleRecord** outResource) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !outRecord || !outResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsDispatchThread()) return WOTBMOD_ERROR_WRONG_THREAD;
    WotbModResult validation = ValidateObjectResource(
        record, control, WOTBMOD_RESOURCE_UI_CONTROL);
    if (validation != WOTBMOD_OK) return validation;
    ResourceHandleRecord* resource =
        ResourceFromHandle(record, control);
    if (!resource || !resource->native_resource) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    *outRecord = record;
    *outResource = resource;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostUiControlFindByName(
    WotbModHandle handle,
    WotbModResourceHandle root,
    const char* name,
    int32_t recursive,
    WotbModResourceHandle* outControl) {
    if (!name || !name[0] || !outControl) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outControl = nullptr;
    ModRecord* record = nullptr;
    ResourceHandleRecord* resource = nullptr;
    WotbModResult result =
        ValidateUiQuery(handle, root, &record, &resource);
    if (result != WOTBMOD_OK) return result;
    if (!g_resourceBackend.ui_find_by_name ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void* nativeResult = nullptr;
    __try {
        result = g_resourceBackend.ui_find_by_name(
            g_resourceBackend.user_data,
            resource->native_resource,
            name,
            recursive != 0,
            &nativeResult);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativeResult) {
        return result == WOTBMOD_OK
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : result;
    }
    return PublishUiQueryResult(record, nativeResult, outControl);
}

static WotbModResult WOTBMOD_CALL HostUiControlGetParent(
    WotbModHandle handle,
    WotbModResourceHandle control,
    WotbModResourceHandle* outParent) {
    if (!outParent) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outParent = nullptr;
    ModRecord* record = nullptr;
    ResourceHandleRecord* resource = nullptr;
    WotbModResult result =
        ValidateUiQuery(handle, control, &record, &resource);
    if (result != WOTBMOD_OK) return result;
    if (!g_resourceBackend.ui_get_parent ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void* nativeResult = nullptr;
    __try {
        result = g_resourceBackend.ui_get_parent(
            g_resourceBackend.user_data,
            resource->native_resource,
            &nativeResult);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativeResult) {
        return result == WOTBMOD_OK
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : result;
    }
    return PublishUiQueryResult(record, nativeResult, outParent);
}

static WotbModResult WOTBMOD_CALL HostUiControlGetChildCount(
    WotbModHandle handle,
    WotbModResourceHandle control,
    uint32_t* outCount) {
    if (!outCount) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outCount = 0;
    ModRecord* record = nullptr;
    ResourceHandleRecord* resource = nullptr;
    WotbModResult result =
        ValidateUiQuery(handle, control, &record, &resource);
    if (result != WOTBMOD_OK) return result;
    if (!g_resourceBackend.ui_get_child_count) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_resourceBackend.ui_get_child_count(
            g_resourceBackend.user_data,
            resource->native_resource,
            outCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL HostUiControlGetChildAt(
    WotbModHandle handle,
    WotbModResourceHandle control,
    uint32_t index,
    WotbModResourceHandle* outChild) {
    if (!outChild) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outChild = nullptr;
    ModRecord* record = nullptr;
    ResourceHandleRecord* resource = nullptr;
    WotbModResult result =
        ValidateUiQuery(handle, control, &record, &resource);
    if (result != WOTBMOD_OK) return result;
    if (!g_resourceBackend.ui_get_child_at ||
        !g_resourceBackend.release) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    void* nativeResult = nullptr;
    __try {
        result = g_resourceBackend.ui_get_child_at(
            g_resourceBackend.user_data,
            resource->native_resource,
            index,
            &nativeResult);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !nativeResult) {
        return result == WOTBMOD_OK
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : result;
    }
    return PublishUiQueryResult(record, nativeResult, outChild);
}

static WotbModResult WOTBMOD_CALL HostUiControlGetState(
    WotbModHandle handle,
    WotbModResourceHandle control,
    WotbModUiControlState* outState) {
    const size_t requiredSize =
        offsetof(WotbModUiControlState, flags) +
        sizeof(outState->flags);
    if (!outState || outState->struct_size < requiredSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    ModRecord* record = nullptr;
    ResourceHandleRecord* resource = nullptr;
    WotbModResult result =
        ValidateUiQuery(handle, control, &record, &resource);
    if (result != WOTBMOD_OK) return result;
    if (!g_resourceBackend.ui_get_state) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    const uint32_t callerSize = outState->struct_size;
    WotbModUiControlState state = {};
    state.struct_size = sizeof(state);
    state.geometry.struct_size = sizeof(state.geometry);
    __try {
        result = g_resourceBackend.ui_get_state(
            g_resourceBackend.user_data,
            resource->native_resource,
            &state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK) return result;
    const uint32_t stateSize = static_cast<uint32_t>(sizeof(state));
    const uint32_t copySize =
        callerSize < stateSize ? callerSize : stateSize;
    memcpy(outState, &state, copySize);
    return callerSize < stateSize
               ? WOTBMOD_ERROR_BUFFER_TOO_SMALL
               : WOTBMOD_OK;
}

typedef WotbModResult(WOTBMOD_CALL* VehicleFactoryCall)(
    void* userData,
    void** outToken);

static WotbModResult PublishVehicleFactory(
    ModRecord* record,
    VehicleFactoryCall factory,
    WotbModVehicleHandle* outVehicle) {
    if (!record || !factory || !outVehicle) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    VehicleHandleRecord* slot = ReserveVehicleSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;
    void* token = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = factory(
            g_gameplayBackend.user_data, &token);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !token) {
        AbandonVehicleSlot(slot);
        return result == WOTBMOD_OK
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : result;
    }
    return PublishVehicleSlot(slot, token, outVehicle);
}

static WotbModResult ValidateVehicleFactory(
    WotbModHandle handle,
    WotbModVehicleHandle* outVehicle,
    ModRecord** outRecord) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !outVehicle || !outRecord) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outVehicle = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsDispatchThread()) return WOTBMOD_ERROR_WRONG_THREAD;
    if (!GameplayBackendReady()) return WOTBMOD_ERROR_PLATFORM;
    *outRecord = record;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL HostVehicleGetLocal(
    WotbModHandle handle,
    WotbModVehicleHandle* outVehicle) {
    ModRecord* record = nullptr;
    WotbModResult result =
        ValidateVehicleFactory(handle, outVehicle, &record);
    if (result != WOTBMOD_OK) return result;
    if (!g_gameplayBackend.vehicle_get_local) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    return PublishVehicleFactory(
        record,
        g_gameplayBackend.vehicle_get_local,
        outVehicle);
}

static WotbModResult WOTBMOD_CALL HostVehicleGetByEntityId(
    WotbModHandle handle,
    uint32_t entityId,
    WotbModVehicleHandle* outVehicle) {
    ModRecord* record = nullptr;
    WotbModResult result =
        ValidateVehicleFactory(handle, outVehicle, &record);
    if (result != WOTBMOD_OK) return result;
    if (entityId == 0 ||
        !g_gameplayBackend.vehicle_get_by_entity_id) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    VehicleHandleRecord* slot = ReserveVehicleSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;
    void* token = nullptr;
    __try {
        result = g_gameplayBackend.vehicle_get_by_entity_id(
            g_gameplayBackend.user_data,
            entityId,
            &token);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !token) {
        AbandonVehicleSlot(slot);
        return result == WOTBMOD_OK
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : result;
    }
    return PublishVehicleSlot(slot, token, outVehicle);
}

static WotbModResult WOTBMOD_CALL HostVehicleGetCount(
    WotbModHandle handle,
    uint32_t* outCount) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !outCount) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outCount = 0;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    if (!IsDispatchThread()) return WOTBMOD_ERROR_WRONG_THREAD;
    if (!g_gameplayBackend.vehicle_get_count) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        return g_gameplayBackend.vehicle_get_count(
            g_gameplayBackend.user_data, outCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL HostVehicleGetAt(
    WotbModHandle handle,
    uint32_t index,
    WotbModVehicleHandle* outVehicle) {
    ModRecord* record = nullptr;
    WotbModResult result =
        ValidateVehicleFactory(handle, outVehicle, &record);
    if (result != WOTBMOD_OK) return result;
    if (!g_gameplayBackend.vehicle_get_at) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    VehicleHandleRecord* slot = ReserveVehicleSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;
    void* token = nullptr;
    __try {
        result = g_gameplayBackend.vehicle_get_at(
            g_gameplayBackend.user_data, index, &token);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !token) {
        AbandonVehicleSlot(slot);
        return result == WOTBMOD_OK
                   ? WOTBMOD_ERROR_NOT_FOUND
                   : result;
    }
    return PublishVehicleSlot(slot, token, outVehicle);
}

static WotbModResult WOTBMOD_CALL HostVehicleClone(
    WotbModHandle handle,
    WotbModVehicleHandle vehicleHandle,
    WotbModVehicleHandle* outVehicle) {
    ModRecord* record = RecordFromHandle(handle);
    if (!record || !vehicleHandle || !outVehicle) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outVehicle = nullptr;
    if (!record->enabled || record->state != WOTBMOD_STATE_ENABLED) {
        return WOTBMOD_ERROR_DISABLED;
    }
    VehicleHandleRecord* vehicle =
        VehicleFromHandle(record, vehicleHandle);
    if (!vehicle) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (vehicle->active != 1 || !vehicle->token) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    if (!g_gameplayBackend.vehicle_clone) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    VehicleHandleRecord* slot = ReserveVehicleSlot(record);
    if (!slot) return WOTBMOD_ERROR_LIMIT_REACHED;
    void* token = nullptr;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_gameplayBackend.vehicle_clone(
            g_gameplayBackend.user_data,
            vehicle->token,
            &token);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK || !token) {
        AbandonVehicleSlot(slot);
        return result == WOTBMOD_OK
                   ? WOTBMOD_ERROR_PLATFORM
                   : result;
    }
    return PublishVehicleSlot(slot, token, outVehicle);
}

static WotbModResult WOTBMOD_CALL HostVehicleRelease(
    WotbModHandle handle,
    WotbModVehicleHandle vehicleHandle) {
    ModRecord* record = RecordFromHandle(handle);
    VehicleHandleRecord* vehicle =
        VehicleFromHandle(record, vehicleHandle);
    if (!record || !vehicle) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (vehicle->active != 1) return WOTBMOD_ERROR_NOT_FOUND;
    if (vehicle->ownership_kind == VEHICLE_BORROWED_EVENT) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (InterlockedCompareExchange(
            &vehicle->active, -1, 1) != 1) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    WotbModResult result = CallGameplayRelease(vehicle->token);
    if (result == WOTBMOD_OK) {
        ZeroMemory(vehicle, sizeof(*vehicle));
    } else {
        InterlockedExchange(&vehicle->active, 1);
    }
    return result;
}

static WotbModResult WOTBMOD_CALL HostVehicleGetInfo(
    WotbModHandle handle,
    WotbModVehicleHandle vehicleHandle,
    WotbModVehicleInfo* outInfo) {
    ModRecord* record = RecordFromHandle(handle);
    VehicleHandleRecord* vehicle =
        VehicleFromHandle(record, vehicleHandle);
    const size_t requiredSize =
        offsetof(WotbModVehicleInfo, health) +
        sizeof(outInfo->health);
    if (!record || !vehicle || !outInfo ||
        outInfo->struct_size < requiredSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (vehicle->active != 1 || !vehicle->token) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    if (!g_gameplayBackend.vehicle_get_info) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    const uint32_t callerSize = outInfo->struct_size;
    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;
    __try {
        result = g_gameplayBackend.vehicle_get_info(
            g_gameplayBackend.user_data,
            vehicle->token,
            &info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (result != WOTBMOD_OK) return result;
    const uint32_t infoSize = static_cast<uint32_t>(sizeof(info));
    const uint32_t copySize =
        callerSize < infoSize ? callerSize : infoSize;
    memcpy(outInfo, &info, copySize);
    return callerSize < infoSize
               ? WOTBMOD_ERROR_BUFFER_TOO_SMALL
               : WOTBMOD_OK;
}

static bool CallClientEvent(
    ModRecord* record,
    const EventSubscriptionRecord* subscription,
    const PendingClientEvent* pending) {
    if (!record ||
        !subscription ||
        !subscription->callback ||
        !pending) {
        return false;
    }
    if (!record->enabled ||
        record->state != WOTBMOD_STATE_ENABLED) {
        return false;
    }

    InvalidateBorrowedEventResources(record);
    InvalidateBorrowedEventVehicles(record);
    WotbModClientEvent event = {};
    event.struct_size = sizeof(event);
    event.type = static_cast<uint32_t>(pending->type);
    event.sequence = pending->sequence;
    if (pending->previous_native_resource) {
        event.previous_resource = PublishBorrowedEventResource(
            &record->borrowed_event_resources[0],
            pending->previous_native_resource,
            pending->resource_type);
    }
    if (pending->native_resource) {
        event.resource = PublishBorrowedEventResource(
            &record->borrowed_event_resources[1],
            pending->native_resource,
            pending->resource_type);
    }
    event.flags = pending->flags;
    event.payload_size = pending->payload_size;
    if (pending->payload_size) {
        memcpy(
            &event.payload,
            &pending->payload,
            pending->payload_size);
    }
    if (GameplayBackendReady() &&
        pending->primary_entity_id != 0) {
        void* token = nullptr;
        WotbModResult result = WOTBMOD_ERROR_PLATFORM;
        __try {
            result = g_gameplayBackend.vehicle_get_by_entity_id(
                g_gameplayBackend.user_data,
                pending->primary_entity_id,
                &token);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result = WOTBMOD_ERROR_CALLBACK_FAULT;
        }
        if (result == WOTBMOD_OK && token) {
            event.vehicle = PublishBorrowedEventVehicle(
                &record->borrowed_event_vehicles[0], token);
            if (!event.vehicle) CallGameplayRelease(token);
        }
    }
    if (GameplayBackendReady() &&
        pending->other_entity_id != 0) {
        void* token = nullptr;
        WotbModResult result = WOTBMOD_ERROR_PLATFORM;
        __try {
            result = g_gameplayBackend.vehicle_get_by_entity_id(
                g_gameplayBackend.user_data,
                pending->other_entity_id,
                &token);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result = WOTBMOD_ERROR_CALLBACK_FAULT;
        }
        if (result == WOTBMOD_OK && token) {
            event.other_vehicle = PublishBorrowedEventVehicle(
                &record->borrowed_event_vehicles[1], token);
            if (!event.other_vehicle) CallGameplayRelease(token);
        }
    }

    bool callbackOk = true;
    DWORD exceptionCode = 0;
    SetCallbackAttribution(record, "client_event");
    _snprintf_s(
        g_safeModeLastEvent,
        sizeof(g_safeModeLastEvent),
        _TRUNCATE,
        "legacy_event_%u",
        static_cast<unsigned int>(event.type));
    __try {
        subscription->callback(
            &g_hostApi,
            record,
            &event,
            subscription->user_data);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        callbackOk = false;
        exceptionCode = GetExceptionCode();
    }
    InvalidateBorrowedEventResources(record);
    InvalidateBorrowedEventVehicles(record);
    if (!callbackOk) {
        MarkFaulted(record, "client_event", exceptionCode);
    }
    return callbackOk;
}

static int __cdecl ComparePendingClientEvents(
    const void* leftValue,
    const void* rightValue) {
    const PendingClientEvent* left =
        static_cast<const PendingClientEvent*>(leftValue);
    const PendingClientEvent* right =
        static_cast<const PendingClientEvent*>(rightValue);
    if (left->sequence < right->sequence) return -1;
    if (left->sequence > right->sequence) return 1;
    return 0;
}

static const char* V3TopicForClientEvent(
    WotbModClientEventType type) {
    switch (type) {
        case WOTBMOD_EVENT_UI_SCREEN_CHANGED:
            return WOTBMOD_V3_EVENT_UI_SCREEN_CHANGED;
        case WOTBMOD_EVENT_SCENE_ACTIVATED:
            return WOTBMOD_V3_EVENT_SCENE_ACTIVATED;
        case WOTBMOD_EVENT_SCENE_DEACTIVATED:
            return WOTBMOD_V3_EVENT_SCENE_DEACTIVATED;
        case WOTBMOD_EVENT_UI_INPUT:
            return WOTBMOD_V3_EVENT_UI_INPUT;
        case WOTBMOD_EVENT_BATTLE_ENTERED:
            return WOTBMOD_V3_EVENT_BATTLE_ENTERED;
        case WOTBMOD_EVENT_BATTLE_STARTED:
            return WOTBMOD_V3_EVENT_BATTLE_STARTED;
        case WOTBMOD_EVENT_BATTLE_ENDED:
            return WOTBMOD_V3_EVENT_BATTLE_ENDED;
        case WOTBMOD_EVENT_BATTLE_LEFT:
            return WOTBMOD_V3_EVENT_BATTLE_LEFT;
        case WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED:
            return WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED;
        case WOTBMOD_EVENT_VEHICLE_SPAWNED:
            return WOTBMOD_V3_EVENT_VEHICLE_SPAWNED;
        case WOTBMOD_EVENT_VEHICLE_DESPAWNED:
            return WOTBMOD_V3_EVENT_VEHICLE_DESPAWNED;
        case WOTBMOD_EVENT_SHOT_FIRED:
            return WOTBMOD_V3_EVENT_SHOT_FIRED;
        case WOTBMOD_EVENT_SHELL_HIT:
            return WOTBMOD_V3_EVENT_SHELL_HIT;
        case WOTBMOD_EVENT_VEHICLE_HEALTH_CHANGED:
            return WOTBMOD_V3_EVENT_VEHICLE_HEALTH_CHANGED;
        case WOTBMOD_EVENT_VEHICLE_DAMAGED:
            return WOTBMOD_V3_EVENT_VEHICLE_DAMAGED;
        case WOTBMOD_EVENT_VEHICLE_DESTROYED:
            return WOTBMOD_V3_EVENT_VEHICLE_DESTROYED;
        case WOTBMOD_EVENT_RELOAD_STATE_CHANGED:
            return WOTBMOD_V3_EVENT_RELOAD_STATE_CHANGED;
        case WOTBMOD_EVENT_AMMO_CHANGED:
            return WOTBMOD_V3_EVENT_AMMO_CHANGED;
        case WOTBMOD_EVENT_AIM_TARGET_CHANGED:
            return WOTBMOD_V3_EVENT_AIM_TARGET_CHANGED;
        case WOTBMOD_EVENT_VEHICLE_SPOTTED:
            return WOTBMOD_V3_EVENT_VEHICLE_SPOTTED;
        case WOTBMOD_EVENT_VEHICLE_UNSPOTTED:
            return WOTBMOD_V3_EVENT_VEHICLE_UNSPOTTED;
        case WOTBMOD_EVENT_CAMERA_MODE_CHANGED:
            return WOTBMOD_V3_EVENT_CAMERA_MODE_CHANGED;
        case WOTBMOD_EVENT_VEHICLE_KILLED:
            return WOTBMOD_V3_EVENT_VEHICLE_KILLED;
        default:
            return nullptr;
    }
}

static void PublishV3ClientEvent(
    const PendingClientEvent* pending) {
    if (!pending) return;
    const char* topic = V3TopicForClientEvent(pending->type);
    if (!topic) return;

    uint64_t boundaryContext = WOTBMOD_V3_CONTEXT_NONE;
    const char* boundaryName = nullptr;
    switch (pending->type) {
        case WOTBMOD_EVENT_BATTLE_ENTERED:
        case WOTBMOD_EVENT_BATTLE_STARTED:
            boundaryContext = WOTBMOD_V3_CONTEXT_BATTLE;
            boundaryName = "BATTLE";
            break;
        case WOTBMOD_EVENT_BATTLE_LEFT:
            boundaryContext = WOTBMOD_V3_CONTEXT_HANGAR;
            boundaryName = "HANGAR";
            break;
        default:
            break;
    }
    if (boundaryContext != WOTBMOD_V3_CONTEXT_NONE) {
        const WotbModV3Result contextResult =
            WotbModV3Runtime_SetContext(boundaryContext);
        if (contextResult == WOTBMOD_V3_OK) {
            RuntimeLog(
                WOTBMOD_LOG_INFO,
                "V3 context changed to %s from client event 0x%08X",
                boundaryName,
                static_cast<unsigned int>(pending->type));
        } else {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "V3 context change to %s failed for client event 0x%08X "
                "(result=%u)",
                boundaryName,
                static_cast<unsigned int>(pending->type),
                static_cast<unsigned int>(contextResult));
        }
    }

    static_assert(
        sizeof(WotbModV3ClientEventData) >=
            sizeof(WotbModClientEventPayload),
        "V3 client event bridge payload must hold every legacy payload");
    WotbModV3ClientEventEnvelope envelope = {};
    envelope.struct_size = sizeof(envelope);
    envelope.api_version =
        WOTBMOD_V3_CLIENT_EVENT_ENVELOPE_VERSION;
    envelope.type = static_cast<uint32_t>(pending->type);
    envelope.flags = pending->flags;
    envelope.sequence = pending->sequence;
    envelope.primary_entity_id = pending->primary_entity_id;
    envelope.other_entity_id = pending->other_entity_id;
    envelope.resource_type =
        static_cast<uint32_t>(pending->resource_type);
    envelope.has_previous_resource =
        pending->previous_native_resource ? 1u : 0u;
    envelope.has_resource =
        pending->native_resource ? 1u : 0u;
    envelope.payload_size = pending->payload_size;
    if (pending->payload_size != 0u) {
        const uint32_t capacity =
            static_cast<uint32_t>(sizeof(envelope.payload));
        const uint32_t copySize =
            pending->payload_size < capacity
                ? pending->payload_size
                : capacity;
        memcpy(
            &envelope.payload,
            &pending->payload,
            copySize);
        envelope.payload_size = copySize;
    }
    wotbmod::v3::PublishSystemEvent(
        topic,
        &envelope,
        sizeof(envelope),
        IsCoalescibleClientEvent(pending->type)
            ? WOTBMOD_V3_EVENT_FLAG_COALESCIBLE
            : WOTBMOD_V3_EVENT_FLAG_NONE);

    const uint32_t derivedFlags =
        IsCoalescibleClientEvent(pending->type)
            ? WOTBMOD_V3_EVENT_FLAG_COALESCIBLE
            : WOTBMOD_V3_EVENT_FLAG_NONE;
    const bool hasTypedPayload =
        pending->payload_size >=
        RequiredClientEventPayloadSize(pending->type);
    if (hasTypedPayload &&
        pending->type == WOTBMOD_EVENT_LOCAL_VEHICLE_CHANGED) {
        g_v3LocalVehicleId = envelope.payload.vehicle.entity_id;
        if (g_v3LocalVehicleId != 0u) {
            wotbmod::v3::PublishSystemEvent(
                WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CREATED,
                &envelope,
                sizeof(envelope),
                WOTBMOD_V3_EVENT_FLAG_NONE);
        }
    } else if (
        hasTypedPayload &&
        pending->type == WOTBMOD_EVENT_VEHICLE_DAMAGED &&
        g_v3LocalVehicleId != 0u &&
        pending->primary_entity_id == g_v3LocalVehicleId) {
        wotbmod::v3::PublishSystemEvent(
            WOTBMOD_V3_EVENT_DAMAGE_RECEIVED,
            &envelope,
            sizeof(envelope),
            derivedFlags);
    } else if (
        hasTypedPayload &&
        pending->type == WOTBMOD_EVENT_VEHICLE_DESTROYED &&
        g_v3LocalVehicleId != 0u &&
        pending->primary_entity_id == g_v3LocalVehicleId) {
        wotbmod::v3::PublishSystemEvent(
            WOTBMOD_V3_EVENT_LOCAL_VEHICLE_DESTROYED,
            &envelope,
            sizeof(envelope),
            WOTBMOD_V3_EVENT_FLAG_NONE);
    } else if (
        hasTypedPayload &&
        pending->type == WOTBMOD_EVENT_CAMERA_MODE_CHANGED) {
        const WotbModV3CameraEventData& camera = envelope.payload.camera;
        if (camera.previous_mode != camera.mode) {
            if (camera.previous_mode == WOTBMOD_V3_CAMERA_MODE_SNIPER) {
                wotbmod::v3::PublishSystemEvent(
                    WOTBMOD_V3_EVENT_SNIPER_EXITED,
                    &envelope,
                    sizeof(envelope),
                    WOTBMOD_V3_EVENT_FLAG_NONE);
            }
            if (camera.mode == WOTBMOD_V3_CAMERA_MODE_SNIPER) {
                wotbmod::v3::PublishSystemEvent(
                    WOTBMOD_V3_EVENT_SNIPER_ENTERED,
                    &envelope,
                    sizeof(envelope),
                    WOTBMOD_V3_EVENT_FLAG_NONE);
            }
        }
    }
    if (pending->type == WOTBMOD_EVENT_BATTLE_LEFT) {
        g_v3LocalVehicleId = 0u;
    }
}

static void DrainClientEvents() {
    PendingClientEvent pending[kMaxPendingClientEvents] = {};
    uint32_t pendingCount = 0;
    AcquireSRWLockExclusive(&g_clientEventLock);
    for (uint32_t index = 0;
         index < kMaxPendingClientEvents;
         ++index) {
        PendingClientEvent* event = &g_pendingClientEvents[index];
        if (!event->active) continue;
        pending[pendingCount++] = *event;
        ZeroMemory(event, sizeof(*event));
    }
    ReleaseSRWLockExclusive(&g_clientEventLock);

    qsort(
        pending,
        pendingCount,
        sizeof(pending[0]),
        &ComparePendingClientEvents);
    const WotbModEventSubscriptionId maximumSubscriptionId =
        static_cast<WotbModEventSubscriptionId>(
            InterlockedCompareExchange64(
                &g_nextEventSubscriptionId,
                0,
                0));
    for (uint32_t eventIndex = 0;
         eventIndex < pendingCount;
         ++eventIndex) {
        PendingClientEvent* event = &pending[eventIndex];
        PublishV3ClientEvent(event);
        const LONG modCount = g_modCount;
        for (LONG modIndex = 0;
             modIndex < modCount;
             ++modIndex) {
            ModRecord* record = &g_mods[modIndex];
            for (uint32_t subscriptionIndex = 0;
                 subscriptionIndex <
                     kMaxEventSubscriptionsPerMod;
                 ++subscriptionIndex) {
                EventSubscriptionRecord subscription = {};
                AcquireSRWLockShared(&g_clientEventLock);
                const EventSubscriptionRecord* candidate =
                    &record->event_subscriptions[
                        subscriptionIndex];
                if (candidate->active &&
                    candidate->id <= maximumSubscriptionId &&
                    (candidate->event_mask &
                     static_cast<uint32_t>(event->type)) != 0) {
                    subscription = *candidate;
                }
                ReleaseSRWLockShared(&g_clientEventLock);
                if (!subscription.active) continue;
                CallClientEvent(record, &subscription, event);
            }
        }
        ReleasePendingClientEventResources(event);
        ZeroMemory(event, sizeof(*event));
    }
}

static void ClearPendingClientEvents() {
    PendingClientEvent pending[kMaxPendingClientEvents] = {};
    uint32_t pendingCount = 0;
    AcquireSRWLockExclusive(&g_clientEventLock);
    for (uint32_t index = 0;
         index < kMaxPendingClientEvents;
         ++index) {
        PendingClientEvent* event = &g_pendingClientEvents[index];
        if (!event->active) continue;
        pending[pendingCount++] = *event;
        ZeroMemory(event, sizeof(*event));
    }
    ReleaseSRWLockExclusive(&g_clientEventLock);
    for (uint32_t index = 0; index < pendingCount; ++index) {
        ReleasePendingClientEventResources(&pending[index]);
    }
}

static int __cdecl CompareMainThreadWork(
    const void* leftValue,
    const void* rightValue) {
    const MainThreadWorkItem* left =
        static_cast<const MainThreadWorkItem*>(leftValue);
    const MainThreadWorkItem* right =
        static_cast<const MainThreadWorkItem*>(rightValue);
    if (left->sequence < right->sequence) return -1;
    if (left->sequence > right->sequence) return 1;
    return 0;
}

static void DrainMainThreadWork() {
    MainThreadWorkItem pending[kMaxMainThreadWorkItems] = {};
    uint32_t pendingCount = 0;
    AcquireSRWLockExclusive(&g_mainThreadWorkLock);
    for (uint32_t index = 0; index < kMaxMainThreadWorkItems; ++index) {
        MainThreadWorkItem* work = &g_mainThreadWork[index];
        if (!work->active) continue;
        pending[pendingCount++] = *work;
        ZeroMemory(work, sizeof(*work));
    }
    ReleaseSRWLockExclusive(&g_mainThreadWorkLock);

    qsort(
        pending,
        pendingCount,
        sizeof(pending[0]),
        &CompareMainThreadWork);
    for (uint32_t index = 0; index < pendingCount; ++index) {
        const WotbModResult result =
            ExecuteMainThreadWork(&pending[index]);
        if (result != WOTBMOD_OK &&
            result != WOTBMOD_ERROR_DISABLED) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] queued main-thread work kind=%d failed (result=%d)",
                RecordLogId(pending[index].owner),
                static_cast<int>(pending[index].kind),
                static_cast<int>(result));
        }
    }
}

static WotbModV3Result EnsureV3PackageMounted(ModRecord* record) {
    if (!record || record->api_generation != 3u ||
        !record->v3_package_root[0]) {
        return WOTBMOD_V3_OK;
    }
    if (record->v3_package_mount != WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_OK;
    }
    WotbModV3Handle mount = WOTBMOD_V3_INVALID_HANDLE;
    const WotbModV3Result result =
        wotbmod::v3::MountVfsPackageForRuntime(
            record->v3_handle,
            record->v3_package_provider,
            record->v3_package_root,
            0,
            &mount);
    if (result == WOTBMOD_V3_OK) {
        record->v3_package_mount = mount;
        return WOTBMOD_V3_OK;
    }
    /*
     * A disable request issued from inside a callback can leave the original
     * owner handle alive while the legacy facade has already transitioned.
     * The provider is still valid in that narrow case, so a duplicate mount
     * is equivalent to success and must not make re-enable destructive.
     */
    if (result == WOTBMOD_V3_E_ALREADY_EXISTS) {
        return WOTBMOD_V3_OK;
    }
    RuntimeLog(
        WOTBMOD_LOG_ERROR,
        "[%s] package root mount failed (result=%d root=%s)",
        RecordLogId(record),
        static_cast<int>(result),
        record->v3_package_root);
    return result;
}

/*
 * Mounts a v3 record's package with that record's enable-transition window
 * held open, and closes the window on every way out of this function.
 *
 * Why the window has to exist: mounting creates handles owned by the mod,
 * and CreateOwnedHandle refuses an owner that is not accepting callbacks.
 * Only WotbModV3Runtime_Enable sets that, reached from CallEnable further
 * down SetRecordEnabled, and the mount has to run first because on_enable
 * reads the package it mounts. On first load the record is still
 * MOD_STATE_LOADED and CreateOwnedHandle's loading exemption covers it; on
 * re-enable it is MOD_STATE_DISABLED and covered by nothing, so the mount
 * could never succeed. That is the bug this closes.
 *
 * __finally rather than a scope guard, and that is a fact about this build
 * rather than a matter of taste. Everything here compiles /EHsc, under
 * which MSVC does not run C++ destructors while unwinding to an __except
 * -- measured on this toolchain, not assumed. Mod callbacks run inside
 * __try/__except(EXCEPTION_EXECUTE_HANDLER) blocks that mark the mod
 * faulted and let the process live on (see DispatchClientEvent and its
 * siblings), and HostSetModEnabled is in g_hostApi, which every mod holds.
 * So a mod can call set_mod_enabled from its own callback and put this
 * function underneath one of those handlers; an access violation in the
 * mount -- filesystem and path handling over a package root is not an
 * exotic place for one -- would then skip a destructor and leave the
 * window pinned open forever, on the thread most mod code runs on. That is
 * precisely the standing exemption for a disabled record this whole
 * mechanism exists to prevent. __finally is a termination handler and does
 * run during SEH unwind, so it closes on that path too.
 *
 * There is deliberately no second clearing mechanism in MarkFaulted or in
 * Disable. One close, three lines below its open, that fires on every
 * abnormal exit, beats a list of cleanup sites that has to be kept in step
 * with every fault path added later -- and that would miss any fault not
 * routed through those two functions.
 *
 * Non-v3 records and records with no v3 handle need no window: nothing on
 * their path reaches CreateOwnedHandle.
 */
static WotbModV3Result MountV3PackageDuringEnable(ModRecord* record) {
    if (!record || record->api_generation != 3u ||
        record->v3_handle == WOTBMOD_V3_INVALID_HANDLE) {
        return EnsureV3PackageMounted(record);
    }
    /*
     * Read out before the guarded region so the __finally below touches
     * only a local. If the fault that brought us there was a bad `record`,
     * dereferencing it again during the unwind would fault a second time,
     * and a fault inside an unwind kills the process outright.
     */
    const WotbModV3Handle modHandle = record->v3_handle;
    const WotbModV3Result opened =
        wotbmod::v3::BeginModEnableTransition(modHandle);
    if (opened != WOTBMOD_V3_OK) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] enable transition refused (result=%d)",
            RecordLogId(record),
            static_cast<int>(opened));
        return opened;
    }
    WotbModV3Result mounted = WOTBMOD_V3_OK;
    __try {
        mounted = EnsureV3PackageMounted(record);
    } __finally {
        wotbmod::v3::EndModEnableTransition(modHandle);
    }
    return mounted;
}

static WotbModResult SetRecordEnabled(ModRecord* record, bool enabled) {
    if (!record) return WOTBMOD_ERROR_NOT_FOUND;
    if (record->state == WOTBMOD_STATE_FAULTED) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }

    if (enabled) {
        const LONG safeMode =
            InterlockedCompareExchange(&g_safeMode, 0, 0);
        if (safeMode == 1 ||
            (safeMode == 2 && !record->v3_content_descriptor[0]) ||
            IsAutoDisabledMod(RecordLogId(record))) {
            RuntimeLog(
                WOTBMOD_LOG_WARNING,
                "[%s] enable blocked by %s",
                RecordLogId(record),
                IsAutoDisabledMod(RecordLogId(record))
                    ? "repeated-crash auto-disable marker"
                    : "crash-loop safe mode");
            return WOTBMOD_ERROR_DISABLED;
        }
        if (record->enabled) return WOTBMOD_OK;
        UpdateSessionMarker("enabling_mod", RecordLogId(record));
        /*
         * The enable-transition window opens and closes entirely inside
         * this call. Past this point the record is enabled and CallEnable
         * makes it accept callbacks for real, so the exemption is neither
         * needed nor still open.
         */
        const WotbModV3Result packageMount =
            MountV3PackageDuringEnable(record);
        if (packageMount != WOTBMOD_V3_OK) {
            return V3ResultToLegacy(packageMount);
        }
        InterlockedExchange(&record->enabled, 1);
        InterlockedExchange(&record->state, WOTBMOD_STATE_ENABLED);
        SyncPublicState(record);
        if (!CallEnable(record)) {
            if (record->api_generation == 3u) {
                record->v3_package_mount =
                    WOTBMOD_V3_INVALID_HANDLE;
            }
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
        if (record->v3_content_descriptor[0] &&
            record->v3_content_apply_result !=
                static_cast<LONG>(WOTBMOD_V3_OK)) {
            const WotbModV3Result contentResult =
                static_cast<WotbModV3Result>(
                    record->v3_content_apply_result);
            WotbModV3Runtime_Disable(record->v3_handle);
            record->v3_package_mount =
                WOTBMOD_V3_INVALID_HANDLE;
            record->v3_content_handle =
                WOTBMOD_V3_INVALID_HANDLE;
            InterlockedExchange(&record->enabled, 0);
            InterlockedExchange(
                &record->state,
                WOTBMOD_STATE_DISABLED);
            SyncPublicState(record);
            return V3ResultToLegacy(contentResult);
        }
        SaveEnabledState(record, true);
        UpdateSessionMarker("running", RecordLogId(record));
        RuntimeLog(WOTBMOD_LOG_INFO, "[%s] enabled", RecordLogId(record));
        return WOTBMOD_OK;
    }

    if (!record->enabled) return WOTBMOD_OK;
    UpdateSessionMarker("disabling_mod", RecordLogId(record));
    InterlockedExchange(&record->enabled, 0);
    InterlockedExchange(&record->state, WOTBMOD_STATE_DISABLED);
    SyncPublicState(record);
    RemoveOwnedEventSubscriptions(record);
    RemoveOwnedMainThreadWork(record);
    const bool callbackOk = CallDisable(record);
    if (record->api_generation == 3u) {
        record->v3_package_mount = WOTBMOD_V3_INVALID_HANDLE;
    }
    RemoveOwnedVehicles(record);
    const WotbModResult hookCleanup = RemoveOwnedHooks(record);
    const WotbModResult soundCleanup = RemoveOwnedSoundEvents(record);
    const WotbModResult audioCleanup = RemoveOwnedAudio(record);
    RemoveOwnedVehicleSkins(record);
    RemoveOwnedResources(record);
    if (record->state != WOTBMOD_STATE_FAULTED) {
        InterlockedExchange(&record->state, WOTBMOD_STATE_DISABLED);
        SyncPublicState(record);
    }
    SaveEnabledState(record, false);
    UpdateSessionMarker("running", RecordLogId(record));
    RuntimeLog(WOTBMOD_LOG_INFO, "[%s] disabled", RecordLogId(record));
    if (!callbackOk) return WOTBMOD_ERROR_CALLBACK_FAULT;
    if (hookCleanup != WOTBMOD_OK) return hookCleanup;
    if (soundCleanup != WOTBMOD_OK) return soundCleanup;
    return audioCleanup;
}

static WotbModResult WOTBMOD_CALL HostSetModEnabled(
    const char* id,
    int32_t enabled) {
    if (!id || !id[0]) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (enabled &&
        (InterlockedCompareExchange(&g_safeMode, 0, 0) == 1 ||
         IsAutoDisabledMod(id))) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "[%s] enable request blocked by crash-loop safe mode",
            id);
        return WOTBMOD_ERROR_DISABLED;
    }
    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        if (_stricmp(g_mods[index].public_info.id, id) == 0) {
            return SetRecordEnabled(&g_mods[index], enabled != 0);
        }
    }
    return WOTBMOD_ERROR_NOT_FOUND;
}

WotbModHostApi g_hostApi = {
    sizeof(WotbModHostApi),
    WOTBMOD_ABI_VERSION,
    WOTBMOD_HOST_VERSION,
    0u,
    &HostLog,
    &HostGetPath,
    &HostGetGameModule,
    &HostResolveRva,
    &HostGetProcAddress,
    &HostFindPattern,
    &HostHookCreate,
    &HostHookEnable,
    &HostHookDisable,
    &HostHookRemove,
    &HostConfigGetInt,
    &HostConfigSetInt,
    &HostConfigGetString,
    &HostConfigSetString,
    &HostGetModCount,
    &HostGetModInfo,
    &HostSetModEnabled,
    &HostResourceMount,
    &HostResourceUnmount,
    &HostResourceResolve,
    &HostResourceLoad,
    &HostResourceReload,
    &HostResourceRelease,
    &HostAudioPlay,
    &HostAudioPause,
    &HostAudioResume,
    &HostAudioStop,
    &HostAudioSetParameters,
    &HostAudioGetState,
    &HostAudioRelease,
    &HostAudioClipLoad,
    &HostSoundEventCreate,
    &HostSoundEventTrigger,
    &HostSoundEventStop,
    &HostSoundEventSetPaused,
    &HostSoundEventSetVolume,
    &HostSoundEventSetPosition,
    &HostSoundEventGetState,
    &HostSoundEventSetParameter,
    &HostSoundEventGetParameter,
    &HostSoundEventHasParameter,
    &HostSoundEventGetName,
    &HostSoundEventRelease,
    &HostMainThreadEnqueue,
    &HostUiControlSetGeometry,
    &HostUiControlSetVisible,
    &HostUiControlAddChild,
    &HostUiControlRemoveChild,
    &HostSceneSetTransform,
    &HostSceneAddChild,
    &HostSceneRemoveChild,
    &HostUiControlCreate,
    &HostUiGetActiveScreen,
    &HostSceneEntityCreate,
    &HostSceneGetActive,
    &HostResourceClone,
    &HostEventSubscribe,
    &HostEventUnsubscribe,
    &HostUiControlFindByName,
    &HostUiControlGetParent,
    &HostUiControlGetChildCount,
    &HostUiControlGetChildAt,
    &HostUiControlGetState,
    &HostUiControlSetInputEnabled,
    &HostUiControlSetDisabled,
    &HostVehicleGetLocal,
    &HostVehicleGetByEntityId,
    &HostVehicleGetCount,
    &HostVehicleGetAt,
    &HostVehicleClone,
    &HostVehicleRelease,
    &HostVehicleGetInfo,
    &HostVehicleSkinRegister,
    &HostVehicleSkinSetEnabled,
    &HostVehicleSkinGetInfo,
    &HostVehicleSkinRelease};

static bool IsValidModId(const char* id) {
    if (!id || !id[0]) return false;
    const size_t length = strlen(id);
    if (length >= WOTBMOD_MAX_ID) return false;
    for (size_t index = 0; index < length; ++index) {
        const char c = id[index];
        const bool valid =
            (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-';
        if (!valid) return false;
    }
    return true;
}

static bool IsDuplicateModId(const char* id) {
    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        if (_stricmp(g_mods[index].public_info.id, id) == 0) return true;
    }
    return false;
}

static void GetFileStem(
    const char* fileName,
    char* output,
    size_t outputCapacity) {
    CopyString(output, outputCapacity, fileName);
    char* dot = strrchr(output, '.');
    if (dot) *dot = '\0';
}

struct PackageLoadContext {
    const char* expected_id;
    const char* expected_version;
    const char* package_root;
    uint32_t granted_permission_tier;
    const wotbmod::v3::PackagePlanEntry* plan;
};

static bool PackageHasPermission(
    const wotbmod::v3::PackagePlanEntry& package,
    const char* permission) {
    if (!permission || !permission[0]) return false;
    for (uint32_t index = 0u;
         index < package.permission_count;
         ++index) {
        if (_stricmp(package.permissions[index], permission) == 0) {
            return true;
        }
    }
    return false;
}

const wotbmod::v3::PackagePlanEntry*
    g_pendingContentPackage = nullptr;

static ModRecord* FindRecordByV3Handle(
    WotbModV3Handle mod) {
    if (mod == WOTBMOD_V3_INVALID_HANDLE) return nullptr;
    for (uint32_t index = 0u; index < kMaxMods; ++index) {
        ModRecord* record = &g_mods[index];
        if (record->magic == kModRecordMagic &&
            record->v3_handle == mod) {
            return record;
        }
    }
    return nullptr;
}

static void WOTBMOD_V3_CALL ContentPackageEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    ModRecord* record = FindRecordByV3Handle(mod);
    if (!record || !bootstrap ||
        !bootstrap->query_interface ||
        !record->v3_content_descriptor[0]) {
        if (record) {
            InterlockedExchange(
                &record->v3_content_apply_result,
                static_cast<LONG>(
                    WOTBMOD_V3_E_INVALID_ARGUMENT));
        }
        return;
    }
    InterlockedExchange(
        &record->v3_content_apply_result,
        static_cast<LONG>(WOTBMOD_V3_E_PLATFORM));
    const void* rawContent = nullptr;
    WotbModV3Result result =
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_CONTENT,
            WOTBMOD_V3_CONTENT_VERSION,
            &rawContent);
    if (result == WOTBMOD_V3_OK && rawContent) {
        const WotbModV3ContentApiV1* content =
            static_cast<
                const WotbModV3ContentApiV1*>(rawContent);
        WotbModV3Handle handle =
            WOTBMOD_V3_INVALID_HANDLE;
        result = content->parse_uri(
            mod,
            record->v3_content_descriptor,
            &handle);
        if (result == WOTBMOD_V3_OK) {
            record->v3_content_handle = handle;
            result = content->validate(mod, handle);
        }
        if (result == WOTBMOD_V3_OK) {
            result = content->apply(mod, handle);
        }
    }
    InterlockedExchange(
        &record->v3_content_apply_result,
        static_cast<LONG>(result));
    if (result != WOTBMOD_V3_OK) {
        WotbModV3ErrorInfo errorInfo = {};
        errorInfo.struct_size = sizeof(errorInfo);
        errorInfo.api_version = WOTBMOD_V3_ABI_VERSION;
        const bool hasError =
            bootstrap->get_last_error &&
            bootstrap->get_last_error(mod, &errorInfo) ==
                WOTBMOD_V3_OK &&
            errorInfo.message[0] != '\0';
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] content descriptor apply failed "
            "(result=%d descriptor=%s reason=%s)",
            RecordLogId(record),
            static_cast<int>(result),
            record->v3_content_descriptor,
            hasError ? errorInfo.message : "unavailable");
    }
}

static void WOTBMOD_V3_CALL ContentPackageDisable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    ModRecord* record = FindRecordByV3Handle(mod);
    if (!record) return;
    const WotbModV3Handle contentHandle =
        record->v3_content_handle;
    record->v3_content_handle =
        WOTBMOD_V3_INVALID_HANDLE;
    if (!bootstrap || !bootstrap->query_interface ||
        contentHandle == WOTBMOD_V3_INVALID_HANDLE) {
        return;
    }
    const void* rawContent = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_CONTENT,
            WOTBMOD_V3_CONTENT_VERSION,
            &rawContent) == WOTBMOD_V3_OK &&
        rawContent) {
        const WotbModV3ContentApiV1* content =
            static_cast<
                const WotbModV3ContentApiV1*>(rawContent);
        const WotbModV3Result result =
            content->unapply(mod, contentHandle);
        if (result != WOTBMOD_V3_OK &&
            result != WOTBMOD_V3_E_INVALID_HANDLE &&
            result != WOTBMOD_V3_E_OBJECT_DESTROYED) {
            RuntimeLog(
                WOTBMOD_LOG_WARNING,
                "[%s] content unapply reported result=%d",
                RecordLogId(record),
                static_cast<int>(result));
        }
    }
}

static WotbModV3Result WOTBMOD_V3_CALL
ContentPackageEntry(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle,
    WotbModV3Info* outInfo) {
    const wotbmod::v3::PackagePlanEntry* package =
        g_pendingContentPackage;
    if (!bootstrap || !outInfo || !package) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    ZeroMemory(outInfo, sizeof(*outInfo));
    outInfo->struct_size = sizeof(*outInfo);
    outInfo->api_version = WOTBMOD_V3_ABI_VERSION;
    outInfo->requested_permission_tier =
        package->requested_permission_tier;
    CopyString(
        outInfo->id,
        sizeof(outInfo->id),
        package->id);
    CopyString(
        outInfo->name,
        sizeof(outInfo->name),
        package->name);
    CopyString(
        outInfo->version,
        sizeof(outInfo->version),
        package->version);
    CopyString(
        outInfo->author,
        sizeof(outInfo->author),
        package->developer);
    CopyString(
        outInfo->description,
        sizeof(outInfo->description),
        "Content-only package");
    outInfo->on_enable = &ContentPackageEnable;
    outInfo->on_disable = &ContentPackageDisable;
    return WOTBMOD_V3_OK;
}

static bool CallModEntry(
    WotbModLoadFn entry,
    ModRecord* record,
    WotbModInfo* outInfo,
    WotbModResult* outResult) {
    __try {
        *outResult = entry(&g_hostApi, record, outInfo);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] WotbModLoad faulted with SEH 0x%08lX",
            RecordLogId(record),
            GetExceptionCode());
        return false;
    }
}

static void ResetFailedRecord(ModRecord* record) {
    if (!record) return;
    if (record->v3_handle != WOTBMOD_V3_INVALID_HANDLE) {
        WotbModV3Runtime_DestroyMod(record->v3_handle);
        record->v3_handle = WOTBMOD_V3_INVALID_HANDLE;
    }
    RemoveOwnedEventSubscriptions(record);
    RemoveOwnedMainThreadWork(record);
    RemoveOwnedVehicles(record);
    RemoveOwnedHooks(record);
    RemoveOwnedSoundEvents(record);
    RemoveOwnedAudio(record);
    RemoveOwnedVehicleSkins(record);
    RemoveOwnedResources(record);
    if (record->module) FreeLibrary(record->module);
    ZeroMemory(record, sizeof(*record));
}

static bool ParentDirectory(
    const char* path,
    char* output,
    size_t outputCapacity);

static bool PackageRelativePath(
    const char* packageRoot,
    const char* physicalPath,
    char* output,
    size_t outputCapacity) {
    if (!packageRoot || !physicalPath || !output ||
        outputCapacity == 0u) {
        return false;
    }
    const size_t rootLength = strlen(packageRoot);
    if (rootLength == 0u ||
        _strnicmp(
            packageRoot,
            physicalPath,
            rootLength) != 0) {
        return false;
    }
    const char* relative = physicalPath + rootLength;
    if (*relative == '\\' || *relative == '/') {
        ++relative;
    } else if (*relative != '\0') {
        return false;
    }
    if (!relative[0] || strlen(relative) >= outputCapacity) {
        return false;
    }
    CopyString(output, outputCapacity, relative);
    for (char* current = output; *current; ++current) {
        if (*current == '\\') *current = '/';
    }
    return strstr(output, "../") == nullptr &&
           strstr(output, "/..") == nullptr &&
           strchr(output, ':') == nullptr;
}

static WotbModV3Result InstallV3PackageMetadata(
    WotbModV3Handle mod,
    const wotbmod::v3::PackagePlanEntry& package,
    const char* packageRoot) {
    if (mod == WOTBMOD_V3_INVALID_HANDLE ||
        !packageRoot || !packageRoot[0] ||
        !package.manifest_path[0]) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    wotbmod::v3::DataManifestPreflightSummary manifest = {};
    manifest.struct_size = sizeof(manifest);
    manifest.api_version =
        WOTBMOD_V3_DATA_PREFLIGHT_VERSION;
    WotbModV3Result result =
        wotbmod::v3::PreflightManifestFile(
            package.manifest_path,
            &manifest);
    if (result != WOTBMOD_V3_OK) return result;
    if (strcmp(manifest.id, package.id) != 0 ||
        strcmp(manifest.version, package.version) != 0 ||
        manifest.package_type != package.package_type ||
        manifest.dependency_count != package.dependency_count ||
        manifest.dependency_count >
            WOTBMOD_V3_RUNTIME_MAX_DEPENDENCIES) {
        return WOTBMOD_V3_E_INCOMPATIBLE;
    }
    result = WotbModV3Runtime_SetPackageRoot(
        mod,
        packageRoot);
    if (result != WOTBMOD_V3_OK) return result;
    return WotbModV3Runtime_SetDependencies(
        mod,
        manifest.dependencies,
        manifest.dependency_count);
}

static WotbModResult LoadContentPackage(
    const wotbmod::v3::PackagePlanEntry& package) {
    if (!IsValidModId(package.id) ||
        !package.version[0] ||
        !package.manifest_path[0] ||
        !package.content_path[0] ||
        package.granted_permission_tier >
            WOTBMOD_V3_PERMISSION_UNSAFE) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    SetCrashAttribution(
        g_safeModeLastResourceTransaction,
        sizeof(g_safeModeLastResourceTransaction),
        "content_descriptor_apply");
    UpdateSessionMarker("loading_content_mod", package.id);
    bool appendedSlot = false;
    const LONG slot = AcquireModSlot(&appendedSlot);
    if (slot < 0) {
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    if (IsDuplicateModId(package.id)) {
        return WOTBMOD_ERROR_ALREADY_EXISTS;
    }

    char packageRoot[MAX_PATH] = {};
    char contentRelative[WOTBMOD_V3_MAX_PATH] = {};
    if (!ParentDirectory(
            package.manifest_path,
            packageRoot,
            sizeof(packageRoot)) ||
        !PackageRelativePath(
            packageRoot,
            package.content_path,
            contentRelative,
            sizeof(contentRelative))) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    ModRecord* record = &g_mods[slot];
    ZeroMemory(record, sizeof(*record));
    record->magic = kModRecordMagic;
    record->state = WOTBMOD_STATE_DISCOVERED;
    record->api_generation = 3u;
    record->v3_content_apply_result =
        static_cast<LONG>(WOTBMOD_V3_E_NOT_SUPPORTED);
    CopyString(
        record->module_path,
        sizeof(record->module_path),
        package.manifest_path);
    const char* fileName =
        strrchr(package.content_path, '\\');
    fileName = fileName
        ? fileName + 1
        : package.content_path;
    CopyString(
        record->module_name,
        sizeof(record->module_name),
        fileName);
    CopyString(
        record->settings_key,
        sizeof(record->settings_key),
        package.id);
    JoinPath(
        record->data_path,
        sizeof(record->data_path),
        g_dataDirectory,
        record->settings_key);
    EnsureDirectory(record->data_path);
    char configName[MAX_PATH] = {};
    _snprintf_s(
        configName,
        sizeof(configName),
        _TRUNCATE,
        "%s.ini",
        record->settings_key);
    JoinPath(
        record->config_path,
        sizeof(record->config_path),
        g_configDirectory,
        configName);

    WotbModV3Result result =
        WotbModV3Runtime_CreateMod(
            package.manifest_path,
            package.granted_permission_tier,
            &record->v3_handle);
    if (result != WOTBMOD_V3_OK) {
        ResetFailedRecord(record);
        return V3ResultToLegacy(result);
    }
    result = InstallV3PackageMetadata(
        record->v3_handle,
        package,
        packageRoot);
    if (result != WOTBMOD_V3_OK) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] content package metadata handoff failed "
            "(result=%d)",
            package.id,
            static_cast<int>(result));
        ResetFailedRecord(record);
        return V3ResultToLegacy(result);
    }
    if (package.permission_count >
        WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS) {
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const char* permissionNames[
        WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS] = {};
    for (uint32_t index = 0u;
         index < package.permission_count; ++index) {
        permissionNames[index] = package.permissions[index];
    }
    result = WotbModV3Runtime_SetPermissionGrants(
        record->v3_handle,
        permissionNames,
        package.permission_count,
        1u);
    if (result != WOTBMOD_V3_OK) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] content package permission grants were rejected "
            "(result=%d)",
            package.id,
            static_cast<int>(result));
        ResetFailedRecord(record);
        return V3ResultToLegacy(result);
    }
    CopyString(
        record->v3_package_root,
        sizeof(record->v3_package_root),
        packageRoot);
    CopyString(
        record->v3_package_provider,
        sizeof(record->v3_package_provider),
        package.id);
    const int descriptorWritten = _snprintf_s(
        record->v3_content_descriptor,
        sizeof(record->v3_content_descriptor),
        _TRUNCATE,
        "mod://self/%s",
        contentRelative);
    if (descriptorWritten < 0) {
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    result = wotbmod::v3::MountVfsPackageForRuntime(
        record->v3_handle,
        record->v3_package_provider,
        record->v3_package_root,
        0,
        &record->v3_package_mount);
    if (result != WOTBMOD_V3_OK) {
        ResetFailedRecord(record);
        return V3ResultToLegacy(result);
    }

    WotbModV3RuntimeModuleInfo info = {};
    info.struct_size = sizeof(info);
    info.api_version = WOTBMOD_V3_ABI_VERSION;
    g_pendingContentPackage = &package;
    result = WotbModV3Runtime_InvokeEntry(
        record->v3_handle,
        &ContentPackageEntry,
        &info);
    g_pendingContentPackage = nullptr;
    if (result != WOTBMOD_V3_OK ||
        strcmp(info.id, package.id) != 0 ||
        strcmp(info.version, package.version) != 0) {
        ResetFailedRecord(record);
        return result == WOTBMOD_V3_OK
            ? WOTBMOD_ERROR_INVALID_ARGUMENT
            : V3ResultToLegacy(result);
    }

    record->public_info.struct_size =
        sizeof(record->public_info);
    record->public_info.abi_version =
        WOTBMOD_V3_ABI_VERSION;
    CopyString(
        record->public_info.id,
        sizeof(record->public_info.id),
        info.id);
    CopyString(
        record->public_info.name,
        sizeof(record->public_info.name),
        info.name);
    CopyString(
        record->public_info.version,
        sizeof(record->public_info.version),
        info.version);
    CopyString(
        record->public_info.author,
        sizeof(record->public_info.author),
        info.author);
    CopyString(
        record->public_info.description,
        sizeof(record->public_info.description),
        info.description);
    CopyString(
        record->public_info.module_path,
        sizeof(record->public_info.module_path),
        package.manifest_path);
    InterlockedExchange(
        &record->state,
        WOTBMOD_STATE_LOADED);
    if (appendedSlot) InterlockedIncrement(&g_modCount);

    const bool enabledByDefault =
        GetPrivateProfileIntA(
            "mods",
            record->settings_key,
            1,
            g_settingsPath) != 0;
    if (enabledByDefault) {
        const WotbModResult enabled =
            SetRecordEnabled(record, true);
        if (enabled != WOTBMOD_OK) {
            return enabled;
        }
    } else {
        InterlockedExchange(&record->enabled, 0);
        InterlockedExchange(
            &record->state,
            WOTBMOD_STATE_DISABLED);
        SyncPublicState(record);
    }
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] loaded content-only package %s v%s "
        "(overlays activate on enable)",
        record->public_info.id,
        record->public_info.name,
        record->public_info.version);
    return WOTBMOD_OK;
}

static WotbModResult LoadOneMod(
    const char* path,
    const PackageLoadContext* package) {
    if (!path || !path[0] || strlen(path) >= MAX_PATH) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "native mod entrypoint path is empty or exceeds MAX_PATH");
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (package &&
        (!IsValidModId(package->expected_id) ||
         !package->expected_version ||
         !package->expected_version[0] ||
         !package->package_root ||
         !package->package_root[0] ||
         strlen(package->package_root) >= MAX_PATH ||
         package->granted_permission_tier >
             WOTBMOD_V3_PERMISSION_UNSAFE ||
         !package->plan ||
         package->plan->permission_count >
             WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS)) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "package load context is invalid for %s",
            path);
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    bool appendedSlot = false;
    const LONG slot = AcquireModSlot(&appendedSlot);
    if (slot < 0) {
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    ModRecord* record = &g_mods[slot];
    ZeroMemory(record, sizeof(*record));
    record->magic = kModRecordMagic;
    record->state = WOTBMOD_STATE_DISCOVERED;
    record->is_package = package ? 1 : 0;
    CopyString(record->module_path, sizeof(record->module_path), path);
    const char* fileName = strrchr(path, '\\');
    fileName = fileName ? fileName + 1 : path;
    CopyString(record->module_name, sizeof(record->module_name), fileName);
    if (package) {
        CopyString(
            record->settings_key,
            sizeof(record->settings_key),
            package->expected_id);
    } else {
        GetFileStem(
            fileName,
            record->settings_key,
            sizeof(record->settings_key));
    }
    UpdateSessionMarker(
        "loading_native_mod",
        package && package->expected_id
            ? package->expected_id
            : record->settings_key);
    JoinPath(
        record->data_path,
        sizeof(record->data_path),
        g_dataDirectory,
        record->settings_key);
    EnsureDirectory(record->data_path);

    char configName[MAX_PATH] = {};
    _snprintf_s(
        configName,
        sizeof(configName),
        _TRUNCATE,
        "%s.ini",
        record->settings_key);
    JoinPath(
        record->config_path,
        sizeof(record->config_path),
        g_configDirectory,
        configName);

    record->module = LoadLibraryExA(
        path,
        nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!record->module && GetLastError() == ERROR_INVALID_PARAMETER) {
        record->module = LoadLibraryA(path);
    }
    if (!record->module) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "failed to load %s (win32=%lu)",
            path,
            GetLastError());
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_PLATFORM;
    }

    WotbModLoadV3Fn entryV3 = reinterpret_cast<WotbModLoadV3Fn>(
        GetProcAddress(record->module, WOTBMOD_V3_ENTRY_NAME));
    if (entryV3) {
        int grantedTier = package
            ? static_cast<int>(
                  package->granted_permission_tier)
            : GetPrivateProfileIntA(
                  "permissions",
                  record->settings_key,
                  WOTBMOD_V3_PERMISSION_SAFE,
                  g_settingsPath);
        if (grantedTier < WOTBMOD_V3_PERMISSION_SAFE)
            grantedTier = WOTBMOD_V3_PERMISSION_SAFE;
        if (grantedTier > WOTBMOD_V3_PERMISSION_UNSAFE)
            grantedTier = WOTBMOD_V3_PERMISSION_UNSAFE;
        WotbModV3Result createResult = WotbModV3Runtime_CreateMod(
            path,
            static_cast<uint32_t>(grantedTier),
            &record->v3_handle);
        if (createResult != WOTBMOD_V3_OK) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "%s could not create a V3 owner (result=%d)",
                fileName,
                static_cast<int>(createResult));
            ResetFailedRecord(record);
            return V3ResultToLegacy(createResult);
        }
        if (package) {
            createResult = InstallV3PackageMetadata(
                record->v3_handle,
                *package->plan,
                package->package_root);
            if (createResult != WOTBMOD_V3_OK) {
                RuntimeLog(
                    WOTBMOD_LOG_ERROR,
                    "[%s] package metadata handoff failed "
                    "(result=%d)",
                    package->expected_id,
                    static_cast<int>(createResult));
                ResetFailedRecord(record);
                return V3ResultToLegacy(createResult);
            }
            const char* permissionNames[
                WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS] = {};
            for (uint32_t index = 0u;
                 index < package->plan->permission_count; ++index) {
                permissionNames[index] =
                    package->plan->permissions[index];
            }
            createResult =
                WotbModV3Runtime_SetPermissionGrants(
                    record->v3_handle,
                    permissionNames,
                    package->plan->permission_count,
                    1u);
            if (createResult != WOTBMOD_V3_OK) {
                RuntimeLog(
                    WOTBMOD_LOG_ERROR,
                    "[%s] package permission grants were rejected "
                    "(result=%d)",
                    package->expected_id,
                    static_cast<int>(createResult));
                ResetFailedRecord(record);
                return V3ResultToLegacy(createResult);
            }
            if (PackageHasPermission(*package->plan, "resources.mod")) {
                CopyString(
                    record->v3_package_root,
                    sizeof(record->v3_package_root),
                    package->package_root);
                CopyString(
                    record->v3_package_provider,
                    sizeof(record->v3_package_provider),
                    package->expected_id);
                createResult =
                    wotbmod::v3::MountVfsPackageForRuntime(
                        record->v3_handle,
                        record->v3_package_provider,
                        record->v3_package_root,
                        0,
                        &record->v3_package_mount);
                if (createResult != WOTBMOD_V3_OK) {
                    RuntimeLog(
                        WOTBMOD_LOG_ERROR,
                        "[%s] package root could not be mounted "
                        "(result=%d root=%s)",
                        package->expected_id,
                        static_cast<int>(createResult),
                        package->package_root);
                    ResetFailedRecord(record);
                    return V3ResultToLegacy(createResult);
                }
            } else {
                RuntimeLog(
                    WOTBMOD_LOG_INFO,
                    "[%s] package VFS mount skipped; "
                    "resources.mod permission not requested",
                    package->expected_id);
            }
        }
        WotbModV3RuntimeModuleInfo v3Info = {};
        v3Info.struct_size = sizeof(v3Info);
        v3Info.api_version = WOTBMOD_V3_ABI_VERSION;
        WotbModV3Result entryResult = WotbModV3Runtime_InvokeEntry(
            record->v3_handle,
            entryV3,
            &v3Info);
        if (entryResult != WOTBMOD_V3_OK) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "%s WotbModLoadV3 failed (result=%d)",
                fileName,
                static_cast<int>(entryResult));
            ResetFailedRecord(record);
            return V3ResultToLegacy(entryResult);
        }
        if (package &&
            (strcmp(v3Info.id, package->expected_id) != 0 ||
             strcmp(
                 v3Info.version,
                 package->expected_version) != 0)) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "%s V3 descriptor identity %s@%s does not "
                "exactly match manifest %s@%s",
                fileName,
                v3Info.id,
                v3Info.version,
                package->expected_id,
                package->expected_version);
            ResetFailedRecord(record);
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
        if (!IsValidModId(v3Info.id) || IsDuplicateModId(v3Info.id)) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "%s has invalid or duplicate V3 mod id '%s'",
                fileName,
                v3Info.id);
            ResetFailedRecord(record);
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
        record->api_generation = 3u;
        record->public_info.struct_size = sizeof(record->public_info);
        record->public_info.abi_version = WOTBMOD_V3_ABI_VERSION;
        CopyString(
            record->public_info.id,
            sizeof(record->public_info.id),
            v3Info.id);
        CopyString(
            record->public_info.name,
            sizeof(record->public_info.name),
            v3Info.name);
        CopyString(
            record->public_info.version,
            sizeof(record->public_info.version),
            v3Info.version);
        CopyString(
            record->public_info.author,
            sizeof(record->public_info.author),
            v3Info.author);
        CopyString(
            record->public_info.description,
            sizeof(record->public_info.description),
            v3Info.description);
        CopyString(
            record->public_info.module_path,
            sizeof(record->public_info.module_path),
            path);
        InterlockedExchange(&record->state, WOTBMOD_STATE_LOADED);

        const bool enabledByDefault =
            GetPrivateProfileIntA(
                "mods", record->settings_key, 1, g_settingsPath) != 0;
        if (appendedSlot) InterlockedIncrement(&g_modCount);
        if (enabledByDefault) {
            const WotbModResult enableResult =
                SetRecordEnabled(record, true);
            if (enableResult != WOTBMOD_OK) return enableResult;
        } else {
            InterlockedExchange(&record->enabled, 0);
            InterlockedExchange(&record->state, WOTBMOD_STATE_DISABLED);
            SyncPublicState(record);
        }
        RuntimeLog(
            WOTBMOD_LOG_INFO,
            "[%s] loaded V3 %s v%s by %s (permission-tier=%u)",
            record->public_info.id,
            record->public_info.name,
            record->public_info.version,
            record->public_info.author[0]
                ? record->public_info.author
                : "unknown",
            v3Info.permission_tier);
        return WOTBMOD_OK;
    }

    if (package) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] manifest-declared native package does not "
            "export %s; legacy fallback is forbidden",
            package->expected_id,
            WOTBMOD_V3_ENTRY_NAME);
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }

    WotbModLoadFn entry = reinterpret_cast<WotbModLoadFn>(
        GetProcAddress(record->module, WOTBMOD_ENTRY_NAME));
    if (!entry) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "%s does not export %s; skipped",
            fileName,
            WOTBMOD_ENTRY_NAME);
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    WotbModInfo info = {};
    info.struct_size = sizeof(info);
    info.abi_version = WOTBMOD_ABI_VERSION;
    WotbModResult entryResult = WOTBMOD_ERROR_PLATFORM;
    if (!CallModEntry(entry, record, &info, &entryResult)) {
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (entryResult != WOTBMOD_OK) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] WotbModLoad returned %d",
            RecordLogId(record),
            (int)entryResult);
        ResetFailedRecord(record);
        return entryResult;
    }

    const size_t requiredInfoSize =
        offsetof(WotbModInfo, on_frame) + sizeof(info.on_frame);
    if (info.struct_size < requiredInfoSize ||
        WOTBMOD_ABI_MAJOR(info.abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION) ||
        WOTBMOD_ABI_MINOR(info.abi_version) >
            WOTBMOD_ABI_MINOR(WOTBMOD_ABI_VERSION)) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "%s uses unsupported ABI 0x%08X or descriptor size %u",
            fileName,
            info.abi_version,
            info.struct_size);
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }
    if (!IsValidModId(info.id) || IsDuplicateModId(info.id)) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "%s has invalid or duplicate mod id '%s'",
            fileName,
            info.id ? info.id : "");
        ResetFailedRecord(record);
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    record->api_generation = 2u;
    record->public_info.struct_size = sizeof(record->public_info);
    record->public_info.abi_version = info.abi_version;
    record->public_info.flags = info.flags;
    CopyString(
        record->public_info.id,
        sizeof(record->public_info.id),
        info.id);
    CopyString(
        record->public_info.name,
        sizeof(record->public_info.name),
        info.name ? info.name : info.id);
    CopyString(
        record->public_info.version,
        sizeof(record->public_info.version),
        info.version ? info.version : "0.0.0");
    CopyString(
        record->public_info.author,
        sizeof(record->public_info.author),
        info.author ? info.author : "");
    CopyString(
        record->public_info.description,
        sizeof(record->public_info.description),
        info.description ? info.description : "");
    CopyString(
        record->public_info.module_path,
        sizeof(record->public_info.module_path),
        path);
    record->callbacks.on_enable = info.on_enable;
    record->callbacks.on_disable = info.on_disable;
    record->callbacks.on_unload = info.on_unload;
    record->callbacks.on_frame = info.on_frame;
    InterlockedExchange(&record->state, WOTBMOD_STATE_LOADED);

    const bool enabledByDefault =
        GetPrivateProfileIntA(
            "mods", record->settings_key, 1, g_settingsPath) != 0;
    if (appendedSlot) InterlockedIncrement(&g_modCount);
    if (enabledByDefault) {
        WotbModResult enableResult = SetRecordEnabled(record, true);
        if (enableResult != WOTBMOD_OK) return enableResult;
    } else {
        InterlockedExchange(&record->enabled, 0);
        InterlockedExchange(&record->state, WOTBMOD_STATE_DISABLED);
        RemoveOwnedHooks(record);
        SyncPublicState(record);
    }

    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] loaded %s v%s by %s",
        record->public_info.id,
        record->public_info.name,
        record->public_info.version,
        record->public_info.author[0] ? record->public_info.author : "unknown");
    return WOTBMOD_OK;
}

static int __cdecl ComparePaths(const void* left, const void* right) {
    return _stricmp(
        static_cast<const char*>(left),
        static_cast<const char*>(right));
}

static bool ParentDirectory(
    const char* path,
    char* output,
    size_t outputCapacity) {
    if (!path || !path[0] || !output || outputCapacity == 0 ||
        strlen(path) >= outputCapacity) {
        return false;
    }
    CopyString(output, outputCapacity, path);
    char* slash = strrchr(output, '\\');
    char* forwardSlash = strrchr(output, '/');
    if (!slash || (forwardSlash && forwardSlash > slash)) {
        slash = forwardSlash;
    }
    if (!slash || slash == output) {
        return false;
    }
    *slash = '\0';
    return true;
}

static bool PathsEqual(const char* left, const char* right) {
    if (!left || !left[0] || !right || !right[0]) {
        return false;
    }
    char normalizedLeft[WOTBMOD_V3_MAX_PATH] = {};
    char normalizedRight[WOTBMOD_V3_MAX_PATH] = {};
    if (!NormalizeAbsolutePath(
            left,
            normalizedLeft,
            sizeof(normalizedLeft)) ||
        !NormalizeAbsolutePath(
            right,
            normalizedRight,
            sizeof(normalizedRight))) {
        return false;
    }
    return _stricmp(normalizedLeft, normalizedRight) == 0;
}

static bool HasSidecarManifest(const char* dllPath) {
    if (!dllPath || !dllPath[0] || strlen(dllPath) >= MAX_PATH) {
        return false;
    }
    char sidecar[MAX_PATH] = {};
    CopyString(sidecar, sizeof(sidecar), dllPath);
    char* extension = strrchr(sidecar, '.');
    if (!extension || _stricmp(extension, ".dll") != 0) {
        return false;
    }
    const size_t prefixLength =
        static_cast<size_t>(extension - sidecar);
    static const char suffix[] = ".manifest.json";
    if (prefixLength + sizeof(suffix) > sizeof(sidecar)) {
        return false;
    }
    memcpy(extension, suffix, sizeof(suffix));
    const DWORD attributes = GetFileAttributesA(sidecar);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool IsClaimedPackageEntrypoint(
    const char* dllPath,
    const wotbmod::v3::PackagePlanEntry* entries,
    uint32_t entryCount) {
    if (HasSidecarManifest(dllPath)) {
        return true;
    }
    for (uint32_t index = 0u; index < entryCount; ++index) {
        if (entries[index].entrypoint_path[0] &&
            PathsEqual(
                dllPath,
                entries[index].entrypoint_path)) {
            return true;
        }
    }
    return false;
}

/* mods.ini [policy] require_trusted_signature=1 turns the loader's
 * permissive signature policy into the mandatory one: unsigned packages and
 * unknown signers are blocked before LoadLibrary, not just warned about.
 * `wotbmod policy --require-signature on` writes the key. Both preflight
 * passes (discovery and materialisation) must carry the bit - the second
 * pass is the one that decides what gets loaded. */
static uint32_t SignaturePolicyFlags(bool logIt) {
    if (GetPrivateProfileIntA(
            "policy",
            "require_trusted_signature",
            0,
            g_settingsPath) == 0) {
        return 0u;
    }
    if (logIt) {
        RuntimeLog(
            WOTBMOD_LOG_INFO,
            "policy: trusted package signatures are required (mods.ini)");
    }
    return wotbmod::v3::PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE;
}

static uint32_t ReadPermissionGrant(const char* modId) {
    int tier = GetPrivateProfileIntA(
        "permissions",
        modId ? modId : "",
        WOTBMOD_V3_PERMISSION_SAFE,
        g_settingsPath);
    if (tier < WOTBMOD_V3_PERMISSION_SAFE) {
        tier = WOTBMOD_V3_PERMISSION_SAFE;
    }
    if (tier > WOTBMOD_V3_PERMISSION_UNSAFE) {
        tier = WOTBMOD_V3_PERMISSION_UNSAFE;
    }
    return static_cast<uint32_t>(tier);
}

static void LogPackageWarnings(
    const wotbmod::v3::PackagePlanEntry& entry) {
    if (entry.warning_flags == 0u) {
        return;
    }
    RuntimeLog(
        WOTBMOD_LOG_WARNING,
        "[%s] package warnings: unreviewed=%s unsafe-tier=%s "
        "embedded-signature-unverified=%s unsigned=%s "
        "untrusted-signer=%s signature-status=%u key=%s",
        entry.id[0] ? entry.id : entry.source_path,
        (entry.warning_flags &
         wotbmod::v3::PACKAGE_WARNING_UNREVIEWED)
            ? "yes"
            : "no",
        (entry.warning_flags &
         wotbmod::v3::PACKAGE_WARNING_UNSAFE_PERMISSION_TIER)
            ? "yes"
            : "no",
        (entry.warning_flags &
         wotbmod::v3::
             PACKAGE_WARNING_EMBEDDED_SIGNATURE_UNVERIFIED)
            ? "yes"
            : "no",
        (entry.warning_flags &
         wotbmod::v3::PACKAGE_WARNING_UNSIGNED)
            ? "yes"
            : "no",
        (entry.warning_flags &
         wotbmod::v3::PACKAGE_WARNING_UNTRUSTED_SIGNER)
            ? "yes"
            : "no",
        entry.signature_status,
        entry.signature_key_id[0]
            ? entry.signature_key_id
            : "-");
}

static double ResolveDeltaSeconds(double suppliedDeltaSeconds) {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (suppliedDeltaSeconds > 0.0) {
        g_lastFrameTime = now;
        return suppliedDeltaSeconds;
    }
    if (g_lastFrameTime.QuadPart == 0 || g_qpcFrequency.QuadPart == 0) {
        g_lastFrameTime = now;
        return 0.0;
    }
    const double delta =
        (double)(now.QuadPart - g_lastFrameTime.QuadPart) /
        (double)g_qpcFrequency.QuadPart;
    g_lastFrameTime = now;
    return delta;
}

static void WOTBMOD_V3_CALL V3RuntimeLogSinkAdapter(
    uint32_t level,
    const char* category,
    const char* message,
    void*) {
    WotbModLogLevel legacyLevel = WOTBMOD_LOG_TRACE;
    if (level == WOTBMOD_V3_LOG_INFO) {
        legacyLevel = WOTBMOD_LOG_INFO;
    } else if (level == WOTBMOD_V3_LOG_WARNING) {
        legacyLevel = WOTBMOD_LOG_WARNING;
    } else if (level >= WOTBMOD_V3_LOG_ERROR) {
        legacyLevel = WOTBMOD_LOG_ERROR;
    }
    RuntimeLog(
        legacyLevel,
        "[v3:%s] %s",
        category && category[0] ? category : "runtime",
        message ? message : "");
}

static WotbModV3Result WOTBMOD_V3_CALL V3ClientInvokeAdapter(
    void*,
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t requestSize,
    void* response,
    uint32_t responseSize) {
    if (!g_v3ClientBackend.invoke) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return g_v3ClientBackend.invoke(
        g_v3ClientBackend.user_data,
        mod,
        operation,
        request,
        requestSize,
        response,
        responseSize);
}

} /* namespace */

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_PreflightCrashLoop(
    const char* modsDirectory,
    WotbModRuntimeLogSink logSink,
    void* logUserData,
    int32_t* outSafeMode) {
    if (outSafeMode) *outSafeMode = 0;
    if (!modsDirectory || !modsDirectory[0] || !outSafeMode) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(&g_initialized, 0, 0) != 0) {
        return WOTBMOD_ERROR_ALREADY_EXISTS;
    }
    g_logSink = logSink;
    g_logUserData = logUserData;

    char normalizedModsDirectory[MAX_PATH] = {};
    char cacheDirectory[MAX_PATH] = {};
    if (!NormalizeAbsolutePath(
            modsDirectory,
            normalizedModsDirectory,
            sizeof(normalizedModsDirectory)) ||
        !EnsureDirectory(normalizedModsDirectory) ||
        !JoinPath(
            cacheDirectory,
            sizeof(cacheDirectory),
            normalizedModsDirectory,
            "cache") ||
        !EnsureDirectory(cacheDirectory) ||
        !EnsureCrashLoopGuardPrepared(cacheDirectory)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    *outSafeMode =
        InterlockedCompareExchange(&g_safeMode, 0, 0) != 0
            ? 1
            : 0;
    return WOTBMOD_OK;
}

extern "C" void WOTBMOD_CALL WotbModRuntime_SetCrashLoopPhase(
    const char* phase) {
    if (!phase || !phase[0] || strlen(phase) >= 96u) return;
    if (strstr(phase, "native") || strstr(phase, "binding")) {
        SetCrashAttribution(
            g_safeModeLastNativeBinding,
            sizeof(g_safeModeLastNativeBinding),
            phase);
    }
    UpdateSessionMarker(phase, nullptr);
}

extern "C" void WOTBMOD_CALL WotbModRuntime_ProcessDetach() {
    CleanSessionMarkerAtProcessDetach();
}

/*
 * Defined further down (just before WotbModRuntime_LoadAll) and installed into
 * the V3 runtime here at Initialize, so the V3 lifecycle service can queue a
 * reload without linking directly against this translation unit. Both __cdecl.
 */
extern "C" WotbModV3Result WOTBMOD_CALL
WotbModRuntime_RequestModReload(WotbModV3Handle target);
extern "C" WotbModV3Result WOTBMOD_CALL
WotbModRuntime_CanReloadMod(WotbModV3Handle target);
extern "C" void WOTBMOD_CALL WotbModV3Runtime_SetReloadHost(
    WotbModV3Result(WOTBMOD_CALL*)(WotbModV3Handle),
    WotbModV3Result(WOTBMOD_CALL*)(WotbModV3Handle));

extern "C" WotbModResult WOTBMOD_CALL WotbModRuntime_Initialize(
    const WotbModRuntimeOptions* options) {
    if (!options ||
        options->struct_size <
            offsetof(WotbModRuntimeOptions, hook_backend) +
                sizeof(options->hook_backend)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(&g_initialized, 1, 0) != 0) {
        return WOTBMOD_ERROR_ALREADY_EXISTS;
    }

    // Hand the V3 lifecycle service the reload entry points now that this
    // runtime (which owns the module handles) is coming up.
    WotbModV3Runtime_SetReloadHost(
        &WotbModRuntime_RequestModReload,
        &WotbModRuntime_CanReloadMod);

    g_runtimeOptionFlags = 0u;
    g_v3LocalVehicleId = 0u;
    if (options->struct_size >=
        offsetof(WotbModRuntimeOptions, flags) +
            sizeof(options->flags)) {
        g_runtimeOptionFlags =
            options->flags &
            WOTBMOD_RUNTIME_OPTION_ALLOW_LEGACY_RAW_PROCESS_API;
    }
    g_logSink = options->log_sink;
    g_logUserData = options->log_user_data;
    if (LegacyRawProcessApiAllowed()) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "developer-only legacy raw process/hook API is enabled");
    }
    g_gameModule = static_cast<HMODULE>(
        options->game_module ? options->game_module : GetModuleHandleA(nullptr));

    if (options->game_directory && options->game_directory[0]) {
        if (!NormalizeAbsolutePath(
                options->game_directory,
                g_gameDirectory,
                sizeof(g_gameDirectory))) {
            InterlockedExchange(&g_initialized, 0);
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    } else if (!DeriveExecutableDirectory(
                   g_gameDirectory, sizeof(g_gameDirectory))) {
        InterlockedExchange(&g_initialized, 0);
        return WOTBMOD_ERROR_PLATFORM;
    }

    if (options->mods_directory && options->mods_directory[0]) {
        if (!NormalizeAbsolutePath(
                options->mods_directory,
                g_modsDirectory,
                sizeof(g_modsDirectory))) {
            InterlockedExchange(&g_initialized, 0);
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    } else if (!JoinPath(
                   g_modsDirectory,
                   sizeof(g_modsDirectory),
                   g_gameDirectory,
                   "mods")) {
        InterlockedExchange(&g_initialized, 0);
        return WOTBMOD_ERROR_PLATFORM;
    }

    if (!EnsureDirectory(g_modsDirectory) ||
        !JoinPath(
            g_dataDirectory,
            sizeof(g_dataDirectory),
            g_modsDirectory,
            "data") ||
        !EnsureDirectory(g_dataDirectory) ||
        !JoinPath(
            g_configDirectory,
            sizeof(g_configDirectory),
            g_modsDirectory,
            "config") ||
        !EnsureDirectory(g_configDirectory) ||
        !JoinPath(
            g_settingsPath,
            sizeof(g_settingsPath),
            g_modsDirectory,
            "mods.ini")) {
        InterlockedExchange(&g_initialized, 0);
        return WOTBMOD_ERROR_PLATFORM;
    }

    char v3CacheDirectory[MAX_PATH] = {};
    if (!JoinPath(
            v3CacheDirectory,
            sizeof(v3CacheDirectory),
            g_modsDirectory,
            "cache") ||
        !EnsureDirectory(v3CacheDirectory) ||
        !EnsureCrashLoopGuardPrepared(v3CacheDirectory)) {
        InterlockedExchange(&g_initialized, 0);
        return WOTBMOD_ERROR_PLATFORM;
    }
    WotbModV3RuntimeOptions v3Options = {};
    v3Options.struct_size = sizeof(v3Options);
    v3Options.api_version = WOTBMOD_V3_ABI_VERSION;
    v3Options.game_directory = g_gameDirectory;
    v3Options.mods_directory = g_modsDirectory;
    v3Options.cache_directory = v3CacheDirectory;
    v3Options.config_directory = g_configDirectory;
    v3Options.process_architecture = 32u;
    v3Options.log_sink = V3RuntimeLogSinkAdapter;
    ZeroMemory(g_clientBuild, sizeof(g_clientBuild));
    ZeroMemory(
        g_clientExecutableSha256,
        sizeof(g_clientExecutableSha256));
    const WotbModRuntimeV3ClientBackend* suppliedV3Backend =
        options->struct_size >=
                offsetof(
                    WotbModRuntimeOptions,
                    v3_client_backend) +
                    sizeof(options->v3_client_backend)
            ? options->v3_client_backend
            : nullptr;
    if (suppliedV3Backend &&
        suppliedV3Backend->struct_size >=
            offsetof(
                WotbModRuntimeV3ClientBackend,
                client_build) +
                sizeof(suppliedV3Backend->client_build) &&
        memchr(
            suppliedV3Backend->client_build,
            '\0',
            sizeof(suppliedV3Backend->client_build))) {
        CopyString(
            g_clientBuild,
            sizeof(g_clientBuild),
            suppliedV3Backend->client_build);
    }
    if (suppliedV3Backend &&
        suppliedV3Backend->struct_size >=
            offsetof(
                WotbModRuntimeV3ClientBackend,
                client_executable_sha256) +
                sizeof(
                    suppliedV3Backend
                        ->client_executable_sha256) &&
        memchr(
            suppliedV3Backend->client_executable_sha256,
            '\0',
            sizeof(
                suppliedV3Backend
                    ->client_executable_sha256))) {
        CopyString(
            g_clientExecutableSha256,
            sizeof(g_clientExecutableSha256),
            suppliedV3Backend->client_executable_sha256);
    }
    if (suppliedV3Backend &&
        suppliedV3Backend->struct_size >=
            offsetof(
                WotbModRuntimeV3ClientBackend,
                binding_pack_version) +
                sizeof(
                    suppliedV3Backend
                        ->binding_pack_version)) {
        v3Options.binding_pack_version =
            suppliedV3Backend->binding_pack_version;
    }
    v3Options.client_version =
        g_clientBuild[0] ? g_clientBuild : nullptr;
    v3Options.executable_sha256 =
        g_clientExecutableSha256[0]
            ? g_clientExecutableSha256
            : nullptr;
    const size_t v3HookBackendEnd =
        offsetof(WotbModRuntimeOptions, v3_hook_backend) +
        sizeof(options->v3_hook_backend);
    v3Options.native_hook_backend =
        options->struct_size >= v3HookBackendEnd
            ? options->v3_hook_backend
            : nullptr;
    const WotbModV3Result v3Initialize =
        WotbModV3Runtime_Initialize(&v3Options);
    if (v3Initialize != WOTBMOD_V3_OK) {
        DeleteOwnedSessionMarker();
        InterlockedExchange(&g_initialized, 0);
        return V3ResultToLegacy(v3Initialize);
    }

    ZeroMemory(&g_hookBackend, sizeof(g_hookBackend));
    if (options->hook_backend &&
        options->hook_backend->struct_size >= sizeof(g_hookBackend)) {
        g_hookBackend = *options->hook_backend;
    }
    ZeroMemory(&g_resourceBackend, sizeof(g_resourceBackend));
    const size_t resourceBackendEnd =
        offsetof(WotbModRuntimeOptions, resource_backend) +
        sizeof(options->resource_backend);
    const size_t minimumResourceBackendSize =
        offsetof(WotbModRuntimeResourceBackend, registry_changed) +
        sizeof(g_resourceBackend.registry_changed);
    if (options->struct_size >= resourceBackendEnd &&
        options->resource_backend &&
        options->resource_backend->struct_size >=
            minimumResourceBackendSize) {
        const size_t copySize =
            options->resource_backend->struct_size < sizeof(g_resourceBackend)
                ? options->resource_backend->struct_size
                : sizeof(g_resourceBackend);
        memcpy(&g_resourceBackend, options->resource_backend, copySize);
        g_resourceBackend.struct_size = sizeof(g_resourceBackend);
    }
    ZeroMemory(&g_audioBackend, sizeof(g_audioBackend));
    const size_t audioBackendEnd =
        offsetof(WotbModRuntimeOptions, audio_backend) +
        sizeof(options->audio_backend);
    const size_t minimumAudioBackendSize =
        offsetof(WotbModRuntimeAudioBackend, release) +
        sizeof(g_audioBackend.release);
    if (options->struct_size >= audioBackendEnd &&
        options->audio_backend &&
        options->audio_backend->struct_size >=
            minimumAudioBackendSize) {
        const size_t copySize =
            options->audio_backend->struct_size < sizeof(g_audioBackend)
                ? options->audio_backend->struct_size
                : sizeof(g_audioBackend);
        memcpy(&g_audioBackend, options->audio_backend, copySize);
        g_audioBackend.struct_size = sizeof(g_audioBackend);
    }
    ZeroMemory(&g_soundBackend, sizeof(g_soundBackend));
    const size_t soundBackendEnd =
        offsetof(WotbModRuntimeOptions, sound_backend) +
        sizeof(options->sound_backend);
    const size_t minimumSoundBackendSize =
        offsetof(WotbModRuntimeSoundBackend, release) +
        sizeof(g_soundBackend.release);
    if (options->struct_size >= soundBackendEnd &&
        options->sound_backend &&
        options->sound_backend->struct_size >= minimumSoundBackendSize) {
        const size_t copySize =
            options->sound_backend->struct_size < sizeof(g_soundBackend)
                ? options->sound_backend->struct_size
                : sizeof(g_soundBackend);
        memcpy(&g_soundBackend, options->sound_backend, copySize);
        g_soundBackend.struct_size = sizeof(g_soundBackend);
    }
    ZeroMemory(&g_gameplayBackend, sizeof(g_gameplayBackend));
    const size_t gameplayBackendEnd =
        offsetof(WotbModRuntimeOptions, gameplay_backend) +
        sizeof(options->gameplay_backend);
    const size_t minimumGameplayBackendSize =
        offsetof(WotbModRuntimeGameplayBackend, vehicle_get_info) +
        sizeof(g_gameplayBackend.vehicle_get_info);
    if (options->struct_size >= gameplayBackendEnd &&
        options->gameplay_backend &&
        options->gameplay_backend->struct_size >=
            minimumGameplayBackendSize) {
        const size_t copySize =
            options->gameplay_backend->struct_size <
                    sizeof(g_gameplayBackend)
                ? options->gameplay_backend->struct_size
                : sizeof(g_gameplayBackend);
        memcpy(
            &g_gameplayBackend,
            options->gameplay_backend,
            copySize);
        g_gameplayBackend.struct_size =
            sizeof(g_gameplayBackend);
    }
    ZeroMemory(&g_v3ClientBackend, sizeof(g_v3ClientBackend));
    g_v3LocalVehicleId = 0u;
    const size_t v3ClientBackendEnd =
        offsetof(WotbModRuntimeOptions, v3_client_backend) +
        sizeof(options->v3_client_backend);
    const size_t minimumV3ClientBackendSize =
        offsetof(WotbModRuntimeV3ClientBackend, invoke) +
        sizeof(g_v3ClientBackend.invoke);
    if (options->struct_size >= v3ClientBackendEnd &&
        options->v3_client_backend &&
        options->v3_client_backend->struct_size >=
            minimumV3ClientBackendSize &&
        options->v3_client_backend->api_version ==
            WOTBMOD_RUNTIME_V3_CLIENT_BACKEND_VERSION &&
        options->v3_client_backend->invoke) {
        const size_t copySize =
            options->v3_client_backend->struct_size <
                    sizeof(g_v3ClientBackend)
                ? options->v3_client_backend->struct_size
                : sizeof(g_v3ClientBackend);
        memcpy(
            &g_v3ClientBackend,
            options->v3_client_backend,
            copySize);
        g_v3ClientBackend.struct_size =
            sizeof(g_v3ClientBackend);
    }
    if (g_v3ClientBackend.invoke) {
        wotbmod::v3::ClientHostBackend clientBackend = {};
        clientBackend.struct_size = sizeof(clientBackend);
        clientBackend.api_version =
            WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
        clientBackend.binding_pack_version =
            g_v3ClientBackend.binding_pack_version;
        clientBackend.compatibility_state =
            g_v3ClientBackend.compatibility_state;
        clientBackend.invoke = &V3ClientInvokeAdapter;
        wotbmod::v3::SetClientHostBackend(&clientBackend);
    } else {
        wotbmod::v3::SetClientHostBackend(nullptr);
    }
    QueryPerformanceFrequency(&g_qpcFrequency);
    QueryPerformanceCounter(&g_lastFrameTime);
    UpdateSessionMarker("runtime_initialized", nullptr);
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "runtime initialized game=%s mods=%s safe-mode=%s hooks=%s resource-load=%s audio=%s custom-audio-files=%s native-sound=%s gameplay=%s",
        g_gameDirectory,
        g_modsDirectory,
        InterlockedCompareExchange(&g_safeMode, 0, 0) == 1
            ? "active"
            : (InterlockedCompareExchange(&g_safeMode, 0, 0) == 2
                ? "portable-only"
                : "inactive"),
        HookBackendReady() ? "available" : "unavailable",
        ResourceBackendReady() ? "available" : "unavailable",
        AudioBackendReady() ? "available" : "unavailable",
        AudioClipBackendReady() ? "available" : "unavailable",
        SoundBackendReady() ? "available" : "unavailable",
        GameplayBackendReady() ? "available" : "unavailable");
    return WOTBMOD_OK;
}

/*
 * Re-run the package preflight and load exactly the one package whose id matches
 * onlyId. This is the same two-pass flow WotbModRuntime_LoadAll() uses (discover
 * -> resolve permission grants -> execute), filtered to a single mod, so a
 * reloaded package comes back configured byte-for-byte the way a fresh boot
 * would configure it. It is deliberately a self-contained replica rather than a
 * refactor of LoadAll, to keep the cold boot path untouched.
 */
static WotbModResult LoadPackagedModById(const char* onlyId) {
    if (!g_initialized) return WOTBMOD_ERROR_DISABLED;
    if (!onlyId || !onlyId[0]) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    const LONG safeMode =
        InterlockedCompareExchange(&g_safeMode, 0, 0);
    if (safeMode == 1) return WOTBMOD_ERROR_DISABLED;

    using namespace wotbmod::v3;
    PackagePreflightOptions packageOptions = {};
    packageOptions.struct_size = sizeof(packageOptions);
    packageOptions.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    packageOptions.flags =
        PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED | SignaturePolicyFlags(true);
    packageOptions.max_permission_tier =
        WOTBMOD_V3_PERMISSION_UNSAFE;
    CopyString(
        packageOptions.mods_root,
        sizeof(packageOptions.mods_root),
        g_modsDirectory);
    if (!JoinPath(
            packageOptions.archive_staging_root,
            sizeof(packageOptions.archive_staging_root),
            g_modsDirectory,
            "cache\\package_staging")) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    CopyString(
        packageOptions.client_build,
        sizeof(packageOptions.client_build),
        g_clientBuild);
    CopyString(
        packageOptions.client_executable_sha256,
        sizeof(packageOptions.client_executable_sha256),
        g_clientExecutableSha256);
    packageOptions.limits = DefaultPackagePreflightLimits();
    packageOptions.limits.max_candidates = kMaxMods;

    PackagePlanSummary discoverySummary = {};
    discoverySummary.struct_size = sizeof(discoverySummary);
    discoverySummary.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    WotbModV3Result packageResult = BuildPackageLoadPlan(
        &packageOptions, nullptr, 0u, &discoverySummary);
    if (packageResult != WOTBMOD_V3_OK &&
        packageResult != WOTBMOD_V3_E_BUFFER_TOO_SMALL) {
        return V3ResultToLegacy(packageResult);
    }
    const uint32_t packageCapacity =
        discoverySummary.required_capacity;
    if (packageCapacity == 0u) return WOTBMOD_ERROR_NOT_FOUND;

    PackagePlanEntry* discoveryEntries =
        static_cast<PackagePlanEntry*>(
            calloc(packageCapacity, sizeof(*discoveryEntries)));
    PackagePermissionGrant* grants =
        static_cast<PackagePermissionGrant*>(
            calloc(packageCapacity, sizeof(*grants)));
    PackagePlanEntry* packageEntries =
        static_cast<PackagePlanEntry*>(
            calloc(packageCapacity, sizeof(*packageEntries)));
    if (!discoveryEntries || !grants || !packageEntries) {
        free(discoveryEntries);
        free(grants);
        free(packageEntries);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    discoverySummary = {};
    discoverySummary.struct_size = sizeof(discoverySummary);
    discoverySummary.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    packageResult = BuildPackageLoadPlan(
        &packageOptions,
        discoveryEntries,
        packageCapacity,
        &discoverySummary);
    if (packageResult != WOTBMOD_V3_OK) {
        free(discoveryEntries);
        free(grants);
        free(packageEntries);
        return V3ResultToLegacy(packageResult);
    }

    uint32_t grantCount = 0u;
    for (uint32_t index = 0u;
         index < discoverySummary.discovered_count; ++index) {
        const char* id = discoveryEntries[index].id;
        if (!id[0]) continue;
        bool duplicate = false;
        for (uint32_t grantIndex = 0u;
             grantIndex < grantCount; ++grantIndex) {
            if (_stricmp(grants[grantIndex].id, id) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        PackagePermissionGrant& grant = grants[grantCount++];
        grant.struct_size = sizeof(grant);
        grant.api_version =
            WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
        grant.max_permission_tier = ReadPermissionGrant(id);
        CopyString(grant.id, sizeof(grant.id), id);
    }

    PackagePlanSummary packageSummary = {};
    packageSummary.struct_size = sizeof(packageSummary);
    packageSummary.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    packageOptions.flags =
        PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED |
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES |
        SignaturePolicyFlags(false);
    packageOptions.max_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    packageOptions.permission_grants = grants;
    packageOptions.permission_grant_count = grantCount;
    packageResult = BuildPackageLoadPlan(
        &packageOptions,
        packageEntries,
        packageCapacity,
        &packageSummary);
    free(discoveryEntries);
    discoveryEntries = nullptr;
    free(grants);
    grants = nullptr;
    if (packageResult != WOTBMOD_V3_OK) {
        free(packageEntries);
        return V3ResultToLegacy(packageResult);
    }

    WotbModResult loadResult = WOTBMOD_ERROR_NOT_FOUND;
    for (uint32_t index = 0u;
         index < packageSummary.discovered_count; ++index) {
        const PackagePlanEntry& entry = packageEntries[index];
        if (_stricmp(entry.id, onlyId) != 0) continue;
        LogPackageWarnings(entry);
        if (entry.status != PACKAGE_PLAN_READY ||
            entry.result != WOTBMOD_V3_OK) {
            loadResult = V3ResultToLegacy(
                static_cast<WotbModV3Result>(entry.result));
            break;
        }
        if (IsAutoDisabledMod(entry.id)) {
            loadResult = WOTBMOD_ERROR_DISABLED;
            break;
        }
        if (entry.package_type ==
            WOTBMOD_V3_PACKAGE_CONTENT_ONLY) {
            loadResult = LoadContentPackage(entry);
            break;
        }
        if (entry.package_type != WOTBMOD_V3_PACKAGE_NATIVE ||
            !entry.entrypoint_path[0]) {
            loadResult = WOTBMOD_ERROR_PLATFORM;
            break;
        }
        if (safeMode == 2) {
            loadResult = WOTBMOD_ERROR_DISABLED;
            break;
        }
        char packageRoot[MAX_PATH] = {};
        if (!ParentDirectory(
                entry.manifest_path,
                packageRoot,
                sizeof(packageRoot))) {
            loadResult = WOTBMOD_ERROR_PLATFORM;
            break;
        }
        PackageLoadContext context = {};
        context.expected_id = entry.id;
        context.expected_version = entry.version;
        context.package_root = packageRoot;
        context.granted_permission_tier =
            entry.granted_permission_tier;
        context.plan = &entry;
        loadResult = LoadOneMod(entry.entrypoint_path, &context);
        break;
    }
    free(packageEntries);
    return loadResult;
}

/*
 * Tear down record in place and reload it. Runs only from DrainPendingReloads()
 * on the frame-pump thread, at a point where none of the module's callbacks are
 * on the stack, so the FreeLibrary inside ResetFailedRecord() can never pull the
 * rug out from under executing mod code. ResetFailedRecord() zeroes the slot,
 * leaving a hole that every consumer (the frame loop, FindRecordByV3Handle)
 * already skips by magic; the fresh instance is appended by LoadOneMod() with a
 * new v3_handle, which is exactly what a reload means.
 */
static void PerformModReload(ModRecord* record) {
    if (!record || record->magic != kModRecordMagic) return;
    char id[MAX_PATH] = {};
    char modulePath[MAX_PATH] = {};
    CopyString(id, sizeof(id), record->settings_key);
    CopyString(modulePath, sizeof(modulePath), record->module_path);
    const bool packaged = record->is_package != 0;
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] hot reload: tearing down for reload",
        id[0] ? id : "unknown");
    UpdateSessionMarker("hot_reload_teardown", id);

    ResetFailedRecord(record);

    const WotbModResult loaded = packaged
        ? LoadPackagedModById(id)
        : LoadOneMod(modulePath, nullptr);
    if (loaded != WOTBMOD_OK) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "[%s] hot reload failed to re-load (result=%d); slot left "
            "unloaded",
            id[0] ? id : "unknown",
            static_cast<int>(loaded));
        UpdateSessionMarker("hot_reload_failed", id);
        return;
    }
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] hot reload complete",
        id[0] ? id : "unknown");
    UpdateSessionMarker("hot_reload_complete", id);
}

/*
 * Drain any reload requests that request_reload() queued since the previous
 * frame. Snapshots the count first so a freshly appended reload instance is not
 * itself re-processed in the same pass; a reload requested from inside on_load
 * (pending_reload set on the new record) is caught next frame.
 */
static void DrainPendingReloads() {
    if (!g_initialized) return;
    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        ModRecord* record = &g_mods[index];
        if (record->magic != kModRecordMagic) continue;
        if (InterlockedCompareExchange(
                &record->pending_reload, 0, 1) == 1) {
            PerformModReload(record);
        }
    }
}

/*
 * V3->legacy bridge. LifecycleRequestReload() (src/v3/runtime_services.cpp)
 * cannot own the HMODULE, so it calls here: we only flip a flag on the target's
 * record. The unload/reload itself is deferred to DrainPendingReloads() at the
 * frame boundary, so this is safe to call from any thread and from inside the
 * calling mod's own code.
 */
extern "C" WotbModV3Result WOTBMOD_CALL
WotbModRuntime_RequestModReload(WotbModV3Handle target) {
    if (!g_initialized) return WOTBMOD_V3_E_NOT_SUPPORTED;
    ModRecord* record = FindRecordByV3Handle(target);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    InterlockedExchange(&record->pending_reload, 1);
    RuntimeLog(
        WOTBMOD_LOG_INFO,
        "[%s] hot reload requested (deferred to frame boundary)",
        RecordLogId(record));
    return WOTBMOD_V3_OK;
}

/*
 * Reports whether target can be hot reloaded right now: OK if it is a known
 * loaded record with no reload already queued; a truthful error code otherwise.
 */
extern "C" WotbModV3Result WOTBMOD_CALL
WotbModRuntime_CanReloadMod(WotbModV3Handle target) {
    if (!g_initialized) return WOTBMOD_V3_E_NOT_SUPPORTED;
    ModRecord* record = FindRecordByV3Handle(target);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    if (InterlockedCompareExchange(
            &record->pending_reload, 0, 0) != 0) {
        return WOTBMOD_V3_E_CONFLICT;
    }
    return WOTBMOD_V3_OK;
}

extern "C" WotbModResult WOTBMOD_CALL WotbModRuntime_LoadAll() {
    if (!g_initialized) return WOTBMOD_ERROR_DISABLED;
    if (InterlockedCompareExchange(&g_loadedAll, 1, 0) != 0) {
        return WOTBMOD_ERROR_ALREADY_EXISTS;
    }
    const LONG safeMode =
        InterlockedCompareExchange(&g_safeMode, 0, 0);
    if (safeMode == 1) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "SAFE MODE: LoadAll completed without loading third-party "
            "packages or loose DLLs");
        return WOTBMOD_OK;
    }
    if (safeMode == 2) {
        RuntimeLog(
            WOTBMOD_LOG_WARNING,
            "SAFE MODE PORTABLE-ONLY: native packages and loose DLLs "
            "will be skipped; only content-only packages are eligible");
    }
    UpdateSessionMarker("package_preflight", nullptr);

    using namespace wotbmod::v3;
    PackagePreflightOptions packageOptions = {};
    packageOptions.struct_size = sizeof(packageOptions);
    packageOptions.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    packageOptions.flags =
        PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED | SignaturePolicyFlags(true);
    packageOptions.max_permission_tier =
        WOTBMOD_V3_PERMISSION_UNSAFE;
    CopyString(
        packageOptions.mods_root,
        sizeof(packageOptions.mods_root),
        g_modsDirectory);
    if (!JoinPath(
            packageOptions.archive_staging_root,
            sizeof(packageOptions.archive_staging_root),
            g_modsDirectory,
            "cache\\package_staging")) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "package staging path exceeds the ABI path limit");
        return WOTBMOD_ERROR_PLATFORM;
    }
    CopyString(
        packageOptions.client_build,
        sizeof(packageOptions.client_build),
        g_clientBuild);
    CopyString(
        packageOptions.client_executable_sha256,
        sizeof(packageOptions.client_executable_sha256),
        g_clientExecutableSha256);
    packageOptions.limits = DefaultPackagePreflightLimits();
    packageOptions.limits.max_candidates = kMaxMods;

    PackagePlanSummary discoverySummary = {};
    discoverySummary.struct_size = sizeof(discoverySummary);
    discoverySummary.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    WotbModV3Result packageResult = BuildPackageLoadPlan(
        &packageOptions,
        nullptr,
        0u,
        &discoverySummary);
    if (packageResult != WOTBMOD_V3_OK &&
        packageResult != WOTBMOD_V3_E_BUFFER_TOO_SMALL) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "package discovery failed (result=%d reason=%s)",
            static_cast<int>(packageResult),
            discoverySummary.reason);
        return V3ResultToLegacy(packageResult);
    }
    const uint32_t packageCapacity =
        discoverySummary.required_capacity;
    PackagePlanEntry* discoveryEntries = nullptr;
    PackagePermissionGrant* grants = nullptr;
    PackagePlanEntry* packageEntries = nullptr;
    if (packageCapacity != 0u) {
        discoveryEntries = static_cast<PackagePlanEntry*>(
            calloc(packageCapacity, sizeof(*discoveryEntries)));
        grants = static_cast<PackagePermissionGrant*>(
            calloc(packageCapacity, sizeof(*grants)));
        packageEntries = static_cast<PackagePlanEntry*>(
            calloc(packageCapacity, sizeof(*packageEntries)));
        if (!discoveryEntries || !grants || !packageEntries) {
            free(discoveryEntries);
            free(grants);
            free(packageEntries);
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "package preflight memory allocation failed");
            return WOTBMOD_ERROR_LIMIT_REACHED;
        }
        discoverySummary = {};
        discoverySummary.struct_size = sizeof(discoverySummary);
        discoverySummary.api_version =
            WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
        packageResult = BuildPackageLoadPlan(
            &packageOptions,
            discoveryEntries,
            packageCapacity,
            &discoverySummary);
        if (packageResult != WOTBMOD_V3_OK) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "package policy discovery failed "
                "(result=%d reason=%s)",
                static_cast<int>(packageResult),
                discoverySummary.reason);
            free(discoveryEntries);
            free(grants);
            free(packageEntries);
            return V3ResultToLegacy(packageResult);
        }
    }

    uint32_t grantCount = 0u;
    for (uint32_t index = 0u;
         index < discoverySummary.discovered_count; ++index) {
        const char* id = discoveryEntries[index].id;
        if (!id[0]) continue;
        bool duplicate = false;
        for (uint32_t grantIndex = 0u;
             grantIndex < grantCount; ++grantIndex) {
            if (_stricmp(grants[grantIndex].id, id) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;
        PackagePermissionGrant& grant = grants[grantCount++];
        grant.struct_size = sizeof(grant);
        grant.api_version =
            WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
        grant.max_permission_tier =
            ReadPermissionGrant(id);
        CopyString(grant.id, sizeof(grant.id), id);
    }

    PackagePlanSummary packageSummary = {};
    packageSummary.struct_size = sizeof(packageSummary);
    packageSummary.api_version =
        WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION;
    packageOptions.flags =
        PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED |
        PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES |
        SignaturePolicyFlags(false);
    packageOptions.max_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    packageOptions.permission_grants = grants;
    packageOptions.permission_grant_count = grantCount;
    packageResult = BuildPackageLoadPlan(
        &packageOptions,
        packageEntries,
        packageCapacity,
        &packageSummary);
    free(discoveryEntries);
    discoveryEntries = nullptr;
    free(grants);
    grants = nullptr;
    if (packageResult != WOTBMOD_V3_OK) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "package execution preflight failed "
            "(result=%d reason=%s)",
            static_cast<int>(packageResult),
            packageSummary.reason);
        free(packageEntries);
        return V3ResultToLegacy(packageResult);
    }

    RuntimeLog(
        packageSummary.blocked_count
            ? WOTBMOD_LOG_WARNING
            : WOTBMOD_LOG_INFO,
        "package preflight discovered=%u ready=%u blocked=%u "
        "warnings=%u client-build=%s client-sha256=%s",
        packageSummary.discovered_count,
        packageSummary.ready_count,
        packageSummary.blocked_count,
        packageSummary.warning_count,
        g_clientBuild[0] ? g_clientBuild : "unavailable",
        g_clientExecutableSha256[0]
            ? g_clientExecutableSha256
            : "unavailable");

    uint32_t packageFailures = 0u;
    uint32_t loadedPackages = 0u;
    for (uint32_t index = 0u;
         index < packageSummary.discovered_count; ++index) {
        const PackagePlanEntry& entry = packageEntries[index];
        LogPackageWarnings(entry);
        if (entry.status != PACKAGE_PLAN_READY ||
            entry.result != WOTBMOD_V3_OK) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] package blocked result=%u source=%s "
                "reason=%s",
                entry.id[0] ? entry.id : "unknown",
                entry.result,
                entry.source_path,
                entry.reason);
            ++packageFailures;
            continue;
        }
        if (IsAutoDisabledMod(entry.id)) {
            RuntimeLog(
                WOTBMOD_LOG_WARNING,
                "[%s] skipped by repeated-crash auto-disable marker %s",
                entry.id,
                g_autoDisabledPath);
            continue;
        }
        if (entry.package_type ==
            WOTBMOD_V3_PACKAGE_CONTENT_ONLY) {
            const WotbModResult loaded =
                LoadContentPackage(entry);
            if (loaded != WOTBMOD_OK) {
                ::RuntimeLog(
                    WOTBMOD_LOG_ERROR,
                    "[%s] content-only package failed to load "
                    "(result=%d descriptor=%s)",
                    entry.id,
                    static_cast<int>(loaded),
                    entry.content_path);
                ++packageFailures;
            } else {
                ++loadedPackages;
            }
            continue;
        }
        if (entry.package_type != WOTBMOD_V3_PACKAGE_NATIVE ||
            !entry.entrypoint_path[0]) {
            ::RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] ready package has no native x86 entrypoint",
                entry.id);
            ++packageFailures;
            continue;
        }
        if (safeMode == 2) {
            ::RuntimeLog(
                WOTBMOD_LOG_WARNING,
                "[%s] native package skipped in portable-only safe mode",
                entry.id);
            continue;
        }
        char packageRoot[MAX_PATH] = {};
        if (!ParentDirectory(
                entry.manifest_path,
                packageRoot,
                sizeof(packageRoot))) {
            ::RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] package root path exceeds runtime limits",
                entry.id);
            ++packageFailures;
            continue;
        }
        PackageLoadContext context = {};
        context.expected_id = entry.id;
        context.expected_version = entry.version;
        context.package_root = packageRoot;
        context.granted_permission_tier =
            entry.granted_permission_tier;
        context.plan = &entry;
        const WotbModResult loaded =
            LoadOneMod(entry.entrypoint_path, &context);
        if (loaded != WOTBMOD_OK) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] package entrypoint failed (result=%d)",
                entry.id,
                static_cast<int>(loaded));
            ++packageFailures;
        } else {
            ++loadedPackages;
        }
    }

    char searchPath[MAX_PATH] = {};
    if (!JoinPath(
            searchPath, sizeof(searchPath), g_modsDirectory, "*.dll")) {
        free(packageEntries);
        return WOTBMOD_ERROR_PLATFORM;
    }

    char paths[kMaxMods][MAX_PATH] = {};
    uint32_t pathCount = 0;
    WIN32_FIND_DATAA findData = {};
    HANDLE find = FindFirstFileA(searchPath, &findData);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                continue;
            }
            if (pathCount >= kMaxMods) {
                RuntimeLog(
                    WOTBMOD_LOG_WARNING,
                    "mod limit reached; remaining DLLs were skipped");
                break;
            }
            if (JoinPath(
                    paths[pathCount],
                    sizeof(paths[pathCount]),
                    g_modsDirectory,
                    findData.cFileName)) {
                ++pathCount;
            }
        } while (FindNextFileA(find, &findData));
        FindClose(find);
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        RuntimeLog(
            WOTBMOD_LOG_ERROR,
            "failed to enumerate %s (win32=%lu)",
            searchPath,
            GetLastError());
        free(packageEntries);
        return WOTBMOD_ERROR_PLATFORM;
    }

    qsort(paths, pathCount, sizeof(paths[0]), &ComparePaths);
    uint32_t failures = packageFailures;
    uint32_t claimedLoose = 0u;
    uint32_t attemptedLoose = 0u;
    for (uint32_t index = 0; index < pathCount; ++index) {
        if (IsClaimedPackageEntrypoint(
                paths[index],
                packageEntries,
                packageSummary.discovered_count)) {
            ++claimedLoose;
            ::RuntimeLog(
                WOTBMOD_LOG_INFO,
                "loose fallback suppressed for manifest-claimed %s",
                paths[index]);
            continue;
        }
        char looseStem[WOTBMOD_MAX_ID] = {};
        const char* looseName = strrchr(paths[index], '\\');
        GetFileStem(
            looseName ? looseName + 1 : paths[index],
            looseStem,
            sizeof(looseStem));
        if (safeMode == 2 || IsAutoDisabledMod(looseStem)) {
            RuntimeLog(
                WOTBMOD_LOG_WARNING,
                "[%s] loose DLL skipped by %s",
                looseStem,
                safeMode == 2
                    ? "portable-only safe mode"
                    : "repeated-crash auto-disable marker");
            continue;
        }
        ++attemptedLoose;
        if (LoadOneMod(paths[index], nullptr) != WOTBMOD_OK) {
            ++failures;
        }
    }
    free(packageEntries);
    RuntimeLog(
        failures ? WOTBMOD_LOG_WARNING : WOTBMOD_LOG_INFO,
        "load complete package-discovered=%u package-loaded=%u "
        "loose-discovered=%u loose-attempted=%u "
        "loose-claimed=%u loaded=%u failed=%u",
        packageSummary.discovered_count,
        loadedPackages,
        pathCount,
        attemptedLoose,
        claimedLoose,
        (uint32_t)g_modCount,
        failures);
    UpdateSessionMarker("running", nullptr);
    return failures ? WOTBMOD_ERROR_PLATFORM : WOTBMOD_OK;
}

extern "C" void WOTBMOD_CALL WotbModRuntime_DispatchFrame(
    void* swapChain,
    void* device,
    void* deviceContext,
    uint32_t backBufferWidth,
    uint32_t backBufferHeight,
    double deltaSeconds) {
    if (!g_initialized) return;

    InterlockedExchange(
        &g_dispatchThreadId,
        static_cast<LONG>(GetCurrentThreadId()));
    // Reload barrier: runs before any mod callback this frame, so tearing down
    // and reloading a module here never unloads code that is on the stack.
    DrainPendingReloads();
    DrainMainThreadWork();
    DrainClientEvents();

    WotbModFrameInfo frame = {};
    frame.struct_size = sizeof(frame);
    frame.frame_index =
        (uint64_t)InterlockedIncrement64(&g_frameIndex);
    frame.delta_seconds = ResolveDeltaSeconds(deltaSeconds);
    frame.swap_chain = swapChain;
    frame.device = device;
    frame.device_context = deviceContext;
    frame.back_buffer_width = backBufferWidth;
    frame.back_buffer_height = backBufferHeight;

    wotbmod::v3::ClientHostFrame clientFrame = {};
    clientFrame.struct_size = sizeof(clientFrame);
    clientFrame.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    clientFrame.render_backend =
        device && deviceContext && swapChain
            ? WOTBMOD_V3_RENDER_BACKEND_D3D11
            : WOTBMOD_V3_RENDER_BACKEND_NONE;
    clientFrame.frame_index = frame.frame_index;
    clientFrame.delta_seconds = frame.delta_seconds;
    clientFrame.viewport.x = 0.0f;
    clientFrame.viewport.y = 0.0f;
    clientFrame.viewport.width =
        static_cast<float>(backBufferWidth);
    clientFrame.viewport.height =
        static_cast<float>(backBufferHeight);
    clientFrame.native_device = device;
    clientFrame.native_context = deviceContext;
    clientFrame.native_swapchain = swapChain;
    wotbmod::v3::PumpClientHostFrame(&clientFrame);

    WotbModV3Runtime_DispatchFrame(
        frame.frame_index,
        frame.delta_seconds);

    const LONG count = g_modCount;
    for (LONG index = 0; index < count; ++index) {
        ModRecord* record = &g_mods[index];
        if (record->enabled &&
            record->state == WOTBMOD_STATE_ENABLED) {
            CallFrame(record, &frame);
        }
    }
    // Lua and native on_frame callbacks can create, move, hide, or destroy UI.
    // Publish their final geometry before Windows input for the next frame can
    // be captured, so a newly mounted panel never has a click-through frame.
    wotbmod::v3::RefreshClientHostUiCaptureSnapshot();
}

extern "C" void WOTBMOD_CALL WotbModRuntime_Shutdown() {
    if (InterlockedCompareExchange(&g_initialized, 0, 1) != 1) return;

    UpdateSessionMarker("shutdown_begin", nullptr);
    const LONG count = g_modCount;
    for (LONG index = count - 1; index >= 0; --index) {
        ModRecord* record = &g_mods[index];
        UpdateSessionMarker("shutdown_mod", RecordLogId(record));
        RemoveOwnedEventSubscriptions(record);
        RemoveOwnedMainThreadWork(record);
        if (record->enabled) {
            InterlockedExchange(&record->enabled, 0);
            CallDisable(record);
        }
        RemoveOwnedVehicles(record);
        const WotbModResult hookCleanup = RemoveOwnedHooks(record);
        const WotbModResult soundCleanup = RemoveOwnedSoundEvents(record);
        const WotbModResult audioCleanup = RemoveOwnedAudio(record);
        RemoveOwnedVehicleSkins(record);
        RemoveOwnedResources(record);
        CallUnload(record);
        const WotbModResult finalHookCleanup =
            hookCleanup == WOTBMOD_OK
                ? WOTBMOD_OK
                : RemoveOwnedHooks(record);
        const WotbModResult finalAudioCleanup =
            audioCleanup == WOTBMOD_OK
                ? WOTBMOD_OK
                : RemoveOwnedAudio(record);
        const WotbModResult finalSoundCleanup =
            soundCleanup == WOTBMOD_OK
                ? WOTBMOD_OK
                : RemoveOwnedSoundEvents(record);
        RemoveOwnedVehicleSkins(record);
        RemoveOwnedResources(record);
        HMODULE module = record->module;
        record->module = nullptr;
        record->magic = 0;
        if (module && finalHookCleanup == WOTBMOD_OK) {
            FreeLibrary(module);
        } else if (module) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] module retained because hook cleanup failed",
                RecordLogId(record));
        }
        if (finalAudioCleanup != WOTBMOD_OK) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] audio playback cleanup failed (result=%d)",
                RecordLogId(record),
                (int)finalAudioCleanup);
        }
        if (finalSoundCleanup != WOTBMOD_OK) {
            RuntimeLog(
                WOTBMOD_LOG_ERROR,
                "[%s] native sound cleanup failed (result=%d)",
                RecordLogId(record),
                (int)finalSoundCleanup);
        }
    }

    ClearPendingClientEvents();
    wotbmod::v3::RemoveDavaNativeBackend();
    WotbModV3Runtime_Shutdown();
    wotbmod::v3::SetClientHostBackend(nullptr);
    DeleteOwnedSessionMarker();
    InterlockedExchange(&g_crashLoopGuardPrepared, 0);
    ZeroMemory(g_mods, sizeof(g_mods));
    InterlockedExchange(&g_modCount, 0);
    InterlockedExchange(&g_loadedAll, 0);
    InterlockedExchange64(&g_frameIndex, 0);
    g_gameModule = nullptr;
    g_logSink = nullptr;
    g_logUserData = nullptr;
    ZeroMemory(&g_hookBackend, sizeof(g_hookBackend));
    ZeroMemory(&g_resourceBackend, sizeof(g_resourceBackend));
    ZeroMemory(&g_audioBackend, sizeof(g_audioBackend));
    ZeroMemory(&g_soundBackend, sizeof(g_soundBackend));
    ZeroMemory(&g_gameplayBackend, sizeof(g_gameplayBackend));
    ZeroMemory(&g_v3ClientBackend, sizeof(g_v3ClientBackend));
    g_runtimeOptionFlags = 0u;
    g_v3LocalVehicleId = 0u;
    InterlockedExchange(&g_safeMode, 0);
    ZeroMemory(g_clientBuild, sizeof(g_clientBuild));
    ZeroMemory(
        g_clientExecutableSha256,
        sizeof(g_clientExecutableSha256));
    ZeroMemory(g_sessionMarkerPath, sizeof(g_sessionMarkerPath));
    ZeroMemory(g_sessionMarkerTempPath, sizeof(g_sessionMarkerTempPath));
    ZeroMemory(g_sessionMarkerToken, sizeof(g_sessionMarkerToken));
    ZeroMemory(g_safeModeLastPhase, sizeof(g_safeModeLastPhase));
    ZeroMemory(g_safeModeLastMod, sizeof(g_safeModeLastMod));
    ZeroMemory(
        g_safeModeLastCallbackOwner,
        sizeof(g_safeModeLastCallbackOwner));
    ZeroMemory(g_safeModeLastCallback, sizeof(g_safeModeLastCallback));
    ZeroMemory(
        g_safeModeLastNativeBinding,
        sizeof(g_safeModeLastNativeBinding));
    ZeroMemory(g_safeModeLastEvent, sizeof(g_safeModeLastEvent));
    ZeroMemory(g_safeModeLastAsync, sizeof(g_safeModeLastAsync));
    ZeroMemory(
        g_safeModeLastResourceTransaction,
        sizeof(g_safeModeLastResourceTransaction));
    ZeroMemory(g_crashHistoryPath, sizeof(g_crashHistoryPath));
    ZeroMemory(g_autoDisabledPath, sizeof(g_autoDisabledPath));
    ZeroMemory(g_autoDisabledMod, sizeof(g_autoDisabledMod));
    ZeroMemory(g_resourceMounts, sizeof(g_resourceMounts));
    AcquireSRWLockExclusive(&g_mainThreadWorkLock);
    ZeroMemory(g_mainThreadWork, sizeof(g_mainThreadWork));
    ReleaseSRWLockExclusive(&g_mainThreadWorkLock);
    InterlockedExchange64(&g_nextResourceMountId, 0);
    InterlockedExchange(&g_nextResourceHandleId, 0);
    InterlockedExchange(&g_nextAudioPlaybackId, 0);
    InterlockedExchange(&g_nextSoundEventId, 0);
    InterlockedExchange64(&g_resourceGeneration, 0);
    InterlockedExchange64(&g_nextMainThreadSequence, 0);
    InterlockedExchange64(&g_nextClientEventSequence, 0);
    InterlockedExchange64(&g_nextEventSubscriptionId, 0);
    InterlockedExchange(&g_nextVehicleHandleId, 0);
    InterlockedExchange(&g_nextVehicleSkinHandleId, 0);
    InterlockedExchange64(&g_nextVehicleSkinSequence, 0);
    InterlockedExchange(&g_dispatchThreadId, 0);
}

extern "C" const WotbModHostApi* WOTBMOD_CALL
WotbModRuntime_GetHostApi() {
    return &g_hostApi;
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_ResolveResourcePath(
    const char* requestedPath,
    char* buffer,
    uint32_t* inoutSize) {
    if (!g_initialized) return WOTBMOD_ERROR_DISABLED;
    return ResolveResourcePathInternal(requestedPath, buffer, inoutSize);
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_ResolveVehicleSkinPath(
    const char* requestedPath,
    char* buffer,
    uint32_t* inoutSize) {
    if (!g_initialized) return WOTBMOD_ERROR_DISABLED;
    return ResolveVehicleSkinPathInternal(
        requestedPath, buffer, inoutSize);
}

extern "C" uint64_t WOTBMOD_CALL
WotbModRuntime_GetResourceGeneration() {
    return (uint64_t)InterlockedCompareExchange64(
        &g_resourceGeneration, 0, 0);
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_NotifyUiScreenChanged(
    void* previousNativeResource,
    void* nativeResource) {
    return QueueClientEvent(
        WOTBMOD_EVENT_UI_SCREEN_CHANGED,
        WOTBMOD_RESOURCE_UI_CONTROL,
        previousNativeResource,
        nativeResource,
        0,
        0,
        0,
        0,
        nullptr);
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_NotifySceneActivated(
    void* nativeResource) {
    return QueueClientEvent(
        WOTBMOD_EVENT_SCENE_ACTIVATED,
        WOTBMOD_RESOURCE_SCENE,
        nullptr,
        nativeResource,
        0,
        0,
        0,
        0,
        nullptr);
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_NotifySceneDeactivated(
    void* nativeResource) {
    return QueueClientEvent(
        WOTBMOD_EVENT_SCENE_DEACTIVATED,
        WOTBMOD_RESOURCE_SCENE,
        nativeResource,
        nullptr,
        0,
        0,
        0,
        0,
        nullptr);
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModRuntime_NotifyClientEvent(
    const WotbModRuntimeClientEvent* event) {
    const size_t requiredSize =
        offsetof(WotbModRuntimeClientEvent, payload) +
        sizeof(event->payload);
    if (!event ||
        event->struct_size < requiredSize ||
        event->payload_size > sizeof(event->payload)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    return QueueClientEvent(
        static_cast<WotbModClientEventType>(event->type),
        event->resource_type,
        event->previous_native_resource,
        event->native_resource,
        event->flags,
        event->primary_entity_id,
        event->other_entity_id,
        event->payload_size,
        event->payload_size ? &event->payload : nullptr);
}

extern "C" const WotbModHostApi* WOTBMOD_CALL
WotbModApi_GetHost() {
    return &g_hostApi;
}

extern "C" uint32_t WOTBMOD_CALL
WotbModApi_GetVersion() {
    return WOTBMOD_ABI_VERSION;
}
