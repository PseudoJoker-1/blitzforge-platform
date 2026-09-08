#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_SETTINGS_VERSION 1u
#define WOTBMOD_V3_SETTING_KEY_MAX 96u
#define WOTBMOD_V3_SETTING_TEXT_MAX 512u

typedef enum WotbModV3SettingType {
    WOTBMOD_V3_SETTING_BOOL = 1,
    WOTBMOD_V3_SETTING_INT = 2,
    WOTBMOD_V3_SETTING_FLOAT = 3,
    WOTBMOD_V3_SETTING_STRING = 4,
    WOTBMOD_V3_SETTING_ENUM = 5,
    WOTBMOD_V3_SETTING_COLOR = 6,
    WOTBMOD_V3_SETTING_KEYBIND = 7,
    WOTBMOD_V3_SETTING_FILE = 8,
    WOTBMOD_V3_SETTING_FOLDER = 9,
    WOTBMOD_V3_SETTING_TITLE = 10,
    WOTBMOD_V3_SETTING_BUTTON = 11,
    WOTBMOD_V3_SETTING_CUSTOM = 12
} WotbModV3SettingType;

typedef enum WotbModV3SettingFlags {
    WOTBMOD_V3_SETTING_FLAG_NONE = 0,
    WOTBMOD_V3_SETTING_FLAG_REQUIRES_RESTART = 1u << 0,
    WOTBMOD_V3_SETTING_FLAG_READ_ONLY = 1u << 1,
    WOTBMOD_V3_SETTING_FLAG_HIDDEN = 1u << 2
} WotbModV3SettingFlags;

typedef struct WotbModV3SettingDefinition {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t flags;
    char key[WOTBMOD_V3_SETTING_KEY_MAX];
    char title[WOTBMOD_V3_MAX_NAME];
    char description[WOTBMOD_V3_MAX_MESSAGE];
    char default_text[WOTBMOD_V3_SETTING_TEXT_MAX];
    char enum_values[WOTBMOD_V3_SETTING_TEXT_MAX];
    char visible_if[WOTBMOD_V3_SETTING_TEXT_MAX];
    char enabled_if[WOTBMOD_V3_SETTING_TEXT_MAX];
    int64_t default_int;
    int64_t min_int;
    int64_t max_int;
    int64_t step_int;
    double default_float;
    double min_float;
    double max_float;
    double step_float;
    WotbModV3Color default_color;
    uint32_t default_bool;
    uint32_t platform_mask;
} WotbModV3SettingDefinition;

typedef struct WotbModV3SettingPresetValue {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t bool_value;
    char key[WOTBMOD_V3_SETTING_KEY_MAX];
    char text_value[WOTBMOD_V3_SETTING_TEXT_MAX];
    int64_t int_value;
    double float_value;
    WotbModV3Color color_value;
} WotbModV3SettingPresetValue;

typedef struct WotbModV3SettingPreset {
    uint32_t struct_size;
    uint32_t api_version;
    char id[WOTBMOD_V3_SETTING_KEY_MAX];
    char name[WOTBMOD_V3_MAX_NAME];
    const WotbModV3SettingPresetValue* values;
    uint32_t value_count;
    uint32_t reserved;
} WotbModV3SettingPreset;

typedef void(WOTBMOD_V3_CALL* WotbModV3SettingChangedCallback)(
    WotbModV3Handle mod,
    const char* key,
    uint32_t type,
    void* user_data);

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3SettingsMigrationCallback)(
    WotbModV3Handle mod,
    uint32_t old_version,
    uint32_t new_version,
    void* user_data);

typedef struct WotbModV3SettingsApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* register_schema)(
        WotbModV3Handle mod,
        uint32_t schema_version,
        const WotbModV3SettingDefinition* definitions,
        uint32_t definition_count);
    WotbModV3Result(WOTBMOD_V3_CALL* register_preset)(
        WotbModV3Handle mod,
        const WotbModV3SettingPreset* preset);
    WotbModV3Result(WOTBMOD_V3_CALL* get_schema_version)(
        WotbModV3Handle mod,
        uint32_t* out_version);
    WotbModV3Result(WOTBMOD_V3_CALL* get_bool)(
        WotbModV3Handle mod,
        const char* key,
        uint32_t* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* get_int)(
        WotbModV3Handle mod,
        const char* key,
        int64_t* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* get_float)(
        WotbModV3Handle mod,
        const char* key,
        double* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* get_string)(
        WotbModV3Handle mod,
        const char* key,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_color)(
        WotbModV3Handle mod,
        const char* key,
        WotbModV3Color* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* set_bool)(
        WotbModV3Handle mod,
        const char* key,
        uint32_t value);
    WotbModV3Result(WOTBMOD_V3_CALL* set_int)(
        WotbModV3Handle mod,
        const char* key,
        int64_t value);
    WotbModV3Result(WOTBMOD_V3_CALL* set_float)(
        WotbModV3Handle mod,
        const char* key,
        double value);
    WotbModV3Result(WOTBMOD_V3_CALL* set_string)(
        WotbModV3Handle mod,
        const char* key,
        const char* value);
    WotbModV3Result(WOTBMOD_V3_CALL* set_color)(
        WotbModV3Handle mod,
        const char* key,
        const WotbModV3Color* value);
    WotbModV3Result(WOTBMOD_V3_CALL* reset)(
        WotbModV3Handle mod,
        const char* key);
    WotbModV3Result(WOTBMOD_V3_CALL* reset_all)(WotbModV3Handle mod);
    WotbModV3Result(WOTBMOD_V3_CALL* apply_preset)(
        WotbModV3Handle mod,
        const char* preset_id);
    WotbModV3Result(WOTBMOD_V3_CALL* subscribe)(
        WotbModV3Handle mod,
        WotbModV3SettingChangedCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unsubscribe)(
        WotbModV3Handle mod,
        WotbModV3Token token);
    WotbModV3Result(WOTBMOD_V3_CALL* run_migration)(
        WotbModV3Handle mod,
        uint32_t target_version,
        WotbModV3SettingsMigrationCallback callback,
        void* user_data);
} WotbModV3SettingsApiV1;

#ifdef __cplusplus
}
#endif
