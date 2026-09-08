#pragma once
#include "base.h"

/*
 * wotbmod.session.cluster -- the login cluster of the current region:
 * enumerate the region's clusters, read the one the client is connected to,
 * and ask the client to reconnect to another one (or to its own automatic
 * choice) without restarting the process. The client does the switch itself
 * (LoginManager::ChangeCluster); this interface only exposes it.
 *
 * Permissions: "session.cluster.read" (SAFE) for enumerate/get_current and
 * the changed event, "session.cluster.change" (REVIEWED) for change().
 * Backend: the loader captures the client's LoginManager on its first login;
 * before that, and on a client build without the backend, every slot answers
 * WOTBMOD_V3_E_NOT_SUPPORTED and the interface is UNAVAILABLE/DEGRADED.
 *
 * Design: docs/superpowers/specs/2026-09-07-cluster-picker-design.md
 */
#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_SESSION_CLUSTER_VERSION 1u
/* cluster_id passed to change(): let the client pick (its stock auto-select). */
#define WOTBMOD_V3_SESSION_CLUSTER_AUTO (-1)

#ifndef WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED
#define WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED "wotbmod.session.cluster.changed"
#endif

typedef struct WotbModV3ClusterInfo {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t cluster_id;             /* id from regions yaml; never -1 in enumerate() */
    uint32_t current;               /* 1 = the client is connected here now */
    uint32_t alive;                 /* descriptor flag: the host answered */
    uint32_t allowed;               /* descriptor flag: login allowed here */
    int32_t ccu;                    /* players online, -1 when the client does not know */
    char name[WOTBMOD_V3_MAX_NAME]; /* "EU_C3" */
} WotbModV3ClusterInfo;

typedef enum WotbModV3ClusterChangeStatus {
    WOTBMOD_V3_CLUSTER_CHANGE_QUEUED = 1,    /* accepted, waiting for the main thread */
    WOTBMOD_V3_CLUSTER_CHANGE_STARTED = 2,   /* ChangeCluster was called */
    WOTBMOD_V3_CLUSTER_CHANGE_CONNECTED = 3, /* back in the hangar, current == to */
    WOTBMOD_V3_CLUSTER_CHANGE_FAILED = 4     /* see the loader log; manual flag cleared */
} WotbModV3ClusterChangeStatus;

typedef struct WotbModV3ClusterChangedEvent {
    uint32_t struct_size;
    uint32_t api_version;
    int32_t from_cluster_id;
    int32_t to_cluster_id;          /* WOTBMOD_V3_SESSION_CLUSTER_AUTO = auto */
    uint32_t status;                /* WotbModV3ClusterChangeStatus */
    uint32_t reserved;
} WotbModV3ClusterChangedEvent;

typedef struct WotbModV3SessionClusterApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    /* Two-pass: items NULL with *inout_count 0 asks for the count. */
    WotbModV3Result(WOTBMOD_V3_CALL* enumerate)(
        WotbModV3Handle mod, WotbModV3ClusterInfo* items, uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* get_current)(
        WotbModV3Handle mod, WotbModV3ClusterInfo* out_info);
    /* HANGAR only; E_BUSY while a change is in flight or within 10 s of the
     * previous one; E_NOT_FOUND for an unknown id; E_CONFLICT for a cluster
     * that is not alive/allowed. The outcome arrives as
     * WOTBMOD_V3_EVENT_SESSION_CLUSTER_CHANGED. */
    WotbModV3Result(WOTBMOD_V3_CALL* change)(
        WotbModV3Handle mod, int32_t cluster_id);
} WotbModV3SessionClusterApiV1;

#ifdef __cplusplus
}
#endif
