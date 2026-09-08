#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_SCENE_VERSION 1u

typedef enum WotbModV3SceneAttachmentPolicy {
    WOTBMOD_V3_ATTACHMENT_DETACH_ON_PARENT_DESTROY = 1u << 0,
    WOTBMOD_V3_ATTACHMENT_DESTROY_ON_PARENT_DESTROY = 1u << 1,
    WOTBMOD_V3_ATTACHMENT_KEEP_WORLD_TRANSFORM = 1u << 2,
    WOTBMOD_V3_ATTACHMENT_KEEP_LOCAL_TRANSFORM = 1u << 3
} WotbModV3SceneAttachmentPolicy;

typedef enum WotbModV3SceneParameterType {
    WOTBMOD_V3_SCENE_PARAMETER_FLOAT = 1,
    WOTBMOD_V3_SCENE_PARAMETER_VEC2 = 2,
    WOTBMOD_V3_SCENE_PARAMETER_VEC3 = 3,
    WOTBMOD_V3_SCENE_PARAMETER_VEC4 = 4,
    WOTBMOD_V3_SCENE_PARAMETER_COLOR = 5,
    WOTBMOD_V3_SCENE_PARAMETER_MATRIX4 = 6,
    WOTBMOD_V3_SCENE_PARAMETER_TEXTURE_URI = 7
} WotbModV3SceneParameterType;

typedef struct WotbModV3SceneBounds {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Vec3 minimum;
    WotbModV3Vec3 maximum;
} WotbModV3SceneBounds;

typedef struct WotbModV3SceneEntityDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    const char* name;
    WotbModV3Transform transform;
    uint32_t render_layer;
    int32_t render_order;
} WotbModV3SceneEntityDescriptor;

typedef struct WotbModV3SceneParameter {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t reserved;
    const char* name;
    union {
        float scalar;
        WotbModV3Vec2 vec2;
        WotbModV3Vec3 vec3;
        WotbModV3Vec4 vec4;
        WotbModV3Color color;
        WotbModV3Matrix4 matrix4;
        const char* texture_uri;
    } value;
} WotbModV3SceneParameter;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3SceneNameVisitor)(
    WotbModV3Handle mod,
    const char* name,
    void* user_data);

typedef struct WotbModV3SceneApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* entity_create)(
        WotbModV3Handle mod,
        const WotbModV3SceneEntityDescriptor* descriptor,
        WotbModV3SceneHandle* out_entity);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_load)(
        WotbModV3Handle mod,
        const char* resource_uri,
        WotbModV3SceneHandle* out_entity);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_clone)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneHandle* out_clone);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_destroy)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity);
    WotbModV3Result(WOTBMOD_V3_CALL* get_active_scene)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle* out_scene);

    WotbModV3Result(WOTBMOD_V3_CALL* entity_get_parent)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneHandle* out_parent);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_parent)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneHandle parent);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_add_child)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneHandle child);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_remove_child)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneHandle child);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_attach_ex)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneHandle parent,
        const char* node,
        uint32_t policy);

    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_transform)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const WotbModV3Transform* transform);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_get_transform)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3Transform* out_transform);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_get_world_transform)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3Transform* out_transform);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_world_transform)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const WotbModV3Transform* transform);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_get_bounds)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneBounds* out_bounds);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_render_layer)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        uint32_t layer);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_render_order)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        int32_t order);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_lod_bias)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        float bias);

    WotbModV3Result(WOTBMOD_V3_CALL* entity_list_nodes)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneNameVisitor visitor,
        void* user_data);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_find_node_by_path)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const char* path,
        WotbModV3SceneHandle* out_node);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_list_animations)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        WotbModV3SceneNameVisitor visitor,
        void* user_data);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_get_animation_duration)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const char* animation,
        float* out_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_animation_speed)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const char* animation,
        float speed);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_animation_loop)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const char* animation,
        uint32_t loop);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_blend_animation)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const char* from_animation,
        const char* to_animation,
        float duration_seconds);

    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_material_parameter)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const char* material_path,
        const WotbModV3SceneParameter* parameter);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_clear_material_parameter)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const char* material_path,
        const char* parameter_name);
    WotbModV3Result(WOTBMOD_V3_CALL* entity_set_shader_parameter)(
        WotbModV3Handle mod,
        WotbModV3SceneHandle entity,
        const WotbModV3SceneParameter* parameter);
} WotbModV3SceneApiV1;

#ifdef __cplusplus
}
#endif
