#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_RENDER_VERSION 1u
#define WOTBMOD_V3_RENDER_NATIVE_VERSION 1u

typedef enum WotbModV3RenderBackend {
    WOTBMOD_V3_RENDER_BACKEND_NONE = 0,
    WOTBMOD_V3_RENDER_BACKEND_D3D11 = 1,
    WOTBMOD_V3_RENDER_BACKEND_D3D12 = 2,
    WOTBMOD_V3_RENDER_BACKEND_VULKAN = 3,
    WOTBMOD_V3_RENDER_BACKEND_METAL = 4,
    WOTBMOD_V3_RENDER_BACKEND_OPENGL = 5
} WotbModV3RenderBackend;

typedef enum WotbModV3RenderPhase {
    WOTBMOD_V3_RENDER_PHASE_BEFORE_UI = 0,
    WOTBMOD_V3_RENDER_PHASE_AFTER_UI = 1,
    WOTBMOD_V3_RENDER_PHASE_PRESENT = 2
} WotbModV3RenderPhase;

typedef enum WotbModV3RenderResourceType {
    WOTBMOD_V3_RENDER_RESOURCE_TEXTURE = 1,
    WOTBMOD_V3_RENDER_RESOURCE_MATERIAL = 2
} WotbModV3RenderResourceType;

typedef enum WotbModV3TextureFormat {
    WOTBMOD_V3_TEXTURE_RGBA8_UNORM = 1,
    WOTBMOD_V3_TEXTURE_BGRA8_UNORM = 2,
    WOTBMOD_V3_TEXTURE_R8_UNORM = 3,
    WOTBMOD_V3_TEXTURE_RGBA16_FLOAT = 4
} WotbModV3TextureFormat;

typedef enum WotbModV3RenderParameterType {
    WOTBMOD_V3_RENDER_PARAMETER_FLOAT = 1,
    WOTBMOD_V3_RENDER_PARAMETER_VEC2 = 2,
    WOTBMOD_V3_RENDER_PARAMETER_VEC3 = 3,
    WOTBMOD_V3_RENDER_PARAMETER_VEC4 = 4,
    WOTBMOD_V3_RENDER_PARAMETER_COLOR = 5,
    WOTBMOD_V3_RENDER_PARAMETER_MATRIX4 = 6,
    WOTBMOD_V3_RENDER_PARAMETER_TEXTURE = 7
} WotbModV3RenderParameterType;

typedef enum WotbModV3RenderLifecycleChange {
    WOTBMOD_V3_RENDER_LIFECYCLE_BACKEND = 1u << 0,
    WOTBMOD_V3_RENDER_LIFECYCLE_DEVICE = 1u << 1,
    WOTBMOD_V3_RENDER_LIFECYCLE_CONTEXT = 1u << 2,
    WOTBMOD_V3_RENDER_LIFECYCLE_SWAPCHAIN = 1u << 3,
    WOTBMOD_V3_RENDER_LIFECYCLE_VIEWPORT = 1u << 4,
    WOTBMOD_V3_RENDER_LIFECYCLE_AVAILABILITY = 1u << 5
} WotbModV3RenderLifecycleChange;

/*
 * Public payload for wotbmod.render.* lifecycle events. Native COM pointers
 * are deliberately omitted. component_changes only says which observed
 * Present-side identities changed, so reviewed mods can rebuild managed
 * resources without gaining native access.
 */
typedef struct WotbModV3RenderLifecycleEvent {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t previous_backend;
    uint32_t backend;
    uint32_t component_changes;
    uint32_t previous_device_available;
    uint32_t device_available;
    uint32_t reserved;
    uint64_t frame_index;
    WotbModV3Rect previous_viewport;
    WotbModV3Rect viewport;
} WotbModV3RenderLifecycleEvent;

typedef struct WotbModV3RenderFrameInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t phase;
    uint32_t backend;
    uint64_t frame_index;
    double delta_seconds;
    WotbModV3Rect viewport;
} WotbModV3RenderFrameInfo;

typedef struct WotbModV3TextureDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t row_pitch;
    uint32_t dynamic;
    uint32_t reserved;
    const void* initial_data;
    uint32_t initial_data_size;
    const char* debug_name;
} WotbModV3TextureDescriptor;

typedef struct WotbModV3MaterialDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    const char* shader_uri;
    const char* debug_name;
    uint32_t blend_enabled;
    uint32_t depth_test_enabled;
} WotbModV3MaterialDescriptor;

typedef struct WotbModV3RenderParameter {
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
        WotbModV3RenderHandle texture;
    } value;
} WotbModV3RenderParameter;

typedef struct WotbModV3DrawSprite {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3RenderHandle texture;
    WotbModV3RenderHandle material;
    WotbModV3Rect destination;
    WotbModV3Rect source_uv;
    WotbModV3Color color;
    float rotation_radians;
    float z;
} WotbModV3DrawSprite;

typedef struct WotbModV3DrawText {
    uint32_t struct_size;
    uint32_t api_version;
    const char* text;
    const char* font_uri;
    WotbModV3Vec2 position;
    WotbModV3Color color;
    float font_size;
    float max_width;
} WotbModV3DrawText;

typedef struct WotbModV3DrawLine {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Vec3 from;
    WotbModV3Vec3 to;
    WotbModV3Color color;
    float width;
} WotbModV3DrawLine;

typedef struct WotbModV3DrawMesh {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3SceneHandle scene_entity;
    WotbModV3RenderHandle material;
    WotbModV3Matrix4 world;
} WotbModV3DrawMesh;

typedef void(WOTBMOD_V3_CALL* WotbModV3RenderCallback)(
    WotbModV3Handle mod,
    const WotbModV3RenderFrameInfo* frame,
    void* user_data);

typedef struct WotbModV3RenderApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* register_callback)(
        WotbModV3Handle mod,
        uint32_t phase,
        int32_t priority,
        WotbModV3RenderCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unregister_callback)(
        WotbModV3Handle mod,
        WotbModV3Token token);
    WotbModV3Result(WOTBMOD_V3_CALL* set_callback_priority)(
        WotbModV3Handle mod,
        WotbModV3Token token,
        int32_t priority);
    WotbModV3Result(WOTBMOD_V3_CALL* get_backend)(
        WotbModV3Handle mod,
        uint32_t* out_backend);
    WotbModV3Result(WOTBMOD_V3_CALL* get_viewport)(
        WotbModV3Handle mod,
        WotbModV3Rect* out_viewport);
    WotbModV3Result(WOTBMOD_V3_CALL* get_frame_index)(
        WotbModV3Handle mod,
        uint64_t* out_frame_index);
    WotbModV3Result(WOTBMOD_V3_CALL* get_delta_time)(
        WotbModV3Handle mod,
        double* out_delta_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* create_texture)(
        WotbModV3Handle mod,
        const WotbModV3TextureDescriptor* descriptor,
        WotbModV3RenderHandle* out_texture);
    WotbModV3Result(WOTBMOD_V3_CALL* update_texture)(
        WotbModV3Handle mod,
        WotbModV3RenderHandle texture,
        const WotbModV3Rect* region,
        const void* data,
        uint32_t data_size,
        uint32_t row_pitch);
    WotbModV3Result(WOTBMOD_V3_CALL* destroy_texture)(
        WotbModV3Handle mod,
        WotbModV3RenderHandle texture);
    WotbModV3Result(WOTBMOD_V3_CALL* create_material)(
        WotbModV3Handle mod,
        const WotbModV3MaterialDescriptor* descriptor,
        WotbModV3RenderHandle* out_material);
    WotbModV3Result(WOTBMOD_V3_CALL* set_material_parameter)(
        WotbModV3Handle mod,
        WotbModV3RenderHandle material,
        const WotbModV3RenderParameter* parameter);
    WotbModV3Result(WOTBMOD_V3_CALL* destroy_material)(
        WotbModV3Handle mod,
        WotbModV3RenderHandle material);
    WotbModV3Result(WOTBMOD_V3_CALL* draw_sprite)(
        WotbModV3Handle mod,
        const WotbModV3DrawSprite* draw);
    WotbModV3Result(WOTBMOD_V3_CALL* draw_text)(
        WotbModV3Handle mod,
        const WotbModV3DrawText* draw);
    WotbModV3Result(WOTBMOD_V3_CALL* draw_line)(
        WotbModV3Handle mod,
        const WotbModV3DrawLine* draw);
    WotbModV3Result(WOTBMOD_V3_CALL* draw_mesh)(
        WotbModV3Handle mod,
        const WotbModV3DrawMesh* draw);
    WotbModV3Result(WOTBMOD_V3_CALL* push_state)(
        WotbModV3Handle mod);
    WotbModV3Result(WOTBMOD_V3_CALL* pop_state)(
        WotbModV3Handle mod);
} WotbModV3RenderApiV1;

typedef struct WotbModV3RenderNativeApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_native_device)(
        WotbModV3Handle mod,
        void** out_device);
    WotbModV3Result(WOTBMOD_V3_CALL* get_native_context)(
        WotbModV3Handle mod,
        void** out_context);
    WotbModV3Result(WOTBMOD_V3_CALL* get_native_swapchain)(
        WotbModV3Handle mod,
        void** out_swapchain);
} WotbModV3RenderNativeApiV1;

#ifdef __cplusplus
}
#endif
