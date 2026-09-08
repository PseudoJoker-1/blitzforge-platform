#include "../src/v3/wotb_mod_v3_internal.h"

#include <cstdio>
#include <cstring>

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

uint32_t g_passed = 0u;
uint32_t g_failed = 0u;

void Check(bool condition, const char* label) {
    if (condition) {
        ++g_passed;
        return;
    }
    ++g_failed;
    std::printf("FAIL: %s\n", label ? label : "(null)");
}

WotbModV3InterfaceInfo GetInfo(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name) {
    WotbModV3InterfaceInfo info = {};
    WOTBMOD_V3_INIT_STRUCT(info, WOTBMOD_V3_ABI_VERSION);
    Check(
        bootstrap->get_interface_info(mod, name, &info) ==
            WOTBMOD_V3_OK,
        "get_interface_info succeeds");
    return info;
}

WotbModV3Result Query(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod,
    const char* name,
    const void** out_table) {
    *out_table = reinterpret_cast<const void*>(1u);
    return bootstrap->query_interface(
        mod,
        name,
        WOTBMOD_V3_IFACE_VERSION_1,
        out_table);
}

void Register(
    const char* name,
    uint32_t permission_tier,
    const char* capability_name,
    uint32_t status = 0xFFFFFFFFu,
    const char* reason = nullptr) {
    wotbmod::v3::InterfaceRegistration registration = {};
    registration.name = name;
    registration.version = WOTBMOD_V3_IFACE_VERSION_1;
    registration.table = &kDummyApi;
    registration.required_permission_tier = permission_tier;
    registration.allowed_contexts = WOTBMOD_V3_CONTEXT_ALL;
    registration.capability_name = capability_name;
    registration.implementation_status = status;
    registration.implementation_reason = reason;
    Check(
        wotbmod::v3::RegisterInterface(registration) ==
            WOTBMOD_V3_OK,
        "interface registration succeeds");
}

void TestDefaults(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    WotbModV3InterfaceInfo info =
        GetInfo(bootstrap, mod, WOTBMOD_V3_IFACE_CORE);
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_AVAILABLE,
        "core is explicitly available");
    Check(
        info.unavailable_reason[0] == '\0',
        "available core has no degradation reason");

    Register(
        "tests.interface.auto.partial",
        WOTBMOD_V3_PERMISSION_SAFE,
        nullptr);
    info = GetInfo(bootstrap, mod, "tests.interface.auto.partial");
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_DEGRADED,
        "unknown implementation defaults to degraded");
    Check(
        info.unavailable_reason[0] != '\0',
        "degraded default explains incomplete coverage");
    const void* table = nullptr;
    Check(
        Query(
            bootstrap,
            mod,
            "tests.interface.auto.partial",
            &table) == WOTBMOD_V3_OK,
        "degraded interface remains queryable");
    Check(table == &kDummyApi, "degraded query returns its table");

    Register(
        WOTBMOD_V3_IFACE_HTTP,
        WOTBMOD_V3_PERMISSION_SAFE,
        nullptr);
    info = GetInfo(bootstrap, mod, WOTBMOD_V3_IFACE_HTTP);
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_AVAILABLE,
        "WinHTTP-backed HTTP interface is available");
    Check(
        info.unavailable_reason[0] == '\0',
        "available HTTP transport has no stale degradation reason");
    table = nullptr;
    Check(
        Query(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_HTTP,
            &table) == WOTBMOD_V3_OK,
        "HTTP request and transport interface is queryable");

    Register(
        WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
        WOTBMOD_V3_PERMISSION_SAFE,
        nullptr);
    info = GetInfo(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_GAMEPLAY_HUD);
    // 2026-09-05: the stock battle controls (Minimap, Lamp, GunAim,
    // RibbonsContainer, DamageStatistics) are driven through the UI host
    // bridge, so the interface is degraded rather than unavailable: the
    // visibility/geometry/opacity slots work, the cosmetic ones still refuse.
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_DEGRADED,
        "gameplay HUD with stock-control backend is degraded");
    Check(
        std::strstr(
            info.unavailable_reason,
            "stock battle controls") != nullptr,
        "degraded gameplay HUD names the stock-control subset");
    table = nullptr;
    Check(
        Query(
            bootstrap,
            mod,
            WOTBMOD_V3_IFACE_GAMEPLAY_HUD,
            &table) == WOTBMOD_V3_OK,
        "degraded gameplay HUD interface is queryable");
    Check(table != nullptr, "degraded gameplay HUD query returns its table");

    Register(
        WOTBMOD_V3_IFACE_CONTENT,
        WOTBMOD_V3_PERMISSION_SAFE,
        nullptr);
    info = GetInfo(bootstrap, mod, WOTBMOD_V3_IFACE_CONTENT);
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_DEGRADED,
        "content descriptor/VFS subset is degraded");
    Check(
        std::strstr(
            info.unavailable_reason,
            "native-intercepted UI, texture and model") != nullptr &&
            std::strstr(
                info.unavailable_reason,
                "semantic audio, hangar and localization") != nullptr,
        "content capability distinguishes file overlays from unsupported semantic kinds");

    Register(
        WOTBMOD_V3_IFACE_VEHICLE_VISUAL,
        WOTBMOD_V3_PERMISSION_GAMEPLAY_TWEAK,
        nullptr);
    info = GetInfo(
        bootstrap,
        mod,
        WOTBMOD_V3_IFACE_VEHICLE_VISUAL);
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_DEGRADED,
        "vehicle registry/readback subset is degraded");
    Check(
        std::strstr(
            info.unavailable_reason,
            "exact-path mesh/material/texture skin packs") != nullptr &&
        std::strstr(
            info.unavailable_reason,
            "active cached model hot swap") != nullptr,
        "vehicle capability distinguishes resource overrides from live hot swap");
}

void TestExplicitAndDynamicStatus(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    Register(
        "tests.interface.explicit.unavailable",
        WOTBMOD_V3_PERMISSION_SAFE,
        nullptr,
        WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "focused backend is absent");
    WotbModV3InterfaceInfo info = GetInfo(
        bootstrap,
        mod,
        "tests.interface.explicit.unavailable");
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "explicit unavailable status is preserved");
    Check(
        std::strcmp(
            info.unavailable_reason,
            "focused backend is absent") == 0,
        "explicit unavailable reason is preserved");

    const void* table = nullptr;
    Check(
        Query(
            bootstrap,
            mod,
            "tests.interface.explicit.unavailable",
            &table) == WOTBMOD_V3_E_NOT_SUPPORTED,
        "explicit unavailable status blocks query");
    WotbModV3ErrorInfo error = {};
    WOTBMOD_V3_INIT_STRUCT(error, WOTBMOD_V3_ABI_VERSION);
    Check(
        bootstrap->get_last_error(mod, &error) == WOTBMOD_V3_OK,
        "last error is readable");
    Check(
        std::strcmp(error.message, "focused backend is absent") == 0,
        "query error uses the truthful reason");

    Check(
        wotbmod::v3::SetInterfaceAvailability(
            "tests.interface.explicit.unavailable",
            WOTBMOD_V3_CAPABILITY_DEGRADED,
            "read-only subset installed") == WOTBMOD_V3_OK,
        "dynamic transition to degraded succeeds");
    info = GetInfo(
        bootstrap,
        mod,
        "tests.interface.explicit.unavailable");
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_DEGRADED,
        "dynamic degraded status is visible");
    Check(
        std::strcmp(
            info.unavailable_reason,
            "read-only subset installed") == 0,
        "dynamic degradation reason is visible");
    table = nullptr;
    Check(
        Query(
            bootstrap,
            mod,
            "tests.interface.explicit.unavailable",
            &table) == WOTBMOD_V3_OK,
        "dynamically degraded interface is queryable");

    Check(
        wotbmod::v3::SetInterfaceAvailability(
            "tests.interface.explicit.unavailable",
            WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH,
            "binding pack mismatch") == WOTBMOD_V3_OK,
        "dynamic transition to client mismatch succeeds");
    info = GetInfo(
        bootstrap,
        mod,
        "tests.interface.explicit.unavailable");
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH,
        "client mismatch status is visible");
    table = nullptr;
    Check(
        Query(
            bootstrap,
            mod,
            "tests.interface.explicit.unavailable",
            &table) == WOTBMOD_V3_E_CLIENT_MISMATCH,
        "client mismatch maps to the matching result code");

    Check(
        wotbmod::v3::SetInterfaceAvailability(
            "tests.interface.explicit.unavailable",
            WOTBMOD_V3_CAPABILITY_AVAILABLE,
            nullptr) == WOTBMOD_V3_OK,
        "dynamic transition to available succeeds");
    info = GetInfo(
        bootstrap,
        mod,
        "tests.interface.explicit.unavailable");
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_AVAILABLE,
        "dynamic available status is visible");
    Check(
        info.unavailable_reason[0] == '\0',
        "available transition clears the reason");
    table = nullptr;
    Check(
        Query(
            bootstrap,
            mod,
            "tests.interface.explicit.unavailable",
            &table) == WOTBMOD_V3_OK,
        "available interface is queryable");

    Check(
        wotbmod::v3::SetInterfaceAvailability(
            "tests.interface.missing",
            WOTBMOD_V3_CAPABILITY_AVAILABLE,
            nullptr) == WOTBMOD_V3_E_NOT_FOUND,
        "status update rejects an unknown interface");
    Check(
        wotbmod::v3::SetInterfaceAvailability(
            "tests.interface.explicit.unavailable",
            WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED,
            nullptr) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "static status rejects caller-specific permission state");
}

void TestCapabilityAndAccessPrecedence(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle unsafe_mod,
    WotbModV3Handle safe_mod) {
    Register(
        "tests.interface.named.partial",
        WOTBMOD_V3_PERMISSION_SAFE,
        "tests.capability.partial");
    const void* capabilities_table = nullptr;
    Check(
        bootstrap->query_interface(
            unsafe_mod,
            WOTBMOD_V3_IFACE_CAPABILITIES,
            WOTBMOD_V3_CAPABILITIES_VERSION,
            &capabilities_table) == WOTBMOD_V3_OK,
        "capabilities interface is queryable");
    const WotbModV3CapabilitiesApiV1* capabilities =
        static_cast<const WotbModV3CapabilitiesApiV1*>(
            capabilities_table);
    WotbModV3CapabilityInfo capability = {};
    WOTBMOD_V3_INIT_STRUCT(
        capability,
        WOTBMOD_V3_CAPABILITIES_VERSION);
    Check(
        capabilities->query(
            unsafe_mod,
            "tests.capability.partial",
            &capability) == WOTBMOD_V3_OK,
        "auto-created capability is queryable");
    Check(
        capability.status == WOTBMOD_V3_CAPABILITY_DEGRADED,
        "auto-created capability inherits degraded coverage");
    Check(
        capability.unavailable_reason[0] != '\0',
        "degraded capability includes a reason");

    Check(
        wotbmod::v3::SetInterfaceAvailability(
            "tests.interface.named.partial",
            WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
            "named backend stopped") == WOTBMOD_V3_OK,
        "named interface transition to unavailable succeeds");
    capability = {};
    WOTBMOD_V3_INIT_STRUCT(
        capability,
        WOTBMOD_V3_CAPABILITIES_VERSION);
    Check(
        capabilities->query(
            unsafe_mod,
            "tests.capability.partial",
            &capability) == WOTBMOD_V3_OK,
        "derived capability remains queryable after transition");
    Check(
        capability.status == WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "derived capability follows interface availability");
    Check(
        std::strcmp(
            capability.unavailable_reason,
            "named backend stopped") == 0,
        "derived capability follows interface reason");
    Check(
        wotbmod::v3::SetInterfaceAvailability(
            "tests.interface.named.partial",
            WOTBMOD_V3_CAPABILITY_DEGRADED,
            "named read-only subset restored") ==
            WOTBMOD_V3_OK,
        "named interface transition back to degraded succeeds");

    capability = {};
    WOTBMOD_V3_INIT_STRUCT(
        capability,
        WOTBMOD_V3_CAPABILITIES_VERSION);
    strcpy_s(
        capability.name,
        "tests.capability.partial");
    capability.interface_version = WOTBMOD_V3_IFACE_VERSION_1;
    capability.allowed_contexts = WOTBMOD_V3_CONTEXT_ALL;
    capability.permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    capability.status = WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH;
    strcpy_s(
        capability.unavailable_reason,
        "capability binding mismatch");
    Check(
        WotbModV3Runtime_SetCapability(&capability) ==
            WOTBMOD_V3_OK,
        "capability client mismatch update succeeds");
    WotbModV3InterfaceInfo info = GetInfo(
        bootstrap,
        unsafe_mod,
        "tests.interface.named.partial");
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_CLIENT_MISMATCH,
        "capability mismatch overrides static degraded status");
    const void* table = nullptr;
    Check(
        Query(
            bootstrap,
            unsafe_mod,
            "tests.interface.named.partial",
            &table) == WOTBMOD_V3_E_CLIENT_MISMATCH,
        "capability mismatch returns client mismatch");

    Register(
        "tests.interface.permission.first",
        WOTBMOD_V3_PERMISSION_UNSAFE,
        nullptr,
        WOTBMOD_V3_CAPABILITY_UNAVAILABLE,
        "private backend is absent");
    info = GetInfo(
        bootstrap,
        safe_mod,
        "tests.interface.permission.first");
    Check(
        info.status == WOTBMOD_V3_CAPABILITY_PERMISSION_DENIED,
        "caller permission denial takes precedence in interface info");
    table = nullptr;
    Check(
        Query(
            bootstrap,
            safe_mod,
            "tests.interface.permission.first",
            &table) == WOTBMOD_V3_E_PERMISSION_DENIED,
        "caller permission denial takes precedence in query");
}

}  // namespace

int main() {
    WotbModV3RuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.api_version = WOTBMOD_V3_ABI_VERSION;
    options.game_directory = ".";
    options.mods_directory = "build\\v3_interface_truth_tmp\\mods";
    options.cache_directory = "build\\v3_interface_truth_tmp\\cache";
    options.config_directory = "build\\v3_interface_truth_tmp\\config";
    options.client_version = "contract-test";
    options.executable_sha256 = "contract-test-sha";
    options.process_architecture = 32u;
    Check(
        WotbModV3Runtime_Initialize(&options) == WOTBMOD_V3_OK,
        "runtime initializes");
    const WotbModV3Bootstrap* bootstrap =
        WotbModV3Runtime_GetBootstrap();
    Check(bootstrap != nullptr, "bootstrap is available");

    WotbModV3Handle unsafe_mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle safe_mod = WOTBMOD_V3_INVALID_HANDLE;
    Check(
        WotbModV3Runtime_CreateMod(
            "interface_truth_unsafe.dll",
            WOTBMOD_V3_PERMISSION_UNSAFE,
            &unsafe_mod) == WOTBMOD_V3_OK,
        "unsafe test mod is created");
    Check(
        WotbModV3Runtime_CreateMod(
            "interface_truth_safe.dll",
            WOTBMOD_V3_PERMISSION_SAFE,
            &safe_mod) == WOTBMOD_V3_OK,
        "safe test mod is created");

    if (bootstrap &&
        unsafe_mod != WOTBMOD_V3_INVALID_HANDLE &&
        safe_mod != WOTBMOD_V3_INVALID_HANDLE) {
        TestDefaults(bootstrap, unsafe_mod);
        TestExplicitAndDynamicStatus(bootstrap, unsafe_mod);
        TestCapabilityAndAccessPrecedence(
            bootstrap,
            unsafe_mod,
            safe_mod);
    }

    WotbModV3Runtime_Shutdown();
    std::printf(
        "V3 interface truth tests: %u passed, %u failed\n",
        g_passed,
        g_failed);
    return g_failed == 0u ? 0 : 1;
}
