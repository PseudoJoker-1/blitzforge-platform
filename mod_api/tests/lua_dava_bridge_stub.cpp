#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../include/wotb_mod_dava_native.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

struct ObjectRecord {
    WotbModV3Handle owner = WOTBMOD_V3_INVALID_HANDLE;
    WotbModDavaNativeToken token = 0u;
    uint32_t kind = 0u;
    bool live = false;
};

constexpr size_t kMaxObjects = 128u;
ObjectRecord g_objects[kMaxObjects] = {};
WotbModDavaNativeToken g_nextToken = 1000u;
uint32_t g_createCount = 0u;
uint32_t g_mutationCount = 0u;
uint32_t g_swapCount = 0u;
uint32_t g_tracerCount = 0u;
uint32_t g_releaseCount = 0u;

ObjectRecord* FindObject(WotbModV3Handle owner,
                         WotbModDavaNativeToken token,
                         uint32_t kind) noexcept {
    for (ObjectRecord& object : g_objects) {
        if (object.live && object.owner == owner && object.token == token &&
            object.kind == kind) {
            return &object;
        }
    }
    return nullptr;
}

WotbModV3Result CreateObject(WotbModV3Handle owner,
                             uint32_t kind,
                             WotbModDavaNativeToken* outToken) noexcept {
    if (!outToken || owner == WOTBMOD_V3_INVALID_HANDLE || kind == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outToken = 0u;
    for (ObjectRecord& object : g_objects) {
        if (object.live) continue;
        object.owner = owner;
        object.token = g_nextToken++;
        object.kind = kind;
        object.live = true;
        *outToken = object.token;
        ++g_createCount;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_BUSY;
}

uint32_t ReviewedKind(const char* name) noexcept {
    if (!name) return 0u;
    if (std::strcmp(name, "DAVA::NMaterial") == 0) {
        return WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL;
    }
    if (std::strcmp(name, "DAVA::Texture") == 0) {
        return WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE;
    }
    if (std::strcmp(name, "DAVA::Mesh") == 0) {
        return WOTBMOD_DAVA_NATIVE_OBJECT_MESH;
    }
    if (std::strcmp(name, "DAVA::MeshConsumer") == 0) {
        return WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER;
    }
    if (std::strcmp(name, "DAVA::UIControl") == 0 ||
        std::strcmp(name, "DAVA::Entity") == 0) {
        return WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE;
    }
    return 0u;
}

bool HasTerminatedPayload(const WotbModDavaNativeClassRequest* request) {
    return request && request->payload && request->payload_size > 0u &&
           static_cast<const char*>(request->payload)[request->payload_size - 1u] == '\0';
}

}  // namespace

extern "C" __declspec(dllexport) uint64_t WOTBMOD_V3_CALL
WotbModLoader_DavaNativeCapabilities() {
    return WOTBMOD_DAVA_NATIVE_CAP_YAML |
           WOTBMOD_DAVA_NATIVE_CAP_RESOURCE_ARCHIVE |
           WOTBMOD_DAVA_NATIVE_CAP_NMATERIAL |
           WOTBMOD_DAVA_NATIVE_CAP_MESH_HOT_SWAP |
           WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER |
           WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY;
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_DavaNativeClassIsRegistered(const char* className,
                                           uint32_t* outRegistered) {
    if (!className || !outRegistered) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *outRegistered = ReviewedKind(className) != 0u ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_DavaNativeClassCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeToken* outObject) {
    if (!request || request->struct_size < sizeof(*request) || !outObject) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const uint32_t reviewedKind = ReviewedKind(request->class_name);
    if (reviewedKind == 0u) return WOTBMOD_V3_E_NOT_SUPPORTED;
    if (reviewedKind != request->object_kind || !HasTerminatedPayload(request)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    return CreateObject(owner, reviewedKind, outObject);
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_DavaNativeMaterialMutate(
    WotbModV3Handle owner,
    WotbModDavaNativeToken material,
    const WotbModDavaNativeMaterialMutation* mutation) {
    if (!mutation || mutation->struct_size < sizeof(*mutation) ||
        !FindObject(owner, material, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (mutation->mutation_kind ==
        WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE) {
        if (!FindObject(owner, mutation->related_object,
                        WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE)) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
    } else if (mutation->mutation_kind ==
               WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER) {
        if (!FindObject(owner, mutation->related_object,
                        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER)) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
    }
    ++g_mutationCount;
    return WOTBMOD_V3_OK;
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_DavaNativeMeshHotSwap(
    WotbModV3Handle owner,
    WotbModDavaNativeToken consumer,
    WotbModDavaNativeToken replacementMesh) {
    if (!FindObject(owner, consumer,
                    WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER) ||
        !FindObject(owner, replacementMesh,
                    WOTBMOD_DAVA_NATIVE_OBJECT_MESH)) {
        return WOTBMOD_V3_E_INVALID_HANDLE;
    }
    ++g_swapCount;
    return WOTBMOD_V3_OK;
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_DavaNativeTracerCreate(
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeToken* outTracer) {
    if (!request || request->struct_size < sizeof(*request) ||
        request->shell_type > 24u || !outTracer) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (float value : request->origin) {
        if (!std::isfinite(value)) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    for (float value : request->destination) {
        if (!std::isfinite(value)) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    const WotbModV3Result result = CreateObject(
        owner, WOTBMOD_DAVA_NATIVE_OBJECT_TRACER, outTracer);
    if (result == WOTBMOD_V3_OK) ++g_tracerCount;
    return result;
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_DavaNativeRelease(WotbModV3Handle owner,
                                WotbModDavaNativeToken token) {
    for (ObjectRecord& object : g_objects) {
        if (!object.live || object.owner != owner || object.token != token) {
            continue;
        }
        object.live = false;
        ++g_releaseCount;
        return WOTBMOD_V3_OK;
    }
    return WOTBMOD_V3_E_INVALID_HANDLE;
}

extern "C" __declspec(dllexport) WotbModV3Result WOTBMOD_V3_CALL
WotbModLoader_DavaNativeReleaseAsync(WotbModV3Handle owner,
                                     WotbModDavaNativeToken token) {
    return WotbModLoader_DavaNativeRelease(owner, token);
}

extern "C" __declspec(dllexport) void WOTBMOD_V3_CALL
WotbModLoader_DavaTestReset() {
    std::memset(g_objects, 0, sizeof(g_objects));
    g_nextToken = 1000u;
    g_createCount = 0u;
    g_mutationCount = 0u;
    g_swapCount = 0u;
    g_tracerCount = 0u;
    g_releaseCount = 0u;
}

extern "C" __declspec(dllexport) uint32_t WOTBMOD_V3_CALL
WotbModLoader_DavaTestLiveCount() {
    uint32_t live = 0u;
    for (const ObjectRecord& object : g_objects) {
        if (object.live) ++live;
    }
    return live;
}

extern "C" __declspec(dllexport) uint32_t WOTBMOD_V3_CALL
WotbModLoader_DavaTestCreateCount() { return g_createCount; }
extern "C" __declspec(dllexport) uint32_t WOTBMOD_V3_CALL
WotbModLoader_DavaTestMutationCount() { return g_mutationCount; }
extern "C" __declspec(dllexport) uint32_t WOTBMOD_V3_CALL
WotbModLoader_DavaTestSwapCount() { return g_swapCount; }
extern "C" __declspec(dllexport) uint32_t WOTBMOD_V3_CALL
WotbModLoader_DavaTestTracerCount() { return g_tracerCount; }
extern "C" __declspec(dllexport) uint32_t WOTBMOD_V3_CALL
WotbModLoader_DavaTestReleaseCount() { return g_releaseCount; }
