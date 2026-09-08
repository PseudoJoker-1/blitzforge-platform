#include "../include/wotb_mod_runtime_v3.h"
#include "../include/wotb_mod_dava_native.h"
#include "../src/v3/data_services_backend.h"
#include "../src/v3/dava_native_registry.h"
#include "../src/v3/wotb_mod_v3_internal.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wotbmod {
namespace v3 {

void RegisterClientServices() {}
void RegisterToolingServices() {}
/* runtime_services.cpp pumps the HUD from the main thread; the HUD lives in
 * client_services.cpp, which this test does not compile. */
void HudMainThreadTick() {}

}  // namespace v3
}  // namespace wotbmod

namespace {

namespace fs = std::filesystem;

std::atomic<uint32_t> g_input_callback_count{0u};
std::atomic<int32_t> g_input_callback_value_milli{0};
std::atomic<uint32_t> g_input_callback_pressed{0u};
std::atomic<uint32_t> g_blocking_workers_started{0u};
std::atomic<bool> g_release_blocking_workers{false};
uint32_t g_check_count = 0u;

struct CapturedWatchEvent {
    std::string topic;
    WotbModV3VfsWatchEvent vfs = {};
    WotbModV3ResourceWatchEvent resource = {};
};

std::vector<CapturedWatchEvent> g_watch_events;

struct NativeSnapshotFake {
    uint32_t release_count = 0u;
    bool bad_crc = false;
    bool case_prefix_collision = false;
};

WotbModV3Result NativeSnapshotCopy(
    const void* source,
    uint32_t size,
    WotbModDavaNativeBuffer* buffer) {
    if (!buffer || buffer->struct_size < sizeof(*buffer)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    buffer->size = size;
    if (!buffer->data || buffer->capacity < size) {
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    if (size != 0u) std::memcpy(buffer->data, source, size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeSnapshotYamlParse(
    void*,
    WotbModV3Handle owner,
    const char* path,
    WotbModDavaNativeProviderToken* out_token) {
    if (owner == WOTBMOD_V3_INVALID_HANDLE || !path || !path[0] ||
        !out_token) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_token = 101u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeSnapshotYamlExport(
    void*,
    WotbModV3Handle,
    WotbModDavaNativeProviderToken token,
    WotbModDavaNativeBuffer* buffer) {
    static const char kYaml[] = "native_snapshot: true\n";
    return token == 101u
        ? NativeSnapshotCopy(kYaml, sizeof(kYaml) - 1u, buffer)
        : WOTBMOD_V3_E_INVALID_HANDLE;
}

WotbModV3Result WOTBMOD_V3_CALL NativeSnapshotArchiveOpen(
    void*,
    WotbModV3Handle owner,
    const char* path,
    WotbModDavaNativeProviderToken* out_token) {
    if (owner == WOTBMOD_V3_INVALID_HANDLE || !path || !path[0] ||
        !out_token) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_token = 202u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeSnapshotArchiveCount(
    void* user_data,
    WotbModV3Handle,
    WotbModDavaNativeProviderToken token,
    uint32_t* out_count) {
    if (token != 202u || !out_count) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    NativeSnapshotFake* fake =
        static_cast<NativeSnapshotFake*>(user_data);
    if (!fake) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_count = fake->case_prefix_collision ? 2u : 1u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeSnapshotArchiveEntry(
    void* user_data,
    WotbModV3Handle,
    WotbModDavaNativeProviderToken token,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* out_entry) {
    NativeSnapshotFake* fake =
        static_cast<NativeSnapshotFake*>(user_data);
    if (!fake || token != 202u || index > 1u ||
        (index == 1u && !fake->case_prefix_collision) || !out_entry ||
        out_entry->struct_size < sizeof(*out_entry)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    out_entry->index = index;
    out_entry->original_size = 5u;
    out_entry->original_crc32 =
        fake->bad_crc ? 0u : 0x2df9f80fu;
    strcpy_s(
        out_entry->relative_path,
        sizeof(out_entry->relative_path),
        index == 0u && fake->case_prefix_collision
            ? "Native" :
        index == 1u
            ? "native/value.bin" : "native/value.bin");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL NativeSnapshotArchiveRead(
    void*,
    WotbModV3Handle,
    WotbModDavaNativeProviderToken token,
    uint32_t index,
    WotbModDavaNativeBuffer* buffer) {
    static const uint8_t kBytes[] = {9u, 8u, 7u, 6u, 5u};
    return token == 202u && index <= 1u
        ? NativeSnapshotCopy(kBytes, sizeof(kBytes), buffer)
        : WOTBMOD_V3_E_INVALID_HANDLE;
}

WotbModV3Result WOTBMOD_V3_CALL NativeSnapshotRelease(
    void* user_data,
    WotbModV3Handle,
    uint32_t kind,
    WotbModDavaNativeProviderToken token) {
    NativeSnapshotFake* fake =
        static_cast<NativeSnapshotFake*>(user_data);
    const bool valid =
        (kind == WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT &&
         token == 101u) ||
        (kind == WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE &&
         token == 202u);
    if (!fake || !valid) return WOTBMOD_V3_E_INVALID_HANDLE;
    ++fake->release_count;
    return WOTBMOD_V3_OK;
}

WotbModDavaNativeBackend NativeSnapshotBackend(
    NativeSnapshotFake* fake) {
    WotbModDavaNativeBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version = WOTBMOD_DAVA_NATIVE_BACKEND_VERSION;
    backend.binding_pack_version = 111900834u;
    backend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED;
    backend.capabilities =
        WOTBMOD_DAVA_NATIVE_CAP_YAML |
        WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE;
    backend.user_data = fake;
    backend.yaml_parse_file = &NativeSnapshotYamlParse;
    backend.yaml_export_utf8 = &NativeSnapshotYamlExport;
    backend.archive_open_file = &NativeSnapshotArchiveOpen;
    backend.archive_get_entry_count = &NativeSnapshotArchiveCount;
    backend.archive_get_entry = &NativeSnapshotArchiveEntry;
    backend.archive_read_entry = &NativeSnapshotArchiveRead;
    backend.release = &NativeSnapshotRelease;
    return backend;
}

void Copy(char* destination, size_t capacity, const char* source) {
    strncpy_s(destination, capacity, source, _TRUNCATE);
}

WotbModV3Result WOTBMOD_V3_CALL TestEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.data-truth");
    Copy(info->name, sizeof(info->name), "Data truth regression");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ObserverEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.data-observer");
    Copy(info->name, sizeof(info->name), "Data watch observer");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL StoppedOwnerEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.data-stopped-owner");
    Copy(info->name, sizeof(info->name), "Stopped data owner");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ToggleOwnerEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.data-toggle-owner");
    Copy(info->name, sizeof(info->name), "Toggled data owner");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

// Builds the one desync that leaves a v3 record ENABLED while the legacy
// facade already believes the mod is disabled. on_frame is dispatched
// under EnterModCallbackCoalescible, which sets g_callback_mod and raises
// g_callback_nesting, so a self-disable from inside it hits the E_BUSY
// guard in WotbModV3Runtime_Disable -- and that guard returns before it
// touches `state` or `accepting_callbacks`.
std::atomic<WotbModV3Result> g_self_disable_result{WOTBMOD_V3_OK};
std::atomic<uint32_t> g_self_disable_frames{0u};

void WOTBMOD_V3_CALL SelfDisableOnFrame(
    const WotbModV3Bootstrap*,
    WotbModV3Handle mod,
    uint64_t,
    double) {
    g_self_disable_frames.fetch_add(1u);
    g_self_disable_result.store(WotbModV3Runtime_Disable(mod));
}

WotbModV3Result WOTBMOD_V3_CALL SelfDisableEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.data-self-disable");
    Copy(info->name, sizeof(info->name), "Self-disabling owner");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    info->on_frame = &SelfDisableOnFrame;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL SettingsHolderEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* info) {
    if (!info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    info->struct_size = sizeof(*info);
    info->api_version = WOTBMOD_V3_ABI_VERSION;
    info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    Copy(info->id, sizeof(info->id), "tests.data-settings-holder");
    Copy(info->name, sizeof(info->name), "Settings lock holder");
    Copy(info->version, sizeof(info->version), "1.0.0");
    Copy(info->author, sizeof(info->author), "tests");
    return WOTBMOD_V3_OK;
}

void WOTBMOD_V3_CALL InputCallback(
    WotbModV3Handle,
    WotbModV3Handle,
    float value,
    uint32_t pressed,
    void*) {
    g_input_callback_value_milli.store(
        static_cast<int32_t>(value * 1000.0f));
    g_input_callback_pressed.store(pressed);
    ++g_input_callback_count;
}

WotbModV3Result WOTBMOD_V3_CALL BlockingWorker(
    WotbModV3Handle,
    WotbModV3TaskHandle,
    void*) {
    ++g_blocking_workers_started;
    while (!g_release_blocking_workers.load()) {
        std::this_thread::yield();
    }
    return WOTBMOD_V3_OK;
}

bool WaitUntil(
    const std::function<bool()>& predicate,
    uint32_t timeout_ms = 5000u) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1));
    }
    return predicate();
}

void WOTBMOD_V3_CALL WatchEventCallback(
    WotbModV3Handle,
    WotbModV3Event* event,
    void*) {
    if (!event || !event->payload) return;
    CapturedWatchEvent captured;
    captured.topic = event->topic;
    if (captured.topic == WOTBMOD_V3_EVENT_VFS_INVALIDATED &&
        event->payload_size == sizeof(captured.vfs)) {
        std::memcpy(
            &captured.vfs,
            event->payload,
            sizeof(captured.vfs));
        g_watch_events.push_back(std::move(captured));
    } else if (
        (captured.topic == WOTBMOD_V3_EVENT_RESOURCE_INVALIDATED ||
         captured.topic == WOTBMOD_V3_EVENT_RESOURCE_RELOADED) &&
        event->payload_size == sizeof(captured.resource)) {
        std::memcpy(
            &captured.resource,
            event->payload,
            sizeof(captured.resource));
        g_watch_events.push_back(std::move(captured));
    }
}

bool WriteFile(const fs::path& path, const std::string& bytes) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(
        bytes.data(),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

uint32_t TestZipCrc32(
    const uint8_t* bytes,
    size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t index = 0u; index < size; ++index) {
        crc ^= bytes[index];
        for (uint32_t bit = 0u; bit < 8u; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1u) ^
                  (0xedb88320u & mask);
        }
    }
    return ~crc;
}

void ZipPush16(
    std::vector<uint8_t>* bytes,
    uint16_t value) {
    bytes->push_back(
        static_cast<uint8_t>(value & 0xffu));
    bytes->push_back(
        static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void ZipPush32(
    std::vector<uint8_t>* bytes,
    uint32_t value) {
    for (uint32_t shift = 0u;
         shift < 32u; shift += 8u) {
        bytes->push_back(static_cast<uint8_t>(
            (value >> shift) & 0xffu));
    }
}

struct TestZipEntry {
    std::string name;
    std::string data;
    uint16_t flags = 0u;
    uint16_t method = 0u;
    uint16_t version_made_by = 20u;
    uint16_t start_disk = 0u;
    uint32_t external_attributes = 0u;
    uint16_t local_flags_override = 0xffffu;
    bool corrupt_crc = false;
    std::vector<uint8_t> central_extra;
    std::vector<uint8_t> local_extra;
};

struct TestZipOptions {
    uint16_t disk = 0u;
    uint16_t central_disk = 0u;
    bool zip64_central_size = false;
};

struct TestZipLayout {
    std::vector<uint64_t> data_offsets;
};

struct PreparedTestZipEntry {
    TestZipEntry source;
    uint32_t local_offset = 0u;
    uint32_t crc32 = 0u;
};

bool WriteTestZip(
    const fs::path& path,
    const std::vector<TestZipEntry>& source_entries,
    const TestZipOptions& options = {},
    TestZipLayout* out_layout = nullptr) {
    if (source_entries.size() > 0xffffu) return false;
    std::vector<uint8_t> bytes;
    std::vector<PreparedTestZipEntry> entries;
    entries.reserve(source_entries.size());
    if (out_layout) {
        out_layout->data_offsets.clear();
        out_layout->data_offsets.reserve(
            source_entries.size());
    }
    for (const TestZipEntry& source : source_entries) {
        if (source.name.size() > 0xffffu ||
            source.data.size() >
                (std::numeric_limits<uint32_t>::max)() ||
            source.local_extra.size() > 0xffffu ||
            source.central_extra.size() > 0xffffu ||
            bytes.size() >
                (std::numeric_limits<uint32_t>::max)()) {
            return false;
        }
        PreparedTestZipEntry entry;
        entry.source = source;
        entry.local_offset =
            static_cast<uint32_t>(bytes.size());
        entry.crc32 = TestZipCrc32(
            reinterpret_cast<const uint8_t*>(
                source.data.data()),
            source.data.size());
        if (source.corrupt_crc) {
            entry.crc32 ^= 1u;
        }
        const uint16_t local_flags =
            source.local_flags_override == 0xffffu
            ? source.flags
            : source.local_flags_override;
        ZipPush32(&bytes, 0x04034b50u);
        ZipPush16(&bytes, 20u);
        ZipPush16(&bytes, local_flags);
        ZipPush16(&bytes, source.method);
        ZipPush16(&bytes, 0u);
        ZipPush16(&bytes, 0u);
        ZipPush32(&bytes, entry.crc32);
        ZipPush32(
            &bytes,
            static_cast<uint32_t>(source.data.size()));
        ZipPush32(
            &bytes,
            static_cast<uint32_t>(source.data.size()));
        ZipPush16(
            &bytes,
            static_cast<uint16_t>(source.name.size()));
        ZipPush16(
            &bytes,
            static_cast<uint16_t>(
                source.local_extra.size()));
        bytes.insert(
            bytes.end(),
            source.name.begin(),
            source.name.end());
        bytes.insert(
            bytes.end(),
            source.local_extra.begin(),
            source.local_extra.end());
        if (out_layout) {
            out_layout->data_offsets.push_back(
                bytes.size());
        }
        bytes.insert(
            bytes.end(),
            source.data.begin(),
            source.data.end());
        entries.push_back(std::move(entry));
    }

    if (bytes.size() >
        (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    const uint32_t central_offset =
        static_cast<uint32_t>(bytes.size());
    for (const PreparedTestZipEntry& entry : entries) {
        const TestZipEntry& source = entry.source;
        ZipPush32(&bytes, 0x02014b50u);
        ZipPush16(&bytes, source.version_made_by);
        ZipPush16(&bytes, 20u);
        ZipPush16(&bytes, source.flags);
        ZipPush16(&bytes, source.method);
        ZipPush16(&bytes, 0u);
        ZipPush16(&bytes, 0u);
        ZipPush32(&bytes, entry.crc32);
        ZipPush32(
            &bytes,
            static_cast<uint32_t>(source.data.size()));
        ZipPush32(
            &bytes,
            static_cast<uint32_t>(source.data.size()));
        ZipPush16(
            &bytes,
            static_cast<uint16_t>(source.name.size()));
        ZipPush16(
            &bytes,
            static_cast<uint16_t>(
                source.central_extra.size()));
        ZipPush16(&bytes, 0u);
        ZipPush16(&bytes, source.start_disk);
        ZipPush16(&bytes, 0u);
        ZipPush32(&bytes, source.external_attributes);
        ZipPush32(&bytes, entry.local_offset);
        bytes.insert(
            bytes.end(),
            source.name.begin(),
            source.name.end());
        bytes.insert(
            bytes.end(),
            source.central_extra.begin(),
            source.central_extra.end());
    }
    if (bytes.size() <
            static_cast<size_t>(central_offset) ||
        bytes.size() -
            static_cast<size_t>(central_offset) >
            (std::numeric_limits<uint32_t>::max)()) {
        return false;
    }
    const uint32_t central_size =
        static_cast<uint32_t>(
            bytes.size() -
            static_cast<size_t>(central_offset));
    ZipPush32(&bytes, 0x06054b50u);
    ZipPush16(&bytes, options.disk);
    ZipPush16(&bytes, options.central_disk);
    ZipPush16(
        &bytes,
        static_cast<uint16_t>(entries.size()));
    ZipPush16(
        &bytes,
        static_cast<uint16_t>(entries.size()));
    ZipPush32(
        &bytes,
        options.zip64_central_size
            ? 0xffffffffu
            : central_size);
    ZipPush32(&bytes, central_offset);
    ZipPush16(&bytes, 0u);

    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

bool OverwriteByte(
    const fs::path& path,
    uint64_t offset,
    uint8_t value) {
    if (offset >
        static_cast<uint64_t>(
            (std::numeric_limits<std::streamoff>::max)())) {
        return false;
    }
    std::fstream file(
        path,
        std::ios::binary |
        std::ios::in |
        std::ios::out);
    if (!file) return false;
    file.seekp(
        static_cast<std::streamoff>(offset),
        std::ios::beg);
    if (!file) return false;
    file.put(static_cast<char>(value));
    file.flush();
    return static_cast<bool>(file);
}

WotbModV3ArchiveLimits TestArchiveLimits(
    uint32_t max_depth,
    uint32_t max_files,
    uint64_t max_total,
    uint64_t max_single) {
    WotbModV3ArchiveLimits limits = {};
    limits.struct_size = sizeof(limits);
    limits.api_version = WOTBMOD_V3_ARCHIVE_VERSION;
    limits.max_depth = max_depth;
    limits.max_files = max_files;
    limits.max_total_unpacked_bytes = max_total;
    limits.max_single_file_bytes = max_single;
    return limits;
}

bool OpenPackageReturns(
    const WotbModV3ArchiveApiV1* api,
    WotbModV3Handle mod,
    const fs::path& package,
    const WotbModV3ArchiveLimits* limits,
    WotbModV3Result expected) {
    if (!api) return false;
    const std::string package_text = package.string();
    WotbModV3ArchiveHandle archive = UINT64_MAX;
    const WotbModV3Result result =
        api->open_package_file(
            mod,
            package_text.c_str(),
            limits,
            &archive);
    return result == expected &&
           archive == WOTBMOD_V3_INVALID_HANDLE;
}

}  // namespace

#define CHECK(expression)                                                \
    do {                                                                 \
        ++g_check_count;                                                  \
        if (!(expression)) {                                             \
            std::fprintf(                                                 \
                stderr,                                                   \
                "check failed at line %d: %s\n",                        \
                __LINE__,                                                 \
                #expression);                                             \
            WotbModV3Runtime_Shutdown();                                  \
            return 1;                                                     \
        }                                                                \
    } while (0)

int main() {
    const fs::path environment =
        fs::absolute(fs::path("build") / "v3_data_truth_env");
    std::error_code error;
    fs::remove_all(environment, error);
    CHECK(!error);

    const fs::path mods = environment / "mods";
    const fs::path cache = environment / "cache";
    const fs::path config = environment / "config";
    const std::string environment_text = environment.string();
    const std::string mods_text = mods.string();
    const std::string cache_text = cache.string();
    const std::string config_text = config.string();

    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = environment_text.c_str();
    options.mods_directory = mods_text.c_str();
    options.cache_directory = cache_text.c_str();
    options.config_directory = config_text.c_str();
    options.client_version = "test";
    CHECK(WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK);

    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "data_truth_test.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &mod) == WOTBMOD_V3_OK);
    WotbModV3RuntimeModuleInfo module = {};
    module.struct_size = sizeof(module);
    module.api_version = WOTBMOD_V3_ABI_VERSION;
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            mod, &TestEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(mod) == WOTBMOD_V3_OK);

    WotbModV3Handle observer = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "data_watch_observer.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &observer) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            observer, &ObserverEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(observer) == WOTBMOD_V3_OK);

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);

    const void* raw_handles = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_HANDLES,
            WOTBMOD_V3_HANDLES_VERSION,
            &raw_handles) == WOTBMOD_V3_OK);
    const WotbModV3HandlesApiV1* handles =
        static_cast<const WotbModV3HandlesApiV1*>(raw_handles);
    CHECK(handles != nullptr);

    const void* raw_input = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_INPUT,
            WOTBMOD_V3_INPUT_VERSION,
            &raw_input) == WOTBMOD_V3_OK);
    const WotbModV3InputApiV1* input =
        static_cast<const WotbModV3InputApiV1*>(raw_input);
    CHECK(input != nullptr);

    WotbModV3InputActionDesc action_desc = {};
    action_desc.struct_size = sizeof(action_desc);
    action_desc.api_version = WOTBMOD_V3_INPUT_VERSION;
    action_desc.value_type = WOTBMOD_V3_INPUT_VALUE_BUTTON;
    action_desc.contexts = WOTBMOD_V3_CONTEXT_ALL;
    WotbModV3InputBinding action_binding = {};
    action_binding.struct_size = sizeof(action_binding);
    action_binding.api_version = WOTBMOD_V3_INPUT_VERSION;
    action_binding.device = WOTBMOD_V3_INPUT_DEVICE_KEYBOARD;
    action_binding.code = 0x74u;  // F5
    action_binding.scale = 1.0f;
    action_desc.default_bindings = &action_binding;
    action_desc.default_binding_count = 1u;
    Copy(action_desc.id, sizeof(action_desc.id), "tests.data-truth.action");
    Copy(
        action_desc.display_name,
        sizeof(action_desc.display_name),
        "Data truth action");

    WotbModV3Handle action = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        input->register_action(mod, &action_desc, &action) ==
        WOTBMOD_V3_OK);
    CHECK(action != WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3Token token = UINT64_MAX;
    CHECK(
        input->subscribe(
            mod,
            action,
            &InputCallback,
            nullptr,
            &token) == WOTBMOD_V3_OK);
    CHECK(token != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(g_input_callback_count.load() == 0u);

    CHECK(
        wotbmod::v3::NotifyNativeInput(
            wotbmod::v3::NATIVE_INPUT_EVENT_BUTTON,
            WOTBMOD_V3_INPUT_DEVICE_KEYBOARD,
            0x74u,
            WOTBMOD_V3_INPUT_MOD_NONE,
            1.0f,
            1u) == WOTBMOD_V3_OK);
    WotbModV3Runtime_DispatchFrame(100u, 1.0 / 60.0);
    CHECK(g_input_callback_count.load() == 1u);
    CHECK(g_input_callback_value_milli.load() == 1000);
    CHECK(g_input_callback_pressed.load() == 1u);
    uint32_t action_state = 0u;
    CHECK(input->is_action_down(mod, action, &action_state) ==
          WOTBMOD_V3_OK);
    CHECK(action_state == 1u);
    CHECK(input->is_action_pressed(mod, action, &action_state) ==
          WOTBMOD_V3_OK);
    CHECK(action_state == 1u);
    WotbModV3Runtime_DispatchFrame(101u, 1.0 / 60.0);
    CHECK(input->is_action_pressed(mod, action, &action_state) ==
          WOTBMOD_V3_OK);
    CHECK(action_state == 0u);

    CHECK(
        wotbmod::v3::NotifyNativeInput(
            wotbmod::v3::NATIVE_INPUT_EVENT_BUTTON,
            WOTBMOD_V3_INPUT_DEVICE_KEYBOARD,
            0x74u,
            WOTBMOD_V3_INPUT_MOD_NONE,
            0.0f,
            0u) == WOTBMOD_V3_OK);
    WotbModV3Runtime_DispatchFrame(102u, 1.0 / 60.0);
    CHECK(g_input_callback_count.load() == 2u);
    CHECK(g_input_callback_pressed.load() == 0u);
    CHECK(input->is_action_down(mod, action, &action_state) ==
          WOTBMOD_V3_OK);
    CHECK(action_state == 0u);

    CHECK(input->capture_begin(mod, WOTBMOD_V3_CONTEXT_ALL) ==
          WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyNativeInput(
            wotbmod::v3::NATIVE_INPUT_EVENT_BUTTON,
            WOTBMOD_V3_INPUT_DEVICE_KEYBOARD,
            0x41u,
            WOTBMOD_V3_INPUT_MOD_CONTROL,
            1.0f,
            1u) == WOTBMOD_V3_OK);
    WotbModV3Runtime_DispatchFrame(103u, 1.0 / 60.0);
    WotbModV3InputBinding captured_binding = {};
    CHECK(input->capture_end(mod, &captured_binding) ==
          WOTBMOD_V3_OK);
    CHECK(captured_binding.device ==
          WOTBMOD_V3_INPUT_DEVICE_KEYBOARD);
    CHECK(captured_binding.code == 0x41u);
    CHECK(captured_binding.modifiers ==
          WOTBMOD_V3_INPUT_MOD_CONTROL);

    WotbModV3InputActionDesc axis_desc = {};
    axis_desc.struct_size = sizeof(axis_desc);
    axis_desc.api_version = WOTBMOD_V3_INPUT_VERSION;
    axis_desc.value_type = WOTBMOD_V3_INPUT_VALUE_AXIS;
    axis_desc.contexts = WOTBMOD_V3_CONTEXT_ALL;
    Copy(axis_desc.id, sizeof(axis_desc.id),
         "tests.data-truth.wheel");
    Copy(axis_desc.display_name, sizeof(axis_desc.display_name),
         "Data truth wheel");
    WotbModV3InputBinding axis_binding = {};
    axis_binding.struct_size = sizeof(axis_binding);
    axis_binding.api_version = WOTBMOD_V3_INPUT_VERSION;
    axis_binding.device = WOTBMOD_V3_INPUT_DEVICE_MOUSE;
    axis_binding.code = WOTBMOD_V3_INPUT_MOUSE_WHEEL;
    axis_binding.scale = 2.0f;
    axis_desc.default_bindings = &axis_binding;
    axis_desc.default_binding_count = 1u;
    WotbModV3Handle axis_action = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(input->register_action(mod, &axis_desc, &axis_action) ==
          WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::NotifyNativeInput(
            wotbmod::v3::NATIVE_INPUT_EVENT_AXIS,
            WOTBMOD_V3_INPUT_DEVICE_MOUSE,
            WOTBMOD_V3_INPUT_MOUSE_WHEEL,
            WOTBMOD_V3_INPUT_MOD_NONE,
            1.5f,
            1u) == WOTBMOD_V3_OK);
    WotbModV3Runtime_DispatchFrame(104u, 1.0 / 60.0);
    float axis_value = 0.0f;
    CHECK(input->get_axis(mod, axis_action, &axis_value) ==
          WOTBMOD_V3_OK);
    CHECK(axis_value == 3.0f);
    WotbModV3Runtime_DispatchFrame(105u, 1.0 / 60.0);
    CHECK(input->get_axis(mod, axis_action, &axis_value) ==
          WOTBMOD_V3_OK);
    CHECK(axis_value == 0.0f);

    CHECK(handles->release(mod, token) == WOTBMOD_V3_OK);
    CHECK(input->unregister_action(mod, action) == WOTBMOD_V3_OK);
    CHECK(input->unregister_action(mod, axis_action) == WOTBMOD_V3_OK);

    const fs::path owned_root =
        mods / "data" / "data_truth_test";
    const fs::path archive_root = owned_root / "archive-source";
    CHECK(
        WriteFile(
            archive_root / "nested" / "value.txt",
            "verified-directory-archive"));
    const fs::path unsupported_package =
        owned_root / "unsupported.wotbmod";
    CHECK(WriteFile(unsupported_package, "not-a-real-package"));
    const fs::path stored_package =
        owned_root / "stored.wotbmod";
    TestZipLayout stored_layout;
    CHECK(
        WriteTestZip(
            stored_package,
            {
                {"dir/", ""},
                {"dir/value.txt", "stored-package-value"},
                {"empty.bin", ""}
            },
            {},
            &stored_layout));

    const void* raw_archive = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_ARCHIVE,
            WOTBMOD_V3_ARCHIVE_VERSION,
            &raw_archive) == WOTBMOD_V3_OK);
    const WotbModV3ArchiveApiV1* archive_api =
        static_cast<const WotbModV3ArchiveApiV1*>(raw_archive);
    CHECK(archive_api != nullptr);

    const std::string archive_root_text = archive_root.string();
    WotbModV3ArchiveHandle archive = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        archive_api->open_directory(
            mod,
            archive_root_text.c_str(),
            nullptr,
            &archive) == WOTBMOD_V3_OK);
    CHECK(archive != WOTBMOD_V3_INVALID_HANDLE);

    uint32_t entry_count = 0u;
    CHECK(
        archive_api->get_entry_count(
            mod, archive, &entry_count) == WOTBMOD_V3_OK);
    CHECK(entry_count == 2u);

    WotbModV3Buffer read = {};
    read.struct_size = sizeof(read);
    read.api_version = WOTBMOD_V3_ARCHIVE_VERSION;
    CHECK(
        archive_api->read_entry(
            mod,
            archive,
            "nested/value.txt",
            &read) == WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    CHECK(read.size == std::strlen("verified-directory-archive"));
    std::vector<uint8_t> bytes(read.size);
    read.data = bytes.data();
    read.capacity = static_cast<uint32_t>(bytes.size());
    CHECK(
        archive_api->read_entry(
            mod,
            archive,
            "nested/value.txt",
            &read) == WOTBMOD_V3_OK);
    CHECK(
        std::string(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()) == "verified-directory-archive");

    char archive_sha256[65] = {};
    CHECK(
        archive_api->get_sha256(
            mod, archive, archive_sha256) == WOTBMOD_V3_OK);
    CHECK(std::strlen(archive_sha256) == 64u);
    CHECK(
        archive_api->verify_sha256(
            mod, archive, archive_sha256) == WOTBMOD_V3_OK);

    const std::string unsupported_package_text =
        unsupported_package.string();
    WotbModV3ArchiveHandle unsupported_archive = UINT64_MAX;
    CHECK(
        archive_api->open_package_file(
            mod,
            unsupported_package_text.c_str(),
            nullptr,
            &unsupported_archive) == WOTBMOD_V3_E_PARSE);
    CHECK(unsupported_archive == WOTBMOD_V3_INVALID_HANDLE);

    const std::string stored_package_text =
        stored_package.string();
    WotbModV3ArchiveHandle stored_archive =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        archive_api->open_package_file(
            mod,
            stored_package_text.c_str(),
            nullptr,
            &stored_archive) == WOTBMOD_V3_OK);
    CHECK(stored_archive != WOTBMOD_V3_INVALID_HANDLE);
    std::vector<WotbModV3ArchiveHandle>
        package_archives{stored_archive};
    entry_count = 0u;
    CHECK(
        archive_api->get_entry_count(
            mod,
            stored_archive,
            &entry_count) == WOTBMOD_V3_OK);
    CHECK(entry_count == 3u);

    WotbModV3ArchiveEntry package_entry = {};
    package_entry.struct_size = sizeof(package_entry);
    package_entry.api_version =
        WOTBMOD_V3_ARCHIVE_VERSION;
    bool saw_stored_value = false;
    for (uint32_t index = 0u;
         index < entry_count; ++index) {
        package_entry = {};
        package_entry.struct_size = sizeof(package_entry);
        package_entry.api_version =
            WOTBMOD_V3_ARCHIVE_VERSION;
        CHECK(
            archive_api->get_entry(
                mod,
                stored_archive,
                index,
                &package_entry) == WOTBMOD_V3_OK);
        if (std::strcmp(
                package_entry.relative_path,
                "dir/value.txt") == 0) {
            saw_stored_value = true;
            CHECK(package_entry.is_directory == 0u);
            CHECK(package_entry.depth == 2u);
            CHECK(
                package_entry.size ==
                std::strlen("stored-package-value"));
            CHECK(
                std::strlen(package_entry.sha256) == 64u);
        }
    }
    CHECK(saw_stored_value);

    read = {};
    read.struct_size = sizeof(read);
    read.api_version = WOTBMOD_V3_ARCHIVE_VERSION;
    CHECK(
        archive_api->read_entry(
            mod,
            stored_archive,
            "dir/value.txt",
            &read) == WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    bytes.assign(read.size, 0u);
    read.data = bytes.data();
    read.capacity = static_cast<uint32_t>(bytes.size());
    CHECK(
        archive_api->read_entry(
            mod,
            stored_archive,
            "dir/value.txt",
            &read) == WOTBMOD_V3_OK);
    CHECK(
        std::string(
            reinterpret_cast<const char*>(bytes.data()),
            bytes.size()) == "stored-package-value");

    char package_sha256[65] = {};
    CHECK(
        archive_api->get_sha256(
            mod,
            stored_archive,
            package_sha256) == WOTBMOD_V3_OK);
    CHECK(std::strlen(package_sha256) == 64u);
    CHECK(
        archive_api->verify_sha256(
            mod,
            stored_archive,
            package_sha256) == WOTBMOD_V3_OK);

    WotbModV3ArchiveLimits expected_limits =
        TestArchiveLimits(
            16u,
            4096u,
            512ull * 1024ull * 1024ull,
            128ull * 1024ull * 1024ull);
    Copy(
        expected_limits.expected_sha256,
        sizeof(expected_limits.expected_sha256),
        package_sha256);
    WotbModV3ArchiveHandle verified_archive =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        archive_api->open_package_file(
            mod,
            stored_package_text.c_str(),
            &expected_limits,
            &verified_archive) == WOTBMOD_V3_OK);
    CHECK(verified_archive != WOTBMOD_V3_INVALID_HANDLE);
    package_archives.push_back(verified_archive);
    std::memset(
        expected_limits.expected_sha256,
        '0',
        64u);
    expected_limits.expected_sha256[64] = '\0';
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            stored_package,
            &expected_limits,
            WOTBMOD_V3_E_HASH_MISMATCH));

    CHECK(
        archive_api->extract_all(
            mod,
            stored_archive,
            "data://self/zip-output") ==
        WOTBMOD_V3_OK);
    std::ifstream extracted(
        owned_root / "zip-output" / "dir" / "value.txt",
        std::ios::binary);
    std::string extracted_text{
        std::istreambuf_iterator<char>(extracted),
        std::istreambuf_iterator<char>()};
    CHECK(extracted_text == "stored-package-value");
    CHECK(
        archive_api->extract_all(
            mod,
            stored_archive,
            "game://self/zip-output") ==
        WOTBMOD_V3_E_PERMISSION_DENIED);

    const fs::path unsafe_path_package =
        owned_root / "unsafe-path.wotbmod";
    CHECK(
        WriteTestZip(
            unsafe_path_package,
            {{"../escape.txt", "escape"}}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            unsafe_path_package,
            nullptr,
            WOTBMOD_V3_E_PERMISSION_DENIED));
    const std::vector<std::string> other_unsafe_paths = {
        "/absolute.txt",
        "C:/drive.txt",
        "folder\\escape.txt",
        "trailing-dot."
    };
    for (size_t index = 0u;
         index < other_unsafe_paths.size(); ++index) {
        const fs::path unsafe_package =
            owned_root /
            ("unsafe-" + std::to_string(index) +
             ".wotbmod");
        CHECK(
            WriteTestZip(
                unsafe_package,
                {{other_unsafe_paths[index], "unsafe"}}));
        CHECK(
            OpenPackageReturns(
                archive_api,
                mod,
                unsafe_package,
                nullptr,
                WOTBMOD_V3_E_PERMISSION_DENIED));
    }

    const fs::path device_path_package =
        owned_root / "device-path.wotbmod";
    CHECK(
        WriteTestZip(
            device_path_package,
            {{"CON.txt", "device"}}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            device_path_package,
            nullptr,
            WOTBMOD_V3_E_PERMISSION_DENIED));

    const fs::path duplicate_package =
        owned_root / "duplicate-path.wotbmod";
    CHECK(
        WriteTestZip(
            duplicate_package,
            {
                {"Textures/Icon.png", "first"},
                {"textures/icon.PNG", "second"}
            }));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            duplicate_package,
            nullptr,
            WOTBMOD_V3_E_CONFLICT));

    const fs::path collision_package =
        owned_root / "prefix-collision.wotbmod";
    CHECK(
        WriteTestZip(
            collision_package,
            {
                {"folder", "file"},
                {"folder/child.txt", "child"}
            }));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            collision_package,
            nullptr,
            WOTBMOD_V3_E_CONFLICT));

    TestZipEntry encrypted_entry{"encrypted.bin", "secret"};
    encrypted_entry.flags = 0x0001u;
    const fs::path encrypted_package =
        owned_root / "encrypted.wotbmod";
    CHECK(
        WriteTestZip(
            encrypted_package,
            {encrypted_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            encrypted_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    TestZipEntry descriptor_entry{"descriptor.bin", "value"};
    descriptor_entry.flags = 0x0008u;
    const fs::path descriptor_package =
        owned_root / "descriptor.wotbmod";
    CHECK(
        WriteTestZip(
            descriptor_package,
            {descriptor_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            descriptor_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    TestZipEntry compressed_entry{"compressed.bin", "raw"};
    compressed_entry.method = 8u;
    const fs::path compressed_package =
        owned_root / "compressed.wotbmod";
    CHECK(
        WriteTestZip(
            compressed_package,
            {compressed_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            compressed_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    TestZipEntry legacy_name_entry{
        std::string("\xc3\xa9.txt", 6u), "utf8"};
    const fs::path legacy_name_package =
        owned_root / "legacy-name.wotbmod";
    CHECK(
        WriteTestZip(
            legacy_name_package,
            {legacy_name_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            legacy_name_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    TestZipEntry symlink_entry{"link", "target"};
    symlink_entry.version_made_by =
        static_cast<uint16_t>((3u << 8u) | 20u);
    symlink_entry.external_attributes = 0xa1ff0000u;
    const fs::path symlink_package =
        owned_root / "symlink.wotbmod";
    CHECK(
        WriteTestZip(
            symlink_package,
            {symlink_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            symlink_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    const fs::path multidisk_package =
        owned_root / "multidisk.wotbmod";
    TestZipOptions multidisk_options;
    multidisk_options.disk = 1u;
    CHECK(
        WriteTestZip(
            multidisk_package,
            {{"value.bin", "value"}},
            multidisk_options));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            multidisk_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    const fs::path zip64_package =
        owned_root / "zip64.wotbmod";
    TestZipOptions zip64_options;
    zip64_options.zip64_central_size = true;
    CHECK(
        WriteTestZip(
            zip64_package,
            {{"value.bin", "value"}},
            zip64_options));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            zip64_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    TestZipEntry zip64_extra_entry{"zip64-extra.bin", "value"};
    zip64_extra_entry.central_extra =
        {0x01u, 0x00u, 0x00u, 0x00u};
    const fs::path zip64_extra_package =
        owned_root / "zip64-extra.wotbmod";
    CHECK(
        WriteTestZip(
            zip64_extra_package,
            {zip64_extra_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            zip64_extra_package,
            nullptr,
            WOTBMOD_V3_E_NOT_SUPPORTED));

    TestZipEntry crc_entry{"crc.bin", "value"};
    crc_entry.corrupt_crc = true;
    const fs::path crc_package =
        owned_root / "bad-crc.wotbmod";
    CHECK(WriteTestZip(crc_package, {crc_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            crc_package,
            nullptr,
            WOTBMOD_V3_E_HASH_MISMATCH));

    TestZipEntry mismatch_entry{"mismatch.bin", "value"};
    mismatch_entry.local_flags_override = 0x0800u;
    const fs::path mismatch_package =
        owned_root / "metadata-mismatch.wotbmod";
    CHECK(
        WriteTestZip(
            mismatch_package,
            {mismatch_entry}));
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            mismatch_package,
            nullptr,
            WOTBMOD_V3_E_PARSE));

    const fs::path limit_package =
        owned_root / "limits.wotbmod";
    CHECK(
        WriteTestZip(
            limit_package,
            {
                {"deep/value.bin", "abc"},
                {"other.bin", "def"}
            }));
    WotbModV3ArchiveLimits count_limits =
        TestArchiveLimits(8u, 1u, 32u, 16u);
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            limit_package,
            &count_limits,
            WOTBMOD_V3_E_LIMIT_REACHED));
    WotbModV3ArchiveLimits depth_limits =
        TestArchiveLimits(1u, 8u, 32u, 16u);
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            limit_package,
            &depth_limits,
            WOTBMOD_V3_E_LIMIT_REACHED));
    WotbModV3ArchiveLimits single_limits =
        TestArchiveLimits(8u, 8u, 32u, 2u);
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            limit_package,
            &single_limits,
            WOTBMOD_V3_E_LIMIT_REACHED));
    WotbModV3ArchiveLimits total_limits =
        TestArchiveLimits(8u, 8u, 5u, 3u);
    CHECK(
        OpenPackageReturns(
            archive_api,
            mod,
            limit_package,
            &total_limits,
            WOTBMOD_V3_E_LIMIT_REACHED));

    CHECK(stored_layout.data_offsets.size() == 3u);
    CHECK(
        OverwriteByte(
            stored_package,
            stored_layout.data_offsets[1],
            static_cast<uint8_t>('X')));
    read.data = bytes.data();
    read.capacity = static_cast<uint32_t>(bytes.size());
    CHECK(
        archive_api->read_entry(
            mod,
            stored_archive,
            "dir/value.txt",
            &read) == WOTBMOD_V3_E_HASH_MISMATCH);

    const void* raw_loaders = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_LOADERS,
            WOTBMOD_V3_LOADERS_VERSION,
            &raw_loaders) == WOTBMOD_V3_OK);
    const WotbModV3LoadersApiV1* loaders =
        static_cast<const WotbModV3LoadersApiV1*>(raw_loaders);
    CHECK(loaders != nullptr);

    WotbModV3ArchiveHandle dava_archive = UINT64_MAX;
    CHECK(
        loaders->open_dava_archive(
            mod,
            "data://unsupported.wotbmod",
            &dava_archive) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(dava_archive == WOTBMOD_V3_INVALID_HANDLE);

    WotbModV3LoaderBackendInfo backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version = WOTBMOD_V3_LOADERS_VERSION;
    CHECK(
        loaders->get_backend_info(
            mod,
            WOTBMOD_V3_LOADER_DAVA_ARCHIVE,
            &backend) == WOTBMOD_V3_OK);
    CHECK(backend.available == 0u);
    CHECK(backend.unavailable_reason[0] != '\0');

    NativeSnapshotFake native_snapshot_fake;
    WotbModDavaNativeBackend native_snapshot_backend =
        NativeSnapshotBackend(&native_snapshot_fake);
    CHECK(
        wotbmod::v3::InstallDavaNativeBackend(
            &native_snapshot_backend) == WOTBMOD_V3_OK);
    backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version = WOTBMOD_V3_LOADERS_VERSION;
    CHECK(
        loaders->get_backend_info(
            mod,
            WOTBMOD_V3_LOADER_DAVA_ARCHIVE,
            &backend) == WOTBMOD_V3_OK);
    CHECK(backend.available == 1u);
    CHECK(backend.unavailable_reason[0] == '\0');

    WotbModV3Handle native_yaml = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        loaders->load_dava_yaml(
            mod,
            "data://self/unsupported.wotbmod",
            &native_yaml) == WOTBMOD_V3_OK);
    CHECK(native_yaml != WOTBMOD_V3_INVALID_HANDLE);

    const void* raw_yaml = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_YAML,
            WOTBMOD_V3_YAML_VERSION,
            &raw_yaml) == WOTBMOD_V3_OK);
    const WotbModV3YamlApiV1* yaml_api =
        static_cast<const WotbModV3YamlApiV1*>(raw_yaml);
    CHECK(yaml_api != nullptr);
    WotbModV3YamlNodeId yaml_root = WOTBMOD_V3_YAML_INVALID_NODE;
    WotbModV3YamlNodeId yaml_value = WOTBMOD_V3_YAML_INVALID_NODE;
    uint32_t yaml_bool = 0u;
    CHECK(
        yaml_api->get_root(mod, native_yaml, &yaml_root) ==
        WOTBMOD_V3_OK);
    CHECK(
        yaml_api->map_get(
            mod,
            native_yaml,
            yaml_root,
            "native_snapshot",
            &yaml_value) == WOTBMOD_V3_OK);
    CHECK(
        yaml_api->get_bool(
            mod, native_yaml, yaml_value, &yaml_bool) == WOTBMOD_V3_OK);
    CHECK(yaml_bool == 1u);

    WotbModV3ArchiveHandle native_archive =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        loaders->open_dava_archive(
            mod,
            "data://self/unsupported.wotbmod",
            &native_archive) == WOTBMOD_V3_OK);
    CHECK(native_archive != WOTBMOD_V3_INVALID_HANDLE);
    CHECK(native_snapshot_fake.release_count == 2u);

    wotbmod::v3::RemoveDavaNativeBackend();
    CHECK(wotbmod::v3::InstalledDavaNativeCapabilities() == 0u);

    uint32_t native_count = 0u;
    CHECK(
        archive_api->get_entry_count(
            mod, native_archive, &native_count) == WOTBMOD_V3_OK);
    CHECK(native_count == 1u);
    WotbModV3ArchiveEntry native_entry = {};
    native_entry.struct_size = sizeof(native_entry);
    native_entry.api_version = WOTBMOD_V3_ARCHIVE_VERSION;
    CHECK(
        archive_api->get_entry(
            mod, native_archive, 0u, &native_entry) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(native_entry.relative_path, "native/value.bin") == 0);
    std::vector<uint8_t> native_bytes(5u);
    WotbModV3Buffer native_read = {};
    native_read.struct_size = sizeof(native_read);
    native_read.data = native_bytes.data();
    native_read.capacity = static_cast<uint32_t>(native_bytes.size());
    CHECK(
        archive_api->read_entry(
            mod,
            native_archive,
            "native/value.bin",
            &native_read) == WOTBMOD_V3_OK);
    CHECK(native_bytes == std::vector<uint8_t>({9u, 8u, 7u, 6u, 5u}));
    CHECK(handles->release(mod, native_archive) == WOTBMOD_V3_OK);
    CHECK(handles->release(mod, native_yaml) == WOTBMOD_V3_OK);

    native_snapshot_fake.bad_crc = true;
    CHECK(
        wotbmod::v3::InstallDavaNativeBackend(
            &native_snapshot_backend) == WOTBMOD_V3_OK);
    WotbModV3ArchiveHandle rejected_native_archive = UINT64_MAX;
    CHECK(
        loaders->open_dava_archive(
            mod,
            "data://self/unsupported.wotbmod",
            &rejected_native_archive) == WOTBMOD_V3_E_HASH_MISMATCH);
    CHECK(rejected_native_archive == WOTBMOD_V3_INVALID_HANDLE);
    CHECK(native_snapshot_fake.release_count == 3u);

    native_snapshot_fake.bad_crc = false;
    native_snapshot_fake.case_prefix_collision = true;
    rejected_native_archive = UINT64_MAX;
    CHECK(
        loaders->open_dava_archive(
            mod,
            "data://self/unsupported.wotbmod",
            &rejected_native_archive) == WOTBMOD_V3_E_CONFLICT);
    CHECK(rejected_native_archive == WOTBMOD_V3_INVALID_HANDLE);
    CHECK(native_snapshot_fake.release_count == 4u);
    wotbmod::v3::RemoveDavaNativeBackend();

    WotbModV3ArchiveHandle removed_archive = UINT64_MAX;
    CHECK(
        loaders->open_dava_archive(
            mod,
            "data://self/unsupported.wotbmod",
            &removed_archive) == WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(removed_archive == WOTBMOD_V3_INVALID_HANDLE);

    const void* raw_events = nullptr;
    CHECK(
        bootstrap->query_interface(
            observer,
            WOTBMOD_V3_IFACE_EVENTS,
            WOTBMOD_V3_EVENTS_VERSION,
            &raw_events) == WOTBMOD_V3_OK);
    const WotbModV3EventsApiV1* events =
        static_cast<const WotbModV3EventsApiV1*>(raw_events);
    CHECK(events != nullptr);
    WotbModV3EventSubscriptionInfo watch_subscription = {};
    watch_subscription.struct_size = sizeof(watch_subscription);
    watch_subscription.api_version = WOTBMOD_V3_EVENTS_VERSION;
    watch_subscription.topic_pattern = "wotbmod.*";
    watch_subscription.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
    watch_subscription.receive_system_events = 1u;
    WotbModV3EventToken watch_event_token =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        events->subscribe(
            observer,
            &watch_subscription,
            &WatchEventCallback,
            nullptr,
            &watch_event_token) == WOTBMOD_V3_OK);

    const void* raw_vfs = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_VFS,
            WOTBMOD_V3_VFS_VERSION,
            &raw_vfs) == WOTBMOD_V3_OK);
    const WotbModV3VfsApiV1* vfs =
        static_cast<const WotbModV3VfsApiV1*>(raw_vfs);
    CHECK(vfs != nullptr);

    const fs::path vfs_watch_path = owned_root / "watch.txt";
    CHECK(WriteFile(vfs_watch_path, "aaa"));
    const fs::file_time_type vfs_watch_time =
        fs::last_write_time(vfs_watch_path, error);
    CHECK(!error);
    WotbModV3Token vfs_watch_token =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        vfs->watch(
            mod,
            "data://self/watch.txt",
            &vfs_watch_token) == WOTBMOD_V3_OK);
    CHECK(vfs_watch_token != WOTBMOD_V3_INVALID_HANDLE);

    g_watch_events.clear();
    CHECK(WriteFile(vfs_watch_path, "bbb"));
    fs::last_write_time(vfs_watch_path, vfs_watch_time, error);
    CHECK(!error);
    WotbModV3Runtime_DispatchFrame(1u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 1u);
    CHECK(
        g_watch_events[0].topic ==
        WOTBMOD_V3_EVENT_VFS_INVALIDATED);
    CHECK(
        g_watch_events[0].vfs.watch_token ==
        vfs_watch_token);
    CHECK(
        g_watch_events[0].vfs.change_kind ==
        WOTBMOD_V3_VFS_CHANGE_MODIFIED);
    CHECK(g_watch_events[0].vfs.previous_exists == 1u);
    CHECK(g_watch_events[0].vfs.exists == 1u);
    CHECK(
        std::strncmp(
            g_watch_events[0].vfs.uri,
            "data://mod-",
            std::strlen("data://mod-")) == 0);
    CHECK(
        std::strstr(
            g_watch_events[0].vfs.uri,
            "/watch.txt") != nullptr);

    CHECK(WriteFile(vfs_watch_path, "ccc"));
    CHECK(WriteFile(vfs_watch_path, "ddd"));
    WotbModV3Runtime_DispatchFrame(2u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 2u);
    CHECK(
        g_watch_events[1].vfs.change_kind ==
        WOTBMOD_V3_VFS_CHANGE_MODIFIED);
    WotbModV3Runtime_DispatchFrame(3u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 2u);

    CHECK(fs::remove(vfs_watch_path, error));
    CHECK(!error);
    WotbModV3Runtime_DispatchFrame(4u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 3u);
    CHECK(
        g_watch_events[2].vfs.change_kind ==
        WOTBMOD_V3_VFS_CHANGE_DELETED);
    CHECK(g_watch_events[2].vfs.exists == 0u);
    WotbModV3Runtime_DispatchFrame(5u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 3u);

    CHECK(WriteFile(vfs_watch_path, "eee"));
    WotbModV3Runtime_DispatchFrame(6u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 4u);
    CHECK(
        g_watch_events[3].vfs.change_kind ==
        WOTBMOD_V3_VFS_CHANGE_CREATED);
    CHECK(g_watch_events[3].vfs.exists == 1u);

    CHECK(
        handles->release(mod, vfs_watch_token) ==
        WOTBMOD_V3_OK);
    g_watch_events.clear();
    CHECK(WriteFile(vfs_watch_path, "fff"));
    WotbModV3Runtime_DispatchFrame(7u, 1.0 / 60.0);
    CHECK(g_watch_events.empty());

    const void* raw_resources = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_RESOURCES,
            WOTBMOD_V3_RESOURCES_VERSION,
            &raw_resources) == WOTBMOD_V3_OK);
    const WotbModV3ResourcesApiV1* resources_api =
        static_cast<const WotbModV3ResourcesApiV1*>(
            raw_resources);
    CHECK(resources_api != nullptr);

    const fs::path async_resource_path =
        owned_root / "resource-async.txt";
    CHECK(
        WriteFile(
            async_resource_path,
            "resource-loaded-on-shared-worker"));
    WotbModV3ResourceLoadDesc async_desc = {};
    async_desc.struct_size = sizeof(async_desc);
    async_desc.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    async_desc.expected_type = WOTBMOD_V3_RESOURCE_TEXT;
    async_desc.max_bytes = 1024u;
    Copy(
        async_desc.uri,
        sizeof(async_desc.uri),
        "data://self/resource-async.txt");
    WotbModV3ResourceHandle async_resource =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        resources_api->load_async_ex(
            mod,
            &async_desc,
            &async_resource) == WOTBMOD_V3_OK);
    CHECK(async_resource != WOTBMOD_V3_INVALID_HANDLE);
    WotbModV3ResourceInfo async_info = {};
    CHECK(
        WaitUntil([&]() {
            async_info = {};
            async_info.struct_size = sizeof(async_info);
            async_info.api_version =
                WOTBMOD_V3_RESOURCES_VERSION;
            return resources_api->get_info(
                       mod,
                       async_resource,
                       &async_info) == WOTBMOD_V3_OK &&
                   async_info.state !=
                       WOTBMOD_V3_RESOURCE_LOADING;
        }));
    CHECK(async_info.state == WOTBMOD_V3_RESOURCE_READY);
    CHECK(
        async_info.memory_bytes ==
        std::strlen("resource-loaded-on-shared-worker"));
    WotbModV3Buffer async_data = {};
    async_data.struct_size = sizeof(async_data);
    async_data.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    CHECK(
        resources_api->copy_data(
            mod,
            async_resource,
            &async_data) == WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    std::vector<uint8_t> async_bytes(async_data.size);
    async_data.data = async_bytes.data();
    async_data.capacity =
        static_cast<uint32_t>(async_bytes.size());
    CHECK(
        resources_api->copy_data(
            mod,
            async_resource,
            &async_data) == WOTBMOD_V3_OK);
    CHECK(
        std::string(
            reinterpret_cast<const char*>(async_bytes.data()),
            async_bytes.size()) ==
        "resource-loaded-on-shared-worker");
    CHECK(
        resources_api->release(
            mod,
            async_resource) == WOTBMOD_V3_OK);

    WotbModV3ResourceLoadDesc missing_async_desc = async_desc;
    Copy(
        missing_async_desc.uri,
        sizeof(missing_async_desc.uri),
        "data://self/resource-missing.txt");
    WotbModV3ResourceHandle missing_async_resource =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        resources_api->load_async_ex(
            mod,
            &missing_async_desc,
            &missing_async_resource) == WOTBMOD_V3_OK);
    CHECK(
        WaitUntil([&]() {
            async_info = {};
            async_info.struct_size = sizeof(async_info);
            async_info.api_version =
                WOTBMOD_V3_RESOURCES_VERSION;
            return resources_api->get_info(
                       mod,
                       missing_async_resource,
                       &async_info) == WOTBMOD_V3_OK &&
                   async_info.state !=
                       WOTBMOD_V3_RESOURCE_LOADING;
        }));
    CHECK(async_info.state == WOTBMOD_V3_RESOURCE_FAILED);
    CHECK(async_info.error[0] != '\0');
    CHECK(
        resources_api->release(
            mod,
            missing_async_resource) == WOTBMOD_V3_OK);

    const fs::path resource_watch_path =
        owned_root / "resource-watch.txt";
    CHECK(WriteFile(resource_watch_path, "resource-old"));
    WotbModV3ResourceLoadDesc resource_desc = {};
    resource_desc.struct_size = sizeof(resource_desc);
    resource_desc.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    resource_desc.expected_type = WOTBMOD_V3_RESOURCE_TEXT;
    resource_desc.max_bytes = 1024u;
    Copy(
        resource_desc.uri,
        sizeof(resource_desc.uri),
        "data://self/resource-watch.txt");
    WotbModV3ResourceHandle watched_resource =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        resources_api->load(
            mod,
            &resource_desc,
            &watched_resource) == WOTBMOD_V3_OK);

    WotbModV3ResourceInfo resource_before = {};
    resource_before.struct_size = sizeof(resource_before);
    resource_before.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    CHECK(
        resources_api->get_info(
            mod,
            watched_resource,
            &resource_before) == WOTBMOD_V3_OK);
    const std::string original_hash = resource_before.sha256;

    WotbModV3Token resource_watch_token =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        resources_api->watch(
            mod,
            watched_resource,
            &resource_watch_token) == WOTBMOD_V3_OK);
    CHECK(
        resource_watch_token !=
        WOTBMOD_V3_INVALID_HANDLE);

    g_watch_events.clear();
    CHECK(WriteFile(resource_watch_path, "resource-new-a"));
    CHECK(WriteFile(resource_watch_path, "resource-new-b"));
    WotbModV3Runtime_DispatchFrame(8u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 2u);
    CHECK(
        g_watch_events[0].topic ==
        WOTBMOD_V3_EVENT_RESOURCE_INVALIDATED);
    CHECK(
        g_watch_events[1].topic ==
        WOTBMOD_V3_EVENT_RESOURCE_RELOADED);
    CHECK(
        g_watch_events[0].resource.watch_token ==
        resource_watch_token);
    CHECK(
        g_watch_events[1].resource.resource ==
        watched_resource);
    CHECK(
        std::string(g_watch_events[0].resource.sha256) ==
        original_hash);
    CHECK(
        std::string(g_watch_events[1].resource.sha256) !=
        original_hash);

    WotbModV3ResourceInfo resource_after = {};
    resource_after.struct_size = sizeof(resource_after);
    resource_after.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    CHECK(
        resources_api->get_info(
            mod,
            watched_resource,
            &resource_after) == WOTBMOD_V3_OK);
    CHECK(
        std::string(resource_after.sha256) ==
        g_watch_events[1].resource.sha256);
    CHECK(
        resource_after.memory_bytes ==
        std::strlen("resource-new-b"));
    WotbModV3Buffer resource_data = {};
    resource_data.struct_size = sizeof(resource_data);
    resource_data.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    CHECK(
        resources_api->copy_data(
            mod,
            watched_resource,
            &resource_data) ==
        WOTBMOD_V3_E_BUFFER_TOO_SMALL);
    std::vector<uint8_t> resource_bytes(resource_data.size);
    resource_data.data = resource_bytes.data();
    resource_data.capacity =
        static_cast<uint32_t>(resource_bytes.size());
    CHECK(
        resources_api->copy_data(
            mod,
            watched_resource,
            &resource_data) == WOTBMOD_V3_OK);
    CHECK(
        std::string(
            reinterpret_cast<const char*>(
                resource_bytes.data()),
            resource_bytes.size()) == "resource-new-b");

    const std::string successful_hash = resource_after.sha256;
    g_watch_events.clear();
    error.clear();
    CHECK(fs::remove(resource_watch_path, error));
    CHECK(!error);
    WotbModV3Runtime_DispatchFrame(9u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 1u);
    CHECK(
        g_watch_events[0].topic ==
        WOTBMOD_V3_EVENT_RESOURCE_INVALIDATED);
    CHECK(
        g_watch_events[0].resource.change_kind ==
        WOTBMOD_V3_VFS_CHANGE_DELETED);

    WotbModV3ResourceInfo resource_rollback = {};
    resource_rollback.struct_size = sizeof(resource_rollback);
    resource_rollback.api_version =
        WOTBMOD_V3_RESOURCES_VERSION;
    CHECK(
        resources_api->get_info(
            mod,
            watched_resource,
            &resource_rollback) == WOTBMOD_V3_OK);
    CHECK(resource_rollback.state == WOTBMOD_V3_RESOURCE_READY);
    CHECK(
        std::string(resource_rollback.sha256) ==
        successful_hash);
    resource_data.data = resource_bytes.data();
    resource_data.capacity =
        static_cast<uint32_t>(resource_bytes.size());
    CHECK(
        resources_api->copy_data(
            mod,
            watched_resource,
            &resource_data) == WOTBMOD_V3_OK);
    CHECK(
        std::string(
            reinterpret_cast<const char*>(
                resource_bytes.data()),
            resource_data.size) == "resource-new-b");
    WotbModV3Runtime_DispatchFrame(10u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 1u);

    g_watch_events.clear();
    CHECK(
        WriteFile(
            resource_watch_path,
            "resource-restored"));
    WotbModV3Runtime_DispatchFrame(11u, 1.0 / 60.0);
    CHECK(g_watch_events.size() == 2u);
    CHECK(
        g_watch_events[0].resource.change_kind ==
        WOTBMOD_V3_VFS_CHANGE_CREATED);
    CHECK(
        g_watch_events[1].topic ==
        WOTBMOD_V3_EVENT_RESOURCE_RELOADED);
    resource_after = {};
    resource_after.struct_size = sizeof(resource_after);
    resource_after.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    CHECK(
        resources_api->get_info(
            mod,
            watched_resource,
            &resource_after) == WOTBMOD_V3_OK);
    CHECK(
        std::string(resource_after.sha256) !=
        successful_hash);
    CHECK(
        resource_after.memory_bytes ==
        std::strlen("resource-restored"));

    CHECK(
        handles->release(mod, resource_watch_token) ==
        WOTBMOD_V3_OK);
    g_watch_events.clear();
    CHECK(WriteFile(resource_watch_path, "resource-after-release"));
    WotbModV3Runtime_DispatchFrame(12u, 1.0 / 60.0);
    CHECK(g_watch_events.empty());
    CHECK(
        resources_api->release(
            mod, watched_resource) == WOTBMOD_V3_OK);

    for (WotbModV3ArchiveHandle package_archive :
         package_archives) {
        CHECK(
            handles->release(
                mod, package_archive) == WOTBMOD_V3_OK);
    }
    CHECK(handles->release(mod, archive) == WOTBMOD_V3_OK);

    CHECK(WriteFile(vfs_watch_path, "owner-before-stop"));
    WotbModV3Token owner_cleanup_watch =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        vfs->watch(
            mod,
            "data://self/watch.txt",
            &owner_cleanup_watch) == WOTBMOD_V3_OK);

    const void* raw_async = nullptr;
    CHECK(
        bootstrap->query_interface(
            observer,
            WOTBMOD_V3_IFACE_ASYNC,
            WOTBMOD_V3_ASYNC_VERSION,
            &raw_async) == WOTBMOD_V3_OK);
    const WotbModV3AsyncApiV1* async_api =
        static_cast<const WotbModV3AsyncApiV1*>(raw_async);
    CHECK(async_api != nullptr);
    g_release_blocking_workers.store(false);
    g_blocking_workers_started.store(0u);
    WotbModV3TaskHandle blocking_tasks[4] = {};
    WotbModV3TaskSubmitInfo blocking_info = {};
    blocking_info.struct_size = sizeof(blocking_info);
    blocking_info.api_version = WOTBMOD_V3_ASYNC_VERSION;
    blocking_info.description = "hold worker for cancellation test";
    blocking_info.work = &BlockingWorker;
    blocking_info.completion_thread_role =
        WOTBMOD_V3_THREAD_WORKER;
    for (WotbModV3TaskHandle& task : blocking_tasks) {
        CHECK(
            async_api->task_submit(
                observer,
                &blocking_info,
                &task) == WOTBMOD_V3_OK);
    }
    CHECK(
        WaitUntil([]() {
            return g_blocking_workers_started.load() == 4u;
        }));

    CHECK(
        WriteFile(
            owned_root / "resource-cancelled.txt",
            "this queued load must be cancelled by owner stop"));
    WotbModV3ResourceLoadDesc cancelled_desc = async_desc;
    Copy(
        cancelled_desc.uri,
        sizeof(cancelled_desc.uri),
        "data://self/resource-cancelled.txt");
    WotbModV3ResourceHandle cancelled_resource =
        WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        resources_api->load_async_ex(
            mod,
            &cancelled_desc,
            &cancelled_resource) == WOTBMOD_V3_OK);
    async_info = {};
    async_info.struct_size = sizeof(async_info);
    async_info.api_version = WOTBMOD_V3_RESOURCES_VERSION;
    CHECK(
        resources_api->get_info(
            mod,
            cancelled_resource,
            &async_info) == WOTBMOD_V3_OK);
    CHECK(async_info.state == WOTBMOD_V3_RESOURCE_LOADING);

    CHECK(WotbModV3Runtime_Disable(mod) == WOTBMOD_V3_OK);
    uint32_t cancelled_alive = 1u;
    CHECK(
        handles->is_alive(
            mod,
            cancelled_resource,
            &cancelled_alive) == WOTBMOD_V3_OK);
    CHECK(cancelled_alive == 0u);
    g_release_blocking_workers.store(true);
    for (WotbModV3TaskHandle task : blocking_tasks) {
        CHECK(
            WaitUntil([&]() {
                uint32_t state = WOTBMOD_V3_TASK_QUEUED;
                return async_api->task_get_state(
                           observer,
                           task,
                           &state) == WOTBMOD_V3_OK &&
                       state == WOTBMOD_V3_TASK_COMPLETED;
            }));
        CHECK(
            handles->release(observer, task) ==
            WOTBMOD_V3_OK);
    }
    uint32_t cleanup_alive = 1u;
    CHECK(
        handles->is_alive(
            mod,
            owner_cleanup_watch,
            &cleanup_alive) == WOTBMOD_V3_OK);
    CHECK(cleanup_alive == 0u);
    g_watch_events.clear();
    CHECK(WriteFile(vfs_watch_path, "owner-after-stop"));
    WotbModV3Runtime_DispatchFrame(13u, 1.0 / 60.0);
    CHECK(g_watch_events.empty());
    CHECK(WotbModV3Runtime_DestroyMod(mod) == WOTBMOD_V3_OK);
    CHECK(
        events->unsubscribe(
            observer,
            watch_event_token) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_Disable(observer) ==
        WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_DestroyMod(observer) ==
        WOTBMOD_V3_OK);

    // Regression, data_services.cpp EnsureDataOwner: a data call made by an
    // owner the handle registry has stopped accepting must come back as an
    // error code. It used to kill the process instead.
    //
    // EnsureDataOwner builds an OwnerAnchor and asks the registry to adopt
    // it. For a stopping or disabled owner the registry refuses with
    // E_CANCELLED, so the anchor has to be destroyed again. ~OwnerAnchor
    // locks g_owner_mutex, which EnsureDataOwner was still holding, and
    // g_owner_mutex is a plain std::mutex: MSVC detects the self-deadlock
    // and throws std::system_error out of an implicitly-noexcept
    // destructor. That is terminate() and abort() -- exit 0xC0000409, no
    // log line, no unwinding, and in the loader DLL that means the
    // player's game disappears. Every *Access gate in data_services.cpp
    // funnels into EnsureDataOwner, so each of them is exercised here.
    WotbModV3Handle stopped = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "data_stopped_owner.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &stopped) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            stopped, &StoppedOwnerEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(stopped) == WOTBMOD_V3_OK);

    const void* raw_stopped_settings = nullptr;
    CHECK(
        bootstrap->query_interface(
            stopped,
            WOTBMOD_V3_IFACE_SETTINGS,
            WOTBMOD_V3_SETTINGS_VERSION,
            &raw_stopped_settings) == WOTBMOD_V3_OK);
    const WotbModV3SettingsApiV1* stopped_settings =
        static_cast<const WotbModV3SettingsApiV1*>(
            raw_stopped_settings);
    CHECK(stopped_settings != nullptr);

    const void* raw_stopped_storage = nullptr;
    CHECK(
        bootstrap->query_interface(
            stopped,
            WOTBMOD_V3_IFACE_STORAGE,
            WOTBMOD_V3_STORAGE_VERSION,
            &raw_stopped_storage) == WOTBMOD_V3_OK);
    const WotbModV3StorageApiV1* stopped_storage =
        static_cast<const WotbModV3StorageApiV1*>(raw_stopped_storage);
    CHECK(stopped_storage != nullptr);

    const void* raw_stopped_input = nullptr;
    CHECK(
        bootstrap->query_interface(
            stopped,
            WOTBMOD_V3_IFACE_INPUT,
            WOTBMOD_V3_INPUT_VERSION,
            &raw_stopped_input) == WOTBMOD_V3_OK);
    const WotbModV3InputApiV1* stopped_input =
        static_cast<const WotbModV3InputApiV1*>(raw_stopped_input);
    CHECK(stopped_input != nullptr);

    const void* raw_stopped_vfs = nullptr;
    CHECK(
        bootstrap->query_interface(
            stopped,
            WOTBMOD_V3_IFACE_VFS,
            WOTBMOD_V3_VFS_VERSION,
            &raw_stopped_vfs) == WOTBMOD_V3_OK);
    const WotbModV3VfsApiV1* stopped_vfs =
        static_cast<const WotbModV3VfsApiV1*>(raw_stopped_vfs);
    CHECK(stopped_vfs != nullptr);

    const void* raw_stopped_content = nullptr;
    CHECK(
        bootstrap->query_interface(
            stopped,
            WOTBMOD_V3_IFACE_CONTENT,
            WOTBMOD_V3_CONTENT_VERSION,
            &raw_stopped_content) == WOTBMOD_V3_OK);
    const WotbModV3ContentApiV1* stopped_content =
        static_cast<const WotbModV3ContentApiV1*>(raw_stopped_content);
    CHECK(stopped_content != nullptr);

    const void* raw_stopped_manifest = nullptr;
    CHECK(
        bootstrap->query_interface(
            stopped,
            WOTBMOD_V3_IFACE_MANIFEST,
            WOTBMOD_V3_MANIFEST_VERSION,
            &raw_stopped_manifest) == WOTBMOD_V3_OK);
    const WotbModV3ManifestApiV1* stopped_manifest =
        static_cast<const WotbModV3ManifestApiV1*>(
            raw_stopped_manifest);
    CHECK(stopped_manifest != nullptr);

    const void* raw_stopped_catalog = nullptr;
    CHECK(
        bootstrap->query_interface(
            stopped,
            WOTBMOD_V3_IFACE_CATALOG,
            WOTBMOD_V3_CATALOG_VERSION,
            &raw_stopped_catalog) == WOTBMOD_V3_OK);
    const WotbModV3CatalogApiV1* stopped_catalog =
        static_cast<const WotbModV3CatalogApiV1*>(raw_stopped_catalog);
    CHECK(stopped_catalog != nullptr);

    // While the owner is live the gate opens: E_NOT_FOUND is the answer to
    // "what schema version", not a refusal, and it means EnsureDataOwner
    // registered an OwnerAnchor for this owner.
    uint32_t stopped_schema_version = 0u;
    CHECK(
        stopped_settings->get_schema_version(
            stopped,
            &stopped_schema_version) == WOTBMOD_V3_E_NOT_FOUND);

    // Disabling releases every handle the owner held, including that
    // anchor, so the next data call has to build a new one -- and the
    // registry now refuses it.
    CHECK(WotbModV3Runtime_Disable(stopped) == WOTBMOD_V3_OK);

    // SettingsAccess.
    stopped_schema_version = 0u;
    CHECK(
        stopped_settings->get_schema_version(
            stopped,
            &stopped_schema_version) == WOTBMOD_V3_E_CANCELLED);

    // StorageAccess.
    uint32_t stopped_contains = 1u;
    CHECK(
        stopped_storage->contains(
            stopped,
            "regression",
            &stopped_contains) == WOTBMOD_V3_E_CANCELLED);

    // InputAccess.
    CHECK(
        stopped_input->unregister_action(
            stopped,
            WOTBMOD_V3_INVALID_HANDLE) == WOTBMOD_V3_E_CANCELLED);

    // VfsAccess.
    char stopped_namespace[128] = {0};
    uint32_t stopped_namespace_size =
        static_cast<uint32_t>(sizeof(stopped_namespace));
    CHECK(
        stopped_vfs->get_namespace(
            stopped,
            stopped_namespace,
            &stopped_namespace_size) == WOTBMOD_V3_E_CANCELLED);

    // VfsOverlayAccess. Its own EnsureDataOwner
    // call is shadowed by the VfsAccess check that runs first in every
    // overlay entry point, so this covers the reachable half of that pair.
    WotbModV3Handle stopped_overlay = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        stopped_vfs->mount_overlay(
            stopped,
            "regression",
            "game://Data/regression.txt",
            "data://self/regression.txt",
            0,
            &stopped_overlay) == WOTBMOD_V3_E_CANCELLED);

    const char kStoppedJson[] = "{}";
    WotbModV3ConstBuffer stopped_json = {};
    stopped_json.struct_size = sizeof(stopped_json);
    stopped_json.api_version = WOTBMOD_V3_ABI_VERSION;
    stopped_json.data = kStoppedJson;
    stopped_json.size = static_cast<uint32_t>(sizeof(kStoppedJson) - 1u);

    // ContentAccess.
    WotbModV3Handle stopped_content_handle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        stopped_content->parse_json(
            stopped,
            &stopped_json,
            &stopped_content_handle) == WOTBMOD_V3_E_CANCELLED);

    // ManifestAccess.
    WotbModV3Handle stopped_manifest_handle = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        stopped_manifest->parse_json(
            stopped,
            &stopped_json,
            &stopped_manifest_handle) == WOTBMOD_V3_E_CANCELLED);

    // CatalogAccess.
    WotbModV3CatalogRecord stopped_record = {};
    stopped_record.struct_size = sizeof(stopped_record);
    stopped_record.api_version = WOTBMOD_V3_CATALOG_VERSION;
    CHECK(
        stopped_catalog->validate_record(
            stopped,
            &stopped_record) == WOTBMOD_V3_E_CANCELLED);

    CHECK(
        WotbModV3Runtime_DestroyMod(stopped) == WOTBMOD_V3_OK);

    // Regression: an anchor the registry refused must tear nothing down.
    //
    // This is the hazard the abort was hiding. ~OwnerAnchor never ran on
    // the refused path -- the process died first -- so making the refusal
    // survivable un-suppressed a destructor that performs full owner
    // teardown: CleanupOwnerState wipes settings, storage, input actions,
    // mounts and resources, and then the g_owner_anchors entry is erased.
    // All of that is keyed by owner, not by anchor identity, so a refused
    // anchor destroyed after its owner came back would destroy the state
    // of whichever anchor did get adopted.
    //
    // Only a thread interleaving reaches it, and this arranges one that
    // does not depend on luck. `toggling` is cycled down and up while
    // worker threads call a settings gate on it, minting refused anchors
    // while it is down. A third thread hammers register_schema on a
    // second mod: g_settings_mutex is global and register_schema holds it
    // across the settings file read and write, so a refused anchor parks
    // at the *first* step of CleanupOwnerState. The main thread then
    // re-enables `toggling` and registers an input action -- which needs
    // g_input_mutex, never g_settings_mutex, so it does not queue behind
    // the parked anchor and cannot be starved by it. When the parked
    // anchor finally runs it reaches g_actions and erases the live
    // action, which the duplicate-id check reports by accepting an id it
    // must reject.
    WotbModV3Handle toggling = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "data_toggle_owner.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &toggling) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            toggling, &ToggleOwnerEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(toggling) == WOTBMOD_V3_OK);

    WotbModV3Handle holder = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "data_settings_holder.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &holder) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            holder, &SettingsHolderEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(holder) == WOTBMOD_V3_OK);

    const void* raw_anchor_settings = nullptr;
    CHECK(
        bootstrap->query_interface(
            toggling,
            WOTBMOD_V3_IFACE_SETTINGS,
            WOTBMOD_V3_SETTINGS_VERSION,
            &raw_anchor_settings) == WOTBMOD_V3_OK);
    const WotbModV3SettingsApiV1* anchor_settings =
        static_cast<const WotbModV3SettingsApiV1*>(raw_anchor_settings);
    CHECK(anchor_settings != nullptr);

    const void* raw_anchor_input = nullptr;
    CHECK(
        bootstrap->query_interface(
            toggling,
            WOTBMOD_V3_IFACE_INPUT,
            WOTBMOD_V3_INPUT_VERSION,
            &raw_anchor_input) == WOTBMOD_V3_OK);
    const WotbModV3InputApiV1* anchor_input =
        static_cast<const WotbModV3InputApiV1*>(raw_anchor_input);
    CHECK(anchor_input != nullptr);

    // Big enough that saving and reloading it keeps g_settings_mutex held
    // for long enough to park a refused anchor across a whole re-enable.
    const uint32_t kAnchorSettingCount = 64u;
    std::vector<WotbModV3SettingDefinition> anchor_definitions(
        kAnchorSettingCount);
    std::string anchor_filler(400u, 'x');
    for (uint32_t index = 0u; index < kAnchorSettingCount; ++index) {
        WotbModV3SettingDefinition& definition = anchor_definitions[index];
        definition.struct_size = sizeof(definition);
        definition.api_version = WOTBMOD_V3_SETTINGS_VERSION;
        definition.type = WOTBMOD_V3_SETTING_STRING;
        char key[WOTBMOD_V3_SETTING_KEY_MAX] = {0};
        std::snprintf(key, sizeof(key), "regression.anchor%u", index);
        Copy(definition.key, sizeof(definition.key), key);
        Copy(
            definition.title,
            sizeof(definition.title),
            "Anchor lifetime regression");
        Copy(
            definition.description,
            sizeof(definition.description),
            "exists only to make the settings file worth writing");
        Copy(
            definition.default_text,
            sizeof(definition.default_text),
            anchor_filler.c_str());
    }

    WotbModV3InputActionDesc anchor_action = {};
    anchor_action.struct_size = sizeof(anchor_action);
    anchor_action.api_version = WOTBMOD_V3_INPUT_VERSION;
    anchor_action.value_type = WOTBMOD_V3_INPUT_VALUE_BUTTON;
    anchor_action.contexts = WOTBMOD_V3_CONTEXT_ALL;
    Copy(
        anchor_action.id,
        sizeof(anchor_action.id),
        "regression.anchor_action");
    Copy(
        anchor_action.display_name,
        sizeof(anchor_action.display_name),
        "Anchor regression action");
    Copy(
        anchor_action.description,
        sizeof(anchor_action.description),
        "registered while a refused anchor is parked mid-teardown");

    std::atomic<bool> anchor_stop{false};
    std::atomic<uint32_t> anchor_attempts{0u};
    std::atomic<uint32_t> anchor_refusals{0u};
    std::atomic<uint32_t> anchor_holder_saves{0u};

    std::vector<std::thread> anchor_workers;
    for (uint32_t worker = 0u; worker < 4u; ++worker) {
        anchor_workers.emplace_back([&]() {
            while (!anchor_stop.load()) {
                uint32_t version = 0u;
                anchor_attempts.fetch_add(1u);
                if (anchor_settings->get_schema_version(
                        toggling, &version) ==
                    WOTBMOD_V3_E_CANCELLED) {
                    anchor_refusals.fetch_add(1u);
                }
            }
        });
    }
    anchor_workers.emplace_back([&]() {
        while (!anchor_stop.load()) {
            if (anchor_settings->register_schema(
                    holder,
                    1u,
                    anchor_definitions.data(),
                    kAnchorSettingCount) == WOTBMOD_V3_OK) {
                anchor_holder_saves.fetch_add(1u);
            }
        }
    });

    const uint32_t kAnchorCycles = 24u;
    uint32_t anchor_lifecycle_errors = 0u;
    uint32_t anchor_cycles_exercised = 0u;
    uint32_t anchor_state_losses = 0u;
    for (uint32_t cycle = 0u; cycle < kAnchorCycles; ++cycle) {
        if (WotbModV3Runtime_Disable(toggling) != WOTBMOD_V3_OK) {
            ++anchor_lifecycle_errors;
        }
        // Give the workers time to enter the gate while the owner is down;
        // each refusal they start is an anchor that has to be destroyed.
        const uint32_t attempts_before = anchor_attempts.load();
        const uint32_t refusals_before = anchor_refusals.load();
        // Deadlined, because five busy-looping threads plus a global
        // settings mutex make "wait until the counter moves" a plausible
        // way to never finish. Expiry does not skip the cycle -- the break
        // falls through into the Enable and every check after it, which is
        // fine, none of them depend on the wait having succeeded. What it
        // costs is the refusals this cycle was meant to provoke, and
        // anchor_cycles_exercised counts only cycles where refusals
        // actually moved. So a wedged run fails
        // CHECK(anchor_cycles_exercised > 0u) instead of hanging.
        const std::chrono::steady_clock::time_point anchor_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (anchor_attempts.load() < attempts_before + 8u) {
            if (std::chrono::steady_clock::now() > anchor_deadline) break;
            std::this_thread::yield();
        }
        if (WotbModV3Runtime_Enable(toggling) != WOTBMOD_V3_OK) {
            ++anchor_lifecycle_errors;
        }
        WotbModV3Handle anchor_action_handle = WOTBMOD_V3_INVALID_HANDLE;
        if (anchor_input->register_action(
                toggling,
                &anchor_action,
                &anchor_action_handle) != WOTBMOD_V3_OK) {
            ++anchor_lifecycle_errors;
            continue;
        }
        // The id is registered and only this thread's next Disable may
        // remove it, so every reading of it as free is a refused anchor
        // having erased g_actions for this owner.
        for (uint32_t probe = 0u; probe < 64u; ++probe) {
            WotbModV3Handle duplicate = WOTBMOD_V3_INVALID_HANDLE;
            const WotbModV3Result again = anchor_input->register_action(
                toggling, &anchor_action, &duplicate);
            if (again != WOTBMOD_V3_E_ALREADY_EXISTS) {
                ++anchor_state_losses;
                break;
            }
            std::this_thread::yield();
        }
        if (anchor_refusals.load() != refusals_before) {
            ++anchor_cycles_exercised;
        }
    }
    anchor_stop.store(true);
    for (std::thread& worker : anchor_workers) {
        worker.join();
    }

    CHECK(anchor_lifecycle_errors == 0u);
    CHECK(anchor_holder_saves.load() > 0u);
    // The refused path was really taken, in cycles that were observed ...
    CHECK(anchor_refusals.load() > 0u);
    CHECK(anchor_cycles_exercised > 0u);
    // ... and taking it destroyed nothing that belonged to anyone else.
    CHECK(anchor_state_losses == 0u);

    // The enable-transition window, and the exact width of it.
    //
    // Mounting a mod's package creates handles the mod owns, and the host
    // has to do it before on_enable runs, because on_enable reads out of
    // that mount. At that instant a re-enabling mod is still
    // MOD_STATE_DISABLED, so CreateOwnedHandle refused it with
    // E_CANCELLED and re-enabling a package mod could never succeed. The
    // window exempts the enabling thread for exactly that span. It must
    // not exempt anything else: a disabled mod acquiring resources is a
    // worse defect than the one being fixed.
    //
    // `toggling` never registers a settings schema, so E_NOT_FOUND is
    // what "the gate let me through" looks like here, and the two results
    // below cannot be confused.
    CHECK(WotbModV3Runtime_Disable(toggling) == WOTBMOD_V3_OK);
    uint32_t transition_version = 0u;
    CHECK(
        anchor_settings->get_schema_version(
            toggling, &transition_version) == WOTBMOD_V3_E_CANCELLED);
    CHECK(
        wotbmod::v3::BeginModEnableTransition(toggling) ==
        WOTBMOD_V3_OK);
    WotbModV3Result foreign_probe_result = WOTBMOD_V3_OK;
    std::thread foreign_probe([&]() {
        uint32_t ignored = 0u;
        foreign_probe_result =
            anchor_settings->get_schema_version(toggling, &ignored);
    });
    foreign_probe.join();
    // The window belongs to the thread that opened it and to no other,
    // including any thread the disabled mod still has running.
    CHECK(foreign_probe_result == WOTBMOD_V3_E_CANCELLED);
    // The opener gets through -- this is the mount that could never run.
    CHECK(
        anchor_settings->get_schema_version(
            toggling, &transition_version) == WOTBMOD_V3_E_NOT_FOUND);
    wotbmod::v3::EndModEnableTransition(toggling);
    // Closing it puts the refusal back, anchor and all.
    CHECK(WotbModV3Runtime_Disable(toggling) == WOTBMOD_V3_OK);
    CHECK(
        anchor_settings->get_schema_version(
            toggling, &transition_version) == WOTBMOD_V3_E_CANCELLED);

    // And the other end of that boundary: an ENABLED record must be
    // admitted, not refused.
    //
    // This is not a hypothetical state at the call site. A mod that asks to
    // be disabled from inside one of its own callbacks is turned away by
    // the E_BUSY guard in WotbModV3Runtime_Disable, which returns before it
    // writes `state` or `accepting_callbacks`. The legacy facade, however,
    // has already set enabled=0 and WOTBMOD_STATE_DISABLED before calling
    // CallDisable, and CallDisable only logs the failure. So the next
    // enable arrives at the window with a legacy record that is disabled
    // and a v3 record that is still ENABLED. EnsureV3PackageMounted's
    // E_ALREADY_EXISTS -> OK branch exists for exactly this case and says
    // re-enable "must not be destructive".
    //
    // Refusing ENABLED here therefore strands the mod disabled until the
    // game restarts, because nothing afterwards moves the v3 record out of
    // ENABLED. The suite used to pass with that refusal in place only
    // because nothing constructed this state; so it is constructed here,
    // through the real mechanism rather than by assertion.
    WotbModV3Handle self_disabler = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(
            "data_self_disable.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &self_disabler) == WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            self_disabler, &SelfDisableEntry, &module) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Enable(self_disabler) == WOTBMOD_V3_OK);
    WotbModV3Runtime_DispatchFrame(9001u, 0.016);
    // The callback ran, and its self-disable was refused as designed.
    CHECK(g_self_disable_frames.load() == 1u);
    CHECK(g_self_disable_result.load() == WOTBMOD_V3_E_BUSY);
    // That refusal is what leaves the record enabled. Enable() sets
    // `enabled` and MOD_STATE_ENABLED together and the E_BUSY path touches
    // neither, so this pins the ENABLED state without depending on the
    // internal state enum's numbering (which is not the public one).
    CHECK(wotbmod::v3::IsModEnabled(self_disabler));
    // The window must admit it. This is the assertion that fails if anyone
    // "tightens" the boundary to an allow-list of CREATED/LOADED/DISABLED.
    CHECK(
        wotbmod::v3::BeginModEnableTransition(self_disabler) ==
        WOTBMOD_V3_OK);
    wotbmod::v3::EndModEnableTransition(self_disabler);
    // A record being torn down is still refused, which is what the list is
    // actually for.
    CHECK(WotbModV3Runtime_Disable(self_disabler) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(self_disabler) == WOTBMOD_V3_OK);

    CHECK(WotbModV3Runtime_Disable(toggling) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(toggling) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_Disable(holder) == WOTBMOD_V3_OK);
    CHECK(WotbModV3Runtime_DestroyMod(holder) == WOTBMOD_V3_OK);

    WotbModV3Runtime_Shutdown();

    std::printf(
        "v3 data truth tests passed: %u/%u\n",
        g_check_count,
        g_check_count);
    return 0;
}
