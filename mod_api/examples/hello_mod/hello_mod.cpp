#include <string.h>

#include "../../include/wotb_mod_api.h"

static const WotbModHostApi* g_host = nullptr;
static WotbModHandle g_mod = nullptr;
static uint64_t g_frames = 0;
static WotbModResourceMountId g_resourceMount = 0;
static WotbModResourceHandle g_uiPackage = nullptr;

static bool HasResourceApi(const WotbModHostApi* host) {
    return host &&
           host->struct_size >=
               offsetof(WotbModHostApi, resource_release) +
                   sizeof(host->resource_release) &&
           host->resource_mount &&
           host->resource_unmount &&
           host->resource_load &&
           host->resource_release;
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    host->log(mod, WOTBMOD_LOG_INFO, "enabled");

    const int32_t enableCount =
        host->config_get_int(mod, "hello", "enable_count", 0) + 1;
    host->config_set_int(mod, "hello", "enable_count", enableCount);

    if (!HasResourceApi(host)) return;
    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Mods/example.hello/";
    mount.source_directory = "resources";
    mount.priority = 100;
    mount.flags = WOTBMOD_RESOURCE_MOUNT_SEARCH_DVPL;
    WotbModResult mountResult =
        host->resource_mount(mod, &mount, &g_resourceMount);
    if (mountResult != WOTBMOD_OK) {
        wotbmod_logf(
            host, mod, WOTBMOD_LOG_WARNING,
            "resource mount unavailable (%d)", (int)mountResult);
        return;
    }

    WotbModResourceLoadRequest request = {};
    request.struct_size = sizeof(request);
    request.type = WOTBMOD_RESOURCE_UI_PACKAGE;
    request.virtual_path = "~res:/Mods/example.hello/example.yaml";
    WotbModResult loadResult =
        host->resource_load(mod, &request, &g_uiPackage);
    if (loadResult != WOTBMOD_OK) {
        wotbmod_logf(
            host, mod, WOTBMOD_LOG_WARNING,
            "UI package bridge unavailable (%d)", (int)loadResult);
    }
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    if (HasResourceApi(host)) {
        if (g_uiPackage) {
            host->resource_release(mod, g_uiPackage);
            g_uiPackage = nullptr;
        }
        if (g_resourceMount) {
            host->resource_unmount(mod, g_resourceMount);
            g_resourceMount = 0;
        }
    }
    host->log(mod, WOTBMOD_LOG_INFO, "disabled");
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    host->log(mod, WOTBMOD_LOG_INFO, "unloaded");
    g_resourceMount = 0;
    g_uiPackage = nullptr;
    g_host = nullptr;
    g_mod = nullptr;
}

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModFrameInfo* frame) {
    ++g_frames;
    if (g_frames == 1 || (g_frames % 600) == 0) {
        wotbmod_logf(
            host,
            mod,
            WOTBMOD_LOG_TRACE,
            "frame=%llu delta=%.4f backbuffer=%ux%u",
            (unsigned long long)frame->frame_index,
            frame->delta_seconds,
            frame->back_buffer_width,
            frame->back_buffer_height);
    }
}

WOTBMOD_ENTRY {
    if (!host || !out_info ||
        WOTBMOD_ABI_MAJOR(host->abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }

    g_host = host;
    g_mod = mod;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "example.hello";
    out_info->name = "Hello Mod";
    out_info->version = "1.0.0";
    out_info->author = "SDK example";
    out_info->description =
        "Lifecycle, config, resource overlay, and UI package example.";
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    out_info->on_frame = &OnFrame;
    return WOTBMOD_OK;
}
