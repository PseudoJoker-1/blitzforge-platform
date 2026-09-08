#pragma once

#include "ui_v2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_UI_VERSION_3 3u

typedef enum WotbModV3UiControlSnapshotFlag {
    WOTBMOD_V3_UI_SNAPSHOT_VISIBLE = 1u << 0,
    WOTBMOD_V3_UI_SNAPSHOT_ENABLED = 1u << 1,
    WOTBMOD_V3_UI_SNAPSHOT_INTERACTABLE = 1u << 2,
    WOTBMOD_V3_UI_SNAPSHOT_FOCUSED = 1u << 3,
    /* The handle wraps an existing DAVA UIControl owned by the game. */
    WOTBMOD_V3_UI_SNAPSHOT_GAME_OWNED = 1u << 4
} WotbModV3UiControlSnapshotFlag;

typedef struct WotbModV3UiControlSnapshot {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3UiHandle control;
    WotbModV3UiHandle parent;
    uint32_t type;
    uint32_t flags;
    WotbModV3Rect geometry;
    int32_t z_order;
    uint32_t child_count;
    char id[WOTBMOD_V3_MAX_ID];
} WotbModV3UiControlSnapshot;

/*
 * V3 preserves the complete WotbModV3UiApiV2 binary prefix. The embedded
 * prefix keeps the C declaration maintainable while retaining byte-for-byte
 * compatibility: a V3 table may be cast to WotbModV3UiApiV2.
 */
typedef struct WotbModV3UiApiV3 {
    WotbModV3UiApiV2 v2;

    /* Requires the reviewed ui.modify.game permission. */
    WotbModV3Result(WOTBMOD_V3_CALL* get_active_screen)(
        WotbModV3Handle mod,
        WotbModV3UiHandle* out_screen);

    /* Refreshes native state before returning the snapshot. */
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_snapshot)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3UiControlSnapshot* out_snapshot);
} WotbModV3UiApiV3;

#ifdef __cplusplus
}
#endif
