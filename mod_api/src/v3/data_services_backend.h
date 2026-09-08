#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "../../include/wotbmod/manifest_v1.h"

namespace wotbmod {
namespace v3 {

#define WOTBMOD_V3_DATA_PREFLIGHT_VERSION 3u
#define WOTBMOD_V3_PREFLIGHT_MAX_DEPENDENCIES 128u
#define WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS 256u
#define WOTBMOD_V3_PREFLIGHT_MAX_CLIENT_BUILDS 64u
#define WOTBMOD_V3_PREFLIGHT_MAX_CLIENT_HASHES 64u

struct DataManifestPreflightSummary {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t manifest_version;
    uint32_t package_type;
    uint32_t requested_permission_tier;
    uint32_t permission_count;
    uint32_t dependency_count;
    uint32_t client_build_count;
    uint32_t client_executable_hash_count;
    uint32_t has_windows_x86_entrypoint;
    uint32_t has_signature;
    char id[WOTBMOD_V3_MAX_ID];
    char name[WOTBMOD_V3_MAX_NAME];
    char version[WOTBMOD_V3_MAX_VERSION];
    char developer[WOTBMOD_V3_MAX_NAME];
    char windows_x86_entrypoint[WOTBMOD_V3_MAX_PATH];
    char content_descriptor_path[WOTBMOD_V3_MAX_PATH];
    char signature_algorithm[WOTBMOD_V3_MAX_NAME];
    char signature_key_id[WOTBMOD_V3_MAX_ID];
    char signature_value[4097];
    char permissions
        [WOTBMOD_V3_PREFLIGHT_MAX_PERMISSIONS]
        [WOTBMOD_V3_MAX_PERMISSION_NAME];
    WotbModV3ManifestDependency
        dependencies[WOTBMOD_V3_PREFLIGHT_MAX_DEPENDENCIES];
    char client_builds
        [WOTBMOD_V3_PREFLIGHT_MAX_CLIENT_BUILDS]
        [WOTBMOD_V3_MAX_NAME];
    char client_executable_hashes
        [WOTBMOD_V3_PREFLIGHT_MAX_CLIENT_HASHES][65];
    char error[WOTBMOD_V3_MAX_MESSAGE];
};

WotbModV3Result PreflightManifestUtf8(
    const void* json_utf8,
    size_t json_size,
    DataManifestPreflightSummary* out_summary);

WotbModV3Result PreflightManifestFile(
    const char* physical_manifest_file,
    DataManifestPreflightSummary* out_summary);

/*
 * Runtime/client-service bridge into the owner-checked VFS. These helpers
 * intentionally preserve the same access, containment, and handle ownership
 * rules as the public VFS table.
 */
WotbModV3Result MountVfsPackageForRuntime(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* physical_directory,
    int32_t priority,
    WotbModV3Handle* out_mount);

WotbModV3Result MountVfsOverlayForRuntime(
    WotbModV3Handle mod,
    const char* provider_id,
    const char* target_game_uri,
    const char* source_mod_uri,
    int32_t priority,
    WotbModV3Handle* out_mount);

WotbModV3Result UnmountVfsForRuntime(
    WotbModV3Handle mod,
    WotbModV3Handle mount);

WotbModV3Result ResolveVfsUriPhysical(
    WotbModV3Handle mod,
    const char* uri,
    char* physical_path,
    uint32_t* inout_size);
/* Same lookup without the VFS named-permission gate (see the definition). */
WotbModV3Result ResolveUriPhysicalForOwner(
    WotbModV3Handle mod,
    const char* uri,
    std::string* out_path);

/*
 * Resolves an active V3 game:// overlay for a DAVA ~res:/ request. This is
 * the narrow bridge used by the loader-owned DAVA::File::Create hook; it
 * never exposes package roots or native objects to mods.
 */
WotbModV3Result ResolveActiveGameOverlayPath(
    const char* requested_path,
    char* physical_path,
    uint32_t* inout_size);

enum NativeInputEventKind : uint32_t {
    NATIVE_INPUT_EVENT_BUTTON = 1u,
    NATIVE_INPUT_EVENT_AXIS = 2u,
    NATIVE_INPUT_EVENT_RESET = 3u
};

/* Thread-safe native ingress. Producers may run on the game window thread;
 * callbacks are always delivered later by DataServicesFramePump. */
WotbModV3Result NotifyNativeInput(
    uint32_t event_kind,
    uint32_t device,
    uint32_t code,
    uint32_t modifiers,
    float value,
    uint32_t down);

void ResetNativeInputState(void);

}  // namespace v3
}  // namespace wotbmod
