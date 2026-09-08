#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_BIGWORLD_RPC_VERSION 1u

#ifndef WOTBMOD_V3_EVENT_RPC_OBSERVED
#define WOTBMOD_V3_EVENT_RPC_OBSERVED \
    "wotbmod.bigworld.rpc.observed"
#endif

/*
 * This interface intentionally exposes metadata-only observation of reviewed
 * RPC labels already received through the stock client. The current native
 * bridge supports incoming observations only; outgoing subscriptions return
 * WOTBMOD_V3_E_NOT_SUPPORTED. It never exposes packet payload bytes and
 * contains no send/modify/drop/replay entry.
 */
typedef enum WotbModV3RpcDirection {
    WOTBMOD_V3_RPC_INCOMING = 1,
    WOTBMOD_V3_RPC_OUTGOING = 2
} WotbModV3RpcDirection;

typedef struct WotbModV3ObservedRpc {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t direction;
    uint32_t public_entity_id;
    uint64_t sequence;
    uint64_t timestamp_microseconds;
    char entity_type[WOTBMOD_V3_MAX_NAME];
    char method_name[WOTBMOD_V3_MAX_NAME];
} WotbModV3ObservedRpc;

typedef void(WOTBMOD_V3_CALL* WotbModV3RpcObservedCallback)(
    WotbModV3Handle mod,
    const WotbModV3ObservedRpc* rpc,
    void* user_data);

typedef struct WotbModV3BigWorldRpcPolicy {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t metadata_observation;
    uint32_t payload_access;
    uint32_t outgoing_injection;
    uint32_t packet_modification;
    uint32_t packet_drop;
    uint32_t packet_replay;
} WotbModV3BigWorldRpcPolicy;

typedef struct WotbModV3BigWorldRpcApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_policy)(
        WotbModV3Handle mod,
        WotbModV3BigWorldRpcPolicy* out_policy);
    WotbModV3Result(WOTBMOD_V3_CALL* subscribe_observed)(
        WotbModV3Handle mod,
        uint32_t direction_mask,
        const char* method_filter,
        WotbModV3RpcObservedCallback callback,
        void* user_data,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* unsubscribe_observed)(
        WotbModV3Handle mod,
        WotbModV3Token token);
} WotbModV3BigWorldRpcApiV1;

#ifdef __cplusplus
}
#endif
