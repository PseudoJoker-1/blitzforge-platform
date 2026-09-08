#pragma once

#include "vehicle_visual_v1.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2 2u
#define WOTBMOD_V3_VEHICLE_SKIN_MAX_ASSETS 64u

typedef enum WotbModV3VehicleSkinAssetKind {
    WOTBMOD_V3_VEHICLE_SKIN_MESH = 1,
    WOTBMOD_V3_VEHICLE_SKIN_MATERIAL = 2,
    WOTBMOD_V3_VEHICLE_SKIN_TEXTURE = 3
} WotbModV3VehicleSkinAssetKind;

typedef struct WotbModV3VehicleSkinAsset {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t kind;
    uint32_t part;
    /* -1 means the non-LOD-specific resource; 0..15 identifies an LOD. */
    int32_t lod;
    uint32_t reserved;
    /* Exact game:// path intercepted by the loader File::Create bridge. */
    const char* stock_game_uri;
    /* Existing mod://, data:// or cache:// replacement file. */
    const char* replacement_uri;
} WotbModV3VehicleSkinAsset;

typedef struct WotbModV3VehicleSkinPack {
    uint32_t struct_size;
    uint32_t api_version;
    const char* id;
    const char* vehicle_name;
    const WotbModV3VehicleSkinAsset* assets;
    uint32_t asset_count;
    int32_t priority;
    uint32_t hangar_only;
    uint32_t reserved;
} WotbModV3VehicleSkinPack;

typedef struct WotbModV3VehicleSkinState {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t applied;
    uint32_t asset_count;
    uint32_t mounted_asset_count;
    uint32_t requires_model_reload;
    WotbModV3EntityHandle vehicle;
} WotbModV3VehicleSkinState;

/*
 * V2 preserves the V1 binary prefix. Replacement is resource-path based:
 * every mesh/material/texture and every LOD is an explicit exact-path entry.
 * Applying a pack is transactional; rollback removes all of its overlays.
 */
typedef struct WotbModV3VehicleVisualApiV2 {
    WotbModV3VehicleVisualApiV1 v1;
    WotbModV3Result(WOTBMOD_V3_CALL* skin_pack_register)(
        WotbModV3Handle mod,
        const WotbModV3VehicleSkinPack* pack,
        WotbModV3Handle* out_pack);
    WotbModV3Result(WOTBMOD_V3_CALL* skin_pack_apply)(
        WotbModV3Handle mod,
        WotbModV3Handle pack,
        WotbModV3EntityHandle vehicle);
    WotbModV3Result(WOTBMOD_V3_CALL* skin_pack_rollback)(
        WotbModV3Handle mod,
        WotbModV3Handle pack);
    WotbModV3Result(WOTBMOD_V3_CALL* skin_pack_get_state)(
        WotbModV3Handle mod,
        WotbModV3Handle pack,
        WotbModV3VehicleSkinState* out_state);
    WotbModV3Result(WOTBMOD_V3_CALL* skin_pack_release)(
        WotbModV3Handle mod,
        WotbModV3Handle pack);
} WotbModV3VehicleVisualApiV2;

#ifdef __cplusplus
}
#endif
