#pragma once

#include "../include/wotb_mod_runtime.h"
#include "../src/v3/client_services_backend.h"

namespace wotbmod {
namespace loader {

struct V3NativeClientServicesOptions {
    uint32_t struct_size;
    const WotbModRuntimeResourceBackend* resource_backend;
    const WotbModRuntimeAudioBackend* audio_backend;
    const WotbModRuntimeSoundBackend* sound_backend;
    void* (WOTBMOD_CALL* resource_identity)(
        void* user_data,
        void* resource);
    void* resource_identity_user_data;
    WotbModResult (WOTBMOD_CALL* resource_bring_ui_to_front)(
        void* user_data,
        void* resource);
    void* resource_bring_ui_to_front_user_data;
    /*
     * Loaded image window for the read-only scene walk. Null base, zero size
     * or a compatibility_state that is not SUPPORTED/DEGRADED leaves the walk
     * permanently unavailable - the walk reads engine objects by fixed offset,
     * so it is gated on the same exact-fingerprint proof as every other
     * fixed-RVA surface, fail-closed.
     */
    const uint8_t* game_base;
    size_t game_image_size;
    uint32_t compatibility_state;
};

WotbModV3Result InitializeV3NativeClientServices(
    const V3NativeClientServicesOptions* options);

WotbModV3Result InvokeV3NativeClientServices(
    WotbModV3Handle mod,
    const char* operation,
    const void* request,
    uint32_t request_size,
    void* response,
    uint32_t response_size);

/* Records DAVA pointer phases so resource replacement never runs while the
 * native input system can retain a pressed or dragged UIControl pointer. */
void NotifyV3NativeClientServicesUiInputPhase(uint32_t phase);

/* Ends any retained pointer gesture when the active DAVA screen changes.
 * The idle-frame barrier still applies before queued mutations resume. */
void ResetV3NativeClientServicesUiInputState();

/* Applies coalesced UI resource rebuilds after input becomes idle, retires
 * replaced controls after several complete frames, and detaches transient
 * Scene entities. Call exactly once from the Present-side frame pump. */
void PumpV3NativeClientServicesFrame();

/*
 * Publishes the PROVEN main-thread identity.
 *
 * Called from the EngineBackend::UpdateAndDrawWindows detour, whose thread is
 * the Win32 message-pump thread by construction (see kEngineUpdateAndDrawWindows
 * Rva). Until it has been called at least once the scene walk answers
 * WOTBMOD_V3_E_WRONG_THREAD to everyone, including the real main thread: an
 * unobserved main thread is not a proven main thread, and guessing here is a
 * use-after-free in someone else's game.
 */
void NotifyV3NativeClientServicesMainThread(
    uint32_t thread_id,
    uint64_t main_frame_index);

/* True only on the thread UpdateAndDrawWindows was last seen running on. */
bool IsV3NativeClientServicesMainThread();

/* -------------------------------------------------------------------------
 * Declared-backend slots owned by this translation unit.
 *
 * ui.read is a pure read-back of the mirror this file already maintains for
 * every control the API has written, so it touches no engine memory at all.
 * scene.enumerate walks game-owned Entity objects and is MAIN THREAD ONLY.
 * The remaining slots of ClientHostDeclaredBackend live in
 * v3_native_bindings.cpp, which owns the hooks and the image window.
 * ------------------------------------------------------------------------- */

WotbModV3Result V3NativeUiReadString(
    void* user_data,
    const v3::ClientHostUiReadRequest* request,
    v3::ClientHostUiReadString* out_value);

WotbModV3Result V3NativeUiReadStyle(
    void* user_data,
    const v3::ClientHostUiReadRequest* request,
    WotbModV3UiStyleSnapshot* out_style);

WotbModV3Result V3NativeSceneWalkLimits(
    void* user_data,
    WotbModV3SceneWalkLimits* out_limits);

WotbModV3Result V3NativeSceneWalkActive(
    void* user_data,
    const WotbModV3SceneWalkRequest* request,
    WotbModV3SceneNodeRecord* out_nodes,
    uint32_t node_capacity,
    uint32_t* out_reached);

void ShutdownV3NativeClientServices();

}  // namespace loader
}  // namespace wotbmod
