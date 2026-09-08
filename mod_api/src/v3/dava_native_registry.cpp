#include "dava_native_registry.h"

#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../include/wotbmod/client_v1.h"

namespace wotbmod {
namespace v3 {

namespace {

std::mutex g_installed_mutex;
std::shared_ptr<DavaNativeRegistry> g_installed_registry;
std::mutex g_host_token_mutex;
WotbModDavaNativeToken g_next_host_token = 1u;

constexpr uint64_t kKnownCapabilities =
    WOTBMOD_DAVA_NATIVE_CAP_YAML |
    WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE |
    WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL |
    WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP |
    WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER |
    WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY;

struct DavaNativeRecord {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    uint32_t kind = 0u;
    WotbModDavaNativeProviderToken provider_token = 0u;
    uint32_t in_flight = 0u;
    bool retiring = false;
};

bool ValidCompatibility(uint32_t compatibility) {
    return compatibility == WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED ||
           compatibility == WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED;
}

bool HasCompleteGroups(const WotbModDavaNativeBackend& backend) {
    if ((backend.capabilities & ~kKnownCapabilities) != 0u ||
        (backend.capabilities != 0u && !backend.release)) {
        return false;
    }
    if ((backend.capabilities & WOTBMOD_DAVA_NATIVE_CAP_YAML) != 0u &&
        (!backend.yaml_parse_file || !backend.yaml_export_utf8)) {
        return false;
    }
    if ((backend.capabilities &
         WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE) != 0u &&
        (!backend.archive_open_file ||
         !backend.archive_get_entry_count ||
         !backend.archive_get_entry ||
         !backend.archive_read_entry)) {
        return false;
    }
    if ((backend.capabilities & WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL) != 0u &&
        !backend.material_mutate) {
        return false;
    }
    if ((backend.capabilities &
         WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP) != 0u &&
        !backend.mesh_hot_swap) {
        return false;
    }
    if ((backend.capabilities & WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER) != 0u &&
        !backend.tracer_create) {
        return false;
    }
    if ((backend.capabilities & WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY) != 0u &&
        (!backend.class_is_registered || !backend.class_create)) {
        return false;
    }
    return true;
}

bool BoundedString(const char* value, size_t capacity) {
    return value && std::memchr(value, '\0', capacity) != nullptr;
}

bool ValidOwner(WotbModV3Handle owner) {
    return owner != WOTBMOD_V3_INVALID_HANDLE;
}

bool ValidObjectKind(uint32_t kind) {
    return kind >= WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT &&
           kind <= WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE;
}

bool NextHostToken(WotbModDavaNativeToken* out_token) {
    if (!out_token) return false;
    std::lock_guard<std::mutex> lock(g_host_token_mutex);
    if (g_next_host_token == 0u) return false;
    *out_token = g_next_host_token++;
    return true;
}

bool ValidClassObjectKind(uint32_t kind) {
    return kind == WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL ||
           kind == WOTBMOD_DAVA_NATIVE_OBJECT_MESH ||
           kind == WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER ||
           kind == WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE ||
           kind == WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE;
}

bool ValidBuffer(const WotbModDavaNativeBuffer* buffer) {
    return buffer && buffer->struct_size >= sizeof(*buffer) &&
           ((buffer->data == nullptr && buffer->capacity == 0u) ||
            (buffer->data != nullptr && buffer->capacity != 0u));
}

bool ValidMaterialMutation(
    const WotbModDavaNativeMaterialMutation* mutation) {
    if (!mutation || mutation->struct_size < sizeof(*mutation) ||
        mutation->mutation_kind <
            WOTBMOD_DAVA_NATIVE_MATERIAL_SET_PROPERTY ||
        mutation->mutation_kind >
            WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_FLAG ||
        !BoundedString(mutation->name, sizeof(mutation->name)) ||
        (mutation->mutation_kind !=
             WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER &&
         mutation->name[0] == '\0')) {
        return false;
    }
    for (float value : mutation->values) {
        if (!std::isfinite(value)) return false;
    }
    if (mutation->mutation_kind ==
            WOTBMOD_DAVA_NATIVE_MATERIAL_SET_PROPERTY) {
        uint32_t width = 0u;
        switch (mutation->value_type) {
            case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT:
            case WOTBMOD_DAVA_NATIVE_VALUE_INT:
            case WOTBMOD_DAVA_NATIVE_VALUE_BOOL:
                width = 1u;
                break;
            case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT2:
                width = 2u;
                break;
            case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT3:
                width = 3u;
                break;
            case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT4:
                width = 4u;
                break;
            default:
                return false;
        }
        if (mutation->array_size == 0u ||
            mutation->array_size > 16u / width ||
            ((mutation->value_type == WOTBMOD_DAVA_NATIVE_VALUE_INT ||
              mutation->value_type == WOTBMOD_DAVA_NATIVE_VALUE_BOOL) &&
             mutation->array_size != 1u)) {
            return false;
        }
    } else if (mutation->array_size != 0u) {
        return false;
    }
    if (mutation->mutation_kind ==
            WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE) {
        return mutation->related_object != 0u;
    }
    if (mutation->mutation_kind ==
            WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER) {
        return mutation->related_object != 0u;
    }
    return mutation->related_object == 0u;
}

bool ValidTracerRequest(const WotbModDavaNativeTracerRequest* request) {
    if (!request || request->struct_size < sizeof(*request) ||
        request->reserved != 0u || !std::isfinite(request->width) ||
        !std::isfinite(request->lifetime_seconds) || request->width <= 0.0f ||
        request->lifetime_seconds <= 0.0f) {
        return false;
    }
    for (float value : request->origin) {
        if (!std::isfinite(value)) return false;
    }
    for (float value : request->destination) {
        if (!std::isfinite(value)) return false;
    }
    for (float value : request->color) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}

WotbModV3Result CallRelease(
    const WotbModDavaNativeBackend& backend,
    const DavaNativeRecord& record) {
    if (!backend.release || record.provider_token == 0u ||
        !ValidObjectKind(record.kind)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    try {
        return backend.release(
            backend.user_data,
            record.owner,
            record.kind,
            record.provider_token);
    } catch (...) {
        return WOTBMOD_V3_E_PLATFORM;
    }
}

}  // namespace

struct DavaNativeRegistry {
    WotbModDavaNativeBackend backend = {};
    mutable std::mutex mutex;
    std::condition_variable quiesced;
    std::unordered_map<WotbModDavaNativeToken, DavaNativeRecord> records;
    bool shutting_down = false;
};

namespace {

class InvocationLease final {
public:
    InvocationLease(
        DavaNativeRegistry* registry,
        WotbModV3Handle owner,
        WotbModDavaNativeToken token,
        uint32_t expected_kind)
        : registry_(registry), token_(token) {
        if (!registry_ || !ValidOwner(owner) || token == 0u) return;
        std::lock_guard<std::mutex> lock(registry_->mutex);
        const auto found = registry_->records.find(token);
        if (registry_->shutting_down || found == registry_->records.end() ||
            found->second.retiring || found->second.owner != owner ||
            (expected_kind != 0u && found->second.kind != expected_kind)) {
            return;
        }
        ++found->second.in_flight;
        provider_token_ = found->second.provider_token;
        kind_ = found->second.kind;
        acquired_ = true;
    }

    ~InvocationLease() {
        if (!acquired_) return;
        std::lock_guard<std::mutex> lock(registry_->mutex);
        const auto found = registry_->records.find(token_);
        if (found != registry_->records.end() &&
            found->second.in_flight != 0u) {
            --found->second.in_flight;
            if (found->second.in_flight == 0u) {
                registry_->quiesced.notify_all();
            }
        }
    }

    InvocationLease(const InvocationLease&) = delete;
    InvocationLease& operator=(const InvocationLease&) = delete;

    explicit operator bool() const { return acquired_; }
    WotbModDavaNativeProviderToken provider_token() const {
        return provider_token_;
    }
    uint32_t kind() const { return kind_; }

private:
    DavaNativeRegistry* registry_ = nullptr;
    WotbModDavaNativeToken token_ = 0u;
    WotbModDavaNativeProviderToken provider_token_ = 0u;
    uint32_t kind_ = 0u;
    bool acquired_ = false;
};

WotbModV3Result RegisterProviderObject(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    uint32_t kind,
    WotbModDavaNativeProviderToken provider_token,
    WotbModDavaNativeToken* out_token) {
    if (!registry || !ValidOwner(owner) || !ValidObjectKind(kind) ||
        provider_token == 0u || !out_token) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_token = 0u;
    try {
        WotbModDavaNativeToken token = 0u;
        if (!NextHostToken(&token)) {
            return WOTBMOD_V3_E_LIMIT_REACHED;
        }
        std::lock_guard<std::mutex> lock(registry->mutex);
        if (registry->shutting_down) return WOTBMOD_V3_E_BUSY;
        if (registry->records.count(token) != 0u) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        DavaNativeRecord record = {};
        record.owner = owner;
        record.kind = kind;
        record.provider_token = provider_token;
        registry->records.emplace(token, record);
        *out_token = token;
        return WOTBMOD_V3_OK;
    } catch (...) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
}

template <typename CreateFn, typename Request>
WotbModV3Result CreateProviderObject(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    uint64_t capability,
    uint32_t kind,
    CreateFn create,
    const Request& request,
    WotbModDavaNativeToken* out_token) {
    if (!registry || !ValidOwner(owner) || !out_token) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_token = 0u;
    if ((registry->backend.capabilities & capability) == 0u || !create) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    WotbModDavaNativeProviderToken provider_token = 0u;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    try {
        result = create(
            registry->backend.user_data,
            owner,
            request,
            &provider_token);
    } catch (...) {
        result = WOTBMOD_V3_E_PLATFORM;
    }
    if (result != WOTBMOD_V3_OK) {
        return result;
    }
    if (provider_token == 0u) return WOTBMOD_V3_E_PLATFORM;

    result = RegisterProviderObject(
        registry, owner, kind, provider_token, out_token);
    if (result != WOTBMOD_V3_OK) {
        DavaNativeRecord record = {};
        record.owner = owner;
        record.kind = kind;
        record.provider_token = provider_token;
        (void)CallRelease(registry->backend, record);
    }
    return result;
}

WotbModV3Result ValidateBufferResult(
    WotbModV3Result result,
    const WotbModDavaNativeBuffer& buffer) {
    if (result == WOTBMOD_V3_OK && buffer.size > buffer.capacity) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    return result;
}

}  // namespace

WotbModV3Result CreateDavaNativeRegistry(
    const WotbModDavaNativeBackend* backend,
    DavaNativeRegistry** out_registry) {
    if (!out_registry) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_registry = nullptr;
    if (!backend || backend->struct_size < sizeof(*backend) ||
        backend->api_version != WOTBMOD_DAVA_NATIVE_BACKEND_VERSION ||
        !ValidCompatibility(backend->compatibility_state)) {
        return WOTBMOD_V3_E_CLIENT_MISMATCH;
    }
    if (!HasCompleteGroups(*backend)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    try {
        DavaNativeRegistry* registry = new DavaNativeRegistry();
        registry->backend = *backend;
        registry->backend.struct_size = sizeof(registry->backend);
        *out_registry = registry;
        return WOTBMOD_V3_OK;
    } catch (...) {
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
}

void DestroyDavaNativeRegistry(DavaNativeRegistry* registry) {
    if (!registry) return;
    std::vector<DavaNativeRecord> records;
    {
        std::unique_lock<std::mutex> lock(registry->mutex);
        registry->shutting_down = true;
        for (auto& item : registry->records) item.second.retiring = true;
        registry->quiesced.wait(lock, [registry]() {
            for (const auto& item : registry->records) {
                if (item.second.in_flight != 0u) return false;
            }
            return true;
        });
        records.reserve(registry->records.size());
        for (const auto& item : registry->records) {
            records.push_back(item.second);
        }
        registry->records.clear();
    }
    for (const DavaNativeRecord& record : records) {
        (void)CallRelease(registry->backend, record);
    }
    delete registry;
}

uint64_t DavaNativeRegistryCapabilities(
    const DavaNativeRegistry* registry) {
    return registry ? registry->backend.capabilities : 0u;
}

WotbModV3Result DavaNativeYamlParseFile(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_document) {
    if (!resolved_file_path || resolved_file_path[0] == '\0') {
        if (out_document) *out_document = 0u;
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return CreateProviderObject(
        registry,
        owner,
        WOTBMOD_DAVA_NATIVE_CAP_YAML,
        WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT,
        registry ? registry->backend.yaml_parse_file : nullptr,
        resolved_file_path,
        out_document);
}

WotbModV3Result DavaNativeYamlExportUtf8(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken document,
    WotbModDavaNativeBuffer* inout_utf8_yaml) {
    if (!registry || !ValidBuffer(inout_utf8_yaml)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    InvocationLease lease(
        registry,
        owner,
        document,
        WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT);
    if (!lease) return WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    try {
        result = registry->backend.yaml_export_utf8(
            registry->backend.user_data,
            owner,
            lease.provider_token(),
            inout_utf8_yaml);
    } catch (...) {
        result = WOTBMOD_V3_E_PLATFORM;
    }
    return ValidateBufferResult(result, *inout_utf8_yaml);
}

WotbModV3Result DavaNativeArchiveOpenFile(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_archive) {
    if (!resolved_file_path || resolved_file_path[0] == '\0') {
        if (out_archive) *out_archive = 0u;
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return CreateProviderObject(
        registry,
        owner,
        WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE,
        registry ? registry->backend.archive_open_file : nullptr,
        resolved_file_path,
        out_archive);
}

WotbModV3Result DavaNativeArchiveGetEntryCount(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t* out_count) {
    if (!registry || !out_count) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_count = 0u;
    InvocationLease lease(
        registry,
        owner,
        archive,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE);
    if (!lease) return WOTBMOD_V3_E_INVALID_HANDLE;
    try {
        return registry->backend.archive_get_entry_count(
            registry->backend.user_data,
            owner,
            lease.provider_token(),
            out_count);
    } catch (...) {
        *out_count = 0u;
        return WOTBMOD_V3_E_PLATFORM;
    }
}

WotbModV3Result DavaNativeArchiveGetEntry(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* out_entry) {
    if (!registry || !out_entry ||
        out_entry->struct_size < sizeof(*out_entry)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::memset(out_entry, 0, sizeof(*out_entry));
    out_entry->struct_size = sizeof(*out_entry);
    InvocationLease lease(
        registry,
        owner,
        archive,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE);
    if (!lease) return WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    try {
        result = registry->backend.archive_get_entry(
            registry->backend.user_data,
            owner,
            lease.provider_token(),
            index,
            out_entry);
    } catch (...) {
        result = WOTBMOD_V3_E_PLATFORM;
    }
    if (result == WOTBMOD_V3_OK &&
        !BoundedString(out_entry->relative_path,
                       sizeof(out_entry->relative_path))) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    return result;
}

WotbModV3Result DavaNativeArchiveReadEntry(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeBuffer* inout_data) {
    if (!registry || !ValidBuffer(inout_data)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    InvocationLease lease(
        registry,
        owner,
        archive,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE);
    if (!lease) return WOTBMOD_V3_E_INVALID_HANDLE;
    WotbModV3Result result = WOTBMOD_V3_E_PLATFORM;
    try {
        result = registry->backend.archive_read_entry(
            registry->backend.user_data,
            owner,
            lease.provider_token(),
            index,
            inout_data);
    } catch (...) {
        result = WOTBMOD_V3_E_PLATFORM;
    }
    return ValidateBufferResult(result, *inout_data);
}

WotbModV3Result DavaNativeMaterialMutate(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken material,
    const WotbModDavaNativeMaterialMutation* mutation) {
    if (!registry || !ValidMaterialMutation(mutation)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if ((registry->backend.capabilities &
         WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL) == 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    InvocationLease material_lease(
        registry,
        owner,
        material,
        WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL);
    if (!material_lease) return WOTBMOD_V3_E_INVALID_HANDLE;

    WotbModDavaNativeMaterialMutation provider_mutation = *mutation;
    InvocationLease related_lease(
        registry,
        owner,
        mutation->related_object,
        mutation->mutation_kind ==
                WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE
            ? WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE
            : mutation->mutation_kind ==
                    WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER
                ? WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER
                : 0u);
    if (mutation->related_object != 0u) {
        if (!related_lease) return WOTBMOD_V3_E_INVALID_HANDLE;
        provider_mutation.related_object = related_lease.provider_token();
    }
    try {
        return registry->backend.material_mutate(
            registry->backend.user_data,
            owner,
            material_lease.provider_token(),
            &provider_mutation);
    } catch (...) {
        return WOTBMOD_V3_E_PLATFORM;
    }
}

WotbModV3Result DavaNativeMeshHotSwap(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken consumer,
    WotbModDavaNativeToken replacement_mesh) {
    if (!registry) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if ((registry->backend.capabilities &
         WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP) == 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    InvocationLease consumer_lease(
        registry,
        owner,
        consumer,
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER);
    InvocationLease mesh_lease(
        registry,
        owner,
        replacement_mesh,
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH);
    if (!consumer_lease || !mesh_lease) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    try {
        return registry->backend.mesh_hot_swap(
            registry->backend.user_data,
            owner,
            consumer_lease.provider_token(),
            mesh_lease.provider_token());
    } catch (...) {
        return WOTBMOD_V3_E_PLATFORM;
    }
}

WotbModV3Result DavaNativeTracerCreate(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeToken* out_tracer) {
    if (!ValidTracerRequest(request)) {
        if (out_tracer) *out_tracer = 0u;
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return CreateProviderObject(
        registry,
        owner,
        WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER,
        WOTBMOD_DAVA_NATIVE_OBJECT_TRACER,
        registry ? registry->backend.tracer_create : nullptr,
        request,
        out_tracer);
}

WotbModV3Result DavaNativeClassIsRegistered(
    DavaNativeRegistry* registry,
    const char* class_name,
    uint32_t* out_registered) {
    if (!registry || !class_name || !out_registered ||
        !BoundedString(class_name, WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME) ||
        class_name[0] == '\0') {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_registered = 0u;
    if ((registry->backend.capabilities &
         WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY) == 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    try {
        const WotbModV3Result result =
            registry->backend.class_is_registered(
                registry->backend.user_data,
                class_name,
                out_registered);
        if (result == WOTBMOD_V3_OK && *out_registered > 1u) {
            *out_registered = 0u;
            return WOTBMOD_V3_E_PLATFORM;
        }
        return result;
    } catch (...) {
        *out_registered = 0u;
        return WOTBMOD_V3_E_PLATFORM;
    }
}

WotbModV3Result DavaNativeClassCreate(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeToken* out_object) {
    if (!out_object) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_object = 0u;
    if (!request || request->struct_size < sizeof(*request) ||
        !ValidClassObjectKind(request->object_kind) ||
        !BoundedString(request->class_name, sizeof(request->class_name)) ||
        request->class_name[0] == '\0' ||
        ((request->payload == nullptr) != (request->payload_size == 0u))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    uint32_t registered = 0u;
    const WotbModV3Result registered_result =
        DavaNativeClassIsRegistered(
            registry, request->class_name, &registered);
    if (registered_result != WOTBMOD_V3_OK) return registered_result;
    if (registered == 0u) return WOTBMOD_V3_E_NOT_SUPPORTED;
    return CreateProviderObject(
        registry,
        owner,
        WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY,
        request->object_kind,
        registry ? registry->backend.class_create : nullptr,
        request,
        out_object);
}

WotbModV3Result DavaNativeSceneMaterial(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object) {
    if (!out_object) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_object = 0u;
    if (!request || request->struct_size < sizeof(*request) ||
        !BoundedString(request->node_name, sizeof(request->node_name))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return CreateProviderObject(
        registry,
        owner,
        WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL,
        WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL,
        registry ? registry->backend.scene_material : nullptr,
        request,
        out_object);
}

WotbModV3Result DavaNativeSceneNodes(
    DavaNativeRegistry* registry,
    char* buffer,
    uint32_t* inout_size) {
    if (!registry || !inout_size) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    if ((registry->backend.capabilities &
         WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL) == 0u ||
        !registry->backend.scene_nodes) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    try {
        return registry->backend.scene_nodes(
            registry->backend.user_data, buffer, inout_size);
    } catch (...) {
        return WOTBMOD_V3_E_PLATFORM;
    }
}

WotbModV3Result DavaNativeRelease(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    WotbModDavaNativeToken token) {
    if (!registry || !ValidOwner(owner) || token == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    DavaNativeRecord record = {};
    {
        std::unique_lock<std::mutex> lock(registry->mutex);
        const auto found = registry->records.find(token);
        if (found == registry->records.end() || found->second.owner != owner ||
            found->second.retiring) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        found->second.retiring = true;
        registry->quiesced.wait(lock, [registry, token]() {
            const auto current = registry->records.find(token);
            return current == registry->records.end() ||
                   current->second.in_flight == 0u;
        });
        const auto quiesced = registry->records.find(token);
        if (quiesced == registry->records.end() ||
            quiesced->second.owner != owner ||
            !quiesced->second.retiring) {
            return WOTBMOD_V3_E_PLATFORM;
        }
        record = quiesced->second;
    }

    const WotbModV3Result result = CallRelease(registry->backend, record);
    std::lock_guard<std::mutex> lock(registry->mutex);
    const auto found = registry->records.find(token);
    if (found == registry->records.end()) return WOTBMOD_V3_E_PLATFORM;
    if (result == WOTBMOD_V3_OK) {
        registry->records.erase(found);
    } else {
        found->second.retiring = false;
        registry->quiesced.notify_all();
    }
    return result;
}

uint32_t DavaNativeReleaseOwner(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner) {
    if (!registry || !ValidOwner(owner)) return 0u;
    std::vector<WotbModDavaNativeToken> tokens;
    {
        std::lock_guard<std::mutex> lock(registry->mutex);
        tokens.reserve(registry->records.size());
        for (const auto& item : registry->records) {
            if (item.second.owner == owner && !item.second.retiring) {
                tokens.push_back(item.first);
            }
        }
    }
    uint32_t released = 0u;
    for (WotbModDavaNativeToken token : tokens) {
        if (DavaNativeRelease(registry, owner, token) == WOTBMOD_V3_OK) {
            ++released;
        }
    }
    return released;
}

namespace {

std::shared_ptr<DavaNativeRegistry> AcquireInstalledRegistry() {
    std::lock_guard<std::mutex> lock(g_installed_mutex);
    return g_installed_registry;
}

}  // namespace

WotbModV3Result InstallDavaNativeBackend(
    const WotbModDavaNativeBackend* backend) {
    DavaNativeRegistry* created = nullptr;
    const WotbModV3Result result =
        CreateDavaNativeRegistry(backend, &created);
    if (result != WOTBMOD_V3_OK) return result;
    std::shared_ptr<DavaNativeRegistry> replacement(
        created,
        [](DavaNativeRegistry* registry) {
            DestroyDavaNativeRegistry(registry);
        });
    std::shared_ptr<DavaNativeRegistry> previous;
    {
        std::lock_guard<std::mutex> lock(g_installed_mutex);
        previous = std::move(g_installed_registry);
        g_installed_registry = std::move(replacement);
    }
    previous.reset();
    return WOTBMOD_V3_OK;
}

void RemoveDavaNativeBackend() {
    std::shared_ptr<DavaNativeRegistry> previous;
    {
        std::lock_guard<std::mutex> lock(g_installed_mutex);
        previous = std::move(g_installed_registry);
    }
    previous.reset();
}

uint64_t InstalledDavaNativeCapabilities() {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return DavaNativeRegistryCapabilities(registry.get());
}

WotbModV3Result InstalledDavaNativeYamlParseFile(
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_document) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    if (!registry) {
        if (out_document) *out_document = 0u;
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return DavaNativeYamlParseFile(
        registry.get(), owner, resolved_file_path, out_document);
}

WotbModV3Result InstalledDavaNativeYamlExportUtf8(
    WotbModV3Handle owner,
    WotbModDavaNativeToken document,
    WotbModDavaNativeBuffer* inout_utf8_yaml) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeYamlExportUtf8(
              registry.get(), owner, document, inout_utf8_yaml)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeArchiveOpenFile(
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeToken* out_archive) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    if (!registry) {
        if (out_archive) *out_archive = 0u;
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return DavaNativeArchiveOpenFile(
        registry.get(), owner, resolved_file_path, out_archive);
}

WotbModV3Result InstalledDavaNativeArchiveGetEntryCount(
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t* out_count) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeArchiveGetEntryCount(
              registry.get(), owner, archive, out_count)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeArchiveGetEntry(
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* out_entry) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeArchiveGetEntry(
              registry.get(), owner, archive, index, out_entry)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeArchiveReadEntry(
    WotbModV3Handle owner,
    WotbModDavaNativeToken archive,
    uint32_t index,
    WotbModDavaNativeBuffer* inout_data) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeArchiveReadEntry(
              registry.get(), owner, archive, index, inout_data)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeMaterialMutate(
    WotbModV3Handle owner,
    WotbModDavaNativeToken material,
    const WotbModDavaNativeMaterialMutation* mutation) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeMaterialMutate(
              registry.get(), owner, material, mutation)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeMeshHotSwap(
    WotbModV3Handle owner,
    WotbModDavaNativeToken consumer,
    WotbModDavaNativeToken replacement_mesh) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeMeshHotSwap(
              registry.get(), owner, consumer, replacement_mesh)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeTracerCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeToken* out_tracer) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    if (!registry) {
        if (out_tracer) *out_tracer = 0u;
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return DavaNativeTracerCreate(
        registry.get(), owner, request, out_tracer);
}

WotbModV3Result InstalledDavaNativeClassIsRegistered(
    const char* class_name,
    uint32_t* out_registered) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    if (!registry) {
        if (out_registered) *out_registered = 0u;
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return DavaNativeClassIsRegistered(
        registry.get(), class_name, out_registered);
}

WotbModV3Result InstalledDavaNativeClassCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeToken* out_object) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    if (!registry) {
        if (out_object) *out_object = 0u;
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return DavaNativeClassCreate(
        registry.get(), owner, request, out_object);
}

WotbModV3Result InstalledDavaNativeSceneMaterial(
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeSceneMaterial(registry.get(), owner, request, out_object)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result DavaNativeSceneBatch(
    DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object) {
    if (!out_object) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_object = 0u;
    if (!request || request->struct_size < sizeof(*request) ||
        !BoundedString(request->node_name, sizeof(request->node_name))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!registry || !registry->backend.scene_batch) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    return CreateProviderObject(
        registry,
        owner,
        WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP,
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER,
        registry->backend.scene_batch,
        request,
        out_object);
}

WotbModV3Result InstalledDavaNativeSceneNodes(
    char* buffer,
    uint32_t* inout_size) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeSceneNodes(registry.get(), buffer, inout_size)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeSceneBatch(
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeToken* out_object) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeSceneBatch(registry.get(), owner, request, out_object)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result InstalledDavaNativeRelease(
    WotbModV3Handle owner,
    WotbModDavaNativeToken token) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeRelease(registry.get(), owner, token)
        : WOTBMOD_V3_E_NOT_SUPPORTED;
}

uint32_t InstalledDavaNativeReleaseOwner(WotbModV3Handle owner) {
    const std::shared_ptr<DavaNativeRegistry> registry =
        AcquireInstalledRegistry();
    return registry
        ? DavaNativeReleaseOwner(registry.get(), owner)
        : 0u;
}

}  // namespace v3
}  // namespace wotbmod
