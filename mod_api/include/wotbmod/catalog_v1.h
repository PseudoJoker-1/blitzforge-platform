#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_CATALOG_VERSION 1u

typedef enum WotbModV3CatalogStatus {
    WOTBMOD_V3_CATALOG_VERIFIED = 1,
    WOTBMOD_V3_CATALOG_COMMUNITY = 2,
    WOTBMOD_V3_CATALOG_UNREVIEWED = 3,
    WOTBMOD_V3_CATALOG_BANNED = 4
} WotbModV3CatalogStatus;

typedef struct WotbModV3CatalogRecord {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t status;
    uint32_t reserved;
    char id[WOTBMOD_V3_MAX_ID];
    char version[WOTBMOD_V3_MAX_VERSION];
    char sha256[65];
    char reason[WOTBMOD_V3_MAX_MESSAGE];
    char signer_id[WOTBMOD_V3_MAX_ID];
} WotbModV3CatalogRecord;

typedef struct WotbModV3CatalogDecision {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t status;
    uint32_t load_allowed;
    uint32_t warning_required;
    uint32_t hash_verified;
    char reason[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3CatalogDecision;

typedef struct WotbModV3CatalogApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* validate_record)(
        WotbModV3Handle mod,
        const WotbModV3CatalogRecord* record);
    WotbModV3Result(WOTBMOD_V3_CALL* evaluate_install)(
        WotbModV3Handle mod,
        const WotbModV3CatalogRecord* record,
        const char* manifest_id,
        const char* manifest_version,
        const char* package_sha256,
        WotbModV3CatalogDecision* out_decision);
    WotbModV3Result(WOTBMOD_V3_CALL* status_name)(
        WotbModV3Handle mod,
        uint32_t status,
        char* buffer,
        uint32_t* inout_size);
} WotbModV3CatalogApiV1;

#ifdef __cplusplus
}
#endif
