#include "ges_schemas.h"

#include <cstring>

namespace wotbmod {
namespace v3 {
namespace {

/*
 * Only entries proven by decompiling the publish site AND seen live go here.
 * Avatar::CameraModeChanged: the publisher at 0x015FF9B0 (11.20.0.887) builds
 * {int32 mode; uint8 flag} on its stack (v19[0] = a2; LOBYTE(v19[1]) = a3);
 * 0 = SNIPER, non-zero = ARCADE (re_anchors.md, corrected 2026-08-15). The
 * byte at +4 has no proven meaning yet and is exposed only as a bool.
 */
const GesSchemaField kCameraModeChangedFields[] = {
    {"mode", 0u, WOTBMOD_V3_GES_FIELD_I32, 4u},
    {"flag", 4u, WOTBMOD_V3_GES_FIELD_BOOL, 1u},
};

/*
 * Batch of 2026-09-04, client 11.20.0.887: every entry below comes from the
 * decompiled inline publish loop (GetListeners(&typeid(T)) followed by
 * _Do_call(node, &event)); the size is the stack object handed to _Do_call
 * and the fields are what the publisher wrote into it. None of these has
 * been seen live yet - the loader's SIZE_KNOWN/SCHEMA_KNOWN flags say "the
 * layout is known", not "the meaning is". Field names with no better source
 * are "flag"/"value*"; a size-1 entry with no fields is a struct the publisher
 * only zero-initialised (an empty event or a bool nobody sets).
 *
 *   PlayerRespawned      sub_157CA00: LOBYTE(a2) = *a2           -> {bool}
 *   DirectShootReady     sub_16D78E0: LOBYTE(a2) = *a2           -> {bool}
 *   ArenaFreeze          sub_112A790: LOBYTE(a2) = *a2           -> {bool}
 *   PeriodBattleFinished sub_15A81B0: v14[0] = 1 / 0             -> {bool}
 *   HUDLayer::ChangeVisibility sub_16D7840: v11[0] = 0           -> {bool}
 *   BattleStatusUpdated  sub_157C5C0: v12 = *a2                  -> {int32}
 *   HighlightTankChanged sub_15B1EA0: v15 = a2 ? id(a2) : 0      -> {int32}
 *   VehicleExploded      sub_1670C50: v20 = this                 -> {ptr}
 *   Session::RoundFinished sub_15A6E10: v12 = {this[1], this[2]} -> {int32, int32}
 *   InputModeChanged     sub_122E0F0: v23[0] = mode byte         -> {uint8}
 *   PlayerDied sub_15AD0A0, AmmoExpanded sub_1626B70, PeriodBattleStarted
 *   sub_15A8530, UnlockZoom sub_162EAA0: one zeroed byte, no fields.
 */
const GesSchemaField kBoolFlagFields[] = {
    {"flag", 0u, WOTBMOD_V3_GES_FIELD_BOOL, 1u},
};
const GesSchemaField kDirectShootReadyFields[] = {
    {"ready", 0u, WOTBMOD_V3_GES_FIELD_BOOL, 1u},
};
const GesSchemaField kArenaFreezeFields[] = {
    {"frozen", 0u, WOTBMOD_V3_GES_FIELD_BOOL, 1u},
};
const GesSchemaField kVisibilityFields[] = {
    {"visible", 0u, WOTBMOD_V3_GES_FIELD_BOOL, 1u},
};
const GesSchemaField kBattleStatusUpdatedFields[] = {
    {"status", 0u, WOTBMOD_V3_GES_FIELD_I32, 4u},
};
const GesSchemaField kHighlightTankChangedFields[] = {
    {"id", 0u, WOTBMOD_V3_GES_FIELD_I32, 4u},
};
const GesSchemaField kVehicleExplodedFields[] = {
    {"source", 0u, WOTBMOD_V3_GES_FIELD_PTR, 4u},
};
const GesSchemaField kRoundFinishedFields[] = {
    {"value0", 0u, WOTBMOD_V3_GES_FIELD_I32, 4u},
    {"value1", 4u, WOTBMOD_V3_GES_FIELD_I32, 4u},
};
const GesSchemaField kInputModeChangedFields[] = {
    {"mode", 0u, WOTBMOD_V3_GES_FIELD_U8, 1u},
};

const GesSchema kSchemas[] = {
    {"Avatar::CameraModeChanged", 8u, kCameraModeChangedFields, 2u},
    {"Avatar::PlayerDied", 1u, nullptr, 0u},
    {"Avatar::PlayerRespawned", 1u, kBoolFlagFields, 1u},
    {"Avatar::DirectShootReady", 1u, kDirectShootReadyFields, 1u},
    {"Avatar::ArenaFreeze", 1u, kArenaFreezeFields, 1u},
    {"Avatar::PeriodBattleStarted", 1u, nullptr, 0u},
    {"Avatar::PeriodBattleFinished", 1u, kBoolFlagFields, 1u},
    {"Avatar::AmmoExpanded", 1u, nullptr, 0u},
    {"Avatar::UnlockZoom", 1u, nullptr, 0u},
    {"Avatar::BattleStatusUpdated", 4u, kBattleStatusUpdatedFields, 1u},
    {"Avatar::HighlightTankChanged", 4u, kHighlightTankChangedFields, 1u},
    {"Avatar::VehicleExploded", 4u, kVehicleExplodedFields, 1u},
    {"HUDLayer::ChangeVisibility", 1u, kVisibilityFields, 1u},
    {"InputMode::InputModeChanged", 1u, kInputModeChangedFields, 1u},
    {"Session::RoundFinished", 8u, kRoundFinishedFields, 2u},
    /* 2026-09-05: BattleController::Update (sub_15B8330, defined with
     * create_function) publishes it right after its turret/camera update
     * (sub_15B8C10) with `char v44 = 0` as the event - a one-byte "positions
     * changed" tick, no coordinates inside. Seen live ~1000 times per battle. */
    {"Avatar::TurretAndCameraPositionsChanged", 1u, nullptr, 0u},
};

const uint32_t kSchemaCount =
    static_cast<uint32_t>(sizeof(kSchemas) / sizeof(kSchemas[0]));

bool Append(char* out, size_t capacity, size_t* used,
            const char* text, size_t length) {
    if (*used + length + 1u > capacity) return false;
    std::memcpy(out + *used, text, length);
    *used += length;
    out[*used] = '\0';
    return true;
}

}  // namespace

bool GesTypeNameFromMangled(const char* mangled, char* out, size_t capacity) {
    if (!mangled || !out || capacity == 0u) return false;
    out[0] = '\0';
    static const char kPrefix[] = ".?AU";
    static const char kSuffix[] = "@GES@@";
    const size_t length = std::strlen(mangled);
    const size_t prefix = sizeof(kPrefix) - 1u;
    const size_t suffix = sizeof(kSuffix) - 1u;
    if (length <= prefix + suffix ||
        std::strncmp(mangled, kPrefix, prefix) != 0 ||
        std::strcmp(mangled + length - suffix, kSuffix) != 0) {
        return false;
    }
    /* Segments between prefix and suffix are innermost-first: Name@Owner@... */
    const char* begin = mangled + prefix;
    const char* end = mangled + length - suffix;
    const char* segments[8] = {};
    size_t lengths[8] = {};
    size_t count = 0u;
    const char* cursor = begin;
    while (cursor < end) {
        const char* at = static_cast<const char*>(
            std::memchr(cursor, '@', static_cast<size_t>(end - cursor)));
        const char* segment_end = at ? at : end;
        if (segment_end == cursor || count == 8u) return false;
        segments[count] = cursor;
        lengths[count] = static_cast<size_t>(segment_end - cursor);
        ++count;
        cursor = segment_end + 1u;
    }
    if (count < 2u) return false;
    size_t used = 0u;
    for (size_t i = count; i-- > 0u;) {
        if (!Append(out, capacity, &used, segments[i], lengths[i])) {
            out[0] = '\0';
            return false;
        }
        if (i != 0u && !Append(out, capacity, &used, "::", 2u)) {
            out[0] = '\0';
            return false;
        }
    }
    return true;
}

bool GesTopicFromTypeName(const char* type_name, char* out, size_t capacity) {
    if (!type_name || !out || capacity == 0u) return false;
    size_t used = 0u;
    out[0] = '\0';
    if (!Append(out, capacity, &used, WOTBMOD_V3_GES_TOPIC_PREFIX,
                sizeof(WOTBMOD_V3_GES_TOPIC_PREFIX) - 1u)) {
        return false;
    }
    for (const char* p = type_name; *p; ++p) {
        if (p[0] == ':' && p[1] == ':') {
            if (!Append(out, capacity, &used, ".", 1u)) {
                out[0] = '\0';
                return false;
            }
            ++p;
        } else if (!Append(out, capacity, &used, p, 1u)) {
            out[0] = '\0';
            return false;
        }
    }
    return used > sizeof(WOTBMOD_V3_GES_TOPIC_PREFIX) - 1u;
}

bool GesTypeNameFromTopic(const char* topic, char* out, size_t capacity) {
    if (!topic || !out || capacity == 0u) return false;
    out[0] = '\0';
    const size_t prefix = sizeof(WOTBMOD_V3_GES_TOPIC_PREFIX) - 1u;
    if (std::strncmp(topic, WOTBMOD_V3_GES_TOPIC_PREFIX, prefix) != 0) {
        return false;
    }
    size_t used = 0u;
    for (const char* p = topic + prefix; *p; ++p) {
        if (*p == '.') {
            if (!Append(out, capacity, &used, "::", 2u)) {
                out[0] = '\0';
                return false;
            }
        } else if (!Append(out, capacity, &used, p, 1u)) {
            out[0] = '\0';
            return false;
        }
    }
    return used > 0u;
}

const GesSchema* GesFindSchema(const char* type_name, uint32_t* out_schema_id) {
    if (out_schema_id) *out_schema_id = 0u;
    if (!type_name) return nullptr;
    for (uint32_t i = 0u; i < kSchemaCount; ++i) {
        if (std::strcmp(kSchemas[i].type_name, type_name) == 0) {
            if (out_schema_id) *out_schema_id = i + 1u;
            return &kSchemas[i];
        }
    }
    return nullptr;
}

const GesSchema* GesSchemaById(uint32_t schema_id) {
    if (schema_id == 0u || schema_id > kSchemaCount) return nullptr;
    return &kSchemas[schema_id - 1u];
}

}  // namespace v3
}  // namespace wotbmod
