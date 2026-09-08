#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../include/wotb_mod_runtime.h"

static volatile LONG g_resourceLoads = 0;
static volatile LONG g_resourceReloads = 0;
static volatile LONG g_resourceReleases = 0;
static volatile LONG g_registryChanges = 0;
static volatile LONG64 g_lastRegistryGeneration = 0;

static void WOTBMOD_CALL LogSink(
    WotbModLogLevel level,
    const char* message,
    void*) {
    const char* name = "info";
    if (level == WOTBMOD_LOG_TRACE) name = "trace";
    if (level == WOTBMOD_LOG_WARNING) name = "warning";
    if (level == WOTBMOD_LOG_ERROR) name = "error";
    printf("[%s] %s\n", name, message ? message : "");
}

static WotbModResult WOTBMOD_CALL ResourceLoad(
    void*,
    const WotbModResourceLoadRequest* request,
    void** outNativeResource) {
    if (!request ||
        request->struct_size < sizeof(*request) ||
        request->type != WOTBMOD_RESOURCE_UI_PACKAGE ||
        !request->virtual_path ||
        strcmp(
            request->virtual_path,
            "~res:/Mods/example.hello/example.yaml") != 0 ||
        !outNativeResource) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    void* resource = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 16);
    if (!resource) return WOTBMOD_ERROR_PLATFORM;
    *outNativeResource = resource;
    InterlockedIncrement(&g_resourceLoads);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceReload(
    void*,
    void* nativeResource) {
    if (!nativeResource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    InterlockedIncrement(&g_resourceReloads);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ResourceRelease(
    void*,
    void* nativeResource) {
    if (!nativeResource) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (!HeapFree(GetProcessHeap(), 0, nativeResource)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedIncrement(&g_resourceReleases);
    return WOTBMOD_OK;
}

static void WOTBMOD_CALL RegistryChanged(
    void*,
    uint64_t generation) {
    InterlockedIncrement(&g_registryChanges);
    InterlockedExchange64(
        &g_lastRegistryGeneration, (LONG64)generation);
}

static int Fail(const char* message) {
    fprintf(stderr, "SMOKE FAILED: %s\n", message);
    WotbModRuntime_Shutdown();
    return 1;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: smoke_host.exe <test-game-directory>\n");
        return 2;
    }

    WotbModRuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.game_directory = argv[1];
    options.game_module = GetModuleHandleA(nullptr);
    options.log_sink = &LogSink;
    WotbModRuntimeResourceBackend resources = {};
    resources.struct_size = sizeof(resources);
    resources.load = &ResourceLoad;
    resources.reload = &ResourceReload;
    resources.release = &ResourceRelease;
    resources.registry_changed = &RegistryChanged;
    options.resource_backend = &resources;

    if (WotbModRuntime_Initialize(&options) != WOTBMOD_OK) {
        return Fail("runtime initialization");
    }
    if (WotbModRuntime_LoadAll() != WOTBMOD_OK) {
        return Fail("load all");
    }

    const WotbModHostApi* host = WotbModRuntime_GetHostApi();
    if (!host || host->get_mod_count() != 3) {
        return Fail("expected three loaded mods");
    }
    if (host->struct_size < sizeof(*host) ||
        !host->resource_mount ||
        !host->resource_resolve ||
        !host->resource_load ||
        g_resourceLoads != 1 ||
        g_registryChanges < 1 ||
        WotbModRuntime_GetResourceGeneration() !=
            (uint64_t)g_lastRegistryGeneration) {
        return Fail("resource API initialization");
    }

    const char* resourcePath =
        "~res:/Mods/example.hello/example.yaml";
    uint32_t requiredPathSize = 0;
    if (WotbModRuntime_ResolveResourcePath(
            resourcePath, nullptr, &requiredPathSize) !=
            WOTBMOD_ERROR_BUFFER_TOO_SMALL ||
        requiredPathSize == 0) {
        return Fail("resource path size contract");
    }
    char resolvedPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t resolvedPathSize = sizeof(resolvedPath);
    if (WotbModRuntime_ResolveResourcePath(
            resourcePath, resolvedPath, &resolvedPathSize) != WOTBMOD_OK ||
        strstr(resolvedPath, "example.yaml.dvpl") == nullptr ||
        GetFileAttributesA(resolvedPath) == INVALID_FILE_ATTRIBUTES) {
        return Fail("resource overlay resolution");
    }
    char dataStyleResolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t dataStyleSize = sizeof(dataStyleResolved);
    if (WotbModRuntime_ResolveResourcePath(
            "Data/Mods/example.hello/example.yaml",
            dataStyleResolved,
            &dataStyleSize) != WOTBMOD_OK ||
        _stricmp(dataStyleResolved, resolvedPath) != 0) {
        return Fail("Data path normalization");
    }
    char priorityPath[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t priorityPathSize = sizeof(priorityPath);
    if (WotbModRuntime_ResolveResourcePath(
            "~res:/Tests/Priority/value.txt",
            priorityPath,
            &priorityPathSize) != WOTBMOD_OK ||
        strstr(priorityPath, "\\high\\value.txt") == nullptr) {
        return Fail("resource priority resolution");
    }
    priorityPathSize = sizeof(priorityPath);
    if (WotbModRuntime_ResolveResourcePath(
            "~res:/Tests/Priority/value.txt.dvpl",
            priorityPath,
            &priorityPathSize) != WOTBMOD_OK ||
        strstr(priorityPath, "\\high\\value.txt") == nullptr) {
        return Fail("loose fallback for a DVPL request");
    }
    int helloIndex = -1;
    int faultIndex = -1;
    for (uint32_t index = 0; index < host->get_mod_count(); ++index) {
        WotbModPublicInfo candidate = {};
        candidate.struct_size = sizeof(candidate);
        if (host->get_mod_info(index, &candidate) != WOTBMOD_OK) {
            return Fail("loaded mod metadata");
        }
        if (strcmp(candidate.id, "example.hello") == 0) helloIndex = (int)index;
        if (strcmp(candidate.id, "test.fault") == 0) faultIndex = (int)index;
    }
    if (helloIndex < 0 || faultIndex < 0) {
        return Fail("expected mod ids were not loaded");
    }

    WotbModRuntime_DispatchFrame(nullptr, nullptr, nullptr, 1920, 1080, 0.016);
    WotbModRuntime_DispatchFrame(nullptr, nullptr, nullptr, 1920, 1080, 0.016);
    WotbModRuntime_DispatchFrame(nullptr, nullptr, nullptr, 1920, 1080, 0.016);

    WotbModPublicInfo faultInfo = {};
    faultInfo.struct_size = sizeof(faultInfo);
    if (host->get_mod_info((uint32_t)faultIndex, &faultInfo) != WOTBMOD_OK ||
        faultInfo.enabled ||
        faultInfo.state != WOTBMOD_STATE_FAULTED ||
        faultInfo.fault_count != 1) {
        return Fail("fault isolation state");
    }

    if (host->set_mod_enabled("example.hello", 0) != WOTBMOD_OK) {
        return Fail("disable lifecycle");
    }
    uint32_t disabledPathSize = sizeof(resolvedPath);
    if (g_resourceReleases != 1 ||
        WotbModRuntime_ResolveResourcePath(
            resourcePath, resolvedPath, &disabledPathSize) !=
            WOTBMOD_ERROR_NOT_FOUND) {
        return Fail("resource cleanup on disable");
    }
    WotbModPublicInfo info = {};
    info.struct_size = sizeof(info);
    if (host->get_mod_info((uint32_t)helloIndex, &info) != WOTBMOD_OK ||
        info.enabled ||
        info.state != WOTBMOD_STATE_DISABLED) {
        return Fail("disabled state");
    }
    if (host->set_mod_enabled("example.hello", 1) != WOTBMOD_OK) {
        return Fail("enable lifecycle");
    }
    resolvedPathSize = sizeof(resolvedPath);
    if (g_resourceLoads != 2 ||
        WotbModRuntime_ResolveResourcePath(
            resourcePath, resolvedPath, &resolvedPathSize) != WOTBMOD_OK) {
        return Fail("resource remount on enable");
    }

    WotbModRuntime_Shutdown();
    if (g_resourceReleases != 2) {
        return Fail("resource cleanup on shutdown");
    }
    printf(
        "SMOKE OK: mods=3 lifecycle+fault-isolation+resources passed\n");
    return 0;
}
