#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/wotb_mod_api_v3.h"

namespace {

volatile LONG g_probeCount = 0;
volatile LONG g_probeFailures = 0;
volatile LONG g_probeFailureMask = 0;

void ProbePackage(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    InterlockedIncrement(&g_probeCount);
    if (!bootstrap || !bootstrap->query_interface ||
        !bootstrap->get_client_info) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 1);
        return;
    }
    WotbModV3ClientInfo client = {};
    client.struct_size = sizeof(client);
    client.api_version = WOTBMOD_V3_ABI_VERSION;
    if (bootstrap->get_client_info(mod, &client) != WOTBMOD_V3_OK ||
        strcmp(client.client_version, "package-test-build") != 0 ||
        strcmp(
            client.executable_sha256,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") != 0) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 2);
        return;
    }
    const void* rawLifecycle = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_LIFECYCLE,
            WOTBMOD_V3_LIFECYCLE_VERSION,
            &rawLifecycle) != WOTBMOD_V3_OK ||
        !rawLifecycle) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 1024);
        return;
    }
    const WotbModV3LifecycleApiV1* lifecycle =
        static_cast<const WotbModV3LifecycleApiV1*>(
            rawLifecycle);
    char installPath[WOTBMOD_V3_MAX_PATH] = {};
    char resourcePath[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t installSize = sizeof(installPath);
    uint32_t resourceSize = sizeof(resourcePath);
    if (lifecycle->get_install_path(
            mod,
            installPath,
            &installSize) != WOTBMOD_V3_OK ||
        lifecycle->get_resource_path(
            mod,
            resourcePath,
            &resourceSize) != WOTBMOD_V3_OK ||
        _stricmp(installPath, resourcePath) == 0 ||
        strstr(installPath, "\\..\\") != nullptr ||
        strstr(resourcePath, "\\..\\") != nullptr) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 2048);
        return;
    }
    char installedDll[WOTBMOD_V3_MAX_PATH] = {};
    char packageAsset[WOTBMOD_V3_MAX_PATH] = {};
    if (_snprintf_s(
            installedDll,
            sizeof(installedDll),
            _TRUNCATE,
            "%s\\package_mod.dll",
            installPath) < 0 ||
        _snprintf_s(
            packageAsset,
            sizeof(packageAsset),
            _TRUNCATE,
            "%s\\asset.txt",
            resourcePath) < 0 ||
        GetFileAttributesA(installedDll) ==
            INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(packageAsset) ==
            INVALID_FILE_ATTRIBUTES) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 2048);
        return;
    }

    const void* rawIntermod = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_INTERMOD,
            WOTBMOD_V3_INTERMOD_VERSION,
            &rawIntermod) != WOTBMOD_V3_OK ||
        !rawIntermod) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 4096);
        return;
    }
    const WotbModV3IntermodApiV1* intermod =
        static_cast<const WotbModV3IntermodApiV1*>(
            rawIntermod);
    WotbModV3Handle dependency =
        WOTBMOD_V3_INVALID_HANDLE;
    uint32_t optional = 99u;
    char dependencyVersion[WOTBMOD_V3_MAX_VERSION] = {};
    uint32_t dependencyVersionSize =
        sizeof(dependencyVersion);
    if (intermod->mod_get_dependency(
            mod,
            "tests.package-content",
            &dependency,
            &optional) != WOTBMOD_V3_OK ||
        dependency == WOTBMOD_V3_INVALID_HANDLE ||
        optional != 0u ||
        intermod->mod_get_version(
            mod,
            dependency,
            dependencyVersion,
            &dependencyVersionSize) != WOTBMOD_V3_OK ||
        strcmp(dependencyVersion, "1.0.0") != 0) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 4096);
        return;
    }
    dependency = static_cast<WotbModV3Handle>(123u);
    optional = 0u;
    if (intermod->mod_get_dependency(
            mod,
            "tests.package-optional",
            &dependency,
            &optional) != WOTBMOD_V3_E_NOT_FOUND ||
        dependency != WOTBMOD_V3_INVALID_HANDLE ||
        optional != 1u) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 8192);
        return;
    }
    dependency = static_cast<WotbModV3Handle>(123u);
    optional = 99u;
    if (intermod->mod_get_dependency(
            mod,
            "tests.package-loose",
            &dependency,
            &optional) != WOTBMOD_V3_E_NOT_FOUND ||
        dependency != WOTBMOD_V3_INVALID_HANDLE ||
        optional != 0u) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 16384);
        return;
    }
    const void* forbiddenRpc =
        reinterpret_cast<const void*>(1u);
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_BIGWORLD_RPC,
            WOTBMOD_V3_BIGWORLD_RPC_VERSION,
            &forbiddenRpc) !=
            WOTBMOD_V3_E_PERMISSION_DENIED ||
        forbiddenRpc != nullptr) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 512);
        return;
    }
    const void* rawVfs = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_VFS,
            WOTBMOD_V3_VFS_VERSION,
            &rawVfs) != WOTBMOD_V3_OK ||
        !rawVfs) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 4);
        return;
    }
    const WotbModV3VfsApiV1* vfs =
        static_cast<const WotbModV3VfsApiV1*>(rawVfs);
    char path[WOTBMOD_V3_MAX_PATH] = {};
    uint32_t pathSize = sizeof(path);
    if (vfs->resolve(
            mod,
            "mod://self/asset.txt",
            path,
            &pathSize) != WOTBMOD_V3_OK) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 8);
        return;
    }
    HANDLE file = CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 16);
        return;
    }
    char bytes[16] = {};
    DWORD read = 0u;
    const BOOL readOk = ReadFile(
        file,
        bytes,
        static_cast<DWORD>(sizeof(bytes) - 1u),
        &read,
        nullptr);
    CloseHandle(file);
    if (!readOk || read != 7u ||
        memcmp(bytes, "mounted", 7u) != 0) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 32);
        return;
    }

    const void* rawLoaders = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_LOADERS,
            WOTBMOD_V3_LOADERS_VERSION,
            &rawLoaders) != WOTBMOD_V3_OK ||
        !rawLoaders) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 64);
        return;
    }
    const WotbModV3LoadersApiV1* loaders =
        static_cast<const WotbModV3LoadersApiV1*>(rawLoaders);
    WotbModV3Buffer unpacked = {};
    unpacked.struct_size = sizeof(unpacked);
    unpacked.api_version = WOTBMOD_V3_ABI_VERSION;
    if (loaders->unpack_dvpl(
            mod,
            "mod://self/packed.dvpl",
            &unpacked) != WOTBMOD_V3_E_BUFFER_TOO_SMALL ||
        unpacked.size != 12u) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 128);
        return;
    }
    char unpackedBytes[12] = {};
    unpacked.data = unpackedBytes;
    unpacked.capacity = sizeof(unpackedBytes);
    if (loaders->unpack_dvpl(
            mod,
            "mod://self/packed.dvpl",
            &unpacked) != WOTBMOD_V3_OK ||
        unpacked.size != sizeof(unpackedBytes) ||
        memcmp(
            unpackedBytes,
            "dvpl-mounted",
            sizeof(unpackedBytes)) != 0) {
        InterlockedIncrement(&g_probeFailures);
        InterlockedOr(&g_probeFailureMask, 256);
    }
}

/*
 * Hot-reload probe, driven by the test through the process environment so
 * it survives the DLL being unloaded and loaded again: the test sets
 * WOTBMOD_TEST_REQUEST_RELOAD=1 and re-enables the mod; this instance asks
 * can_hot_reload, requests its own reload, asks again (a queued reload must
 * now be reported as not possible), writes the three results to
 * WOTBMOD_TEST_RELOAD_RESULT and clears the request so the instance that
 * comes back after the frame boundary does not loop. Every entry bumps
 * WOTBMOD_TEST_LOAD_GENERATION, which is how the test proves a fresh instance
 * really came up.
 */
void BumpLoadGeneration() {
    char text[16] = {};
    const DWORD length = GetEnvironmentVariableA(
        "WOTBMOD_TEST_LOAD_GENERATION", text, sizeof(text));
    const int generation =
        length > 0u && length < sizeof(text) ? atoi(text) : 0;
    char next[16] = {};
    sprintf_s(next, sizeof(next), "%d", generation + 1);
    SetEnvironmentVariableA("WOTBMOD_TEST_LOAD_GENERATION", next);
}

void MaybeRequestReload(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    char flag[8] = {};
    if (GetEnvironmentVariableA(
            "WOTBMOD_TEST_REQUEST_RELOAD", flag, sizeof(flag)) == 0u ||
        strcmp(flag, "1") != 0) {
        return;
    }
    SetEnvironmentVariableA("WOTBMOD_TEST_REQUEST_RELOAD", "0");
    const void* rawLifecycle = nullptr;
    if (!bootstrap || !bootstrap->query_interface ||
        bootstrap->query_interface(
            mod, WOTBMOD_V3_IFACE_LIFECYCLE, WOTBMOD_V3_LIFECYCLE_VERSION,
            &rawLifecycle) != WOTBMOD_V3_OK || !rawLifecycle) {
        SetEnvironmentVariableA("WOTBMOD_TEST_RELOAD_RESULT", "no-lifecycle");
        return;
    }
    const WotbModV3LifecycleApiV1* lifecycle =
        static_cast<const WotbModV3LifecycleApiV1*>(rawLifecycle);
    uint32_t canBefore = 99u;
    uint32_t canAfter = 99u;
    const WotbModV3Result before =
        lifecycle->can_hot_reload(mod, mod, &canBefore, nullptr, nullptr);
    const WotbModV3Result request = lifecycle->request_reload(mod, mod);
    const WotbModV3Result after =
        lifecycle->can_hot_reload(mod, mod, &canAfter, nullptr, nullptr);
    char text[96] = {};
    sprintf_s(
        text, sizeof(text), "before=%d/%u request=%d after=%d/%u",
        static_cast<int>(before), canBefore, static_cast<int>(request),
        static_cast<int>(after), canAfter);
    SetEnvironmentVariableA("WOTBMOD_TEST_RELOAD_RESULT", text);
}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {
    ProbePackage(bootstrap, mod);
    MaybeRequestReload(bootstrap, mod);
}

}  // namespace

extern "C" __declspec(dllexport) uint32_t
WOTBMOD_V3_CALL PackageRuntimeProbeCount() {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_probeCount, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t
WOTBMOD_V3_CALL PackageRuntimeProbeFailures() {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_probeFailures, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t
WOTBMOD_V3_CALL PackageRuntimeProbeFailureMask() {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_probeFailureMask, 0, 0));
}

WOTBMOD_V3_ENTRY {
    if (!bootstrap || !out_info) {
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }
    BumpLoadGeneration();
    ProbePackage(bootstrap, mod);
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier =
        WOTBMOD_V3_PERMISSION_UNSAFE;
    strcpy_s(
        out_info->id,
        sizeof(out_info->id),
        "tests.package-runtime");
    strcpy_s(
        out_info->name,
        sizeof(out_info->name),
        "Package Runtime Test");
    strcpy_s(
        out_info->version,
        sizeof(out_info->version),
        "1.2.3");
    strcpy_s(
        out_info->author,
        sizeof(out_info->author),
        "tests");
    out_info->on_enable = &OnEnable;
    return WOTBMOD_V3_OK;
}
