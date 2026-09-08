#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <float.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../include/wotb_mod_api.h"

namespace {

const uint32_t kExpectedEventMask =
    WOTBMOD_EVENT_UI_INPUT |
    WOTBMOD_EVENT_SHELL_HIT |
    WOTBMOD_EVENT_AMMO_CHANGED |
    WOTBMOD_EVENT_AIM_TARGET_CHANGED |
    WOTBMOD_EVENT_VEHICLE_SPOTTED |
    WOTBMOD_EVENT_VEHICLE_UNSPOTTED |
    WOTBMOD_EVENT_CAMERA_MODE_CHANGED;

const WotbModHostApi* g_host = nullptr;
WotbModHandle g_mod = nullptr;
WotbModEventSubscriptionId g_subscription = 0;
volatile LONG g_passes = 0;
volatile LONG g_failures = 0;
volatile LONG g_seenMask = 0;
volatile LONG g_finished = 0;
DWORD g_enableThreadId = 0;
uint64_t g_lastSequence = 0;
bool g_threadChecked = false;
bool g_borrowedVehicleChecked = false;

static void Log(
    WotbModLogLevel level,
    const char* status,
    const char* test) {
    if (!g_host || !g_mod) return;
    char message[256] = {};
    _snprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        "new-events:%s:%s",
        status ? status : "INFO",
        test ? test : "");
    g_host->log(g_mod, level, message);
}

static void Pass(const char* test) {
    InterlockedIncrement(&g_passes);
    Log(WOTBMOD_LOG_INFO, "PASS", test);
}

static void Fail(const char* test) {
    InterlockedIncrement(&g_failures);
    Log(WOTBMOD_LOG_ERROR, "FAIL", test);
}

static bool ExpectResult(
    const char* test,
    WotbModResult actual,
    WotbModResult expected = WOTBMOD_OK) {
    if (actual == expected) {
        Pass(test);
        return true;
    }
    char detail[192] = {};
    _snprintf_s(
        detail,
        sizeof(detail),
        _TRUNCATE,
        "%s:actual=%d:expected=%d",
        test,
        static_cast<int>(actual),
        static_cast<int>(expected));
    Fail(detail);
    return false;
}

static const char* EventName(uint32_t type) {
    switch (type) {
    case WOTBMOD_EVENT_UI_INPUT:
        return "ui-input";
    case WOTBMOD_EVENT_SHELL_HIT:
        return "shell-hit";
    case WOTBMOD_EVENT_AMMO_CHANGED:
        return "ammo-changed";
    case WOTBMOD_EVENT_AIM_TARGET_CHANGED:
        return "aim-target-changed";
    case WOTBMOD_EVENT_VEHICLE_SPOTTED:
        return "vehicle-spotted";
    case WOTBMOD_EVENT_VEHICLE_UNSPOTTED:
        return "vehicle-unspotted";
    case WOTBMOD_EVENT_CAMERA_MODE_CHANGED:
        return "camera-mode-changed";
    default:
        return "unknown";
    }
}

static bool IsFinite(float value) {
    return _finite(value) != 0;
}

static bool IsFinite(const WotbModVec3& value) {
    return IsFinite(value.x) &&
           IsFinite(value.y) &&
           IsFinite(value.z);
}

static bool HasPayload(
    const WotbModClientEvent* event,
    uint32_t requiredSize) {
    return event && event->payload_size >= requiredSize;
}

static bool ValidatePayload(const WotbModClientEvent* event) {
    if (!event) return false;
    switch (event->type) {
    case WOTBMOD_EVENT_UI_INPUT:
        return HasPayload(
                   event,
                   static_cast<uint32_t>(
                       sizeof(WotbModUiInputEventData))) &&
               IsFinite(event->payload.ui_input.screen_x) &&
               IsFinite(event->payload.ui_input.screen_y) &&
               IsFinite(event->payload.ui_input.local_x) &&
               IsFinite(event->payload.ui_input.local_y);

    case WOTBMOD_EVENT_SHELL_HIT:
        return HasPayload(
                   event,
                   static_cast<uint32_t>(
                       sizeof(WotbModHitEventData))) &&
               IsFinite(event->payload.hit.position) &&
               IsFinite(event->payload.hit.normal);

    case WOTBMOD_EVENT_AMMO_CHANGED:
        return HasPayload(
            event,
            static_cast<uint32_t>(
                sizeof(WotbModAmmoEventData)));

    case WOTBMOD_EVENT_AIM_TARGET_CHANGED:
    case WOTBMOD_EVENT_VEHICLE_SPOTTED:
    case WOTBMOD_EVENT_VEHICLE_UNSPOTTED:
        return HasPayload(
            event,
            static_cast<uint32_t>(
                sizeof(WotbModVehicleEventData)));

    case WOTBMOD_EVENT_CAMERA_MODE_CHANGED:
        return HasPayload(
                   event,
                   static_cast<uint32_t>(
                       sizeof(WotbModCameraEventData))) &&
               event->payload.camera.mode != 0u &&
               event->payload.camera.native_mode >= 0;

    default:
        return false;
    }
}

static void ValidateBorrowedVehicle(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event) {
    if (g_borrowedVehicleChecked || !event || !event->vehicle) {
        return;
    }
    g_borrowedVehicleChecked = true;

    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    if (host->vehicle_get_info(
            mod,
            event->vehicle,
            &info) == WOTBMOD_OK &&
        info.entity_id != 0) {
        Pass("borrowed-vehicle:get-info");
    } else {
        Fail("borrowed-vehicle:get-info");
    }

    ExpectResult(
        "borrowed-vehicle:release-denied",
        host->vehicle_release(mod, event->vehicle),
        WOTBMOD_ERROR_ACCESS_DENIED);

    WotbModVehicleHandle clone = nullptr;
    if (ExpectResult(
            "borrowed-vehicle:clone",
            host->vehicle_clone(
                mod,
                event->vehicle,
                &clone))) {
        ExpectResult(
            "borrowed-vehicle:clone-release",
            host->vehicle_release(mod, clone));
    }
}

static void FinishIfComplete() {
    const uint32_t seen = static_cast<uint32_t>(
        InterlockedCompareExchange(&g_seenMask, 0, 0));
    if ((seen & kExpectedEventMask) != kExpectedEventMask ||
        InterlockedCompareExchange(&g_finished, 0, 0) != 0) {
        return;
    }

    InterlockedExchange(&g_finished, 1);
    char summary[192] = {};
    _snprintf_s(
        summary,
        sizeof(summary),
        _TRUNCATE,
        "mask=0x%08X passes=%ld failures=%ld",
        seen,
        InterlockedCompareExchange(&g_passes, 0, 0),
        InterlockedCompareExchange(&g_failures, 0, 0));
    Log(
        InterlockedCompareExchange(&g_failures, 0, 0) == 0
            ? WOTBMOD_LOG_INFO
            : WOTBMOD_LOG_ERROR,
        InterlockedCompareExchange(&g_failures, 0, 0) == 0
            ? "SUMMARY"
            : "SUMMARY-FAILED",
        summary);
}

static void WOTBMOD_CALL OnClientEvent(
    const WotbModHostApi* host,
    WotbModHandle mod,
    const WotbModClientEvent* event,
    void*) {
    if (!host || mod != g_mod || !event ||
        event->struct_size < sizeof(WotbModClientEvent)) {
        Fail("callback-contract");
        return;
    }
    if ((event->type & kExpectedEventMask) == 0 ||
        (event->type & (event->type - 1u)) != 0) {
        Fail("unexpected-event-type");
        return;
    }

    if (!g_threadChecked) {
        g_threadChecked = true;
        if (GetCurrentThreadId() == g_enableThreadId) {
            Pass("dispatch-thread");
        } else {
            Fail("dispatch-thread");
        }
    }

    if (event->sequence == 0 ||
        (g_lastSequence != 0 &&
         event->sequence <= g_lastSequence)) {
        Fail("sequence-monotonic");
    }
    g_lastSequence = event->sequence;

    const bool first =
        (InterlockedOr(
             &g_seenMask,
             static_cast<LONG>(event->type)) &
         static_cast<LONG>(event->type)) == 0;
    if (!ValidatePayload(event)) {
        char test[128] = {};
        _snprintf_s(
            test,
            sizeof(test),
            _TRUNCATE,
            "%s:payload",
            EventName(event->type));
        Fail(test);
        return;
    }

    ValidateBorrowedVehicle(host, mod, event);
    if (first) {
        char test[128] = {};
        _snprintf_s(
            test,
            sizeof(test),
            _TRUNCATE,
            "%s:payload",
            EventName(event->type));
        Pass(test);
    }
    FinishIfComplete();
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    g_host = host;
    g_mod = mod;
    g_subscription = 0;
    g_enableThreadId = GetCurrentThreadId();
    g_lastSequence = 0;
    g_threadChecked = false;
    g_borrowedVehicleChecked = false;
    InterlockedExchange(&g_passes, 0);
    InterlockedExchange(&g_failures, 0);
    InterlockedExchange(&g_seenMask, 0);
    InterlockedExchange(&g_finished, 0);

    Log(WOTBMOD_LOG_INFO, "BEGIN", "ABI-2.9");
    if (!WOTBMOD_HOST_HAS(host, event_subscribe) ||
        !WOTBMOD_HOST_HAS(host, event_unsubscribe) ||
        !WOTBMOD_HOST_HAS(host, vehicle_get_info) ||
        !WOTBMOD_HOST_HAS(host, vehicle_clone) ||
        !WOTBMOD_HOST_HAS(host, vehicle_release)) {
        Fail("host-surface");
        return;
    }
    Pass("host-surface");

    ExpectResult(
        "event-subscribe",
        host->event_subscribe(
            mod,
            kExpectedEventMask,
            &OnClientEvent,
            nullptr,
            &g_subscription));
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi*,
    WotbModHandle) {
    g_subscription = 0;
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi*,
    WotbModHandle) {
}

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModFrameInfo*) {
    FinishIfComplete();
}

} /* namespace */

extern "C" __declspec(dllexport) uint32_t WOTBMOD_CALL
WotbNewGameplayEventsTest_GetPasses(void) {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_passes, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t WOTBMOD_CALL
WotbNewGameplayEventsTest_GetFailures(void) {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_failures, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t WOTBMOD_CALL
WotbNewGameplayEventsTest_GetSeenMask(void) {
    return static_cast<uint32_t>(
        InterlockedCompareExchange(&g_seenMask, 0, 0));
}

extern "C" __declspec(dllexport) uint32_t WOTBMOD_CALL
WotbNewGameplayEventsTest_GetExpectedMask(void) {
    return kExpectedEventMask;
}

extern "C" __declspec(dllexport) int32_t WOTBMOD_CALL
WotbNewGameplayEventsTest_IsFinished(void) {
    return static_cast<int32_t>(
        InterlockedCompareExchange(&g_finished, 0, 0));
}

extern "C" __declspec(dllexport) WotbModResult WOTBMOD_CALL
WotbModLoad(
    const WotbModHostApi* host,
    WotbModHandle mod,
    WotbModInfo* outInfo) {
    if (!host || !mod || !outInfo) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    outInfo->struct_size = sizeof(*outInfo);
    outInfo->abi_version = WOTBMOD_ABI_VERSION_2_9;
    outInfo->id = "test.new-gameplay-events";
    outInfo->name = "New Gameplay Events Test";
    outInfo->version = "1.0.0";
    outInfo->author = "BlitzForge SDK";
    outInfo->description =
        "Validates ABI 2.9 UI input, shell hit, ammo, aim and spotting event ingress";
    outInfo->flags = 0;
    outInfo->on_enable = &OnEnable;
    outInfo->on_disable = &OnDisable;
    outInfo->on_unload = &OnUnload;
    outInfo->on_frame = &OnFrame;
    return WOTBMOD_OK;
}
