#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../include/wotb_mod_api_v3.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

uint32_t g_checks = 0u;
uint32_t g_failures = 0u;

#define CHECK(condition)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(condition)) {                                                \
            ++g_failures;                                                  \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n",          \
                         __FILE__, __LINE__, #condition);                  \
        }                                                                  \
    } while (0)

WotbModV3Result WOTBMOD_V3_CALL MockQueryInterface(
    WotbModV3Handle,
    const char*,
    uint32_t,
    const void** out_interface) {
    if (!out_interface) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    *out_interface = nullptr;
    return WOTBMOD_V3_E_NOT_SUPPORTED;
}

WotbModV3Result WOTBMOD_V3_CALL MockGetInterfaceInfo(
    WotbModV3Handle,
    const char* interface_name,
    WotbModV3InterfaceInfo* out_info) {
    if (!interface_name || !out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->status = WOTBMOD_V3_CAPABILITY_UNAVAILABLE;
    strncpy_s(out_info->name, interface_name, _TRUNCATE);
    strncpy_s(out_info->unavailable_reason, "host contract mock", _TRUNCATE);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL MockGetLastError(
    WotbModV3Handle,
    WotbModV3ErrorInfo* out_error) {
    if (!out_error) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_error, 0, sizeof(*out_error));
    WOTBMOD_V3_INIT_STRUCT(*out_error, WOTBMOD_V3_ABI_VERSION);
    return WOTBMOD_V3_OK;
}

WotbModV3Result WOTBMOD_V3_CALL MockGetClientInfo(
    WotbModV3Handle,
    WotbModV3ClientInfo* out_info) {
    if (!out_info) return WOTBMOD_V3_E_INVALID_ARGUMENT;
    std::memset(out_info, 0, sizeof(*out_info));
    WOTBMOD_V3_INIT_STRUCT(*out_info, WOTBMOD_V3_ABI_VERSION);
    out_info->supported = 1u;
    out_info->compatibility_state = WOTBMOD_V3_CLIENT_COMPATIBILITY_SUPPORTED;
    out_info->binding_pack_version = 112000887u;
    strncpy_s(out_info->client_version, "11.20.0.887", _TRUNCATE);
    strncpy_s(
        out_info->executable_sha256,
        "4813544d3d6b9f45a357e87f5a14d065bd108c4acd324ac1506cb6d1ad6ff0af",
        _TRUNCATE);
    return WOTBMOD_V3_OK;
}

template <typename T>
T Export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 || !argv[1] || !argv[1][0]) {
        std::fprintf(stderr, "usage: native_validation_host_tests <dll>\n");
        return 2;
    }
    HMODULE module = LoadLibraryA(argv[1]);
    CHECK(module != nullptr);
    if (!module) return 1;

    using CountFn = uint32_t(WOTBMOD_V3_CALL*)();
    using StringAtFn = const char*(WOTBMOD_V3_CALL*)(uint32_t);
    using NativeAtFn = uint32_t(WOTBMOD_V3_CALL*)(uint32_t);
    using StatusFn = const char*(WOTBMOD_V3_CALL*)(uint32_t, const char*);
    using SanitizeFn = uint32_t(WOTBMOD_V3_CALL*)(
        const char*, char*, uint32_t);
    using FlagsFn = uint32_t(WOTBMOD_V3_CALL*)();
    using PerfFn = uint64_t(WOTBMOD_V3_CALL*)(uint32_t);
    using FingerprintStatusFn = const char*(WOTBMOD_V3_CALL*)(
        uint32_t, const char*, const char*, const char*);

    const CountFn count = Export<CountFn>(
        module, "WotbNativeValidation_GetCapabilityCount");
    const StringAtFn id_at = Export<StringAtFn>(
        module, "WotbNativeValidation_GetCapabilityId");
    const StringAtFn section_at = Export<StringAtFn>(
        module, "WotbNativeValidation_GetCapabilitySection");
    const NativeAtFn native_at = Export<NativeAtFn>(
        module, "WotbNativeValidation_GetCapabilityNative");
    const StatusFn status_after = Export<StatusFn>(
        module, "WotbNativeValidation_StatusAfterVerdict");
    const SanitizeFn sanitize = Export<SanitizeFn>(
        module, "WotbNativeValidation_SanitizeComment");
    const FlagsFn contract_flags = Export<FlagsFn>(
        module, "WotbNativeValidation_GetRuntimeContractFlags");
    const PerfFn perf = Export<PerfFn>(
        module, "WotbNativeValidation_GetPerfCounter");
    const FingerprintStatusFn fingerprint_status =
        Export<FingerprintStatusFn>(
            module,
            "WotbNativeValidation_StatusAfterFingerprintVerdict");
    const WotbModLoadV3Fn entry = Export<WotbModLoadV3Fn>(
        module, WOTBMOD_V3_ENTRY_NAME);
    CHECK(count && id_at && section_at && native_at && status_after &&
          sanitize && contract_flags && perf && fingerprint_status && entry);

    if (contract_flags && perf) {
        CHECK(contract_flags() == 0x0fu);
        for (uint32_t index = 0u; index < 7u; ++index) {
            CHECK(perf(index) == 0u);
        }
    }

    if (count && id_at && section_at && native_at) {
        // 57 API capabilities plus one row per published hook symbol; the
        // 42 hook rows are cross-checked against the binding pack by
        // _mod_tools/test_hook_symbol_table.py.
        CHECK(count() == 103u);
        const std::set<std::string> required_sections = {
            "Client", "Lifecycle", "Camera", "Events", "BigWorld Entity",
            "RPC Metadata", "Shell / Projectile", "Tracer",
            "Render / D3D11 / DXGI", "UI", "Scene", "Material", "Audio",
            "Resources", "Reload", "Errors", "GES", "Session", "Hooks"};
        std::set<std::string> ids;
        std::set<std::string> sections;
        uint32_t native_count = 0u;
        for (uint32_t index = 0u; index < count(); ++index) {
            const char* id = id_at(index);
            const char* section = section_at(index);
            CHECK(id && id[0]);
            CHECK(section && section[0]);
            if (id) CHECK(ids.insert(id).second);
            if (section) sections.insert(section);
            native_count += native_at(index) ? 1u : 0u;
        }
        CHECK(ids.size() == count());
        for (const char* ges_id : {"ges.observe_all", "ges.event_count",
                                   "ges.publish_echo"}) {
            CHECK(ids.count(ges_id) == 1u);
        }
        CHECK(sections == required_sections);
        CHECK(native_count >= 20u);
        CHECK(id_at(count()) == nullptr);
        CHECK(section_at(count()) == nullptr);
        CHECK(native_at(count()) == 0u);
    }

    if (status_after) {
        CHECK(std::strcmp(status_after(1u, nullptr), "LIVE_TEST_PENDING") == 0);
        CHECK(std::strcmp(status_after(1u, "SKIP"), "LIVE_TEST_PENDING") == 0);
        CHECK(std::strcmp(status_after(0u, nullptr), "HOST_TESTED") == 0);
        CHECK(std::strcmp(status_after(1u, "PASS"), "LIVE_TEST_PENDING") == 0);
        CHECK(std::strcmp(status_after(1u, "FAIL"), "FAILED") == 0);
    }
    if (fingerprint_status) {
        CHECK(std::strcmp(
            fingerprint_status(1u, "PASS", "exact", "exact"),
            "SUPPORTED") == 0);
        CHECK(std::strcmp(
            fingerprint_status(1u, "PASS", "old", "current"),
            "LIVE_TEST_PENDING") == 0);
        CHECK(std::strcmp(
            fingerprint_status(1u, "PASS", nullptr, "current"),
            "LIVE_TEST_PENDING") == 0);
    }

    if (sanitize) {
        char output[256] = {};
        CHECK(sanitize("visual event observed", output, sizeof(output)) > 0u);
        CHECK(std::strcmp(output, "visual event observed") == 0);
        CHECK(sanitize("line one\nline two", output, sizeof(output)) > 0u);
        CHECK(std::strcmp(output, "line one line two") == 0);
        CHECK(sanitize("password=secret", output, sizeof(output)) > 0u);
        CHECK(std::strcmp(output, "[REDACTED_BY_PRIVACY_POLICY]") == 0);
        CHECK(sanitize("person@example.test", output, sizeof(output)) > 0u);
        CHECK(std::strcmp(output, "[REDACTED_BY_PRIVACY_POLICY]") == 0);
        char too_small[4] = {};
        CHECK(sanitize("long", too_small, sizeof(too_small)) == 0u);
        CHECK(sanitize("x", nullptr, 0u) == 0u);
    }

    if (entry) {
        WotbModV3Bootstrap bootstrap = {};
        WOTBMOD_V3_INIT_STRUCT(bootstrap, WOTBMOD_V3_ABI_VERSION);
        bootstrap.sdk_version = WOTBMOD_V3_SDK_VERSION;
        bootstrap.bootstrap_version = WOTBMOD_V3_BOOTSTRAP_VERSION;
        bootstrap.query_interface = &MockQueryInterface;
        bootstrap.get_interface_info = &MockGetInterfaceInfo;
        bootstrap.get_last_error = &MockGetLastError;
        bootstrap.get_client_info = &MockGetClientInfo;
        WotbModV3Info info = {};
        CHECK(entry(nullptr, 1u, &info) == WOTBMOD_V3_E_INVALID_ARGUMENT);
        CHECK(entry(&bootstrap, WOTBMOD_V3_INVALID_HANDLE, &info) ==
              WOTBMOD_V3_E_INVALID_ARGUMENT);
        CHECK(entry(&bootstrap, 1u, nullptr) == WOTBMOD_V3_E_INVALID_ARGUMENT);
        CHECK(entry(&bootstrap, 1u, &info) == WOTBMOD_V3_OK);
        CHECK(std::strcmp(info.id, "wotbmod.native_validation") == 0);
        // Must equal manifest.json's version and the package's. The loader
        // refuses a mod whose binary and manifest disagree, so this assertion
        // is what stops the two drifting apart in the source tree.
        CHECK(std::strcmp(info.version, "1.1.1") == 0);
        CHECK(info.requested_permission_tier == WOTBMOD_V3_PERMISSION_UNSAFE);
        CHECK(info.on_enable && info.on_disable && info.on_unload);
        CHECK(info.on_frame == nullptr);
    }

    FreeLibrary(module);
    if (g_failures != 0u) {
        std::fprintf(stderr, "NATIVE VALIDATION HOST FAILED: %u/%u\n",
                     g_failures, g_checks);
        return 1;
    }
    std::printf("NATIVE VALIDATION HOST OK: %u assertions, 103 capabilities, 19 sections\n",
                g_checks);
    return 0;
}
