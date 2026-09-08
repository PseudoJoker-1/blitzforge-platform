#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define WOTBMOD_V3_EXTERN_C extern "C"
#else
#define WOTBMOD_V3_EXTERN_C extern
#endif

#if defined(_MSC_VER)
#define WOTBMOD_V3_CALL __cdecl
#define WOTBMOD_V3_EXPORT WOTBMOD_V3_EXTERN_C __declspec(dllexport)
#else
#define WOTBMOD_V3_CALL
#define WOTBMOD_V3_EXPORT WOTBMOD_V3_EXTERN_C
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_ABI_VERSION 0x00030000u
#define WOTBMOD_V3_SDK_VERSION 0x00030000u
#define WOTBMOD_V3_BOOTSTRAP_VERSION 1u

#define WOTBMOD_V3_MAX_ID 96u
#define WOTBMOD_V3_MAX_NAME 128u
#define WOTBMOD_V3_MAX_VERSION 48u
#define WOTBMOD_V3_MAX_PATH 1024u
#define WOTBMOD_V3_MAX_MESSAGE 512u
#define WOTBMOD_V3_MAX_CONTEXT_JSON 2048u
#define WOTBMOD_V3_MAX_INTERFACE_NAME 96u
#define WOTBMOD_V3_MAX_CAPABILITY_NAME 128u
#define WOTBMOD_V3_MAX_PERMISSION_NAME 128u

typedef uint64_t WotbModV3Handle;
typedef uint64_t WotbModV3Token;
typedef uint64_t WotbModV3TaskHandle;
typedef uint64_t WotbModV3TimerHandle;
typedef uint64_t WotbModV3EventToken;
typedef uint64_t WotbModV3HookHandle;
typedef uint64_t WotbModV3ResourceHandle;
typedef uint64_t WotbModV3UiHandle;
typedef uint64_t WotbModV3SceneHandle;
typedef uint64_t WotbModV3AudioHandle;
typedef uint64_t WotbModV3RenderHandle;
typedef uint64_t WotbModV3CameraHandle;
typedef uint64_t WotbModV3EntityHandle;
typedef uint64_t WotbModV3ProjectileHandle;
typedef uint64_t WotbModV3ArchiveHandle;
typedef uint64_t WotbModV3HttpHandle;

#define WOTBMOD_V3_INVALID_HANDLE ((uint64_t)0)

typedef enum WotbModV3Result {
    WOTBMOD_V3_OK = 0,
    WOTBMOD_V3_E_INVALID_ARGUMENT = 1,
    WOTBMOD_V3_E_INVALID_HANDLE = 2,
    WOTBMOD_V3_E_NOT_SUPPORTED = 3,
    WOTBMOD_V3_E_NOT_FOUND = 4,
    WOTBMOD_V3_E_ALREADY_EXISTS = 5,
    WOTBMOD_V3_E_WRONG_THREAD = 6,
    WOTBMOD_V3_E_PERMISSION_DENIED = 7,
    WOTBMOD_V3_E_CLIENT_MISMATCH = 8,
    WOTBMOD_V3_E_OBJECT_DESTROYED = 9,
    WOTBMOD_V3_E_CONFLICT = 10,
    WOTBMOD_V3_E_CANCELLED = 11,
    WOTBMOD_V3_E_BUFFER_TOO_SMALL = 12,
    WOTBMOD_V3_E_LIMIT_REACHED = 13,
    WOTBMOD_V3_E_BUSY = 14,
    WOTBMOD_V3_E_IO = 15,
    WOTBMOD_V3_E_PARSE = 16,
    WOTBMOD_V3_E_HASH_MISMATCH = 17,
    WOTBMOD_V3_E_SIGNATURE_INVALID = 18,
    WOTBMOD_V3_E_DEPENDENCY_MISSING = 19,
    WOTBMOD_V3_E_INCOMPATIBLE = 20,
    WOTBMOD_V3_E_CALLBACK_FAULT = 21,
    WOTBMOD_V3_E_PLATFORM = 22,
    WOTBMOD_V3_E_TIMEOUT = 23
} WotbModV3Result;

typedef enum WotbModV3ThreadRole {
    WOTBMOD_V3_THREAD_UNKNOWN = 0,
    WOTBMOD_V3_THREAD_MAIN = 1,
    WOTBMOD_V3_THREAD_RENDER = 2,
    WOTBMOD_V3_THREAD_AUDIO = 3,
    WOTBMOD_V3_THREAD_WORKER = 4,
    WOTBMOD_V3_THREAD_IO = 5
} WotbModV3ThreadRole;

typedef enum WotbModV3GameContext {
    WOTBMOD_V3_CONTEXT_NONE = 0,
    WOTBMOD_V3_CONTEXT_LOADING = 1u << 0,
    WOTBMOD_V3_CONTEXT_HANGAR = 1u << 1,
    WOTBMOD_V3_CONTEXT_BATTLE = 1u << 2,
    WOTBMOD_V3_CONTEXT_REPLAY = 1u << 3,
    WOTBMOD_V3_CONTEXT_TRAINING = 1u << 4,
    WOTBMOD_V3_CONTEXT_RESULTS = 1u << 5,
    WOTBMOD_V3_CONTEXT_MOD_SCREEN = 1u << 6,
    WOTBMOD_V3_CONTEXT_TEXT_INPUT = 1u << 7,
    WOTBMOD_V3_CONTEXT_ALL = 0x000000FFu
} WotbModV3GameContext;

typedef enum WotbModV3PermissionTier {
    WOTBMOD_V3_PERMISSION_SAFE = 0,
    WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK = 1,
    WOTBMOD_V3_PERMISSION_REVIEWED = 2,
    WOTBMOD_V3_PERMISSION_UNSAFE = 3
} WotbModV3PermissionTier;

typedef enum WotbModV3CapabilityStatus {
    WOTBMOD_V3_CAPABILITY_AVAILABLE = 0,
    WOTBMOD_V3_CAPABILITY_UNAVAILABLE = 1,
    WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH = 2,
    WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED = 3,
    WOTBMOD_V3_CAPABILITY_CONTEXT_RESTRICTED = 4,
    WOTBMOD_V3_CAPABILITY_DEGRADED = 5
} WotbModV3CapabilityStatus;

typedef enum WotbModV3HandleType {
    WOTBMOD_V3_HANDLE_UNKNOWN = 0,
    WOTBMOD_V3_HANDLE_MOD = 1,
    WOTBMOD_V3_HANDLE_RESOURCE = 2,
    WOTBMOD_V3_HANDLE_UI_CONTROL = 3,
    WOTBMOD_V3_HANDLE_UI_SLOT = 4,
    WOTBMOD_V3_HANDLE_SCENE_ENTITY = 5,
    WOTBMOD_V3_HANDLE_SCENE = 6,
    WOTBMOD_V3_HANDLE_AUDIO = 7,
    WOTBMOD_V3_HANDLE_SOUND_EVENT = 8,
    WOTBMOD_V3_HANDLE_RENDER_RESOURCE = 9,
    WOTBMOD_V3_HANDLE_CAMERA = 10,
    WOTBMOD_V3_HANDLE_ENTITY = 11,
    WOTBMOD_V3_HANDLE_PROJECTILE = 12,
    WOTBMOD_V3_HANDLE_ARCHIVE = 13,
    WOTBMOD_V3_HANDLE_TASK = 14,
    WOTBMOD_V3_HANDLE_TIMER = 15,
    WOTBMOD_V3_HANDLE_HTTP_REQUEST = 16,
    WOTBMOD_V3_HANDLE_HOOK = 17,
    WOTBMOD_V3_HANDLE_SUBSCRIPTION = 18,
    WOTBMOD_V3_HANDLE_STYLE_OVERRIDE = 19,
    WOTBMOD_V3_HANDLE_INPUT_ACTION = 20,
    WOTBMOD_V3_HANDLE_PROFILER_SPAN = 21,
    WOTBMOD_V3_HANDLE_DIAGNOSTIC_SCOPE = 22,
    WOTBMOD_V3_HANDLE_CAPABILITY_SUBSCRIPTION = 23,
    WOTBMOD_V3_HANDLE_LIFECYCLE_CLEANUP = 24,
    WOTBMOD_V3_HANDLE_EVENT_SUBSCRIPTION = 25,
    WOTBMOD_V3_HANDLE_INTERMOD_EXPORT = 26,
    WOTBMOD_V3_HANDLE_INTERMOD_SUBSCRIPTION = 27,
    WOTBMOD_V3_HANDLE_RENDER_CALLBACK = 28,
    WOTBMOD_V3_HANDLE_INPUT_BINDING = 29,
    WOTBMOD_V3_HANDLE_RESOURCE_MOUNT = 30,
    WOTBMOD_V3_HANDLE_AUDIO_OVERRIDE = 31,
    WOTBMOD_V3_HANDLE_PROJECTILE_STYLE = 32,
    WOTBMOD_V3_HANDLE_UI_EVENT_SUBSCRIPTION = 33
} WotbModV3HandleType;

typedef struct WotbModV3StructHeader {
    uint32_t struct_size;
    uint32_t api_version;
} WotbModV3StructHeader;

typedef struct WotbModV3Buffer {
    uint32_t struct_size;
    uint32_t api_version;
    void* data;
    uint32_t capacity;
    uint32_t size;
} WotbModV3Buffer;

typedef struct WotbModV3ConstBuffer {
    uint32_t struct_size;
    uint32_t api_version;
    const void* data;
    uint32_t size;
    uint32_t reserved;
} WotbModV3ConstBuffer;

typedef struct WotbModV3Vec2 {
    float x;
    float y;
} WotbModV3Vec2;

typedef struct WotbModV3Vec3 {
    float x;
    float y;
    float z;
} WotbModV3Vec3;

typedef struct WotbModV3Vec4 {
    float x;
    float y;
    float z;
    float w;
} WotbModV3Vec4;

typedef struct WotbModV3Rect {
    float x;
    float y;
    float width;
    float height;
} WotbModV3Rect;

typedef struct WotbModV3Transform {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Vec3 position;
    WotbModV3Vec4 rotation;
    WotbModV3Vec3 scale;
} WotbModV3Transform;

typedef struct WotbModV3Matrix4 {
    /* Row-major affine matrix. Translation is values[12..14]. */
    float values[16];
} WotbModV3Matrix4;

typedef struct WotbModV3Color {
    float r;
    float g;
    float b;
    float a;
} WotbModV3Color;

typedef struct WotbModV3InterfaceInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t interface_version;
    uint32_t status;
    char name[WOTBMOD_V3_MAX_INTERFACE_NAME];
    char unavailable_reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3InterfaceInfo;

typedef struct WotbModV3CapabilityInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t interface_version;
    uint32_t status;
    uint64_t allowed_contexts;
    uint32_t permission_tier;
    uint32_t reserved;
    char name[WOTBMOD_V3_MAX_CAPABILITY_NAME];
    char unavailable_reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3CapabilityInfo;

typedef struct WotbModV3HandleInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t generation;
    uint32_t reference_count;
    uint32_t alive;
    WotbModV3Handle owner_mod;
} WotbModV3HandleInfo;

typedef struct WotbModV3ErrorInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t code;
    uint32_t thread_role;
    WotbModV3Handle owner_mod;
    char message[WOTBMOD_V3_MAX_MESSAGE];
    char context_json[WOTBMOD_V3_MAX_CONTEXT_JSON];
} WotbModV3ErrorInfo;

#define WOTBMOD_V3_INIT_STRUCT(value, version_value) \
    do {                                              \
        (value).struct_size = sizeof(value);          \
        (value).api_version = (version_value);        \
    } while (0)

#ifdef __cplusplus
}
#endif
