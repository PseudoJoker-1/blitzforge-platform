#include "v3_native_session_cluster.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace wotbmod {
namespace loader {
namespace session_cluster {
namespace {

Layout g_layout = {};
ChangeClusterFn g_change_cluster = nullptr;
DisconnectFn g_disconnect = nullptr;
LogFn g_log = nullptr;

/* Captured on OnHostChosen. Written on the client's login thread, read from
 * any thread; each is a whole pointer store, and readers treat any of them
 * being null as "not ready". */
volatile void* g_login_manager = nullptr;
volatile void* g_services = nullptr;
volatile void* g_connection_manager = nullptr;
volatile void* g_region = nullptr;   /* {ClusterHost* begin, ClusterHost* end} */
volatile LONG g_faulted = 0;

void Log(const char* text) {
    if (g_log) g_log(text);
}

bool PlausiblePointer(const void* pointer) {
    return reinterpret_cast<uintptr_t>(pointer) >= 0x10000u;
}

bool ReadPointer(const void* object, uint32_t offset, void** out) {
    *out = nullptr;
    if (!PlausiblePointer(object)) return false;
    __try {
        *out = *reinterpret_cast<void* const*>(
            static_cast<const uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadInt32(const void* object, uint32_t offset, int32_t* out) {
    *out = 0;
    if (!PlausiblePointer(object)) return false;
    __try {
        *out = *reinterpret_cast<const int32_t*>(
            static_cast<const uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadByte(const void* object, uint32_t offset, uint8_t* out) {
    *out = 0u;
    if (!PlausiblePointer(object)) return false;
    __try {
        *out = *(static_cast<const uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteByte(void* object, uint32_t offset, uint8_t value) {
    if (!PlausiblePointer(object)) return false;
    __try {
        *(static_cast<uint8_t*>(object) + offset) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/* MSVC x86 std::string: 16-byte SSO buffer or heap pointer, size at +16,
 * capacity at +20. Always NUL-terminates `out`. */
bool ReadStdString(const void* object, char* out, size_t out_size) {
    out[0] = '\0';
    if (!PlausiblePointer(object) || out_size == 0u) return false;
    __try {
        const uint8_t* bytes = static_cast<const uint8_t*>(object);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(bytes + 16);
        const uint32_t capacity = *reinterpret_cast<const uint32_t*>(bytes + 20);
        if (capacity < 15u || size > capacity || size > 4096u) return false;
        const char* data = capacity > 15u
            ? *reinterpret_cast<const char* const*>(bytes)
            : reinterpret_cast<const char*>(bytes);
        if (!data) return false;
        const size_t count = size < out_size - 1u ? size : out_size - 1u;
        memcpy(out, data, count);
        out[count] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

typedef void* (__fastcall* GetterFn)(void* self, void* edx);

/* services->vtbl[slot]() under SEH. */
bool CallGetter(void* object, uint32_t slot_byte_offset, void** out) {
    *out = nullptr;
    void* vtable = nullptr;
    if (!ReadPointer(object, 0u, &vtable) || !PlausiblePointer(vtable)) return false;
    void* entry = nullptr;
    if (!ReadPointer(vtable, slot_byte_offset, &entry) || !PlausiblePointer(entry)) {
        return false;
    }
    __try {
        *out = reinterpret_cast<GetterFn>(entry)(object, nullptr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return false;
    }
}

bool ReadFloat(const void* object, uint32_t offset, float* out) {
    *out = 0.0f;
    if (!PlausiblePointer(object)) return false;
    __try {
        *out = *reinterpret_cast<const float*>(
            static_cast<const uint8_t*>(object) + offset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/* "EU_C4": letters, digits, underscores, an underscore-C somewhere. */
bool LooksLikeClusterName(const char* name) {
    const size_t length = strlen(name);
    if (length < 3u || length > 31u) return false;
    for (size_t i = 0u; i < length; ++i) {
        const char c = name[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    return strstr(name, "_C") != nullptr;
}

/* login4.wotblitz.eu:20016 + id 4 -> EU_C4 (realm from the domain). */
void DeriveName(const char* url, int32_t id, char* out, size_t out_size) {
    const char* realm = "C";
    if (strstr(url, ".eu")) realm = "EU";
    else if (strstr(url, ".asia")) realm = "SG";
    else if (strstr(url, ".com")) realm = "NA";
    _snprintf_s(out, out_size, _TRUNCATE, "%s_C%d", realm, id);
}

/* Region::HasAliveCluster's per-host test: some login address answered
 * the ping. Entries are 208 bytes; a vector that is empty or malformed
 * reads as "not alive", never as a fault. */
bool HostAlive(const uint8_t* host) {
    void* begin = nullptr;
    void* end = nullptr;
    if (!ReadPointer(host, g_layout.host_addresses_begin_offset, &begin) ||
        !ReadPointer(host, g_layout.host_addresses_end_offset, &end)) {
        return false;
    }
    if (!PlausiblePointer(begin) || !PlausiblePointer(end) || end < begin) return false;
    const size_t bytes = static_cast<const uint8_t*>(end) - static_cast<const uint8_t*>(begin);
    if (g_layout.address_stride == 0u || bytes % g_layout.address_stride != 0u) return false;
    const size_t count = bytes / g_layout.address_stride;
    if (count > 64u) return false;
    const uint8_t* entry = static_cast<const uint8_t*>(begin);
    for (size_t i = 0u; i < count; ++i, entry += g_layout.address_stride) {
        float ping = 0.0f;
        if (ReadFloat(entry, g_layout.address_ping_offset, &ping) && ping > 0.0f) return true;
    }
    return false;
}

bool Fill(const uint8_t* host, const void* current_host, WotbModV3ClusterInfo* out) {
    void* descriptor = nullptr;
    if (!ReadPointer(host, g_layout.host_descriptor_offset, &descriptor) ||
        !PlausiblePointer(descriptor)) {
        return false;
    }
    int32_t id = 0;
    int32_t state = 0;
    if (!ReadInt32(descriptor, g_layout.descriptor_id_offset, &id) ||
        !ReadInt32(host, g_layout.host_state_offset, &state)) {
        return false;
    }
    char name[WOTBMOD_V3_MAX_NAME] = {};
    ReadStdString(static_cast<const uint8_t*>(descriptor) + g_layout.descriptor_name_offset,
                  name, sizeof(name));
    if (!LooksLikeClusterName(name)) {
        char url[160] = {};
        ReadStdString(static_cast<const uint8_t*>(descriptor) + g_layout.descriptor_url_offset,
                      url, sizeof(url));
        DeriveName(url, id, name, sizeof(name));
    }
    out->cluster_id = id;
    out->current = host == current_host ? 1u : 0u;
    out->alive = HostAlive(host) ? 1u : 0u;
    out->allowed = state != 1 ? 1u : 0u;
    out->ccu = -1;
    strncpy_s(out->name, sizeof(out->name), name, _TRUNCATE);
    return true;
}

bool RegionBounds(const uint8_t** begin, const uint8_t** end, uint32_t* count) {
    *begin = nullptr;
    *end = nullptr;
    *count = 0u;
    void* region = const_cast<void*>(g_region);
    void* first = nullptr;
    void* last = nullptr;
    if (!ReadPointer(region, 0u, &first) || !ReadPointer(region, 4u, &last)) return false;
    if (!PlausiblePointer(first) || !PlausiblePointer(last) || last < first) return false;
    const size_t bytes = static_cast<const uint8_t*>(last) - static_cast<const uint8_t*>(first);
    if (g_layout.host_stride == 0u || bytes % g_layout.host_stride != 0u) return false;
    const size_t n = bytes / g_layout.host_stride;
    if (n > kMaxClusters) return false;
    *begin = static_cast<const uint8_t*>(first);
    *end = static_cast<const uint8_t*>(last);
    *count = static_cast<uint32_t>(n);
    return true;
}

void* CurrentHost() {
    void* host = nullptr;
    ReadPointer(const_cast<void*>(g_connection_manager),
                g_layout.connection_current_host_offset, &host);
    return host;
}

}  // namespace

void Init(const Layout& layout, ChangeClusterFn change_cluster, DisconnectFn disconnect, LogFn log) {
    g_layout = layout;
    g_change_cluster = change_cluster;
    g_disconnect = disconnect;
    g_log = log;
    Shutdown();
}

bool OnHostChosen(void* login_manager, void* chosen_host) {
    if (!PlausiblePointer(login_manager)) {
        Log("session.cluster: OnHostChosen with a null login manager; not captured");
        return false;
    }
    void* services = nullptr;
    if (!ReadPointer(login_manager, g_layout.services_offset, &services) ||
        !PlausiblePointer(services)) {
        Log("session.cluster: LoginManager+services is not a pointer; not captured");
        return false;
    }
    void* login_check = nullptr;
    if (!CallGetter(services, g_layout.services_login_slot, &login_check) ||
        login_check != login_manager) {
        Log("session.cluster: services->login manager does not point back; not captured");
        return false;
    }
    void* connection = nullptr;
    if (!CallGetter(services, g_layout.services_connection_slot, &connection) ||
        !PlausiblePointer(connection)) {
        Log("session.cluster: services->connection manager is null; not captured");
        return false;
    }
    void* appctx_holder = nullptr;
    void* appctx = nullptr;
    void* region = nullptr;
    if (!CallGetter(services, g_layout.services_appctx_slot, &appctx_holder) ||
        !ReadPointer(appctx_holder, g_layout.appctx_owner_offset, &appctx) ||
        !ReadPointer(appctx, g_layout.appctx_region_offset, &region) ||
        !PlausiblePointer(region)) {
        Log("session.cluster: region vector is unreachable from services; not captured");
        return false;
    }
    g_login_manager = login_manager;
    g_services = services;
    g_connection_manager = connection;
    g_region = region;
    InterlockedExchange(&g_faulted, 0);
    const uint8_t* begin = nullptr;
    const uint8_t* end = nullptr;
    uint32_t count = 0u;
    char line[160] = {};
    if (!RegionBounds(&begin, &end, &count)) {
        Log("session.cluster: region vector has an unexpected shape; not captured");
        Shutdown();
        return false;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "session.cluster: captured login manager %p services %p region of %u hosts (chosen host %p)",
                login_manager, services, count, chosen_host);
    Log(line);
    return true;
}

bool Ready() {
    return g_login_manager != nullptr && g_services != nullptr &&
           g_connection_manager != nullptr && g_region != nullptr &&
           InterlockedCompareExchange(&g_faulted, 0, 0) == 0;
}

WotbModV3Result Enumerate(WotbModV3ClusterInfo* items, uint32_t* inout_count) {
    if (!inout_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!Ready()) {
        *inout_count = 0u;
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    const uint8_t* begin = nullptr;
    const uint8_t* end = nullptr;
    uint32_t count = 0u;
    if (!RegionBounds(&begin, &end, &count)) {
        *inout_count = 0u;
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (!items) {
        *inout_count = count;
        return WOTBMOD_V3_OK;
    }
    const uint32_t capacity = *inout_count;
    *inout_count = count;
    if (capacity < count) return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    const void* current = CurrentHost();
    uint32_t written = 0u;
    for (const uint8_t* host = begin; host != end; host += g_layout.host_stride) {
        WotbModV3ClusterInfo info = {};
        if (!Fill(host, current, &info)) return WOTBMOD_V3_E_PLATFORM;
        const uint32_t size = items[written].struct_size;
        const uint32_t version = items[written].api_version;
        items[written] = info;
        items[written].struct_size = size;
        items[written].api_version = version;
        ++written;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result GetCurrent(WotbModV3ClusterInfo* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!Ready()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    const void* current = CurrentHost();
    if (!PlausiblePointer(current)) return WOTBMOD_V3_E_NOT_FOUND;
    const uint8_t* begin = nullptr;
    const uint8_t* end = nullptr;
    uint32_t count = 0u;
    if (!RegionBounds(&begin, &end, &count)) return WOTBMOD_V3_E_PLATFORM;
    for (const uint8_t* host = begin; host != end; host += g_layout.host_stride) {
        if (host != current) continue;
        WotbModV3ClusterInfo info = {};
        if (!Fill(host, current, &info)) return WOTBMOD_V3_E_PLATFORM;
        const uint32_t size = out_info->struct_size;
        const uint32_t version = out_info->api_version;
        *out_info = info;
        out_info->struct_size = size;
        out_info->api_version = version;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_NOT_FOUND;   /* connected to a host outside the region list */
}

WotbModV3Result SetManual(bool manual) {
    if (!Ready()) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return WriteByte(const_cast<void*>(g_login_manager), g_layout.manual_flag_offset,
                     manual ? 1u : 0u)
        ? WOTBMOD_V3_OK
        : WOTBMOD_V3_E_PLATFORM;
}

WotbModV3Result Change(int32_t cluster_id) {
    if (!Ready() || !g_change_cluster) return WOTBMOD_V3_E_NOT_SUPPORTED;
    void* login_manager = const_cast<void*>(g_login_manager);
    char line[128] = {};
    if (cluster_id == WOTBMOD_V3_SESSION_CLUSTER_AUTO) {
        if (!g_disconnect) return WOTBMOD_V3_E_NOT_SUPPORTED;
        void* connection = const_cast<void*>(g_connection_manager);
        if (!WriteByte(login_manager, g_layout.manual_flag_offset, 0u)) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "session.cluster: AUTO -> ConnectionManager::Disconnect(%d) for the client's own pick",
                    kAutoDisconnectReason);
        Log(line);
        __try {
            g_disconnect(connection, nullptr, kAutoDisconnectReason, 0.0f, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&g_faulted, 1);
            Log("session.cluster: Disconnect faulted; backend retired");
            return WOTBMOD_V3_E_PLATFORM;
        }
        return WOTBMOD_V3_OK;
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "session.cluster: calling LoginManager::ChangeCluster(%d)", cluster_id);
    Log(line);
    __try {
        g_change_cluster(login_manager, nullptr, cluster_id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted, 1);
        Log("session.cluster: ChangeCluster faulted; backend retired");
        return WOTBMOD_V3_E_PLATFORM;
    }
    if (!WriteByte(login_manager, g_layout.manual_flag_offset,
                   cluster_id >= 0 ? 1u : 0u)) {
        Log("session.cluster: manual flag write failed after ChangeCluster");
        return WOTBMOD_V3_E_PLATFORM;
    }
    return WOTBMOD_V3_OK;
}

void Shutdown() {
    g_login_manager = nullptr;
    g_services = nullptr;
    g_connection_manager = nullptr;
    g_region = nullptr;
    InterlockedExchange(&g_faulted, 0);
}

}  // namespace session_cluster
}  // namespace loader
}  // namespace wotbmod
