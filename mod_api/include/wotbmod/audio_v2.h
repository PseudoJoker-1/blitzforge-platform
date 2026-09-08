#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_AUDIO_VERSION 2u

typedef enum WotbModV3AudioState {
    WOTBMOD_V3_AUDIO_STATE_CREATED = 0,
    WOTBMOD_V3_AUDIO_STATE_PRELOADED = 1,
    WOTBMOD_V3_AUDIO_STATE_PLAYING = 2,
    WOTBMOD_V3_AUDIO_STATE_PAUSED = 3,
    WOTBMOD_V3_AUDIO_STATE_STOPPED = 4,
    WOTBMOD_V3_AUDIO_STATE_ERROR = 5
} WotbModV3AudioState;

typedef enum WotbModV3AudioCreateFlag {
    WOTBMOD_V3_AUDIO_CREATE_NONE = 0,
    WOTBMOD_V3_AUDIO_CREATE_SPATIAL = 1u << 0,
    WOTBMOD_V3_AUDIO_CREATE_LOOP = 1u << 1,
    WOTBMOD_V3_AUDIO_CREATE_STREAM = 1u << 2
} WotbModV3AudioCreateFlag;

typedef struct WotbModV3AudioDescriptor {
    uint32_t struct_size;
    uint32_t api_version;
    const char* uri;
    uint32_t flags;
    uint32_t priority;
    float volume;
    float pitch;
    const char* bus;
} WotbModV3AudioDescriptor;

typedef struct WotbModV3AudioError {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3AudioHandle audio;
    uint32_t code;
    char message[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3AudioError;

typedef struct WotbModV3SoundOverrideConflict {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Token override_token;
    WotbModV3Handle owner_mod;
    int32_t priority;
    char event_name[WOTBMOD_V3_MAX_NAME];
    char replacement_uri[WOTBMOD_V3_MAX_PATH];
} WotbModV3SoundOverrideConflict;

typedef void(WOTBMOD_V3_CALL* WotbModV3AudioLifecycleCallback)(
    WotbModV3Handle mod,
    WotbModV3AudioHandle audio,
    void* user_data);

typedef void(WOTBMOD_V3_CALL* WotbModV3AudioErrorCallback)(
    WotbModV3Handle mod,
    const WotbModV3AudioError* error,
    void* user_data);

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3SoundConflictVisitor)(
    WotbModV3Handle mod,
    const WotbModV3SoundOverrideConflict* conflict,
    void* user_data);

typedef struct WotbModV3AudioApiV2 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* create)(
        WotbModV3Handle mod,
        const WotbModV3AudioDescriptor* descriptor,
        WotbModV3AudioHandle* out_audio);
    WotbModV3Result(WOTBMOD_V3_CALL* create_stream)(
        WotbModV3Handle mod,
        const WotbModV3AudioDescriptor* descriptor,
        WotbModV3AudioHandle* out_audio);
    WotbModV3Result(WOTBMOD_V3_CALL* preload)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio);
    WotbModV3Result(WOTBMOD_V3_CALL* play)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio);
    WotbModV3Result(WOTBMOD_V3_CALL* pause)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio);
    WotbModV3Result(WOTBMOD_V3_CALL* resume)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio);
    WotbModV3Result(WOTBMOD_V3_CALL* stop)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        float fade_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* destroy)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio);
    WotbModV3Result(WOTBMOD_V3_CALL* is_playing)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        uint32_t* out_playing);
    WotbModV3Result(WOTBMOD_V3_CALL* get_state)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        uint32_t* out_state);
    WotbModV3Result(WOTBMOD_V3_CALL* set_volume)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        float volume);
    WotbModV3Result(WOTBMOD_V3_CALL* set_pitch)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        float pitch);
    WotbModV3Result(WOTBMOD_V3_CALL* set_pan)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        float pan);
    WotbModV3Result(WOTBMOD_V3_CALL* set_bus)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        const char* bus);
    WotbModV3Result(WOTBMOD_V3_CALL* set_position_3d)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        WotbModV3Vec3 position);
    WotbModV3Result(WOTBMOD_V3_CALL* set_min_distance)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        float distance);
    WotbModV3Result(WOTBMOD_V3_CALL* set_max_distance)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        float distance);
    WotbModV3Result(WOTBMOD_V3_CALL* fade_to)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        float target_volume,
        float duration_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* seek)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        double seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* get_position)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        double* out_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* get_duration)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        double* out_seconds);
    WotbModV3Result(WOTBMOD_V3_CALL* set_loop)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        uint32_t loop);
    WotbModV3Result(WOTBMOD_V3_CALL* on_started)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        WotbModV3AudioLifecycleCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* on_finished)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        WotbModV3AudioLifecycleCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* on_error)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle audio,
        WotbModV3AudioErrorCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unsubscribe)(
        WotbModV3Handle mod,
        WotbModV3Token token);

    WotbModV3Result(WOTBMOD_V3_CALL* sound_override_register)(
        WotbModV3Handle mod,
        const char* event_name,
        const char* replacement_uri,
        int32_t priority,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_override_unregister)(
        WotbModV3Handle mod,
        WotbModV3Token token);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_override_set_priority)(
        WotbModV3Handle mod,
        WotbModV3Token token,
        int32_t priority);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_override_get_conflicts)(
        WotbModV3Handle mod,
        const char* event_name,
        WotbModV3SoundConflictVisitor visitor,
        void* user_data);

    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_create)(
        WotbModV3Handle mod,
        const char* event_name,
        WotbModV3AudioHandle* out_event);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_trigger)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_stop)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        uint32_t force);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_paused)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        uint32_t paused);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_volume)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        float volume);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_position)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        WotbModV3Vec3 position);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_parameter)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        const char* name,
        float value);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_get_parameter)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        const char* name,
        float* out_value);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_has_parameter)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        const char* name,
        uint32_t* out_has_parameter);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_get_name)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_get_bus)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_speed)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        float speed);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_direction)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        WotbModV3Vec3 direction);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_velocity)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        WotbModV3Vec3 velocity);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_loop_count)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        int32_t loop_count);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_set_priority)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event,
        int32_t priority);
    WotbModV3Result(WOTBMOD_V3_CALL* sound_event_destroy)(
        WotbModV3Handle mod,
        WotbModV3AudioHandle event);

    WotbModV3Result(WOTBMOD_V3_CALL* set_listener_transform)(
        WotbModV3Handle mod,
        const WotbModV3Transform* transform);
    WotbModV3Result(WOTBMOD_V3_CALL* set_bus_volume)(
        WotbModV3Handle mod,
        const char* bus,
        float volume);
} WotbModV3AudioApiV2;

#ifdef __cplusplus
}
#endif
