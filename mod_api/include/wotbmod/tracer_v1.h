#pragma once

#include "base.h"

/*
 * wotbmod.tracer -- READ ONLY query of the stock tracer style a shell type
 * maps to. Declared 2026-08-16 as part of the release contract that follows
 * RC1. Cross-checked against 11.19.0.834 / 41960DBD...E0AD.
 *
 * The proven data
 * ---------------
 * A 25-entry table of const char* at 0x03FD6168 maps a byte shell-type code
 * (0..24) onto eight distinct style names. The eight, read out of the image:
 * ARMOR_PIERCING, ARMOR_PIERCING_CR, HIGH_EXPLOSIVE, HOLLOW_CHARGE,
 * ANTI_TANK_GUIDED_MISSILE, RAILGUN, IMPROVED_DETECTION, STT_TRACER. Codes
 * 0..15 cycle through the first four in groups of four; 16..24 are the
 * remainder. A code of 25 or more is out of the table and is an error, never a
 * clamp to the last entry. The stock RGBA default is the Color written at
 * record+0x34 of the per-shell record built by the tracer parameter
 * configuration path (0x015599F0).
 *
 * What is NOT here, and why
 * -------------------------
 * Tracer CREATION and style MUTATION. re_anchors.md is explicit: the 11.19
 * constructor builds refcounted/keyed per-shell records holding Color and
 * Width, which proves the data exists but NOT a stable ownership or mutation
 * ABI. TracerManager::ShowTracer (0x0157D670, retn 0x1C, seven internal
 * arguments consuming refcounted/keyed shell and scene objects) and the
 * explosion-path candidate are research anchors, not callable backends: no
 * argument layout, lifetime, thread restriction or local-owner filter has been
 * validated live.
 *
 * That is precisely the failure API_V3_RC1_FREEZE.md forbids -- returning OK
 * without performing the operation and crashing later, or writing objects by
 * guessed offsets. So there is no create slot to return OK from. Reading a
 * static lookup table is safe and provable; everything else about tracers is
 * not, and the ABI says so by having no words for it.
 *
 * Every slot returns WOTBMOD_V3_E_NOT_SUPPORTED until the backend lands.
 * Permission: reuses the existing REVIEWED `visible.projectile.events`, which
 * already names `native_tracer_style` as its outstanding partial binding.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define WOTBMOD_V3_TRACER_VERSION 1u

/* Entries in the table at 0x03FD6168. Valid shell types are 0..24. */
#define WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT 25u
/* Longest observed name is ANTI_TANK_GUIDED_MISSILE, 24 bytes plus NUL. */
#define WOTBMOD_V3_TRACER_STYLE_NAME_SIZE 32u

/*
 * The eight names the table actually contains, as a stable numbering. The
 * NAME is the source of truth: if a future build introduces a ninth style,
 * `style_id` reports UNKNOWN and `style_name` still carries the real string,
 * so a mod degrades instead of silently reading a wrong style. Existing
 * numbers are never reassigned.
 */
typedef enum WotbModV3TracerStyleId {
    WOTBMOD_V3_TRACER_STYLE_UNKNOWN = 0,
    WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING = 1,
    WOTBMOD_V3_TRACER_STYLE_ARMOR_PIERCING_CR = 2,
    WOTBMOD_V3_TRACER_STYLE_HIGH_EXPLOSIVE = 3,
    WOTBMOD_V3_TRACER_STYLE_HOLLOW_CHARGE = 4,
    WOTBMOD_V3_TRACER_STYLE_ANTI_TANK_GUIDED_MISSILE = 5,
    WOTBMOD_V3_TRACER_STYLE_RAILGUN = 6,
    WOTBMOD_V3_TRACER_STYLE_IMPROVED_DETECTION = 7,
    WOTBMOD_V3_TRACER_STYLE_STT_TRACER = 8
} WotbModV3TracerStyleId;

typedef enum WotbModV3TracerStyleFlag {
    /* style_id and style_name were resolved from the table. */
    WOTBMOD_V3_TRACER_STYLE_NAME_VALID = 1u << 0,
    /*
     * color was read from a per-shell record that actually existed. When
     * clear, color is zeroed and carries no meaning. The name comes from a
     * static table and the colour from a runtime record, so the two are
     * independently valid -- a backend that has one never fabricates the other.
     */
    WOTBMOD_V3_TRACER_STYLE_COLOR_VALID = 1u << 1
} WotbModV3TracerStyleFlag;

typedef struct WotbModV3TracerStyle {
    uint32_t struct_size;
    uint32_t api_version;
    /* Echoed back so a copied record stays self-describing. */
    uint32_t shell_type;
    uint32_t style_id;
    uint32_t flags;
    uint32_t reserved;
    /* Stock default RGBA from record+0x34. */
    WotbModV3Color color;
    char style_name[WOTBMOD_V3_TRACER_STYLE_NAME_SIZE];
} WotbModV3TracerStyle;

typedef struct WotbModV3TracerApiV1 {
    uint32_t struct_size;
    uint32_t api_version;

    /*
     * The table length the running client actually has. A mod iterates against
     * this, not against WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT, so a client whose
     * table is a different size is a smaller loop rather than an overrun.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* get_shell_type_count)(
        WotbModV3Handle mod,
        uint32_t* out_count);
    /*
     * shell_type at or above the reported count is
     * WOTBMOD_V3_E_INVALID_ARGUMENT. It is never clamped and never falls back
     * to a default style.
     */
    WotbModV3Result(WOTBMOD_V3_CALL* get_style_for_shell_type)(
        WotbModV3Handle mod,
        uint32_t shell_type,
        WotbModV3TracerStyle* out_style);
} WotbModV3TracerApiV1;

#ifdef __cplusplus
}
#endif
