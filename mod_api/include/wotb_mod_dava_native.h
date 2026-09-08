#pragma once

#include <stdint.h>

#include "wotbmod/base.h"
#include "wotbmod/client_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Loader/runtime-private DAVA object ABI.
 *
 * This table is deliberately separate from the frozen public V3 ABI. Mods and
 * Lua never receive provider_token values or engine pointers. The portable
 * runtime wraps every provider token in an owner-checked host token before it
 * can be used by a public interface.
 */

#define WOTBMOD_DAVA_NATIVE_BACKEND_VERSION 1u
#define WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME 128u

typedef uint64_t WotbModDavaNativeProviderToken;
typedef uint64_t WotbModDavaNativeToken;

typedef enum WotbModDavaNativeObjectKind {
    WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT = 1,
    WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE = 2,
    WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL = 3,
    WOTBMOD_DAVA_NATIVE_OBJECT_MESH = 4,
    WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER = 5,
    WOTBMOD_DAVA_NATIVE_OBJECT_TRACER = 6,
    WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE = 7,
    WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE = 8
} WotbModDavaNativeObjectKind;

typedef enum WotbModDavaNativeCapability {
    WOTBMOD_DAVA_NATIVE_CAP_YAML = UINT64_C(1) << 0,
    WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE = UINT64_C(1) << 1,
    WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL = UINT64_C(1) << 2,
    WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP = UINT64_C(1) << 3,
    WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER = UINT64_C(1) << 4,
    WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY = UINT64_C(1) << 5
} WotbModDavaNativeCapability;

typedef enum WotbModDavaNativeValueType {
    WOTBMOD_DAVA_NATIVE_VALUE_FLOAT = 1,
    WOTBMOD_DAVA_NATIVE_VALUE_FLOAT2 = 2,
    WOTBMOD_DAVA_NATIVE_VALUE_FLOAT3 = 3,
    WOTBMOD_DAVA_NATIVE_VALUE_FLOAT4 = 4,
    WOTBMOD_DAVA_NATIVE_VALUE_INT = 5,
    WOTBMOD_DAVA_NATIVE_VALUE_BOOL = 6,
    WOTBMOD_DAVA_NATIVE_VALUE_TEXTURE = 7,
    WOTBMOD_DAVA_NATIVE_VALUE_FAST_NAME = 8
} WotbModDavaNativeValueType;

typedef enum WotbModDavaNativeMaterialMutationKind {
    WOTBMOD_DAVA_NATIVE_MATERIAL_SET_PROPERTY = 1,
    WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_PROPERTY = 2,
    WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE = 3,
    WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_TEXTURE = 4,
    WOTBMOD_DAVA_NATIVE_MATERIAL_SET_FLAG = 5,
    WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_FLAG = 6,
    WOTBMOD_DAVA_NATIVE_MATERIAL_SET_FX = 7,
    WOTBMOD_DAVA_NATIVE_MATERIAL_SET_QUALITY = 8,
    WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER = 9,
    /*
     * QUERIES, appended. They mutate nothing and answer through the result:
     * OK means the name is on the material, E_NOT_FOUND means it is not.
     *
     * They exist so that taking a camouflage off can stop being destructive.
     * Without them a rollback removes every name it set, including ones the
     * GAME had already set - there was no way to tell the two apart, which is
     * what "presence-only restore" (ruling R11) meant. With them, apply records
     * which names pre-existed and rollback leaves exactly those alone.
     */
    WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_PROPERTY = 10,
    WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_TEXTURE = 11,
    WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_FLAG = 12
} WotbModDavaNativeMaterialMutationKind;

typedef struct WotbModDavaNativeBuffer {
    uint32_t struct_size;
    void* data;
    uint32_t capacity;
    uint32_t size;
} WotbModDavaNativeBuffer;

typedef struct WotbModDavaNativeArchiveEntry {
    uint32_t struct_size;
    uint32_t index;
    uint64_t original_size;
    uint64_t compressed_size;
    uint32_t original_crc32;
    uint32_t compressed_crc32;
    uint32_t compression_type;
    uint32_t flags;
    char relative_path[WOTBMOD_V3_MAX_PATH];
} WotbModDavaNativeArchiveEntry;

typedef struct WotbModDavaNativeMaterialMutation {
    uint32_t struct_size;
    uint32_t mutation_kind;
    uint32_t value_type;
    uint32_t array_size;
    char name[WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME];
    float values[16];
    int32_t int_value;
    uint32_t bool_value;
    /* Host token on registry ingress; rewritten to a provider token before
     * material_mutate is called. A texture token is required by SET_TEXTURE;
     * a mesh-consumer token is required by APPLY_TO_MESH_CONSUMER; otherwise
     * this field is zero. */
    WotbModDavaNativeToken related_object;
} WotbModDavaNativeMaterialMutation;

typedef struct WotbModDavaNativeTracerRequest {
    uint32_t struct_size;
    uint32_t shell_type;
    uint32_t flags;
    uint32_t reserved;
    float origin[3];
    float destination[3];
    float color[4];
    float width;
    float lifetime_seconds;
} WotbModDavaNativeTracerRequest;

typedef struct WotbModDavaNativeClassRequest {
    uint32_t struct_size;
    uint32_t object_kind;
    uint32_t flags;
    uint32_t payload_size;
    char class_name[WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME];
    const void* payload;
} WotbModDavaNativeClassRequest;

/*
 * The class callbacks are a provider-reviewed factory registry, not a raw
 * DAVA::ObjectFactory pass-through. A provider must return registered=1 only
 * for classes whose constructor, destructor, size, thread, ownership and
 * object-kind contract were validated for its exact client fingerprint.
 */

/*
 * The MATERIAL OF A NODE IN THE LIVE SCENE.
 *
 * This is what makes painting a built-in camouflage possible at all. Everything
 * else needed was already published - the material mutators, the entity ->
 * RenderObject -> RenderBatch walk, the active scene - and the one missing link
 * was reaching a material that the GAME owns rather than one the mod created.
 *
 * `node_name` is a DAVA::Entity name. Empty means "the first node under the
 * active scene that has a material at all", which is what a caller uses to
 * discover the shape of a hangar before it knows any names.
 *
 * The token that comes back names a RETAINED material: the provider adds a
 * reference before handing it out, because releasing this token must not
 * decrement a count the mod never incremented. It is released like any other
 * object token.
 */
typedef struct WotbModDavaNativeSceneMaterialRequest {
    uint32_t struct_size;
    uint32_t api_version;
    /* Which render batch of the matched node. 0 is the first. */
    uint32_t batch_index;
    uint32_t reserved;
    char node_name[WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME];
} WotbModDavaNativeSceneMaterialRequest;

typedef WotbModV3Result (*WotbModDavaNativeSceneMaterialFn)(
    void* user_data,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeProviderToken* out_provider_token);

/*
 * Every node name under the active scene, newline separated, for discovery.
 * A mod that does not yet know what the hangar calls the tank asks this once.
 */
typedef WotbModV3Result (*WotbModDavaNativeSceneNodesFn)(
    void* user_data,
    char* buffer,
    uint32_t* inout_size);

typedef WotbModV3Result (*WotbModDavaNativeCreatePathObjectFn)(
    void* user_data,
    WotbModV3Handle owner,
    const char* resolved_file_path,
    WotbModDavaNativeProviderToken* out_provider_token);

typedef WotbModV3Result (*WotbModDavaNativeYamlExportFn)(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken document,
    WotbModDavaNativeBuffer* inout_utf8_yaml);

typedef WotbModV3Result (*WotbModDavaNativeArchiveCountFn)(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t* out_count);

typedef WotbModV3Result (*WotbModDavaNativeArchiveEntryFn)(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* out_entry);

typedef WotbModV3Result (*WotbModDavaNativeArchiveReadFn)(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t index,
    WotbModDavaNativeBuffer* inout_data);

typedef WotbModV3Result (*WotbModDavaNativeMaterialMutateFn)(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken material,
    const WotbModDavaNativeMaterialMutation* mutation);

typedef WotbModV3Result (*WotbModDavaNativeMeshHotSwapFn)(
    void* user_data,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken consumer,
    WotbModDavaNativeProviderToken replacement_mesh);

typedef WotbModV3Result (*WotbModDavaNativeTracerCreateFn)(
    void* user_data,
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeProviderToken* out_provider_token);

typedef WotbModV3Result (*WotbModDavaNativeClassRegisteredFn)(
    void* user_data,
    const char* class_name,
    uint32_t* out_registered);

typedef WotbModV3Result (*WotbModDavaNativeClassCreateFn)(
    void* user_data,
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeProviderToken* out_provider_token);

typedef WotbModV3Result (*WotbModDavaNativeReleaseFn)(
    void* user_data,
    WotbModV3Handle owner,
    uint32_t object_kind,
    WotbModDavaNativeProviderToken provider_token);

typedef struct WotbModDavaNativeBackend {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t binding_pack_version;
    uint32_t compatibility_state;
    uint64_t capabilities;
    void* user_data;

    WotbModDavaNativeCreatePathObjectFn yaml_parse_file;
    WotbModDavaNativeYamlExportFn yaml_export_utf8;

    WotbModDavaNativeCreatePathObjectFn archive_open_file;
    WotbModDavaNativeArchiveCountFn archive_get_entry_count;
    WotbModDavaNativeArchiveEntryFn archive_get_entry;
    WotbModDavaNativeArchiveReadFn archive_read_entry;

    WotbModDavaNativeMaterialMutateFn material_mutate;
    WotbModDavaNativeMeshHotSwapFn mesh_hot_swap;
    WotbModDavaNativeTracerCreateFn tracer_create;

    WotbModDavaNativeClassRegisteredFn class_is_registered;
    WotbModDavaNativeClassCreateFn class_create;

    WotbModDavaNativeReleaseFn release;

    /*
     * APPENDED, and that is deliberate: every field above keeps its offset, so
     * a provider built against the older struct still binds - `struct_size`
     * says how far this one goes and a shorter one simply does not carry these.
     */
    WotbModDavaNativeSceneMaterialFn scene_material;
    WotbModDavaNativeSceneNodesFn scene_nodes;
    /*
     * The RENDER BATCH of a node in the live scene - the mesh-consumer half of
     * a live geometry swap. Same request shape as scene_material (node name +
     * batch index); the token comes back under OBJECT_MESH_CONSUMER with the
     * batch in both object slots, so `mesh_hot_swap` consumes it unchanged.
     * Optional like scene_material: a provider built before this slot existed
     * simply does not carry it.
     */
    WotbModDavaNativeSceneMaterialFn scene_batch;
} WotbModDavaNativeBackend;

#ifdef __cplusplus
} /* extern "C" */
#endif
