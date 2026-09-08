#pragma once
#include "base.h"

/*
 * wotbmod.ges -- the client's GES::GameEventSystem exposed as system events.
 *
 * Every GES event type (601 on 11.20.0.887) is delivered through events_v1 as
 * topic "wotbmod.ges.<Owner>.<Name>" with a WotbModV3GesEvent payload. The
 * payload pointer inside is the engine's own event object and is valid ONLY
 * while the delivering callback runs; read it through the read_* slots, which
 * refuse a stale event with WOTBMOD_V3_E_OBJECT_DESTROYED.
 *
 * Permissions: "ges.observe" (SAFE) for the interface and the topics,
 * "ges.publish" (REVIEWED) for publish(). Backend: the loader registers a
 * listener in the engine per observed type; without it every slot returns
 * WOTBMOD_V3_E_NOT_SUPPORTED and the interface is UNAVAILABLE.
 *
 * Design: docs/superpowers/specs/2026-09-03-ges-event-bus-design.md
 */
#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_GES_VERSION 1u
#define WOTBMOD_V3_GES_TYPE_NAME_SIZE 128u
#define WOTBMOD_V3_GES_TOPIC_PREFIX "wotbmod.ges."

/* WotbModV3GesEvent::flags */
#define WOTBMOD_V3_GES_EVENT_MOD_PUBLISHED 1u
#define WOTBMOD_V3_GES_EVENT_SIZE_KNOWN 2u
#define WOTBMOD_V3_GES_EVENT_SCHEMA_KNOWN 4u

/* publish() flags */
#define WOTBMOD_V3_GES_PUBLISH_ECHO 1u

typedef enum WotbModV3GesFieldKind {
    WOTBMOD_V3_GES_FIELD_I32 = 1,
    WOTBMOD_V3_GES_FIELD_U32 = 2,
    WOTBMOD_V3_GES_FIELD_F32 = 3,
    WOTBMOD_V3_GES_FIELD_BOOL = 4,
    WOTBMOD_V3_GES_FIELD_PTR = 5,
    WOTBMOD_V3_GES_FIELD_CSTR = 6,
    WOTBMOD_V3_GES_FIELD_FASTNAME = 7,
    WOTBMOD_V3_GES_FIELD_BYTES = 8,
    /* One byte read through read_bool; the raw value, not a truth value
     * (small enums such as InputMode::InputModeChanged::mode). */
    WOTBMOD_V3_GES_FIELD_U8 = 9
} WotbModV3GesFieldKind;

typedef struct WotbModV3GesEvent {
    uint32_t struct_size;
    uint32_t api_version;
    char type_name[WOTBMOD_V3_GES_TYPE_NAME_SIZE]; /* "Avatar::CameraModeChanged" */
    const void* payload;      /* engine event object; delivery-scoped */
    uint32_t payload_size;    /* 0 = unknown */
    uint32_t schema_id;       /* 0 = no schema */
    uint32_t publisher_rva;   /* return address of the publish site; 0 for mod publishes */
    uint32_t flags;           /* WOTBMOD_V3_GES_EVENT_* */
} WotbModV3GesEvent;

typedef struct WotbModV3GesField {
    uint32_t struct_size;
    uint32_t api_version;
    const char* name;
    uint32_t offset;
    uint32_t kind;            /* WotbModV3GesFieldKind */
    uint32_t size;
} WotbModV3GesField;

typedef struct WotbModV3GesApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    /* Copies up to `capacity` type names ("Owner::Name") into `names`;
     * out_count receives the total. capacity 0 + names NULL is a count query. */
    WotbModV3Result(WOTBMOD_V3_CALL* list_types)(
        WotbModV3Handle mod, const char** names, uint32_t capacity,
        uint32_t* out_count);
    WotbModV3Result(WOTBMOD_V3_CALL* read_i32)(
        const WotbModV3GesEvent* event, uint32_t offset, int32_t* out);
    WotbModV3Result(WOTBMOD_V3_CALL* read_u32)(
        const WotbModV3GesEvent* event, uint32_t offset, uint32_t* out);
    WotbModV3Result(WOTBMOD_V3_CALL* read_f32)(
        const WotbModV3GesEvent* event, uint32_t offset, float* out);
    WotbModV3Result(WOTBMOD_V3_CALL* read_bool)(
        const WotbModV3GesEvent* event, uint32_t offset, uint8_t* out);
    WotbModV3Result(WOTBMOD_V3_CALL* read_ptr)(
        const WotbModV3GesEvent* event, uint32_t offset, const void** out);
    WotbModV3Result(WOTBMOD_V3_CALL* read_cstring)(
        const WotbModV3GesEvent* event, uint32_t offset, char* buffer,
        uint32_t capacity);
    WotbModV3Result(WOTBMOD_V3_CALL* get_schema)(
        const char* type_name, uint32_t* out_schema_id,
        uint32_t* out_payload_size, uint32_t* out_field_count);
    WotbModV3Result(WOTBMOD_V3_CALL* schema_field)(
        uint32_t schema_id, uint32_t index, WotbModV3GesField* out_field);
    WotbModV3Result(WOTBMOD_V3_CALL* publish)(
        WotbModV3Handle mod, const char* type_name, const void* payload,
        uint32_t payload_size, uint32_t flags);
} WotbModV3GesApiV1;

#ifdef __cplusplus
}
#endif
