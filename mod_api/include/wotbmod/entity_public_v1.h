#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_ENTITY_PUBLIC_VERSION 1u

#ifndef WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED
#define WOTBMOD_V3_EVENT_PUBLIC_ENTITY_ADDED \
    "wotbmod.entity.public.added"
#endif
#ifndef WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED
#define WOTBMOD_V3_EVENT_PUBLIC_ENTITY_UPDATED \
    "wotbmod.entity.public.updated"
#endif
#ifndef WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED
#define WOTBMOD_V3_EVENT_PUBLIC_ENTITY_REMOVED \
    "wotbmod.entity.public.removed"
#endif

typedef enum WotbModV3PublicEntityType {
    WOTBMOD_V3_PUBLIC_ENTITY_UNKNOWN = 0,
    WOTBMOD_V3_PUBLIC_ENTITY_VEHICLE = 1,
    WOTBMOD_V3_PUBLIC_ENTITY_PROJECTILE = 2,
    WOTBMOD_V3_PUBLIC_ENTITY_EFFECT = 3
} WotbModV3PublicEntityType;

typedef enum WotbModV3PublicValueType {
    WOTBMOD_V3_PUBLIC_VALUE_BOOL = 1,
    WOTBMOD_V3_PUBLIC_VALUE_INT64 = 2,
    WOTBMOD_V3_PUBLIC_VALUE_DOUBLE = 3,
    WOTBMOD_V3_PUBLIC_VALUE_VEC3 = 4,
    WOTBMOD_V3_PUBLIC_VALUE_STRING = 5
} WotbModV3PublicValueType;

typedef struct WotbModV3PublicValue {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t reserved;
    union {
        uint32_t boolean;
        int64_t integer;
        double number;
        WotbModV3Vec3 vec3;
        char string_value[WOTBMOD_V3_MAX_NAME];
    } value;
} WotbModV3PublicValue;

typedef struct WotbModV3PublicEntitySnapshot {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3EntityHandle handle;
    uint32_t public_id;
    uint32_t type;
    uint32_t visible_to_player;
    uint32_t local_player;
    uint32_t team;
    int32_t health;
    int32_t max_health;
    WotbModV3Vec3 position;
    WotbModV3Vec3 direction;
    char public_type[WOTBMOD_V3_MAX_NAME];
    char display_name[WOTBMOD_V3_MAX_NAME];
} WotbModV3PublicEntitySnapshot;

typedef enum WotbModV3PublicEntityChangeReason {
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_UNKNOWN = 0,
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_LOCAL_PLAYER = 1,
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_VISIBLE = 2,
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_UPDATED = 3,
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_HIDDEN = 4,
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_LEFT_WORLD = 5,
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_NATIVE_DESTROYED = 6,
    WOTBMOD_V3_PUBLIC_ENTITY_REASON_SHUTDOWN = 7
} WotbModV3PublicEntityChangeReason;

/*
 * Public entity lifecycle events contain only the same player-visible
 * snapshot exposed by enumerate_visible(). A non-local entity is removed as
 * soon as the stock client reports it unspotted.
 */
typedef struct WotbModV3PublicEntityLifecycleEvent {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t reason;
    uint32_t reserved;
    WotbModV3PublicEntitySnapshot snapshot;
} WotbModV3PublicEntityLifecycleEvent;

typedef void(WOTBMOD_V3_CALL* WotbModV3PublicPropertyCallback)(
    WotbModV3Handle mod,
    WotbModV3EntityHandle entity,
    const char* property,
    const WotbModV3PublicValue* value,
    void* user_data);

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3PublicEntityVisitor)(
    WotbModV3Handle mod,
    const WotbModV3PublicEntitySnapshot* entity,
    void* user_data);

typedef struct WotbModV3EntityPublicApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_public_id)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle entity,
        uint32_t* out_public_id);
    WotbModV3Result(WOTBMOD_V3_CALL* get_public_type)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle entity,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_public_property)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle entity,
        const char* property,
        WotbModV3PublicValue* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* subscribe_public_property)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle entity,
        const char* property,
        WotbModV3PublicPropertyCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unsubscribe_public_property)(
        WotbModV3Handle mod,
        WotbModV3Token token);
    WotbModV3Result(WOTBMOD_V3_CALL* is_visible_to_player)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle entity,
        uint32_t* out_visible);
    WotbModV3Result(WOTBMOD_V3_CALL* get_snapshot)(
        WotbModV3Handle mod,
        WotbModV3EntityHandle entity,
        WotbModV3PublicEntitySnapshot* out_snapshot);
    WotbModV3Result(WOTBMOD_V3_CALL* enumerate_visible)(
        WotbModV3Handle mod,
        WotbModV3PublicEntityVisitor visitor,
        void* user_data);
} WotbModV3EntityPublicApiV1;

#ifdef __cplusplus
}
#endif
