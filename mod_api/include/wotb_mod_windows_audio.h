#pragma once

#include <stdint.h>

#include "wotb_mod_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* WotbModWindowsAudioHandle;

typedef struct WotbModWindowsAudioOptions {
    uint32_t struct_size;
    /*
     * Maximum decoded PCM bytes held by one clip. Zero selects 128 MiB.
     * The limit applies after Media Foundation decoding, not to file size.
     */
    uint32_t max_decoded_bytes;
} WotbModWindowsAudioOptions;

/*
 * Creates a ready WotbModRuntimeAudioBackend backed by Media Foundation and
 * XAudio2. It accepts formats for which Windows has an installed Media
 * Foundation decoder (normally WAV, MP3, AAC/M4A, and WMA).
 *
 * Keep out_handle alive until after WotbModRuntime_Shutdown. Destroy returns
 * ACCESS_DENIED while clips or playbacks are still owned by the runtime.
 */
WotbModResult WOTBMOD_CALL WotbModWindowsAudio_Create(
    const WotbModWindowsAudioOptions* options,
    WotbModWindowsAudioHandle* out_handle,
    WotbModRuntimeAudioBackend* out_backend);
WotbModResult WOTBMOD_CALL WotbModWindowsAudio_Destroy(
    WotbModWindowsAudioHandle handle);

#ifdef __cplusplus
} /* extern "C" */
#endif
