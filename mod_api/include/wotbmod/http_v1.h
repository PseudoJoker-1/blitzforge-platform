#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_HTTP_VERSION 1u
#define WOTBMOD_V3_HTTP_DEFAULT_MAX_RESPONSE (8u * 1024u * 1024u)
#define WOTBMOD_V3_HTTP_ABSOLUTE_MAX_RESPONSE (64u * 1024u * 1024u)

typedef enum WotbModV3HttpState {
    WOTBMOD_V3_HTTP_CREATED = 0,
    WOTBMOD_V3_HTTP_CONFIGURED = 1,
    WOTBMOD_V3_HTTP_SENDING = 2,
    WOTBMOD_V3_HTTP_COMPLETED = 3,
    WOTBMOD_V3_HTTP_FAILED = 4,
    WOTBMOD_V3_HTTP_CANCELLED = 5
} WotbModV3HttpState;

typedef struct WotbModV3HttpRequestInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t state;
    uint32_t timeout_ms;
    uint32_t max_response_size;
    uint32_t response_status;
    uint32_t response_size;
    uint32_t reserved;
    char method[16];
    char url[WOTBMOD_V3_MAX_PATH];
} WotbModV3HttpRequestInfo;

typedef void(WOTBMOD_V3_CALL* WotbModV3HttpCompletionCallback)(
    WotbModV3Handle mod,
    WotbModV3HttpHandle request,
    WotbModV3Result result,
    void* user_data);

typedef struct WotbModV3HttpApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* request_create)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle* out_request);
    WotbModV3Result(WOTBMOD_V3_CALL* request_set_method)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        const char* method);
    WotbModV3Result(WOTBMOD_V3_CALL* request_set_url)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        const char* url);
    WotbModV3Result(WOTBMOD_V3_CALL* request_set_header)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        const char* name,
        const char* value);
    WotbModV3Result(WOTBMOD_V3_CALL* request_set_body)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        const void* data,
        uint32_t size);
    WotbModV3Result(WOTBMOD_V3_CALL* request_set_timeout)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        uint32_t timeout_ms);
    WotbModV3Result(WOTBMOD_V3_CALL* request_set_max_response_size)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        uint32_t max_response_size);
    WotbModV3Result(WOTBMOD_V3_CALL* request_send_async)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        WotbModV3HttpCompletionCallback completion,
        void* user_data);
    WotbModV3Result(WOTBMOD_V3_CALL* request_cancel)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request);
    WotbModV3Result(WOTBMOD_V3_CALL* request_get_info)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        WotbModV3HttpRequestInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* response_get_status)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        uint32_t* out_status);
    WotbModV3Result(WOTBMOD_V3_CALL* response_get_header)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        const char* name,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* response_get_body)(
        WotbModV3Handle mod,
        WotbModV3HttpHandle request,
        WotbModV3Buffer* out_body);
} WotbModV3HttpApiV1;

#ifdef __cplusplus
}
#endif
