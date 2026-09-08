#pragma once

#include <stdint.h>

#include "../../include/wotbmod/base.h"
#include "../../include/wotbmod/catalog_v1.h"
#include "../../include/wotbmod/manifest_v1.h"
#include "data_services_backend.h"

namespace wotbmod {
namespace v3 {

#define WOTBMOD_V3_PACKAGE_PREFLIGHT_VERSION 3u
#define WOTBMOD_V3_PACKAGE_PREFLIGHT_MAX_CANDIDATES 256u

enum PackageSourceKind : uint32_t {
    PACKAGE_SOURCE_DIRECTORY = 1u,
    PACKAGE_SOURCE_SIDECAR_MANIFEST = 2u,
    PACKAGE_SOURCE_STORED_WOTBMOD_ARCHIVE = 3u
};

enum PackagePlanStatus : uint32_t {
    PACKAGE_PLAN_READY = 1u,
    PACKAGE_PLAN_BLOCKED = 2u
};

enum PackagePreflightFlags : uint32_t {
    PACKAGE_PREFLIGHT_ALLOW_UNREVIEWED = 1u << 0,
    PACKAGE_PREFLIGHT_REQUIRE_CATALOG_RECORD = 1u << 1,
    PACKAGE_PREFLIGHT_MATERIALIZE_STORED_ARCHIVES = 1u << 2,
    PACKAGE_PREFLIGHT_REQUIRE_TRUSTED_SIGNATURE = 1u << 3
};

enum PackageWarningFlags : uint32_t {
    PACKAGE_WARNING_NONE = 0u,
    PACKAGE_WARNING_UNREVIEWED = 1u << 0,
    PACKAGE_WARNING_UNSAFE_PERMISSION_TIER = 1u << 1,
    PACKAGE_WARNING_EMBEDDED_SIGNATURE_UNVERIFIED = 1u << 2,
    PACKAGE_WARNING_UNSIGNED = 1u << 3,
    PACKAGE_WARNING_UNTRUSTED_SIGNER = 1u << 4
};

struct PackagePreflightLimits {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t max_candidates;
    uint32_t max_archive_entries;
    uint32_t max_archive_depth;
    uint32_t reserved;
    uint64_t max_archive_bytes;
    uint64_t max_archive_unpacked_bytes;
    uint64_t max_archive_single_file_bytes;
    uint64_t max_directory_package_bytes;
};

struct PackagePermissionGrant {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t max_permission_tier;
    uint32_t reserved;
    char id[WOTBMOD_V3_MAX_ID];
};

struct PackagePreflightOptions {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t flags;
    uint32_t max_permission_tier;
    char mods_root[WOTBMOD_V3_MAX_PATH];
    char archive_staging_root[WOTBMOD_V3_MAX_PATH];
    char client_build[WOTBMOD_V3_MAX_NAME];
    char client_executable_sha256[65];
    const PackagePermissionGrant* permission_grants;
    uint32_t permission_grant_count;
    uint32_t reserved_permission;
    const WotbModV3InstalledMod* installed_mods;
    uint32_t installed_mod_count;
    uint32_t reserved0;
    const WotbModV3CatalogRecord* catalog_records;
    uint32_t catalog_record_count;
    uint32_t reserved1;
    PackagePreflightLimits limits;
};

struct PackagePlanEntry {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t source_kind;
    uint32_t package_type;
    uint32_t status;
    uint32_t result;
    uint32_t requested_permission_tier;
    uint32_t granted_permission_tier;
    uint32_t catalog_status;
    uint32_t warning_flags;
    uint32_t permission_count;
    uint32_t dependency_count;
    uint32_t load_order;
    uint32_t archive_compression_method;
    uint32_t signature_status;
    char id[WOTBMOD_V3_MAX_ID];
    char name[WOTBMOD_V3_MAX_NAME];
    char version[WOTBMOD_V3_MAX_VERSION];
    char developer[WOTBMOD_V3_MAX_NAME];
    char source_path[WOTBMOD_V3_MAX_PATH];
    char manifest_path[WOTBMOD_V3_MAX_PATH];
    char entrypoint_path[WOTBMOD_V3_MAX_PATH];
    char content_path[WOTBMOD_V3_MAX_PATH];
    char package_sha256[65];
    char payload_sha256[65];
    char signature_key_id[WOTBMOD_V3_MAX_ID];
    char permissions
        [WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS]
        [WOTBMOD_V3_MAX_PERMISSION_NAME];
    char reason[WOTBMOD_V3_MAX_MESSAGE];
};

struct PackagePlanSummary {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t discovered_count;
    uint32_t ready_count;
    uint32_t blocked_count;
    uint32_t warning_count;
    uint32_t required_capacity;
    uint32_t reserved;
    char reason[WOTBMOD_V3_MAX_MESSAGE];
};

PackagePreflightLimits DefaultPackagePreflightLimits();

WotbModV3Result Sha256FileUtf8(
    const char* physical_file,
    char out_sha256[65]);

WotbModV3Result BuildPackageLoadPlan(
    const PackagePreflightOptions* options,
    PackagePlanEntry* out_entries,
    uint32_t entry_capacity,
    PackagePlanSummary* out_summary);

}  // namespace v3
}  // namespace wotbmod
