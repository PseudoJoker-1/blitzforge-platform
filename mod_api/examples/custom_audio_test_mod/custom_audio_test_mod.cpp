#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../include/wotb_mod_api.h"

static const WotbModHostApi* g_host = nullptr;
static WotbModHandle g_mod = nullptr;
static WotbModResourceMountId g_mount = 0;
static WotbModResourceHandle g_clip = nullptr;
static WotbModAudioPlaybackHandle g_playback = nullptr;
static volatile LONG g_passes = 0;
static volatile LONG g_failures = 0;
static double g_elapsed = 0.0;
static uint32_t g_step = 0;
static bool g_finished = false;

#pragma pack(push, 1)
struct WaveHeader {
    char riff[4];
    uint32_t riff_size;
    char wave[4];
    char fmt[4];
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
};
#pragma pack(pop)

static void Pass(const char* name) {
    InterlockedIncrement(&g_passes);
    wotbmod_logf(
        g_host,
        g_mod,
        WOTBMOD_LOG_INFO,
        "custom-audio-test:PASS:%s",
        name);
}

static void Fail(const char* name, WotbModResult result) {
    InterlockedIncrement(&g_failures);
    wotbmod_logf(
        g_host,
        g_mod,
        WOTBMOD_LOG_ERROR,
        "custom-audio-test:FAIL:%s result=%d",
        name,
        (int)result);
}

static bool Expect(
    const char* name,
    WotbModResult actual,
    WotbModResult expected = WOTBMOD_OK) {
    if (actual == expected) {
        Pass(name);
        return true;
    }
    Fail(name, actual);
    return false;
}

static bool GetDataPath(char* buffer, uint32_t capacity) {
    uint32_t size = capacity;
    const WotbModResult result =
        g_host->get_path(g_mod, WOTBMOD_PATH_DATA, buffer, &size);
    if (result != WOTBMOD_OK || !buffer[0]) {
        Fail("get_path:data", result);
        return false;
    }
    Pass("get_path:data");
    return true;
}

static bool WriteToneFile(void) {
    char dataPath[WOTBMOD_MAX_PATH] = {};
    if (!GetDataPath(dataPath, sizeof(dataPath))) return false;

    char directory[WOTBMOD_MAX_PATH] = {};
    if (_snprintf_s(
            directory,
            sizeof(directory),
            _TRUNCATE,
            "%s\\audio",
            dataPath) <= 0) {
        Fail("tone:directory-path", WOTBMOD_ERROR_BUFFER_TOO_SMALL);
        return false;
    }
    if (!CreateDirectoryA(directory, nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        Fail("tone:create-directory", WOTBMOD_ERROR_PLATFORM);
        return false;
    }

    char filePath[WOTBMOD_MAX_PATH] = {};
    if (_snprintf_s(
            filePath,
            sizeof(filePath),
            _TRUNCATE,
            "%s\\api-test-tone.wav",
            directory) <= 0) {
        Fail("tone:file-path", WOTBMOD_ERROR_BUFFER_TOO_SMALL);
        return false;
    }

    const uint32_t sampleRate = 44100;
    const uint32_t sampleCount = sampleRate * 2;
    const uint32_t dataSize = sampleCount * sizeof(int16_t);
    WaveHeader header = {
        {'R', 'I', 'F', 'F'},
        36u + dataSize,
        {'W', 'A', 'V', 'E'},
        {'f', 'm', 't', ' '},
        16u,
        1u,
        1u,
        sampleRate,
        sampleRate * sizeof(int16_t),
        sizeof(int16_t),
        16u,
        {'d', 'a', 't', 'a'},
        dataSize};

    HANDLE file = CreateFileA(
        filePath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Fail("tone:create", WOTBMOD_ERROR_PLATFORM);
        return false;
    }

    DWORD written = 0;
    bool ok =
        WriteFile(
            file,
            &header,
            sizeof(header),
            &written,
            nullptr) &&
        written == sizeof(header);

    int16_t samples[1024] = {};
    uint32_t produced = 0;
    while (ok && produced < sampleCount) {
        const uint32_t batch =
            sampleCount - produced < 1024
                ? sampleCount - produced
                : 1024;
        for (uint32_t index = 0; index < batch; ++index) {
            const double time =
                (double)(produced + index) / sampleRate;
            const double envelope =
                0.55 + 0.45 * sin(time * 6.283185307179586 * 2.0);
            samples[index] = (int16_t)(
                sin(time * 6.283185307179586 * 440.0) *
                envelope *
                5000.0);
        }
        written = 0;
        const DWORD bytes = batch * sizeof(int16_t);
        ok = WriteFile(
                 file,
                 samples,
                 bytes,
                 &written,
                 nullptr) &&
             written == bytes;
        produced += batch;
    }
    CloseHandle(file);

    if (!ok) {
        Fail("tone:write", WOTBMOD_ERROR_PLATFORM);
        return false;
    }
    Pass("tone:write");
    return true;
}

static bool AudioApiAvailable(void) {
    return g_host &&
           g_host->struct_size >=
               offsetof(WotbModHostApi, audio_clip_load) +
                   sizeof(g_host->audio_clip_load) &&
           g_host->resource_mount &&
           g_host->resource_unmount &&
           g_host->resource_load &&
           g_host->resource_reload &&
           g_host->resource_release &&
           g_host->audio_clip_load &&
           g_host->audio_play &&
           g_host->audio_pause &&
           g_host->audio_resume &&
           g_host->audio_stop &&
           g_host->audio_set_parameters &&
           g_host->audio_get_state &&
           g_host->audio_release;
}

static void Cleanup(void) {
    if (g_playback) {
        g_host->audio_stop(g_mod, g_playback);
        g_host->audio_release(g_mod, g_playback);
        g_playback = nullptr;
    }
    if (g_clip) {
        g_host->resource_release(g_mod, g_clip);
        g_clip = nullptr;
    }
    if (g_mount) {
        g_host->resource_unmount(g_mod, g_mount);
        g_mount = 0;
    }
}

static bool PrepareAudio(void) {
    if (!AudioApiAvailable()) {
        Fail("host-surface", WOTBMOD_ERROR_UNSUPPORTED_ABI);
        return false;
    }
    Pass("host-surface");
    if (!WriteToneFile()) return false;

    WotbModResourceMountInfo mount = {};
    mount.struct_size = sizeof(mount);
    mount.virtual_root = "~res:/Mods/test.custom-audio/";
    mount.source_directory = "audio";
    mount.priority = 100;
    if (!Expect(
            "resource_mount",
            g_host->resource_mount(g_mod, &mount, &g_mount))) {
        return false;
    }

    static const char virtualPath[] =
        "~res:/Mods/test.custom-audio/api-test-tone.wav";
    if (!Expect(
            "audio_clip_load",
            g_host->audio_clip_load(
                g_mod,
                virtualPath,
                &g_clip))) {
        return false;
    }

    WotbModResourceLoadRequest request = {};
    request.struct_size = sizeof(request);
    request.type = WOTBMOD_RESOURCE_AUDIO_CLIP;
    request.virtual_path = virtualPath;
    WotbModResourceHandle alias = nullptr;
    if (Expect(
            "resource_load:audio",
            g_host->resource_load(g_mod, &request, &alias))) {
        Expect(
            "resource_reload:audio-alias",
            g_host->resource_reload(g_mod, alias));
        Expect(
            "resource_release:audio-alias",
            g_host->resource_release(g_mod, alias));
    }

    WotbModAudioPlayInfo play = {};
    play.struct_size = sizeof(play);
    play.flags =
        WOTBMOD_AUDIO_PLAY_LOOP |
        WOTBMOD_AUDIO_PLAY_START_PAUSED;
    play.volume = 0.6f;
    play.pitch = 1.0f;
    play.pan = 0.0f;
    play.min_distance = 1.0f;
    play.max_distance = 100.0f;
    return Expect(
        "audio_play:start-paused-loop",
        g_host->audio_play(
            g_mod,
            g_clip,
            &play,
            &g_playback));
}

static void Finish(void) {
    Cleanup();
    g_finished = true;
    wotbmod_logf(
        g_host,
        g_mod,
        InterlockedCompareExchange(&g_failures, 0, 0) == 0
            ? WOTBMOD_LOG_INFO
            : WOTBMOD_LOG_ERROR,
        "custom-audio-test:SUMMARY passes=%ld failures=%ld",
        InterlockedCompareExchange(&g_passes, 0, 0),
        InterlockedCompareExchange(&g_failures, 0, 0));
}

static void WOTBMOD_CALL OnEnable(
    const WotbModHostApi* host,
    WotbModHandle mod) {
    g_host = host;
    g_mod = mod;
    g_elapsed = 0.0;
    g_step = 0;
    g_finished = false;
    InterlockedExchange(&g_passes, 0);
    InterlockedExchange(&g_failures, 0);
    host->log(
        mod,
        WOTBMOD_LOG_INFO,
        "custom-audio-test:BEGIN");
    if (!PrepareAudio()) Finish();
}

static void WOTBMOD_CALL OnFrame(
    const WotbModHostApi*,
    WotbModHandle,
    const WotbModFrameInfo* frame) {
    if (g_finished || !frame) return;
    g_elapsed += frame->delta_seconds;

    if (g_step == 0 && g_elapsed >= 0.10f) {
        WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
        if (Expect(
                "audio_get_state:paused",
                g_host->audio_get_state(
                    g_mod, g_playback, &state)) &&
            state != WOTBMOD_AUDIO_PAUSED) {
            Fail("audio_get_state:expected-paused", WOTBMOD_ERROR_PLATFORM);
        }
        Expect(
            "audio_resume:first",
            g_host->audio_resume(g_mod, g_playback));
        ++g_step;
    } else if (g_step == 1 && g_elapsed >= 0.50f) {
        WotbModAudioPlayInfo parameters = {};
        parameters.struct_size = sizeof(parameters);
        parameters.flags = WOTBMOD_AUDIO_PLAY_LOOP;
        parameters.volume = 0.35f;
        parameters.pitch = 1.1f;
        parameters.pan = -0.4f;
        parameters.min_distance = 1.0f;
        parameters.max_distance = 100.0f;
        Expect(
            "audio_set_parameters",
            g_host->audio_set_parameters(
                g_mod, g_playback, &parameters));
        ++g_step;
    } else if (g_step == 2 && g_elapsed >= 1.00f) {
        Expect(
            "audio_pause",
            g_host->audio_pause(g_mod, g_playback));
        ++g_step;
    } else if (g_step == 3 && g_elapsed >= 1.25f) {
        Expect(
            "audio_resume:second",
            g_host->audio_resume(g_mod, g_playback));
        ++g_step;
    } else if (g_step == 4 && g_elapsed >= 1.75f) {
        WotbModAudioState state = WOTBMOD_AUDIO_STOPPED;
        if (Expect(
                "audio_get_state:playing",
                g_host->audio_get_state(
                    g_mod, g_playback, &state)) &&
            state != WOTBMOD_AUDIO_PLAYING) {
            Fail("audio_get_state:expected-playing", WOTBMOD_ERROR_PLATFORM);
        }
        ++g_step;
    } else if (g_step == 5 && g_elapsed >= 2.00f) {
        Expect(
            "audio_stop:first",
            g_host->audio_stop(g_mod, g_playback));
        ++g_step;
    } else if (g_step == 6 && g_elapsed >= 2.10f) {
        WotbModAudioState state = WOTBMOD_AUDIO_PLAYING;
        if (Expect(
                "audio_get_state:stopped",
                g_host->audio_get_state(
                    g_mod, g_playback, &state)) &&
            state != WOTBMOD_AUDIO_STOPPED) {
            Fail("audio_get_state:expected-stopped", WOTBMOD_ERROR_PLATFORM);
        }
        Expect(
            "audio_release:first",
            g_host->audio_release(g_mod, g_playback));
        g_playback = nullptr;
        ++g_step;
    } else if (g_step == 7 && g_elapsed >= 2.20f) {
        Expect(
            "resource_reload:audio",
            g_host->resource_reload(g_mod, g_clip));
        ++g_step;
    } else if (g_step == 8 && g_elapsed >= 2.30f) {
        Expect(
            "audio_play:defaults",
            g_host->audio_play(
                g_mod,
                g_clip,
                nullptr,
                &g_playback));
        ++g_step;
    } else if (g_step == 9 && g_elapsed >= 2.80f) {
        Expect(
            "audio_stop:second",
            g_host->audio_stop(g_mod, g_playback));
        Expect(
            "audio_release:second",
            g_host->audio_release(g_mod, g_playback));
        g_playback = nullptr;
        Expect(
            "resource_release:audio",
            g_host->resource_release(g_mod, g_clip));
        g_clip = nullptr;
        Expect(
            "resource_unmount",
            g_host->resource_unmount(g_mod, g_mount));
        g_mount = 0;
        Finish();
    }
}

static void WOTBMOD_CALL OnDisable(
    const WotbModHostApi*,
    WotbModHandle) {
    Cleanup();
}

static void WOTBMOD_CALL OnUnload(
    const WotbModHostApi*,
    WotbModHandle) {
    g_host = nullptr;
    g_mod = nullptr;
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbCustomAudioTest_GetPasses(void) {
    return (uint32_t)InterlockedCompareExchange(&g_passes, 0, 0);
}

WOTBMOD_EXPORT uint32_t WOTBMOD_CALL
WotbCustomAudioTest_GetFailures(void) {
    return (uint32_t)InterlockedCompareExchange(&g_failures, 0, 0);
}

WOTBMOD_EXPORT int32_t WOTBMOD_CALL
WotbCustomAudioTest_IsFinished(void) {
    return g_finished ? 1 : 0;
}

WOTBMOD_ENTRY {
    if (!host || !mod || !out_info ||
        WOTBMOD_ABI_MAJOR(host->abi_version) !=
            WOTBMOD_ABI_MAJOR(WOTBMOD_ABI_VERSION)) {
        return WOTBMOD_ERROR_UNSUPPORTED_ABI;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->abi_version = WOTBMOD_ABI_VERSION;
    out_info->id = "test.custom-audio";
    out_info->name = "Custom Audio API Test";
    out_info->version = "1.2.0";
    out_info->author = "BlitzForge SDK";
    out_info->description =
        "Generates a WAV and tests every custom-audio operation.";
    out_info->on_enable = &OnEnable;
    out_info->on_frame = &OnFrame;
    out_info->on_disable = &OnDisable;
    out_info->on_unload = &OnUnload;
    return WOTBMOD_OK;
}
