#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../include/wotb_mod_api.h"

static WotbModResourceMountId g_lowMount = 0;
static WotbModResourceMountId g_highMount = 0;
static const DWORD kAssertionException = 0xE0000001u;

static void Require(bool condition) {
    if (!condition) {
        RaiseException(kAssertionException, 0, 0, nullptr);
    }
}

static bool WriteFixture(
    const char* directory,
    const char* value) {
    if (!CreateDirectoryA(directory, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    char path[WOTBMOD_MAX_PATH] = {};
    if (_snprintf_s(
            path,
            sizeof(path),
            _TRUNCATE,
            "%s\\value.txt",
            directory) <= 0) {
        return false;
    }
    HANDLE file = CreateFileA(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD size = (DWORD)strlen(value);
    const BOOL ok =
        WriteFile(file, value, size, &written, nullptr);
    CloseHandle(file);
    return ok && written == size;
}

static bool EnsureFixtures(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    char dataPath[WOTBMOD_MAX_PATH] = {};
    uint32_t size = sizeof(dataPath);
    if (host->get_path(
            mod,
            WOTBMOD_PATH_DATA,
            dataPath,
            &size) != WOTBMOD_OK) {
        return false;
    }
    char low[WOTBMOD_MAX_PATH] = {};
    char high[WOTBMOD_MAX_PATH] = {};
    if (_snprintf_s(
            low,
            sizeof(low),
            _TRUNCATE,
            "%s\\low",
            dataPath) <= 0 ||
        _snprintf_s(
            high,
            sizeof(high),
            _TRUNCATE,
            "%s\\high",
            dataPath) <= 0) {
        return false;
    }
    return WriteFixture(low, "low") &&
           WriteFixture(high, "high");
}

static WotbModResult Mount(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const char* source,
    int32_t priority,
    WotbModResourceMountId* outMount) {
    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Tests/Priority/";
    mount.source_directory = source;
    mount.priority = priority;
    mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;
    return host->resource_mount(mod, &mount, outMount);
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    Require(
        host->struct_size >=
            offsetof(WotbModHostApi, resource_unmount) +
                sizeof(host->resource_unmount));
    Require(EnsureFixtures(host, mod));

    WotbModResourceMountId invalidMount = 0;
    Require(
        Mount(host, mod, "../escape", 1000, &invalidMount) ==
        WOTBMOD_ERROR_INVALID_ARGUMENT);
    Require(Mount(host, mod, "low", 10, &g_lowMount) == WOTBMOD_OK);
    Require(Mount(host, mod, "high", 20, &g_highMount) == WOTBMOD_OK);
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    if (g_highMount) {
        host->resource_unmount(mod, g_highMount);
        g_highMount = 0;
    }
    if (g_lowMount) {
        host->resource_unmount(mod, g_lowMount);
        g_lowMount = 0;
    }
}

WOTBMOD_ENTRY {
    (void)mod;
    if (!host || !out_info) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.resources";
    out_info->name = "Resource Overlay Test";
    out_info->version = "1.2.0";
    out_info->author = "SDK tests";
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    return WOTBMOD_OK;
}
