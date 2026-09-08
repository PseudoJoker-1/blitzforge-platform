#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

#include <cmath>
#include <new>
#include <string>
#include <vector>

#include "../include/wotb_mod_windows_audio.h"

namespace {

const uint32_t kDefaultMaxDecodedBytes = 128u * 1024u * 1024u;

template <typename T>
static void SafeRelease(T*& value) {
    if (!value) return;
    value->Release();
    value = nullptr;
}

class ScopedCom {
public:
    ScopedCom()
        : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
          owns_(result_ == S_OK || result_ == S_FALSE) {}

    ~ScopedCom() {
        if (owns_) CoUninitialize();
    }

    bool available() const {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_;
    bool owns_;
};

struct WindowsAudioContext;

struct WindowsAudioClip {
    WindowsAudioContext* context;
    std::wstring file_path;
    std::vector<uint8_t> wave_format;
    std::vector<uint8_t> decoded_audio;
    volatile LONG playback_refs;
};

struct WindowsAudioPlayback {
    WindowsAudioContext* context;
    WindowsAudioClip* clip;
    IXAudio2SourceVoice* voice;
    WotbModAudioPlayInfo parameters;
    volatile LONG state;
    uint64_t samples_played_base;
    uint32_t play_begin_samples;
    double seek_base_seconds;
    SRWLOCK lock;
};

struct WindowsAudioContext {
    IXAudio2* engine;
    IXAudio2MasteringVoice* mastering_voice;
    uint32_t mastering_channels;
    uint32_t max_decoded_bytes;
    volatile LONG clip_count;
    volatile LONG playback_count;
};

static bool Utf8OrAnsiToWide(
    const char* input,
    std::wstring* output) {
    if (!input || !input[0] || !output) return false;
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int required = MultiByteToWideChar(
        codePage, flags, input, -1, nullptr, 0);
    if (required <= 0) {
        codePage = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(
            codePage, flags, input, -1, nullptr, 0);
    }
    if (required <= 1) return false;
    try {
        std::vector<wchar_t> buffer((size_t)required);
        if (MultiByteToWideChar(
                codePage,
                flags,
                input,
                -1,
                buffer.data(),
                required) <= 0) {
            return false;
        }
        output->assign(buffer.data());
        return true;
    } catch (...) {
        return false;
    }
}

static WotbModResult DecodeAudioFile(
    WindowsAudioContext* context,
    const wchar_t* filePath,
    std::vector<uint8_t>* outWaveFormat,
    std::vector<uint8_t>* outDecodedAudio) {
    if (!context ||
        !filePath ||
        !filePath[0] ||
        !outWaveFormat ||
        !outDecodedAudio) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    ScopedCom com;
    if (!com.available()) return WOTBMOD_ERROR_PLATFORM;

    IMFSourceReader* reader = nullptr;
    IMFMediaType* requestedType = nullptr;
    IMFMediaType* currentType = nullptr;
    WAVEFORMATEX* waveFormat = nullptr;
    UINT32 waveFormatSize = 0;
    WotbModResult result = WOTBMOD_ERROR_PLATFORM;

    HRESULT hr = MFCreateSourceReaderFromURL(
        filePath, nullptr, &reader);
    if (FAILED(hr)) {
        result = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) == hr
                     ? WOTBMOD_ERROR_NOT_FOUND
                     : WOTBMOD_ERROR_PLATFORM;
        goto cleanup;
    }

    hr = reader->SetStreamSelection(
        (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED(hr)) goto cleanup;
    hr = reader->SetStreamSelection(
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) goto cleanup;

    hr = MFCreateMediaType(&requestedType);
    if (FAILED(hr)) goto cleanup;
    hr = requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (FAILED(hr)) goto cleanup;
    hr = requestedType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (FAILED(hr)) goto cleanup;
    hr = reader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
        nullptr,
        requestedType);
    if (FAILED(hr)) goto cleanup;
    hr = reader->GetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
        &currentType);
    if (FAILED(hr)) goto cleanup;
    hr = MFCreateWaveFormatExFromMFMediaType(
        currentType,
        &waveFormat,
        &waveFormatSize,
        MFWaveFormatExConvertFlag_Normal);
    if (FAILED(hr) || !waveFormat || waveFormatSize < sizeof(WAVEFORMATEX)) {
        goto cleanup;
    }

    try {
        outWaveFormat->assign(
            reinterpret_cast<const uint8_t*>(waveFormat),
            reinterpret_cast<const uint8_t*>(waveFormat) +
                waveFormatSize);
        outDecodedAudio->clear();
    } catch (...) {
        result = WOTBMOD_ERROR_LIMIT_REACHED;
        goto cleanup;
    }

    for (;;) {
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        hr = reader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0,
            &actualStream,
            &flags,
            &timestamp,
            &sample);
        if (FAILED(hr)) {
            SafeRelease(sample);
            goto cleanup;
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            SafeRelease(sample);
            goto cleanup;
        }
        if (sample) {
            IMFMediaBuffer* mediaBuffer = nullptr;
            BYTE* bytes = nullptr;
            DWORD maxLength = 0;
            DWORD currentLength = 0;
            hr = sample->ConvertToContiguousBuffer(&mediaBuffer);
            if (SUCCEEDED(hr)) {
                hr = mediaBuffer->Lock(
                    &bytes, &maxLength, &currentLength);
            }
            if (SUCCEEDED(hr)) {
                const size_t currentSize = outDecodedAudio->size();
                const size_t decodedLimit =
                    (size_t)context->max_decoded_bytes;
                if (currentSize > decodedLimit ||
                    (size_t)currentLength >
                        decodedLimit - currentSize) {
                    result = WOTBMOD_ERROR_LIMIT_REACHED;
                    mediaBuffer->Unlock();
                    SafeRelease(mediaBuffer);
                    SafeRelease(sample);
                    goto cleanup;
                }
                try {
                    outDecodedAudio->insert(
                        outDecodedAudio->end(),
                        bytes,
                        bytes + currentLength);
                } catch (...) {
                    result = WOTBMOD_ERROR_LIMIT_REACHED;
                    mediaBuffer->Unlock();
                    SafeRelease(mediaBuffer);
                    SafeRelease(sample);
                    goto cleanup;
                }
                mediaBuffer->Unlock();
            }
            SafeRelease(mediaBuffer);
            SafeRelease(sample);
            if (FAILED(hr)) goto cleanup;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
    }

    if (outDecodedAudio->empty()) {
        result = WOTBMOD_ERROR_PLATFORM;
        goto cleanup;
    }
    result = WOTBMOD_OK;

cleanup:
    if (result != WOTBMOD_OK) {
        outWaveFormat->clear();
        outDecodedAudio->clear();
    }
    if (waveFormat) CoTaskMemFree(waveFormat);
    SafeRelease(currentType);
    SafeRelease(requestedType);
    SafeRelease(reader);
    return result;
}

static WotbModResult SubmitPlaybackBuffer(
    WindowsAudioPlayback* playback,
    uint32_t flags) {
    if (!playback ||
        !playback->voice ||
        !playback->clip ||
        playback->clip->decoded_audio.empty() ||
        playback->clip->decoded_audio.size() > UINT32_MAX) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    XAUDIO2_BUFFER buffer = {};
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes =
        (UINT32)playback->clip->decoded_audio.size();
    buffer.pAudioData =
        playback->clip->decoded_audio.data();
    buffer.PlayBegin = playback->play_begin_samples;
    buffer.LoopCount =
        (flags & WOTBMOD_AUDIO_PLAY_LOOP)
            ? XAUDIO2_LOOP_INFINITE
            : 0;
    if (buffer.LoopCount != 0u) {
        buffer.LoopBegin = playback->play_begin_samples;
    }
    return SUCCEEDED(playback->voice->SubmitSourceBuffer(&buffer))
               ? WOTBMOD_OK
               : WOTBMOD_ERROR_PLATFORM;
}

static bool GetClipTiming(
    const WindowsAudioClip* clip,
    uint64_t* outAlignedBytes,
    uint32_t* outSampleFrames,
    double* outDurationSeconds) {
    if (!clip ||
        clip->wave_format.size() < sizeof(WAVEFORMATEX) ||
        clip->decoded_audio.empty()) {
        return false;
    }
    const WAVEFORMATEX* format =
        reinterpret_cast<const WAVEFORMATEX*>(
            clip->wave_format.data());
    if (format->nBlockAlign == 0u ||
        format->nAvgBytesPerSec == 0u ||
        format->nSamplesPerSec == 0u) {
        return false;
    }
    const uint64_t alignedBytes =
        (static_cast<uint64_t>(clip->decoded_audio.size()) /
         format->nBlockAlign) *
        format->nBlockAlign;
    const uint64_t sampleFrames64 =
        alignedBytes / format->nBlockAlign;
    if (alignedBytes == 0u ||
        alignedBytes > UINT32_MAX ||
        sampleFrames64 > UINT32_MAX) {
        return false;
    }
    if (outAlignedBytes) *outAlignedBytes = alignedBytes;
    if (outSampleFrames) {
        *outSampleFrames = static_cast<uint32_t>(sampleFrames64);
    }
    if (outDurationSeconds) {
        *outDurationSeconds =
            static_cast<double>(alignedBytes) /
            static_cast<double>(format->nAvgBytesPerSec);
    }
    return true;
}

static double GetPlaybackPositionLocked(
    WindowsAudioPlayback* playback) {
    uint64_t alignedBytes = 0u;
    double durationSeconds = 0.0;
    if (!playback || !playback->voice ||
        !GetClipTiming(
            playback->clip,
            &alignedBytes,
            nullptr,
            &durationSeconds)) {
        return -1.0;
    }
    (void)alignedBytes;
    const WAVEFORMATEX* format =
        reinterpret_cast<const WAVEFORMATEX*>(
            playback->clip->wave_format.data());
    XAUDIO2_VOICE_STATE voiceState = {};
    playback->voice->GetState(&voiceState, 0u);
    const uint64_t samplesSinceSeek =
        voiceState.SamplesPlayed >= playback->samples_played_base
            ? voiceState.SamplesPlayed -
                  playback->samples_played_base
            : 0u;
    const double elapsedSeconds =
        static_cast<double>(samplesSinceSeek) /
        static_cast<double>(format->nSamplesPerSec);
    double position = playback->seek_base_seconds + elapsedSeconds;
    if ((playback->parameters.flags &
         WOTBMOD_AUDIO_PLAY_LOOP) != 0u) {
        const double loopDuration =
            durationSeconds - playback->seek_base_seconds;
        if (loopDuration > 0.0 && position >= durationSeconds) {
            position =
                playback->seek_base_seconds +
                std::fmod(elapsedSeconds, loopDuration);
        }
    } else if (position > durationSeconds) {
        position = durationSeconds;
    }
    return position < 0.0 ? 0.0 : position;
}

static WotbModResult SeekPlaybackLocked(
    WindowsAudioPlayback* playback,
    double seconds) {
    if (!playback || !playback->voice || !std::isfinite(seconds)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    uint64_t alignedBytes = 0u;
    uint32_t sampleFrames = 0u;
    double durationSeconds = 0.0;
    if (!GetClipTiming(
            playback->clip,
            &alignedBytes,
            &sampleFrames,
            &durationSeconds)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    const WAVEFORMATEX* format =
        reinterpret_cast<const WAVEFORMATEX*>(
            playback->clip->wave_format.data());
    double clampedSeconds = seconds;
    if (clampedSeconds < 0.0) clampedSeconds = 0.0;
    if (clampedSeconds > durationSeconds) {
        clampedSeconds = durationSeconds;
    }
    uint64_t byteOffset = static_cast<uint64_t>(
        clampedSeconds *
        static_cast<double>(format->nAvgBytesPerSec));
    byteOffset =
        (byteOffset / format->nBlockAlign) *
        format->nBlockAlign;
    /*
     * XAudio2 requires PlayBegin to identify a playable sample. Represent an
     * exact end seek by the final complete sample frame.
     */
    if (byteOffset >= alignedBytes) {
        byteOffset = alignedBytes - format->nBlockAlign;
    }
    const uint32_t playBegin = static_cast<uint32_t>(
        byteOffset / format->nBlockAlign);
    if (playBegin >= sampleFrames) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    const LONG previousState = InterlockedCompareExchange(
        &playback->state, 0, 0);
    HRESULT hr = playback->voice->Stop();
    if (SUCCEEDED(hr)) {
        hr = playback->voice->FlushSourceBuffers();
    }
    if (FAILED(hr)) return WOTBMOD_ERROR_PLATFORM;

    playback->play_begin_samples = playBegin;
    playback->seek_base_seconds =
        static_cast<double>(byteOffset) /
        static_cast<double>(format->nAvgBytesPerSec);
    WotbModResult result = SubmitPlaybackBuffer(
        playback, playback->parameters.flags);
    if (result != WOTBMOD_OK) return result;

    XAUDIO2_VOICE_STATE voiceState = {};
    playback->voice->GetState(&voiceState, 0u);
    playback->samples_played_base = voiceState.SamplesPlayed;
    if (previousState == WOTBMOD_AUDIO_PLAYING &&
        FAILED(playback->voice->Start())) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    InterlockedExchange(&playback->state, previousState);
    return WOTBMOD_OK;
}

static WotbModResult ApplyPlaybackParameters(
    WindowsAudioPlayback* playback,
    const WotbModAudioPlayInfo* parameters) {
    if (!playback ||
        !playback->voice ||
        !playback->clip ||
        !parameters) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if ((parameters->flags & WOTBMOD_AUDIO_PLAY_SPATIAL) != 0) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    if (parameters->pitch > XAUDIO2_MAX_FREQ_RATIO) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    HRESULT hr = playback->voice->SetVolume(parameters->volume);
    if (FAILED(hr)) return WOTBMOD_ERROR_PLATFORM;
    hr = playback->voice->SetFrequencyRatio(parameters->pitch);
    if (FAILED(hr)) return WOTBMOD_ERROR_PLATFORM;

    const WAVEFORMATEX* format =
        reinterpret_cast<const WAVEFORMATEX*>(
            playback->clip->wave_format.data());
    const uint32_t sourceChannels = format->nChannels;
    const uint32_t destinationChannels =
        playback->context->mastering_channels;
    if (destinationChannels >= 2 &&
        (sourceChannels == 1 || sourceChannels == 2)) {
        const float left =
            parameters->pan > 0.0f
                ? 1.0f - parameters->pan
                : 1.0f;
        const float right =
            parameters->pan < 0.0f
                ? 1.0f + parameters->pan
                : 1.0f;
        std::vector<float> matrix;
        try {
            matrix.assign(
                (size_t)sourceChannels * destinationChannels,
                0.0f);
        } catch (...) {
            return WOTBMOD_ERROR_LIMIT_REACHED;
        }
        if (sourceChannels == 1) {
            matrix[0] = left;
            matrix[1] = right;
        } else {
            matrix[0] = left;
            matrix[destinationChannels + 1] = right;
        }
        hr = playback->voice->SetOutputMatrix(
            playback->context->mastering_voice,
            sourceChannels,
            destinationChannels,
            matrix.data());
        if (FAILED(hr)) return WOTBMOD_ERROR_PLATFORM;
    } else if (parameters->pan != 0.0f) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL LoadClip(
    void* userData,
    const char* resolvedFilePath,
    void** outNativeAudioClip) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    if (!context || !resolvedFilePath || !outNativeAudioClip) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativeAudioClip = nullptr;

    WindowsAudioClip* clip =
        new (std::nothrow) WindowsAudioClip();
    if (!clip) return WOTBMOD_ERROR_LIMIT_REACHED;
    clip->context = context;
    clip->playback_refs = 0;
    if (!Utf8OrAnsiToWide(resolvedFilePath, &clip->file_path)) {
        delete clip;
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    const WotbModResult result = DecodeAudioFile(
        context,
        clip->file_path.c_str(),
        &clip->wave_format,
        &clip->decoded_audio);
    if (result != WOTBMOD_OK) {
        delete clip;
        return result;
    }
    InterlockedIncrement(&context->clip_count);
    *outNativeAudioClip = clip;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ReloadClip(
    void* userData,
    void* nativeAudioClip) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioClip* clip =
        static_cast<WindowsAudioClip*>(nativeAudioClip);
    if (!context || !clip || clip->context != context) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(
            &clip->playback_refs, 0, 0) != 0) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    std::vector<uint8_t> newFormat;
    std::vector<uint8_t> newAudio;
    const WotbModResult result = DecodeAudioFile(
        context,
        clip->file_path.c_str(),
        &newFormat,
        &newAudio);
    if (result != WOTBMOD_OK) return result;
    clip->wave_format.swap(newFormat);
    clip->decoded_audio.swap(newAudio);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ReleaseClip(
    void* userData,
    void* nativeAudioClip) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioClip* clip =
        static_cast<WindowsAudioClip*>(nativeAudioClip);
    if (!context || !clip || clip->context != context) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    if (InterlockedCompareExchange(
            &clip->playback_refs, 0, 0) != 0) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    delete clip;
    InterlockedDecrement(&context->clip_count);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL Play(
    void* userData,
    void* nativeAudioClip,
    const WotbModAudioPlayInfo* playInfo,
    void** outNativePlayback) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioClip* clip =
        static_cast<WindowsAudioClip*>(nativeAudioClip);
    if (!context ||
        !clip ||
        clip->context != context ||
        !playInfo ||
        !outNativePlayback) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outNativePlayback = nullptr;
    if ((playInfo->flags & WOTBMOD_AUDIO_PLAY_SPATIAL) != 0) {
        return WOTBMOD_ERROR_PLATFORM;
    }

    WindowsAudioPlayback* playback =
        new (std::nothrow) WindowsAudioPlayback();
    if (!playback) return WOTBMOD_ERROR_LIMIT_REACHED;
    ZeroMemory(playback, sizeof(*playback));
    playback->context = context;
    playback->clip = clip;
    playback->parameters = *playInfo;
    InitializeSRWLock(&playback->lock);

    WAVEFORMATEX* format =
        reinterpret_cast<WAVEFORMATEX*>(
            clip->wave_format.data());
    HRESULT hr = context->engine->CreateSourceVoice(
        &playback->voice,
        format,
        0,
        XAUDIO2_MAX_FREQ_RATIO);
    if (FAILED(hr) || !playback->voice) {
        delete playback;
        return WOTBMOD_ERROR_PLATFORM;
    }

    WotbModResult result =
        ApplyPlaybackParameters(playback, playInfo);
    if (result == WOTBMOD_OK) {
        result = SubmitPlaybackBuffer(playback, playInfo->flags);
    }
    if (result == WOTBMOD_OK) {
        XAUDIO2_VOICE_STATE voiceState = {};
        playback->voice->GetState(&voiceState, 0u);
        playback->samples_played_base = voiceState.SamplesPlayed;
    }
    if (result == WOTBMOD_OK &&
        (playInfo->flags & WOTBMOD_AUDIO_PLAY_START_PAUSED) == 0) {
        if (FAILED(playback->voice->Start())) {
            result = WOTBMOD_ERROR_PLATFORM;
        }
    }
    if (result != WOTBMOD_OK) {
        playback->voice->DestroyVoice();
        delete playback;
        return result;
    }

    playback->state =
        (playInfo->flags & WOTBMOD_AUDIO_PLAY_START_PAUSED)
            ? WOTBMOD_AUDIO_PAUSED
            : WOTBMOD_AUDIO_PLAYING;
    InterlockedIncrement(&clip->playback_refs);
    InterlockedIncrement(&context->playback_count);
    *outNativePlayback = playback;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL Pause(
    void* userData,
    void* nativePlayback) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context || !playback || playback->context != context) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&playback->lock);
    const HRESULT hr = playback->voice->Stop();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(
            &playback->state, WOTBMOD_AUDIO_PAUSED);
    }
    ReleaseSRWLockExclusive(&playback->lock);
    return SUCCEEDED(hr) ? WOTBMOD_OK : WOTBMOD_ERROR_PLATFORM;
}

static WotbModResult WOTBMOD_CALL Resume(
    void* userData,
    void* nativePlayback) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context || !playback || playback->context != context) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&playback->lock);
    XAUDIO2_VOICE_STATE voiceState = {};
    HRESULT hr = S_OK;
    if (InterlockedCompareExchange(
            &playback->state, 0, 0) == WOTBMOD_AUDIO_STOPPED) {
        hr = E_FAIL;
    } else {
        playback->voice->GetState(
            &voiceState, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        hr = voiceState.BuffersQueued == 0
                 ? E_FAIL
                 : playback->voice->Start();
    }
    if (SUCCEEDED(hr)) {
        InterlockedExchange(
            &playback->state, WOTBMOD_AUDIO_PLAYING);
    }
    ReleaseSRWLockExclusive(&playback->lock);
    return SUCCEEDED(hr) ? WOTBMOD_OK : WOTBMOD_ERROR_PLATFORM;
}

static WotbModResult WOTBMOD_CALL Stop(
    void* userData,
    void* nativePlayback) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context || !playback || playback->context != context) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&playback->lock);
    HRESULT hr = playback->voice->Stop();
    if (SUCCEEDED(hr)) hr = playback->voice->FlushSourceBuffers();
    if (SUCCEEDED(hr)) {
        InterlockedExchange(
            &playback->state, WOTBMOD_AUDIO_STOPPED);
    }
    ReleaseSRWLockExclusive(&playback->lock);
    return SUCCEEDED(hr) ? WOTBMOD_OK : WOTBMOD_ERROR_PLATFORM;
}

static WotbModResult WOTBMOD_CALL Seek(
    void* userData,
    void* nativePlayback,
    double seconds) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context || !playback || playback->context != context ||
        !std::isfinite(seconds)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&playback->lock);
    const WotbModResult result =
        SeekPlaybackLocked(playback, seconds);
    ReleaseSRWLockExclusive(&playback->lock);
    return result;
}

static WotbModResult WOTBMOD_CALL GetPosition(
    void* userData,
    void* nativePlayback,
    double* outSeconds) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context || !playback || playback->context != context ||
        !outSeconds) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockShared(&playback->lock);
    const double position = GetPlaybackPositionLocked(playback);
    ReleaseSRWLockShared(&playback->lock);
    if (!std::isfinite(position) || position < 0.0) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    *outSeconds = position;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL GetDuration(
    void* userData,
    void* nativeAudioClip,
    double* outSeconds) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioClip* clip =
        static_cast<WindowsAudioClip*>(nativeAudioClip);
    if (!context || !clip || clip->context != context ||
        !outSeconds) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    double duration = 0.0;
    if (!GetClipTiming(clip, nullptr, nullptr, &duration)) {
        return WOTBMOD_ERROR_PLATFORM;
    }
    *outSeconds = duration;
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL SetParameters(
    void* userData,
    void* nativePlayback,
    const WotbModAudioPlayInfo* parameters) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context ||
        !playback ||
        playback->context != context ||
        !parameters) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&playback->lock);
    WotbModResult result =
        ApplyPlaybackParameters(playback, parameters);
    const bool loopChanged =
        ((playback->parameters.flags ^ parameters->flags) &
         WOTBMOD_AUDIO_PLAY_LOOP) != 0;
    if (result == WOTBMOD_OK && loopChanged) {
        const LONG previousState = InterlockedCompareExchange(
            &playback->state, 0, 0);
        HRESULT hr = playback->voice->Stop();
        if (SUCCEEDED(hr)) hr = playback->voice->FlushSourceBuffers();
        if (SUCCEEDED(hr)) {
            result = SubmitPlaybackBuffer(
                playback, parameters->flags);
            if (result == WOTBMOD_OK) {
                XAUDIO2_VOICE_STATE voiceState = {};
                playback->voice->GetState(&voiceState, 0u);
                playback->samples_played_base =
                    voiceState.SamplesPlayed;
            }
        } else {
            result = WOTBMOD_ERROR_PLATFORM;
        }
        if (result == WOTBMOD_OK &&
            previousState == WOTBMOD_AUDIO_PLAYING &&
            FAILED(playback->voice->Start())) {
            result = WOTBMOD_ERROR_PLATFORM;
        }
    }
    if (result == WOTBMOD_OK) {
        playback->parameters = *parameters;
    }
    ReleaseSRWLockExclusive(&playback->lock);
    return result;
}

static WotbModResult WOTBMOD_CALL GetState(
    void* userData,
    void* nativePlayback,
    WotbModAudioState* outState) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context ||
        !playback ||
        playback->context != context ||
        !outState) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockShared(&playback->lock);
    LONG state = InterlockedCompareExchange(
        &playback->state, 0, 0);
    if (state == WOTBMOD_AUDIO_PLAYING) {
        XAUDIO2_VOICE_STATE voiceState = {};
        playback->voice->GetState(
            &voiceState, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (voiceState.BuffersQueued == 0) {
            state = WOTBMOD_AUDIO_STOPPED;
            InterlockedExchange(&playback->state, state);
        }
    }
    *outState = static_cast<WotbModAudioState>(state);
    ReleaseSRWLockShared(&playback->lock);
    return WOTBMOD_OK;
}

static WotbModResult WOTBMOD_CALL ReleasePlayback(
    void* userData,
    void* nativePlayback) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(userData);
    WindowsAudioPlayback* playback =
        static_cast<WindowsAudioPlayback*>(nativePlayback);
    if (!context || !playback || playback->context != context) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    AcquireSRWLockExclusive(&playback->lock);
    playback->voice->Stop();
    playback->voice->FlushSourceBuffers();
    playback->voice->DestroyVoice();
    playback->voice = nullptr;
    WindowsAudioClip* clip = playback->clip;
    playback->clip = nullptr;
    ReleaseSRWLockExclusive(&playback->lock);
    delete playback;
    InterlockedDecrement(&clip->playback_refs);
    InterlockedDecrement(&context->playback_count);
    return WOTBMOD_OK;
}

} /* namespace */

extern "C" WotbModResult WOTBMOD_CALL WotbModWindowsAudio_Create(
    const WotbModWindowsAudioOptions* options,
    WotbModWindowsAudioHandle* outHandle,
    WotbModRuntimeAudioBackend* outBackend) {
    if (!outHandle || !outBackend) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }
    *outHandle = nullptr;
    ZeroMemory(outBackend, sizeof(*outBackend));
    if (options &&
        options->struct_size <
            offsetof(WotbModWindowsAudioOptions, max_decoded_bytes) +
                sizeof(options->max_decoded_bytes)) {
        return WOTBMOD_ERROR_INVALID_ARGUMENT;
    }

    ScopedCom com;
    if (!com.available()) return WOTBMOD_ERROR_PLATFORM;

    WindowsAudioContext* context =
        new (std::nothrow) WindowsAudioContext();
    if (!context) return WOTBMOD_ERROR_LIMIT_REACHED;
    ZeroMemory(context, sizeof(*context));
    context->max_decoded_bytes =
        options && options->max_decoded_bytes
            ? options->max_decoded_bytes
            : kDefaultMaxDecodedBytes;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        delete context;
        return WOTBMOD_ERROR_PLATFORM;
    }
    hr = XAudio2Create(&context->engine);
    if (SUCCEEDED(hr)) {
        hr = context->engine->CreateMasteringVoice(
            &context->mastering_voice,
            XAUDIO2_DEFAULT_CHANNELS,
            XAUDIO2_DEFAULT_SAMPLERATE,
            0,
            nullptr,
            nullptr,
            AudioCategory_GameEffects);
    }
    if (FAILED(hr) || !context->mastering_voice) {
        SafeRelease(context->engine);
        MFShutdown();
        delete context;
        return WOTBMOD_ERROR_PLATFORM;
    }

    XAUDIO2_VOICE_DETAILS details = {};
    context->mastering_voice->GetVoiceDetails(&details);
    context->mastering_channels = details.InputChannels;

    outBackend->struct_size = sizeof(*outBackend);
    outBackend->user_data = context;
    outBackend->play = &Play;
    outBackend->pause = &Pause;
    outBackend->resume = &Resume;
    outBackend->stop = &Stop;
    outBackend->set_parameters = &SetParameters;
    outBackend->get_state = &GetState;
    outBackend->release = &ReleasePlayback;
    outBackend->load_clip = &LoadClip;
    outBackend->reload_clip = &ReloadClip;
    outBackend->release_clip = &ReleaseClip;
    outBackend->seek = &Seek;
    outBackend->get_position = &GetPosition;
    outBackend->get_duration = &GetDuration;
    *outHandle = context;
    return WOTBMOD_OK;
}

extern "C" WotbModResult WOTBMOD_CALL WotbModWindowsAudio_Destroy(
    WotbModWindowsAudioHandle handle) {
    WindowsAudioContext* context =
        static_cast<WindowsAudioContext*>(handle);
    if (!context) return WOTBMOD_ERROR_INVALID_ARGUMENT;
    if (InterlockedCompareExchange(
            &context->clip_count, 0, 0) != 0 ||
        InterlockedCompareExchange(
            &context->playback_count, 0, 0) != 0) {
        return WOTBMOD_ERROR_ACCESS_DENIED;
    }
    if (context->mastering_voice) {
        context->mastering_voice->DestroyVoice();
        context->mastering_voice = nullptr;
    }
    SafeRelease(context->engine);
    MFShutdown();
    delete context;
    return WOTBMOD_OK;
}
