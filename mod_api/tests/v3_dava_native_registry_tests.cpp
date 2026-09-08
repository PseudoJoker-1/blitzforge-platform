#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../include/wotb_mod_dava_native.h"
#include "../src/v3/dava_native_registry.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(condition)) {                                                    \
            ++g_failures;                                                      \
            std::fprintf(                                                      \
                stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
        }                                                                      \
    } while (0)

struct FakeObject {
    WotbModV3Handle owner = 0u;
    uint32_t kind = 0u;
};

struct FakeBackend {
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable resume;
    std::unordered_map<uint64_t, FakeObject> objects;
    uint64_t next_token = 1000u;
    uint32_t release_count = 0u;
    bool fail_release = false;
    bool overflow_buffer = false;
    bool malformed_entry = false;
    bool block_yaml_export = false;
    bool yaml_export_entered = false;
    bool allow_yaml_export = false;
    uint64_t last_material_related = 0u;
    uint64_t last_mesh_consumer = 0u;
    uint64_t last_mesh_replacement = 0u;
};

uint64_t AddObject(
    FakeBackend* fake,
    WotbModV3Handle owner,
    uint32_t kind) {
    std::lock_guard<std::mutex> lock(fake->mutex);
    const uint64_t token = fake->next_token++;
    fake->objects.emplace(token, FakeObject{owner, kind});
    return token;
}

bool HasObject(
    FakeBackend* fake,
    WotbModV3Handle owner,
    uint64_t token,
    uint32_t kind) {
    std::lock_guard<std::mutex> lock(fake->mutex);
    const auto found = fake->objects.find(token);
    return found != fake->objects.end() && found->second.owner == owner &&
           found->second.kind == kind;
}

WotbModV3Result CreatePathObject(
    void* user_data,
    WotbModV3Handle owner,
    const char* path,
    WotbModDavaNativeProviderToken* out_provider_token,
    uint32_t kind) {
    if (!user_data || owner == 0u || !path || !path[0] ||
        !out_provider_token) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_provider_token =
        AddObject(static_cast<FakeBackend*>(user_data), owner, kind);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeYamlParse(
    void* user_data,
    WotbModV3Handle owner,
    const char* path,
    WotbModDavaNativeProviderToken* out_provider_token) {
    return CreatePathObject(
        user_data,
        owner,
        path,
        out_provider_token,
        WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT);
}

WotbModV3Result CopyBuffer(
    FakeBackend* fake,
    const void* source,
    uint32_t size,
    WotbModDavaNativeBuffer* buffer) {
    if (!buffer || buffer->struct_size < sizeof(*buffer)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    buffer->size = fake->overflow_buffer ? buffer->capacity + 1u : size;
    if (fake->overflow_buffer) return WOTBMOD_V3_OK;
    if (!buffer->data || buffer->capacity < size) {
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    std::memcpy(buffer->data, source, size);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeYamlExport(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken document,
    WotbModDavaNativeBuffer* buffer) {
    FakeBackend* fake = static_cast<FakeBackend*>(user_data);
    if (!HasObject(
            fake,
            owner,
            document,
            WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    {
        std::unique_lock<std::mutex> lock(fake->mutex);
        if (fake->block_yaml_export) {
            fake->yaml_export_entered = true;
            fake->entered.notify_all();
            fake->resume.wait(lock, [fake]() {
                return fake->allow_yaml_export;
            });
        }
    }
    static const char kYaml[] = "native: true\n";
    return CopyBuffer(fake, kYaml, sizeof(kYaml) - 1u, buffer);
}

WotbModV3Result WOTBMOD_V3_CALL FakeArchiveOpen(
    void* user_data,
    WotbModV3Handle owner,
    const char* path,
    WotbModDavaNativeProviderToken* out_provider_token) {
    return CreatePathObject(
        user_data,
        owner,
        path,
        out_provider_token,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE);
}

WotbModV3Result WOTBMOD_V3_CALL FakeArchiveCount(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t* out_count) {
    FakeBackend* fake = static_cast<FakeBackend*>(user_data);
    if (!out_count ||
        !HasObject(
            fake,
            owner,
            archive,
            WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    *out_count = 2u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeArchiveEntry(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* out_entry) {
    FakeBackend* fake = static_cast<FakeBackend*>(user_data);
    if (!out_entry || index >= 2u ||
        !HasObject(
            fake,
            owner,
            archive,
            WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    out_entry->index = index;
    out_entry->original_size = index == 0u ? 3u : 4u;
    if (fake->malformed_entry) {
        std::memset(
            out_entry->relative_path,
            'x',
            sizeof(out_entry->relative_path));
    } else {
        strcpy_s(
            out_entry->relative_path,
            sizeof(out_entry->relative_path),
            index == 0u ? "a.txt" : "b.bin");
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeArchiveRead(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t index,
    WotbModDavaNativeBuffer* buffer) {
    FakeBackend* fake = static_cast<FakeBackend*>(user_data);
    if (index >= 2u ||
        !HasObject(
            fake,
            owner,
            archive,
            WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    static const uint8_t kA[] = {1u, 2u, 3u};
    static const uint8_t kB[] = {4u, 5u, 6u, 7u};
    return index == 0u
        ? CopyBuffer(fake, kA, sizeof(kA), buffer)
        : CopyBuffer(fake, kB, sizeof(kB), buffer);
}

WotbModV3Result WOTBMOD_V3_CALL FakeMaterialMutate(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken material,
    const WotbModDavaNativeMaterialMutation* mutation) {
    FakeBackend* fake = static_cast<FakeBackend*>(user_data);
    if (!mutation ||
        !HasObject(
            fake,
            owner,
            material,
            WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    if (mutation->mutation_kind ==
        WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE) {
        if (!HasObject(
                fake,
                owner,
                mutation->related_object,
                WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE)) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        fake->last_material_related = mutation->related_object;
    } else if (mutation->mutation_kind ==
               WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER) {
        if (!HasObject(
                fake,
                owner,
                mutation->related_object,
                WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER)) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        fake->last_material_related = mutation->related_object;
    }
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeMeshHotSwap(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken consumer,
    WotbModDavaNativeProviderToken replacement_mesh) {
    FakeBackend* fake = static_cast<FakeBackend*>(user_data);
    if (!HasObject(
            fake,
            owner,
            consumer,
            WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER) ||
        !HasObject(
            fake,
            owner,
            replacement_mesh,
            WOTBMOD_DAVA_NATIVE_OBJECT_MESH)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    fake->last_mesh_consumer = consumer;
    fake->last_mesh_replacement = replacement_mesh;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeTracerCreate(
    void* user_data,
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeProviderToken* out_provider_token) {
    if (!request || request->width <= 0.0f) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_provider_token = AddObject(
        static_cast<FakeBackend*>(user_data),
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_TRACER);
    return WOTBMOD_V3_OK;
}

bool KnownClass(const char* class_name) {
    return std::strcmp(class_name, "DAVA::NMaterial") == 0 ||
           std::strcmp(class_name, "DAVA::Texture") == 0 ||
           std::strcmp(class_name, "DAVA::RenderComponent") == 0 ||
           std::strcmp(class_name, "DAVA::Mesh") == 0 ||
           std::strcmp(class_name, "DAVA::UIControl") == 0;
}

WotbModV3Result WOTBMOD_V3_CALL FakeClassRegistered(
    void*,
    const char* class_name,
    uint32_t* out_registered) {
    if (!class_name || !out_registered) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *out_registered = KnownClass(class_name) ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeClassCreate(
    void* user_data,
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeProviderToken* out_provider_token) {
    if (!request || !KnownClass(request->class_name) ||
        !out_provider_token) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
    *out_provider_token = AddObject(
        static_cast<FakeBackend*>(user_data), owner, request->object_kind);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL FakeRelease(
    void* user_data,
    WotbModV3Handle owner,
    uint32_t kind,
    WotbModDavaNativeProviderToken provider_token) {
    FakeBackend* fake = static_cast<FakeBackend*>(user_data);
    std::lock_guard<std::mutex> lock(fake->mutex);
    const auto found = fake->objects.find(provider_token);
    if (found == fake->objects.end() || found->second.owner != owner ||
        found->second.kind != kind) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    if (fake->fail_release) return WOTBMOD_V3_E_BUSY;
    fake->objects.erase(found);
    ++fake->release_count;
    return WOTBMOD_V3_OK;
}

WotbModDavaNativeBackend MakeBackend(FakeBackend* fake) {
    WotbModDavaNativeBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.api_version = WOTBMOD_DAVA_NATIVE_BACKEND_VERSION;
    backend.binding_pack_version = 111900834u;
    backend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_DEGRADED;
    backend.capabilities =
        WOTBMOD_DAVA_NATIVE_CAP_YAML |
        WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE |
        WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL |
        WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP |
        WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER |
        WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY;
    backend.user_data = fake;
    backend.yaml_parse_file = &FakeYamlParse;
    backend.yaml_export_utf8 = &FakeYamlExport;
    backend.archive_open_file = &FakeArchiveOpen;
    backend.archive_get_entry_count = &FakeArchiveCount;
    backend.archive_get_entry = &FakeArchiveEntry;
    backend.archive_read_entry = &FakeArchiveRead;
    backend.material_mutate = &FakeMaterialMutate;
    backend.mesh_hot_swap = &FakeMeshHotSwap;
    backend.tracer_create = &FakeTracerCreate;
    backend.class_is_registered = &FakeClassRegistered;
    backend.class_create = &FakeClassCreate;
    backend.release = &FakeRelease;
    return backend;
}

WotbModDavaNativeToken CreateClass(
    wotbmod::v3::DavaNativeRegistry* registry,
    WotbModV3Handle owner,
    const char* class_name,
    uint32_t kind) {
    WotbModDavaNativeClassRequest request = {};
    request.struct_size = sizeof(request);
    request.object_kind = kind;
    strcpy_s(request.class_name, sizeof(request.class_name), class_name);
    WotbModDavaNativeToken token = 0u;
    CHECK(
        wotbmod::v3::DavaNativeClassCreate(
            registry, owner, &request, &token) == WOTBMOD_V3_OK);
    CHECK(token != 0u);
    return token;
}

void TestInstallGate() {
    FakeBackend fake;
    WotbModDavaNativeBackend backend = MakeBackend(&fake);
    wotbmod::v3::DavaNativeRegistry* registry = nullptr;

    backend.compatibility_state =
        WOTBMOD_V3_CLIENT_COMPATIBILITY_UNKNOWN;
    CHECK(
        wotbmod::v3::CreateDavaNativeRegistry(&backend, &registry) ==
        WOTBMOD_V3_E_CLIENT_MISMATCH);
    CHECK(registry == nullptr);

    backend = MakeBackend(&fake);
    backend.yaml_export_utf8 = nullptr;
    CHECK(
        wotbmod::v3::CreateDavaNativeRegistry(&backend, &registry) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(registry == nullptr);

    backend = MakeBackend(&fake);
    backend.capabilities = 0u;
    backend.yaml_parse_file = nullptr;
    backend.yaml_export_utf8 = nullptr;
    backend.archive_open_file = nullptr;
    backend.archive_get_entry_count = nullptr;
    backend.archive_get_entry = nullptr;
    backend.archive_read_entry = nullptr;
    backend.material_mutate = nullptr;
    backend.mesh_hot_swap = nullptr;
    backend.tracer_create = nullptr;
    backend.class_is_registered = nullptr;
    backend.class_create = nullptr;
    backend.release = nullptr;
    CHECK(
        wotbmod::v3::CreateDavaNativeRegistry(&backend, &registry) ==
        WOTBMOD_V3_OK);
    CHECK(wotbmod::v3::DavaNativeRegistryCapabilities(registry) == 0u);
    WotbModDavaNativeToken token = UINT64_MAX;
    CHECK(
        wotbmod::v3::DavaNativeYamlParseFile(
            registry, 1u, "C:\\safe.yaml", &token) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(token == 0u);
    wotbmod::v3::DestroyDavaNativeRegistry(registry);
}

void TestYamlArchiveAndOwnership() {
    FakeBackend fake;
    WotbModDavaNativeBackend backend = MakeBackend(&fake);
    wotbmod::v3::DavaNativeRegistry* registry = nullptr;
    CHECK(
        wotbmod::v3::CreateDavaNativeRegistry(&backend, &registry) ==
        WOTBMOD_V3_OK);
    const WotbModV3Handle owner_a = 11u;
    const WotbModV3Handle owner_b = 22u;

    WotbModDavaNativeToken yaml = 0u;
    CHECK(
        wotbmod::v3::DavaNativeYamlParseFile(
            registry, owner_a, "C:\\safe.yaml", &yaml) == WOTBMOD_V3_OK);
    CHECK(yaml != 0u);
    CHECK(yaml != 1000u);

    char yaml_bytes[32] = {};
    WotbModDavaNativeBuffer yaml_buffer = {};
    yaml_buffer.struct_size = sizeof(yaml_buffer);
    yaml_buffer.data = yaml_bytes;
    yaml_buffer.capacity = sizeof(yaml_bytes);
    CHECK(
        wotbmod::v3::DavaNativeYamlExportUtf8(
            registry, owner_b, yaml, &yaml_buffer) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        wotbmod::v3::DavaNativeYamlExportUtf8(
            registry, owner_a, yaml, &yaml_buffer) == WOTBMOD_V3_OK);
    CHECK(std::string(yaml_bytes, yaml_buffer.size) == "native: true\n");

    fake.overflow_buffer = true;
    CHECK(
        wotbmod::v3::DavaNativeYamlExportUtf8(
            registry, owner_a, yaml, &yaml_buffer) ==
        WOTBMOD_V3_E_PLATFORM);
    fake.overflow_buffer = false;

    WotbModDavaNativeToken archive = 0u;
    CHECK(
        wotbmod::v3::DavaNativeArchiveOpenFile(
            registry, owner_a, "C:\\safe.dvpk", &archive) ==
        WOTBMOD_V3_OK);
    uint32_t count = 0u;
    CHECK(
        wotbmod::v3::DavaNativeArchiveGetEntryCount(
            registry, owner_a, archive, &count) == WOTBMOD_V3_OK);
    CHECK(count == 2u);

    WotbModDavaNativeArchiveEntry entry = {};
    entry.struct_size = sizeof(entry);
    CHECK(
        wotbmod::v3::DavaNativeArchiveGetEntry(
            registry, owner_a, archive, 0u, &entry) == WOTBMOD_V3_OK);
    CHECK(std::strcmp(entry.relative_path, "a.txt") == 0);
    fake.malformed_entry = true;
    entry.struct_size = sizeof(entry);
    CHECK(
        wotbmod::v3::DavaNativeArchiveGetEntry(
            registry, owner_a, archive, 0u, &entry) ==
        WOTBMOD_V3_E_PLATFORM);
    fake.malformed_entry = false;

    uint8_t data[4] = {};
    WotbModDavaNativeBuffer data_buffer = {};
    data_buffer.struct_size = sizeof(data_buffer);
    data_buffer.data = data;
    data_buffer.capacity = sizeof(data);
    CHECK(
        wotbmod::v3::DavaNativeArchiveReadEntry(
            registry, owner_a, archive, 1u, &data_buffer) == WOTBMOD_V3_OK);
    CHECK(data_buffer.size == 4u && data[0] == 4u && data[3] == 7u);

    fake.fail_release = true;
    CHECK(
        wotbmod::v3::DavaNativeRelease(registry, owner_a, yaml) ==
        WOTBMOD_V3_E_BUSY);
    fake.fail_release = false;
    CHECK(
        wotbmod::v3::DavaNativeYamlExportUtf8(
            registry, owner_a, yaml, &yaml_buffer) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::DavaNativeRelease(registry, owner_b, yaml) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        wotbmod::v3::DavaNativeRelease(registry, owner_a, yaml) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::DavaNativeYamlExportUtf8(
            registry, owner_a, yaml, &yaml_buffer) ==
        WOTBMOD_V3_E_INVALID_HANDLE);

    CHECK(wotbmod::v3::DavaNativeReleaseOwner(registry, owner_b) == 0u);
    CHECK(wotbmod::v3::DavaNativeReleaseOwner(registry, owner_a) == 1u);
    wotbmod::v3::DestroyDavaNativeRegistry(registry);
    CHECK(fake.objects.empty());
}

void TestMaterialMeshTracerAndReviewedClasses() {
    FakeBackend fake;
    WotbModDavaNativeBackend backend = MakeBackend(&fake);
    wotbmod::v3::DavaNativeRegistry* registry = nullptr;
    CHECK(
        wotbmod::v3::CreateDavaNativeRegistry(&backend, &registry) ==
        WOTBMOD_V3_OK);
    const WotbModV3Handle owner = 31u;
    const WotbModV3Handle foreign = 32u;

    uint32_t registered = 0u;
    CHECK(
        wotbmod::v3::DavaNativeClassIsRegistered(
            registry, "DAVA::UIControl", &registered) == WOTBMOD_V3_OK);
    CHECK(registered == 1u);
    CHECK(
        wotbmod::v3::DavaNativeClassIsRegistered(
            registry, "DAVA::Unreviewed", &registered) == WOTBMOD_V3_OK);
    CHECK(registered == 0u);

    WotbModDavaNativeClassRequest denied = {};
    denied.struct_size = sizeof(denied);
    denied.object_kind = WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE;
    strcpy_s(
        denied.class_name,
        sizeof(denied.class_name),
        "DAVA::Unreviewed");
    WotbModDavaNativeToken denied_token = UINT64_MAX;
    CHECK(
        wotbmod::v3::DavaNativeClassCreate(
            registry, owner, &denied, &denied_token) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(denied_token == 0u);

    const WotbModDavaNativeToken material = CreateClass(
        registry,
        owner,
        "DAVA::NMaterial",
        WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL);
    const WotbModDavaNativeToken texture = CreateClass(
        registry,
        owner,
        "DAVA::Texture",
        WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE);
    const WotbModDavaNativeToken consumer = CreateClass(
        registry,
        owner,
        "DAVA::RenderComponent",
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER);
    const WotbModDavaNativeToken mesh = CreateClass(
        registry,
        owner,
        "DAVA::Mesh",
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH);

    WotbModDavaNativeMaterialMutation mutation = {};
    mutation.struct_size = sizeof(mutation);
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE;
    mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_TEXTURE;
    strcpy_s(mutation.name, sizeof(mutation.name), "albedo");
    mutation.related_object = texture;
    CHECK(
        wotbmod::v3::DavaNativeMaterialMutate(
            registry, foreign, material, &mutation) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        wotbmod::v3::DavaNativeMaterialMutate(
            registry, owner, material, &mutation) == WOTBMOD_V3_OK);
    CHECK(fake.last_material_related >= 1000u);
    CHECK(fake.last_material_related != texture);

    mutation = {};
    mutation.struct_size = sizeof(mutation);
    mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_SET_PROPERTY;
    mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT;
    mutation.array_size = 16u;
    strcpy_s(mutation.name, sizeof(mutation.name), "floatArray16");
    CHECK(
        wotbmod::v3::DavaNativeMaterialMutate(
            registry, owner, material, &mutation) == WOTBMOD_V3_OK);
    mutation.value_type = WOTBMOD_DAVA_NATIVE_VALUE_FLOAT4;
    mutation.array_size = 5u;
    CHECK(
        wotbmod::v3::DavaNativeMaterialMutate(
            registry, owner, material, &mutation) ==
        WOTBMOD_V3_E_INVALID_ARGUMENT);

    mutation = {};
    mutation.struct_size = sizeof(mutation);
    mutation.mutation_kind =
        WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER;
    mutation.related_object = texture;
    CHECK(
        wotbmod::v3::DavaNativeMaterialMutate(
            registry, owner, material, &mutation) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    mutation.related_object = consumer;
    CHECK(
        wotbmod::v3::DavaNativeMaterialMutate(
            registry, owner, material, &mutation) == WOTBMOD_V3_OK);
    CHECK(fake.last_material_related >= 1000u);
    CHECK(fake.last_material_related != consumer);

    CHECK(
        wotbmod::v3::DavaNativeMeshHotSwap(
            registry, foreign, consumer, mesh) ==
        WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(
        wotbmod::v3::DavaNativeMeshHotSwap(
            registry, owner, consumer, mesh) == WOTBMOD_V3_OK);
    CHECK(fake.last_mesh_consumer >= 1000u);
    CHECK(fake.last_mesh_replacement >= 1000u);

    WotbModDavaNativeTracerRequest tracer_request = {};
    tracer_request.struct_size = sizeof(tracer_request);
    tracer_request.destination[2] = 100.0f;
    tracer_request.color[0] = 1.0f;
    tracer_request.color[3] = 1.0f;
    tracer_request.width = 0.1f;
    tracer_request.lifetime_seconds = 0.5f;
    WotbModDavaNativeToken tracer = 0u;
    CHECK(
        wotbmod::v3::DavaNativeTracerCreate(
            registry, owner, &tracer_request, &tracer) == WOTBMOD_V3_OK);
    CHECK(tracer != 0u);

    CHECK(wotbmod::v3::DavaNativeReleaseOwner(registry, owner) == 5u);
    CHECK(fake.objects.empty());
    wotbmod::v3::DestroyDavaNativeRegistry(registry);
}

void TestReleaseWaitsForInFlightCall() {
    FakeBackend fake;
    WotbModDavaNativeBackend backend = MakeBackend(&fake);
    wotbmod::v3::DavaNativeRegistry* registry = nullptr;
    CHECK(
        wotbmod::v3::CreateDavaNativeRegistry(&backend, &registry) ==
        WOTBMOD_V3_OK);
    const WotbModV3Handle owner = 41u;
    WotbModDavaNativeToken yaml = 0u;
    CHECK(
        wotbmod::v3::DavaNativeYamlParseFile(
            registry, owner, "C:\\safe.yaml", &yaml) == WOTBMOD_V3_OK);

    fake.block_yaml_export = true;
    char bytes[32] = {};
    WotbModDavaNativeBuffer buffer = {};
    buffer.struct_size = sizeof(buffer);
    buffer.data = bytes;
    buffer.capacity = sizeof(bytes);
    std::atomic<WotbModV3Result> export_result{WOTBMOD_V3_E_BUSY};
    std::atomic<WotbModV3Result> release_result{WOTBMOD_V3_E_BUSY};

    std::thread exporter([&]() {
        export_result.store(
            wotbmod::v3::DavaNativeYamlExportUtf8(
                registry, owner, yaml, &buffer));
    });
    {
        std::unique_lock<std::mutex> lock(fake.mutex);
        CHECK(fake.entered.wait_for(
            lock,
            std::chrono::seconds(2),
            [&fake]() { return fake.yaml_export_entered; }));
    }
    std::thread releaser([&]() {
        release_result.store(
            wotbmod::v3::DavaNativeRelease(registry, owner, yaml));
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(release_result.load() == WOTBMOD_V3_E_BUSY);

    std::vector<WotbModDavaNativeToken> churn_tokens;
    churn_tokens.reserve(2048u);
    bool churn_created = true;
    for (uint32_t index = 0u; index < 2048u; ++index) {
        WotbModDavaNativeToken churn = 0u;
        if (wotbmod::v3::DavaNativeYamlParseFile(
                registry,
                owner,
                "C:\\rehash.yaml",
                &churn) != WOTBMOD_V3_OK ||
            churn == 0u) {
            churn_created = false;
            break;
        }
        churn_tokens.push_back(churn);
    }
    CHECK(churn_created);
    CHECK(churn_tokens.size() == 2048u);

    {
        std::lock_guard<std::mutex> lock(fake.mutex);
        fake.allow_yaml_export = true;
        fake.resume.notify_all();
    }
    exporter.join();
    releaser.join();
    CHECK(export_result.load() == WOTBMOD_V3_OK);
    CHECK(release_result.load() == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::DavaNativeReleaseOwner(registry, owner) ==
        churn_tokens.size());
    CHECK(fake.objects.empty());
    wotbmod::v3::DestroyDavaNativeRegistry(registry);
}

void TestInstalledRegistryLifecycle() {
    wotbmod::v3::RemoveDavaNativeBackend();
    WotbModDavaNativeToken token = UINT64_MAX;
    CHECK(
        wotbmod::v3::InstalledDavaNativeYamlParseFile(
            51u, "C:\\safe.yaml", &token) ==
        WOTBMOD_V3_E_NOT_SUPPORTED);
    CHECK(token == 0u);

    FakeBackend first;
    WotbModDavaNativeBackend first_backend = MakeBackend(&first);
    CHECK(
        wotbmod::v3::InstallDavaNativeBackend(&first_backend) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::InstalledDavaNativeCapabilities() ==
        first_backend.capabilities);
    CHECK(
        wotbmod::v3::InstalledDavaNativeYamlParseFile(
            51u, "C:\\safe.yaml", &token) == WOTBMOD_V3_OK);
    CHECK(token != 0u && token != 1000u);

    FakeBackend second;
    WotbModDavaNativeBackend second_backend = MakeBackend(&second);
    CHECK(
        wotbmod::v3::InstallDavaNativeBackend(&second_backend) ==
        WOTBMOD_V3_OK);
    CHECK(first.release_count == 1u);
    CHECK(first.objects.empty());

    char bytes[32] = {};
    WotbModDavaNativeBuffer buffer = {};
    buffer.struct_size = sizeof(buffer);
    buffer.data = bytes;
    buffer.capacity = sizeof(bytes);
    CHECK(
        wotbmod::v3::InstalledDavaNativeYamlExportUtf8(
            51u, token, &buffer) == WOTBMOD_V3_E_INVALID_HANDLE);

    WotbModDavaNativeToken replacement_token = 0u;
    CHECK(
        wotbmod::v3::InstalledDavaNativeYamlParseFile(
            52u, "C:\\other.yaml", &replacement_token) ==
        WOTBMOD_V3_OK);
    CHECK(replacement_token != token);
    CHECK(
        wotbmod::v3::InstalledDavaNativeYamlExportUtf8(
            51u, token, &buffer) == WOTBMOD_V3_E_INVALID_HANDLE);
    CHECK(wotbmod::v3::InstalledDavaNativeReleaseOwner(52u) == 1u);
    CHECK(second.release_count == 1u);
    CHECK(second.objects.empty());

    wotbmod::v3::RemoveDavaNativeBackend();
    CHECK(wotbmod::v3::InstalledDavaNativeCapabilities() == 0u);
    CHECK(
        wotbmod::v3::InstalledDavaNativeRelease(
            52u, replacement_token) == WOTBMOD_V3_E_NOT_SUPPORTED);
}

}  // namespace

int main() {
    TestInstallGate();
    TestYamlArchiveAndOwnership();
    TestMaterialMeshTracerAndReviewedClasses();
    TestReleaseWaitsForInFlightCall();
    TestInstalledRegistryLifecycle();
    std::printf(
        "V3 DAVA NATIVE REGISTRY: %d checks, %d failures\n",
        g_checks,
        g_failures);
    return g_failures == 0 ? 0 : 1;
}
