#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_VEHICLE_VISUAL_VERSION 1u

typedef enum WotbModV3VehicleAppearanceState {
    WOTBMOD_V3_VEHICLE_APPEARANCE_UNKNOWN = 0,
    WOTBMOD_V3_VEHICLE_APPEARANCE_LOADING = 1,
    WOTBMOD_V3_VEHICLE_APPEARANCE_READY = 2,
    WOTBMOD_V3_VEHICLE_APPEARANCE_DESTROYED = 3
} WotbModV3VehicleAppearanceState;

typedef enum WotbModV3VehiclePart {
    WOTBMOD_V3_VEHICLE_PART_HULL = 1,
    WOTBMOD_V3_VEHICLE_PART_TURRET = 2,
    WOTBMOD_V3_VEHICLE_PART_GUN = 3,
    WOTBMOD_V3_VEHICLE_PART_TRACKS = 4,
    WOTBMOD_V3_VEHICLE_PART_CHASSIS = 5,
    WOTBMOD_V3_VEHICLE_PART_EFFECTS = 6
} WotbModV3VehiclePart;

typedef struct WotbModV3VehicleVisualProfile {
    uint32_t struct_size;
    uint32_t api_version;
    const char* id;
    const char* vehicle_name;
    const char* hull_mesh_uri;
    const char* turret_mesh_uri;
    const char* gun_mesh_uri;
    const char* chassis_mesh_uri;
    const char* material_overlay_uri;
    const char* texture_pack_uri;
    int32_t priority;
    uint32_t hangar_only;
} WotbModV3VehicleVisualProfile;

typedef struct WotbModV3VehicleAttachment {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3EntityHandle vehicle;
    WotbModV3SceneHandle entity;
    const char* attachment_point;
    WotbModV3Transform local_transform;
    uint32_t lifetime_policy;
    uint32_t reserved;
} WotbModV3VehicleAttachment;

typedef struct WotbModV3VehicleVisualApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_local_player_vehicle)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle* out_vehicle);
    WotbModV3Result(WOTBMOD_V3_CALL* is_local_player)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t* out_is_local);
    WotbModV3Result(WOTBMOD_V3_CALL* is_hangar_vehicle)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t* out_is_hangar);
    WotbModV3Result(WOTBMOD_V3_CALL* get_appearance_state)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t* out_state);
    WotbModV3Result(WOTBMOD_V3_CALL* get_enemy)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t* out_is_enemy);
    WotbModV3Result(WOTBMOD_V3_CALL* get_enemy_name)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_position)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        WotbModV3Vec3* out_position);
    WotbModV3Result(WOTBMOD_V3_CALL* get_part_entity)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t part,
        WotbModV3SceneHandle* out_entity);
    WotbModV3Result(WOTBMOD_V3_CALL* find_attachment_point)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* point_name,
        WotbModV3Transform* out_world_transform);
    WotbModV3Result(WOTBMOD_V3_CALL* attach_entity_ex)(
        WotbModV3Handle mod,
        const WotbModV3VehicleAttachment* attachment);
    WotbModV3Result(WOTBMOD_V3_CALL* set_decal_override)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* slot,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_texture_override)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* material_path,
        const char* texture_slot,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_color_override)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* material_path,
        WotbModV3Color color);
    WotbModV3Result(WOTBMOD_V3_CALL* get_available_animation)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t index,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* play_animation)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* animation,
        float blend_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* restore_appearance)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle);
    WotbModV3Result(WOTBMOD_V3_CALL* set_part_visible)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t part,
        uint32_t visible);
    WotbModV3Result(WOTBMOD_V3_CALL* set_material_override)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* material_path,
        const char* replacement_material_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* skin_apply_to_entity)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* skin_uri);

    WotbModV3Result(WOTBMOD_V3_CALL* set_custom_skin)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* texture_pack_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_custom_camouflage)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* camouflage_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_emblem)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t slot,
        const char* texture_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_inscription)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        uint32_t slot,
        const char* text,
        const char* font_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_engine_sound)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* audio_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* set_gun_sound)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* audio_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* play_hangar_animation)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* animation);
    WotbModV3Result(WOTBMOD_V3_CALL* set_hangar_idle_animation)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle vehicle,
        const char* animation);

    WotbModV3Result(WOTBMOD_V3_CALL* profile_register)(
        WotbModV3Handle mod,
        const WotbModV3VehicleVisualProfile* profile,
        WotbModV3Handle* out_profile);
    WotbModV3Result(WOTBMOD_V3_CALL* profile_apply)(
        WotbModV3Handle mod,
        WotbModV3Handle profile,
        WotbModV3EntityHandle vehicle);
    WotbModV3Result(WOTBMOD_V3_CALL* profile_release)(
        WotbModV3Handle mod,
        WotbModV3Handle profile);
} WotbModV3VehicleVisualApiV1;

#ifdef __cplusplus
}
#endif
