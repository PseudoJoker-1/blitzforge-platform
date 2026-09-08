#pragma once

#include "base.h"

/*
 * wotbmod.ui.read -- read back the UI strings and style values that THIS API
 * previously committed. Declared 2026-08-16 as part of the release contract
 * that follows RC1.
 *
 * Why a separate interface id and not a widened WotbModV3UiApiV3
 * --------------------------------------------------------------
 * `wotbmod.ui` is registered today and answers DEGRADED because a real,
 * working subset of it exists. These getters have no backend yet. Folding
 * them into the same interface id would force one capability status to
 * describe both, and the honest merge of "mostly works" and "does not exist"
 * is a status that lies about one of them. A separate id lets `wotbmod.ui`
 * keep its status while `wotbmod.ui.read` says UNAVAILABLE until its backend
 * lands. It also lets a mod that only wants to read text avoid asking for the
 * whole UI grant.
 *
 * What is actually knowable, and what is deliberately not
 * ------------------------------------------------------
 * No reverse engineering is involved here. The loader owns a C++ mirror of
 * every control this API touched (loader/v3_native_client_services.cpp,
 * struct NativeUi: text, texture, font, colours, opacity, font size and the
 * three text-layout flags), and the V3 client mirror (src/v3/client_services.cpp,
 * struct UiControl) holds the same values. UiSetString commits the mirror only
 * after the host call has returned WOTBMOD_V3_OK, so the mirror is exactly
 * "the last value this API successfully wrote" -- never a guess.
 *
 * That is the limit of the four mirror getters: they never read the engine
 * object, and WOTBMOD_V3_UI_READ_GAME_OWNED tells the caller when the
 * underlying control belongs to the game and may have been changed behind
 * the mirror's back.
 *
 * control_get_live_text (2026-09-04) is the one slot that does read the
 * engine: the UTF-8 string of the control's DAVA::UITextComponent
 * (11.20.0.887: vftable at RVA 0x0029AAA8, std::string at +0x44). It is
 * named apart from the mirror getters so nobody mistakes one for the other.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_UI_VERSION_4 4u

typedef enum WotbModV3UiReadFlag {
    /*
     * The handle wraps a UIControl owned by the game. The mirrored values are
     * still exactly what this API wrote, but the game may have overwritten the
     * control since. Treat a set bit as "this reading may be stale".
     */
    WOTBMOD_V3_UI_READ_GAME_OWNED = 1u << 0,
    /* Per-field provenance: the bit is set only if this API committed it. */
    WOTBMOD_V3_UI_READ_TEXT_SET = 1u << 1,
    WOTBMOD_V3_UI_READ_TEXTURE_SET = 1u << 2,
    WOTBMOD_V3_UI_READ_FONT_SET = 1u << 3,
    WOTBMOD_V3_UI_READ_COLOR_SET = 1u << 4,
    WOTBMOD_V3_UI_READ_BACKGROUND_COLOR_SET = 1u << 5,
    WOTBMOD_V3_UI_READ_OPACITY_SET = 1u << 6,
    WOTBMOD_V3_UI_READ_FONT_SIZE_SET = 1u << 7,
    WOTBMOD_V3_UI_READ_TEXT_LAYOUT_SET = 1u << 8,
    /*
     * control_get_live_text found a DAVA::UITextComponent on the control and
     * copied its current string. Set by the loader, never by the mirror.
     */
    WOTBMOD_V3_UI_READ_LIVE_TEXT_SET = 1u << 9
} WotbModV3UiReadFlag;

/*
 * Cleared fields are the mirror's construction defaults, not measurements.
 * Read `flags` before trusting any individual value.
 */
typedef struct WotbModV3UiStyleSnapshot {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t flags;
    uint32_t text_alignment;
    uint32_t text_wrap;
    uint32_t rich_text;
    float opacity;
    float font_size;
    WotbModV3Color color;
    WotbModV3Color background_color;
} WotbModV3UiStyleSnapshot;

typedef struct WotbModV3UiApiV4 {
    uint32_t struct_size;
    uint32_t api_version;

    /*
     * Copy-out convention is the one WotbModV3UiApiV2::control_get_id already
     * uses: on entry *inout_size is the buffer capacity in bytes, on success
     * it receives the byte length written including the terminator. A buffer
     * that is too small returns WOTBMOD_V3_E_BUFFER_TOO_SMALL and writes the
     * required size. No pointer into runtime storage is ever handed out.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_text)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_texture)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_font)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        char* buffer,
        uint32_t* inout_size);
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_style)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        WotbModV3UiStyleSnapshot* out_style);
    /*
     * 2026-09-04. The ENGINE's current text of the control - the UTF-8 string
     * of its DAVA::UITextComponent - as opposed to control_get_text, which
     * reads back this API's mirror. Works on game-owned controls (REVIEWED
     * tier, ui.modify.game) and on controls this API created. Answers
     * WOTBMOD_V3_E_NOT_FOUND when the control has no text component,
     * WOTBMOD_V3_E_NOT_SUPPORTED when the loader has no live-text backend.
     * The loader validates the component list before touching it and reads
     * under an exception guard; status is LIVE_TEST_PENDING until a live run
     * confirms the layout on this fingerprint.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* control_get_live_text)(
        WotbModV3Handle mod,
        WotbModV3UiHandle control,
        char* buffer,
        uint32_t* inout_size);
} WotbModV3UiApiV4;

#ifdef __cplusplus
}
#endif
