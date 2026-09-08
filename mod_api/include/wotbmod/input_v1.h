#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_INPUT_VERSION 1u
#define WOTBMOD_V3_INPUT_ACTION_ID_MAX 96u
#define WOTBMOD_V3_INPUT_BINDINGS_MAX 8u

/* Keyboard bindings use platform-neutral Win32 virtual-key values on
 * Windows (for example 0x70 for F1 and 0x41 for A). Mouse values below are
 * stable WotbMod API codes and are not Windows message identifiers. */
#define WOTBMOD_V3_INPUT_MOUSE_LEFT 1u
#define WOTBMOD_V3_INPUT_MOUSE_RIGHT 2u
#define WOTBMOD_V3_INPUT_MOUSE_MIDDLE 3u
#define WOTBMOD_V3_INPUT_MOUSE_X1 4u
#define WOTBMOD_V3_INPUT_MOUSE_X2 5u
#define WOTBMOD_V3_INPUT_MOUSE_WHEEL 0x100u
#define WOTBMOD_V3_INPUT_MOUSE_MOVE_X 0x101u
#define WOTBMOD_V3_INPUT_MOUSE_MOVE_Y 0x102u

typedef enum WotbModV3InputDevice {
    WOTBMOD_V3_INPUT_DEVICE_KEYBOARD = 1,
    WOTBMOD_V3_INPUT_DEVICE_MOUSE = 2,
    WOTBMOD_V3_INPUT_DEVICE_GAMEPAD = 3,
    WOTBMOD_V3_INPUT_DEVICE_TOUCH = 4
} WotbModV3InputDevice;

typedef enum WotbModV3InputValueType {
    WOTBMOD_V3_INPUT_VALUE_BUTTON = 1,
    WOTBMOD_V3_INPUT_VALUE_AXIS = 2
} WotbModV3InputValueType;

typedef enum WotbModV3InputModifiers {
    WOTBMOD_V3_INPUT_MOD_NONE = 0,
    WOTBMOD_V3_INPUT_MOD_SHIFT = 1u << 0,
    WOTBMOD_V3_INPUT_MOD_CONTROL = 1u << 1,
    WOTBMOD_V3_INPUT_MOD_ALT = 1u << 2,
    WOTBMOD_V3_INPUT_MOD_META = 1u << 3
} WotbModV3InputModifiers;

typedef struct WotbModV3InputBinding {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t device;
    uint32_t code;
    uint32_t modifiers;
    uint32_t reserved;
    float scale;
} WotbModV3InputBinding;

typedef struct WotbModV3InputActionDesc {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t value_type;
    uint32_t reserved;
    uint64_t contexts;
    char id[WOTBMOD_V3_INPUT_ACTION_ID_MAX];
    char display_name[WOTBMOD_V3_MAX_NAME];
    char description[WOTBMOD_V3_MAX_MESSAGE];
    const WotbModV3InputBinding* default_bindings;
    uint32_t default_binding_count;
    uint32_t reserved2;
} WotbModV3InputActionDesc;

typedef struct WotbModV3InputConflict {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle owner_mod;
    WotbModV3Handle action;
    char action_id[WOTBMOD_V3_INPUT_ACTION_ID_MAX];
    WotbModV3InputBinding binding;
    uint64_t overlapping_contexts;
} WotbModV3InputConflict;

typedef void(WOTBMOD_V3_CALL* WotbModV3InputActionCallback)(
    WotbModV3Handle mod,
    WotbModV3Handle action,
    float value,
    uint32_t pressed,
    void* user_data);

typedef struct WotbModV3InputApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* register_action)(
        WotbModV3Handle mod,
        const WotbModV3InputActionDesc* desc,
        WotbModV3Handle* out_action);
    WotbModV3Result(WOTBMOD_V3_CALL* unregister_action)(
        WotbModV3Handle mod,
        WotbModV3Handle action);
    WotbModV3Result(WOTBMOD_V3_CALL* set_contexts)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        uint64_t contexts);
    WotbModV3Result(WOTBMOD_V3_CALL* get_bindings)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        WotbModV3InputBinding* bindings,
        uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* set_bindings)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        const WotbModV3InputBinding* bindings,
        uint32_t binding_count);
    WotbModV3Result(WOTBMOD_V3_CALL* subscribe)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        WotbModV3InputActionCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* is_action_down)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        uint32_t* out_down);
    WotbModV3Result(WOTBMOD_V3_CALL* is_action_pressed)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        uint32_t* out_pressed);
    WotbModV3Result(WOTBMOD_V3_CALL* get_axis)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        float* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* capture_begin)(
        WotbModV3Handle mod,
        uint64_t contexts);
    WotbModV3Result(WOTBMOD_V3_CALL* capture_end)(
        WotbModV3Handle mod,
        WotbModV3InputBinding* out_binding);
    WotbModV3Result(WOTBMOD_V3_CALL* find_conflicts)(
        WotbModV3Handle mod,
        WotbModV3Handle action,
        WotbModV3InputConflict* conflicts,
        uint32_t* inout_count);
} WotbModV3InputApiV1;

#ifdef __cplusplus
}
#endif
