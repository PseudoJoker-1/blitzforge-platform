#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmreg.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../include/wotb_mod_dava_sound.h"
#include "../include/wotb_mod_windows_audio.h"
#include "../loader/v3_native_client_services.h"

namespace {

int g_passes = 0;
int g_failures = 0;

void Check(bool condition, const char* name) {
    if (condition) {
        ++g_passes;
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

bool Near(double actual, double expected, double tolerance = 0.01) {
    return std::fabs(actual - expected) <= tolerance;
}

uint32_t RvaOf(const void* address) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(
        GetModuleHandleA(nullptr));
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(address) - base);
}

/* ---------------- DAVA vtable bridge fixture ---------------- */

struct FakeDavaObject {
    void** vtable;
};

void* g_fakeSystemVtable[4] = {};
void* g_fakeEventVtable[18] = {};
FakeDavaObject g_fakeSystem = {};
FakeDavaObject g_fakeEvent = {};
void* g_fakeSystemSingleton = nullptr;
int32_t g_fakeSoundGroup = -1;
int32_t g_davaStopForce = -1;
float g_davaSpeed = 0.0f;
float g_davaDirection[3] = {};
float g_davaVelocity[3] = {};
int32_t g_davaLoopCount = 0;
int32_t g_davaPriority = 0;

struct FakeDavaMethods {
    void* FastNameCtor(const char* name) {
        *reinterpret_cast<const char**>(this) = name;
        return this;
    }

    void Release() {}

    void* CreateEvent(const void*, const int32_t*) {
        return &g_fakeEvent;
    }

    int IsActive() {
        return 1;
    }

    void Void() {}

    void Stop(bool force) {
        g_davaStopForce = force ? 1 : 0;
    }

    void Bool(bool) {}

    void Volume(float) {}

    void Speed(float speed) {
        g_davaSpeed = speed;
    }

    void Direction(const float* value) {
        std::memcpy(
            g_davaDirection, value, sizeof(g_davaDirection));
    }

    void Position(const float*) {}

    void Velocity(const float* value) {
        std::memcpy(
            g_davaVelocity, value, sizeof(g_davaVelocity));
    }

    void LoopCount(int32_t value) {
        g_davaLoopCount = value;
    }

    void Priority(int32_t value) {
        g_davaPriority = value;
    }

    void SetParameter(const void*, float) {}

    float GetParameter(const void*) {
        return 1.0f;
    }

    int HasParameter(const void*) {
        return 1;
    }
};

template <typename T>
void* MemberAddress(T method) {
    static_assert(
        sizeof(method) == sizeof(void*),
        "x86 single-inheritance member pointer required");
    void* address = nullptr;
    std::memcpy(&address, &method, sizeof(address));
    return address;
}

void InitializeFakeDavaObjects() {
    std::memset(g_fakeSystemVtable, 0, sizeof(g_fakeSystemVtable));
    std::memset(g_fakeEventVtable, 0, sizeof(g_fakeEventVtable));
    g_fakeSystemVtable[3] =
        MemberAddress(&FakeDavaMethods::CreateEvent);
    g_fakeEventVtable[4] =
        MemberAddress(&FakeDavaMethods::IsActive);
    g_fakeEventVtable[5] =
        MemberAddress(&FakeDavaMethods::Void);
    g_fakeEventVtable[6] =
        MemberAddress(&FakeDavaMethods::Stop);
    g_fakeEventVtable[7] =
        MemberAddress(&FakeDavaMethods::Bool);
    g_fakeEventVtable[8] =
        MemberAddress(&FakeDavaMethods::Volume);
    g_fakeEventVtable[9] =
        MemberAddress(&FakeDavaMethods::Speed);
    g_fakeEventVtable[10] =
        MemberAddress(&FakeDavaMethods::Direction);
    g_fakeEventVtable[11] =
        MemberAddress(&FakeDavaMethods::Position);
    g_fakeEventVtable[12] =
        MemberAddress(&FakeDavaMethods::Velocity);
    g_fakeEventVtable[13] =
        MemberAddress(&FakeDavaMethods::LoopCount);
    g_fakeEventVtable[14] =
        MemberAddress(&FakeDavaMethods::Priority);
    g_fakeEventVtable[15] =
        MemberAddress(&FakeDavaMethods::SetParameter);
    g_fakeEventVtable[16] =
        MemberAddress(&FakeDavaMethods::GetParameter);
    g_fakeEventVtable[17] =
        MemberAddress(&FakeDavaMethods::HasParameter);
    g_fakeSystem.vtable = g_fakeSystemVtable;
    g_fakeEvent.vtable = g_fakeEventVtable;
    g_fakeSystemSingleton = &g_fakeSystem;
}

void TestDavaExtensions() {
    InitializeFakeDavaObjects();
    WotbModDavaSoundOptions options = {};
    options.struct_size = sizeof(options);
    options.game_module = GetModuleHandleA(nullptr);
    options.sound_system_singleton_rva =
        RvaOf(&g_fakeSystemSingleton);
    options.fast_name_ctor_rva =
        RvaOf(MemberAddress(&FakeDavaMethods::FastNameCtor));
    options.ref_counted_release_rva =
        RvaOf(MemberAddress(&FakeDavaMethods::Release));
    options.default_sound_group_rva = RvaOf(&g_fakeSoundGroup);
    options.sound_system_vtable_rva = RvaOf(g_fakeSystemVtable);
    options.sound_system_proxy_vtable_rva =
        RvaOf(g_fakeSystemVtable);
    options.hybrid_event_vtable_rva = RvaOf(g_fakeEventVtable);
    options.wwise_event_vtable_rva = RvaOf(g_fakeEventVtable);

    WotbModDavaSoundHandle handle = nullptr;
    WotbModRuntimeSoundBackend backend = {};
    Check(
        WotbModDavaSound_Create(
            &options, &handle, &backend) == WOTBMOD_OK,
        "DAVA bridge create");
    Check(
        backend.stop_with_force && backend.set_speed &&
            backend.set_direction && backend.set_velocity &&
            backend.set_loop_count && backend.set_priority,
        "DAVA extension callbacks exported");

    void* event = nullptr;
    Check(
        backend.create(
            backend.user_data, "fixture/event", &event) == WOTBMOD_OK &&
            event != nullptr,
        "DAVA fixture event create");
    Check(
        backend.stop_with_force(
            backend.user_data, event, 0) == WOTBMOD_OK &&
            g_davaStopForce == 0,
        "DAVA stop force false");
    Check(
        backend.stop_with_force(
            backend.user_data, event, 1) == WOTBMOD_OK &&
            g_davaStopForce == 1,
        "DAVA stop force true");
    Check(
        backend.stop_with_force(
            backend.user_data, event, 2) ==
            WOTBMOD_ERROR_INVALID_ARGUMENT,
        "DAVA stop rejects invalid force");
    g_davaStopForce = -1;
    Check(
        backend.stop(backend.user_data, event) == WOTBMOD_OK &&
            g_davaStopForce == 1,
        "DAVA legacy stop remains forced");
    Check(
        backend.set_speed(
            backend.user_data, event, 1.75f) == WOTBMOD_OK &&
            Near(g_davaSpeed, 1.75),
        "DAVA speed slot 9");
    Check(
        backend.set_direction(
            backend.user_data, event, 1.0f, 2.0f, 3.0f) ==
                WOTBMOD_OK &&
            Near(g_davaDirection[0], 1.0) &&
            Near(g_davaDirection[1], 2.0) &&
            Near(g_davaDirection[2], 3.0),
        "DAVA direction slot 10");
    Check(
        backend.set_velocity(
            backend.user_data, event, 4.0f, 5.0f, 6.0f) ==
                WOTBMOD_OK &&
            Near(g_davaVelocity[0], 4.0) &&
            Near(g_davaVelocity[1], 5.0) &&
            Near(g_davaVelocity[2], 6.0),
        "DAVA velocity slot 12");
    Check(
        backend.set_loop_count(
            backend.user_data, event, -1) == WOTBMOD_OK &&
            g_davaLoopCount == -1,
        "DAVA loop-count slot 13");
    Check(
        backend.set_priority(
            backend.user_data, event, -42) == WOTBMOD_OK &&
            g_davaPriority == -42,
        "DAVA priority slot 14");
    Check(
        backend.release(backend.user_data, event) == WOTBMOD_OK,
        "DAVA fixture event release");
    Check(
        WotbModDavaSound_Destroy(handle) == WOTBMOD_OK,
        "DAVA bridge destroy");
}

/* ---------------- Native V3 dispatcher fixture ---------------- */

struct FakeAudioClip {};
struct FakeAudioPlayback {
    WotbModAudioState state;
    double position;
};
struct FakeSoundEvent {};

FakeAudioClip g_dispatchClip;
FakeAudioPlayback g_dispatchPlayback = {};
FakeSoundEvent g_dispatchSound;
int32_t g_dispatchForce = -1;
int32_t g_dispatchLegacyStops = 0;
float g_dispatchSpeed = 0.0f;
float g_dispatchDirection[3] = {};
float g_dispatchVelocity[3] = {};
int32_t g_dispatchLoopCount = 0;
int32_t g_dispatchPriority = 0;

WotbModResult WOTBMOD_CALL DispatchLoadClip(
    void*,
    const char*,
    void** output) {
    *output = &g_dispatchClip;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchClipOperation(void*, void*) {
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchPlay(
    void*,
    void*,
    const WotbModAudioPlayInfo*,
    void** output) {
    g_dispatchPlayback.state = WOTBMOD_AUDIO_PLAYING;
    g_dispatchPlayback.position = 0.0;
    *output = &g_dispatchPlayback;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchPlaybackOperation(
    void*,
    void* playback) {
    static_cast<FakeAudioPlayback*>(playback)->state =
        WOTBMOD_AUDIO_STOPPED;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchSetParameters(
    void*,
    void*,
    const WotbModAudioPlayInfo*) {
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchGetState(
    void*,
    void* playback,
    WotbModAudioState* output) {
    *output = static_cast<FakeAudioPlayback*>(playback)->state;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchSeek(
    void*,
    void* playback,
    double seconds) {
    static_cast<FakeAudioPlayback*>(playback)->position = seconds;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchGetPosition(
    void*,
    void* playback,
    double* output) {
    *output = static_cast<FakeAudioPlayback*>(playback)->position;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchGetDuration(
    void*,
    void* clip,
    double* output) {
    if (clip != &g_dispatchClip) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    *output = 3.25;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchCreateSound(
    void*,
    const char*,
    void** output) {
    *output = &g_dispatchSound;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchSoundOperation(void*, void*) {
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchLegacyStop(void*, void*) {
    ++g_dispatchLegacyStops;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchStopWithForce(
    void*,
    void*,
    int32_t force) {
    g_dispatchForce = force;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchPaused(void*, void*, int32_t) {
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchScalar(
    void*,
    void*,
    float value) {
    g_dispatchSpeed = value;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchDirection(
    void*,
    void*,
    float x,
    float y,
    float z) {
    g_dispatchDirection[0] = x;
    g_dispatchDirection[1] = y;
    g_dispatchDirection[2] = z;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchVelocity(
    void*,
    void*,
    float x,
    float y,
    float z) {
    g_dispatchVelocity[0] = x;
    g_dispatchVelocity[1] = y;
    g_dispatchVelocity[2] = z;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchLoopCount(
    void*,
    void*,
    int32_t value) {
    g_dispatchLoopCount = value;
    return WOTBMOD_OK;
}

WotbModResult WOTBMOD_CALL DispatchPriority(
    void*,
    void*,
    int32_t value) {
    g_dispatchPriority = value;
    return WOTBMOD_OK;
}

wotbmod::v3::ClientHostObjectRequest MakeRequest() {
    wotbmod::v3::ClientHostObjectRequest request = {};
    request.struct_size = sizeof(request);
    request.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return request;
}

wotbmod::v3::ClientHostObjectResponse MakeResponse() {
    wotbmod::v3::ClientHostObjectResponse response = {};
    response.struct_size = sizeof(response);
    response.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    return response;
}

WotbModV3Result Invoke(
    WotbModV3Handle mod,
    const char* operation,
    const wotbmod::v3::ClientHostObjectRequest& request,
    wotbmod::v3::ClientHostObjectResponse* response = nullptr) {
    return wotbmod::loader::InvokeV3NativeClientServices(
        mod,
        operation,
        &request,
        sizeof(request),
        response,
        response ? sizeof(*response) : 0u);
}

WotbModRuntimeAudioBackend MakeDispatchAudioBackend() {
    WotbModRuntimeAudioBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.load_clip = &DispatchLoadClip;
    backend.reload_clip = &DispatchClipOperation;
    backend.release_clip = &DispatchClipOperation;
    backend.play = &DispatchPlay;
    backend.pause = &DispatchPlaybackOperation;
    backend.resume = &DispatchPlaybackOperation;
    backend.stop = &DispatchPlaybackOperation;
    backend.set_parameters = &DispatchSetParameters;
    backend.get_state = &DispatchGetState;
    backend.release = &DispatchPlaybackOperation;
    backend.seek = &DispatchSeek;
    backend.get_position = &DispatchGetPosition;
    backend.get_duration = &DispatchGetDuration;
    return backend;
}

WotbModRuntimeSoundBackend MakeDispatchSoundBackend() {
    WotbModRuntimeSoundBackend backend = {};
    backend.struct_size = sizeof(backend);
    backend.create = &DispatchCreateSound;
    backend.trigger = &DispatchSoundOperation;
    backend.stop = &DispatchLegacyStop;
    backend.set_paused = &DispatchPaused;
    backend.set_volume = &DispatchScalar;
    backend.set_position = &DispatchDirection;
    backend.release = &DispatchSoundOperation;
    backend.stop_with_force = &DispatchStopWithForce;
    backend.set_speed = &DispatchScalar;
    backend.set_direction = &DispatchDirection;
    backend.set_velocity = &DispatchVelocity;
    backend.set_loop_count = &DispatchLoopCount;
    backend.set_priority = &DispatchPriority;
    return backend;
}

void TestDispatcherExtensions() {
    WotbModRuntimeAudioBackend audio = MakeDispatchAudioBackend();
    WotbModRuntimeSoundBackend sound = MakeDispatchSoundBackend();
    wotbmod::loader::V3NativeClientServicesOptions options = {};
    options.struct_size = sizeof(options);
    options.audio_backend = &audio;
    options.sound_backend = &sound;
    Check(
        wotbmod::loader::InitializeV3NativeClientServices(
            &options) == WOTBMOD_V3_OK,
        "V3 dispatcher initialize");

    const WotbModV3Handle mod = static_cast<WotbModV3Handle>(101u);
    wotbmod::v3::ClientHostObjectRequest request = MakeRequest();
    request.name = "fixture";
    request.secondary_name = "fixture.wav";
    request.scalar0 = 1.0;
    request.scalar1 = 1.0;
    wotbmod::v3::ClientHostObjectResponse response = MakeResponse();
    Check(
        Invoke(mod, "audio_create", request, &response) ==
                WOTBMOD_V3_OK &&
            response.object != 0u,
        "V3 audio create");
    const uint64_t audioObject = response.object;

    wotbmod::v3::ClientHostAudioSubscriptionRequest subscription = {};
    subscription.struct_size = sizeof(subscription);
    subscription.api_version =
        WOTBMOD_V3_CLIENT_HOST_BACKEND_VERSION;
    subscription.object = audioObject;
    subscription.mod = mod;
    subscription.lifecycle_kind =
        wotbmod::v3::CLIENT_HOST_AUDIO_STARTED;
    response = MakeResponse();
    Check(
        wotbmod::loader::InvokeV3NativeClientServices(
            mod,
            "audio_subscribe_started",
            &subscription,
            sizeof(subscription),
            &response,
            sizeof(response)) == WOTBMOD_V3_E_NOT_SUPPORTED &&
            response.object == 0u,
        "V3 audio lifecycle subscription refuses missing native ingress");
    subscription.lifecycle_kind =
        wotbmod::v3::CLIENT_HOST_AUDIO_FINISHED;
    Check(
        wotbmod::loader::InvokeV3NativeClientServices(
            mod,
            "audio_subscribe_started",
            &subscription,
            sizeof(subscription),
            nullptr,
            0u) == WOTBMOD_V3_E_INVALID_ARGUMENT,
        "V3 audio lifecycle subscription validates its event kind");
    request = MakeRequest();
    request.object = UINT64_C(0x1234);
    Check(
        Invoke(mod, "audio_unsubscribe", request) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "V3 audio unsubscribe refuses an unavailable lifecycle backend");
    request = MakeRequest();
    request.name = "hangar.vehicle_selected";
    request.secondary_name = "fixture.wav";
    Check(
        Invoke(mod, "sound_override_register", request, &response) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "V3 stock sound override refuses a missing interception backend");

    request = MakeRequest();
    request.object = audioObject;
    response = MakeResponse();
    Check(
        Invoke(mod, "audio_get_duration", request, &response) ==
                WOTBMOD_V3_OK &&
            Near(response.value_f64, 3.25),
        "V3 audio duration dispatch");
    response = MakeResponse();
    Check(
        Invoke(mod, "audio_get_position", request, &response) ==
            WOTBMOD_V3_E_NOT_FOUND,
        "V3 position requires playback");
    Check(
        Invoke(mod, "audio_play", request) == WOTBMOD_V3_OK,
        "V3 audio play");
    request.scalar0 = 1.5;
    Check(
        Invoke(mod, "audio_seek", request) == WOTBMOD_V3_OK,
        "V3 audio seek dispatch");
    response = MakeResponse();
    Check(
        Invoke(mod, "audio_get_position", request, &response) ==
                WOTBMOD_V3_OK &&
            Near(response.value_f64, 1.5),
        "V3 audio position dispatch");

    request = MakeRequest();
    request.name = "fixture/event";
    response = MakeResponse();
    Check(
        Invoke(mod, "sound_event_create", request, &response) ==
                WOTBMOD_V3_OK &&
            response.object != 0u,
        "V3 sound event create");
    const uint64_t soundObject = response.object;
    request = MakeRequest();
    request.object = soundObject;
    request.flags = 0u;
    Check(
        Invoke(mod, "sound_event_stop", request) == WOTBMOD_V3_OK &&
            g_dispatchForce == 0,
        "V3 sound stop forwards force false");
    request.flags = 1u;
    Check(
        Invoke(mod, "sound_event_stop", request) == WOTBMOD_V3_OK &&
            g_dispatchForce == 1,
        "V3 sound stop forwards force true");
    request.flags = 2u;
    Check(
        Invoke(mod, "sound_event_stop", request) ==
            WOTBMOD_V3_E_INVALID_ARGUMENT,
        "V3 sound stop rejects invalid force");
    request.flags = 0u;
    request.scalar0 = 2.0;
    Check(
        Invoke(mod, "sound_event_set_speed", request) ==
                WOTBMOD_V3_OK &&
            Near(g_dispatchSpeed, 2.0),
        "V3 sound speed dispatch");
    request.vector = {1.0f, 2.0f, 3.0f, 0.0f};
    Check(
        Invoke(mod, "sound_event_set_direction", request) ==
                WOTBMOD_V3_OK &&
            Near(g_dispatchDirection[2], 3.0),
        "V3 sound direction dispatch");
    request.vector = {4.0f, 5.0f, 6.0f, 0.0f};
    Check(
        Invoke(mod, "sound_event_set_velocity", request) ==
                WOTBMOD_V3_OK &&
            Near(g_dispatchVelocity[0], 4.0),
        "V3 sound velocity dispatch");
    request.signed_value = -1;
    Check(
        Invoke(mod, "sound_event_set_loop_count", request) ==
                WOTBMOD_V3_OK &&
            g_dispatchLoopCount == -1,
        "V3 sound loop-count dispatch");
    request.signed_value = -77;
    Check(
        Invoke(mod, "sound_event_set_priority", request) ==
                WOTBMOD_V3_OK &&
            g_dispatchPriority == -77,
        "V3 sound priority dispatch");
    response = MakeResponse();
    Check(
        Invoke(mod, "sound_event_get_bus", request, &response) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "V3 sound bus remains unsupported");
    Check(
        Invoke(mod, "sound_event_destroy", request) ==
            WOTBMOD_V3_OK,
        "V3 sound event destroy");
    request = MakeRequest();
    request.object = audioObject;
    Check(
        Invoke(mod, "audio_destroy", request) == WOTBMOD_V3_OK,
        "V3 audio destroy");
    wotbmod::loader::ShutdownV3NativeClientServices();

    sound = MakeDispatchSoundBackend();
    sound.struct_size = static_cast<uint32_t>(
        offsetof(WotbModRuntimeSoundBackend, stop_with_force));
    audio = MakeDispatchAudioBackend();
    audio.struct_size = static_cast<uint32_t>(
        offsetof(WotbModRuntimeAudioBackend, seek));
    options.audio_backend = &audio;
    options.sound_backend = &sound;
    Check(
        wotbmod::loader::InitializeV3NativeClientServices(
            &options) == WOTBMOD_V3_OK,
        "V3 short sound backend initialize");
    request = MakeRequest();
    request.name = "legacy-clip";
    request.secondary_name = "legacy.wav";
    request.scalar0 = 1.0;
    request.scalar1 = 1.0;
    response = MakeResponse();
    Check(
        Invoke(mod, "audio_create", request, &response) ==
            WOTBMOD_V3_OK,
        "V3 short audio backend create");
    const uint64_t shortAudioObject = response.object;
    request = MakeRequest();
    request.object = shortAudioObject;
    response = MakeResponse();
    Check(
        Invoke(mod, "audio_get_duration", request, &response) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "V3 short backend gates appended duration");
    request = MakeRequest();
    request.object = shortAudioObject;
    Check(
        Invoke(mod, "audio_destroy", request) == WOTBMOD_V3_OK,
        "V3 short audio backend destroy");
    request = MakeRequest();
    request.name = "legacy/event";
    response = MakeResponse();
    Check(
        Invoke(mod, "sound_event_create", request, &response) ==
            WOTBMOD_V3_OK,
        "V3 legacy sound create");
    request = MakeRequest();
    request.object = response.object;
    request.scalar0 = 1.0;
    Check(
        Invoke(mod, "sound_event_set_speed", request) ==
            WOTBMOD_V3_E_NOT_SUPPORTED,
        "V3 short backend gates appended speed");
    const int32_t stopsBefore = g_dispatchLegacyStops;
    request.flags = 0u;
    Check(
        Invoke(mod, "sound_event_stop", request) == WOTBMOD_V3_OK &&
            g_dispatchLegacyStops == stopsBefore + 1,
        "V3 legacy stop fallback");
    Check(
        Invoke(mod, "sound_event_destroy", request) ==
            WOTBMOD_V3_OK,
        "V3 legacy sound destroy");
    wotbmod::loader::ShutdownV3NativeClientServices();
}

/* ---------------- Windows MF/XAudio2 timing fixture ---------------- */

#pragma pack(push, 1)
struct WaveHeader {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format_tag;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t average_bytes_per_second;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};
#pragma pack(pop)

bool WriteSilentWave(
    const char* path,
    uint32_t sampleRate,
    uint32_t seconds) {
    const uint32_t dataSize =
        sampleRate * seconds * sizeof(int16_t);
    WaveHeader header = {};
    std::memcpy(header.riff, "RIFF", 4u);
    header.riff_size =
        static_cast<uint32_t>(sizeof(header) - 8u + dataSize);
    std::memcpy(header.wave, "WAVE", 4u);
    std::memcpy(header.fmt, "fmt ", 4u);
    header.fmt_size = 16u;
    header.format_tag = WAVE_FORMAT_PCM;
    header.channels = 1u;
    header.sample_rate = sampleRate;
    header.average_bytes_per_second =
        sampleRate * sizeof(int16_t);
    header.block_align = sizeof(int16_t);
    header.bits_per_sample = 16u;
    std::memcpy(header.data, "data", 4u);
    header.data_size = dataSize;
    std::vector<uint8_t> bytes(
        sizeof(header) + dataSize, 0u);
    std::memcpy(bytes.data(), &header, sizeof(header));
    HANDLE file = CreateFileA(
        path,
        GENERIC_WRITE,
        0u,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0u;
    const BOOL ok = WriteFile(
        file,
        bytes.data(),
        static_cast<DWORD>(bytes.size()),
        &written,
        nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

void TestWindowsTiming() {
    const char* path = "build\\audio_extensions_test.wav";
    Check(
        WriteSilentWave(path, 8000u, 2u),
        "Windows timing WAV fixture");

    WotbModWindowsAudioHandle handle = nullptr;
    WotbModRuntimeAudioBackend backend = {};
    WotbModWindowsAudioOptions options = {};
    options.struct_size = sizeof(options);
    const WotbModResult createResult =
        WotbModWindowsAudio_Create(
            &options, &handle, &backend);
    Check(
        createResult == WOTBMOD_OK,
        "Windows timing backend create");
    if (createResult != WOTBMOD_OK) {
        DeleteFileA(path);
        return;
    }
    Check(
        backend.seek && backend.get_position &&
            backend.get_duration,
        "Windows timing callbacks exported");
    void* clip = nullptr;
    Check(
        backend.load_clip(
            backend.user_data, path, &clip) == WOTBMOD_OK &&
            clip != nullptr,
        "Windows timing clip decode");
    double duration = 0.0;
    Check(
        backend.get_duration(
            backend.user_data, clip, &duration) == WOTBMOD_OK &&
            Near(duration, 2.0, 0.001),
        "Windows duration from decoded PCM");

    WotbModAudioPlayInfo info = {};
    info.struct_size = sizeof(info);
    info.flags =
        WOTBMOD_AUDIO_PLAY_LOOP |
        WOTBMOD_AUDIO_PLAY_START_PAUSED;
    info.volume = 1.0f;
    info.pitch = 1.0f;
    info.min_distance = 1.0f;
    info.max_distance = 100.0f;
    void* playback = nullptr;
    Check(
        backend.play(
            backend.user_data,
            clip,
            &info,
            &playback) == WOTBMOD_OK &&
            playback != nullptr,
        "Windows start-paused loop playback");

    WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
    Check(
        backend.seek(
            backend.user_data, playback, 0.5) == WOTBMOD_OK &&
            backend.get_state(
                backend.user_data, playback, &state) == WOTBMOD_OK &&
            state == WOTBMOD_AUDIO_PAUSED,
        "Windows seek preserves paused state");
    double position = 0.0;
    Check(
        backend.get_position(
            backend.user_data, playback, &position) == WOTBMOD_OK &&
            Near(position, 0.5, 0.002),
        "Windows paused seek position");
    Check(
        backend.resume(
            backend.user_data, playback) == WOTBMOD_OK,
        "Windows timing resume");
    Check(
        backend.seek(
            backend.user_data, playback, 0.25) == WOTBMOD_OK &&
            backend.get_state(
                backend.user_data, playback, &state) == WOTBMOD_OK &&
            state == WOTBMOD_AUDIO_PLAYING,
        "Windows seek preserves playing state");
    Sleep(30u);
    Check(
        backend.get_position(
            backend.user_data, playback, &position) == WOTBMOD_OK &&
            position >= 0.25 && position < 1.0,
        "Windows position uses SamplesPlayed plus seek base");
    Check(
        backend.pause(
            backend.user_data, playback) == WOTBMOD_OK &&
            backend.seek(
                backend.user_data, playback, 1.25) == WOTBMOD_OK &&
            backend.get_state(
                backend.user_data, playback, &state) == WOTBMOD_OK &&
            state == WOTBMOD_AUDIO_PAUSED,
        "Windows second paused seek");
    Check(
        backend.stop(
            backend.user_data, playback) == WOTBMOD_OK &&
            backend.seek(
                backend.user_data, playback, 0.75) == WOTBMOD_OK &&
            backend.get_state(
                backend.user_data, playback, &state) == WOTBMOD_OK &&
            state == WOTBMOD_AUDIO_STOPPED,
        "Windows seek preserves stopped state");
    Check(
        backend.get_position(
            backend.user_data, playback, &position) == WOTBMOD_OK &&
            Near(position, 0.75, 0.002),
        "Windows stopped seek position");
    Check(
        backend.seek(
            backend.user_data, playback, -10.0) == WOTBMOD_OK &&
            backend.get_position(
                backend.user_data, playback, &position) == WOTBMOD_OK &&
            Near(position, 0.0, 0.002),
        "Windows seek clamps below zero");
    Check(
        backend.seek(
            backend.user_data, playback, 99.0) == WOTBMOD_OK &&
            backend.get_position(
                backend.user_data, playback, &position) == WOTBMOD_OK &&
            position <= duration && position > 1.99,
        "Windows seek clamps above duration with block alignment");

    Check(
        backend.release(
            backend.user_data, playback) == WOTBMOD_OK,
        "Windows timing playback release");
    Check(
        backend.release_clip(
            backend.user_data, clip) == WOTBMOD_OK,
        "Windows timing clip release");
    Check(
        WotbModWindowsAudio_Destroy(handle) == WOTBMOD_OK,
        "Windows timing backend destroy");
    DeleteFileA(path);
}

}  // namespace

int main() {
    TestDavaExtensions();
    TestDispatcherExtensions();
    TestWindowsTiming();
    std::printf(
        "AUDIO EXTENSIONS: %d PASS / %d FAIL\n",
        g_passes,
        g_failures);
    return g_failures == 0 ? 0 : 1;
}
