#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "../include/wotb_mod_dava_resources.h"
#include "../loader/anchor_rvas.h"

namespace {

const uint32_t kContextMagic = 0x52564144u;  /* DAVR */
const uint32_t kResourceMagic = 0x43525344u; /* DSRC */
const uint32_t kNativeRecordMagic = 0x54414E44u; /* DNAT */

const uint32_t kDefaultUiControlSystemOffset = 0x0000003Cu;

const size_t kUiControlSize = 0x140u;
const size_t kEntitySize = 0x50u;
const size_t kNMaterialSize = 0xA8u;
const size_t kUiPackageLoaderSize = 0x88u;
const size_t kUiPackageBuilderSize = 0xA0u;
const size_t kUiPackageErrorOffset = 0x08u;
const size_t kUiPackageControlsBeginOffset = 0x24u;
const size_t kUiPackageControlsEndOffset = 0x28u;
const size_t kUiPackageBuilderPackageOffset = 0x5Cu;
const size_t kMaxObjectName = 256u;
const size_t kEntityTransformComponentOffset = 0x3Cu;
const size_t kEntityChildrenBeginOffset = 0x08u;
const size_t kEntityChildrenEndOffset = 0x0Cu;
const size_t kRenderObjectBatchesBeginOffset = 0x3Cu;
const size_t kRenderObjectBatchesEndOffset = 0x40u;
const size_t kRenderBatchPolygonGroupOffset = 0x40u;
/*
 * RenderBatch's material pointer. The anchor comment for
 * kDefaultRenderBatchSetMaterialRva already states it: that function "retains
 * the replacement NMaterial, swaps the pointer at +0x44 and releases the
 * previous material". Reading the same slot is the inverse of the write this
 * build was already anchored on.
 */
const size_t kRenderBatchMaterialOffset = 0x44u;
/*
 * DAVA::Entity's name, a FastName VALUE (the interned char*), not an object.
 *
 * DERIVED FROM THE BINARY, not guessed, and corroborated four ways on
 * 11.19.0.834 (sha256 41960dbd...2e0ad):
 *
 *   1. Entity's real constructor at 0x008CE250 takes the FastName by reference,
 *      dereferences it and stores the dword: `mov eax,[ebp+8]` /
 *      `mov eax,[eax]` / `mov [esi+0x1C],eax`.
 *   2. Entity vtable slot 10 (0x00916230) is SetName(const char*): it calls
 *      0x0050DE50 - which is kDefaultFastNameCtorRva, ALREADY ANCHORED - and
 *      stores the result's dword at +0x1C.
 *   3. Entity vtable slot 11 (0x00916220) is the FastName overload of the same
 *      setter, writing the same field.
 *   4. The fields around it match anchors this loader already uses live:
 *      the constructor zeroes +0x08/+0x0C/+0x10 (the children vector,
 *      kEntityChildrenBeginOffset/EndOffset) and +0x3C
 *      (kEntityTransformComponentOffset).
 *
 * The value is compared by POINTER EQUALITY against a FastName built with
 * MakeFastName, exactly as FindUiChildByFastName already does for UIControl's
 * name at +0x30. Two FastNames of the same text share one interned pointer.
 */
const size_t kEntityNameOffset = 0x1Cu;
const uint32_t kMaxMeshTraversalDepth = 128u;
const uint32_t kMaxMeshChildrenPerEntity = 65536u;
const uint32_t kMaxRenderBatchesPerObject = 65536u;
/* A hangar scene is a few hundred nodes; a battle scene is larger but
 * still nowhere near this. The cap exists so a cyclic or corrupted graph
 * costs a bounded walk rather than the process. */
const uint32_t kMaxSceneNodesVisited = 65536u;
/* The DUMP's own, much smaller cap. The walk may cross a whole scene;
 * a listing a human reads out of a log must not. Two thousand lines is
 * about eighty kilobytes and is far past any hangar. */
const uint32_t kMaxSceneNodesDumped = 2000u;
const size_t kSceneActiveOffset = 0x100u;
const uint32_t kMaxNativeYamlBytes = 8u * 1024u * 1024u;
const uint32_t kMaxNativeArchiveEntries = 100000u;
const size_t kDavaStringInlineCapacity = 16u;
const size_t kDavaArchiveFileInfoSize = 44u;
const size_t kDavaArchiveLookupPathCapacity = 512u;

const size_t kUiNameOffset = 0x30u;
const size_t kUiParentOffset = 0x38u;
const size_t kUiChildrenOffset = 0x44u;
const size_t kUiFlagsOffset = 0x4Cu;
const size_t kUiDirtyFlagsOffset = 0x4Du;
const size_t kUiPositionOffset = 0x6Cu;
const size_t kUiSizeOffset = 0x74u;
const size_t kUiInputProcessorsCountOffset = 0x94u;
const size_t kUiControlStateOffset = 0xA0u;
const uint8_t kUiVisibleBit = 0x02u;
/*
 * DAVA::UITextComponent on 11.20.0.887 (re_anchors.md): object 0xE0 bytes,
 * std::string text at +0x44 (MSVC layout: buffer/pointer at +0, size at
 * +0x10, capacity at +0x14); the UIComponent base keeps its owning UIControl*
 * at +0x08. UIControl is 0x140 bytes.
 */
const size_t kUiTextComponentTextOffset = 0x44u;
const size_t kUiDynamicAtlasTextComponentTextOffset = 0x60u;
const size_t kUiComponentControlOffset = 0x08u;
const size_t kUiControlObjectSize = 0x140u;
const size_t kUiComponentsMaxCount = 32u;
const uint32_t kUiLiveTextMaxLength = 0x10000u;
const uint8_t kUiHierarchyDirtyBit = 0x08u;
const uint32_t kUiDisabledStateBit = 1u << 3;

const size_t kUiSetPositionVtableIndex = 4u;
const size_t kUiSetSizeVtableIndex = 5u;
const size_t kUiSetInputEnabledVtableIndex = 6u;
const size_t kUiSetDisabledVtableIndex = 7u;
const size_t kUiAddControlVtableIndex = 11u;
const size_t kUiRemoveControlVtableIndex = 12u;
/* MoveControlToFront: unlink an existing child and insert it before the
 * intrusive-list sentinel (the tail drawn above earlier siblings). */
const size_t kUiBringToFrontVtableIndex = 13u;
const size_t kEntityAddNodeVtableIndex = 5u;
const size_t kEntityRemoveNodeVtableIndex = 6u;
const size_t kSceneDrawVtableIndex = 21u;

struct DavaFilePath32 {
    std::string absolute_pathname;
    int32_t path_type;

    explicit DavaFilePath32(const char* path)
        : absolute_pathname(path ? path : ""), path_type(0) {
    }
};

struct DavaRect32 {
    float x;
    float y;
    float width;
    float height;
};

struct DavaVector2 {
    float x;
    float y;
};

struct DavaTransform32 {
    float translation[3];
    float scale[3];
    float rotation[4];
};

struct DavaYamlParserResult32 {
    uint8_t parsed;
    uint8_t reserved[3];
    void* root;
};

struct DavaRawVector32 {
    uint8_t* begin;
    uint8_t* end;
    uint8_t* capacity;
};

/* The exact Blitz 11.19 ResourceArchive implementation does not receive the
 * std::string stored in FileInfo. Its lookup ABI is an inline 512-byte path
 * followed by the byte count at +0x200. Both PackArchive and ZipArchive read
 * this shape directly before hashing/comparing the path. */
struct DavaArchiveLookupPath32 {
    char data[kDavaArchiveLookupPathCapacity];
    uint32_t size;

    explicit DavaArchiveLookupPath32(const char* path)
        : data{}, size(0u) {
        if (!path) return;
        const size_t length = strlen(path);
        if (length >= kDavaArchiveLookupPathCapacity) return;
        memcpy(data, path, length);
        size = static_cast<uint32_t>(length);
    }

    bool IsValidFor(const char* path) const {
        return path && (path[0] == '\0' || size != 0u);
    }
};

struct DavaStringStorage32 {
    union {
        char inline_data[kDavaStringInlineCapacity];
        const char* heap_data;
    } storage;
    uint32_t size;
    uint32_t capacity;
};

struct DavaArchiveFileInfo32 {
    DavaStringStorage32 relative_path;
    uint32_t original_size;
    uint32_t original_crc32;
    uint32_t compressed_size;
    uint32_t compressed_crc32;
    uint32_t compression_type;
};

#if defined(_M_IX86)
static_assert(sizeof(std::string) == 24u, "DAVA String ABI changed");
static_assert(sizeof(DavaFilePath32) == 28u, "DAVA FilePath ABI changed");
static_assert(sizeof(DavaRect32) == 16u, "DAVA Rect ABI changed");
static_assert(sizeof(DavaTransform32) == 40u, "DAVA Transform ABI changed");
static_assert(sizeof(DavaYamlParserResult32) == 8u,
              "DAVA YamlParser result ABI changed");
static_assert(sizeof(DavaRawVector32) == 12u,
              "DAVA Vector ABI changed");
static_assert(sizeof(DavaArchiveLookupPath32) == 516u,
              "DAVA ResourceArchive lookup path ABI changed");
static_assert(sizeof(DavaStringStorage32) == sizeof(std::string),
              "DAVA String storage ABI changed");
static_assert(sizeof(DavaArchiveFileInfo32) == kDavaArchiveFileInfoSize,
              "DAVA ResourceArchive::FileInfo ABI changed");
#endif

struct DavaResourcesContext {
    uint32_t magic;
    HMODULE game_module;
    uint8_t* image_base;
    uint32_t image_size;
    char data_directory[MAX_PATH];
    uint32_t ref_counted_retain_rva;
    uint32_t ref_counted_release_rva;
    uint32_t fast_name_ctor_rva;
    uint32_t yaml_parse_file_wrapper_rva;
    uint32_t resource_archive_ctor_rva;
    uint32_t resource_archive_dtor_rva;
    uint32_t pack_archive_vtable_rva;
    uint32_t zip_archive_vtable_rva;
    uint32_t nmaterial_ctor_rva;
    uint32_t nmaterial_vtable_rva;
    uint32_t nmaterial_set_fx_rva;
    uint32_t nmaterial_set_quality_rva;
    uint32_t nmaterial_has_property_rva;
    uint32_t nmaterial_add_property_rva;
    uint32_t nmaterial_set_property_rva;
    uint32_t nmaterial_remove_property_rva;
    uint32_t nmaterial_has_flag_rva;
    uint32_t nmaterial_set_flag_rva;
    uint32_t nmaterial_remove_flag_rva;
    uint32_t nmaterial_has_texture_rva;
    uint32_t nmaterial_set_texture_rva;
    uint32_t nmaterial_remove_texture_rva;
    uint32_t texture_create_from_file_rva;
    uint32_t entity_get_render_object_rva;
    uint32_t render_object_get_render_batch_rva;
    uint32_t render_batch_vtable_rva;
    uint32_t render_batch_set_material_rva;
    uint32_t render_batch_set_polygon_group_rva;
    WotbModDavaStockTracerCreateFn stock_tracer_create;
    void* stock_tracer_user_data;
    WotbModDavaIsMainThreadFn is_main_thread;
    void* main_thread_user_data;
    WotbModDavaInvokeMainThreadFn invoke_main_thread;
    void* invoke_main_thread_user_data;
    uint32_t operator_new_rva;
    uint32_t operator_delete_rva;
    uint32_t ui_control_ctor_rva;
    uint32_t ui_text_component_vtable_rva;
    uint32_t ui_dynamic_atlas_text_component_vtable_rva;
    uint32_t entity_ctor_rva;
    uint32_t get_engine_context_rva;
    uint32_t ui_control_system_offset;
    uint32_t ui_control_system_get_screen_rva;
    uint32_t ui_package_loader_ctor_rva;
    uint32_t ui_package_loader_dtor_rva;
    uint32_t ui_load_package_rva;
    uint32_t ui_package_builder_ctor_rva;
    uint32_t ui_package_builder_dtor_rva;
    uint32_t ui_extract_control_rva;
    uint32_t scene_ctor_rva;
    uint32_t scene_load_from_file_rva;
    uint32_t scene_load_entity_rva;
    uint32_t ui_package_loader_vtable_rva;
    uint32_t ui_package_vtable_rva;
    uint32_t ui_control_vtable_rva;
    uint32_t entity_vtable_rva;
    uint32_t scene_vtable_rva;
    uint32_t scene_draw_rva;
    uint32_t scene_activate_rva;
    uint32_t scene_deactivate_rva;
    uint32_t transform_set_local_transform_rva;
    WotbModRuntimeLogSink log_sink;
    void* log_user_data;
    volatile LONG active_resources;
    PVOID volatile active_scene;
};

enum DavaResourceProvenance {
    DAVA_RESOURCE_FILE = 1,
    DAVA_RESOURCE_CREATED = 2,
    DAVA_RESOURCE_ACQUIRED = 3
};

struct DavaResource {
    uint32_t magic;
    DavaResourcesContext* context;
    WotbModResourceType type;
    uint32_t flags;
    DavaResourceProvenance provenance;
    void* native_object;
    uint64_t file_size;
    char virtual_path[WOTBMOD_MAX_RESOURCE_PATH];
    char resolved_path[WOTBMOD_MAX_RESOURCE_PATH];
    char object_name[kMaxObjectName];
};

struct DavaNativeRecord {
    uint32_t magic;
    DavaResourcesContext* context;
    WotbModV3Handle owner;
    uint32_t kind;
    void* native_object;
    void* related_object;
    uint8_t* bytes;
    uint32_t byte_count;
};

typedef void(__thiscall* RefCountedFn)(void*);
typedef void*(__cdecl* OperatorNewFn)(size_t);
typedef void(__cdecl* OperatorDeleteFn)(void*, size_t);
typedef void*(__thiscall* NativeCtorFn)(void*);
typedef void*(__cdecl* GetEngineContextFn)();
typedef void*(__cdecl* EntityGetRenderObjectFn)(const void*);
typedef void*(__thiscall* RenderObjectGetRenderBatchFn)(void*, uint32_t);
typedef void(__thiscall* RenderBatchSetMaterialFn)(void*, void*);
typedef void(__thiscall* RenderBatchSetPolygonGroupFn)(void*, void*);
typedef void*(__thiscall* UiGetScreenFn)(void*);
typedef void(__thiscall* UiSetVisibilityFlagFn)(void*, bool);
typedef void*(__thiscall* UiPackageLoaderCtorFn)(void*, bool);
typedef void(__thiscall* ObjectDtorFn)(void*);
typedef bool(__thiscall* UiLoadPackageFn)(
    void*, const DavaFilePath32*, void*);
typedef void*(__thiscall* UiPackageBuilderCtorFn)(void*, void**);
typedef void*(__thiscall* UiExtractControlFn)(
    void*, void**, const std::string*);
typedef void**(__stdcall* SceneLoadEntityFn)(
    void**, const DavaFilePath32*);
typedef void*(__thiscall* FastNameCtorFn)(void*, const char*);
typedef void(__thiscall* UiSetVectorFn)(void*, const DavaVector2*);
typedef void(__thiscall* UiSetFlagFn)(void*, bool, bool);
typedef void(__thiscall* ObjectPairFn)(void*, void*);
typedef void(__thiscall* TransformSetLocalTransformFn)(
    void*, const DavaTransform32*);
typedef void*(__cdecl* YamlParseFileWrapperFn)(
    DavaYamlParserResult32*, const DavaFilePath32*, uint32_t);
typedef void*(__thiscall* ResourceArchiveCtorFn)(
    void*, const DavaFilePath32*);
typedef void*(__thiscall* ArchiveGetFilesInfoFn)(void*);
typedef bool(__thiscall* ArchiveLoadFileFn)(
    void*, const DavaArchiveLookupPath32*, DavaRawVector32*);
struct FastNameScope;
typedef void*(__thiscall* NMaterialCtorFn)(void*, const FastNameScope*);
typedef void(__thiscall* MaterialRefNameFn)(
    void*, const FastNameScope*);
typedef bool(__thiscall* MaterialHasRefNameFn)(
    void*, const FastNameScope*);
typedef void(__thiscall* MaterialAddPropertyFn)(
    void*,
    const FastNameScope*,
    const float*,
    uint32_t,
    uint32_t,
    uint32_t);
typedef void(__thiscall* MaterialSetPropertyFn)(
    void*, const FastNameScope*, const float*);
typedef void(__thiscall* MaterialSetFlagFn)(
    void*, const FastNameScope*, int32_t);
typedef void(__thiscall* MaterialSetTextureFn)(
    void*, const FastNameScope*, void*, bool);
typedef void*(__cdecl* TextureCreateFromFileFn)(
    const DavaFilePath32*, const FastNameScope*);

static uint32_t SelectRva(uint32_t supplied, uint32_t fallback) {
    return supplied ? supplied : fallback;
}

static uint32_t SelectOptionRva(
    const WotbModDavaResourcesOptions* options,
    size_t fieldOffset,
    uint32_t fallback) {
    if (!options ||
        options->struct_size < fieldOffset + sizeof(uint32_t)) {
        return fallback;
    }
    const uint32_t supplied = *reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(options) + fieldOffset);
    return SelectRva(supplied, fallback);
}

static bool ReadImageSize(HMODULE module, uint32_t* outSize) {
    if (!module || !outSize) return false;
    __try {
        const uint8_t* base = reinterpret_cast<const uint8_t*>(module);
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
            return false;
        }
        const IMAGE_NT_HEADERS32* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS32*>(
                base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386) {
            return false;
        }
        *outSize = nt->OptionalHeader.SizeOfImage;
        return *outSize != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool RvaInImage(
    const DavaResourcesContext* context,
    uint32_t rva,
    size_t size) {
    if (!context || !context->image_base || rva >= context->image_size) {
        return false;
    }
    return size <= context->image_size - rva;
}

static void* Address(
    const DavaResourcesContext* context,
    uint32_t rva,
    size_t size = 1u) {
    return RvaInImage(context, rva, size)
               ? context->image_base + rva
               : nullptr;
}

/* 2026-09-05: VirtualQuery costs roughly 0.5-1 ms per call inside the live
 * client (the address-space lock is contended by the engine's streaming
 * threads), and every wrapped or walked UI control paid at least one -
 * a whole-tree walk of the 2500-control hangar took about 2 s and the
 * per-frame active-screen fetch 3-5 ms. Addresses inside the game image are
 * now answered from the PE section table without a syscall; heap addresses
 * still go to the OS. */
struct ImageCodeRange {
    uintptr_t start;
    uintptr_t end;
};
static ImageCodeRange g_imageCodeRanges[16] = {};
static size_t g_imageCodeRangeCount = 0u;
static uintptr_t g_imageStart = 0u;
static uintptr_t g_imageEnd = 0u;

static void RecordImageCodeRanges(HMODULE module, uint32_t imageSize) {
    g_imageCodeRangeCount = 0u;
    g_imageStart = reinterpret_cast<uintptr_t>(module);
    g_imageEnd = g_imageStart + imageSize;
    __try {
        const IMAGE_DOS_HEADER* dos =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(
                reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections &&
                         g_imageCodeRangeCount < 16u; ++i, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0u) {
                continue;
            }
            const uintptr_t start = g_imageStart + section->VirtualAddress;
            const uintptr_t size = section->Misc.VirtualSize
                ? section->Misc.VirtualSize : section->SizeOfRawData;
            if (start < g_imageStart || size > g_imageEnd - start) continue;
            g_imageCodeRanges[g_imageCodeRangeCount].start = start;
            g_imageCodeRanges[g_imageCodeRangeCount].end = start + size;
            ++g_imageCodeRangeCount;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_imageCodeRangeCount = 0u;
    }
}

static bool IsImageCodeAddress(const void* address) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    for (size_t i = 0u; i < g_imageCodeRangeCount; ++i) {
        if (value >= g_imageCodeRanges[i].start &&
            value < g_imageCodeRanges[i].end) {
            return true;
        }
    }
    return false;
}

static bool IsExecutableAddress(const void* address) {
    if (!address) return false;
    if (IsImageCodeAddress(address)) return true;
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

static bool IsReadableRange(const void* address, size_t size) {
    if (!address || size == 0u) return false;
    {
        /* A mapped PE image is committed end to end. */
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        /* 2026-09-08: `start < g_imageEnd` was missing, so any address above
         * the image start underflowed the size test and passed as readable -
         * the live-text walk then dereferenced heap garbage (32 first-chance
         * AVs per sweep, with_text=1 instead of ~170 in the hangar). */
        if (g_imageEnd != 0u && start >= g_imageStart && start < g_imageEnd &&
            size <= g_imageEnd - start) {
            return true;
        }
    }
    MEMORY_BASIC_INFORMATION info = {};
    if (!VirtualQuery(address, &info, sizeof(info)) ||
        info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t regionStart =
        reinterpret_cast<uintptr_t>(info.BaseAddress);
    const uintptr_t regionEnd = regionStart + info.RegionSize;
    return start >= regionStart && start <= regionEnd &&
           size <= regionEnd - start;
}

static DavaResourcesContext* ValidContext(void* userData) {
    DavaResourcesContext* context =
        static_cast<DavaResourcesContext*>(userData);
    return context && context->magic == kContextMagic ? context : nullptr;
}

static DavaResource* ValidResource(
    DavaResourcesContext* context,
    void* nativeResource) {
    DavaResource* resource =
        static_cast<DavaResource*>(nativeResource);
    return resource &&
                   resource->magic == kResourceMagic &&
                   resource->context == context
               ? resource
               : nullptr;
}

static bool IsDavaMainThread(DavaResourcesContext* context) {
    if (!context || !context->is_main_thread) return false;
    __try {
        return context->is_main_thread(
                   context->main_thread_user_data) != 0u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename Callable>
static WotbModV3Result WOTBMOD_V3_CALL DavaMainThreadThunk(
    void* callData) {
    return (*static_cast<Callable*>(callData))();
}

template <typename Callable>
static WotbModV3Result InvokeDavaMainThread(
    DavaResourcesContext* context,
    Callable& call) {
    if (IsDavaMainThread(context)) return call();
    if (!context || !context->invoke_main_thread) {
        return WOTBMOD_V3_E_WRONG_THREAD;
    }
    return context->invoke_main_thread(
        context->invoke_main_thread_user_data,
        &DavaMainThreadThunk<Callable>,
        &call);
}

static DavaNativeRecord* ValidNativeRecord(
    DavaResourcesContext* context,
    WotbModV3Handle owner,
    uint32_t expectedKind,
    WotbModDavaNativeProviderToken providerToken) {
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE ||
        providerToken == 0u) {
        return nullptr;
    }
    DavaNativeRecord* record = reinterpret_cast<DavaNativeRecord*>(
        static_cast<uintptr_t>(providerToken));
    __try {
        return record->magic == kNativeRecordMagic &&
                       record->context == context &&
                       record->owner == owner &&
                       record->kind == expectedKind
                   ? record
                   : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static void Log(
    DavaResourcesContext* context,
    WotbModLogLevel level,
    const char* message) {
    if (context && context->log_sink) {
        context->log_sink(level, message ? message : "", context->log_user_data);
    }
}

static void LogResource(
    DavaResourcesContext* context,
    WotbModLogLevel level,
    const char* operation,
    WotbModResourceType type,
    const char* path,
    const char* objectName) {
    char line[1400] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "DAVA resource %s type=%d path=%s%s%s",
        operation ? operation : "operation",
        static_cast<int>(type),
        path ? path : "",
        objectName && objectName[0] ? " object=" : "",
        objectName && objectName[0] ? objectName : "");
    Log(context, level, line);
}

static bool ReadFileSize(const char* path, uint64_t* outSize) {
    if (!path || !outSize) return false;
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExA(
            path,
            GetFileExInfoStandard,
            &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    *outSize =
        (static_cast<uint64_t>(data.nFileSizeHigh) << 32) |
        data.nFileSizeLow;
    return true;
}

static WotbModV3Result ReadNativeFileBytes(
    const char* path,
    uint32_t maxBytes,
    uint8_t** outBytes,
    uint32_t* outSize) {
    if (!path || !path[0] || !outBytes || !outSize || maxBytes == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outBytes = nullptr;
    *outSize = 0u;
    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ||
                       error == ERROR_PATH_NOT_FOUND
                   ? WOTBMOD_V3_E_NOT_FOUND
                   : WOTBMOD_V3_E_IO;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
        CloseHandle(file);
        return size.QuadPart == 0 ? WOTBMOD_V3_E_PARSE : WOTBMOD_V3_E_IO;
    }
    if (static_cast<uint64_t>(size.QuadPart) > maxBytes) {
        CloseHandle(file);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    const uint32_t byteCount = static_cast<uint32_t>(size.QuadPart);
    uint8_t* bytes = static_cast<uint8_t*>(HeapAlloc(
        GetProcessHeap(), 0, byteCount));
    if (!bytes) {
        CloseHandle(file);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    DWORD read = 0u;
    const BOOL readOk = ReadFile(file, bytes, byteCount, &read, nullptr);
    const BOOL closeOk = CloseHandle(file);
    if (!readOk || read != byteCount || !closeOk) {
        HeapFree(GetProcessHeap(), 0, bytes);
        return WOTBMOD_V3_E_IO;
    }
    *outBytes = bytes;
    *outSize = byteCount;
    return WOTBMOD_V3_OK;
}

static WotbModV3Result CopyNativeBuffer(
    const void* source,
    uint32_t size,
    WotbModDavaNativeBuffer* buffer) {
    if (!buffer || buffer->struct_size < sizeof(*buffer) ||
        (size != 0u && !source)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    buffer->size = size;
    if (!buffer->data || buffer->capacity < size) {
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    if (size != 0u) memcpy(buffer->data, source, size);
    return WOTBMOD_V3_OK;
}

static WotbModResult ResolveMountedFile(
    const char* virtualPath,
    char* resolvedPath,
    uint32_t capacity,
    uint64_t* outSize) {
    if (!virtualPath || !resolvedPath || capacity == 0 || !outSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    uint32_t size = capacity;
    const WotbModResult result = WotbModRuntime_ResolveResourcePath(
        virtualPath,
        resolvedPath,
        &size);
    if (result != WOTBMOD_OK) return result;
    return ReadFileSize(resolvedPath, outSize)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_NOT_FOUND;
}

static void InitializeDavaDataDirectory(DavaResourcesContext* context) {
    if (!context) return;
    context->data_directory[0] = '\0';
    const DWORD length = GetModuleFileNameA(
        context->game_module,
        context->data_directory,
        static_cast<DWORD>(sizeof(context->data_directory)));
    if (length == 0u || length >= sizeof(context->data_directory)) {
        context->data_directory[0] = '\0';
        return;
    }
    char* separator = strrchr(context->data_directory, '\\');
    char* slash = strrchr(context->data_directory, '/');
    if (!separator || (slash && slash > separator)) separator = slash;
    if (!separator) {
        context->data_directory[0] = '\0';
        return;
    }
    separator[1] = '\0';
    if (strcat_s(
            context->data_directory,
            sizeof(context->data_directory),
            "Data/") != 0) {
        context->data_directory[0] = '\0';
        return;
    }
    for (char* current = context->data_directory; *current; ++current) {
        if (*current == '\\') *current = '/';
    }
}

static void BuildDavaPath(
    const DavaResourcesContext* context,
    const char* resolvedPath,
    char* output,
    size_t capacity) {
    if (!output || capacity == 0) return;
    output[0] = '\0';
    if (!resolvedPath) return;
    strcpy_s(output, capacity, resolvedPath);
    size_t length = strlen(output);
    if (length >= 5u &&
        _stricmp(output + length - 5u, ".dvpl") == 0) {
        output[length - 5u] = '\0';
    }
    for (char* current = output; *current; ++current) {
        if (*current == '\\') *current = '/';
    }
    if (!context || !context->data_directory[0]) return;
    const size_t dataDirectoryLength = strlen(context->data_directory);
    if (_strnicmp(
            output,
            context->data_directory,
            dataDirectoryLength) != 0) {
        return;
    }
    const char* relative = output + dataDirectoryLength;
    const size_t relativeLength = strlen(relative);
    static const char kResourcePrefix[] = "~res:/";
    if (sizeof(kResourcePrefix) + relativeLength > capacity) {
        output[0] = '\0';
        return;
    }
    memmove(
        output + sizeof(kResourcePrefix) - 1u,
        relative,
        relativeLength + 1u);
    memcpy(output, kResourcePrefix, sizeof(kResourcePrefix) - 1u);
}

static bool InvokeRetain(
    DavaResourcesContext* context,
    void* object) {
    RefCountedFn retain = reinterpret_cast<RefCountedFn>(Address(
        context, context->ref_counted_retain_rva));
    if (!object || !retain ||
        !IsExecutableAddress(reinterpret_cast<const void*>(retain))) {
        return false;
    }
    __try {
        retain(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeRelease(
    DavaResourcesContext* context,
    void* object) {
    RefCountedFn release = reinterpret_cast<RefCountedFn>(Address(
        context, context->ref_counted_release_rva));
    if (!object || !release ||
        !IsExecutableAddress(reinterpret_cast<const void*>(release))) {
        return false;
    }
    __try {
        release(object);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ValidateObject(
    DavaResourcesContext* context,
    void* object,
    uint32_t exactVtableRva) {
    if (!context || !object) return false;
    __try {
        void** vtable = *reinterpret_cast<void***>(object);
        if (!vtable) return false;
        if (exactVtableRva != 0 &&
            vtable != Address(context, exactVtableRva, sizeof(void*))) {
            return false;
        }
        const uintptr_t vtableAddress =
            reinterpret_cast<uintptr_t>(vtable);
        const uintptr_t imageStart =
            reinterpret_cast<uintptr_t>(context->image_base);
        const uintptr_t imageEnd = imageStart + context->image_size;
        return vtableAddress >= imageStart &&
               vtableAddress + sizeof(void*) <= imageEnd &&
               IsExecutableAddress(vtable[0]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool HasVirtualTarget(
    DavaResourcesContext* context,
    void* object,
    size_t index,
    uint32_t targetRva) {
    if (!ValidateObject(context, object, 0)) return false;
    __try {
        void** vtable = *reinterpret_cast<void***>(object);
        return vtable[index] == Address(context, targetRva);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool IsSceneActive(void* scene) {
    if (!scene) return false;
    __try {
        return *reinterpret_cast<const uint8_t*>(
                   static_cast<const uint8_t*>(scene) +
                   kSceneActiveOffset) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename FunctionType>
static FunctionType VirtualFunction(
    void* object,
    size_t index) {
    if (!object) return nullptr;
    __try {
        void** vtable = *reinterpret_cast<void***>(object);
        void* target = vtable ? vtable[index] : nullptr;
        return IsExecutableAddress(target)
                   ? reinterpret_cast<FunctionType>(target)
                   : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static DavaResource* TypedObjectResource(
    DavaResourcesContext* context,
    void* nativeResource,
    WotbModResourceType expectedType) {
    DavaResource* resource = ValidResource(context, nativeResource);
    if (!resource ||
        resource->type != expectedType ||
        !resource->native_object ||
        !ValidateObject(context, resource->native_object, 0)) {
        return nullptr;
    }
    return resource;
}

static DavaResource* AllocateResource(
    DavaResourcesContext* context,
    WotbModResourceType type,
    DavaResourceProvenance provenance,
    void* nativeObject) {
    if (!context || !nativeObject) return nullptr;
    DavaResource* resource = static_cast<DavaResource*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(DavaResource)));
    if (!resource) return nullptr;
    resource->magic = kResourceMagic;
    resource->context = context;
    resource->type = type;
    resource->provenance = provenance;
    resource->native_object = nativeObject;
    InterlockedIncrement(&context->active_resources);
    return resource;
}

static DavaNativeRecord* AllocateNativeRecord(
    DavaResourcesContext* context,
    WotbModV3Handle owner,
    uint32_t kind) {
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE || kind == 0u) {
        return nullptr;
    }
    DavaNativeRecord* record = static_cast<DavaNativeRecord*>(HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DavaNativeRecord)));
    if (!record) return nullptr;
    record->context = context;
    record->owner = owner;
    record->kind = kind;
    record->magic = kNativeRecordMagic;
    InterlockedIncrement(&context->active_resources);
    return record;
}

static void FreeNativeRecord(DavaNativeRecord* record) {
    if (!record) return;
    DavaResourcesContext* context = record->context;
    record->magic = 0u;
    record->context = nullptr;
    if (record->bytes) {
        HeapFree(GetProcessHeap(), 0, record->bytes);
        record->bytes = nullptr;
    }
    record->byte_count = 0u;
    HeapFree(GetProcessHeap(), 0, record);
    if (context) InterlockedDecrement(&context->active_resources);
}

struct FastNameScope {
    void* value;
};

static bool MakeFastName(
    DavaResourcesContext* context,
    const char* name,
    FastNameScope* outName) {
    if (!context || !name || !name[0] || !outName) return false;
    outName->value = nullptr;
    FastNameCtorFn constructor =
        reinterpret_cast<FastNameCtorFn>(Address(
            context, context->fast_name_ctor_rva));
    if (!constructor ||
        !IsExecutableAddress(reinterpret_cast<const void*>(constructor))) {
        return false;
    }
    __try {
        constructor(outName, name);
        return outName->value != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outName->value = nullptr;
        return false;
    }
}

static void ReleaseFastName(
    DavaResourcesContext* context,
    FastNameScope* name) {
    (void)context;
    if (!name) return;
    /* FastName stores a stable char pointer owned by the process-wide intern
     * database. It is not a RefCounted object. Calling Retain/Release on this
     * pointer writes into the interned string and corrupts future lookups. */
    name->value = nullptr;
}

static bool InvokeOperatorNew(
    DavaResourcesContext* context,
    size_t size,
    void** outMemory) {
    OperatorNewFn operatorNew =
        context ? reinterpret_cast<OperatorNewFn>(Address(
                      context, context->operator_new_rva))
                : nullptr;
    if (!outMemory || !operatorNew ||
        !IsExecutableAddress(reinterpret_cast<const void*>(operatorNew))) {
        return false;
    }
    *outMemory = nullptr;
    __try {
        *outMemory = operatorNew(size);
        return *outMemory != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outMemory = nullptr;
        return false;
    }
}

static bool InvokeNMaterialCtor(
    DavaResourcesContext* context,
    void* memory,
    const FastNameScope* name,
    void** outObject) {
    NMaterialCtorFn constructor =
        context ? reinterpret_cast<NMaterialCtorFn>(Address(
                      context, context->nmaterial_ctor_rva))
                : nullptr;
    if (!memory || !name || !outObject || !constructor ||
        !IsExecutableAddress(reinterpret_cast<const void*>(constructor))) {
        return false;
    }
    *outObject = nullptr;
    __try {
        *outObject = constructor(memory, name);
        return *outObject == memory;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outObject = nullptr;
        return false;
    }
}

static bool InvokeMaterialHasRefName(
    DavaResourcesContext* context,
    uint32_t rva,
    void* material,
    const FastNameScope* name,
    bool* outPresent) {
    MaterialHasRefNameFn has =
        context ? reinterpret_cast<MaterialHasRefNameFn>(
                      Address(context, rva))
                : nullptr;
    if (!material || !name || !outPresent || !has ||
        !IsExecutableAddress(reinterpret_cast<const void*>(has))) {
        return false;
    }
    __try {
        *outPresent = has(material, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outPresent = false;
        return false;
    }
}

static bool InvokeMaterialRefName(
    DavaResourcesContext* context,
    uint32_t rva,
    void* material,
    const FastNameScope* name) {
    MaterialRefNameFn mutate =
        context ? reinterpret_cast<MaterialRefNameFn>(Address(context, rva))
                : nullptr;
    if (!material || !name || !mutate ||
        !IsExecutableAddress(reinterpret_cast<const void*>(mutate))) {
        return false;
    }
    __try {
        mutate(material, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeMaterialAddProperty(
    DavaResourcesContext* context,
    void* material,
    const FastNameScope* name,
    const float* values,
    uint32_t shaderType,
    uint32_t arraySize) {
    MaterialAddPropertyFn add =
        context ? reinterpret_cast<MaterialAddPropertyFn>(Address(
                      context, context->nmaterial_add_property_rva))
                : nullptr;
    if (!material || !name || !values || !add ||
        !IsExecutableAddress(reinterpret_cast<const void*>(add))) {
        return false;
    }
    __try {
        /* The fifth argument exists in the 2026-08 binding pack and is zero at
         * every reviewed game call site. Omitting it makes the callee execute
         * `ret 14h` against a 16-byte call frame and corrupts the stack. */
        add(material, name, values, shaderType, arraySize, 0u);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeMaterialSetProperty(
    DavaResourcesContext* context,
    void* material,
    const FastNameScope* name,
    const float* values) {
    MaterialSetPropertyFn set =
        context ? reinterpret_cast<MaterialSetPropertyFn>(Address(
                      context, context->nmaterial_set_property_rva))
                : nullptr;
    if (!material || !name || !name->value || !values || !set ||
        !IsExecutableAddress(reinterpret_cast<const void*>(set))) {
        return false;
    }
    __try {
        set(material, name, values);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeMaterialSetFlag(
    DavaResourcesContext* context,
    void* material,
    const FastNameScope* name,
    int32_t value) {
    MaterialSetFlagFn set =
        context ? reinterpret_cast<MaterialSetFlagFn>(Address(
                      context, context->nmaterial_set_flag_rva))
                : nullptr;
    if (!material || !name || !set ||
        !IsExecutableAddress(reinterpret_cast<const void*>(set))) {
        return false;
    }
    __try {
        set(material, name, value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeMaterialSetTexture(
    DavaResourcesContext* context,
    void* material,
    const FastNameScope* name,
    void* texture) {
    MaterialSetTextureFn set =
        context ? reinterpret_cast<MaterialSetTextureFn>(Address(
                      context, context->nmaterial_set_texture_rva))
                : nullptr;
    if (!material || !name || !texture || !set ||
        !IsExecutableAddress(reinterpret_cast<const void*>(set))) {
        return false;
    }
    __try {
        set(material, name, texture, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeTextureCreateFromFile(
    DavaResourcesContext* context,
    const DavaFilePath32* path,
    const FastNameScope* group,
    void** outTexture) {
    TextureCreateFromFileFn create =
        context ? reinterpret_cast<TextureCreateFromFileFn>(Address(
                      context, context->texture_create_from_file_rva))
                : nullptr;
    if (!path || !group || !outTexture || !create ||
        !IsExecutableAddress(reinterpret_cast<const void*>(create))) {
        return false;
    }
    *outTexture = nullptr;
    __try {
        *outTexture = create(path, group);
        return *outTexture != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outTexture = nullptr;
        return false;
    }
}

static bool CopyNativeBytes(
    void* destination,
    const void* source,
    size_t size) {
    if (!destination || (!source && size != 0u)) return false;
    __try {
        if (size != 0u) memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static WotbModResult WrapRetainedUiControl(
    DavaResourcesContext* context,
    void* control,
    void** outNativeResource) {
    if (!context || !control || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    if (!ValidateObject(context, control, 0) ||
        !InvokeRetain(context, control)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    DavaResource* resource = AllocateResource(
        context,
        WOTBMOD_RESOURCE_UI_CONTROL,
        DAVA_RESOURCE_ACQUIRED,
        control);
    if (!resource) {
        InvokeRelease(context, control);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    *outNativeResource = resource;
    return WOTBMOD_OK;
}

static void* FindUiChildByFastName(
    DavaResourcesContext* context,
    void* root,
    void* fastNameValue,
    bool recursive,
    uint32_t depth,
    uint32_t* visited) {
    if (!context || !root || !fastNameValue || !visited ||
        depth > 128u || *visited >= 16384u) {
        return nullptr;
    }
    __try {
        void* sentinel = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(root) + kUiChildrenOffset);
        if (!sentinel) return nullptr;
        void* node = *reinterpret_cast<void**>(sentinel);
        while (node && node != sentinel && *visited < 16384u) {
            ++*visited;
            void* child = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(node) + 8u);
            if (child && ValidateObject(context, child, 0)) {
                void* childName = *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(child) + kUiNameOffset);
                if (childName == fastNameValue) return child;
                if (recursive) {
                    void* nested = FindUiChildByFastName(
                        context,
                        child,
                        fastNameValue,
                        true,
                        depth + 1u,
                        visited);
                    if (nested) return nested;
                }
            }
            void* next = *reinterpret_cast<void**>(node);
            if (next == node) return nullptr;
            node = next;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

static bool UiControlHasFastName(
    DavaResourcesContext* context,
    void* control,
    void* fastNameValue) {
    if (!context || !control || !fastNameValue ||
        !ValidateObject(context, control, 0)) {
        return false;
    }
    __try {
        return *reinterpret_cast<void**>(
                   static_cast<uint8_t*>(control) + kUiNameOffset) ==
               fastNameValue;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeOperatorDelete(
    DavaResourcesContext* context,
    void* memory,
    size_t size) {
    OperatorDeleteFn operatorDelete =
        context ? reinterpret_cast<OperatorDeleteFn>(Address(
                      context, context->operator_delete_rva))
                : nullptr;
    if (!memory || !operatorDelete ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(operatorDelete))) {
        return false;
    }
    __try {
        operatorDelete(memory, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ValidateArchiveImpl(
    DavaResourcesContext* context,
    void* impl) {
    if (!context || !impl) return false;
    __try {
        void** vtable = *reinterpret_cast<void***>(impl);
        return (vtable == Address(
                    context,
                    context->pack_archive_vtable_rva,
                    sizeof(void*)) ||
                vtable == Address(
                    context,
                    context->zip_archive_vtable_rva,
                    sizeof(void*))) &&
               IsExecutableAddress(vtable[0]) &&
               IsExecutableAddress(vtable[1]) &&
               IsExecutableAddress(vtable[4]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/* SEH helpers stay in frames that own no C++ objects. MSVC rejects __try in
 * functions that also require C++ unwinding, while the exact DAVA entrypoints
 * may still fault on a stale fingerprint or malformed native asset. */
static bool InvokeYamlParseFile(
    DavaResourcesContext* context,
    const DavaFilePath32* path,
    DavaYamlParserResult32* outResult) {
    YamlParseFileWrapperFn parse =
        context ? reinterpret_cast<YamlParseFileWrapperFn>(Address(
                      context, context->yaml_parse_file_wrapper_rva))
                : nullptr;
    if (!path || !outResult || !parse ||
        !IsExecutableAddress(reinterpret_cast<const void*>(parse))) {
        return false;
    }
    __try {
        return parse(outResult, path, 1u) == outResult;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outResult->parsed = 0u;
        outResult->root = nullptr;
        return false;
    }
}

static bool InvokeArchiveCtor(
    DavaResourcesContext* context,
    void** archiveStorage,
    const DavaFilePath32* path) {
    ResourceArchiveCtorFn constructor =
        context ? reinterpret_cast<ResourceArchiveCtorFn>(Address(
                      context, context->resource_archive_ctor_rva))
                : nullptr;
    if (!archiveStorage || !path || !constructor ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(constructor))) {
        return false;
    }
    __try {
        return constructor(archiveStorage, path) == archiveStorage &&
               ValidateArchiveImpl(context, *archiveStorage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *archiveStorage = nullptr;
        return false;
    }
}

static bool InvokeArchiveDtor(
    DavaResourcesContext* context,
    void** archiveStorage) {
    ObjectDtorFn destructor =
        context ? reinterpret_cast<ObjectDtorFn>(Address(
                      context, context->resource_archive_dtor_rva))
                : nullptr;
    if (!archiveStorage || !destructor ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(destructor))) {
        return false;
    }
    __try {
        destructor(archiveStorage);
        *archiveStorage = nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool GetArchiveFilesInfo(
    DavaResourcesContext* context,
    void* impl,
    DavaRawVector32* outVector) {
    if (!outVector || !ValidateArchiveImpl(context, impl)) return false;
    __try {
        void** vtable = *reinterpret_cast<void***>(impl);
        ArchiveGetFilesInfoFn getFiles =
            reinterpret_cast<ArchiveGetFilesInfoFn>(vtable[1]);
        DavaRawVector32* files = static_cast<DavaRawVector32*>(
            getFiles(impl));
        if (!files) return false;
        *outVector = *files;
        const uintptr_t begin = reinterpret_cast<uintptr_t>(outVector->begin);
        const uintptr_t end = reinterpret_cast<uintptr_t>(outVector->end);
        const uintptr_t capacity =
            reinterpret_cast<uintptr_t>(outVector->capacity);
        if (begin == 0u) {
            return end == 0u && capacity == 0u;
        }
        if (end < begin || capacity < end) return false;
        const uintptr_t byteCount = end - begin;
        return byteCount % kDavaArchiveFileInfoSize == 0u &&
               byteCount / kDavaArchiveFileInfoSize <=
                   kMaxNativeArchiveEntries;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ZeroMemory(outVector, sizeof(*outVector));
        return false;
    }
}

static bool GetArchiveFileInfo(
    DavaResourcesContext* context,
    void* impl,
    uint32_t index,
    DavaArchiveFileInfo32* outInfo) {
    if (!outInfo) return false;
    DavaRawVector32 files = {};
    if (!GetArchiveFilesInfo(context, impl, &files)) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(files.begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(files.end);
    const uintptr_t count = begin == 0u
        ? 0u : (end - begin) / kDavaArchiveFileInfoSize;
    if (index >= count) return false;
    __try {
        *outInfo = *reinterpret_cast<const DavaArchiveFileInfo32*>(
            files.begin + index * kDavaArchiveFileInfoSize);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ZeroMemory(outInfo, sizeof(*outInfo));
        return false;
    }
}

static bool CopyDavaString(
    const DavaStringStorage32* value,
    char* output,
    size_t capacity) {
    if (!value || !output || capacity == 0u) return false;
    output[0] = '\0';
    __try {
        if (value->size >= capacity || value->capacity < value->size) {
            return false;
        }
        const char* data = value->capacity < kDavaStringInlineCapacity
            ? value->storage.inline_data
            : value->storage.heap_data;
        if (!data && value->size != 0u) return false;
        if (value->size != 0u) memcpy(output, data, value->size);
        output[value->size] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        output[0] = '\0';
        return false;
    }
}

static bool InvokeArchiveLoadFile(
    DavaResourcesContext* context,
    void* impl,
    const DavaArchiveLookupPath32* relativePath,
    DavaRawVector32* outBytes) {
    if (!relativePath || !outBytes ||
        !ValidateArchiveImpl(context, impl)) {
        return false;
    }
    __try {
        void** vtable = *reinterpret_cast<void***>(impl);
        ArchiveLoadFileFn load =
            reinterpret_cast<ArchiveLoadFileFn>(vtable[4]);
        return load(impl, relativePath, outBytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool FreeGameByteVector(
    DavaResourcesContext* context,
    DavaRawVector32* vector) {
    if (!vector) return false;
    uint8_t* begin = vector->begin;
    uint8_t* capacity = vector->capacity;
    vector->begin = nullptr;
    vector->end = nullptr;
    vector->capacity = nullptr;
    if (!begin) return capacity == nullptr;

    void* allocation = begin;
    size_t allocationSize = 0u;
    __try {
        const uintptr_t beginAddress = reinterpret_cast<uintptr_t>(begin);
        const uintptr_t capacityAddress =
            reinterpret_cast<uintptr_t>(capacity);
        if (!capacity || capacityAddress < beginAddress) return false;
        allocationSize = capacityAddress - beginAddress;
        if (allocationSize >= 0x1000u) {
            void* raw = *(reinterpret_cast<void**>(begin) - 1);
            const uintptr_t rawAddress = reinterpret_cast<uintptr_t>(raw);
            if (!raw || rawAddress >= beginAddress ||
                beginAddress - rawAddress < sizeof(void*) ||
                beginAddress - rawAddress > 0x23u ||
                allocationSize > SIZE_MAX - 0x23u) {
                return false;
            }
            allocation = raw;
            allocationSize += 0x23u;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return InvokeOperatorDelete(context, allocation, allocationSize);
}

static WotbModResult CreateNativeObject(
    DavaResourcesContext* context,
    size_t objectSize,
    uint32_t constructorRva,
    uint32_t vtableRva,
    void** outObject) {
    if (!context || !outObject) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outObject = nullptr;
    OperatorNewFn operatorNew = reinterpret_cast<OperatorNewFn>(Address(
        context, context->operator_new_rva));
    NativeCtorFn constructor = reinterpret_cast<NativeCtorFn>(Address(
        context, constructorRva));
    if (!operatorNew || !constructor ||
        !IsExecutableAddress(reinterpret_cast<const void*>(operatorNew)) ||
        !IsExecutableAddress(reinterpret_cast<const void*>(constructor))) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    void* memory = nullptr;
    __try {
        memory = operatorNew(objectSize);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (!memory) return WOTBMOD_ERROR_LIMIT_REACHED;

    void* object = nullptr;
    __try {
        object = constructor(memory);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InvokeOperatorDelete(context, memory, objectSize);
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (object != memory ||
        !ValidateObject(context, object, vtableRva)) {
        InvokeOperatorDelete(context, memory, objectSize);
        return WOTBMOD_ERROR_PLATFORM;
    }
    *outObject = object;
    return WOTBMOD_OK;
}

static bool InvokeBuilderCtor(
    DavaResourcesContext* context,
    void* builder,
    void** emptyCacheRef) {
    UiPackageBuilderCtorFn constructor =
        reinterpret_cast<UiPackageBuilderCtorFn>(Address(
            context, context->ui_package_builder_ctor_rva));
    if (!constructor ||
        !IsExecutableAddress(reinterpret_cast<const void*>(constructor))) {
        return false;
    }
    __try {
        return constructor(builder, emptyCacheRef) == builder;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeBuilderDtor(
    DavaResourcesContext* context,
    void* builder) {
    ObjectDtorFn destructor = reinterpret_cast<ObjectDtorFn>(Address(
        context, context->ui_package_builder_dtor_rva));
    if (!destructor ||
        !IsExecutableAddress(reinterpret_cast<const void*>(destructor))) {
        return false;
    }
    __try {
        destructor(builder);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeLoaderCtor(
    DavaResourcesContext* context,
    void* loader) {
    UiPackageLoaderCtorFn constructor =
        reinterpret_cast<UiPackageLoaderCtorFn>(Address(
            context, context->ui_package_loader_ctor_rva));
    if (!constructor ||
        !IsExecutableAddress(reinterpret_cast<const void*>(constructor))) {
        return false;
    }
    __try {
        if (constructor(loader, true) != loader) return false;
        void** vtable = *reinterpret_cast<void***>(loader);
        return vtable == Address(
            context,
            context->ui_package_loader_vtable_rva,
            sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeLoaderDtor(
    DavaResourcesContext* context,
    void* loader) {
    ObjectDtorFn destructor = reinterpret_cast<ObjectDtorFn>(Address(
        context, context->ui_package_loader_dtor_rva));
    if (!destructor ||
        !IsExecutableAddress(reinterpret_cast<const void*>(destructor))) {
        return false;
    }
    __try {
        destructor(loader);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeLoadPackage(
    DavaResourcesContext* context,
    void* loader,
    const DavaFilePath32* path,
    void* builder,
    bool* outLoaded) {
    UiLoadPackageFn load = reinterpret_cast<UiLoadPackageFn>(Address(
        context, context->ui_load_package_rva));
    if (!load || !outLoaded ||
        !IsExecutableAddress(reinterpret_cast<const void*>(load))) {
        return false;
    }
    __try {
        *outLoaded = load(loader, path, builder);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outLoaded = false;
        return false;
    }
}

static bool InvokeExtractControl(
    DavaResourcesContext* context,
    void* package,
    void** outControl,
    const std::string* name) {
    UiExtractControlFn extract =
        reinterpret_cast<UiExtractControlFn>(Address(
            context, context->ui_extract_control_rva));
    if (!extract || !outControl || !name ||
        !IsExecutableAddress(reinterpret_cast<const void*>(extract))) {
        return false;
    }
    __try {
        extract(package, outControl, name);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outControl = nullptr;
        return false;
    }
}

static bool ReadPackageState(
    void* builder,
    void** outPackage,
    bool* outError) {
    if (!builder || !outPackage || !outError) return false;
    __try {
        *outPackage = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(builder) +
            kUiPackageBuilderPackageOffset);
        *outError = *reinterpret_cast<uint8_t*>(
            static_cast<uint8_t*>(builder) +
            kUiPackageErrorOffset) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outPackage = nullptr;
        *outError = true;
        return false;
    }
}

static bool ReadFirstPackageControl(
    void* package,
    void** outControl) {
    if (!package || !outControl) return false;
    __try {
        uint8_t* bytes = static_cast<uint8_t*>(package);
        void** begin = *reinterpret_cast<void***>(
            bytes + kUiPackageControlsBeginOffset);
        void** end = *reinterpret_cast<void***>(
            bytes + kUiPackageControlsEndOffset);
        if (!begin || begin >= end) {
            *outControl = nullptr;
            return true;
        }
        *outControl = *begin;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outControl = nullptr;
        return false;
    }
}

static WotbModResult LoadUiObject(
    DavaResourcesContext* context,
    WotbModResourceType type,
    const char* davaPath,
    const char* objectName,
    void** outNativeObject) {
    if (!context || !davaPath || !outNativeObject) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeObject = nullptr;

    void* builder = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, kUiPackageBuilderSize);
    void* loader = HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, kUiPackageLoaderSize);
    if (!builder || !loader) {
        if (builder) HeapFree(GetProcessHeap(), 0, builder);
        if (loader) HeapFree(GetProcessHeap(), 0, loader);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    void* emptyCache = nullptr;
    bool builderConstructed = InvokeBuilderCtor(
        context, builder, &emptyCache);
    bool loaderConstructed = builderConstructed &&
                             InvokeLoaderCtor(context, loader);
    bool loadInvoked = false;
    bool loaded = false;
    DavaFilePath32 path(davaPath);
    if (loaderConstructed) {
        loadInvoked = InvokeLoadPackage(
            context, loader, &path, builder, &loaded);
    }
    bool loaderDestroyed = true;
    if (loaderConstructed) {
        loaderDestroyed = InvokeLoaderDtor(context, loader);
    }

    void* package = nullptr;
    bool packageError = true;
    const bool packageStateRead =
        builderConstructed &&
        ReadPackageState(builder, &package, &packageError);
    WotbModResult result = WOTBMOD_ERROR_NOT_FOUND;
    void* resultObject = nullptr;

    if (loadInvoked &&
        loaded &&
        loaderDestroyed &&
        packageStateRead &&
        !packageError &&
        ValidateObject(
            context, package, context->ui_package_vtable_rva)) {
        if (type == WOTBMOD_RESOURCE_UI_PACKAGE) {
            if (InvokeRetain(context, package)) {
                resultObject = package;
                result = WOTBMOD_OK;
            } else {
                result = WOTBMOD_ERROR_PLATFORM;
            }
        } else if (objectName && objectName[0]) {
            std::string name(objectName);
            void* control = nullptr;
            if (!InvokeExtractControl(
                    context, package, &control, &name)) {
                result = WOTBMOD_ERROR_CALLBACK_FAULT;
            } else if (control &&
                       ValidateObject(context, control, 0)) {
                resultObject = control;
                result = WOTBMOD_OK;
            } else {
                if (control) InvokeRelease(context, control);
                result = WOTBMOD_ERROR_NOT_FOUND;
            }
        } else {
            void* control = nullptr;
            if (!ReadFirstPackageControl(package, &control)) {
                result = WOTBMOD_ERROR_CALLBACK_FAULT;
            } else if (control &&
                       ValidateObject(context, control, 0) &&
                       InvokeRetain(context, control)) {
                resultObject = control;
                result = WOTBMOD_OK;
            } else {
                result = WOTBMOD_ERROR_NOT_FOUND;
            }
        }
    } else if (!builderConstructed ||
               !loaderConstructed ||
               !loadInvoked ||
               !loaderDestroyed ||
               !packageStateRead) {
        result = WOTBMOD_ERROR_CALLBACK_FAULT;
    }

    bool builderDestroyed = true;
    if (builderConstructed) {
        builderDestroyed = InvokeBuilderDtor(context, builder);
    }
    HeapFree(GetProcessHeap(), 0, loader);
    HeapFree(GetProcessHeap(), 0, builder);

    if (!builderDestroyed) {
        if (resultObject) InvokeRelease(context, resultObject);
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    *outNativeObject = resultObject;
    return result;
}

static bool InvokeSceneLoadEntity(
    DavaResourcesContext* context,
    const DavaFilePath32* path,
    void** outScene) {
    SceneLoadEntityFn load =
        reinterpret_cast<SceneLoadEntityFn>(Address(
            context, context->scene_load_entity_rva));
    if (!load ||
        !outScene ||
        !IsExecutableAddress(reinterpret_cast<const void*>(load))) {
        return false;
    }
    __try {
        load(outScene, path);
        return *outScene != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outScene = nullptr;
        return false;
    }
}

static WotbModResult LoadSceneObject(
    DavaResourcesContext* context,
    const char* davaPath,
    void** outNativeObject) {
    if (!context || !davaPath || !outNativeObject) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeObject = nullptr;

    DavaFilePath32 path(davaPath);
    void* scene = nullptr;
    if (!InvokeSceneLoadEntity(context, &path, &scene)) {
        Log(
            context,
            WOTBMOD_LOG_ERROR,
            "DAVA Scene EntityCache load wrapper faulted");
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (!ValidateObject(context, scene, context->scene_vtable_rva)) {
        InvokeRelease(context, scene);
        Log(
            context,
            WOTBMOD_LOG_ERROR,
            "DAVA Scene object failed vtable validation");
        return WOTBMOD_ERROR_PLATFORM;
    }
    *outNativeObject = scene;
    return WOTBMOD_OK;
}

static bool InvokeEntityGetRenderObject(
    DavaResourcesContext* context,
    const void* entity,
    void** outRenderObject) {
    if (!context || !entity || !outRenderObject) return false;
    *outRenderObject = nullptr;
    EntityGetRenderObjectFn getRenderObject =
        reinterpret_cast<EntityGetRenderObjectFn>(Address(
            context, context->entity_get_render_object_rva));
    if (!getRenderObject ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(getRenderObject))) {
        return false;
    }
    __try {
        *outRenderObject = getRenderObject(entity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outRenderObject = nullptr;
        return false;
    }
}

static bool InvokeRenderObjectGetBatch(
    DavaResourcesContext* context,
    void* renderObject,
    uint32_t index,
    void** outBatch) {
    if (!context || !renderObject || !outBatch) return false;
    *outBatch = nullptr;
    RenderObjectGetRenderBatchFn getBatch =
        reinterpret_cast<RenderObjectGetRenderBatchFn>(Address(
            context, context->render_object_get_render_batch_rva));
    if (!getBatch ||
        !IsExecutableAddress(reinterpret_cast<const void*>(getBatch))) {
        return false;
    }
    __try {
        *outBatch = getBatch(renderObject, index);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outBatch = nullptr;
        return false;
    }
}

static bool InvokeRenderBatchSetPolygonGroup(
    DavaResourcesContext* context,
    void* batch,
    void* polygonGroup) {
    if (!context || !batch || !polygonGroup ||
        !ValidateObject(context, batch, context->render_batch_vtable_rva) ||
        !ValidateObject(context, polygonGroup, 0u)) {
        return false;
    }
    RenderBatchSetPolygonGroupFn setPolygonGroup =
        reinterpret_cast<RenderBatchSetPolygonGroupFn>(Address(
            context, context->render_batch_set_polygon_group_rva));
    if (!setPolygonGroup ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(setPolygonGroup))) {
        return false;
    }
    __try {
        setPolygonGroup(batch, polygonGroup);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool InvokeRenderBatchSetMaterial(
    DavaResourcesContext* context,
    void* batch,
    void* material) {
    if (!context || !batch || !material ||
        !ValidateObject(context, batch, context->render_batch_vtable_rva) ||
        !ValidateObject(context, material, context->nmaterial_vtable_rva)) {
        return false;
    }
    RenderBatchSetMaterialFn setMaterial =
        reinterpret_cast<RenderBatchSetMaterialFn>(Address(
            context, context->render_batch_set_material_rva));
    if (!setMaterial ||
        !IsExecutableAddress(reinterpret_cast<const void*>(setMaterial))) {
        return false;
    }
    __try {
        setMaterial(batch, material);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadPointerVector(
    const void* object,
    size_t beginOffset,
    size_t endOffset,
    void*** outBegin,
    void*** outEnd) {
    if (!object || !outBegin || !outEnd) return false;
    *outBegin = nullptr;
    *outEnd = nullptr;
    __try {
        const uint8_t* bytes = static_cast<const uint8_t*>(object);
        void** begin = *reinterpret_cast<void***>(
            const_cast<uint8_t*>(bytes) + beginOffset);
        void** end = *reinterpret_cast<void***>(
            const_cast<uint8_t*>(bytes) + endOffset);
        if ((!begin && end) || (begin && !end) || end < begin) return false;
        *outBegin = begin;
        *outEnd = end;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadRenderBatchCount(
    void* renderObject,
    uint32_t* outCount) {
    if (!renderObject || !outCount) return false;
    *outCount = 0u;
    __try {
        uint8_t* bytes = static_cast<uint8_t*>(renderObject);
        uint8_t* begin = *reinterpret_cast<uint8_t**>(
            bytes + kRenderObjectBatchesBeginOffset);
        uint8_t* end = *reinterpret_cast<uint8_t**>(
            bytes + kRenderObjectBatchesEndOffset);
        if ((!begin && end) || (begin && !end) || end < begin) return false;
        const uintptr_t byteCount =
            reinterpret_cast<uintptr_t>(end) -
            reinterpret_cast<uintptr_t>(begin);
        if ((byteCount & 0x0Fu) != 0u) return false;
        const uintptr_t count = byteCount >> 4u;
        if (count > kMaxRenderBatchesPerObject) return false;
        *outCount = static_cast<uint32_t>(count);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadBatchPolygonGroup(
    DavaResourcesContext* context,
    void* batch,
    void** outPolygonGroup) {
    if (!context || !batch || !outPolygonGroup ||
        !ValidateObject(context, batch, context->render_batch_vtable_rva)) {
        return false;
    }
    *outPolygonGroup = nullptr;
    __try {
        void* polygonGroup = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(batch) +
            kRenderBatchPolygonGroupOffset);
        if (polygonGroup && !ValidateObject(context, polygonGroup, 0u)) {
            return false;
        }
        *outPolygonGroup = polygonGroup;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool FindFirstMeshBatch(
    DavaResourcesContext* context,
    void* entity,
    uint32_t depth,
    void** outBatch,
    void** outPolygonGroup) {
    if (!context || !entity || !outBatch || !outPolygonGroup ||
        depth > kMaxMeshTraversalDepth ||
        !ValidateObject(context, entity, 0u)) {
        return false;
    }

    void* renderObject = nullptr;
    if (!InvokeEntityGetRenderObject(context, entity, &renderObject)) {
        return false;
    }
    if (renderObject) {
        uint32_t batchCount = 0u;
        if (!ValidateObject(context, renderObject, 0u) ||
            !ReadRenderBatchCount(renderObject, &batchCount)) {
            return false;
        }
        for (uint32_t index = 0u; index < batchCount; ++index) {
            void* batch = nullptr;
            void* polygonGroup = nullptr;
            if (!InvokeRenderObjectGetBatch(
                    context, renderObject, index, &batch) ||
                !batch ||
                !ReadBatchPolygonGroup(
                    context, batch, &polygonGroup)) {
                return false;
            }
            if (polygonGroup) {
                *outBatch = batch;
                *outPolygonGroup = polygonGroup;
                return true;
            }
        }
    }

    void** childrenBegin = nullptr;
    void** childrenEnd = nullptr;
    if (!ReadPointerVector(
            entity,
            kEntityChildrenBeginOffset,
            kEntityChildrenEndOffset,
            &childrenBegin,
            &childrenEnd)) {
        return false;
    }
    const uintptr_t childCount = childrenBegin
        ? static_cast<uintptr_t>(childrenEnd - childrenBegin)
        : 0u;
    if (childCount > kMaxMeshChildrenPerEntity) return false;
    for (uintptr_t index = 0u; index < childCount; ++index) {
        void* child = nullptr;
        __try {
            child = childrenBegin[index];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (!child) continue;
        if (FindFirstMeshBatch(
                context,
                child,
                depth + 1u,
                outBatch,
                outPolygonGroup)) {
            return true;
        }
    }
    return false;
}

static WotbModResult LoadFirstMeshBatch(
    DavaResourcesContext* context,
    const char* resolvedPath,
    void** outScene,
    void** outBatch,
    void** outPolygonGroup) {
    if (!context || !resolvedPath || !resolvedPath[0] ||
        !outScene || !outBatch || !outPolygonGroup) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outScene = nullptr;
    *outBatch = nullptr;
    *outPolygonGroup = nullptr;
    char davaPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    BuildDavaPath(context, resolvedPath, davaPath, sizeof(davaPath));
    if (!davaPath[0]) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    void* scene = nullptr;
    WotbModResult result = LoadSceneObject(context, davaPath, &scene);
    if (result != WOTBMOD_OK) return result;
    void* batch = nullptr;
    void* polygonGroup = nullptr;
    if (!FindFirstMeshBatch(
            context, scene, 0u, &batch, &polygonGroup)) {
        (void)InvokeRelease(context, scene);
        return WOTBMOD_ERROR_NOT_FOUND;
    }
    *outScene = scene;
    *outBatch = batch;
    *outPolygonGroup = polygonGroup;
    return WOTBMOD_OK;
}

static bool IsDavaObjectType(WotbModResourceType type) {
    return type == WOTBMOD_RESOURCE_UI_PACKAGE ||
           type == WOTBMOD_RESOURCE_UI_CONTROL ||
           type == WOTBMOD_RESOURCE_SCENE;
}

static WotbModResult BuildNativeObject(
    DavaResourcesContext* context,
    WotbModResourceType type,
    const char* resolvedPath,
    const char* objectName,
    void** outNativeObject) {
    if (!outNativeObject) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outNativeObject = nullptr;
    if (!IsDavaObjectType(type)) return WOTBMOD_OK;

    char davaPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    BuildDavaPath(context, resolvedPath, davaPath, sizeof(davaPath));
    if (!davaPath[0]) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    if (type == WOTBMOD_RESOURCE_SCENE) {
        return LoadSceneObject(context, davaPath, outNativeObject);
    }
    return LoadUiObject(
        context, type, davaPath, objectName, outNativeObject);
}

static WotbModResult WOTBMOD_CALL ResourceLoad(
    void* userData,
    const WotbModResourceLoadRequest* request,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context ||
        !request ||
        !request->virtual_path ||
        !outNativeResource ||
        request->type < WOTBMOD_RESOURCE_GENERIC ||
        request->type > WOTBMOD_RESOURCE_AUDIO_CLIP) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    if (request->object_name &&
        strlen(request->object_name) >= kMaxObjectName) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint64_t fileSize = 0;
    WotbModResult result = ResolveMountedFile(
        request->virtual_path,
        resolved,
        sizeof(resolved),
        &fileSize);
    if (result != WOTBMOD_OK) return result;

    void* nativeObject = nullptr;
    result = BuildNativeObject(
        context,
        request->type,
        resolved,
        request->object_name,
        &nativeObject);
    if (result != WOTBMOD_OK) {
        LogResource(
            context,
            WOTBMOD_LOG_ERROR,
            "load failed",
            request->type,
            request->virtual_path,
            request->object_name);
        return result;
    }

    DavaResource* resource = static_cast<DavaResource*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(DavaResource)));
    if (!resource) {
        if (nativeObject) InvokeRelease(context, nativeObject);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }

    resource->magic = kResourceMagic;
    resource->context = context;
    resource->type = request->type;
    resource->flags = request->flags;
    resource->provenance = DAVA_RESOURCE_FILE;
    resource->native_object = nativeObject;
    resource->file_size = fileSize;
    strcpy_s(resource->virtual_path, request->virtual_path);
    strcpy_s(resource->resolved_path, resolved);
    if (request->object_name) {
        strcpy_s(resource->object_name, request->object_name);
    }

    InterlockedIncrement(&context->active_resources);
    *outNativeResource = resource;
    LogResource(
        context,
        WOTBMOD_LOG_INFO,
        nativeObject ? "loaded native object" : "loaded file",
        request->type,
        request->virtual_path,
        request->object_name);
    return WOTBMOD_OK;
}

static bool IsAbsoluteWindowsPath(const char* path) {
    if (!path || !path[0]) return false;
    const bool drivePath =
        ((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' &&
        (path[2] == '\\' || path[2] == '/');
    const bool uncPath =
        (path[0] == '\\' || path[0] == '/') &&
        (path[1] == '\\' || path[1] == '/');
    return drivePath || uncPath;
}

static WotbModResult WOTBMOD_CALL ResourceLoadResolved(
    void* userData,
    WotbModResourceType type,
    const char* resolvedFilePath,
    const char* objectName,
    uint32_t flags,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context ||
        !resolvedFilePath ||
        !outNativeResource ||
        !IsAbsoluteWindowsPath(resolvedFilePath) ||
        strlen(resolvedFilePath) >= WOTBMOD_MAX_RESOURCE_PATH ||
        (objectName && strlen(objectName) >= kMaxObjectName) ||
        type < WOTBMOD_RESOURCE_GENERIC ||
        type > WOTBMOD_RESOURCE_AUDIO_CLIP) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;

    char canonicalPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    const DWORD canonicalLength = GetFullPathNameA(
        resolvedFilePath,
        static_cast<DWORD>(sizeof(canonicalPath)),
        canonicalPath,
        nullptr);
    if (canonicalLength == 0u ||
        canonicalLength >= sizeof(canonicalPath) ||
        !IsAbsoluteWindowsPath(canonicalPath)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    uint64_t fileSize = 0u;
    if (!ReadFileSize(canonicalPath, &fileSize)) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    void* nativeObject = nullptr;
    WotbModResult result = BuildNativeObject(
        context,
        type,
        canonicalPath,
        objectName,
        &nativeObject);
    if (result != WOTBMOD_OK) {
        LogResource(
            context,
            WOTBMOD_LOG_ERROR,
            "resolved load failed",
            type,
            canonicalPath,
            objectName);
        return result;
    }

    DavaResource* resource = static_cast<DavaResource*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(DavaResource)));
    if (!resource) {
        if (nativeObject) InvokeRelease(context, nativeObject);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    resource->magic = kResourceMagic;
    resource->context = context;
    resource->type = type;
    resource->flags = flags;
    resource->provenance = DAVA_RESOURCE_FILE;
    resource->native_object = nativeObject;
    resource->file_size = fileSize;
    strcpy_s(resource->virtual_path, canonicalPath);
    strcpy_s(resource->resolved_path, canonicalPath);
    if (objectName) {
        strcpy_s(resource->object_name, objectName);
    }
    InterlockedIncrement(&context->active_resources);
    *outNativeResource = resource;
    /* A game-owned UI wrapper has no path: the loader makes one per frame
     * for the active-screen probe and a tree walk makes thousands, and each
     * line was a synchronous write into a log that had reached 100+ MB
     * (2026-09-05, hangar stutter). Those wrappers stay silent. */
    if (!(type == WOTBMOD_RESOURCE_UI_CONTROL && canonicalPath[0] == '\0')) {
        LogResource(
            context,
            WOTBMOD_LOG_INFO,
            nativeObject ? "loaded resolved native object"
                         : "loaded resolved file",
            type,
            canonicalPath,
            objectName);
    }
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceReload(
    void* userData,
    void* nativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource =
        ValidResource(context, nativeResource);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (resource->provenance != DAVA_RESOURCE_FILE) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint64_t fileSize = 0;
    WotbModResult result = ResolveMountedFile(
        resource->virtual_path,
        resolved,
        sizeof(resolved),
        &fileSize);
    if (result != WOTBMOD_OK) return result;

    void* replacement = nullptr;
    result = BuildNativeObject(
        context,
        resource->type,
        resolved,
        resource->object_name,
        &replacement);
    if (result != WOTBMOD_OK) return result;

    if (resource->native_object &&
        !InvokeRelease(context, resource->native_object)) {
        if (replacement) InvokeRelease(context, replacement);
        return WOTBMOD_ERROR_PLATFORM;
    }
    resource->native_object = replacement;
    resource->file_size = fileSize;
    strcpy_s(resource->resolved_path, resolved);
    LogResource(
        context,
        WOTBMOD_LOG_INFO,
        replacement ? "reloaded native object" : "reloaded file",
        resource->type,
        resource->virtual_path,
        resource->object_name);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceRelease(
    void* userData,
    void* nativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource =
        ValidResource(context, nativeResource);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    if (resource->native_object &&
        !InvokeRelease(context, resource->native_object)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    if (!(resource->type == WOTBMOD_RESOURCE_UI_CONTROL &&
          resource->virtual_path[0] == '\0')) {
        LogResource(
            context,
            WOTBMOD_LOG_INFO,
            "released",
            resource->type,
            resource->virtual_path,
            resource->object_name);
    }
    resource->native_object = nullptr;
    resource->magic = 0;
    resource->context = nullptr;
    const BOOL freed = HeapFree(GetProcessHeap(), 0, resource);
    InterlockedDecrement(&context->active_resources);
    return freed ? WOTBMOD_OK : WOTBMOD_ERROR_PLATFORM;
}

static WotbModResult WOTBMOD_CALL ResourceClone(
    void* userData,
    void* nativeResource,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* source =
        ValidResource(context, nativeResource);
    if (!source || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;

    DavaResource* clone = static_cast<DavaResource*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(DavaResource)));
    if (!clone) return WOTBMOD_ERROR_LIMIT_REACHED;
    if (source->native_object &&
        !InvokeRetain(context, source->native_object)) {
        HeapFree(GetProcessHeap(), 0, clone);
        return WOTBMOD_ERROR_PLATFORM;
    }
    memcpy(clone, source, sizeof(*clone));
    InterlockedIncrement(&context->active_resources);
    *outNativeResource = clone;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiSetGeometry(
    void* userData,
    void* nativeResource,
    const WotbModUiControlGeometry* geometry) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context, nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !geometry) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    UiSetVectorFn setPosition = VirtualFunction<UiSetVectorFn>(
        resource->native_object, kUiSetPositionVtableIndex);
    UiSetVectorFn setSize = VirtualFunction<UiSetVectorFn>(
        resource->native_object, kUiSetSizeVtableIndex);
    if (!setPosition || !setSize) return WOTBMOD_ERROR_PLATFORM;

    const DavaVector2 position = {geometry->x, geometry->y};
    const DavaVector2 size = {geometry->width, geometry->height};
    __try {
        setPosition(resource->native_object, &position);
        setSize(resource->native_object, &size);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL UiSetVisible(
    void* userData,
    void* nativeResource,
    int32_t visible) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context, nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    /*
     * 2026-09-08: the engine's own setter, when the image still carries it
     * at the anchored address. It cascades the view state (OnVisible /
     * OnInvisible - a StyledButton paints its background from there) and
     * marks the layout dirty; the raw flag write below did neither, so a
     * game-owned button shown through this slot stayed blank.
     */
    const uint8_t* setter = static_cast<const uint8_t*>(
        Address(context, kDefaultUiControlSetVisibilityFlagRva, 6u));
    static const uint8_t kSetVisibilityPrologue[6] = {
        0x55, 0x8B, 0xEC, 0x8A, 0x55, 0x08};
    if (setter && IsImageCodeAddress(setter) &&
        memcmp(setter, kSetVisibilityPrologue,
               sizeof(kSetVisibilityPrologue)) == 0) {
        UiSetVisibilityFlagFn setVisibility =
            reinterpret_cast<UiSetVisibilityFlagFn>(
                const_cast<uint8_t*>(setter));
        __try {
            setVisibility(resource->native_object, visible != 0);
            return WOTBMOD_OK;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }
    __try {
        uint8_t* flags = static_cast<uint8_t*>(
            resource->native_object) + kUiFlagsOffset;
        const uint8_t oldFlags = *flags;
        const uint8_t newFlags =
            visible != 0
                ? static_cast<uint8_t>(oldFlags | kUiVisibleBit)
                : static_cast<uint8_t>(oldFlags & ~kUiVisibleBit);
        if (oldFlags != newFlags) {
            *flags = newFlags;
            *(static_cast<uint8_t*>(resource->native_object) +
              kUiDirtyFlagsOffset) |= kUiHierarchyDirtyBit;
        }
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL UiFindByName(
    void* userData,
    void* rootNativeResource,
    const char* name,
    int32_t recursive,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* root = TypedObjectResource(
        context,
        rootNativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!root || !name || !name[0] || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    FastNameScope fastName = {};
    if (!MakeFastName(context, name, &fastName)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    uint32_t visited = 0;
    void* found = UiControlHasFastName(
                      context,
                      root->native_object,
                      fastName.value)
        ? root->native_object
        : FindUiChildByFastName(
              context,
              root->native_object,
              fastName.value,
              recursive != 0,
              0u,
              &visited);
    ReleaseFastName(context, &fastName);
    if (!found) return WOTBMOD_ERROR_NOT_FOUND;
    return WrapRetainedUiControl(
        context, found, outNativeResource);
}

static WotbModResult WOTBMOD_CALL UiGetParent(
    void* userData,
    void* nativeResource,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    void* parent = nullptr;
    __try {
        parent = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(resource->native_object) +
            kUiParentOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (!parent) return WOTBMOD_ERROR_NOT_FOUND;
    return WrapRetainedUiControl(
        context, parent, outNativeResource);
}

static WotbModResult WOTBMOD_CALL UiGetChildCount(
    void* userData,
    void* nativeResource,
    uint32_t* outCount) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outCount) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outCount = 0;
    __try {
        void* sentinel = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(resource->native_object) +
            kUiChildrenOffset);
        if (!sentinel) return WOTBMOD_ERROR_PLATFORM;
        void* node = *reinterpret_cast<void**>(sentinel);
        while (node && node != sentinel && *outCount < 16384u) {
            ++*outCount;
            void* next = *reinterpret_cast<void**>(node);
            if (next == node) return WOTBMOD_ERROR_PLATFORM;
            node = next;
        }
        return node == sentinel
                   ? WOTBMOD_OK
                   : WOTBMOD_ERROR_PLATFORM;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCount = 0;
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL UiGetChildAt(
    void* userData,
    void* nativeResource,
    uint32_t index,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outNativeResource || index >= 16384u) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;
    void* child = nullptr;
    __try {
        void* sentinel = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(resource->native_object) +
            kUiChildrenOffset);
        if (!sentinel) return WOTBMOD_ERROR_PLATFORM;
        void* node = *reinterpret_cast<void**>(sentinel);
        uint32_t current = 0;
        while (node && node != sentinel && current < index) {
            void* next = *reinterpret_cast<void**>(node);
            if (next == node) return WOTBMOD_ERROR_PLATFORM;
            node = next;
            ++current;
        }
        if (!node || node == sentinel) return WOTBMOD_ERROR_NOT_FOUND;
        child = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(node) + 8u);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (!child) return WOTBMOD_ERROR_NOT_FOUND;
    return WrapRetainedUiControl(
        context, child, outNativeResource);
}

static WotbModResult WOTBMOD_CALL UiGetState(
    void* userData,
    void* nativeResource,
    WotbModUiControlState* outState) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outState) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    __try {
        const uint8_t* object = static_cast<const uint8_t*>(
            resource->native_object);
        outState->struct_size = sizeof(*outState);
        outState->geometry.struct_size =
            sizeof(outState->geometry);
        outState->geometry.x =
            *reinterpret_cast<const float*>(
                object + kUiPositionOffset);
        outState->geometry.y =
            *reinterpret_cast<const float*>(
                object + kUiPositionOffset + sizeof(float));
        outState->geometry.width =
            *reinterpret_cast<const float*>(
                object + kUiSizeOffset);
        outState->geometry.height =
            *reinterpret_cast<const float*>(
                object + kUiSizeOffset + sizeof(float));
        outState->flags = 0;
        if ((object[kUiFlagsOffset] & kUiVisibleBit) != 0) {
            outState->flags |= WOTBMOD_UI_CONTROL_VISIBLE;
        }
        if (*reinterpret_cast<const int32_t*>(
                object + kUiInputProcessorsCountOffset) > 0) {
            outState->flags |= WOTBMOD_UI_CONTROL_INPUT_ENABLED;
        }
        if ((*reinterpret_cast<const uint32_t*>(
                 object + kUiControlStateOffset) &
             kUiDisabledStateBit) != 0) {
            outState->flags |= WOTBMOD_UI_CONTROL_DISABLED;
        }
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

/*
 * UIControl components on 11.20.0.887 (IDA wotb1120, UIControl dtor
 * sub_B8BDC0 -> RemoveAllComponents sub_BC4660, 2026-09-04): the control
 * holds a std::vector of per-type std::vector<UIComponent*> at +0xE4
 * ({begin,end,capacity}, 12-byte inner vectors), and every component points
 * back at its control through UIComponent+0x08. The text component is the
 * element whose vtable is DAVA::UITextComponent's. Every pointer is checked
 * (readable, vtable inside the client image, back-pointer equal to the
 * control) before anything is used, under SEH. Returns the UITextComponent
 * of the control, or null when it has none or the layout does not validate.
 */
const size_t kUiComponentsOuterOffset = 0xE4u;
const size_t kUiComponentsInnerStride = 12u;
const size_t kUiComponentsMaxTypes = 64u;
static volatile LONG g_uiComponentsProbeLogs = 0;

/* True when `pointer` lies inside the client executable image: the only
 * place a real vtable can be. */
static bool PointsIntoImage(DavaResourcesContext* context, const void* pointer) {
    const uint8_t* base = static_cast<const uint8_t*>(Address(context, 0u));
    if (!base || !pointer) return false;
    const uint8_t* candidate = static_cast<const uint8_t*>(pointer);
    if (candidate < base) return false;
    if (!IsReadableRange(base, 0x400u)) return false;
    const uint32_t ntOffset = *reinterpret_cast<const uint32_t*>(base + 0x3Cu);
    if (ntOffset == 0u || ntOffset > 0x1000u) return false;
    const uint32_t sizeOfImage =
        *reinterpret_cast<const uint32_t*>(base + ntOffset + 0x50u);
    return sizeOfImage != 0u && candidate < base + sizeOfImage;
}

/* One line for the first few controls whose component table did not
 * validate: the outer pair and the first inner vector, so a layout drift on
 * a new build is read off the log rather than guessed. */
static void LogUiComponentsProbe(
    DavaResourcesContext* context,
    const uint8_t* control,
    const void* textVtable,
    const char* reason) {
    if (InterlockedIncrement(&g_uiComponentsProbeLogs) > 3) return;
    char line[512] = {};
    const uint8_t* outerBegin = nullptr;
    const uint8_t* outerEnd = nullptr;
    const uint8_t* innerBegin = nullptr;
    const uint8_t* innerEnd = nullptr;
    const void* firstVtable = nullptr;
    const void* firstBack = nullptr;
    if (IsReadableRange(control + kUiComponentsOuterOffset, 8u)) {
        outerBegin = *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset);
        outerEnd = *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset + 4u);
        if (outerBegin && IsReadableRange(outerBegin, kUiComponentsInnerStride)) {
            innerBegin = *reinterpret_cast<const uint8_t* const*>(outerBegin);
            innerEnd = *reinterpret_cast<const uint8_t* const*>(outerBegin + 4u);
            if (innerBegin && innerEnd > innerBegin && IsReadableRange(innerBegin, 4u)) {
                const uint8_t* element = *reinterpret_cast<const uint8_t* const*>(innerBegin);
                if (element && IsReadableRange(element, kUiComponentControlOffset + 4u)) {
                    firstVtable = *reinterpret_cast<const void* const*>(element);
                    firstBack = *reinterpret_cast<const void* const*>(element + kUiComponentControlOffset);
                }
            }
        }
    }
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "DAVA ui text probe (%s): control=%p text_vtable=%p outer=%p..%p inner0=%p..%p first_vtable=%p first_back=%p",
                reason, control, textVtable, outerBegin, outerEnd, innerBegin, innerEnd,
                firstVtable, firstBack);
    Log(context, WOTBMOD_LOG_INFO, line);
}

/* The two text component classes and where each keeps its UTF-8 text. */
struct UiTextClass {
    const void* vtable;
    size_t text_offset;
};

/* When `component` is a readable text component of `control`, returns the
 * offset of its std::string text; 0 otherwise. */
static size_t TextOffsetOf(
    DavaResourcesContext* context,
    const uint8_t* component,
    const uint8_t* control,
    const UiTextClass* classes,
    size_t class_count) {
    if (!component ||
        !IsReadableRange(component, kUiComponentControlOffset + sizeof(void*))) {
        return 0u;
    }
    const void* vtable = *reinterpret_cast<const void* const*>(component);
    size_t offset = 0u;
    for (size_t i = 0u; i < class_count; ++i) {
        if (classes[i].vtable && classes[i].vtable == vtable) offset = classes[i].text_offset;
    }
    if (offset == 0u || !PointsIntoImage(context, vtable)) return 0u;
    return *reinterpret_cast<const uint8_t* const*>(
               component + kUiComponentControlOffset) == control ? offset : 0u;
}

/* DAVA::UIStaticText (11.20.0.887, ctor sub_B8A3A0) keeps the UITextComponent
 * it created through GetOrCreateComponent in its own field right after the
 * UIControl body, at +0x140. Reading it is guarded: the dword must be
 * readable, point at an object whose vtable is UITextComponent's and whose
 * back-pointer is this control - a plain 0x140-byte UIControl fails the first
 * or second test and is never dereferenced further. */
const size_t kUiStaticTextComponentOffset = 0x140u;
static volatile LONG g_uiTextScanLogs = 0;

/* MSVC RTTI, read under the same guards as everything else here: the class
 * name of a polymorphic object (vtable[-1] -> CompleteObjectLocator ->
 * TypeDescriptor -> name at +8). Empty when any pointer fails validation. */
static void RttiClassName(
    DavaResourcesContext* context,
    const void* object,
    char* out,
    size_t capacity) {
    out[0] = '\0';
    if (!object || !IsReadableRange(object, sizeof(void*))) return;
    const uint8_t* vtable = *reinterpret_cast<const uint8_t* const*>(object);
    if (!PointsIntoImage(context, vtable) || !IsReadableRange(vtable - 4u, 4u)) return;
    const uint8_t* locator = *reinterpret_cast<const uint8_t* const*>(vtable - 4u);
    if (!PointsIntoImage(context, locator) || !IsReadableRange(locator, 16u)) return;
    const uint8_t* descriptor = *reinterpret_cast<const uint8_t* const*>(locator + 12u);
    if (!PointsIntoImage(context, descriptor) || !IsReadableRange(descriptor + 8u, 64u)) return;
    const char* name = reinterpret_cast<const char*>(descriptor + 8u);
    size_t n = 0u;
    while (n + 1u < capacity && n < 63u && name[n] != '\0') { out[n] = name[n]; ++n; }
    out[n] = '\0';
}

/* Live class inventory: the first 48 distinct control vtables met by the
 * text finder are logged once with their RTTI name, so the class that
 * actually carries hangar/HUD text can be read off the log. */
static const void* g_uiSeenVtables[48] = {};
static volatile LONG g_uiSeenVtableCount = 0;

static void LogControlClassOnce(DavaResourcesContext* context, const uint8_t* control) {
    if (!IsReadableRange(control, sizeof(void*))) return;
    const void* vtable = *reinterpret_cast<const void* const*>(control);
    const LONG count = InterlockedCompareExchange(&g_uiSeenVtableCount, 0, 0);
    for (LONG i = 0; i < count && i < 48; ++i) {
        if (g_uiSeenVtables[i] == vtable) return;
    }
    if (count >= 48) return;
    g_uiSeenVtables[count] = vtable;
    InterlockedIncrement(&g_uiSeenVtableCount);
    char name[80] = {};
    RttiClassName(context, control, name, sizeof(name));
    const uint8_t* base = static_cast<const uint8_t*>(Address(context, 0u));
    char line[200] = {};
    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "DAVA ui class #%ld: vtable_rva=0x%08X %s",
                count + 1,
                base ? static_cast<unsigned>(static_cast<const uint8_t*>(vtable) - base) : 0u,
                name[0] ? name : "(no rtti)");
    Log(context, WOTBMOD_LOG_INFO, line);
}

/* Live diagnostics for the first five UIStaticText controls met: the class
 * of the object the control keeps at +0x140 and the class of every entry in
 * its component table, so the text-bearing class can be read off the log. */
static volatile LONG g_uiStaticTextDumps = 0;

static void LogStaticTextLayoutOnce(DavaResourcesContext* context, const uint8_t* control) {
    if (InterlockedCompareExchange(&g_uiStaticTextDumps, 0, 0) >= 5) return;
    char cls[80] = {};
    RttiClassName(context, control, cls, sizeof(cls));
    if (!strstr(cls, "UIStaticText")) return;
    InterlockedIncrement(&g_uiStaticTextDumps);
    char line[900] = {};
    int used = _snprintf_s(line, sizeof(line), _TRUNCATE, "DAVA ui static text layout: control=%p %s", control, cls);
    if (IsReadableRange(control + kUiStaticTextComponentOffset, sizeof(void*))) {
        const uint8_t* direct = *reinterpret_cast<const uint8_t* const*>(control + kUiStaticTextComponentOffset);
        char dcls[80] = {};
        RttiClassName(context, direct, dcls, sizeof(dcls));
        const void* dvt = (direct && IsReadableRange(direct, 4u)) ? *reinterpret_cast<const void* const*>(direct) : nullptr;
        int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " +0x140=%p vt=%p %s", direct, dvt, dcls[0] ? dcls : "?");
        if (n > 0) used += n;
    }
    if (IsReadableRange(control + kUiComponentsOuterOffset, 8u)) {
        const uint8_t* outerBegin = *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset);
        const uint8_t* outerEnd = *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset + 4u);
        if (outerBegin && outerEnd > outerBegin && IsReadableRange(outerBegin, static_cast<size_t>(outerEnd - outerBegin))) {
            const size_t types = static_cast<size_t>(outerEnd - outerBegin) / kUiComponentsInnerStride;
            for (size_t t = 0u; t < types && t < 24u && used < static_cast<int>(sizeof(line)) - 100; ++t) {
                const uint8_t* inner = outerBegin + t * kUiComponentsInnerStride;
                const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(inner);
                const uint8_t* en = *reinterpret_cast<const uint8_t* const*>(inner + 4u);
                if (!b || en <= b || !IsReadableRange(b, 4u)) continue;
                const uint8_t* element = *reinterpret_cast<const uint8_t* const*>(b);
                char ecls[80] = {};
                RttiClassName(context, element, ecls, sizeof(ecls));
                int n = _snprintf_s(line + used, sizeof(line) - used, _TRUNCATE, " [%u]=%s", static_cast<unsigned>(t), ecls[0] ? ecls : "?");
                if (n > 0) used += n;
            }
        }
    }
    Log(context, WOTBMOD_LOG_INFO, line);
}

static const uint8_t* FindUiTextComponent(
    DavaResourcesContext* context,
    const uint8_t* control,
    size_t* outTextOffset) {
    *outTextOffset = 0u;
    const UiTextClass classes[2] = {
        {Address(context, context->ui_text_component_vtable_rva), kUiTextComponentTextOffset},
        {Address(context, context->ui_dynamic_atlas_text_component_vtable_rva),
         kUiDynamicAtlasTextComponentTextOffset},
    };
    const void* textVtable = classes[0].vtable;
    if ((!classes[0].vtable && !classes[1].vtable) || !control) return nullptr;
    LogControlClassOnce(context, control);
    LogStaticTextLayoutOnce(context, control);
    if (IsReadableRange(control + kUiStaticTextComponentOffset, sizeof(void*))) {
        const uint8_t* direct = *reinterpret_cast<const uint8_t* const*>(
            control + kUiStaticTextComponentOffset);
        const size_t offset = TextOffsetOf(context, direct, control, classes, 2u);
        if (offset) { *outTextOffset = offset; return direct; }
    }
    if (!IsReadableRange(control + kUiComponentsOuterOffset, 8u)) return nullptr;
    const uint8_t* outerBegin =
        *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset);
    const uint8_t* outerEnd =
        *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset + 4u);
    if (!outerBegin || outerEnd < outerBegin ||
        (static_cast<size_t>(outerEnd - outerBegin) % kUiComponentsInnerStride) != 0u) {
        LogUiComponentsProbe(context, control, textVtable, "outer shape");
        return nullptr;
    }
    const size_t types = static_cast<size_t>(outerEnd - outerBegin) / kUiComponentsInnerStride;
    if (types == 0u) return nullptr; /* a bare control: no components at all */
    if (types > kUiComponentsMaxTypes ||
        !IsReadableRange(outerBegin, types * kUiComponentsInnerStride)) {
        LogUiComponentsProbe(context, control, textVtable, "outer size");
        return nullptr;
    }
    const uint8_t* found = nullptr;
    if (InterlockedCompareExchange(&g_uiTextScanLogs, 0, 0) < 5) {
        /* Live diagnostics: the first five controls with a component table -
         * slot count, first component vtable and back-pointer, expected text
         * vtable - so a wrong assumption is read off the log, not guessed. */
        const uint8_t* firstElement = nullptr;
        for (size_t t = 0u; t < types && !firstElement; ++t) {
            const uint8_t* inner = outerBegin + t * kUiComponentsInnerStride;
            const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(inner);
            const uint8_t* en = *reinterpret_cast<const uint8_t* const*>(inner + 4u);
            if (b && en > b && IsReadableRange(b, sizeof(void*))) {
                firstElement = *reinterpret_cast<const uint8_t* const*>(b);
            }
        }
        const void* firstVtable = nullptr;
        const void* firstBack = nullptr;
        if (firstElement && IsReadableRange(firstElement, kUiComponentControlOffset + sizeof(void*))) {
            firstVtable = *reinterpret_cast<const void* const*>(firstElement);
            firstBack = *reinterpret_cast<const void* const*>(firstElement + kUiComponentControlOffset);
        }
        const void* controlVtable = IsReadableRange(control, sizeof(void*))
            ? *reinterpret_cast<const void* const*>(control) : nullptr;
        char line[320] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "DAVA ui text scan: control=%p vtable=%p slots=%u first_component=%p its_vtable=%p back=%p text_vtable=%p",
                    control, controlVtable, static_cast<unsigned>(types), firstElement,
                    firstVtable, firstBack, textVtable);
        Log(context, WOTBMOD_LOG_INFO, line);
        InterlockedIncrement(&g_uiTextScanLogs);
    }
    for (size_t t = 0u; t < types; ++t) {
        const uint8_t* inner = outerBegin + t * kUiComponentsInnerStride;
        const uint8_t* begin = *reinterpret_cast<const uint8_t* const*>(inner);
        const uint8_t* end = *reinterpret_cast<const uint8_t* const*>(inner + 4u);
        if (!begin || end <= begin) continue;
        if ((static_cast<size_t>(end - begin) % sizeof(void*)) != 0u) {
            LogUiComponentsProbe(context, control, textVtable, "inner shape");
            return nullptr;
        }
        const size_t count = static_cast<size_t>(end - begin) / sizeof(void*);
        if (count > kUiComponentsMaxCount || !IsReadableRange(begin, count * sizeof(void*))) {
            LogUiComponentsProbe(context, control, textVtable, "inner size");
            return nullptr;
        }
        for (size_t i = 0u; i < count; ++i) {
            const uint8_t* element =
                *reinterpret_cast<const uint8_t* const*>(begin + i * sizeof(void*));
            if (!element) continue;
            if (!IsReadableRange(element, kUiComponentControlOffset + sizeof(void*)) ||
                !PointsIntoImage(context, *reinterpret_cast<const void* const*>(element)) ||
                *reinterpret_cast<const uint8_t* const*>(
                    element + kUiComponentControlOffset) != control) {
                LogUiComponentsProbe(context, control, textVtable, "element");
                return nullptr;
            }
            const size_t offset = TextOffsetOf(context, element, control, classes, 2u);
            if (offset) {
                found = element;
                *outTextOffset = offset;
            }
        }
    }
    return found;
}

static WotbModResult WOTBMOD_CALL UiGetText(
    void* userData,
    void* nativeResource,
    char* buffer,
    uint32_t* inoutSize) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !inoutSize) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    __try {
        size_t textOffset = 0u;
        const uint8_t* component = FindUiTextComponent(
            context,
            static_cast<const uint8_t*>(resource->native_object),
            &textOffset);
        if (!component || textOffset == 0u) {
            *inoutSize = 0u;
            return WOTBMOD_ERROR_NOT_FOUND;
        }
        const uint8_t* text = component + textOffset;
        if (!IsReadableRange(text, 0x18u)) return WOTBMOD_ERROR_CALLBACK_FAULT;
        const uint32_t size = *reinterpret_cast<const uint32_t*>(text + 0x10u);
        const uint32_t capacity = *reinterpret_cast<const uint32_t*>(text + 0x14u);
        const char* data = capacity >= 16u
            ? *reinterpret_cast<const char* const*>(text)
            : reinterpret_cast<const char*>(text);
        if (size > capacity || size >= kUiLiveTextMaxLength || !data ||
            !IsReadableRange(data, size + 1u)) {
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
        if (!buffer || *inoutSize <= size) {
            *inoutSize = size + 1u;
            return WOTBMOD_ERROR_BUFFER_TOO_SMALL;
        }
        memcpy(buffer, data, size);
        buffer[size] = '\0';
        *inoutSize = size;
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

/* DAVA::UIControlBackground (11.20.0.887): its reflection registration
 * (sub_B98F00, the "Background" class with drawType/sprite/frame/color/...)
 * binds "color" to the getter sub_7C00F0 (`return this + 0x60`) and the
 * setter sub_BC7E30 (`this[6] = *a2`, one 16-byte store), so the four RGBA
 * floats live at +0x60. The background is a UIComponent on this engine (it
 * shows up as `.?AVUIControlBackground@DAVA@@` in the live component tables,
 * 2026-09-05), found here by RTTI name; the vtable that matched once is kept
 * so later lookups skip the RTTI parse. A control that draws nothing itself
 * has no such component and answers NOT_FOUND. */
const size_t kUiControlBackgroundColorOffset = 0x60u;
/* DAVA::UIDynamicAtlasImageComponent (registered as "Atlas Image" in
 * sub_10F4E20 with imagePath/frame/color/flip/drawType/align): its "color"
 * getter sub_DFFCA0 returns `this + 0x3C` and the setter sub_110C7E0 stores
 * 16 bytes there. Most Blitz HUD sprites (minimap markers, the lamp icon,
 * the aim segments) are drawn by this component, not by a background
 * (live 2026-09-05 diagnostics), so it is the second colour carrier here. */
const size_t kUiDynamicAtlasImageColorOffset = 0x3Cu;
static const void* g_uiBackgroundVtable = nullptr;
static const void* g_uiAtlasImageVtable = nullptr;
/* Live diagnostics: the first 16 distinct control classes that were asked
 * for a background and had none are logged with their component classes,
 * so a sprite that lives in another component is read off the log. */
static const void* g_uiNoBackgroundSeen[16] = {};
static volatile LONG g_uiNoBackgroundCount = 0;

static void LogNoBackgroundOnce(
    DavaResourcesContext* context,
    const uint8_t* control,
    const uint8_t* outerBegin,
    size_t types) {
    if (!IsReadableRange(control, sizeof(void*))) return;
    const LONG count = InterlockedCompareExchange(&g_uiNoBackgroundCount, 0, 0);
    for (LONG i = 0; i < count && i < 16; ++i) {
        if (g_uiNoBackgroundSeen[i] == control) return;
    }
    if (count >= 16) return;
    g_uiNoBackgroundSeen[count] = control;
    InterlockedIncrement(&g_uiNoBackgroundCount);
    char controlClass[96] = {};
    RttiClassName(context, control, controlClass, sizeof(controlClass));
    char line[640] = {};
    size_t used = static_cast<size_t>(_snprintf_s(
        line, sizeof(line), _TRUNCATE,
        "DAVA ui no background: control=%p class=%s slots=%u components=",
        control, controlClass, static_cast<unsigned>(types)));
    for (size_t t = 0u; t < types && used + 40u < sizeof(line); ++t) {
        const uint8_t* inner = outerBegin + t * kUiComponentsInnerStride;
        const uint8_t* begin = *reinterpret_cast<const uint8_t* const*>(inner);
        const uint8_t* end = *reinterpret_cast<const uint8_t* const*>(inner + 4u);
        if (!begin || end <= begin) continue;
        const size_t elements = static_cast<size_t>(end - begin) / sizeof(void*);
        for (size_t i = 0u; i < elements && used + 40u < sizeof(line); ++i) {
            const uint8_t* element =
                *reinterpret_cast<const uint8_t* const*>(begin + i * sizeof(void*));
            if (!element) continue;
            char cls[96] = {};
            RttiClassName(context, element, cls, sizeof(cls));
            used += static_cast<size_t>(_snprintf_s(
                line + used, sizeof(line) - used, _TRUNCATE, "[%u]=%s ",
                static_cast<unsigned>(t), cls[0] ? cls : "?"));
        }
    }
    Log(context, WOTBMOD_LOG_INFO, line);
}

/* The component that carries the control's colour and the offset of the
 * RGBA floats inside it: a UIControlBackground when the control has one,
 * otherwise a UIDynamicAtlasImageComponent. */
static uint8_t* FindUiBackgroundComponent(
    DavaResourcesContext* context,
    const uint8_t* control,
    size_t* outColorOffset) {
    *outColorOffset = 0u;
    if (!control || !IsReadableRange(control + kUiComponentsOuterOffset, 8u)) {
        return nullptr;
    }
    const uint8_t* outerBegin =
        *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset);
    const uint8_t* outerEnd =
        *reinterpret_cast<const uint8_t* const*>(control + kUiComponentsOuterOffset + 4u);
    if (!outerBegin || outerEnd < outerBegin ||
        (static_cast<size_t>(outerEnd - outerBegin) % kUiComponentsInnerStride) != 0u) {
        return nullptr;
    }
    const size_t types = static_cast<size_t>(outerEnd - outerBegin) / kUiComponentsInnerStride;
    if (types == 0u || types > kUiComponentsMaxTypes ||
        !IsReadableRange(outerBegin, types * kUiComponentsInnerStride)) {
        return nullptr;
    }
    for (size_t t = 0u; t < types; ++t) {
        const uint8_t* inner = outerBegin + t * kUiComponentsInnerStride;
        const uint8_t* begin = *reinterpret_cast<const uint8_t* const*>(inner);
        const uint8_t* end = *reinterpret_cast<const uint8_t* const*>(inner + 4u);
        if (!begin || end <= begin ||
            (static_cast<size_t>(end - begin) % sizeof(void*)) != 0u) {
            continue;
        }
        const size_t count = static_cast<size_t>(end - begin) / sizeof(void*);
        if (count > kUiComponentsMaxCount ||
            !IsReadableRange(begin, count * sizeof(void*))) {
            continue;
        }
        for (size_t i = 0u; i < count; ++i) {
            uint8_t* element =
                *reinterpret_cast<uint8_t* const*>(begin + i * sizeof(void*));
            if (!element ||
                !IsReadableRange(element, kUiComponentControlOffset + sizeof(void*))) {
                continue;
            }
            const void* vtable = *reinterpret_cast<const void* const*>(element);
            if (!PointsIntoImage(context, vtable) ||
                *reinterpret_cast<const uint8_t* const*>(
                    element + kUiComponentControlOffset) != control) {
                continue;
            }
            size_t offset = 0u;
            if (vtable == g_uiBackgroundVtable) {
                offset = kUiControlBackgroundColorOffset;
            } else if (vtable == g_uiAtlasImageVtable) {
                offset = kUiDynamicAtlasImageColorOffset;
            } else {
                char raw[96] = {};
                RttiClassName(context, element, raw, sizeof(raw));
                if (strcmp(raw, ".?AVUIControlBackground@DAVA@@") == 0) {
                    g_uiBackgroundVtable = vtable;
                    offset = kUiControlBackgroundColorOffset;
                } else if (strcmp(raw, ".?AVUIDynamicAtlasImageComponent@DAVA@@") == 0) {
                    g_uiAtlasImageVtable = vtable;
                    offset = kUiDynamicAtlasImageColorOffset;
                } else {
                    continue;
                }
            }
            if (!IsReadableRange(element + offset, 4u * sizeof(float))) {
                return nullptr;
            }
            *outColorOffset = offset;
            return element;
        }
    }
    LogNoBackgroundOnce(context, control, outerBegin, types);
    return nullptr;
}

static WotbModResult WOTBMOD_CALL UiGetBackgroundColor(
    void* userData,
    void* nativeResource,
    float* outRgba) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !outRgba) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    __try {
        size_t offset = 0u;
        const uint8_t* component = FindUiBackgroundComponent(
            context,
            static_cast<const uint8_t*>(resource->native_object),
            &offset);
        if (!component) return WOTBMOD_ERROR_NOT_FOUND;
        memcpy(outRgba, component + offset, 4u * sizeof(float));
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL UiSetBackgroundColor(
    void* userData,
    void* nativeResource,
    const float* rgba) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource || !rgba) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    for (size_t i = 0u; i < 4u; ++i) {
        if (!(rgba[i] >= 0.0f && rgba[i] <= 1.0f)) {
            return WOTBMOD_ERROR_INVALID_ARGUMENT;
        }
    }
    __try {
        size_t offset = 0u;
        uint8_t* component = FindUiBackgroundComponent(
            context,
            static_cast<const uint8_t*>(resource->native_object),
            &offset);
        if (!component) return WOTBMOD_ERROR_NOT_FOUND;
        memcpy(component + offset, rgba, 4u * sizeof(float));
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

/* ".?AVUIStaticText@DAVA@@" -> "DAVA::UIStaticText"; anything unparsable is
 * copied as-is. */
static void PrettyRttiName(const char* raw, char* out, size_t capacity) {
    out[0] = '\0';
    if (!raw || capacity < 2u) return;
    const char* body = raw;
    if (body[0] == '.' && body[1] == '?' && body[2] == 'A' && body[3] != '\0') body += 4;
    /* segments are separated by '@' and listed innermost first */
    const char* segments[8] = {};
    size_t count = 0u;
    const char* cursor = body;
    while (*cursor && count < 8u) {
        segments[count++] = cursor;
        const char* end = cursor;
        while (*end && *end != '@') ++end;
        if (*end != '@') { cursor = end; break; }
        cursor = end + 1;
        if (*cursor == '@') break;
    }
    size_t used = 0u;
    for (size_t i = count; i > 0u; --i) {
        const char* seg = segments[i - 1u];
        while (*seg && *seg != '@' && used + 1u < capacity) out[used++] = *seg++;
        if (i > 1u && used + 3u < capacity) { out[used++] = ':'; out[used++] = ':'; }
    }
    out[used] = '\0';
}

static WotbModResult WOTBMOD_CALL UiGetIdentity(
    void* userData,
    void* nativeResource,
    char* name,
    uint32_t nameCapacity,
    char* className,
    uint32_t classCapacity) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (name && nameCapacity) name[0] = '\0';
    if (className && classCapacity) className[0] = '\0';
    __try {
        const uint8_t* control = static_cast<const uint8_t*>(resource->native_object);
        if (name && nameCapacity > 1u &&
            IsReadableRange(control + kUiNameOffset, sizeof(void*))) {
            const char* text = *reinterpret_cast<const char* const*>(control + kUiNameOffset);
            if (text && IsReadableRange(text, 1u)) {
                uint32_t n = 0u;
                while (n + 1u < nameCapacity && n < 127u &&
                       IsReadableRange(text + n, 1u) && text[n] != '\0' &&
                       static_cast<unsigned char>(text[n]) >= 0x20u) {
                    name[n] = text[n];
                    ++n;
                }
                name[n] = '\0';
            }
        }
        if (className && classCapacity > 1u) {
            char raw[96] = {};
            RttiClassName(context, control, raw, sizeof(raw));
            PrettyRttiName(raw, className, classCapacity);
        }
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult UiSetFlag(
    void* userData,
    void* nativeResource,
    int32_t value,
    int32_t hierarchical,
    size_t vtableIndex) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context,
        nativeResource,
        WOTBMOD_RESOURCE_UI_CONTROL);
    if (!resource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    UiSetFlagFn setFlag = VirtualFunction<UiSetFlagFn>(
        resource->native_object, vtableIndex);
    if (!setFlag) return WOTBMOD_ERROR_PLATFORM;
    __try {
        setFlag(
            resource->native_object,
            value != 0,
            hierarchical != 0);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL UiSetInputEnabled(
    void* userData,
    void* nativeResource,
    int32_t enabled,
    int32_t hierarchical) {
    return UiSetFlag(
        userData,
        nativeResource,
        enabled,
        hierarchical,
        kUiSetInputEnabledVtableIndex);
}

static WotbModResult WOTBMOD_CALL UiSetDisabled(
    void* userData,
    void* nativeResource,
    int32_t disabled,
    int32_t hierarchical) {
    return UiSetFlag(
        userData,
        nativeResource,
        disabled,
        hierarchical,
        kUiSetDisabledVtableIndex);
}

static WotbModResult UiControlPairOperation(
    void* userData,
    void* parentNativeResource,
    void* childNativeResource,
    size_t vtableIndex) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* parent = TypedObjectResource(
        context, parentNativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    DavaResource* child = TypedObjectResource(
        context, childNativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!parent || !child || parent == child ||
        parent->native_object == child->native_object) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    ObjectPairFn operation = VirtualFunction<ObjectPairFn>(
        parent->native_object, vtableIndex);
    if (!operation) return WOTBMOD_ERROR_PLATFORM;
    __try {
        operation(parent->native_object, child->native_object);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL UiAddChild(
    void* userData,
    void* parentNativeResource,
    void* childNativeResource) {
    return UiControlPairOperation(
        userData,
        parentNativeResource,
        childNativeResource,
        kUiAddControlVtableIndex);
}

static WotbModResult WOTBMOD_CALL UiRemoveChild(
    void* userData,
    void* parentNativeResource,
    void* childNativeResource) {
    return UiControlPairOperation(
        userData,
        parentNativeResource,
        childNativeResource,
        kUiRemoveControlVtableIndex);
}

static WotbModResult WOTBMOD_CALL SceneSetTransform(
    void* userData,
    void* nativeResource,
    const WotbModSceneTransform* transform) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* resource = TypedObjectResource(
        context, nativeResource, WOTBMOD_RESOURCE_SCENE);
    TransformSetLocalTransformFn setTransform =
        context ? reinterpret_cast<TransformSetLocalTransformFn>(Address(
                      context,
                      context->transform_set_local_transform_rva))
                : nullptr;
    if (!resource || !transform) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!setTransform ||
        !IsExecutableAddress(reinterpret_cast<const void*>(setTransform))) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    void* transformComponent = nullptr;
    __try {
        transformComponent = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(resource->native_object) +
            kEntityTransformComponentOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (!transformComponent ||
        !ValidateObject(context, transformComponent, 0)) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    const DavaTransform32 nativeTransform = {
        {
            transform->position_x,
            transform->position_y,
            transform->position_z,
        },
        {
            transform->scale_x,
            transform->scale_y,
            transform->scale_z,
        },
        {
            transform->rotation_x,
            transform->rotation_y,
            transform->rotation_z,
            transform->rotation_w,
        },
    };
    __try {
        setTransform(transformComponent, &nativeTransform);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult ScenePairOperation(
    void* userData,
    void* parentNativeResource,
    void* childNativeResource,
    size_t vtableIndex) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaResource* parent = TypedObjectResource(
        context, parentNativeResource, WOTBMOD_RESOURCE_SCENE);
    DavaResource* child = TypedObjectResource(
        context, childNativeResource, WOTBMOD_RESOURCE_SCENE);
    if (!parent || !child || parent == child ||
        parent->native_object == child->native_object) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    ObjectPairFn operation = VirtualFunction<ObjectPairFn>(
        parent->native_object, vtableIndex);
    if (!operation) return WOTBMOD_ERROR_PLATFORM;
    __try {
        operation(parent->native_object, child->native_object);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

static WotbModResult WOTBMOD_CALL SceneAddChild(
    void* userData,
    void* parentNativeResource,
    void* childNativeResource) {
    return ScenePairOperation(
        userData,
        parentNativeResource,
        childNativeResource,
        kEntityAddNodeVtableIndex);
}

static WotbModResult WOTBMOD_CALL SceneRemoveChild(
    void* userData,
    void* parentNativeResource,
    void* childNativeResource) {
    return ScenePairOperation(
        userData,
        parentNativeResource,
        childNativeResource,
        kEntityRemoveNodeVtableIndex);
}

static WotbModResult WOTBMOD_CALL UiCreate(
    void* userData,
    const WotbModUiControlGeometry* geometry,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;

    void* control = nullptr;
    WotbModResult result = CreateNativeObject(
        context,
        kUiControlSize,
        context->ui_control_ctor_rva,
        context->ui_control_vtable_rva,
        &control);
    if (result != WOTBMOD_OK) return result;

    if (geometry) {
        UiSetVectorFn setPosition =
            VirtualFunction<UiSetVectorFn>(
                control, kUiSetPositionVtableIndex);
        UiSetVectorFn setSize =
            VirtualFunction<UiSetVectorFn>(
                control, kUiSetSizeVtableIndex);
        if (!setPosition || !setSize) {
            InvokeRelease(context, control);
            return WOTBMOD_ERROR_PLATFORM;
        }
        const DavaVector2 position = {
            geometry->x, geometry->y};
        const DavaVector2 size = {
            geometry->width, geometry->height};
        __try {
            setPosition(control, &position);
            setSize(control, &size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InvokeRelease(context, control);
            return WOTBMOD_ERROR_CALLBACK_FAULT;
        }
    }

    DavaResource* resource = AllocateResource(
        context,
        WOTBMOD_RESOURCE_UI_CONTROL,
        DAVA_RESOURCE_CREATED,
        control);
    if (!resource) {
        InvokeRelease(context, control);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    *outNativeResource = resource;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiGetActiveScreen(
    void* userData,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;

    GetEngineContextFn getEngineContext =
        reinterpret_cast<GetEngineContextFn>(Address(
            context, context->get_engine_context_rva));
    UiGetScreenFn getScreen = reinterpret_cast<UiGetScreenFn>(Address(
        context, context->ui_control_system_get_screen_rva));
    if (!getEngineContext || !getScreen ||
        !IsExecutableAddress(
            reinterpret_cast<const void*>(getEngineContext)) ||
        !IsExecutableAddress(reinterpret_cast<const void*>(getScreen))) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    void* screen = nullptr;
    __try {
        void* engineContext = getEngineContext();
        if (!engineContext) return WOTBMOD_ERROR_NOT_FOUND;
        void* uiControlSystem = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(engineContext) +
            context->ui_control_system_offset);
        if (!uiControlSystem) return WOTBMOD_ERROR_NOT_FOUND;
        screen = getScreen(uiControlSystem);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (!screen) return WOTBMOD_ERROR_NOT_FOUND;
    if (!ValidateObject(context, screen, 0) ||
        !InvokeRetain(context, screen)) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    DavaResource* resource = AllocateResource(
        context,
        WOTBMOD_RESOURCE_UI_CONTROL,
        DAVA_RESOURCE_ACQUIRED,
        screen);
    if (!resource) {
        InvokeRelease(context, screen);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    *outNativeResource = resource;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SceneEntityCreate(
    void* userData,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;

    void* entity = nullptr;
    WotbModResult result = CreateNativeObject(
        context,
        kEntitySize,
        context->entity_ctor_rva,
        context->entity_vtable_rva,
        &entity);
    if (result != WOTBMOD_OK) return result;

    DavaResource* resource = AllocateResource(
        context,
        WOTBMOD_RESOURCE_SCENE,
        DAVA_RESOURCE_CREATED,
        entity);
    if (!resource) {
        InvokeRelease(context, entity);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    *outNativeResource = resource;
    return WOTBMOD_OK;
}

/* ------------------------------------- the live scene's own materials ---- */

/*
 * Reaching a material the GAME owns, rather than one this mod created.
 *
 * This is what the built-in camouflages were missing. Every other link already
 * existed and was live-verified: the material mutators, entity ->
 * GetRenderObject -> GetRenderBatch, and the active scene. What did not exist
 * was a way to name a node in that scene and take its material - and both
 * published routes to a vehicle part answer an unconditional refusal
 * (`vehicle_get_part_entity` and `set_custom_camouflage` both end in
 * VehicleVisualUnavailable, src/v3/client_services.cpp:12059 and :12127).
 *
 * OWNERSHIP IS THE SUBTLE PART. `CreateNativeMaterial` hands out a token for a
 * material it just constructed, so the reference it holds is its own. Here the
 * material belongs to the client's scene, and the token's eventual release
 * would decrement a count this mod never incremented - a use-after-free the
 * moment the game drops its own reference. So the material is RETAINED before
 * the token is minted, and the release path then balances what was added.
 */
static bool ReadEntityName(
    DavaResourcesContext* context,
    void* entity,
    void** outName) {
    if (!context || !entity || !outName) return false;
    *outName = nullptr;
    if (!ValidateObject(context, entity, 0u)) return false;
    __try {
        *outName = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(entity) + kEntityNameOffset);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadBatchMaterial(
    DavaResourcesContext* context,
    void* batch,
    void** outMaterial) {
    if (!context || !batch || !outMaterial) return false;
    *outMaterial = nullptr;
    if (!ValidateObject(
            context, batch, context->render_batch_vtable_rva)) {
        return false;
    }
    __try {
        void* material = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(batch) + kRenderBatchMaterialOffset);
        /*
         * A batch with no material is legal and answers success with nullptr;
         * a batch whose material is not an NMaterial is a layout that moved,
         * and that answers failure rather than handing out the pointer.
         */
        if (material &&
            !ValidateObject(
                context, material, context->nmaterial_vtable_rva)) {
            return false;
        }
        *outMaterial = material;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/*
 * The material of one node's chosen render batch.
 *
 * `batchIndex` selects among a node's batches - a tank part is usually one, but
 * a node with several is normal and the caller may want a specific one. An
 * index past the end is NOT_FOUND rather than a silent fallback to batch 0: a
 * caller that asked for the second batch and got the first would paint the
 * wrong thing and never know.
 */
static bool EntityBatchMaterial(
    DavaResourcesContext* context,
    void* entity,
    uint32_t batchIndex,
    void** outMaterial) {
    if (!context || !entity || !outMaterial) return false;
    *outMaterial = nullptr;
    void* renderObject = nullptr;
    if (!InvokeEntityGetRenderObject(context, entity, &renderObject) ||
        !renderObject) {
        return false;
    }
    uint32_t batchCount = 0u;
    if (!ValidateObject(context, renderObject, 0u) ||
        !ReadRenderBatchCount(renderObject, &batchCount) ||
        batchIndex >= batchCount) {
        return false;
    }
    void* batch = nullptr;
    if (!InvokeRenderObjectGetBatch(
            context, renderObject, batchIndex, &batch) ||
        !batch) {
        return false;
    }
    void* material = nullptr;
    if (!ReadBatchMaterial(context, batch, &material) || !material) {
        return false;
    }
    *outMaterial = material;
    return true;
}

/*
 * Walks the scene for a node and takes its material.
 *
 * `wantedName` of nullptr means "the first node that has a material at all",
 * which is what a caller uses before it knows what the hangar calls anything.
 * The walk is bounded on depth and on nodes visited for the same reason every
 * other traversal in this file is: a cyclic or corrupted graph must cost a
 * bounded amount of stack and time, not the process.
 */
static bool FindSceneMaterial(
    DavaResourcesContext* context,
    void* entity,
    void* wantedName,
    uint32_t batchIndex,
    uint32_t depth,
    uint32_t* visited,
    void** outMaterial) {
    if (!context || !entity || !outMaterial || !visited ||
        depth > kMaxMeshTraversalDepth ||
        *visited >= kMaxSceneNodesVisited ||
        !ValidateObject(context, entity, 0u)) {
        return false;
    }
    ++*visited;

    bool matches = wantedName == nullptr;
    if (!matches) {
        void* name = nullptr;
        if (ReadEntityName(context, entity, &name) && name == wantedName) {
            matches = true;
        }
    }
    if (matches &&
        EntityBatchMaterial(context, entity, batchIndex, outMaterial)) {
        return true;
    }
    /*
     * A NAMED node that matched but carries no material still lets the walk
     * continue into its children, because a tank part is often a named group
     * whose geometry sits one level down. An unnamed search never stops early
     * for the same reason.
     */

    void** childrenBegin = nullptr;
    void** childrenEnd = nullptr;
    if (!ReadPointerVector(
            entity,
            kEntityChildrenBeginOffset,
            kEntityChildrenEndOffset,
            &childrenBegin,
            &childrenEnd)) {
        return false;
    }
    const uintptr_t childCount = childrenBegin
        ? static_cast<uintptr_t>(childrenEnd - childrenBegin)
        : 0u;
    if (childCount > kMaxMeshChildrenPerEntity) return false;
    for (uintptr_t index = 0u; index < childCount; ++index) {
        void* child = nullptr;
        __try {
            child = childrenBegin[index];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (!child) continue;
        if (FindSceneMaterial(
                context, child, wantedName, batchIndex,
                depth + 1u, visited, outMaterial)) {
            return true;
        }
    }
    return false;
}

/*
 * The active scene, validated, WITHOUT taking a resource handle for it.
 *
 * `SceneGetActive` below does the same validation and then allocates a
 * DavaResource, because its caller gets a handle back. These two want the
 * pointer for the duration of one call and must not leak a resource to get it,
 * so the shared part is here and the retain is the caller's business.
 */
static void* AcquireActiveScene(DavaResourcesContext* context) {
    if (!context) return nullptr;
    void* scene = InterlockedCompareExchangePointer(
        &context->active_scene, nullptr, nullptr);
    if (!scene) return nullptr;
    if (!HasVirtualTarget(
            context,
            scene,
            kSceneDrawVtableIndex,
            context->scene_draw_rva) ||
        !IsSceneActive(scene)) {
        return nullptr;
    }
    return scene;
}

static WotbModV3Result WOTBMOD_CALL SceneMaterialCreate(
    void* userData,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeProviderToken* outProviderToken);
static WotbModV3Result WOTBMOD_CALL SceneNodesList(
    void* userData,
    char* buffer,
    uint32_t* inoutSize);

static WotbModV3Result WOTBMOD_CALL SceneMaterialCreate(
    void* userData,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeProviderToken* outProviderToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE || !request ||
        !outProviderToken ||
        request->struct_size < sizeof(*request)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outProviderToken = 0u;
    if (!memchr(request->node_name, '\0', sizeof(request->node_name))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    /*
     * MAIN THREAD OR NOTHING, and this is not pedantry - it is the same reason
     * every other walk in this file hops. AddNode/RemoveNode memmove the child
     * vector with no lock and Release the child immediately after compacting,
     * so a walk on any other thread can read a pointer that is freed one
     * instruction later. The first version of this function did NOT hop; it
     * happened to work in a live probe, which is exactly how this class of bug
     * survives to ship.
     */
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return SceneMaterialCreate(
                userData, owner, request, outProviderToken);
        };
        return InvokeDavaMainThread(context, call);
    }

    void* scene = AcquireActiveScene(context);
    if (!scene) return WOTBMOD_V3_E_NOT_FOUND;

    FastNameScope wanted = {};
    const bool named = request->node_name[0] != '\0';
    if (named && !MakeFastName(context, request->node_name, &wanted)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }

    uint32_t visited = 0u;
    void* material = nullptr;
    const bool found = FindSceneMaterial(
        context,
        scene,
        named ? wanted.value : nullptr,
        request->batch_index,
        0u,
        &visited,
        &material);
    if (named) ReleaseFastName(context, &wanted);
    if (!found || !material) return WOTBMOD_V3_E_NOT_FOUND;

    /* See the ownership note above: the reference is added BEFORE the token
     * exists, so there is no window in which a release could outrun it. */
    if (!InvokeRetain(context, material)) return WOTBMOD_V3_E_PLATFORM;

    DavaNativeRecord* record = AllocateNativeRecord(
        context, owner, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL);
    if (!record) {
        (void)InvokeRelease(context, material);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    record->native_object = material;
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

/*
 * The same walk as FindSceneMaterial, ending one link earlier: the RENDER
 * BATCH itself instead of the material it carries. This is what makes a live
 * geometry swap possible - `mesh_hot_swap` needs a batch to re-point at a
 * replacement polygon group, and until now only file-loaded batches could be
 * reached. The batch is validated against the exact-build RenderBatch vtable,
 * so a wrong node can answer NOT_FOUND but never a foreign object.
 */
static bool EntityRenderBatch(
    DavaResourcesContext* context,
    void* entity,
    uint32_t batchIndex,
    void** outBatch) {
    if (!context || !entity || !outBatch) return false;
    *outBatch = nullptr;
    void* renderObject = nullptr;
    if (!InvokeEntityGetRenderObject(context, entity, &renderObject) ||
        !renderObject) {
        return false;
    }
    uint32_t batchCount = 0u;
    if (!ValidateObject(context, renderObject, 0u) ||
        !ReadRenderBatchCount(renderObject, &batchCount) ||
        batchIndex >= batchCount) {
        return false;
    }
    void* batch = nullptr;
    if (!InvokeRenderObjectGetBatch(
            context, renderObject, batchIndex, &batch) ||
        !batch) {
        return false;
    }
    if (!ValidateObject(
            context, batch, context->render_batch_vtable_rva)) {
        return false;
    }
    *outBatch = batch;
    return true;
}

static bool FindSceneRenderBatch(
    DavaResourcesContext* context,
    void* entity,
    void* wantedName,
    uint32_t batchIndex,
    uint32_t depth,
    uint32_t* visited,
    void** outBatch) {
    if (!context || !entity || !outBatch || !visited ||
        depth > kMaxMeshTraversalDepth ||
        *visited >= kMaxSceneNodesVisited ||
        !ValidateObject(context, entity, 0u)) {
        return false;
    }
    ++*visited;

    bool matches = wantedName == nullptr;
    if (!matches) {
        void* name = nullptr;
        if (ReadEntityName(context, entity, &name) && name == wantedName) {
            matches = true;
        }
    }
    if (matches &&
        EntityRenderBatch(context, entity, batchIndex, outBatch)) {
        return true;
    }

    void** childrenBegin = nullptr;
    void** childrenEnd = nullptr;
    if (!ReadPointerVector(
            entity,
            kEntityChildrenBeginOffset,
            kEntityChildrenEndOffset,
            &childrenBegin,
            &childrenEnd)) {
        return false;
    }
    const uintptr_t childCount = childrenBegin
        ? static_cast<uintptr_t>(childrenEnd - childrenBegin)
        : 0u;
    if (childCount > kMaxMeshChildrenPerEntity) return false;
    for (uintptr_t index = 0u; index < childCount; ++index) {
        void* child = nullptr;
        __try {
            child = childrenBegin[index];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (!child) continue;
        if (FindSceneRenderBatch(
                context, child, wantedName, batchIndex,
                depth + 1u, visited, outBatch)) {
            return true;
        }
    }
    return false;
}

static WotbModV3Result WOTBMOD_V3_CALL SceneBatchCreate(
    void* userData,
    WotbModV3Handle owner,
    const WotbModDavaNativeSceneMaterialRequest* request,
    WotbModDavaNativeProviderToken* outProviderToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE || !request ||
        !outProviderToken ||
        request->struct_size < sizeof(*request)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outProviderToken = 0u;
    if (!memchr(request->node_name, '\0', sizeof(request->node_name))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    /* Main-thread-or-nothing: same reason as SceneMaterialCreate above. */
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return SceneBatchCreate(
                userData, owner, request, outProviderToken);
        };
        return InvokeDavaMainThread(context, call);
    }

    void* scene = AcquireActiveScene(context);
    if (!scene) return WOTBMOD_V3_E_NOT_FOUND;

    FastNameScope wanted = {};
    const bool named = request->node_name[0] != '\0';
    if (named && !MakeFastName(context, request->node_name, &wanted)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }

    uint32_t visited = 0u;
    void* batch = nullptr;
    const bool found = FindSceneRenderBatch(
        context,
        scene,
        named ? wanted.value : nullptr,
        request->batch_index,
        0u,
        &visited,
        &batch);
    if (named) ReleaseFastName(context, &wanted);
    if (!found || !batch) return WOTBMOD_V3_E_NOT_FOUND;

    /*
     * The batch rides the render object's reference count. Retain BEFORE the
     * token exists, exactly like SceneMaterialCreate: a token that hands out
     * something the mod never took a reference on cannot be released honestly.
     */
    if (!InvokeRetain(context, batch)) return WOTBMOD_V3_E_PLATFORM;

    DavaNativeRecord* record = AllocateNativeRecord(
        context, owner, WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER);
    if (!record) {
        (void)InvokeRelease(context, batch);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    record->native_object = batch;
    /*
     * mesh_hot_swap consumes related_object and release frees native_object.
     * Both name the SAME retained batch here, so a swap re-points live
     * geometry and a later release lets go of the one reference taken above -
     * never two.
     */
    record->related_object = batch;
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

/*
 * Every node name under the active scene, newline separated.
 *
 * This exists because the names are not knowable from the client's files: a
 * mod that wants to paint the hangar tank has to be told what the hangar calls
 * it, once, by looking. Nodes with no name are skipped rather than reported as
 * an empty line - an empty name is not something a caller can ask for.
 */
/*
 * Reading a node's NAME as text, with every raw dereference inside SEH.
 *
 * Split out because MSVC refuses __try in a function that also has objects
 * needing unwinding, and the walk below builds a std::string. Splitting is not
 * a workaround here - it is the shape the rule wants: the unsafe reads are one
 * short function with no destructors in sight.
 */
static bool CopyEntityNameText(
    DavaResourcesContext* context,
    void* entity,
    char* out,
    size_t capacity) {
    if (!out || capacity == 0u) return false;
    out[0] = '\0';
    void* name = nullptr;
    if (!ReadEntityName(context, entity, &name) || !name) return false;
    __try {
        const char* source = static_cast<const char*>(name);
        size_t index = 0u;
        for (; index + 1u < capacity; ++index) {
            const char ch = source[index];
            if (ch == '\0') break;
            /* Only printable ASCII is reported: the name is read through a
             * derived offset, and anything else means the derivation is wrong
             * on this build rather than that the node is oddly named. */
            if (ch < 0x20 || ch > 0x7E) { out[0] = '\0'; return false; }
            out[index] = ch;
        }
        out[index] = '\0';
        return out[0] != '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

/* One child pointer, read under SEH for the same reason. */
static bool ReadChildAt(void** begin, uintptr_t index, void** out) {
    if (!begin || !out) return false;
    *out = nullptr;
    __try {
        *out = begin[index];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/*
 * The scene as a TREE, one line per node: depth|children|batches|name.
 *
 * The first version emitted names only and skipped a node whose name was empty.
 * A live run then printed exactly ONE line - `hv_main.sc2` - where a hangar has
 * hundreds of nodes, and there was no way to tell which of three very different
 * things had happened: the children vector read as empty, the walk stopped at
 * its first child, or every child simply had no name and was dropped in
 * silence.
 *
 * Three explanations and no way to choose between them is a measurement that
 * was not made. So nothing is filtered now. An unnamed node is reported AS
 * unnamed, a node that fails validation is reported as unreadable - that one is
 * the single most useful line this dump can produce, because it is where a walk
 * that looks empty actually stopped - and every line carries the two numbers
 * that tell the shapes apart: how many children the node has, and how many
 * render batches. A tank is a node with batches; a node with children and none
 * is scaffolding to descend through.
 */
static void CollectSceneNodes(
    DavaResourcesContext* context,
    void* entity,
    uint32_t depth,
    uint32_t* visited,
    std::string* out) {
    if (!context || !entity || !out || !visited ||
        depth > kMaxMeshTraversalDepth ||
        *visited >= kMaxSceneNodesDumped) {
        return;
    }
    if (!ValidateObject(context, entity, 0u)) {
        if (!out->empty()) out->push_back('\n');
        out->append(std::to_string(depth));
        out->append("|?|?|<unreadable>");
        return;
    }
    ++*visited;

    char text[kMaxObjectName] = {};
    const bool named = CopyEntityNameText(context, entity, text, sizeof(text));

    uint32_t batches = 0u;
    void* renderObject = nullptr;
    if (InvokeEntityGetRenderObject(context, entity, &renderObject) &&
        renderObject && ValidateObject(context, renderObject, 0u)) {
        (void)ReadRenderBatchCount(renderObject, &batches);
    }

    void** childrenBegin = nullptr;
    void** childrenEnd = nullptr;
    const bool readChildren = ReadPointerVector(
        entity,
        kEntityChildrenBeginOffset,
        kEntityChildrenEndOffset,
        &childrenBegin,
        &childrenEnd);
    const uintptr_t childCount =
        readChildren && childrenBegin
            ? static_cast<uintptr_t>(childrenEnd - childrenBegin)
            : 0u;

    if (!out->empty()) out->push_back('\n');
    out->append(std::to_string(depth));
    out->push_back('|');
    out->append(readChildren ? std::to_string(childCount) : std::string("?"));
    out->push_back('|');
    out->append(std::to_string(batches));
    out->push_back('|');
    out->append(named ? text : "<unnamed>");

    if (!readChildren) return;
    if (childCount > kMaxMeshChildrenPerEntity) return;
    for (uintptr_t index = 0u; index < childCount; ++index) {
        void* child = nullptr;
        if (!ReadChildAt(childrenBegin, index, &child)) return;
        if (child) {
            CollectSceneNodes(context, child, depth + 1u, visited, out);
        }
    }
}


static WotbModV3Result WOTBMOD_CALL SceneNodesList(
    void* userData,
    char* buffer,
    uint32_t* inoutSize) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || !inoutSize) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    /* The same walk, so the same rule - see SceneMaterialCreate. */
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return SceneNodesList(userData, buffer, inoutSize);
        };
        return InvokeDavaMainThread(context, call);
    }
    void* scene = AcquireActiveScene(context);
    if (!scene) return WOTBMOD_V3_E_NOT_FOUND;

    std::string names;
    uint32_t visited = 0u;
    CollectSceneNodes(context, scene, 0u, &visited, &names);

    const uint32_t required = static_cast<uint32_t>(names.size() + 1u);
    if (!buffer || *inoutSize < required) {
        *inoutSize = required;
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }
    memcpy(buffer, names.c_str(), names.size() + 1u);
    *inoutSize = required;
    return WOTBMOD_V3_OK;
}

static WotbModResult WOTBMOD_CALL SceneGetActive(
    void* userData,
    void** outNativeResource) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeResource = nullptr;

    void* scene = InterlockedCompareExchangePointer(
        &context->active_scene, nullptr, nullptr);
    if (!scene) return WOTBMOD_ERROR_NOT_FOUND;
    if (!HasVirtualTarget(
            context,
            scene,
            kSceneDrawVtableIndex,
            context->scene_draw_rva) ||
        !IsSceneActive(scene) ||
        !InvokeRetain(context, scene)) {
        return WOTBMOD_ERROR_NOT_FOUND;
    }

    DavaResource* resource = AllocateResource(
        context,
        WOTBMOD_RESOURCE_SCENE,
        DAVA_RESOURCE_ACQUIRED,
        scene);
    if (!resource) {
        InvokeRelease(context, scene);
        return WOTBMOD_ERROR_LIMIT_REACHED;
    }
    *outNativeResource = resource;
    return WOTBMOD_OK;
}

static WotbModV3Result LegacyResultToV3(WotbModResult result) {
    switch (result) {
        case WOTBMOD_OK:
            return WOTBMOD_V3_OK;
        case WOTBMOD_ERROR_INVALID_ARGUMENT:
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        case WOTBMOD_ERROR_UNSUPPORTED_ABI:
            return WOTBMOD_V3_E_NOT_SUPPORTED;
        case WOTBMOD_ERROR_NOT_FOUND:
            return WOTBMOD_V3_E_NOT_FOUND;
        case WOTBMOD_ERROR_ALREADY_EXISTS:
            return WOTBMOD_V3_E_ALREADY_EXISTS;
        case WOTBMOD_ERROR_ACCESS_DENIED:
            return WOTBMOD_V3_E_PERMISSION_DENIED;
        case WOTBMOD_ERROR_BUFFER_TOO_SMALL:
            return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
        case WOTBMOD_ERROR_DISABLED:
            return WOTBMOD_V3_E_CANCELLED;
        case WOTBMOD_ERROR_LIMIT_REACHED:
            return WOTBMOD_V3_E_LIMIT_REACHED;
        case WOTBMOD_ERROR_CALLBACK_FAULT:
            return WOTBMOD_V3_E_CALLBACK_FAULT;
        case WOTBMOD_ERROR_WRONG_THREAD:
            return WOTBMOD_V3_E_WRONG_THREAD;
        case WOTBMOD_ERROR_PLATFORM:
        default:
            return WOTBMOD_V3_E_PLATFORM;
    }
}

static WotbModV3Result WOTBMOD_V3_CALL DavaYamlParseFile(
    void* userData,
    WotbModV3Handle owner,
    const char* resolvedFilePath,
    WotbModDavaNativeProviderToken* outProviderToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE ||
        !resolvedFilePath || !resolvedFilePath[0] || !outProviderToken ||
        strlen(resolvedFilePath) >= WOTBMOD_MAX_RESOURCE_PATH) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outProviderToken = 0u;
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaYamlParseFile(
                userData, owner, resolvedFilePath, outProviderToken);
        };
        return InvokeDavaMainThread(context, call);
    }

    uint8_t* bytes = nullptr;
    uint32_t byteCount = 0u;
    WotbModV3Result read = ReadNativeFileBytes(
        resolvedFilePath,
        kMaxNativeYamlBytes,
        &bytes,
        &byteCount);
    if (read != WOTBMOD_V3_OK) return read;

    char davaPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    BuildDavaPath(context, resolvedFilePath, davaPath, sizeof(davaPath));
    DavaFilePath32 path(davaPath);
    DavaYamlParserResult32 parsed = {};
    const bool invoked = InvokeYamlParseFile(context, &path, &parsed);
    /* YamlNode is RefCounted but non-polymorphic in this client build. A
     * vtable check rejects a valid parser result whose first dword is the
     * reference counter, so validate readable storage rather than dispatch. */
    const bool rootReadable = IsReadableRange(parsed.root, sizeof(uint32_t));
    char diagnostic[1400] = {};
    _snprintf_s(
        diagnostic,
        sizeof(diagnostic),
        _TRUNCATE,
        "DAVA YAML parse invoked=%u parsed=%u root=%p "
        "root-readable=%u path-type=%d path=%s",
        invoked ? 1u : 0u,
        static_cast<unsigned int>(parsed.parsed),
        parsed.root,
        rootReadable ? 1u : 0u,
        static_cast<int>(path.path_type),
        davaPath);
    Log(
        context,
        invoked && parsed.parsed != 0u && rootReadable
            ? WOTBMOD_LOG_TRACE
            : WOTBMOD_LOG_ERROR,
        diagnostic);
    if (!invoked || parsed.parsed == 0u || !parsed.root || !rootReadable) {
        if (parsed.root) (void)InvokeRelease(context, parsed.root);
        HeapFree(GetProcessHeap(), 0, bytes);
        return invoked ? WOTBMOD_V3_E_PARSE
                       : WOTBMOD_V3_E_CALLBACK_FAULT;
    }

    DavaNativeRecord* record = AllocateNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT);
    if (!record) {
        (void)InvokeRelease(context, parsed.root);
        HeapFree(GetProcessHeap(), 0, bytes);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    record->native_object = parsed.root;
    record->bytes = bytes;
    record->byte_count = byteCount;
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaYamlExportUtf8(
    void* userData,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken document,
    WotbModDavaNativeBuffer* inoutUtf8Yaml) {
    DavaResourcesContext* context = ValidContext(userData);
    DavaNativeRecord* record = ValidNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT,
        document);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    return CopyNativeBuffer(
        record->bytes, record->byte_count, inoutUtf8Yaml);
}

static WotbModV3Result WOTBMOD_V3_CALL DavaArchiveOpenFile(
    void* userData,
    WotbModV3Handle owner,
    const char* resolvedFilePath,
    WotbModDavaNativeProviderToken* outProviderToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE ||
        !resolvedFilePath || !resolvedFilePath[0] || !outProviderToken ||
        strlen(resolvedFilePath) >= WOTBMOD_MAX_RESOURCE_PATH) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outProviderToken = 0u;
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaArchiveOpenFile(
                userData, owner, resolvedFilePath, outProviderToken);
        };
        return InvokeDavaMainThread(context, call);
    }
    uint64_t fileSize = 0u;
    if (!ReadFileSize(resolvedFilePath, &fileSize)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }

    DavaNativeRecord* record = AllocateNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE);
    if (!record) return WOTBMOD_V3_E_LIMIT_REACHED;

    char davaPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    BuildDavaPath(context, resolvedFilePath, davaPath, sizeof(davaPath));
    DavaFilePath32 path(davaPath);
    if (!InvokeArchiveCtor(context, &record->native_object, &path)) {
        if (record->native_object) {
            (void)InvokeArchiveDtor(context, &record->native_object);
        }
        FreeNativeRecord(record);
        return WOTBMOD_V3_E_PARSE;
    }
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaArchiveGetEntryCount(
    void* userData,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t* outCount) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!outCount) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *outCount = 0u;
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaArchiveGetEntryCount(
                userData, owner, archive, outCount);
        };
        return InvokeDavaMainThread(context, call);
    }
    DavaNativeRecord* record = ValidNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE,
        archive);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;
    DavaRawVector32 files = {};
    if (!GetArchiveFilesInfo(context, record->native_object, &files)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(files.begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(files.end);
    *outCount = begin == 0u
        ? 0u
        : static_cast<uint32_t>(
              (end - begin) / kDavaArchiveFileInfoSize);
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaArchiveGetEntry(
    void* userData,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t index,
    WotbModDavaNativeArchiveEntry* outEntry) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!outEntry || outEntry->struct_size < sizeof(*outEntry)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaArchiveGetEntry(
                userData, owner, archive, index, outEntry);
        };
        return InvokeDavaMainThread(context, call);
    }
    DavaNativeRecord* record = ValidNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE,
        archive);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;

    DavaArchiveFileInfo32 file = {};
    if (!GetArchiveFileInfo(
            context, record->native_object, index, &file)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    WotbModDavaNativeArchiveEntry result = {};
    result.struct_size = sizeof(result);
    result.index = index;
    result.original_size = file.original_size;
    result.compressed_size = file.compressed_size;
    result.original_crc32 = file.original_crc32;
    result.compressed_crc32 = file.compressed_crc32;
    result.compression_type = file.compression_type;
    if (!CopyDavaString(
            &file.relative_path,
            result.relative_path,
            sizeof(result.relative_path))) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    *outEntry = result;
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaArchiveReadEntry(
    void* userData,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken archive,
    uint32_t index,
    WotbModDavaNativeBuffer* inoutData) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!inoutData || inoutData->struct_size < sizeof(*inoutData)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaArchiveReadEntry(
                userData, owner, archive, index, inoutData);
        };
        return InvokeDavaMainThread(context, call);
    }
    DavaNativeRecord* record = ValidNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE,
        archive);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;

    DavaArchiveFileInfo32 file = {};
    if (!GetArchiveFileInfo(
            context, record->native_object, index, &file)) {
        return WOTBMOD_V3_E_NOT_FOUND;
    }
    inoutData->size = file.original_size;
    if (file.original_size == 0u) return WOTBMOD_V3_OK;
    if (!inoutData->data || inoutData->capacity < file.original_size) {
        return WOTBMOD_V3_E_BUFFER_TOO_SMALL;
    }

    char relativePath[WOTBMOD_V3_MAX_PATH] = {};
    if (!CopyDavaString(
            &file.relative_path, relativePath, sizeof(relativePath))) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    DavaArchiveLookupPath32 davaRelativePath(relativePath);
    if (!davaRelativePath.IsValidFor(relativePath)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    DavaRawVector32 bytes = {};
    const bool loaded = InvokeArchiveLoadFile(
        context,
        record->native_object,
        &davaRelativePath,
        &bytes);

    const uintptr_t begin = reinterpret_cast<uintptr_t>(bytes.begin);
    const uintptr_t end = reinterpret_cast<uintptr_t>(bytes.end);
    const bool validRange = begin != 0u && end >= begin &&
        end - begin == file.original_size;
    const bool copied = loaded && validRange && CopyNativeBytes(
        inoutData->data, bytes.begin, file.original_size);
    const bool freed = FreeGameByteVector(context, &bytes);
    if (!loaded) return WOTBMOD_V3_E_IO;
    if (!validRange || !copied || !freed) return WOTBMOD_V3_E_PLATFORM;
    return WOTBMOD_V3_OK;
}

static WotbModV3Result CreateNativeMaterial(
    DavaResourcesContext* context,
    WotbModV3Handle owner,
    const char* materialName,
    WotbModDavaNativeProviderToken* outProviderToken) {
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE ||
        !materialName || !materialName[0] || !outProviderToken) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }

    FastNameScope name = {};
    if (!MakeFastName(context, materialName, &name)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    void* memory = nullptr;
    if (!InvokeOperatorNew(context, kNMaterialSize, &memory)) {
        ReleaseFastName(context, &name);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    void* material = nullptr;
    const bool constructed = InvokeNMaterialCtor(
        context, memory, &name, &material);
    if (!constructed || material != memory ||
        !ValidateObject(
            context, material, context->nmaterial_vtable_rva)) {
        if (constructed && material == memory) {
            (void)InvokeRelease(context, material);
        } else {
            (void)InvokeOperatorDelete(context, memory, kNMaterialSize);
        }
        ReleaseFastName(context, &name);
        return constructed ? WOTBMOD_V3_E_PLATFORM
                           : WOTBMOD_V3_E_CALLBACK_FAULT;
    }

    DavaNativeRecord* record = AllocateNativeRecord(
        context, owner, WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL);
    if (!record) {
        (void)InvokeRelease(context, material);
        ReleaseFastName(context, &name);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    record->native_object = material;
    ReleaseFastName(context, &name);
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

static WotbModV3Result CreateNativeTexture(
    DavaResourcesContext* context,
    WotbModV3Handle owner,
    const char* resolvedFilePath,
    WotbModDavaNativeProviderToken* outProviderToken) {
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE ||
        !resolvedFilePath || !resolvedFilePath[0] || !outProviderToken ||
        strlen(resolvedFilePath) >= WOTBMOD_MAX_RESOURCE_PATH) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    char davaPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    BuildDavaPath(context, resolvedFilePath, davaPath, sizeof(davaPath));
    DavaFilePath32 path(davaPath);
    FastNameScope group = {};
    if (!MakeFastName(context, "wotbmod.native.texture", &group)) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    void* texture = nullptr;
    const bool created = InvokeTextureCreateFromFile(
        context, &path, &group, &texture);
    ReleaseFastName(context, &group);
    if (!created || !ValidateObject(context, texture, 0u)) {
        if (texture) (void)InvokeRelease(context, texture);
        return created ? WOTBMOD_V3_E_PLATFORM : WOTBMOD_V3_E_IO;
    }

    DavaNativeRecord* record = AllocateNativeRecord(
        context, owner, WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE);
    if (!record) {
        (void)InvokeRelease(context, texture);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    record->native_object = texture;
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

static WotbModV3Result CreateNativeMeshObject(
    DavaResourcesContext* context,
    WotbModV3Handle owner,
    uint32_t objectKind,
    const char* resolvedFilePath,
    WotbModDavaNativeProviderToken* outProviderToken) {
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE ||
        (objectKind != WOTBMOD_DAVA_NATIVE_OBJECT_MESH &&
         objectKind != WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER) ||
        !resolvedFilePath || !resolvedFilePath[0] || !outProviderToken ||
        strlen(resolvedFilePath) >= WOTBMOD_MAX_RESOURCE_PATH) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outProviderToken = 0u;

    void* scene = nullptr;
    void* batch = nullptr;
    void* polygonGroup = nullptr;
    const WotbModResult loaded = LoadFirstMeshBatch(
        context,
        resolvedFilePath,
        &scene,
        &batch,
        &polygonGroup);
    if (loaded != WOTBMOD_OK) return LegacyResultToV3(loaded);

    void* ownedObject = scene;
    void* relatedObject = batch;
    if (objectKind == WOTBMOD_DAVA_NATIVE_OBJECT_MESH) {
        if (!InvokeRetain(context, polygonGroup)) {
            (void)InvokeRelease(context, scene);
            return WOTBMOD_V3_E_CALLBACK_FAULT;
        }
        if (!InvokeRelease(context, scene)) {
            (void)InvokeRelease(context, polygonGroup);
            return WOTBMOD_V3_E_CALLBACK_FAULT;
        }
        ownedObject = polygonGroup;
        relatedObject = nullptr;
    }

    DavaNativeRecord* record = AllocateNativeRecord(
        context, owner, objectKind);
    if (!record) {
        (void)InvokeRelease(context, ownedObject);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    record->native_object = ownedObject;
    record->related_object = relatedObject;
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaMeshHotSwap(
    void* userData,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken consumerToken,
    WotbModDavaNativeProviderToken replacementMeshToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaMeshHotSwap(
                userData, owner, consumerToken, replacementMeshToken);
        };
        return InvokeDavaMainThread(context, call);
    }
    DavaNativeRecord* consumer = ValidNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER,
        consumerToken);
    DavaNativeRecord* replacement = ValidNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_MESH,
        replacementMeshToken);
    if (!consumer || !replacement) return WOTBMOD_V3_E_INVALID_HANDLE;
    if (!consumer->native_object || !consumer->related_object ||
        !replacement->native_object) {
        return WOTBMOD_V3_E_PLATFORM;
    }
    return InvokeRenderBatchSetPolygonGroup(
               context,
               consumer->related_object,
               replacement->native_object)
        ? WOTBMOD_V3_OK
        : WOTBMOD_V3_E_CALLBACK_FAULT;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaTracerCreate(
    void* userData,
    WotbModV3Handle owner,
    const WotbModDavaNativeTracerRequest* request,
    WotbModDavaNativeProviderToken* outProviderToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE || !request ||
        request->struct_size < sizeof(*request) || !outProviderToken) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outProviderToken = 0u;
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaTracerCreate(
                userData, owner, request, outProviderToken);
        };
        return InvokeDavaMainThread(context, call);
    }
    if (!context->stock_tracer_create) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    WotbModV3Result created = WOTBMOD_V3_E_CALLBACK_FAULT;
    __try {
        created = context->stock_tracer_create(
            context->stock_tracer_user_data, request);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_V3_E_CALLBACK_FAULT;
    }
    if (created != WOTBMOD_V3_OK) return created;

    DavaNativeRecord* record = AllocateNativeRecord(
        context, owner, WOTBMOD_DAVA_NATIVE_OBJECT_TRACER);
    if (!record) return WOTBMOD_V3_E_LIMIT_REACHED;
    /* The stock manager owns and recycles the visual node. This record is a
     * completion/ownership token only; it never releases a game pointer. */
    record->native_object = nullptr;
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaMaterialMutate(
    void* userData,
    WotbModV3Handle owner,
    WotbModDavaNativeProviderToken materialToken,
    const WotbModDavaNativeMaterialMutation* mutation) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!mutation || mutation->struct_size < sizeof(*mutation) ||
        !memchr(mutation->name, '\0', sizeof(mutation->name))) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaMaterialMutate(
                userData, owner, materialToken, mutation);
        };
        return InvokeDavaMainThread(context, call);
    }
    DavaNativeRecord* material = ValidNativeRecord(
        context,
        owner,
        WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL,
        materialToken);
    if (!material) return WOTBMOD_V3_E_INVALID_HANDLE;
    if (mutation->mutation_kind ==
            WOTBMOD_DAVA_NATIVE_MATERIAL_APPLY_TO_MESH_CONSUMER) {
        DavaNativeRecord* consumer = ValidNativeRecord(
            context,
            owner,
            WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER,
            static_cast<WotbModDavaNativeProviderToken>(
                mutation->related_object));
        if (!consumer || !consumer->related_object) {
            return WOTBMOD_V3_E_INVALID_HANDLE;
        }
        return InvokeRenderBatchSetMaterial(
                   context,
                   consumer->related_object,
                   material->native_object)
            ? WOTBMOD_V3_OK
            : WOTBMOD_V3_E_CALLBACK_FAULT;
    }
    if (!mutation->name[0]) return WOTBMOD_V3_E_INVALID_ARGUMENT;

    FastNameScope name = {};
    if (!MakeFastName(context, mutation->name, &name)) {
        return WOTBMOD_V3_E_PLATFORM;
    }

    WotbModV3Result result = WOTBMOD_V3_E_INVALID_ARGUMENT;
    bool present = false;
    switch (mutation->mutation_kind) {
        case WOTBMOD_DAVA_NATIVE_MATERIAL_SET_PROPERTY: {
            uint32_t shaderType = 0u;
            uint32_t valueWidth = 0u;
            const float* values = mutation->values;
            float scalar = 0.0f;
            switch (mutation->value_type) {
                case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT:
                    shaderType = 0u;
                    valueWidth = 1u;
                    break;
                case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT2:
                    shaderType = 1u;
                    valueWidth = 2u;
                    break;
                case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT3:
                    shaderType = 2u;
                    valueWidth = 3u;
                    break;
                case WOTBMOD_DAVA_NATIVE_VALUE_FLOAT4:
                    shaderType = 3u;
                    valueWidth = 4u;
                    break;
                case WOTBMOD_DAVA_NATIVE_VALUE_INT:
                    shaderType = 0u;
                    valueWidth = 1u;
                    scalar = static_cast<float>(mutation->int_value);
                    values = &scalar;
                    break;
                case WOTBMOD_DAVA_NATIVE_VALUE_BOOL:
                    shaderType = 0u;
                    valueWidth = 1u;
                    scalar = mutation->bool_value ? 1.0f : 0.0f;
                    values = &scalar;
                    break;
                default:
                    break;
            }
            const uint32_t arraySize = mutation->array_size;
            if (valueWidth == 0u || arraySize == 0u ||
                arraySize > 16u / valueWidth ||
                ((mutation->value_type == WOTBMOD_DAVA_NATIVE_VALUE_INT ||
                  mutation->value_type == WOTBMOD_DAVA_NATIVE_VALUE_BOOL) &&
                 arraySize != 1u)) {
                result = WOTBMOD_V3_E_INVALID_ARGUMENT;
                break;
            }
            if (!InvokeMaterialHasRefName(
                    context,
                    context->nmaterial_has_property_rva,
                    material->native_object,
                    &name,
                    &present)) {
                result = WOTBMOD_V3_E_CALLBACK_FAULT;
                break;
            }
            const bool changed = present
                ? InvokeMaterialSetProperty(
                      context, material->native_object, &name, values)
                : InvokeMaterialAddProperty(
                      context,
                      material->native_object,
                      &name,
                      values,
                      shaderType,
                      arraySize);
            result = changed ? WOTBMOD_V3_OK
                             : WOTBMOD_V3_E_CALLBACK_FAULT;
            break;
        }
        case WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_PROPERTY:
            if (!InvokeMaterialHasRefName(
                    context,
                    context->nmaterial_has_property_rva,
                    material->native_object,
                    &name,
                    &present)) {
                result = WOTBMOD_V3_E_CALLBACK_FAULT;
            } else if (!present) {
                result = WOTBMOD_V3_E_NOT_FOUND;
            } else {
                result = InvokeMaterialRefName(
                    context,
                    context->nmaterial_remove_property_rva,
                    material->native_object,
                    &name)
                    ? WOTBMOD_V3_OK : WOTBMOD_V3_E_CALLBACK_FAULT;
            }
            break;
        case WOTBMOD_DAVA_NATIVE_MATERIAL_SET_TEXTURE: {
            DavaNativeRecord* texture = ValidNativeRecord(
                context,
                owner,
                WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE,
                static_cast<WotbModDavaNativeProviderToken>(
                    mutation->related_object));
            if (!texture) {
                result = WOTBMOD_V3_E_INVALID_HANDLE;
            } else {
                result = InvokeMaterialSetTexture(
                    context,
                    material->native_object,
                    &name,
                    texture->native_object)
                    ? WOTBMOD_V3_OK : WOTBMOD_V3_E_CALLBACK_FAULT;
            }
            break;
        }
        case WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_TEXTURE:
            if (!InvokeMaterialHasRefName(
                    context,
                    context->nmaterial_has_texture_rva,
                    material->native_object,
                    &name,
                    &present)) {
                result = WOTBMOD_V3_E_CALLBACK_FAULT;
            } else if (!present) {
                result = WOTBMOD_V3_E_NOT_FOUND;
            } else {
                result = InvokeMaterialRefName(
                    context,
                    context->nmaterial_remove_texture_rva,
                    material->native_object,
                    &name)
                    ? WOTBMOD_V3_OK : WOTBMOD_V3_E_CALLBACK_FAULT;
            }
            break;
        case WOTBMOD_DAVA_NATIVE_MATERIAL_SET_FLAG:
            result = InvokeMaterialSetFlag(
                context,
                material->native_object,
                &name,
                mutation->int_value)
                ? WOTBMOD_V3_OK : WOTBMOD_V3_E_CALLBACK_FAULT;
            break;
        case WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_FLAG:
            if (!InvokeMaterialHasRefName(
                    context,
                    context->nmaterial_has_flag_rva,
                    material->native_object,
                    &name,
                    &present)) {
                result = WOTBMOD_V3_E_CALLBACK_FAULT;
            } else if (!present) {
                result = WOTBMOD_V3_E_NOT_FOUND;
            } else {
                result = InvokeMaterialRefName(
                    context,
                    context->nmaterial_remove_flag_rva,
                    material->native_object,
                    &name)
                    ? WOTBMOD_V3_OK : WOTBMOD_V3_E_CALLBACK_FAULT;
            }
            break;
        /*
         * THE THREE QUERIES. They share one body because they differ only in
         * which of the client's three Has* functions is asked - and each of
         * those was already anchored and already used above to decide between
         * add and set. Nothing here writes.
         */
        case WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_PROPERTY:
        case WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_TEXTURE:
        case WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_FLAG: {
            const uint32_t queryRva =
                mutation->mutation_kind ==
                    WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_PROPERTY
                    ? context->nmaterial_has_property_rva
                    : (mutation->mutation_kind ==
                       WOTBMOD_DAVA_NATIVE_MATERIAL_HAS_TEXTURE
                           ? context->nmaterial_has_texture_rva
                           : context->nmaterial_has_flag_rva);
            if (!InvokeMaterialHasRefName(
                    context,
                    queryRva,
                    material->native_object,
                    &name,
                    &present)) {
                result = WOTBMOD_V3_E_CALLBACK_FAULT;
            } else {
                result = present ? WOTBMOD_V3_OK : WOTBMOD_V3_E_NOT_FOUND;
            }
            break;
        }
        case WOTBMOD_DAVA_NATIVE_MATERIAL_SET_FX:
            result = InvokeMaterialRefName(
                context,
                context->nmaterial_set_fx_rva,
                material->native_object,
                &name)
                ? WOTBMOD_V3_OK : WOTBMOD_V3_E_CALLBACK_FAULT;
            break;
        case WOTBMOD_DAVA_NATIVE_MATERIAL_SET_QUALITY:
            result = InvokeMaterialRefName(
                context,
                context->nmaterial_set_quality_rva,
                material->native_object,
                &name)
                ? WOTBMOD_V3_OK : WOTBMOD_V3_E_CALLBACK_FAULT;
            break;
        default:
            break;
    }
    ReleaseFastName(context, &name);
    return result;
}

static bool ReviewedNativeClass(const char* className) {
    return className &&
           (strcmp(className, "DAVA::UIControl") == 0 ||
            strcmp(className, "DAVA::Entity") == 0 ||
            strcmp(className, "DAVA::NMaterial") == 0 ||
            strcmp(className, "DAVA::Texture") == 0 ||
            strcmp(className, "DAVA::Mesh") == 0 ||
            strcmp(className, "DAVA::MeshConsumer") == 0);
}

static bool ReadClassStringPayload(
    const WotbModDavaNativeClassRequest* request,
    uint32_t maximumSize,
    const char** outPayload) {
    if (!request || !outPayload || !request->payload ||
        request->payload_size < 2u ||
        request->payload_size > maximumSize) {
        return false;
    }
    *outPayload = nullptr;
    __try {
        const char* payload =
            reinterpret_cast<const char*>(request->payload);
        if (payload[request->payload_size - 1u] != '\0' ||
            strnlen_s(payload, request->payload_size) !=
                request->payload_size - 1u) {
            return false;
        }
        *outPayload = payload;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static WotbModV3Result WOTBMOD_V3_CALL DavaClassIsRegistered(
    void* userData,
    const char* className,
    uint32_t* outRegistered) {
    if (!ValidContext(userData) || !className || !outRegistered) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outRegistered = ReviewedNativeClass(className) ? 1u : 0u;
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaClassCreate(
    void* userData,
    WotbModV3Handle owner,
    const WotbModDavaNativeClassRequest* request,
    WotbModDavaNativeProviderToken* outProviderToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE || !request ||
        request->struct_size < sizeof(*request) || !outProviderToken) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    *outProviderToken = 0u;
    if (!IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaClassCreate(
                userData, owner, request, outProviderToken);
        };
        return InvokeDavaMainThread(context, call);
    }
    if (request->flags != 0u ||
        !memchr(request->class_name, '\0', sizeof(request->class_name)) ||
        !ReviewedNativeClass(request->class_name)) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    if (strcmp(request->class_name, "DAVA::NMaterial") == 0) {
        const char* name = "wotbmod.native.material";
        if (request->object_kind != WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL ||
            (request->payload_size != 0u && !request->payload) ||
            request->payload_size >= WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        if (request->payload_size != 0u &&
            !ReadClassStringPayload(
                request,
                WOTBMOD_DAVA_NATIVE_MAX_CLASS_NAME - 1u,
                &name)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return CreateNativeMaterial(context, owner, name, outProviderToken);
    }

    if (strcmp(request->class_name, "DAVA::Texture") == 0) {
        const char* path = nullptr;
        if (request->object_kind != WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE ||
            !ReadClassStringPayload(
                request, WOTBMOD_MAX_RESOURCE_PATH, &path)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return CreateNativeTexture(
            context,
            owner,
            path,
            outProviderToken);
    }

    if (strcmp(request->class_name, "DAVA::Mesh") == 0 ||
        strcmp(request->class_name, "DAVA::MeshConsumer") == 0) {
        const uint32_t expectedKind =
            strcmp(request->class_name, "DAVA::Mesh") == 0
                ? WOTBMOD_DAVA_NATIVE_OBJECT_MESH
                : WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER;
        const char* path = nullptr;
        if (request->object_kind != expectedKind ||
            !ReadClassStringPayload(
                request, WOTBMOD_MAX_RESOURCE_PATH, &path)) {
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
        }
        return CreateNativeMeshObject(
            context, owner, expectedKind, path, outProviderToken);
    }

    if (request->object_kind != WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE ||
        request->payload != nullptr || request->payload_size != 0u) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }

    void* resource = nullptr;
    const WotbModResult created =
        strcmp(request->class_name, "DAVA::UIControl") == 0
            ? UiCreate(context, nullptr, &resource)
            : SceneEntityCreate(context, &resource);
    if (created != WOTBMOD_OK) return LegacyResultToV3(created);
    if (!resource) return WOTBMOD_V3_E_PLATFORM;
    DavaNativeRecord* record = AllocateNativeRecord(
        context, owner, request->object_kind);
    if (!record) {
        (void)ResourceRelease(context, resource);
        return WOTBMOD_V3_E_LIMIT_REACHED;
    }
    record->native_object = resource;
    *outProviderToken = static_cast<WotbModDavaNativeProviderToken>(
        reinterpret_cast<uintptr_t>(record));
    return WOTBMOD_V3_OK;
}

static WotbModV3Result WOTBMOD_V3_CALL DavaNativeRelease(
    void* userData,
    WotbModV3Handle owner,
    uint32_t objectKind,
    WotbModDavaNativeProviderToken providerToken) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context || owner == WOTBMOD_V3_INVALID_HANDLE ||
        providerToken == 0u) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (objectKind != WOTBMOD_DAVA_NATIVE_OBJECT_TRACER &&
        !IsDavaMainThread(context)) {
        auto call = [=]() {
            return DavaNativeRelease(
                userData, owner, objectKind, providerToken);
        };
        return InvokeDavaMainThread(context, call);
    }
    DavaNativeRecord* record = ValidNativeRecord(
        context, owner, objectKind, providerToken);
    if (!record) return WOTBMOD_V3_E_INVALID_HANDLE;

    WotbModV3Result result = WOTBMOD_V3_OK;
    switch (objectKind) {
        case WOTBMOD_DAVA_NATIVE_OBJECT_YAML_DOCUMENT:
            if (!InvokeRelease(context, record->native_object)) {
                result = WOTBMOD_V3_E_PLATFORM;
            }
            break;
        case WOTBMOD_DAVA_NATIVE_OBJECT_RESOURCE_ARCHIVE:
            if (!InvokeArchiveDtor(context, &record->native_object)) {
                result = WOTBMOD_V3_E_PLATFORM;
            }
            break;
        case WOTBMOD_DAVA_NATIVE_OBJECT_NMATERIAL:
        case WOTBMOD_DAVA_NATIVE_OBJECT_TEXTURE:
        case WOTBMOD_DAVA_NATIVE_OBJECT_MESH:
        case WOTBMOD_DAVA_NATIVE_OBJECT_MESH_CONSUMER:
            if (!InvokeRelease(context, record->native_object)) {
                result = WOTBMOD_V3_E_PLATFORM;
            }
            break;
        case WOTBMOD_DAVA_NATIVE_OBJECT_CLASS_INSTANCE:
            result = LegacyResultToV3(
                ResourceRelease(context, record->native_object));
            break;
        case WOTBMOD_DAVA_NATIVE_OBJECT_TRACER:
            /* TracerManager owns the visual and returns it to its pool. */
            break;
        default:
            return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (result != WOTBMOD_V3_OK) return result;
    record->native_object = nullptr;
    record->related_object = nullptr;
    FreeNativeRecord(record);
    return WOTBMOD_V3_OK;
}

static void WOTBMOD_CALL ResourceRegistryChanged(
    void* userData,
    uint64_t generation) {
    DavaResourcesContext* context = ValidContext(userData);
    if (!context) return;
    char line[128] = {};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "DAVA resource registry generation=%llu",
        static_cast<unsigned long long>(generation));
    Log(context, WOTBMOD_LOG_TRACE, line);
}

} /* namespace */

extern "C" WotbModResult WOTBMOD_CALL WotbModDavaResources_Create(
    const WotbModDavaResourcesOptions* options,
    WotbModDavaResourcesHandle* outHandle,
    WotbModRuntimeResourceBackend* outBackend) {
    if (!outHandle || !outBackend) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outHandle = nullptr;
    ZeroMemory(outBackend, sizeof(*outBackend));

    const size_t minimumOptionsSize =
        offsetof(WotbModDavaResourcesOptions, game_module) +
        sizeof(options->game_module);
    if (options && options->struct_size < minimumOptionsSize) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    HMODULE module = static_cast<HMODULE>(
        options && options->game_module
            ? options->game_module
            : GetModuleHandleA(nullptr));
    uint32_t imageSize = 0;
    if (!ReadImageSize(module, &imageSize)) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    DavaResourcesContext* context =
        static_cast<DavaResourcesContext*>(HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(DavaResourcesContext)));
    if (!context) return WOTBMOD_ERROR_LIMIT_REACHED;

    context->magic = kContextMagic;
    context->game_module = module;
    context->image_base = reinterpret_cast<uint8_t*>(module);
    context->image_size = imageSize;
    RecordImageCodeRanges(module, imageSize);
    InitializeDavaDataDirectory(context);
    context->ref_counted_retain_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ref_counted_retain_rva),
        kDefaultRefCountedRetainRva);
    context->ref_counted_release_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ref_counted_release_rva),
        kDefaultRefCountedReleaseRva);
    context->fast_name_ctor_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            fast_name_ctor_rva),
        kDefaultFastNameCtorRva);
    context->yaml_parse_file_wrapper_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            yaml_parse_file_wrapper_rva),
        kDefaultYamlParseFileWrapperRva);
    context->resource_archive_ctor_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            resource_archive_ctor_rva),
        kDefaultResourceArchiveCtorRva);
    context->resource_archive_dtor_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            resource_archive_dtor_rva),
        kDefaultResourceArchiveDtorRva);
    context->pack_archive_vtable_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            pack_archive_vtable_rva),
        kDefaultPackArchiveVtableRva);
    context->zip_archive_vtable_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            zip_archive_vtable_rva),
        kDefaultZipArchiveVtableRva);
    context->nmaterial_ctor_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_ctor_rva),
        kDefaultNMaterialCtorRva);
    context->nmaterial_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_vtable_rva),
        kDefaultNMaterialVtableRva);
    context->nmaterial_set_fx_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_set_fx_rva),
        kDefaultNMaterialSetFxRva);
    context->nmaterial_set_quality_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_set_quality_rva),
        kDefaultNMaterialSetQualityRva);
    context->nmaterial_has_property_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_has_property_rva),
        kDefaultNMaterialHasPropertyRva);
    context->nmaterial_add_property_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_add_property_rva),
        kDefaultNMaterialAddPropertyRva);
    context->nmaterial_set_property_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_set_property_rva),
        kDefaultNMaterialSetPropertyRva);
    context->nmaterial_remove_property_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_remove_property_rva),
        kDefaultNMaterialRemovePropertyRva);
    context->nmaterial_has_flag_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_has_flag_rva),
        kDefaultNMaterialHasFlagRva);
    context->nmaterial_set_flag_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_set_flag_rva),
        kDefaultNMaterialSetFlagRva);
    context->nmaterial_remove_flag_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_remove_flag_rva),
        kDefaultNMaterialRemoveFlagRva);
    context->nmaterial_has_texture_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_has_texture_rva),
        kDefaultNMaterialHasTextureRva);
    context->nmaterial_set_texture_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_set_texture_rva),
        kDefaultNMaterialSetTextureRva);
    context->nmaterial_remove_texture_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, nmaterial_remove_texture_rva),
        kDefaultNMaterialRemoveTextureRva);
    context->texture_create_from_file_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, texture_create_from_file_rva),
        kDefaultTextureCreateFromFileRva);
    context->entity_get_render_object_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            entity_get_render_object_rva),
        kDefaultEntityGetRenderObjectRva);
    context->render_object_get_render_batch_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            render_object_get_render_batch_rva),
        kDefaultRenderObjectGetRenderBatchRva);
    context->render_batch_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, render_batch_vtable_rva),
        kDefaultRenderBatchVtableRva);
    context->render_batch_set_material_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            render_batch_set_material_rva),
        kDefaultRenderBatchSetMaterialRva);
    context->render_batch_set_polygon_group_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            render_batch_set_polygon_group_rva),
        kDefaultRenderBatchSetPolygonGroupRva);
    if (options &&
        options->struct_size >=
            offsetof(WotbModDavaResourcesOptions, stock_tracer_create) +
                sizeof(options->stock_tracer_create)) {
        context->stock_tracer_create = options->stock_tracer_create;
    }
    if (options &&
        options->struct_size >=
            offsetof(WotbModDavaResourcesOptions, stock_tracer_user_data) +
                sizeof(options->stock_tracer_user_data)) {
        context->stock_tracer_user_data = options->stock_tracer_user_data;
    }
    if (options &&
        options->struct_size >=
            offsetof(WotbModDavaResourcesOptions, is_main_thread) +
                sizeof(options->is_main_thread)) {
        context->is_main_thread = options->is_main_thread;
    }
    if (options &&
        options->struct_size >=
            offsetof(WotbModDavaResourcesOptions, main_thread_user_data) +
                sizeof(options->main_thread_user_data)) {
        context->main_thread_user_data = options->main_thread_user_data;
    }
    if (options &&
        options->struct_size >=
            offsetof(WotbModDavaResourcesOptions, invoke_main_thread) +
                sizeof(options->invoke_main_thread)) {
        context->invoke_main_thread = options->invoke_main_thread;
    }
    if (options &&
        options->struct_size >=
            offsetof(
                WotbModDavaResourcesOptions,
                invoke_main_thread_user_data) +
                sizeof(options->invoke_main_thread_user_data)) {
        context->invoke_main_thread_user_data =
            options->invoke_main_thread_user_data;
    }
    context->operator_new_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, operator_new_rva),
        kDefaultOperatorNewRva);
    context->operator_delete_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, operator_delete_rva),
        kDefaultOperatorDeleteRva);
    context->ui_control_ctor_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, ui_control_ctor_rva),
        kDefaultUiControlCtorRva);
    context->ui_text_component_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, ui_text_component_vtable_rva),
        kDefaultUiTextComponentVtableRva);
    context->ui_dynamic_atlas_text_component_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, ui_dynamic_atlas_text_component_vtable_rva),
        kDefaultUiDynamicAtlasTextComponentVtableRva);
    context->entity_ctor_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, entity_ctor_rva),
        kDefaultEntityCtorRva);
    context->get_engine_context_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, get_engine_context_rva),
        kDefaultGetEngineContextRva);
    context->ui_control_system_offset = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_control_system_offset),
        kDefaultUiControlSystemOffset);
    context->ui_control_system_get_screen_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_control_system_get_screen_rva),
        kDefaultUiControlSystemGetScreenRva);
    context->ui_package_loader_ctor_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_package_loader_ctor_rva),
        kDefaultUiPackageLoaderCtorRva);
    context->ui_package_loader_dtor_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_package_loader_dtor_rva),
        kDefaultUiPackageLoaderDtorRva);
    context->ui_load_package_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, ui_load_package_rva),
        kDefaultUiLoadPackageRva);
    context->ui_package_builder_ctor_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_package_builder_ctor_rva),
        kDefaultUiPackageBuilderCtorRva);
    context->ui_package_builder_dtor_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_package_builder_dtor_rva),
        kDefaultUiPackageBuilderDtorRva);
    context->ui_extract_control_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_extract_control_rva),
        kDefaultUiExtractControlRva);
    context->scene_ctor_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, scene_ctor_rva),
        kDefaultSceneCtorRva);
    context->scene_load_from_file_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            scene_load_from_file_rva),
        kDefaultSceneLoadFromFileRva);
    context->scene_load_entity_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            scene_load_entity_rva),
        kDefaultSceneLoadEntityRva);
    context->ui_package_loader_vtable_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_package_loader_vtable_rva),
        kDefaultUiPackageLoaderVtableRva);
    context->ui_package_vtable_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_package_vtable_rva),
        kDefaultUiPackageVtableRva);
    context->ui_control_vtable_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            ui_control_vtable_rva),
        kDefaultUiControlVtableRva);
    context->entity_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, entity_vtable_rva),
        kDefaultEntityVtableRva);
    context->scene_vtable_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, scene_vtable_rva),
        kDefaultSceneVtableRva);
    context->scene_draw_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, scene_draw_rva),
        kDefaultSceneDrawRva);
    context->scene_activate_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, scene_activate_rva),
        kDefaultSceneActivateRva);
    context->scene_deactivate_rva = SelectOptionRva(
        options,
        offsetof(WotbModDavaResourcesOptions, scene_deactivate_rva),
        kDefaultSceneDeactivateRva);
    context->transform_set_local_transform_rva = SelectOptionRva(
        options,
        offsetof(
            WotbModDavaResourcesOptions,
            transform_component_set_local_transform_rva),
        kDefaultTransformSetLocalTransformRva);
    if (options &&
        options->struct_size >=
            offsetof(WotbModDavaResourcesOptions, log_sink) +
                sizeof(options->log_sink)) {
        context->log_sink = options->log_sink;
    }
    if (options &&
        options->struct_size >=
            offsetof(WotbModDavaResourcesOptions, log_user_data) +
                sizeof(options->log_user_data)) {
        context->log_user_data = options->log_user_data;
    }
    {
        LARGE_INTEGER frequency = {}, started = {}, finished = {};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        MEMORY_BASIC_INFORMATION info = {};
        for (int i = 0; i < 64; ++i) {
            VirtualQuery(
                context->image_base + static_cast<size_t>(i) * 4096u,
                &info, sizeof(info));
        }
        QueryPerformanceCounter(&finished);
        const double microsPerCall = frequency.QuadPart
            ? (finished.QuadPart - started.QuadPart) * 1000000.0 /
                  static_cast<double>(frequency.QuadPart) / 64.0
            : 0.0;
        char line[160] = {};
        _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "address checks: VirtualQuery %.1f us/call at load, "
                    "image code ranges %u (in-image checks are syscall-free)",
                    microsPerCall,
                    static_cast<unsigned>(g_imageCodeRangeCount));
        Log(context, WOTBMOD_LOG_INFO, line);
    }

    const uint32_t executableRvas[] = {
        context->ref_counted_retain_rva,
        context->ref_counted_release_rva,
        context->fast_name_ctor_rva,
        context->yaml_parse_file_wrapper_rva,
        context->resource_archive_ctor_rva,
        context->resource_archive_dtor_rva,
        context->nmaterial_ctor_rva,
        context->nmaterial_set_fx_rva,
        context->nmaterial_set_quality_rva,
        context->nmaterial_has_property_rva,
        context->nmaterial_add_property_rva,
        context->nmaterial_set_property_rva,
        context->nmaterial_remove_property_rva,
        context->nmaterial_has_flag_rva,
        context->nmaterial_set_flag_rva,
        context->nmaterial_remove_flag_rva,
        context->nmaterial_has_texture_rva,
        context->nmaterial_set_texture_rva,
        context->nmaterial_remove_texture_rva,
        context->texture_create_from_file_rva,
        context->entity_get_render_object_rva,
        context->render_object_get_render_batch_rva,
        context->render_batch_set_material_rva,
        context->render_batch_set_polygon_group_rva,
        context->operator_new_rva,
        context->operator_delete_rva,
        context->ui_control_ctor_rva,
        context->entity_ctor_rva,
        context->get_engine_context_rva,
        context->ui_control_system_get_screen_rva,
        context->ui_package_loader_ctor_rva,
        context->ui_package_loader_dtor_rva,
        context->ui_load_package_rva,
        context->ui_package_builder_ctor_rva,
        context->ui_package_builder_dtor_rva,
        context->ui_extract_control_rva,
        context->scene_ctor_rva,
        context->scene_load_from_file_rva,
        context->scene_load_entity_rva,
        context->scene_draw_rva,
        context->scene_activate_rva,
        context->scene_deactivate_rva,
        context->transform_set_local_transform_rva,
    };
    bool valid = true;
    for (size_t index = 0;
         index < sizeof(executableRvas) / sizeof(executableRvas[0]);
         ++index) {
        void* address = Address(context, executableRvas[index]);
        if (!address || !IsExecutableAddress(address)) {
            valid = false;
            break;
        }
    }
    valid = valid &&
            RvaInImage(
                context,
                context->ui_package_loader_vtable_rva,
                sizeof(void*)) &&
            RvaInImage(
                context,
                context->ui_package_vtable_rva,
                sizeof(void*)) &&
            RvaInImage(
                context,
                context->ui_control_vtable_rva,
                sizeof(void*)) &&
            RvaInImage(
                context,
                context->entity_vtable_rva,
                sizeof(void*)) &&
            RvaInImage(
                context,
                context->scene_vtable_rva,
                sizeof(void*)) &&
            RvaInImage(
                context,
                context->pack_archive_vtable_rva,
                5u * sizeof(void*)) &&
            RvaInImage(
                context,
                context->zip_archive_vtable_rva,
                5u * sizeof(void*)) &&
            RvaInImage(
                context,
                context->nmaterial_vtable_rva,
                sizeof(void*)) &&
            RvaInImage(
                context,
                context->render_batch_vtable_rva,
                sizeof(void*));
    if (!valid) {
        context->magic = 0;
        HeapFree(GetProcessHeap(), 0, context);
        return WOTBMOD_ERROR_PLATFORM;
    }

    outBackend->struct_size = sizeof(*outBackend);
    outBackend->user_data = context;
    outBackend->load = &ResourceLoad;
    outBackend->reload = &ResourceReload;
    outBackend->release = &ResourceRelease;
    outBackend->registry_changed = &ResourceRegistryChanged;
    outBackend->ui_set_geometry = &UiSetGeometry;
    outBackend->ui_set_visible = &UiSetVisible;
    outBackend->ui_add_child = &UiAddChild;
    outBackend->ui_remove_child = &UiRemoveChild;
    outBackend->scene_set_transform = &SceneSetTransform;
    outBackend->scene_add_child = &SceneAddChild;
    outBackend->scene_remove_child = &SceneRemoveChild;
    outBackend->ui_create = &UiCreate;
    outBackend->ui_get_active_screen = &UiGetActiveScreen;
    outBackend->scene_entity_create = &SceneEntityCreate;
    outBackend->scene_get_active = &SceneGetActive;
    outBackend->clone = &ResourceClone;
    outBackend->ui_find_by_name = &UiFindByName;
    outBackend->ui_get_parent = &UiGetParent;
    outBackend->ui_get_child_count = &UiGetChildCount;
    outBackend->ui_get_child_at = &UiGetChildAt;
    outBackend->ui_get_state = &UiGetState;
    outBackend->ui_set_input_enabled = &UiSetInputEnabled;
    outBackend->ui_set_disabled = &UiSetDisabled;
    outBackend->ui_get_text = &UiGetText;
    outBackend->ui_get_identity = &UiGetIdentity;
    outBackend->ui_get_background_color = &UiGetBackgroundColor;
    outBackend->ui_set_background_color = &UiSetBackgroundColor;
    outBackend->load_resolved = &ResourceLoadResolved;
    *outHandle = context;
    return WOTBMOD_OK;
}

extern "C" WotbModResult WOTBMOD_CALL WotbModDavaResources_Destroy(
    WotbModDavaResourcesHandle handle) {
    DavaResourcesContext* context =
        static_cast<DavaResourcesContext*>(handle);
    if (!context || context->magic != kContextMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(
            &context->active_resources, 0, 0) != 0) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    InterlockedExchangePointer(&context->active_scene, nullptr);
    context->magic = 0;
    return HeapFree(GetProcessHeap(), 0, context)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

extern "C" WotbModV3Result WOTBMOD_CALL
WotbModDavaResources_GetNativeBackend(
    WotbModDavaResourcesHandle handle,
    WotbModDavaNativeBackend* outBackend) {
    DavaResourcesContext* context = ValidContext(handle);
    if (!context || !outBackend ||
        outBackend->struct_size < sizeof(*outBackend)) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    if (!context->is_main_thread || !context->invoke_main_thread) {
        return WOTBMOD_V3_E_NOT_SUPPORTED;
    }
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
        WOTBMOD_DAVA_NATIVE_CAP_CLASS_FACTORY;
    if (context->stock_tracer_create) {
        backend.capabilities |= WOTBMOD_DAVA_NATIVE_CAP_STOCK_TRACER;
    }
    backend.user_data = context;
    backend.yaml_parse_file = &DavaYamlParseFile;
    backend.yaml_export_utf8 = &DavaYamlExportUtf8;
    backend.archive_open_file = &DavaArchiveOpenFile;
    backend.archive_get_entry_count = &DavaArchiveGetEntryCount;
    backend.archive_get_entry = &DavaArchiveGetEntry;
    backend.archive_read_entry = &DavaArchiveReadEntry;
    backend.material_mutate = &DavaMaterialMutate;
    backend.mesh_hot_swap = &DavaMeshHotSwap;
    backend.tracer_create = context->stock_tracer_create
        ? &DavaTracerCreate : nullptr;
    backend.class_is_registered = &DavaClassIsRegistered;
    backend.class_create = &DavaClassCreate;
    backend.release = &DavaNativeRelease;
    /* The live scene's own materials. Under the NMATERIAL capability rather
     * than one of its own: what it hands back IS an NMaterial token, and a
     * provider that cannot mint those cannot mint these either. */
    backend.scene_material = &SceneMaterialCreate;
    backend.scene_nodes = &SceneNodesList;
    /* The live scene's own render batches - the consumer half of a live
     * geometry swap. Under MESH_HOT_SWAP because what it hands back is what
     * mesh_hot_swap consumes; a provider without that capability has no use
     * for this route either. */
    backend.scene_batch = &SceneBatchCreate;
    *outBackend = backend;
    return WOTBMOD_V3_OK;
}

extern "C" void* WOTBMOD_CALL
WotbModDavaResources_GetSceneDrawTarget(
    WotbModDavaResourcesHandle handle) {
    DavaResourcesContext* context = ValidContext(handle);
    if (!context) return nullptr;
    void* target = Address(context, context->scene_draw_rva);
    return IsExecutableAddress(target) ? target : nullptr;
}

extern "C" void* WOTBMOD_CALL
WotbModDavaResources_GetSceneActivateTarget(
    WotbModDavaResourcesHandle handle) {
    DavaResourcesContext* context = ValidContext(handle);
    if (!context) return nullptr;
    void* target = Address(context, context->scene_activate_rva);
    return IsExecutableAddress(target) ? target : nullptr;
}

extern "C" void* WOTBMOD_CALL
WotbModDavaResources_GetSceneDeactivateTarget(
    WotbModDavaResourcesHandle handle) {
    DavaResourcesContext* context = ValidContext(handle);
    if (!context) return nullptr;
    void* target = Address(context, context->scene_deactivate_rva);
    return IsExecutableAddress(target) ? target : nullptr;
}

extern "C" void* WOTBMOD_CALL
WotbModDavaResources_GetNativeObject(
    WotbModDavaResourcesHandle handle,
    void* nativeResource) {
    DavaResourcesContext* context = ValidContext(handle);
    DavaResource* resource =
        ValidResource(context, nativeResource);
    return resource ? resource->native_object : nullptr;
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModDavaResources_BringUiToFront(
    WotbModDavaResourcesHandle handle,
    void* nativeResource) {
    DavaResourcesContext* context = ValidContext(handle);
    DavaResource* child = TypedObjectResource(
        context, nativeResource, WOTBMOD_RESOURCE_UI_CONTROL);
    if (!child) return WOTBMOD_ERROR_INVALID_ARGUMENT;

    void* parent = nullptr;
    __try {
        parent = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(child->native_object) +
            kUiParentOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
    if (!parent) return WOTBMOD_ERROR_NOT_FOUND;

    ObjectPairFn bringToFront = VirtualFunction<ObjectPairFn>(
        parent, kUiBringToFrontVtableIndex);
    if (!bringToFront ||
        !IsExecutableAddress(reinterpret_cast<const void*>(bringToFront))) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    __try {
        bringToFront(parent, child->native_object);
        return WOTBMOD_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return WOTBMOD_ERROR_CALLBACK_FAULT;
    }
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModDavaResources_TrackActiveScene(
    WotbModDavaResourcesHandle handle,
    void* scene) {
    DavaResourcesContext* context = ValidContext(handle);
    if (!context) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (scene &&
        !HasVirtualTarget(
            context,
            scene,
            kSceneDrawVtableIndex,
            context->scene_draw_rva)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    InterlockedExchangePointer(&context->active_scene, scene);
    return WOTBMOD_OK;
}

extern "C" WotbModResult WOTBMOD_CALL
WotbModDavaResources_UntrackActiveScene(
    WotbModDavaResourcesHandle handle,
    void* scene) {
    DavaResourcesContext* context = ValidContext(handle);
    if (!context || !scene) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    InterlockedCompareExchangePointer(
        &context->active_scene, nullptr, scene);
    return WOTBMOD_OK;
}
