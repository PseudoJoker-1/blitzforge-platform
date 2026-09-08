#pragma once

#include <stdint.h>

#include "wotb_mod_runtime.h"
#include "wotb_mod_dava_native.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* WotbModDavaResourcesHandle;

/* Loader-private bridge into the exact-client TracerManager adapter. The
 * resource provider receives only a result; the game-owned manager and tracer
 * nodes never cross this boundary. */
typedef WotbModV3Result (WOTBMOD_V3_CALL*
    WotbModDavaStockTracerCreateFn)(
        void* user_data,
        const WotbModDavaNativeTracerRequest* request);

/* Loader-private thread bridge. Exact DAVA constructors, mutators and
 * destructors run only on the proven game main thread. Calls from Lua, event
 * and render threads are synchronously marshalled through the loader's MAIN
 * ingress; an unavailable ingress or timeout is reported instead of entering
 * DAVA from the wrong thread. */
typedef uint32_t (WOTBMOD_CALL* WotbModDavaIsMainThreadFn)(
    void* user_data);
typedef WotbModV3Result (WOTBMOD_V3_CALL*
    WotbModDavaMainThreadCallFn)(void* call_data);
typedef WotbModV3Result (WOTBMOD_CALL*
    WotbModDavaInvokeMainThreadFn)(
        void* user_data,
        WotbModDavaMainThreadCallFn call,
        void* call_data);

/*
 * Version-specific object bridge for the WoT Blitz 11.19.0.834 x86 client.
 *
 * The bridge owns all DAVA C++ objects. Mods continue to receive only opaque
 * WotbModResourceHandle values through the compiler-neutral public ABI.
 * Every address is image-base-relative. Zero RVA fields select the verified
 * 11.19 defaults; loaders for newer clients may override them individually.
 */
typedef struct WotbModDavaResourcesOptions {
    uint32_t struct_size;
    void* game_module;
    uint32_t ref_counted_retain_rva;
    uint32_t ref_counted_release_rva;
    uint32_t operator_new_rva;
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
    uint32_t scene_vtable_rva;
    WotbModRuntimeLogSink log_sink;
    void* log_user_data;
    uint32_t transform_component_set_local_transform_rva;
    uint32_t operator_delete_rva;
    uint32_t ui_control_ctor_rva;
    uint32_t entity_ctor_rva;
    uint32_t get_engine_context_rva;
    uint32_t ui_control_system_offset;
    uint32_t ui_control_system_get_screen_rva;
    uint32_t entity_vtable_rva;
    uint32_t scene_draw_rva;
    uint32_t scene_activate_rva;
    uint32_t scene_deactivate_rva;
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
    /* DAVA::UITextComponent vtable; 0 keeps the built-in default. */
    uint32_t ui_text_component_vtable_rva;
    /* DAVA::UIDynamicAtlasTextComponent vtable - the text component the
     * Blitz hangar and HUD actually use (11.20.0.887); 0 keeps the default. */
    uint32_t ui_dynamic_atlas_text_component_vtable_rva;
} WotbModDavaResourcesOptions;

WotbModResult WOTBMOD_CALL WotbModDavaResources_Create(
    const WotbModDavaResourcesOptions* options,
    WotbModDavaResourcesHandle* out_handle,
    WotbModRuntimeResourceBackend* out_backend);

WotbModResult WOTBMOD_CALL WotbModDavaResources_Destroy(
    WotbModDavaResourcesHandle handle);

/* Loader-private reviewed class provider. The returned table exposes only
 * exact-build factories owned by this bridge; it never returns native object
 * pointers to a mod or Lua state. */
WotbModV3Result WOTBMOD_CALL WotbModDavaResources_GetNativeBackend(
    WotbModDavaResourcesHandle handle,
    WotbModDavaNativeBackend* out_backend);

/*
 * Loader-only active Scene tracker. The live loader hooks the verified
 * DAVA::Scene::Draw implementation, forwards the observed `this` pointer to
 * TrackActiveScene, and exposes the retained object through scene_get_active.
 * Mods never receive or call these raw addresses.
 */
void* WOTBMOD_CALL WotbModDavaResources_GetSceneDrawTarget(
    WotbModDavaResourcesHandle handle);
void* WOTBMOD_CALL WotbModDavaResources_GetSceneActivateTarget(
    WotbModDavaResourcesHandle handle);
void* WOTBMOD_CALL WotbModDavaResources_GetSceneDeactivateTarget(
    WotbModDavaResourcesHandle handle);
void* WOTBMOD_CALL WotbModDavaResources_GetNativeObject(
    WotbModDavaResourcesHandle handle,
    void* native_resource);
WotbModResult WOTBMOD_CALL WotbModDavaResources_BringUiToFront(
    WotbModDavaResourcesHandle handle,
    void* native_resource);
WotbModResult WOTBMOD_CALL WotbModDavaResources_TrackActiveScene(
    WotbModDavaResourcesHandle handle,
    void* scene);
WotbModResult WOTBMOD_CALL WotbModDavaResources_UntrackActiveScene(
    WotbModDavaResourcesHandle handle,
    void* scene);

#ifdef __cplusplus
} /* extern "C" */
#endif
