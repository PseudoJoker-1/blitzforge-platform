#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_VFS_VERSION 1u
#define WOTBMOD_V3_VFS_URI_MAX WOTBMOD_V3_MAX_PATH
#define WOTBMOD_V3_VFS_WATCH_EVENT_VERSION 1u
#define WOTBMOD_V3_EVENT_VFS_INVALIDATED "wotbmod.vfs.invalidated"

typedef enum WotbModV3VfsEntryType {
    WOTBMOD_V3_VFS_ENTRY_FILE = 1,
    WOTBMOD_V3_VFS_ENTRY_DIRECTORY = 2
} WotbModV3VfsEntryType;

typedef enum WotbModV3VfsMountKind {
    WOTBMOD_V3_VFS_MOUNT_PACKAGE = 1,
    WOTBMOD_V3_VFS_MOUNT_OVERLAY = 2
} WotbModV3VfsMountKind;

typedef enum WotbModV3VfsChangeKind {
    WOTBMOD_V3_VFS_CHANGE_CREATED = 1,
    WOTBMOD_V3_VFS_CHANGE_MODIFIED = 2,
    WOTBMOD_V3_VFS_CHANGE_DELETED = 3
} WotbModV3VfsChangeKind;

typedef struct WotbModV3VfsWatchEvent {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Token watch_token;
    uint32_t change_kind;
    uint32_t previous_exists;
    uint32_t exists;
    uint32_t is_directory;
    uint64_t previous_size;
    uint64_t size;
    uint64_t previous_modified_unix_ms;
    uint64_t modified_unix_ms;
    char uri[WOTBMOD_V3_VFS_URI_MAX];
} WotbModV3VfsWatchEvent;

typedef struct WotbModV3VfsStat {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t read_only;
    uint64_t size;
    uint64_t modified_unix_ms;
    char resolved_uri[WOTBMOD_V3_VFS_URI_MAX];
    char provider[WOTBMOD_V3_MAX_ID];
} WotbModV3VfsStat;

typedef struct WotbModV3VfsListEntry {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t type;
    uint32_t reserved;
    uint64_t size;
    char name[WOTBMOD_V3_MAX_NAME];
    char uri[WOTBMOD_V3_VFS_URI_MAX];
} WotbModV3VfsListEntry;

typedef struct WotbModV3VfsProvider {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Handle mount;
    WotbModV3Handle owner_mod;
    int32_t priority;
    uint32_t kind;
    char provider_id[WOTBMOD_V3_MAX_ID];
    char target_uri[WOTBMOD_V3_VFS_URI_MAX];
} WotbModV3VfsProvider;

typedef struct WotbModV3VfsConflict {
    uint32_t struct_size;
    uint32_t api_version;
    char uri[WOTBMOD_V3_VFS_URI_MAX];
    uint32_t provider_count;
    uint32_t reserved;
} WotbModV3VfsConflict;

typedef struct WotbModV3VfsApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* get_namespace)(
        WotbModV3Handle mod,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* normalize_uri)(
        WotbModV3Handle mod,
        const char* uri,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* mount_package)(
        WotbModV3Handle mod,
        const char* provider_id,
        const char* physical_directory,
        int32_t priority,
        WotbModV3Handle* out_mount);
    WotbModV3Result(WOTBMOD_V3_CALL* mount_overlay)(
        WotbModV3Handle mod,
        const char* provider_id,
        const char* target_game_uri,
        const char* source_mod_uri,
        int32_t priority,
        WotbModV3Handle* out_mount);
    WotbModV3Result(WOTBMOD_V3_CALL* unmount)(
        WotbModV3Handle mod,
        WotbModV3Handle mount);
    WotbModV3Result(WOTBMOD_V3_CALL* resolve)(
        WotbModV3Handle mod,
        const char* uri,
        char* physical_path,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* open)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3Handle* out_file);
    WotbModV3Result(WOTBMOD_V3_CALL* read)(
        WotbModV3Handle mod,
        WotbModV3Handle file,
        uint64_t offset,
        WotbModV3Buffer* inout_buffer);
    WotbModV3Result(WOTBMOD_V3_CALL* list)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3VfsListEntry* entries,
        uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* stat)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3VfsStat* out_stat);
    WotbModV3Result(WOTBMOD_V3_CALL* watch)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3Token* out_token);
    WotbModV3Result(WOTBMOD_V3_CALL* get_providers)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3VfsProvider* providers,
        uint32_t* inout_count);
    WotbModV3Result(WOTBMOD_V3_CALL* set_provider_priority)(
        WotbModV3Handle mod,
        WotbModV3Handle mount,
        int32_t priority);
    WotbModV3Result(WOTBMOD_V3_CALL* get_conflicts)(
        WotbModV3Handle mod,
        WotbModV3VfsConflict* conflicts,
        uint32_t* inout_count);
} WotbModV3VfsApiV1;

#ifdef __cplusplus
}
#endif
