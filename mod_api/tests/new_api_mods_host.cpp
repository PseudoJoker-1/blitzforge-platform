#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/wotb_mod_runtime.h"
#include "../include/wotbmod/camera_v1.h"

namespace {

const uint32_t kVehicleMagic = 0x56454854u; /* VEHT */
volatile LONG g_vehicleTokens = 0;
uint32_t g_failures = 0;

struct VehicleToken {
    uint32_t magic;
    uint32_t entity_id;
};

typedef uint32_t(WOTBMOD_CALL* GetCounterFn)(void);
typedef int32_t(WOTBMOD_CALL* IsFinishedFn)(void);

static void Fail(const char* test) {
    ++g_failures;
    fprintf(stderr, "NEW API MODS FAIL: %s\n", test);
}

static void Pass(const char* test) {
    printf("NEW API MODS PASS: %s\n", test);
}

static bool ExpectResult(
    const char* test,
    WotbModResult actual,
    WotbModResult expected = WOTBMOD_OK) {
    if (actual == expected) {
        Pass(test);
        return true;
    }
    fprintf(
        stderr,
        "NEW API MODS FAIL: %s actual=%d expected=%d\n",
        test,
        static_cast<int>(actual),
        static_cast<int>(expected));
    ++g_failures;
    return false;
}

static void WOTBMOD_CALL LogSink(
    WotbModLogLevel,
    const char* message,
    void*) {
    printf("[new-api-mods] %s\n", message ? message : "");
}

static WotbModResult AllocateVehicle(
    uint32_t entityId,
    void** outToken) {
    if (!entityId || !outToken) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outToken = nullptr;
    VehicleToken* token = static_cast<VehicleToken*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(VehicleToken)));
    if (!token) return WOTBMOD_ERROR_LIMIT_REACHED;
    token->magic = kVehicleMagic;
    token->entity_id = entityId;
    InterlockedIncrement(&g_vehicleTokens);
    *outToken = token;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleGetLocal(
    void*,
    void** outToken) {
    return AllocateVehicle(101, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleGetByEntityId(
    void*,
    uint32_t entityId,
    void** outToken) {
    return AllocateVehicle(entityId, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleGetCount(
    void*,
    uint32_t* outCount) {
    if (!outCount) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outCount = 2;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleGetAt(
    void*,
    uint32_t index,
    void** outToken) {
    if (index >= 2) return WOTBMOD_ERROR_NOT_FOUND;
    return AllocateVehicle(index == 0 ? 101 : 202, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleClone(
    void*,
    void* sourceToken,
    void** outToken) {
    VehicleToken* source =
        static_cast<VehicleToken*>(sourceToken);
    if (!source || source->magic != kVehicleMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    return AllocateVehicle(source->entity_id, outToken);
}

static WotbModResult WOTBMOD_CALL VehicleRelease(
    void*,
    void* nativeToken) {
    VehicleToken* token =
        static_cast<VehicleToken*>(nativeToken);
    if (!token || token->magic != kVehicleMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    token->magic = 0;
    if (!HeapFree(GetProcessHeap(), 0, token)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedDecrement(&g_vehicleTokens);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL VehicleGetInfo(
    void*,
    void* nativeToken,
    WotbModVehicleInfo* outInfo) {
    VehicleToken* token =
        static_cast<VehicleToken*>(nativeToken);
    if (!token || token->magic != kVehicleMagic ||
        !outInfo ||
        outInfo->struct_size <
            offsetof(WotbModVehicleInfo, vehicle_name) +
                sizeof(outInfo->vehicle_name)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    WotbModVehicleInfo info = {};
    info.struct_size = sizeof(info);
    info.entity_id = token->entity_id;
    info.team = token->entity_id == 101 ? 1u : 2u;
    info.flags = WOTBMOD_VEHICLE_ALIVE;
    if (token->entity_id == 101) {
        info.flags |= WOTBMOD_VEHICLE_LOCAL;
    }
    info.health = 1000;
    info.max_health = 1000;
    strcpy_s(info.player_name, "NewApiTest");
    strcpy_s(
        info.vehicle_name,
        token->entity_id == 101 ? "T-34" : "TargetTank");
    memcpy(
        outInfo,
        &info,
        outInfo->struct_size < sizeof(info)
            ? outInfo->struct_size
            : sizeof(info));
    return WOTBMOD_OK;
}

template <typename T>
static T Export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(
        module ? GetProcAddress(module, name) : nullptr);
}

static bool ResolveContains(
    const char* test,
    const char* stockPath,
    const char* expectedFragment) {
    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t size = sizeof(resolved);
    const WotbModResult result =
        WotbModRuntime_ResolveVehicleSkinPath(
            stockPath,
            resolved,
            &size);
    if (result == WOTBMOD_OK &&
        strstr(resolved, expectedFragment) != nullptr &&
        GetFileAttributesA(resolved) != INVALID_FILE_ATTRIBUTES) {
        Pass(test);
        return true;
    }
    fprintf(
        stderr,
        "NEW API MODS FAIL: %s result=%d path=%s\n",
        test,
        static_cast<int>(result),
        resolved);
    ++g_failures;
    return false;
}

static bool ResolveEquals(
    const char* test,
    const char* stockPath,
    const char* expectedPath) {
    char resolved[WOTBMOD_MAX_RESOURCE_PATH] = {};
    uint32_t size = sizeof(resolved);
    const WotbModResult result =
        WotbModRuntime_ResolveVehicleSkinPath(
            stockPath,
            resolved,
            &size);
    if (result == WOTBMOD_OK &&
        _stricmp(resolved, expectedPath) == 0) {
        Pass(test);
        return true;
    }
    fprintf(
        stderr,
        "NEW API MODS FAIL: %s result=%d path=%s expected=%s\n",
        test,
        static_cast<int>(result),
        resolved,
        expectedPath);
    ++g_failures;
    return false;
}

static void SendEvent(
    const char* test,
    const WotbModRuntimeClientEvent& event) {
    ExpectResult(
        test,
        WotbModRuntime_NotifyClientEvent(&event));
}

static void SendNewEvents() {
    WotbModRuntimeClientEvent event = {};
    event.struct_size = sizeof(event);

    event.type = WOTBMOD_EVENT_UI_INPUT;
    event.payload_size = sizeof(WotbModUiInputEventData);
    event.payload.ui_input.action = 1;
    event.payload.ui_input.buttons = 1;
    event.payload.ui_input.pointer_id = 7;
    event.payload.ui_input.screen_x = 640.0f;
    event.payload.ui_input.screen_y = 360.0f;
    event.payload.ui_input.local_x = 32.0f;
    event.payload.ui_input.local_y = 16.0f;
    SendEvent("notify-ui-input", event);

    event = {};
    event.struct_size = sizeof(event);
    event.type = WOTBMOD_EVENT_SHELL_HIT;
    event.primary_entity_id = 101;
    event.other_entity_id = 202;
    event.payload_size = sizeof(WotbModHitEventData);
    event.payload.hit.shot_id = 55;
    event.payload.hit.shell_id = 77;
    event.payload.hit.flags = 3;
    event.payload.hit.position = {1.0f, 2.0f, 3.0f};
    event.payload.hit.normal = {0.0f, 1.0f, 0.0f};
    SendEvent("notify-shell-hit", event);

    event = {};
    event.struct_size = sizeof(event);
    event.type = WOTBMOD_EVENT_AMMO_CHANGED;
    event.primary_entity_id = 101;
    event.payload_size = sizeof(WotbModAmmoEventData);
    event.payload.ammo.previous_shell_id = 10;
    event.payload.ammo.shell_id = 11;
    event.payload.ammo.count = 15;
    SendEvent("notify-ammo-changed", event);

    event = {};
    event.struct_size = sizeof(event);
    event.type = WOTBMOD_EVENT_AIM_TARGET_CHANGED;
    event.primary_entity_id = 101;
    event.other_entity_id = 202;
    event.payload_size = sizeof(WotbModVehicleEventData);
    event.payload.vehicle.entity_id = 101;
    event.payload.vehicle.other_entity_id = 202;
    SendEvent("notify-aim-target", event);

    event = {};
    event.struct_size = sizeof(event);
    event.type = WOTBMOD_EVENT_VEHICLE_SPOTTED;
    event.primary_entity_id = 202;
    event.payload_size = sizeof(WotbModVehicleEventData);
    event.payload.vehicle.entity_id = 202;
    event.payload.vehicle.flags = WOTBMOD_VEHICLE_ALIVE;
    SendEvent("notify-vehicle-spotted", event);

    event = {};
    event.struct_size = sizeof(event);
    event.type = WOTBMOD_EVENT_VEHICLE_UNSPOTTED;
    event.primary_entity_id = 202;
    event.payload_size = sizeof(WotbModVehicleEventData);
    event.payload.vehicle.entity_id = 202;
    event.payload.vehicle.flags = WOTBMOD_VEHICLE_ALIVE;
    SendEvent("notify-vehicle-unspotted", event);

    event = {};
    event.struct_size = sizeof(event);
    event.type = WOTBMOD_EVENT_CAMERA_MODE_CHANGED;
    event.payload_size = sizeof(WotbModCameraEventData);
    event.payload.camera.previous_mode =
        WOTBMOD_V3_CAMERA_MODE_ARCADE;
    event.payload.camera.mode = WOTBMOD_V3_CAMERA_MODE_SNIPER;
    event.payload.camera.native_mode = 1;
    SendEvent("notify-camera-mode", event);
}

} /* namespace */

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(
            stderr,
            "usage: new_api_mods_host.exe <environment>\n");
        return 2;
    }

    WotbModRuntimeGameplayBackend gameplay = {};
    gameplay.struct_size = sizeof(gameplay);
    gameplay.vehicle_get_local = &VehicleGetLocal;
    gameplay.vehicle_get_by_entity_id =
        &VehicleGetByEntityId;
    gameplay.vehicle_get_count = &VehicleGetCount;
    gameplay.vehicle_get_at = &VehicleGetAt;
    gameplay.vehicle_clone = &VehicleClone;
    gameplay.vehicle_release = &VehicleRelease;
    gameplay.vehicle_get_info = &VehicleGetInfo;

    WotbModRuntimeOptions options = {};
    options.struct_size = sizeof(options);
    options.game_directory = argv[1];
    options.game_module = GetModuleHandleA(nullptr);
    options.log_sink = &LogSink;
    options.gameplay_backend = &gameplay;

    WotbModResult result =
        WotbModRuntime_Initialize(&options);
    if (result != WOTBMOD_OK) {
        fprintf(
            stderr,
            "new API runtime initialize failed: %d\n",
            static_cast<int>(result));
        return 1;
    }
    result = WotbModRuntime_LoadAll();
    if (result != WOTBMOD_OK) {
        fprintf(
            stderr,
            "new API runtime load failed: %d\n",
            static_cast<int>(result));
        WotbModRuntime_Shutdown();
        return 1;
    }

    HMODULE eventModule =
        GetModuleHandleA("new_gameplay_events_test_mod.dll");
    HMODULE skinModule =
        GetModuleHandleA("vehicle_skin_test_mod.dll");
    HMODULE object260Module =
        GetModuleHandleA("object260_t3485_model_mod.dll");
    const GetCounterFn eventPasses =
        Export<GetCounterFn>(
            eventModule,
            "WotbNewGameplayEventsTest_GetPasses");
    const GetCounterFn eventFailures =
        Export<GetCounterFn>(
            eventModule,
            "WotbNewGameplayEventsTest_GetFailures");
    const GetCounterFn eventSeen =
        Export<GetCounterFn>(
            eventModule,
            "WotbNewGameplayEventsTest_GetSeenMask");
    const GetCounterFn eventExpected =
        Export<GetCounterFn>(
            eventModule,
            "WotbNewGameplayEventsTest_GetExpectedMask");
    const IsFinishedFn eventFinished =
        Export<IsFinishedFn>(
            eventModule,
            "WotbNewGameplayEventsTest_IsFinished");
    const GetCounterFn skinPasses =
        Export<GetCounterFn>(
            skinModule,
            "WotbVehicleSkinTest_GetPasses");
    const GetCounterFn skinFailures =
        Export<GetCounterFn>(
            skinModule,
            "WotbVehicleSkinTest_GetFailures");
    const IsFinishedFn skinFinished =
        Export<IsFinishedFn>(
            skinModule,
            "WotbVehicleSkinTest_IsFinished");
    const IsFinishedFn object260Registered =
        Export<IsFinishedFn>(
            object260Module,
            "WotbObject260T3485_IsRegistered");
    const GetCounterFn object260Failures =
        Export<GetCounterFn>(
            object260Module,
            "WotbObject260T3485_GetFailures");

    if (!eventModule || !skinModule || !object260Module ||
        !eventPasses || !eventFailures ||
        !eventSeen || !eventExpected || !eventFinished ||
        !skinPasses || !skinFailures || !skinFinished ||
        !object260Registered || !object260Failures) {
        Fail("test-mod-exports");
    } else {
        Pass("test-mod-exports");
    }

    ResolveContains(
        "skin-resolve-mesh-dvpl",
        "Data/3d/Tanks/USSR/T-34-85.sc2.dvpl",
        "high\\T-34-85.sc2");
    ResolveContains(
        "skin-resolve-material",
        "~res:/3d/Tanks/USSR/T-34-85.material.yaml",
        "high\\T-34-85.material.yaml");
    ResolveContains(
        "skin-resolve-texture",
        "~res:/3d/Tanks/USSR/images/T-34-85.dx11.dds",
        "high\\T-34-85.dx11.dds");
    ResolveEquals(
        "object260-resolve-sc2-stock-alias",
        "Data/3d/Tanks/USSR/R110_Object_260.sc2.dvpl",
        "~res:/3d/Tanks/USSR/T-34-85.sc2");
    ResolveEquals(
        "object260-resolve-scg-stock-alias",
        "~res:/3d/Tanks/USSR/R110_Object_260.scg",
        "~res:/3d/Tanks/USSR/T-34-85.scg");
    if (!object260Registered ||
        !object260Failures ||
        object260Registered() != 1 ||
        object260Failures() != 0) {
        Fail("object260-model-mod-result");
    } else {
        Pass("object260-model-mod-result");
    }

    SendNewEvents();
    WotbModRuntime_DispatchFrame(
        nullptr,
        nullptr,
        nullptr,
        1280,
        720,
        1.0 / 60.0);

    if (eventPasses && eventFailures &&
        eventSeen && eventExpected && eventFinished) {
        printf(
            "NEW EVENTS MOD: passes=%u failures=%u seen=0x%08X expected=0x%08X finished=%d\n",
            eventPasses(),
            eventFailures(),
            eventSeen(),
            eventExpected(),
            eventFinished());
        if (eventFailures() != 0 ||
            eventSeen() != eventExpected() ||
            eventFinished() != 1) {
            Fail("new-events-result");
        } else {
            Pass("new-events-result");
        }
    }

    if (skinPasses && skinFailures && skinFinished) {
        printf(
            "VEHICLE SKIN MOD: passes=%u failures=%u finished=%d\n",
            skinPasses(),
            skinFailures(),
            skinFinished());
        if (skinFailures() != 0 ||
            skinFinished() != 1) {
            Fail("vehicle-skin-result");
        } else {
            Pass("vehicle-skin-result");
        }
    }

    WotbModRuntime_Shutdown();
    if (InterlockedCompareExchange(
            &g_vehicleTokens,
            0,
            0) != 0) {
        Fail("vehicle-token-balance");
    } else {
        Pass("vehicle-token-balance");
    }

    if (g_failures != 0) {
        fprintf(
            stderr,
            "NEW API MODS FAILED: failures=%u\n",
            g_failures);
        return 1;
    }
    printf("NEW API MODS OK\n");
    return 0;
}
