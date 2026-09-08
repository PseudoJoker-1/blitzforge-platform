#include "../src/v3/wotb_mod_v3_internal.h"

#include "../include/wotbmod/interface_ids.h"
#include "../include/wotbmod/permissions_v1.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace wotbmod {
namespace v3 {

void RegisterRuntimeServices() {}
void RegisterDataServices() {}
void RegisterClientServices() {}
void RegisterToolingServices() {}
void SetNativeHookBackend(const WotbModV3NativeHookBackend*) {}

// This focused build links wotb_mod_v3_runtime.cpp without the services
// layer, so every services symbol it references must be stubbed here.
// wotb_mod_v3_runtime.cpp announces capability changes on the event bus
// via PublishSystemEvent; the event bus itself is not under test here.
WotbModV3Result PublishSystemEvent(
    const char*,
    const void*,
    uint32_t,
    uint32_t) {
    return WOTBMOD_V3_OK;
}

}  // namespace v3
}  // namespace wotbmod

namespace {

struct DummyApi {
    uint32_t struct_size;
    uint32_t api_version;
};

const DummyApi kDummyApi = {
    sizeof(DummyApi),
    WOTBMOD_V3_IFACE_VERSION_1
};

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::fprintf(
        stderr,
        "V3 NAMED PERMISSIONS FAIL line %d: %s\n",
        line,
        expression ? expression : "(null)");
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

WotbModV3PermissionInfo QueryPermission(
    const WotbModV3PermissionsApiV1* permissions,
    WotbModV3Handle mod,
    const char* name,
    WotbModV3Result expected_result = WOTBMOD_V3_OK) {
    WotbModV3PermissionInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(
        info,
        WOTBMOD_V3_PERMISSIONS_VERSION);
    const WotbModV3Result result =
        permissions->query(mod, name, &info);
    CHECK(result == expected_result);
    return info;
}

WotbModV3Result WOTBMOD_V3_CALL LoadedEntry(
    const WotbModV3Bootstrap*,
    WotbModV3Handle,
    WotbModV3Info* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_SAFE;
    strcpy_s(out_info->id, "named_permissions_loaded");
    strcpy_s(out_info->name, "Named permissions loaded probe");
    strcpy_s(out_info->version, "1.0.0");
    return WOTBMOD_V3_OK;
}

WotbModV3Handle CreateMod(
    const char* path,
    uint32_t tier) {
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    CHECK(
        WotbModV3Runtime_CreateMod(path, tier, &mod) ==
            WOTBMOD_V3_OK);
    CHECK(mod != WOTBMOD_V3_INVALID_HANDLE);
    return mod;
}

void RegisterPermissionGatedInterface(
    const char* name,
    uint32_t tier,
    const char* capability_name,
    uint32_t implementation_status = 0xFFFFFFFFu) {
    wotbmod::v3::InterfaceRegistration registration = {};
    registration.name = name;
    registration.version = WOTBMOD_V3_IFACE_VERSION_1;
    registration.table = &kDummyApi;
    registration.required_permission_tier = tier;
    registration.allowed_contexts = WOTBMOD_V3_CONTEXT_ALL;
    registration.capability_name = capability_name;
    registration.implementation_status = implementation_status;
    CHECK(
        wotbmod::v3::RegisterInterface(registration) ==
        WOTBMOD_V3_OK);
}

void TestAvailabilityTruth(
    const WotbModV3Bootstrap* bootstrap) {
    const WotbModV3Handle mod = CreateMod(
        "permission_availability_truth.dll",
        WOTBMOD_V3_PERMISSION_UNSAFE);
    struct Expectation {
        const char* name;
        uint32_t status;
    };
    const Expectation expectations[] = {
        {WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
         WOTBMOD_V3_CAPABILITY_DEGRADED},
        {WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
         WOTBMOD_V3_CAPABILITY_DEGRADED},
        {WOTBMOD_V3_IFACE_BIGWORLD_RPC,
         WOTBMOD_V3_CAPABILITY_DEGRADED},
        {WOTBMOD_V3_IFACE_DEVICE,
         WOTBMOD_V3_CAPABILITY_AVAILABLE}
    };
    for (const Expectation& expectation : expectations) {
        WotbModV3InterfaceInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
        CHECK(
            bootstrap->get_interface_info(
                mod,
                expectation.name,
                &info) == WOTBMOD_V3_OK);
        CHECK(info.status == expectation.status);
        if (expectation.status ==
            WOTBMOD_V3_CAPABILITY_AVAILABLE) {
            CHECK(info.unavailable_reason[0] == '\0');
        } else {
            CHECK(info.unavailable_reason[0] != '\0');
        }
    }
    CHECK(
        WotbModV3Runtime_DestroyMod(mod) ==
        WOTBMOD_V3_OK);
}

void TestInterfaceAndAccessGate(
    const WotbModV3Bootstrap* bootstrap) {
    const WotbModV3Handle mod = CreateMod(
        "permission_interface_gate.dll",
        WOTBMOD_V3_PERMISSION_REVIEWED);
    const char* grants[] = {"render.callbacks"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            grants,
            1u,
            1u) == WOTBMOD_V3_OK);

    const void* table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_RENDER,
            WOTBMOD_V3_IFACE_VERSION_1,
            &table) == WOTBMOD_V3_OK);
    CHECK(table == &kDummyApi);

    table = reinterpret_cast<const void*>(1u);
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
            WOTBMOD_V3_IFACE_VERSION_1,
            &table) == WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(table == nullptr);

    WotbModV3InterfaceInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    CHECK(
        bootstrap->get_interface_info(
            mod,
            WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
            &info) == WOTBMOD_V3_OK);
    CHECK(
        info.status ==
        WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED);

    table = nullptr;
    CHECK(
        bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_PERMISSIONS,
            WOTBMOD_V3_PERMISSIONS_VERSION,
            &table) == WOTBMOD_V3_OK);
    CHECK(table != nullptr);

    CHECK(
        wotbmod::v3::CheckAccess(
            mod,
            WOTBMOD_V3_PERMISSION_SAFE,
            WOTBMOD_V3_CONTEXT_ALL,
            "settings") ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    const char* settings_grant[] = {"settings"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            settings_grant,
            1u,
            1u) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::CheckAccess(
            mod,
            WOTBMOD_V3_PERMISSION_SAFE,
            WOTBMOD_V3_CONTEXT_ALL,
            "settings") ==
        WOTBMOD_V3_OK);

    CHECK(
        WotbModV3Runtime_DestroyMod(mod) ==
        WOTBMOD_V3_OK);
}

void TestLegacyTierMode(
    const WotbModV3PermissionsApiV1* permissions) {
    const WotbModV3Handle mod = CreateMod(
        "loose_legacy.dll",
        WOTBMOD_V3_PERMISSION_SAFE);
    const WotbModV3PermissionInfo audio =
        QueryPermission(permissions, mod, "audio.custom");
    CHECK(
        audio.state ==
        WOTBMOD_V3_PERMISSION_STATE_GRANTED);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "resources.mod",
            WOTBMOD_V3_PERMISSION_SAFE) ==
        WOTBMOD_V3_OK);
    CHECK(
        WotbModV3Runtime_DestroyMod(mod) ==
        WOTBMOD_V3_OK);
}

void TestRestrictedNamedMode(
    const WotbModV3PermissionsApiV1* permissions) {
    const WotbModV3Handle mod = CreateMod(
        "restricted_package.dll",
        WOTBMOD_V3_PERMISSION_SAFE);
    const char* grants[] = {
        "UI.CREATE",
        "resources",
        "ui.create"
    };
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            grants,
            static_cast<uint32_t>(
                sizeof(grants) / sizeof(grants[0])),
            1u) == WOTBMOD_V3_OK);

    const WotbModV3PermissionInfo declared =
        QueryPermission(permissions, mod, "ui.create");
    CHECK(
        declared.state ==
        WOTBMOD_V3_PERMISSION_STATE_GRANTED);

    const WotbModV3PermissionInfo undeclared =
        QueryPermission(permissions, mod, "audio.custom");
    CHECK(
        undeclared.tier == WOTBMOD_V3_PERMISSION_SAFE);
    CHECK(
        undeclared.state ==
        WOTBMOD_V3_PERMISSION_STATE_DENIED);
    CHECK(
        std::strstr(undeclared.reason, "not declared") !=
        nullptr);

    const WotbModV3PermissionInfo child =
        QueryPermission(permissions, mod, "resources.mod");
    CHECK(
        child.state ==
        WOTBMOD_V3_PERMISSION_STATE_GRANTED);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "resources.mod",
            WOTBMOD_V3_PERMISSION_SAFE) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "resources.overlay.game",
            WOTBMOD_V3_PERMISSION_REVIEWED) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "resource.mod",
            WOTBMOD_V3_PERMISSION_SAFE) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);

    const WotbModV3PermissionInfo core =
        QueryPermission(permissions, mod, "core");
    CHECK(
        core.state ==
        WOTBMOD_V3_PERMISSION_STATE_GRANTED);

    uint32_t permission_count = 0u;
    CHECK(
        permissions->get_count(mod, &permission_count) ==
        WOTBMOD_V3_OK);
    bool found_undeclared = false;
    for (uint32_t index = 0u;
         index < permission_count;
         ++index) {
        WotbModV3PermissionInfo info = {};
        WOTBMOD_V3_INIT_STRUCT(
            info,
            WOTBMOD_V3_PERMISSIONS_VERSION);
        CHECK(
            permissions->get_at(mod, index, &info) ==
            WOTBMOD_V3_OK);
        if (std::strcmp(info.name, "audio.custom") == 0) {
            found_undeclared = true;
            CHECK(
                info.state ==
                WOTBMOD_V3_PERMISSION_STATE_DENIED);
        }
    }
    CHECK(found_undeclared);

    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            nullptr,
            0u,
            0u) == WOTBMOD_V3_OK);
    const WotbModV3PermissionInfo restored =
        QueryPermission(permissions, mod, "audio.custom");
    CHECK(
        restored.state ==
        WOTBMOD_V3_PERMISSION_STATE_GRANTED);

    CHECK(
        WotbModV3Runtime_DestroyMod(mod) ==
        WOTBMOD_V3_OK);
}

void TestInputValidationAndAtomicity(
    const WotbModV3PermissionsApiV1* permissions) {
    const WotbModV3Handle mod = CreateMod(
        "permission_validation.dll",
        WOTBMOD_V3_PERMISSION_SAFE);
    const char* initial[] = {"settings"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            initial,
            1u,
            1u) == WOTBMOD_V3_OK);

    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            initial,
            WOTBMOD_V3_RUNTIME_MAX_PERMISSION_GRANTS + 1u,
            1u) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            nullptr,
            1u,
            1u) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            initial,
            1u,
            2u) == WOTBMOD_V3_E_INVALID_ARGUMENT);

    const char* null_name[] = {nullptr};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            null_name,
            1u,
            1u) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    const char* empty_name[] = {""};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            empty_name,
            1u,
            1u) == WOTBMOD_V3_E_INVALID_ARGUMENT);
    const char* malformed_name[] = {"ui..create"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            malformed_name,
            1u,
            1u) == WOTBMOD_V3_E_INVALID_ARGUMENT);

    std::string oversized(
        WOTBMOD_V3_MAX_PERMISSION_NAME,
        'a');
    const char* oversized_name[] = {oversized.c_str()};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            oversized_name,
            1u,
            1u) == WOTBMOD_V3_E_INVALID_ARGUMENT);

    const WotbModV3PermissionInfo still_granted =
        QueryPermission(permissions, mod, "settings");
    CHECK(
        still_granted.state ==
        WOTBMOD_V3_PERMISSION_STATE_GRANTED);
    const WotbModV3PermissionInfo still_denied =
        QueryPermission(permissions, mod, "storage");
    CHECK(
        still_denied.state ==
        WOTBMOD_V3_PERMISSION_STATE_DENIED);

    const char* network_grant[] = {
        "network:https://api.example.com"
    };
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            mod,
            network_grant,
            1u,
            1u) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "network:https://api.example.com",
            WOTBMOD_V3_PERMISSION_SAFE) ==
        WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "network:https://other.example.com",
            WOTBMOD_V3_PERMISSION_SAFE) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "network:https://api.example.com.evil",
            WOTBMOD_V3_PERMISSION_SAFE) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            mod,
            "network:https://API.EXAMPLE.COM",
            WOTBMOD_V3_PERMISSION_SAFE) ==
        WOTBMOD_V3_OK);

    CHECK(
        WotbModV3Runtime_DestroyMod(mod) ==
        WOTBMOD_V3_OK);
}

void TestTierAndEntryBoundary(
    const WotbModV3PermissionsApiV1* permissions) {
    const WotbModV3Handle tier_mod = CreateMod(
        "permission_tier.dll",
        WOTBMOD_V3_PERMISSION_SAFE);
    const char* grants[] = {"native"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            tier_mod,
            grants,
            1u,
            1u) == WOTBMOD_V3_OK);
    CHECK(
        wotbmod::v3::CheckNamedPermission(
            tier_mod,
            "native.memory",
            WOTBMOD_V3_PERMISSION_UNSAFE) ==
        WOTBMOD_V3_E_PERMISSION_DENIED);
    const WotbModV3PermissionInfo native =
        QueryPermission(permissions, tier_mod, "native.memory");
    CHECK(
        native.state ==
        WOTBMOD_V3_PERMISSION_STATE_DENIED);
    CHECK(
        WotbModV3Runtime_DestroyMod(tier_mod) ==
        WOTBMOD_V3_OK);

    const WotbModV3Handle loaded_mod = CreateMod(
        "permission_loaded.dll",
        WOTBMOD_V3_PERMISSION_SAFE);
    WotbModV3RuntimeModuleInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    CHECK(
        WotbModV3Runtime_InvokeEntry(
            loaded_mod,
            LoadedEntry,
            &info) == WOTBMOD_V3_OK);
    const char* settings[] = {"settings"};
    CHECK(
        WotbModV3Runtime_SetPermissionGrants(
            loaded_mod,
            settings,
            1u,
            1u) == WOTBMOD_V3_E_CONFLICT);
    CHECK(
        WotbModV3Runtime_DestroyMod(loaded_mod) ==
        WOTBMOD_V3_OK);
}

}  // namespace

int main() {
    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory =
        "build\\v3_named_permissions\\mods";
    options.cache_directory =
        "build\\v3_named_permissions\\cache";
    options.config_directory =
        "build\\v3_named_permissions\\config";
    options.client_version = "named-permissions-test";
    options.process_architecture = 32u;
    CHECK(
        WotbModV3Runtime_Initialize(&options) ==
        WOTBMOD_V3_OK);

    RegisterPermissionGatedInterface(
        WOTBMOD_V3_IFACE_RENDER,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        nullptr);
    RegisterPermissionGatedInterface(
        WOTBMOD_V3_IFACE_ENTITY_PUBLIC,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        nullptr);
    RegisterPermissionGatedInterface(
        WOTBMOD_V3_IFACE_SETTINGS,
        WOTBMOD_V3_PERMISSION_SAFE,
        "settings");
    RegisterPermissionGatedInterface(
        WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        nullptr);
    RegisterPermissionGatedInterface(
        WOTBMOD_V3_IFACE_BIGWORLD_RPC,
        WOTBMOD_V3_PERMISSION_REVIEWED,
        nullptr);
    RegisterPermissionGatedInterface(
        WOTBMOD_V3_IFACE_DEVICE,
        WOTBMOD_V3_PERMISSION_SAFE,
        nullptr);

    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    CHECK(bootstrap != nullptr);
    const WotbModV3Handle interface_probe = CreateMod(
        "permission_interface_probe.dll",
        WOTBMOD_V3_PERMISSION_SAFE);
    const void* permissions_table = nullptr;
    CHECK(
        bootstrap->query_interface(
            interface_probe,
            WOTBMOD_V3_IFACE_PERMISSIONS,
            WOTBMOD_V3_PERMISSIONS_VERSION,
            &permissions_table) == WOTBMOD_V3_OK);
    CHECK(permissions_table != nullptr);
    const WotbModV3PermissionsApiV1* permissions =
        static_cast<const WotbModV3PermissionsApiV1*>(
            permissions_table);
    CHECK(
        WotbModV3Runtime_DestroyMod(interface_probe) ==
        WOTBMOD_V3_OK);

    TestAvailabilityTruth(bootstrap);
    TestInterfaceAndAccessGate(bootstrap);
    TestLegacyTierMode(permissions);
    TestRestrictedNamedMode(permissions);
    TestInputValidationAndAtomicity(permissions);
    TestTierAndEntryBoundary(permissions);

    WotbModV3Runtime_Shutdown();
    std::printf(
        "V3 NAMED PERMISSIONS: %u checks, %u failures\n",
        g_checks,
        g_failures);
    return g_failures == 0u ? 0 : 1;
}
