#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_ASYNC_VERSION 1u
#define WOTBMOD_V3_MAX_TASK_RESULT (1024u * 1024u)

typedef enum WotbModV3TaskState {
    WOTBMOD_V3_TASK_QUEUED = 0,
    WOTBMOD_V3_TASK_RUNNING = 1,
    WOTBMOD_V3_TASK_COMPLETED = 2,
    WOTBMOD_V3_TASK_FAILED = 3,
    WOTBMOD_V3_TASK_CANCELLED = 4
} WotbModV3TaskState;

typedef struct WotbModV3TaskInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t state;
    uint32_t completion_thread_role;
    float progress;
    uint32_t result_code;
    uint32_t result_size;
    uint32_t cancellation_requested;
    char description[WOTBMOD_V3_MAX_NAME];
} WotbModV3TaskInfo;

typedef WotbModV3Result(WOTBMOD_V3_CALL* WotbModV3TaskWorkCallback)(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task,
    void* user_data);

typedef void(WOTBMOD_V3_CALL* WotbModV3TaskCompletionCallback)(
    WotbModV3Handle mod,
    WotbModV3TaskHandle task,
    WotbModV3Result result,
    void* user_data);

typedef void(WOTBMOD_V3_CALL* WotbModV3DispatchCallback)(
    WotbModV3Handle mod,
    void* user_data);

typedef void(WOTBMOD_V3_CALL* WotbModV3TimerCallback)(
    WotbModV3Handle mod,
    WotbModV3TimerHandle timer,
    uint64_t fire_count,
    void* user_data);

typedef struct WotbModV3TaskSubmitInfo {
    uint32_t struct_size;
    uint32_t api_version;
    const char* description;
    WotbModV3TaskWorkCallback work;
    WotbModV3TaskCompletionCallback completion;
    void* user_data;
    uint32_t completion_thread_role;
    uint32_t reserved;
} WotbModV3TaskSubmitInfo;

typedef struct WotbModV3TimerCreateInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t delay_ms;
    uint64_t interval_ms;
    uint32_t repeating;
    uint32_t callback_thread_role;
    WotbModV3TimerCallback callback;
    void* user_data;
} WotbModV3TimerCreateInfo;

typedef struct WotbModV3TimerInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t interval_ms;
    uint64_t fire_count;
    uint64_t remaining_ms;
    uint32_t repeating;
    uint32_t paused;
    uint32_t cancelled;
    uint32_t callback_thread_role;
} WotbModV3TimerInfo;

typedef struct WotbModV3AsyncApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* task_submit)(
        WotbModV3Handle mod,
        const WotbModV3TaskSubmitInfo* info,
        WotbModV3TaskHandle* out_task);
    WotbModV3Result(WOTBMOD_V3_CALL* task_cancel)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task);
    WotbModV3Result(WOTBMOD_V3_CALL* task_get_info)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task,
        WotbModV3TaskInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* task_get_state)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task,
        uint32_t* out_state);
    WotbModV3Result(WOTBMOD_V3_CALL* task_get_progress)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task,
        float* out_progress);
    WotbModV3Result(WOTBMOD_V3_CALL* task_set_progress)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task,
        float progress);
    WotbModV3Result(WOTBMOD_V3_CALL* task_set_result)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task,
        const void* data,
        uint32_t size);
    WotbModV3Result(WOTBMOD_V3_CALL* task_get_result)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task,
        WotbModV3Buffer* out_result);
    WotbModV3Result(WOTBMOD_V3_CALL* task_is_cancellation_requested)(
        WotbModV3Handle mod,
        WotbModV3TaskHandle task,
        uint32_t* out_requested);
    WotbModV3Result(WOTBMOD_V3_CALL* dispatch_to_main_thread)(
        WotbModV3Handle mod,
        WotbModV3DispatchCallback callback,
        void* user_data,
        WotbModV3TaskHandle* out_task);
    WotbModV3Result(WOTBMOD_V3_CALL* dispatch_to_render_thread)(
        WotbModV3Handle mod,
        WotbModV3DispatchCallback callback,
        void* user_data,
        WotbModV3TaskHandle* out_task);
    WotbModV3Result(WOTBMOD_V3_CALL* dispatch_to_audio_thread)(
        WotbModV3Handle mod,
        WotbModV3DispatchCallback callback,
        void* user_data,
        WotbModV3TaskHandle* out_task);
    WotbModV3Result(WOTBMOD_V3_CALL* pump_current_thread)(
        WotbModV3Handle mod,
        uint32_t max_callbacks,
        uint32_t* out_executed);
    WotbModV3Result(WOTBMOD_V3_CALL* timer_create)(
        WotbModV3Handle mod,
        const WotbModV3TimerCreateInfo* info,
        WotbModV3TimerHandle* out_timer);
    WotbModV3Result(WOTBMOD_V3_CALL* timer_cancel)(
        WotbModV3Handle mod,
        WotbModV3TimerHandle timer);
    WotbModV3Result(WOTBMOD_V3_CALL* timer_pause)(
        WotbModV3Handle mod,
        WotbModV3TimerHandle timer);
    WotbModV3Result(WOTBMOD_V3_CALL* timer_resume)(
        WotbModV3Handle mod,
        WotbModV3TimerHandle timer);
    WotbModV3Result(WOTBMOD_V3_CALL* timer_get_info)(
        WotbModV3Handle mod,
        WotbModV3TimerHandle timer,
        WotbModV3TimerInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* thread_get_current_role)(
        WotbModV3Handle mod,
        uint32_t* out_role);
    WotbModV3Result(WOTBMOD_V3_CALL* thread_is_main)(
        WotbModV3Handle mod,
        uint32_t* out_is_main);
} WotbModV3AsyncApiV1;

#ifdef __cplusplus
}
#endif
