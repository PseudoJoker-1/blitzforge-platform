#include "ges_services.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../../include/wotb_mod_runtime_v3.h"
#include "../../include/wotbmod/events_v1.h"
#include "client_services_backend.h"
#include "ges_schemas.h"
#include "wotb_mod_v3_internal.h"

namespace wotbmod {
namespace v3 {
namespace {

std::mutex g_mutex;
std::map<std::string, uint32_t> g_observe_refs;   /* type_name -> refcount */
/* Types whose first ges_observe answered E_NOT_SUPPORTED: the loader had not
 * captured the bus yet. They keep their refcount and are retried from the
 * frame boundary (GesRetryPendingObservations) until the backend takes them,
 * so a subscription made during start-up is not silently lost. */
std::set<std::string> g_observe_pending;
GesDeclaredBackendAccessor g_backend_accessor = nullptr;

bool CurrentBackend(ClientHostDeclaredBackend* out) {
    *out = ClientHostDeclaredBackend{};
    return g_backend_accessor && g_backend_accessor(out);
}
thread_local const WotbModV3GesEvent* t_current = nullptr;
thread_local uint32_t t_depth = 0u;
const uint32_t kMaxDepth = 8u;

bool ReadableRange(const void* pointer, size_t size) {
    if (!pointer || size == 0u) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(pointer, &info, sizeof(info)) == 0u) return false;
    if (info.State != MEM_COMMIT) return false;
    if ((info.Protect & PAGE_NOACCESS) != 0u ||
        (info.Protect & PAGE_GUARD) != 0u) {
        return false;
    }
    const uint8_t* region_end =
        static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    return static_cast<const uint8_t*>(pointer) + size <= region_end;
}

bool Readable(const WotbModV3GesEvent* event, uint32_t offset, uint32_t size) {
    if (!event || !event->payload) return false;
    if (event->payload_size != 0u && offset + size > event->payload_size) {
        return false;
    }
    return ReadableRange(
        static_cast<const uint8_t*>(event->payload) + offset, size);
}

/* The events runtime hands callbacks a COPY of the WotbModV3GesEvent, so
 * identity is the engine payload pointer of the delivery in flight on this
 * thread, not the struct address. */
bool CurrentDelivery(const WotbModV3GesEvent* event) {
    return event && t_current && event->payload &&
           event->payload == t_current->payload &&
           event->struct_size >= sizeof(WotbModV3GesEvent);
}

template <typename T>
WotbModV3Result ReadScalar(
    const WotbModV3GesEvent* event, uint32_t offset, T* out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (!CurrentDelivery(event)) return WOTBMOD_V3_E_OBJECT_DESTROYED;
    if (!Readable(event, offset, sizeof(T))) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memcpy(out, static_cast<const uint8_t*>(event->payload) + offset,
                sizeof(T));
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL ReadI32(
    const WotbModV3GesEvent* event, uint32_t offset, int32_t* out) {
    return ReadScalar(event, offset, out);
}
WotbModV3Result WOTBMOD_V3_CALL ReadU32(
    const WotbModV3GesEvent* event, uint32_t offset, uint32_t* out) {
    return ReadScalar(event, offset, out);
}
WotbModV3Result WOTBMOD_V3_CALL ReadF32(
    const WotbModV3GesEvent* event, uint32_t offset, float* out) {
    return ReadScalar(event, offset, out);
}
WotbModV3Result WOTBMOD_V3_CALL ReadBool(
    const WotbModV3GesEvent* event, uint32_t offset, uint8_t* out) {
    return ReadScalar(event, offset, out);
}
WotbModV3Result WOTBMOD_V3_CALL ReadPtr(
    const WotbModV3GesEvent* event, uint32_t offset, const void** out) {
    if (!out) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    uintptr_t value = 0u;
    const WotbModV3Result result = ReadScalar(event, offset, &value);
    if (result == WOTBMOD_V3_OK) *out = reinterpret_cast<const void*>(value);
    return result;
}

WotbModV3Result WOTBMOD_V3_CALL ReadCString(
    const WotbModV3GesEvent* event, uint32_t offset, char* buffer,
    uint32_t capacity) {
    if (!buffer || capacity == 0u) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    buffer[0] = '\0';
    const void* pointer = nullptr;
    const WotbModV3Result read = ReadPtr(event, offset, &pointer);
    if (read != WOTBMOD_V3_OK) return read;
    if (!pointer) return WOTBMOD_V3_E_NOT_FOUND;
    const char* text = static_cast<const char*>(pointer);
    uint32_t n = 0u;
    for (; n + 1u < capacity; ++n) {
        if (!ReadableRange(text + n, 1u)) {
            buffer[0] = '\0';
            return WOTBMOD_V3_E_PLATFORM;
        }
        buffer[n] = text[n];
        if (text[n] == '\0') return WOTBMOD_V3_OK;
    }
    buffer[n] = '\0';
    return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
}

struct CopyVisit {
    const char** names;
    uint32_t capacity;
    uint32_t count;
};

void CopyVisitFn(void* data, const char* name) {
    CopyVisit* visit = static_cast<CopyVisit*>(data);
    if (visit->count < visit->capacity) visit->names[visit->count] = name;
    ++visit->count;
}

void CollectVisitFn(void* data, const char* name) {
    static_cast<std::vector<std::string>*>(data)->push_back(name);
}

WotbModV3Result WOTBMOD_V3_CALL ListTypes(
    WotbModV3Handle mod, const char** names, uint32_t capacity,
    uint32_t* out_count) {
    if (!out_count || (capacity != 0u && !names)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_count = 0u;
    const WotbModV3Result permission = CheckNamedPermission(
        mod, "ges.observe", WOTBMOD_V3_PERMISSION_SAFE);
    if (permission != WOTBMOD_V3_OK) return permission;
    ClientHostDeclaredBackend backend = {};
    if (!CurrentBackend(&backend) || !backend.ges_list_types) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    CopyVisit visit = {names, capacity, 0u};
    const WotbModV3Result result =
        backend.ges_list_types(backend.user_data, &CopyVisitFn, &visit);
    *out_count = visit.count;
    if (result != WOTBMOD_V3_OK) return result;
    return capacity != 0u && visit.count > capacity
        ? WOTBMOD_V3_E_BUFFER_TOO_SMALL
        : WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL GetSchema(
    const char* type_name, uint32_t* out_id, uint32_t* out_size,
    uint32_t* out_fields) {
    if (!type_name || !out_id || !out_size || !out_fields) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const GesSchema* schema = GesFindSchema(type_name, out_id);
    *out_size = schema ? schema->payload_size : 0u;
    *out_fields = schema ? schema->field_count : 0u;
    return schema ? WOTBMOD_V3_OK : WOTBMOD_V3_E_NOT_FOUND;
}

WotbModV3Result WOTBMOD_V3_CALL SchemaField(
    uint32_t schema_id, uint32_t index, WotbModV3GesField* out) {
    if (!out || out->struct_size < sizeof(WotbModV3GesField) ||
        out->api_version != WOTBMOD_V3_GES_VERSION) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const GesSchema* schema = GesSchemaById(schema_id);
    if (!schema || index >= schema->field_count) return WOTBMOD_V3_E_NOT_FOUND;
    out->name = schema->fields[index].name;
    out->offset = schema->fields[index].offset;
    out->kind = schema->fields[index].kind;
    out->size = schema->fields[index].size;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL Publish(
    WotbModV3Handle mod, const char* type_name, const void* payload,
    uint32_t payload_size, uint32_t flags) {
    if (!type_name || !payload || payload_size == 0u ||
        (flags & ~static_cast<uint32_t>(WOTBMOD_V3_GES_PUBLISH_ECHO)) != 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    uint32_t schema_id = 0u;
    const GesSchema* schema = GesFindSchema(type_name, &schema_id);
    if (!schema || schema->payload_size == 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    if (payload_size != schema->payload_size) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const WotbModV3Result permission = CheckNamedPermission(
        mod, "ges.publish", WOTBMOD_V3_PERMISSION_REVIEWED);
    if (permission != WOTBMOD_V3_OK) return permission;
    if (t_depth >= kMaxDepth) return WOTBMOD_V3_E_LIMIT_REACHED;
    ClientHostDeclaredBackend backend = {};
    if (!CurrentBackend(&backend) || !backend.ges_publish) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    std::vector<uint8_t> copy(
        static_cast<const uint8_t*>(payload),
        static_cast<const uint8_t*>(payload) + payload_size);
    return backend.ges_publish(
        backend.user_data, type_name, copy.data(), payload_size, flags);
}

WotbModV3GesApiV1 BuildApi() {
    WotbModV3GesApiV1 api = {};
    api.struct_size = sizeof(api);
    api.api_version = WOTBMOD_V3_GES_VERSION;
    api.list_types = &ListTypes;
    api.read_i32 = &ReadI32;
    api.read_u32 = &ReadU32;
    api.read_f32 = &ReadF32;
    api.read_bool = &ReadBool;
    api.read_ptr = &ReadPtr;
    api.read_cstring = &ReadCString;
    api.get_schema = &GetSchema;
    api.schema_field = &SchemaField;
    api.publish = &Publish;
    return api;
}

const WotbModV3GesApiV1 kApi = BuildApi();

/* Every known type name from the backend, or empty without one. */
std::vector<std::string> KnownTypes(const ClientHostDeclaredBackend& backend) {
    std::vector<std::string> names;
    if (backend.ges_list_types) {
        backend.ges_list_types(backend.user_data, &CollectVisitFn, &names);
    }
    return names;
}

bool PatternMatchesTopic(const char* pattern, const std::string& topic) {
    const size_t length = std::strlen(pattern);
    if (length > 0u && pattern[length - 1u] == '*') {
        return topic.compare(0u, length - 1u, pattern, length - 1u) == 0;
    }
    return topic == pattern;
}

void AdjustObservation(const char* pattern, int delta) {
    if (!pattern || std::strncmp(pattern, "wotbmod.", 8u) != 0) return;
    ClientHostDeclaredBackend backend = {};
    if (!CurrentBackend(&backend) || !backend.ges_observe) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const std::string& name : KnownTypes(backend)) {
        char topic[WOTBMOD_V3_MAX_EVENT_TOPIC] = {};
        if (!GesTopicFromTypeName(name.c_str(), topic, sizeof(topic))) continue;
        if (!PatternMatchesTopic(pattern, topic)) continue;
        if (delta > 0) {
            uint32_t& refs = g_observe_refs[name];
            if (refs++ == 0u) {
                const WotbModV3Result added =
                    backend.ges_observe(backend.user_data, name.c_str(), 1u);
                if (added == WOTBMOD_V3_E_NOT_SUPPORTED) {
                    g_observe_pending.insert(name);
                }
            }
        } else {
            std::map<std::string, uint32_t>::iterator it =
                g_observe_refs.find(name);
            if (it == g_observe_refs.end()) continue;
            if (--it->second == 0u) {
                std::set<std::string>::iterator pending =
                    g_observe_pending.find(name);
                if (pending != g_observe_pending.end()) {
                    /* never registered natively: nothing to release */
                    g_observe_pending.erase(pending);
                } else {
                    backend.ges_observe(backend.user_data, name.c_str(), 0u);
                }
                g_observe_refs.erase(it);
            }
        }
    }
}

}  // namespace

const WotbModV3GesApiV1& GesApi() { return kApi; }

bool GesPatternCanMatch(const char* pattern, uint64_t source_mask) {
    if (!pattern || (source_mask & WOTBMOD_V3_EVENT_SOURCE_GES) == 0u) {
        return false;
    }
    const size_t prefix = sizeof(WOTBMOD_V3_GES_TOPIC_PREFIX) - 1u;
    const size_t length = std::strlen(pattern);
    if (length > 0u && pattern[length - 1u] == '*') {
        /* "wotbmod.*", "wotbmod.ges.*", "wotbmod.ges.Avatar.*": the stem and
         * the topic prefix must agree over their common length. */
        const size_t stem = length - 1u;
        const size_t common = stem < prefix ? stem : prefix;
        return std::strncmp(pattern, WOTBMOD_V3_GES_TOPIC_PREFIX, common) == 0;
    }
    return std::strncmp(pattern, WOTBMOD_V3_GES_TOPIC_PREFIX, prefix) == 0 &&
           length > prefix;
}

WotbModV3Result GesHostPublish(
    const char* type_name, const void* payload, uint32_t publisher_rva,
    uint32_t flags) {
    if (!type_name || !payload) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if (t_depth >= kMaxDepth) return WOTBMOD_V3_E_LIMIT_REACHED;
    WotbModV3GesEvent event = {};
    event.struct_size = sizeof(event);
    event.api_version = WOTBMOD_V3_GES_VERSION;
    const size_t name_length = std::strlen(type_name);
    if (name_length == 0u || name_length >= sizeof(event.type_name)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::memcpy(event.type_name, type_name, name_length + 1u);
    event.payload = payload;
    uint32_t schema_id = 0u;
    const GesSchema* schema = GesFindSchema(type_name, &schema_id);
    event.schema_id = schema_id;
    event.payload_size = schema ? schema->payload_size : 0u;
    event.publisher_rva = publisher_rva;
    event.flags = (flags & WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED) |
                  (schema ? (WOTBMOD_V3_GES_EVENT_SCHEMA_KNOWN |
                             WOTBMOD_V3_GES_EVENT_SIZE_KNOWN)
                          : 0u);
    char topic[WOTBMOD_V3_MAX_EVENT_TOPIC] = {};
    if (!GesTopicFromTypeName(type_name, topic, sizeof(topic))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const WotbModV3GesEvent* previous = t_current;
    t_current = &event;
    ++t_depth;
    const WotbModV3Result result =
        PublishSystemEvent(topic, &event, sizeof(event), 0u);
    --t_depth;
    t_current = previous;
    return result;
}

void GesOnPatternSubscribed(const char* pattern) {
    AdjustObservation(pattern, +1);
}

void GesRetryPendingObservations() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_observe_pending.empty()) return;
    ClientHostDeclaredBackend backend = {};
    if (!CurrentBackend(&backend) || !backend.ges_observe) return;
    const std::vector<std::string> names(g_observe_pending.begin(),
                                         g_observe_pending.end());
    for (const std::string& name : names) {
        if (g_observe_refs.find(name) == g_observe_refs.end()) {
            g_observe_pending.erase(name);
            continue;
        }
        const WotbModV3Result added =
            backend.ges_observe(backend.user_data, name.c_str(), 1u);
        /* Still no bus: keep waiting. Any other failure is final for this
         * type (unknown name, faulted backend): stop retrying it. */
        if (added != WOTBMOD_V3_E_NOT_SUPPORTED) g_observe_pending.erase(name);
    }
}

void GesOnPatternUnsubscribed(const char* pattern) {
    AdjustObservation(pattern, -1);
}

void GesSetDeclaredBackendAccessor(GesDeclaredBackendAccessor accessor) {
    g_backend_accessor = accessor;
}

void GesResetForTests() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_observe_refs.clear();
    g_observe_pending.clear();
    t_current = nullptr;
    t_depth = 0u;
}

}  // namespace v3
}  // namespace wotbmod
