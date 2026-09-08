#pragma once

#include "base.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_MANIFEST_VERSION 1u

typedef enum WotbModV3PackageType {
    WOTBMOD_V3_PACKAGE_NATIVE = 1,
    WOTBMOD_V3_PACKAGE_CONTENT_ONLY = 2
} WotbModV3PackageType;

typedef enum WotbModV3DependencyKind {
    WOTBMOD_V3_DEPENDENCY_REQUIRED = 1,
    WOTBMOD_V3_DEPENDENCY_OPTIONAL = 2,
    WOTBMOD_V3_DEPENDENCY_INCOMPATIBLE = 3
} WotbModV3DependencyKind;

typedef enum WotbModV3SignatureStatus {
    WOTBMOD_V3_SIGNATURE_UNSIGNED = 0,
    WOTBMOD_V3_SIGNATURE_DECLARED = 1,
    WOTBMOD_V3_SIGNATURE_VALID = 2,
    WOTBMOD_V3_SIGNATURE_INVALID = 3,
    WOTBMOD_V3_SIGNATURE_UNSUPPORTED = 4
} WotbModV3SignatureStatus;

typedef struct WotbModV3ManifestApiRequirement {
    uint32_t struct_size;
    uint32_t api_version;
    char interface_name[WOTBMOD_V3_MAX_INTERFACE_NAME];
    char version_range[WOTBMOD_V3_MAX_VERSION];
} WotbModV3ManifestApiRequirement;

typedef struct WotbModV3ManifestInfo {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t manifest_version;
    uint32_t package_type;
    uint32_t api_requirement_count;
    uint32_t dependency_count;
    uint32_t permission_count;
    uint32_t resource_count;
    uint32_t locale_count;
    uint32_t entrypoint_count;
    uint32_t client_build_count;
    uint32_t client_executable_hash_count;
    uint32_t signature_status;
    uint32_t reserved;
    char id[WOTBMOD_V3_MAX_ID];
    char name[WOTBMOD_V3_MAX_NAME];
    char version[WOTBMOD_V3_MAX_VERSION];
    char developer[WOTBMOD_V3_MAX_NAME];
    char settings_path[WOTBMOD_V3_MAX_PATH];
    char content_descriptor_path[WOTBMOD_V3_MAX_PATH];
} WotbModV3ManifestInfo;

typedef struct WotbModV3ManifestDependency {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t kind;
    uint32_t reserved;
    char id[WOTBMOD_V3_MAX_ID];
    char version_range[WOTBMOD_V3_MAX_VERSION];
} WotbModV3ManifestDependency;

typedef struct WotbModV3ManifestEntrypoint {
    uint32_t struct_size;
    uint32_t api_version;
    char platform[WOTBMOD_V3_MAX_ID];
    char relative_path[WOTBMOD_V3_MAX_PATH];
} WotbModV3ManifestEntrypoint;

typedef struct WotbModV3InstalledMod {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t enabled;
    uint32_t reserved;
    char id[WOTBMOD_V3_MAX_ID];
    char version[WOTBMOD_V3_MAX_VERSION];
} WotbModV3InstalledMod;

typedef struct WotbModV3DependencyReport {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t missing_required;
    uint32_t version_mismatch;
    uint32_t incompatibility_count;
    uint32_t optional_missing;
    char message[WOTBMOD_V3_MAX_MESSAGE];
} WotbModV3DependencyReport;

typedef struct WotbModV3ManifestApiV1 {
    uint32_t struct_size;
    uint32_t api_version;
    WotbModV3Result(WOTBMOD_V3_CALL* parse_json)(
        WotbModV3Handle mod,
        const WotbModV3ConstBuffer* json_utf8,
        WotbModV3Handle* out_manifest);
    WotbModV3Result(WOTBMOD_V3_CALL* parse_uri)(
        WotbModV3Handle mod,
        const char* uri,
        WotbModV3Handle* out_manifest);
    WotbModV3Result(WOTBMOD_V3_CALL* validate)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest);
    WotbModV3Result(WOTBMOD_V3_CALL* get_info)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        WotbModV3ManifestInfo* out_info);
    WotbModV3Result(WOTBMOD_V3_CALL* get_api_requirement)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        WotbModV3ManifestApiRequirement* out_requirement);
    WotbModV3Result(WOTBMOD_V3_CALL* get_dependency)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        WotbModV3ManifestDependency* out_dependency);
    WotbModV3Result(WOTBMOD_V3_CALL* get_permission)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_resource_pattern)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_locale_pattern)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_entrypoint)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        WotbModV3ManifestEntrypoint* out_entrypoint);
    WotbModV3Result(WOTBMOD_V3_CALL* get_client_build)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* get_client_executable_hash)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        uint32_t index,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* resolve_dependencies)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        const WotbModV3InstalledMod* installed,
        uint32_t installed_count,
        WotbModV3DependencyReport* out_report);
    WotbModV3Result(WOTBMOD_V3_CALL* verify_content_sha256)(
        WotbModV3Handle mod,
        const char* physical_file,
        const char* expected_sha256);
    WotbModV3Result(WOTBMOD_V3_CALL* verify_signature)(
        WotbModV3Handle mod,
        WotbModV3Handle manifest,
        const char* physical_package);
} WotbModV3ManifestApiV1;

#ifdef __cplusplus
}
#endif
