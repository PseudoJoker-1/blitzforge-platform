#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>

#include "../include/wotb_mod_runtime.h"
#include "../include/wotb_mod_windows_audio.h"

typedef uint32_t(WOTBMOD_CALL* GetCounterFn)(void);
typedef int32_t(WOTBMOD_CALL* IsFinishedFn)(void);

struct SelfTestObject {
    uint32_t magic;
    WotbModResourceType type;
};

static const uint32_t kSelfTestObjectMagic = 0x544A424Fu; /* OBJT */

static WotbModResult AllocateObject(
    WotbModResourceType type,
    void** outObject) {
    if (!outObject) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *outObject = nullptr;
    SelfTestObject* object = static_cast<SelfTestObject*>(
        HeapAlloc(
            GetProcessHeap(),
            HEAP_ZERO_MEMORY,
            sizeof(SelfTestObject)));
    if (!object) return WOTBMOD_ERROR_LIMIT_REACHED;
    object->magic = kSelfTestObjectMagic;
    object->type = type;
    *outObject = object;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL UiCreate(
    void*,
    const WotbModUiControlGeometry*,
    void** outObject) {
    return AllocateObject(WOTBMOD_RESOURCE_UI_CONTROL, outObject);
}

static WotbModResult WOTBMOD_CALL UiGetActiveScreen(
    void*,
    void** outObject) {
    return AllocateObject(WOTBMOD_RESOURCE_UI_CONTROL, outObject);
}

static WotbModResult WOTBMOD_CALL SceneEntityCreate(
    void*,
    void** outObject) {
    return AllocateObject(WOTBMOD_RESOURCE_SCENE, outObject);
}

static WotbModResult WOTBMOD_CALL SceneGetActive(
    void*,
    void** outObject) {
    return AllocateObject(WOTBMOD_RESOURCE_SCENE, outObject);
}

static WotbModResult WOTBMOD_CALL ResourceRelease(
    void*,
    void* nativeResource) {
    SelfTestObject* object =
        static_cast<SelfTestObject*>(nativeResource);
    if (!object || object->magic != kSelfTestObjectMagic) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    object->magic = 0;
    return HeapFree(GetProcessHeap(), 0, object)
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

static void WOTBMOD_CALL LogSink(
    WotbModLogLevel,
    const char* message,
    void*) {
    printf("[published-test] %s\n", message ? message : "");
}

template <typename T>
static T Export(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(
            stderr,
            "usage: published_selftest_host.exe <environment>\n");
        return 2;
    }

    WotbModWindowsAudioOptions audioOptions = {};
    audioOptions.struct_size = sizeof(audioOptions);
    WotbModWindowsAudioHandle audioHandle = nullptr;
    WotbModRuntimeAudioBackend audio = {};
    WotbModResult result = WotbModWindowsAudio_Create(
        &audioOptions,
        &audioHandle,
        &audio);
    if (result != WOTBMOD_OK) {
        fprintf(stderr, "windows audio create failed: %d\n", result);
        return 1;
    }

    WotbModRuntimeOptions options = {};
    WotbModRuntimeResourceBackend resources = {};
    resources.struct_size = sizeof(resources);
    resources.release = &ResourceRelease;
    resources.ui_create = &UiCreate;
    resources.ui_get_active_screen = &UiGetActiveScreen;
    resources.scene_entity_create = &SceneEntityCreate;
    resources.scene_get_active = &SceneGetActive;
    options.struct_size = sizeof(options);
    options.game_directory = argv[1];
    options.game_module = GetModuleHandleA(nullptr);
    options.log_sink = &LogSink;
    options.resource_backend = &resources;
    options.audio_backend = &audio;
    result = WotbModRuntime_Initialize(&options);
    if (result != WOTBMOD_OK) {
        fprintf(stderr, "runtime initialize failed: %d\n", result);
        WotbModWindowsAudio_Destroy(audioHandle);
        return 1;
    }
    result = WotbModRuntime_LoadAll();
    if (result != WOTBMOD_OK) {
        fprintf(stderr, "runtime load failed: %d\n", result);
        WotbModRuntime_Shutdown();
        WotbModWindowsAudio_Destroy(audioHandle);
        return 1;
    }

    for (uint32_t frame = 0; frame < 12; ++frame) {
        WotbModRuntime_DispatchFrame(
            nullptr,
            nullptr,
            nullptr,
            1280,
            720,
            0.25f);
        Sleep(20);
    }

    HMODULE apiModule = GetModuleHandleA("api_selftest_mod.dll");
    HMODULE audioModule =
        GetModuleHandleA("custom_audio_test_mod.dll");
    const GetCounterFn apiPasses =
        Export<GetCounterFn>(
            apiModule,
            "WotbApiSelfTest_GetPasses");
    const GetCounterFn apiFailures =
        Export<GetCounterFn>(
            apiModule,
            "WotbApiSelfTest_GetFailures");
    const GetCounterFn apiSkips =
        Export<GetCounterFn>(
            apiModule,
            "WotbApiSelfTest_GetSkips");
    const GetCounterFn audioPasses =
        Export<GetCounterFn>(
            audioModule,
            "WotbCustomAudioTest_GetPasses");
    const GetCounterFn audioFailures =
        Export<GetCounterFn>(
            audioModule,
            "WotbCustomAudioTest_GetFailures");
    const IsFinishedFn audioFinished =
        Export<IsFinishedFn>(
            audioModule,
            "WotbCustomAudioTest_IsFinished");

    bool passed =
        apiModule &&
        audioModule &&
        apiPasses &&
        apiFailures &&
        apiSkips &&
        audioPasses &&
        audioFailures &&
        audioFinished &&
        apiPasses() > 0 &&
        apiFailures() == 0 &&
        audioPasses() > 0 &&
        audioFailures() == 0 &&
        audioFinished() == 1;

    if (apiPasses && apiFailures && apiSkips) {
        printf(
            "API SELFTEST: passes=%u skips=%u failures=%u\n",
            apiPasses(),
            apiSkips(),
            apiFailures());
    }
    if (audioPasses && audioFailures && audioFinished) {
        printf(
            "AUDIO SELFTEST: passes=%u failures=%u finished=%d\n",
            audioPasses(),
            audioFailures(),
            audioFinished());
    }

    WotbModRuntime_Shutdown();
    result = WotbModWindowsAudio_Destroy(audioHandle);
    if (result != WOTBMOD_OK) {
        fprintf(stderr, "windows audio destroy failed: %d\n", result);
        passed = false;
    }

    if (!passed) {
        fprintf(stderr, "PUBLISHED SELFTEST FAILED\n");
        return 1;
    }
    printf("PUBLISHED SELFTEST OK\n");
    return 0;
}
