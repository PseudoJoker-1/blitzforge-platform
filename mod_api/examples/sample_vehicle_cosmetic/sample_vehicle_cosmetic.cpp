#include "wotb_mod_api_v3.h"

#include <cstring>

namespace {
const WotbModV3Bootstrap* g_bootstrap = nullptr;
const WotbModV3CoreApiV1* g_core = nullptr;
const WotbModV3ResourcesApiV1* g_resources = nullptr;
const WotbModV3VehicleVisualApiV2* g_visual = nullptr;
const WotbModV3EventsApiV1* g_events = nullptr;
const WotbModV3HandlesApiV1* g_handles = nullptr;
WotbModV3Handle g_mod = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3ResourceHandle g_descriptor = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3Handle g_pack = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3EntityHandle g_vehicle = WOTBMOD_V3_INVALID_HANDLE;
WotbModV3EventToken g_vehicle_event = WOTBMOD_V3_INVALID_HANDLE;
uint32_t g_state = 0u;
uint32_t g_reapply_count = 0u;
uint32_t g_requires_reload = 0u;

template <typename T>
const T* Query(const char* name, uint32_t version) {
    const void* table = nullptr;
    return g_bootstrap && g_bootstrap->query_interface &&
           g_bootstrap->query_interface(g_mod, name, version, &table) ==
               WOTBMOD_V3_OK
        ? static_cast<const T*>(table)
        : nullptr;
}

void Log(uint32_t level, const char* text) {
    if (g_core && g_core->log) {
        g_core->log(g_mod, level, "sample.vehicle_cosmetic", text);
    }
}

bool ApplyCurrentVehicle() {
    if (!g_visual || g_pack == WOTBMOD_V3_INVALID_HANDLE) return false;
    if (g_vehicle != WOTBMOD_V3_INVALID_HANDLE && g_handles) {
        g_handles->release(g_mod, g_vehicle);
        g_vehicle = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_visual->v1.get_local_player_vehicle(g_mod, &g_vehicle) !=
            WOTBMOD_V3_OK ||
        g_visual->skin_pack_apply(g_mod, g_pack, g_vehicle) !=
            WOTBMOD_V3_OK) {
        return false;
    }
    WotbModV3VehicleSkinState state = {};
    WOTBMOD_V3_INIT_STRUCT(state, WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    if (g_visual->skin_pack_get_state(g_mod, g_pack, &state) !=
        WOTBMOD_V3_OK) return false;
    g_requires_reload = state.requires_model_reload;
    ++g_reapply_count;
    g_state |= 1u << 3;
    if (state.requires_model_reload) g_state |= 1u << 4;
    return true;
}

void WOTBMOD_V3_CALL VehicleChanged(
    WotbModV3Handle, WotbModV3Event*, void*) {
    if (g_visual && g_pack != WOTBMOD_V3_INVALID_HANDLE) {
        g_visual->skin_pack_rollback(g_mod, g_pack);
    }
    if (ApplyCurrentVehicle()) g_state |= 1u << 6;
}

void Cleanup() {
    if (g_events && g_vehicle_event != WOTBMOD_V3_INVALID_HANDLE) {
        g_events->unsubscribe(g_mod, g_vehicle_event);
        g_vehicle_event = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_visual && g_pack != WOTBMOD_V3_INVALID_HANDLE) {
        if (g_visual->skin_pack_rollback(g_mod, g_pack) == WOTBMOD_V3_OK) {
            g_state |= 1u << 5;
        }
        g_visual->skin_pack_release(g_mod, g_pack);
        g_pack = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_handles && g_vehicle != WOTBMOD_V3_INVALID_HANDLE) {
        g_handles->release(g_mod, g_vehicle);
        g_vehicle = WOTBMOD_V3_INVALID_HANDLE;
    }
    if (g_resources && g_descriptor != WOTBMOD_V3_INVALID_HANDLE) {
        g_resources->release(g_mod, g_descriptor);
        g_descriptor = WOTBMOD_V3_INVALID_HANDLE;
    }
    g_state |= 1u << 7;
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    g_bootstrap = bootstrap;
    g_mod = mod;
    g_state = 0u;
    g_reapply_count = 0u;
    g_requires_reload = 0u;
    g_core = Query<WotbModV3CoreApiV1>(
        WOTBMOD_V3_IFACE_CORE, WOTBMOD_V3_CORE_VERSION);
    g_resources = Query<WotbModV3ResourcesApiV1>(
        WOTBMOD_V3_IFACE_RESOURCES, WOTBMOD_V3_RESOURCES_VERSION);
    g_visual = Query<WotbModV3VehicleVisualApiV2>(
        WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
        WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    g_events = Query<WotbModV3EventsApiV1>(
        WOTBMOD_V3_IFACE_EVENTS, WOTBMOD_V3_EVENTS_VERSION);
    g_handles = Query<WotbModV3HandlesApiV1>(
        WOTBMOD_V3_IFACE_HANDLES, WOTBMOD_V3_HANDLES_VERSION);
    if (!g_resources || !g_visual) return;
    WotbModV3ResourceLoadDesc load = {};
    WOTBMOD_V3_INIT_STRUCT(load, WOTBMOD_V3_RESOURCES_VERSION);
    load.expected_type = WOTBMOD_V3_RESOURCE_JSON;
    load.max_bytes = 64u * 1024u;
    strcpy_s(load.uri, "mod://skin/skin.json");
    if (g_resources->load(g_mod, &load, &g_descriptor) == WOTBMOD_V3_OK) {
        g_state |= 1u << 0;
    }
    static const WotbModV3VehicleSkinAsset assets[] = {
        {sizeof(WotbModV3VehicleSkinAsset), WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2,
         WOTBMOD_V3_VEHICLE_SKIN_MESH, WOTBMOD_V3_VEHICLE_PART_HULL, 0, 0,
         "game://Data/3d/Tanks/USSR/R110_Object_260.sc2",
         "mod://skin/T-34-85.sc2.dvpl"},
        {sizeof(WotbModV3VehicleSkinAsset), WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2,
         WOTBMOD_V3_VEHICLE_SKIN_MESH, WOTBMOD_V3_VEHICLE_PART_HULL, 1, 0,
         "game://Data/3d/Tanks/USSR/R110_Object_260.scg",
         "mod://skin/T-34-85.scg.dvpl"},
        {sizeof(WotbModV3VehicleSkinAsset), WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2,
         WOTBMOD_V3_VEHICLE_SKIN_MATERIAL, WOTBMOD_V3_VEHICLE_PART_HULL, -1, 0,
         "game://Data/3d/Tanks/USSR/R110_Object_260.material.yaml",
         "mod://skin/R110_Object_260.material.yaml"},
        {sizeof(WotbModV3VehicleSkinAsset), WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2,
         WOTBMOD_V3_VEHICLE_SKIN_TEXTURE, WOTBMOD_V3_VEHICLE_PART_HULL, 0, 0,
         "game://Data/3d/Tanks/USSR/images/R110_Object_260.dx11.dds",
         "mod://skin/T-34-85.dx11.dds.dvpl"},
        {sizeof(WotbModV3VehicleSkinAsset), WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2,
         WOTBMOD_V3_VEHICLE_SKIN_TEXTURE, WOTBMOD_V3_VEHICLE_PART_HULL, 1, 0,
         "game://Data/3d/Tanks/USSR/images/R110_Object_260_NM.dx11.dds",
         "mod://skin/T-34-85_NM.dx11.dds.dvpl"}
    };
    WotbModV3VehicleSkinPack pack = {};
    WOTBMOD_V3_INIT_STRUCT(pack, WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2);
    pack.id = "sample.vehicle_cosmetic.object260";
    pack.vehicle_name = "R110_Object_260";
    pack.assets = assets;
    pack.asset_count = static_cast<uint32_t>(sizeof(assets) / sizeof(assets[0]));
    pack.priority = 100;
    pack.hangar_only = 1u;
    if (g_visual->skin_pack_register(g_mod, &pack, &g_pack) ==
        WOTBMOD_V3_OK) {
        g_state |= 1u << 1;
        if (ApplyCurrentVehicle()) g_state |= 1u << 2;
    }
    if (g_events) {
        WotbModV3EventSubscriptionInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_EVENTS_VERSION);
        info.topic_pattern = WOTBMOD_V3_EVENT_LOCAL_VEHICLE_CHANGED;
        info.priority = WOTBMOD_V3_EVENT_PRIORITY_NORMAL;
        info.receive_system_events = 1u;
        if (g_events->subscribe(
                g_mod, &info, &VehicleChanged, nullptr,
                &g_vehicle_event) == WOTBMOD_V3_OK) {
            g_state |= 1u << 8;
        }
    }
    Log(WOTBMOD_V3_LOG_INFO,
        "skin registered; cached model requires reload when state says so");
}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap*, WotbModV3Handle) { Cleanup(); }
void WOTBMOD_V3_CALL OnUnload(
    const WotbModV3Bootstrap*, WotbModV3Handle) { Cleanup(); }
}

WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleVehicle_GetState() { return g_state; }
WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleVehicle_GetReapplyCount() { return g_reapply_count; }
WOTBMOD_V3_EXPORT uint32_t WOTBMOD_V3_CALL
WotbSampleVehicle_RequiresReload() { return g_requires_reload; }

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info || mod == WOTBMOD_V3_INVALID_HANDLE) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->requested_permission_tier = WOTBMOD_V3_PERMISSION_REVIEWED;
    strcpy_s(out_info->id, "sample.vehicle_cosmetic");
    strcpy_s(out_info->name, "RC1 Vehicle Cosmetic Sample");
    strcpy_s(out_info->version, "1.0.0-rc1");
    strcpy_s(out_info->author, "WotbMod SDK");
    strcpy_s(out_info->description,
             "Exact-path vehicle mesh/material/texture LOD overlays with rollback.");
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    return WOTBMOD_V3_OK;
}
