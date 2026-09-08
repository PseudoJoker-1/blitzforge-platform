#pragma once

#include <stdint.h>

#include "wotb_mod_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* WotbModDavaSoundHandle;

/*
 * Version-specific anchors for the WoT Blitz 11.19.0.834 x86 client.
 * All values are image-base-relative. A loader may override every RVA after
 * re-anchoring a newer client. Zero fields select the verified 11.19 defaults.
 */
typedef struct WotbModDavaSoundOptions {
    uint32_t struct_size;
    void* game_module;
    uint32_t sound_system_singleton_rva;
    uint32_t fast_name_ctor_rva;
    uint32_t ref_counted_release_rva;
    uint32_t default_sound_group_rva;
    uint32_t sound_system_vtable_rva;
    uint32_t hybrid_event_vtable_rva;
    uint32_t wwise_event_vtable_rva;
    uint32_t sound_system_proxy_vtable_rva;
} WotbModDavaSoundOptions;

WotbModResult WOTBMOD_CALL WotbModDavaSound_Create(
    const WotbModDavaSoundOptions* options,
    WotbModDavaSoundHandle* out_handle,
    WotbModRuntimeSoundBackend* out_backend);

WotbModResult WOTBMOD_CALL WotbModDavaSound_Destroy(
    WotbModDavaSoundHandle handle);

#ifdef __cplusplus
} /* extern "C" */
#endif
