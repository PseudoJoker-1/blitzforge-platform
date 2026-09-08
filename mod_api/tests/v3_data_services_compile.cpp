#include "../include/wotbmod/archive_v1.h"
#include "../include/wotbmod/catalog_v1.h"
#include "../include/wotbmod/content_v1.h"
#include "../include/wotbmod/input_v1.h"
#include "../include/wotbmod/loaders_v1.h"
#include "../include/wotbmod/manifest_v1.h"
#include "../include/wotbmod/resources_v1.h"
#include "../include/wotbmod/settings_v1.h"
#include "../include/wotbmod/storage_v1.h"
#include "../include/wotbmod/vfs_v1.h"
#include "../include/wotbmod/yaml_v1.h"
#include "../src/v3/data_services_backend.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

#define WOTBMOD_ASSERT_ABI_HEADER(type)                                  \
    static_assert(std::is_standard_layout<type>::value, #type);          \
    static_assert(offsetof(type, struct_size) == 0u, #type);             \
    static_assert(offsetof(type, api_version) == sizeof(uint32_t), #type)

WOTBMOD_ASSERT_ABI_HEADER(WotbModV3SettingDefinition);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3SettingPresetValue);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3SettingPreset);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3SettingsApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3StorageApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3InputBinding);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3InputActionDesc);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3InputConflict);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3InputApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3VfsStat);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3VfsListEntry);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3VfsProvider);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3VfsConflict);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3VfsApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ResourceLoadDesc);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ResourceInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ResourcesApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3YamlLimits);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3YamlApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ArchiveLimits);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ArchiveEntry);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ArchiveApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3LoaderBackendInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3LoadersApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ManifestApiRequirement);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ManifestInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ManifestDependency);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ManifestEntrypoint);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3InstalledMod);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3DependencyReport);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ManifestApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3CatalogRecord);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3CatalogDecision);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3CatalogApiV1);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ContentInfo);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ContentOverride);
WOTBMOD_ASSERT_ABI_HEADER(WotbModV3ContentApiV1);
WOTBMOD_ASSERT_ABI_HEADER(wotbmod::v3::DataManifestPreflightSummary);

static_assert(
    WOTBMOD_V3_RESOURCE_LOAD_DEFAULT == 0,
    "default resource loading must remain raw and side-effect free");
static_assert(
    WOTBMOD_V3_RESOURCE_BACKING_RAW_VFS_BYTES !=
        WOTBMOD_V3_RESOURCE_BACKING_NATIVE_CLIENT_OBJECT,
    "raw and native resources must have distinct backing values");
static_assert(
    std::is_same<
        decltype(WotbModV3ResourceInfo::backing),
        uint32_t>::value,
    "resource backing is part of the stable C ABI");

int main() {
#if defined(WOTBMOD_V3_DATA_SERVICES_STANDALONE_TEST)
    static const char native_manifest[] = R"json({
        "manifest_version": 1,
        "type": "native",
        "id": "tests.preflight",
        "name": "Preflight test",
        "version": "1.2.3",
        "developer": "tests",
        "api": {
            "core": "^1.0",
            "wotbmod.resources": ">=1.0 <2.0"
        },
        "client": {
            "builds": ["11.7.0.123"],
            "executable_hashes": [
                "0000000000000000000000000000000000000000000000000000000000000000"
            ]
        },
        "entrypoints": {
            "windows-x86": "bin/windows-x86/mod.dll"
        },
        "dependencies": {
            "tests.common": "^2.0"
        },
        "permissions": [
            "resources",
            "network:https://api.example.com"
        ],
        "settings": "settings.schema.json",
        "locales": ["locales/*.json"],
        "resources": ["assets/**"]
    })json";
    wotbmod::v3::DataManifestPreflightSummary native = {};
    native.struct_size = sizeof(native);
    if (wotbmod::v3::PreflightManifestUtf8(
            native_manifest,
            sizeof(native_manifest) - 1u,
            &native) != WOTBMOD_V3_OK ||
        native.package_type != WOTBMOD_V3_PACKAGE_NATIVE ||
        native.requested_permission_tier !=
            WOTBMOD_V3_PERMISSION_REVIEWED ||
        native.permission_count != 2u ||
        std::strcmp(
            native.permissions[0],
            "resources") != 0 ||
        std::strcmp(
            native.permissions[1],
            "network:https://api.example.com") != 0 ||
        native.has_windows_x86_entrypoint == 0u ||
        native.dependency_count != 1u ||
        native.client_build_count != 1u ||
        native.client_executable_hash_count != 1u ||
        std::strcmp(native.id, "tests.preflight") != 0 ||
        std::strcmp(
            native.windows_x86_entrypoint,
            "bin/windows-x86/mod.dll") != 0) {
        return 1;
    }

    static const char content_manifest[] = R"json({
        "manifest_version": 1,
        "type": "content",
        "id": "tests.content",
        "name": "Content test",
        "version": "1.0.0",
        "developer": "tests",
        "content": "descriptors/content.json",
        "permissions": ["resources"]
    })json";
    wotbmod::v3::DataManifestPreflightSummary content = {};
    content.struct_size = sizeof(content);
    if (wotbmod::v3::PreflightManifestUtf8(
            content_manifest,
            sizeof(content_manifest) - 1u,
            &content) != WOTBMOD_V3_OK ||
        content.package_type !=
            WOTBMOD_V3_PACKAGE_CONTENT_ONLY ||
        content.permission_count != 1u ||
        std::strcmp(
            content.permissions[0],
            "resources") != 0 ||
        content.has_windows_x86_entrypoint != 0u ||
        std::strcmp(
            content.content_descriptor_path,
            "descriptors/content.json") != 0) {
        return 2;
    }

    static const char traversal_manifest[] = R"json({
        "manifest_version": 1,
        "type": "content",
        "id": "tests.traversal",
        "name": "Traversal test",
        "version": "1.0.0",
        "developer": "tests",
        "content": "../content.json"
    })json";
    wotbmod::v3::DataManifestPreflightSummary traversal = {};
    traversal.struct_size = sizeof(traversal);
    if (wotbmod::v3::PreflightManifestUtf8(
            traversal_manifest,
            sizeof(traversal_manifest) - 1u,
            &traversal) != WOTBMOD_V3_E_PARSE) {
        return 3;
    }
    return 0;
#else
    const WotbModV3SettingsApiV1 settings = {};
    const WotbModV3StorageApiV1 storage = {};
    const WotbModV3InputApiV1 input = {};
    const WotbModV3VfsApiV1 vfs = {};
    const WotbModV3ResourcesApiV1 resources = {};
    const WotbModV3YamlApiV1 yaml = {};
    const WotbModV3ArchiveApiV1 archive = {};
    const WotbModV3LoadersApiV1 loaders = {};
    const WotbModV3ManifestApiV1 manifest = {};
    const WotbModV3CatalogApiV1 catalog = {};
    const WotbModV3ContentApiV1 content = {};
    return static_cast<int>(
        settings.struct_size + storage.struct_size +
        input.struct_size + vfs.struct_size +
        resources.struct_size + yaml.struct_size +
        archive.struct_size + loaders.struct_size +
        manifest.struct_size + catalog.struct_size +
        content.struct_size);
#endif
}
