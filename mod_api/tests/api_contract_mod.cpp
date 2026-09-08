#include <string.h>

#include "../include/wotb_mod_api.h"

static const WotbModHostApi* g_host = nullptr;
static WotbModHandle g_mod = nullptr;
static volatile long g_enableCount = 0;
static volatile long g_disableCount = 0;
static volatile long g_unloadCount = 0;
static volatile long g_frameCount = 0;
static WotbModFrameInfo g_lastFrame = {};

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    ++g_enableCount;
    host->log(mod, WOTBMOD_LOG_INFO, "contract:on_enable");
    wotbmod_logf(
        host,
        mod,
        WOTBMOD_LOG_WARNING,
        "contract:formatted:%d:%s",
        42,
        "ok");
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    ++g_disableCount;
    host->log(mod, WOTBMOD_LOG_INFO, "contract:on_disable");
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    ++g_unloadCount;
    host->log(mod, WOTBMOD_LOG_INFO, "contract:on_unload");
    g_host = nullptr;
    g_mod = nullptr;
}

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModFrameInfo* frame) {
    ++g_frameCount;
    if (frame) g_lastFrame = *frame;
}

WOTBMOD_EXPORT WotbModHandle WOTBMOD_CALL
WotbTestContract_GetHandle(void) {
    return g_mod;
}

WOTBMOD_EXPORT const WotbModHostApi* WOTBMOD_CALL
WotbTestContract_GetHost(void) {
    return g_host;
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbTestContract_GetEnableCount(void) {
    return (uint32_t)g_enableCount;
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbTestContract_GetDisableCount(void) {
    return (uint32_t)g_disableCount;
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbTestContract_GetUnloadCount(void) {
    return (uint32_t)g_unloadCount;
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbTestContract_GetFrameCount(void) {
    return (uint32_t)g_frameCount;
}

WOTBMOD_EXPORT int32_t WOTBMOD_CALL
WotbTestContract_CopyLastFrame(WotbModFrameInfo* frame) {
    if (!frame || frame->struct_size < sizeof(*frame)) return 0;
    *frame = g_lastFrame;
    return 1;
}

WOTBMOD_ENTRY {
    if (!host || !mod || !out_info) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    g_host = host;
    g_mod = mod;
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.contract";
    out_info->name = "Full API Contract Test";
    out_info->version = "2.9.0";
    out_info->author = "SDK tests";
    out_info->description =
        "Exposes its host handle to the full public API test harness.";
    out_info->flags = 0xA5u;
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    out_info->on_frame = &OnFrame;
    return WOTBMOD_OK;
}
