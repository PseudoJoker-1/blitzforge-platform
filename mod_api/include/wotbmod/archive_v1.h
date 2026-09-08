#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_ARCHIVE_VERSION 1u

typedef struct WotbModV3ArchiveLimits {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t max_depth;
    uint32_t max_files;
    uint64_t max_total_unpacked_bytes;
    uint64_t max_single_file_bytes;
    char expected_sha256[65];
} WotbModV3ArchiveLimits;

typedef struct WotbModV3ArchiveEntry {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t is_directory;
    uint32_t depth;
    uint64_t size;
    char relative_path[WOTBMOD_V3_MAX_PATH];
    char sha256[65];
} WotbModV3ArchiveEntry;

typedef struct WotbModV3ArchiveApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* open_directory)(
        WotbModV3Handle mod,
        const char* physical_directory,
        const WotbModV3ArchiveLimits* limits,
        WotbModV3ArchiveHandle* out_archive);
    WotbModV3Result(WOTBMOD_V3_CALL* open_package_file)(
        WotbModV3Handle mod,
        const char* physical_file,
        const WotbModV3ArchiveLimits* limits,
        WotbModV3ArchiveHandle* out_archive);
    WotbModV3Result(WOTBMOD_V3_CALL* get_entry_count)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive,
        uint32_t* out_count);
    WotbModV3Result(WOTBMOD_V3_CALL* get_entry)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive,
        uint32_t index,
        WotbModV3ArchiveEntry* out_entry);
    WotbModV3Result(WOTBMOD_V3_CALL* read_entry)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive,
        const char* relative_path,
        WotbModV3Buffer* inout_buffer);
    WotbModV3Result(WOTBMOD_V3_CALL* extract_entry)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive,
        const char* relative_path,
        const char* destination_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* extract_all)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive,
        const char* destination_uri);
    WotbModV3Result(WOTBMOD_V3_CALL* get_sha256)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive,
        char out_sha256[65]);
    WotbModV3Result(WOTBMOD_V3_CALL* verify_sha256)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive,
        const char* expected_sha256);
    WotbModV3Result(WOTBMOD_V3_CALL* cancel)(
        WotbModV3Handle mod,
        WotbModV3ArchiveHandle archive);
} WotbModV3ArchiveApiV1;

#ifdef __cplusplus
}
#endif
