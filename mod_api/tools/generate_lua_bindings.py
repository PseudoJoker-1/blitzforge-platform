#!/usr/bin/env python3
"""Generate the broad Lua bridge from the structural V3 API model.

Hand-written storage/events/UI/GES functions remain authoritative for the 51 slots
that established the public convention.  Every other current slot is emitted
here so header evolution produces a reviewable coverage failure instead of a
silent hole.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

from lua_api_model import (
    ABI_PREFIX,
    Constant,
    DataField,
    HeaderModel,
    INTERFACES,
    Slot,
)


CUSTOM_SLOTS = {
    *(f"storage.{name}" for name in (
        "get_json", "set_json", "get_bytes", "set_bytes", "erase", "contains",
        "flush", "begin_transaction", "transaction_set_json",
        "transaction_set_bytes", "transaction_erase", "commit", "rollback",
        "get_path")),
    *(f"events.{name}" for name in (
        "subscribe", "unsubscribe", "set_priority", "post", "stop_propagation",
        "get_dispatch_info", "get_thread", "get_timestamp", "get_context")),
    *(f"ui.{name}" for name in (
        "control_create", "control_destroy", "control_add_child",
        "control_remove_child", "control_set_id", "control_find_by_id",
        "control_is_alive", "control_set_text", "control_set_visible",
        "control_set_position", "control_get_position", "control_set_size",
        "control_get_size", "control_set_anchor", "control_set_pivot",
        "slot_find", "slot_attach", "slot_detach")),
    # wotb.ges is hand-written (lua_bind_ges.cpp): every read_* slot takes the
    # engine's delivery-scoped event pointer, which only the delivering callback
    # may hand to Lua, and publish() encodes a table against a schema.
    *(f"ges.{name}" for name in (
        "list_types", "read_i32", "read_u32", "read_f32", "read_bool",
        "read_ptr", "read_cstring", "get_schema", "schema_field", "publish")),
}

# These operations fundamentally require a native code/data pointer. Exposing a
# lightuserdata-shaped imitation would reintroduce FFI through the back door.
NATIVE_ONLY = {
    "unsafe_native.create_address_hook",
    "intermod.export_interface",
}

# Successful calls in these groups change ownership without returning a new
# handle. Keeping the parameter name explicit makes an ABI rename or a new
# release-shaped slot fail review instead of being guessed from English verbs.
RETAIN_INPUTS = {
    "handles.retain": "handle",
    "resources.retain": "resource",
}

GENERIC_RELEASE_INPUTS = {
    "handles.release": "handle",
    "hooks.remove": "hook",
    "ui.style_pop": "override_handle",
    "input.unregister_action": "action",
    "vfs.unmount": "mount",
    "resources.release": "resource",
    "intermod.unexport_interface": "export_token",
    "render.destroy_texture": "texture",
    "render.destroy_material": "material",
    "scene.entity_destroy": "entity",
    "audio.destroy": "audio",
    "audio.sound_override_unregister": "token",
    "audio.sound_event_destroy": "event",
    "vehicle_visual.profile_release": "profile",
    "vehicle_visual.skin_pack_release": "pack",
    "projectile.tracer_style_unregister": "style",
    "projectile.impact_visual_unregister": "visual",
    "devtools.span_end": "span",
}

CALLBACK_RELEASE_INPUTS = {
    "capabilities.unsubscribe": "token",
    "lifecycle.unregister_cleanup": "token",
    "ui.event_unsubscribe": "token",
    "settings.unsubscribe": "token",
    "intermod.message_unsubscribe": "token",
    "render.unregister_callback": "token",
    "camera.remove_modifier": "token",
    "audio.unsubscribe": "token",
    "entity_public.unsubscribe_public_property": "token",
    "bigworld_rpc.unsubscribe_observed": "token",
}

# A generated table is guarded as one family, while several families contain
# native operations protected by independent runtime permissions. The native
# runtime sees this host's aggregate mod handle, not the individual Lua script,
# so the inner fence must conservatively require every permission used anywhere
# in that family. Otherwise one script grant could borrow a different grant
# held by the host. The first name is the stable denial label shown to Lua.
PERMISSIONS = {
    "core": ("core",),
    "capabilities": ("core",),
    "permissions": ("core",),
    "handles": ("core",),
    "lifecycle": ("core",),
    "hooks": ("hooks.symbol",),
    "unsafe_native": ("native.hook.address",),
    "events": ("events.public",),
    "ui": ("ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"),
    "settings": ("settings",),
    "storage": ("storage",),
    "input": ("input.actions",),
    # NOT extended with "resources.write.mod_data" for VFS V2's four write
    # routes, and that is deliberate: SetFuncsGuardedAll ORs this tuple into one
    # mask and PermissionGate answers AllowsAllBits, so every name listed here
    # is required by EVERY function in the table. Adding the write permission
    # would make it a precondition for `vfs.read` too and break every read-only
    # consumer. The write routes gate themselves natively, per function, in
    # VfsWriteAccess - which is the level the distinction actually lives at.
    "vfs": ("resources.mod", "resources.overlay.game"),
    "resources": ("resources.mod",),
    "async": ("core",),
    "http": ("network.http",),
    "intermod": ("core",),
    "render": ("battle.render.overlay", "render.callbacks"),
    "render_native": ("render.native",),
    "camera": ("camera.battle.read", "camera.hangar", "camera.replay",
               "gameplay.tweak.camera"),
    "scene": ("hangar.scene", "resources.mod"),
    "audio": ("audio.custom", "audio.events"),
    "vehicle_visual": ("gameplay.tweak.vehicle", "vehicle.local.cosmetic",
                       "game.entity.public", "resources.mod",
                       "resources.overlay.game"),
    "gameplay_camera": ("gameplay.tweak.camera", "gameplay.tweak.freecam"),
    "gameplay_hud": ("gameplay.tweak.hud", "battle.ui"),
    "gameplay_hangar": ("gameplay.tweak.hangar", "hangar.scene"),
    "gameplay_replay": ("gameplay.tweak.replay",),
    "entity_public": ("entity.public.visible", "game.entity.public"),
    "bigworld_rpc": ("bigworld.rpc.observe", "bigworld.observe",
                     "bigworld.rpc.metadata"),
    "projectile": ("gameplay.tweak.projectile_visual",
                   "visible.projectile.events"),
    "yaml": ("resources.mod",),
    "archive": ("resources.mod",),
    "loaders": ("resources.mod",),
    "client": ("client.leave_to_hangar",),
    "device": ("core",),
    "diagnostics": ("core",),
    "devtools": ("core",),
    "manifest": ("content",),
    "catalog": ("content",),
    "content": ("content", "resources.mod", "resources.overlay.game"),
    # Post-RC1 contract, 2026-08-16. Every one of these reuses an existing
    # permission name; the RC1 vocabulary of 51 is unchanged. Adding a
    # permission is a larger commitment than adding a slot, and none of these
    # five discloses or mutates anything the reused grant does not already
    # cover.
    #   ui_read          reads back the loader's own mirror of what a mod wrote
    #   camera_state     read-only, same disclosure as the camera read grant
    #   audio_intercept  the grant that already governs sound_override_register
    #   scene_enumerate  discloses identity and world placement of game-owned
    #                    objects -- the same disclosure class as public entities
    #   tracer           read-only stock style lookup, no creation
    "ui_read": ("ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"),
    "camera_state": ("camera.battle.read",),
    "audio_intercept": ("audio.events",),
    "scene_enumerate": ("game.entity.public",),
    "tracer": ("visible.projectile.events",),
    "ges": ("ges.observe",),
    # change() checks session.cluster.change natively, per call; the family
    # gate asks only for the read grant so enumerate/get_current stay usable.
    "session_cluster": ("session.cluster.read",),
}

INTEGER_TYPES = {
    "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t",
    "uint64_t", "int64_t", "size_t", "WotbModV3YamlNodeId",
}
NUMBER_TYPES = {"float", "double"}

# A counted array - in either direction - is never an array of these. `char` is
# a string: the ABI spells a string out-parameter `char* buffer, uint32_t*
# inout_size` and `const char* text` for one going in, and both shapes are
# claimed by earlier rules. `void` is opaque by construction and has no element
# size at all. Treating either as an element type turns a plain string argument
# into a phantom array, which is exactly how settings.get_bool
# (`const char* key, uint32_t* out_value`) came to be emitted as an array of
# chars whose binding dropped the key.
NON_ELEMENT_TYPES = {"char", "void"}

# C cannot say which of two `uint32_t*` parameters is in-out, so the ABI says it
# in the name. Every counted array in these headers spells its count
# `inout_count`: the caller writes the capacity in and the client writes the
# length back out, which is the only reason the emitter's two-pass probe (call
# once with a null array to learn the count, then again with a buffer that big)
# means anything. A `uint32_t*` spelled `out_...` is a scalar result, and there
# is nothing for a second pass to do. This mirrors the `out_` convention the
# emitter already trusts for single outputs in emit_sized_string_slot,
# emit_input_array_slot and emit_direct_slot.
COUNT_PARAMETER_PREFIX = "inout_"

# A newest interface table embeds its older binary prefixes. Struct arguments
# keep the version of the header that declared the struct; using the newest
# table version makes the real client reject an otherwise valid V2 payload.
# UI V3 is the first interface where this distinction is observable in Lua:
# style_push lives in the embedded V2 table, while control_get_snapshot is V3.
STRUCT_VERSION_MACROS = {
    "WotbModV3UiControlDescriptor": "WOTBMOD_V3_UI_VERSION",
    "WotbModV3UiLayoutDescriptor": "WOTBMOD_V3_UI_VERSION",
    "WotbModV3UiStylePatch": "WOTBMOD_V3_UI_VERSION",
    "WotbModV3UiEvent": "WOTBMOD_V3_UI_VERSION",
    "WotbModV3UiChoiceDescriptor": "WOTBMOD_V3_UI_VERSION",
    "WotbModV3UiDialogDescriptor": "WOTBMOD_V3_UI_VERSION",
}


# ---------------------------------------------------------------------------
# Constants.
#
# Interface.header names the NEWEST header of an interface, which is not the
# same thing as the set of headers its constants live in: ui_v2.h alone holds
# 52 of the 57 UI enumerators, and mapping only the newest header silently
# drops them. Every superseded header that still declares constants is listed
# here, next to the interface whose Lua table those constants belong on.
LEGACY_CONSTANT_HEADERS = {
    "ui_v2.h": "ui",
    "vehicle_visual_v1.h": "vehicle_visual",
    # vfs_v2.h adds four write slots and declares two constants of its own; all
    # SEVEN of the VFS enumerators - entry kinds, mount kinds, change kinds -
    # are still declared in v1. Without this line they reach no table, and
    # `wotb.vfs.MOUNT_OVERLAY` stops existing for every mod that already reads
    # it. Exactly the drop the comment above warns about.
    "vfs_v1.h": "vfs",
    "projectile_v1.h": "projectile",
    "devtools_v1.h": "devtools",
    "devtools_v2.h": "devtools",
    "diagnostics_v1.h": "diagnostics",
}

# render_v1.h is claimed by two interfaces: INTERFACES declares both `render`
# and `render_native` against it. Its six enums (backend, phase, resource type,
# texture format, parameter type, lifecycle change) describe the portable
# render surface, so all of them go on `render` and none on `render_native` -
# render_native is the three-slot escape hatch for a native device pointer and
# has no vocabulary of its own. A constant with two homes is a constant whose
# spelling a script has to guess at.
CONSTANT_HEADER_OWNERS = {
    interface.header: interface.lua_name
    for interface in INTERFACES
    if interface.lua_name != "render_native"
}
CONSTANT_HEADER_OWNERS.update(LEGACY_CONSTANT_HEADERS)

# base.h declares 93 constants and belongs to no interface, so each group is
# placed - or refused - by hand rather than by a rule.
#
# Placed:
#   WotbModV3ThreadRole      -> events. Not a new home: lua_bind_events.cpp
#                               already publishes THREAD_* there because the
#                               thread_role an event carries is one of these.
#                               Generating them keeps that table honest.
#   WotbModV3CapabilityStatus-> capabilities. It is the value capabilities.query
#                               reports and it has no other home anywhere.
#   WotbModV3PermissionTier  -> permissions. Same argument: the tier of a
#                               permission is what permissions.* talks about.
#
# Refused, deliberately:
#   WotbModV3Result     - the whole Lua convention is `return nil, message`.
#                         Publishing numeric result codes invites
#                         `if err == wotb.core.E_TIMEOUT`, which is a second,
#                         weaker error protocol competing with the first.
#   WotbModV3HandleType - Lua never sees a raw handle type. Handles arrive as
#                         typed userdata and the host checks the type; a script
#                         comparing against a numeric type tag would be reading
#                         a fact it cannot obtain.
#   WotbModV3GameContext- already published as wotb.context.HANGAR/BATTLE/...
#                         by the Lua context library in lua_bindings.cpp. A
#                         second spelling of the same value is exactly the
#                         confusion this generator exists to remove.
#   base.h's own #defines - WOTBMOD_V3_INVALID_HANDLE is a raw handle value,
#                         and the MAX_* buffer sizes are host marshalling
#                         limits rather than limits a script can hit. The one
#                         a script can hit, the path length, is reachable as
#                         wotb.vfs.URI_MAX (defined as WOTBMOD_V3_MAX_PATH).
UNOWNED_CONSTANT_HEADERS = {"base.h", "bootstrap.h", "interface_ids.h"}
BASE_CONSTANT_HOMES = {
    "WotbModV3ThreadRole": "events",
    "WotbModV3CapabilityStatus": "capabilities",
    "WotbModV3PermissionTier": "permissions",
}

# Event topics are strings passed to events.subscribe, and the ABI declares
# them wherever the feature that raises them lives: projectile_v1.h and
# projectile_v2.h between them declare seven, resources_v1.h two, vfs_v1.h one,
# lifecycle_v1.h six. They are still events, and the hand-written layer already
# homes four of them on wotb.events (TOPIC_LOCAL_SHELL_FIRED,
# TOPIC_PROJECTILE_*). This is the one place a constant's home is decided by
# its name rather than by its header, and it is narrow on purpose: a
# WOTBMOD_V3_EVENT_* define is a topic string by ABI convention, and a
# non-string one is treated as a header bug rather than guessed at.
EVENT_TOPIC_PREFIX = "WOTBMOD_V3_EVENT_"

# Size caps are limits, not vocabulary: WOTBMOD_V3_STORAGE_KEY_MAX tells a
# script how long a key may be, it does not name one of a closed set of values.
# They are opted in one at a time, with the Lua name written out here, so that
# a new limit in a header is inert until somebody decides a script should see
# it - and so that a limit cannot arrive on a table as a side effect of a
# prefix rule. Any numeric define whose name contains MAX and is not listed
# here is excluded and reported.
LIMIT_NAME_TOKENS = ("MAX",)
SIZE_LIMITS = {
    "WOTBMOD_V3_MAX_TASK_RESULT": ("async", "MAX_TASK_RESULT"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_ENTRIES": ("diagnostics", "MAX_CONTEXT_ENTRIES"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMBS": ("diagnostics", "MAX_BREADCRUMBS"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_KEY": ("diagnostics", "MAX_CONTEXT_KEY"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_CONTEXT_VALUE": ("diagnostics", "MAX_CONTEXT_VALUE"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_ACTION": ("diagnostics", "MAX_ACTION"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CATEGORY": ("diagnostics", "MAX_BREADCRUMB_CATEGORY"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_MESSAGE": ("diagnostics", "MAX_BREADCRUMB_MESSAGE"),
    "WOTBMOD_V3_DIAGNOSTICS_MAX_BREADCRUMB_CONTEXT": ("diagnostics", "MAX_BREADCRUMB_CONTEXT"),
    # Opted in because a caller CAN hit it and can do something about it: this
    # is the ceiling on one write and on a composed file's total size, so a mod
    # composing an overlay checks against it instead of discovering it as a
    # refusal halfway through.
    "WOTBMOD_V3_VFS_MAX_WRITE_BYTES": ("vfs", "MAX_WRITE_BYTES"),
    "WOTBMOD_V3_MAX_EVENT_TOPIC": ("events", "MAX_TOPIC"),
    "WOTBMOD_V3_MAX_EVENT_PAYLOAD": ("events", "MAX_PAYLOAD"),
    "WOTBMOD_V3_MAX_HOOK_TARGET": ("hooks", "MAX_TARGET"),
    "WOTBMOD_V3_HTTP_DEFAULT_MAX_RESPONSE": ("http", "DEFAULT_MAX_RESPONSE"),
    "WOTBMOD_V3_HTTP_ABSOLUTE_MAX_RESPONSE": ("http", "ABSOLUTE_MAX_RESPONSE"),
    "WOTBMOD_V3_INPUT_ACTION_ID_MAX": ("input", "ACTION_ID_MAX"),
    "WOTBMOD_V3_INPUT_BINDINGS_MAX": ("input", "BINDINGS_MAX"),
    "WOTBMOD_V3_MAX_SERVICE_ID": ("intermod", "MAX_SERVICE_ID"),
    "WOTBMOD_V3_MAX_MESSAGE_TOPIC": ("intermod", "MAX_MESSAGE_TOPIC"),
    "WOTBMOD_V3_MAX_INTERMOD_PAYLOAD": ("intermod", "MAX_PAYLOAD"),
    "WOTBMOD_V3_SETTING_KEY_MAX": ("settings", "KEY_MAX"),
    "WOTBMOD_V3_SETTING_TEXT_MAX": ("settings", "TEXT_MAX"),
    "WOTBMOD_V3_STORAGE_KEY_MAX": ("storage", "KEY_MAX"),
    "WOTBMOD_V3_VEHICLE_SKIN_MAX_ASSETS": ("vehicle_visual", "SKIN_MAX_ASSETS"),
    "WOTBMOD_V3_VFS_URI_MAX": ("vfs", "URI_MAX"),
    # Post-RC1 contract, 2026-08-16. A scene walk is bounded in three
    # independent directions because the child vector is mutated by a lockless
    # memmove and RemoveNode Releases the child immediately afterwards, so an
    # unbounded walk is not merely slow, it is a use-after-free waiting for a
    # mistimed frame. A script needs these to size its own buffer.
    "WOTBMOD_V3_SCENE_WALK_MAX_DEPTH": ("scene_enumerate", "WALK_MAX_DEPTH"),
    "WOTBMOD_V3_SCENE_WALK_MAX_CHILDREN": ("scene_enumerate", "WALK_MAX_CHILDREN"),
    "WOTBMOD_V3_SCENE_WALK_MAX_NODES": ("scene_enumerate", "WALK_MAX_NODES"),
    "WOTBMOD_V3_SCENE_NODE_NAME_SIZE": ("scene_enumerate", "NODE_NAME_SIZE"),
    "WOTBMOD_V3_TRACER_STYLE_NAME_SIZE": ("tracer", "STYLE_NAME_SIZE"),
    "WOTBMOD_V3_TRACER_SHELL_TYPE_COUNT": ("tracer", "SHELL_TYPE_COUNT"),
}

# How a C name becomes a Lua name, per interface, as (prefix, replacement)
# pairs applied after WOTBMOD_V3_ comes off. Longest matching prefix wins.
#
# Per interface, never global: WOTBMOD_V3_CAMERA_MODE_* is camera_v1.h while
# WOTBMOD_V3_CAMERA_TRANSITION_* is gameplay_camera_v1.h. The same C prefix,
# two different tables, and no global rule can tell them apart. Never derived
# from the lua_name either - `content` would derive CONTENT_ and turn
# WOTBMOD_V3_CONTENT_AUDIO into a bare AUDIO that no longer says what it is.
#
# The replacements are empty (a plain strip) except where something forces a
# rename, and each of those is called out below. Where a hand-written spelling
# already exists - lua_bind_events.cpp and lua_bind_ui.cpp - it wins outright,
# because scripts and the shipped examples are written against it.
CONSTANT_PREFIXES = {
    # WotbModV3LogLevel keeps LOG_. Stripping it yields wotb.core.ERROR, sat
    # next to Lua's own `error`, and a bare wotb.core.DEBUG/INFO/FATAL says
    # nothing about being a log level.
    "core": (),
    "capabilities": (("CAPABILITY_", "STATUS_"),),
    "permissions": (("PERMISSION_STATE_", "STATE_"), ("PERMISSION_", "TIER_")),
    "handles": (),
    "lifecycle": (("MOD_", ""),),
    "hooks": (
        ("HOOK_STATUS_", "STATUS_"),
        ("HOOK_CREATE_", "CREATE_"),
        ("HOOK_", "MODE_"),
    ),
    "unsafe_native": (),
    # The 22 hard collisions. events_v1.h declares a topic STRING and a
    # client-event-type ENUMERATOR for the same 22 concepts (BATTLE_STARTED,
    # UI_INPUT, SHOT_FIRED, ...). lua_bind_events.cpp already separates them
    # with two disjoint prefixes and this reproduces exactly that: TOPIC_ for
    # the string you subscribe with, TYPE_ for the enum a typed payload
    # carries. The generator refuses to emit a duplicate name on one table, so
    # collapsing these two back into one prefix fails the build rather than
    # shipping a table where one of the 22 silently shadows the other.
    "events": (
        ("CLIENT_EVENT_", "TYPE_"),
        ("EVENT_PRIORITY_", "PRIORITY_"),
        ("EVENT_FLAG_", "FLAG_"),
        ("EVENT_", "TOPIC_"),
    ),
    # ui_v2.h declares two alignment enums. WotbModV3UiTextAlignment
    # (UI_TEXT_ALIGN_LEFT/CENTER/RIGHT/JUSTIFY) is the one lua_bind_ui.cpp
    # publishes as ALIGN_*, and shipped examples use those names, so ALIGN_
    # stays where it is. WotbModV3UiAlignment (UI_ALIGN_START/CENTER/END/
    # STRETCH) is the layout cross-axis alignment; it would collide on
    # ALIGN_CENTER, so it takes the unambiguous LAYOUT_ALIGN_ instead - it is
    # the alignment a layout applies, and it sits next to LAYOUT_FLEX and
    # LAYOUT_GRID from WotbModV3UiLayoutType.
    "ui": (
        ("UI_TEXT_ALIGN_", "ALIGN_"),
        ("UI_ALIGN_", "LAYOUT_ALIGN_"),
        ("UI_", ""),
    ),
    "settings": (("SETTING_FLAG_", "FLAG_"), ("SETTING_", "TYPE_")),
    "storage": (("STORAGE_", ""),),
    "input": (("INPUT_", ""),),
    "vfs": (("VFS_", ""),),
    "resources": (("RESOURCE_", ""),),
    "async": (),
    "http": (("HTTP_", ""),),
    "intermod": (),
    "render": (("RENDER_", ""),),
    "render_native": (),
    "camera": (("CAMERA_", ""),),
    "scene": (("SCENE_", ""),),
    "audio": (("AUDIO_", ""),),
    "vehicle_visual": (("VEHICLE_", ""),),
    "gameplay_camera": (("CAMERA_", ""),),
    "gameplay_hud": (("HUD_", ""),),
    "gameplay_hangar": (("HANGAR_", ""),),
    "gameplay_replay": (("REPLAY_", ""),),
    "entity_public": (
        ("PUBLIC_ENTITY_REASON_", "REASON_"),
        ("PUBLIC_ENTITY_", "TYPE_"),
        ("PUBLIC_VALUE_", "VALUE_"),
    ),
    "bigworld_rpc": (),
    "projectile": (("PROJECTILE_", ""),),
    "yaml": (("YAML_", ""),),
    "archive": (),
    "loaders": (("LOADER_", ""),),
    "client": (("CLIENT_", ""),),
    # WotbModV3ProcessorArchitecture keeps ARCH_. Stripping it yields
    # wotb.device.UNKNOWN/X86/X64/ARM32/ARM64, and a bare UNKNOWN on a table
    # that also reports memory and screen size is unreadable.
    "device": (),
    "diagnostics": (("DIAGNOSTIC_", ""),),
    "devtools": (),
    "manifest": (),
    "catalog": (("CATALOG_", "STATUS_"),),
    "content": (("CONTENT_", "OVERRIDE_"),),
    # Post-RC1 contract, 2026-08-16.
    "ui_read": (("UI_READ_", ""),),
    # Two independent families on one table, and they must stay
    # distinguishable: ANIMATION_* is the 7-value animation-controller state
    # machine at CameraController+0x5C, VIEW_* is the separate 2-valued
    # arcade/sniper index. Conflating them is exactly the mistake the older
    # anchor note made. Note VIEW_SNIPER is 0, not 1 -- the previously
    # published mapping was inverted; see re_anchors.md.
    "camera_state": (
        ("CAMERA_ANIMATION_", "ANIMATION_"),
        ("CAMERA_VIEW_", "VIEW_"),
    ),
    "audio_intercept": (("SOUND_INTERCEPT_", ""),),
    "scene_enumerate": (
        ("SCENE_NODE_", "NODE_"),
        ("SCENE_WALK_", "WALK_"),
    ),
    "tracer": (("TRACER_STYLE_", "STYLE_"),),
    "ges": (("GES_EVENT_", "EVENT_"), ("GES_FIELD_", "FIELD_"), ("GES_PUBLISH_", "PUBLISH_"), ("GES_", "")),
    "session_cluster": (("CLUSTER_CHANGE_", "CHANGE_"), ("SESSION_CLUSTER_", "")),
}


class ConstantConflict(Exception):
    """Two constants want the same name on one wotb.* table."""


def constant_lua_name(interface: str, c_name: str) -> str:
    """The Lua spelling of one C constant on one interface's table."""
    if c_name in SIZE_LIMITS:
        return SIZE_LIMITS[c_name][1]
    if not c_name.startswith(ABI_PREFIX):
        raise ValueError(f"constant outside the V3 namespace: {c_name}")
    stem = c_name[len(ABI_PREFIX):]
    best: tuple[str, str] | None = None
    for prefix, replacement in CONSTANT_PREFIXES[interface]:
        if stem.startswith(prefix) and (best is None or len(prefix) > len(best[0])):
            best = (prefix, replacement)
    if best is None:
        return stem
    candidate = best[1] + stem[len(best[0]):]
    # Never strip to nothing, and never to something that is not an
    # identifier: an enum whose only member is its own prefix would otherwise
    # produce wotb.camera[""].
    if not candidate or candidate[0].isdigit():
        return stem
    return candidate


def place_constant(constant: Constant) -> tuple[str | None, str]:
    """Which wotb.* table this constant belongs on, or why it belongs on none.

    Header first, name only where the ABI's own convention makes the name the
    better evidence. Exactly one rule here is name-based and it is the topic
    rule; everything else follows the header the constant is declared in.
    """
    name = constant.c_name
    if name.startswith(EVENT_TOPIC_PREFIX) and constant.kind == "define":
        if not constant.is_string:
            raise ValueError(
                f"{name} is a WOTBMOD_V3_EVENT_ define that is not a topic "
                f"string; decide its home explicitly"
            )
        return "events", ""
    if name in SIZE_LIMITS:
        return SIZE_LIMITS[name][0], ""
    if constant.header in UNOWNED_CONSTANT_HEADERS:
        home = BASE_CONSTANT_HOMES.get(constant.enum_type or "")
        if home:
            return home, ""
        return None, f"{constant.header} belongs to no interface"
    if constant.kind == "define" and any(
        token in name for token in LIMIT_NAME_TOKENS
    ):
        return None, "size limit not opted into SIZE_LIMITS"
    owner = CONSTANT_HEADER_OWNERS.get(constant.header)
    if owner:
        return owner, ""
    return None, f"{constant.header} maps to no interface"


def validate_constant_catalogue(model: HeaderModel) -> None:
    """Fail on a hand-written table that no longer matches the headers.

    Each of these lists is a decision somebody made about a header that exists
    today. A stale entry is silent otherwise: a renamed size cap simply stops
    being published, and a legacy header that gained an interface goes on being
    mapped by hand.
    """
    lua_names = {interface.lua_name for interface in INTERFACES}
    missing_prefixes = lua_names - set(CONSTANT_PREFIXES)
    if missing_prefixes:
        raise ValueError(
            f"CONSTANT_PREFIXES has no entry for {sorted(missing_prefixes)}"
        )
    unknown_prefixes = set(CONSTANT_PREFIXES) - lua_names
    if unknown_prefixes:
        raise ValueError(
            f"CONSTANT_PREFIXES names no such interface: {sorted(unknown_prefixes)}"
        )
    for header, interface in LEGACY_CONSTANT_HEADERS.items():
        if header not in model.sources:
            raise ValueError(f"LEGACY_CONSTANT_HEADERS names a missing header: {header}")
        if interface not in lua_names:
            raise ValueError(f"LEGACY_CONSTANT_HEADERS names no such interface: {interface}")
    declared = {constant.c_name for constant in model.constants}
    for c_name, (interface, _) in SIZE_LIMITS.items():
        if c_name not in declared:
            raise ValueError(f"SIZE_LIMITS names a constant no header declares: {c_name}")
        if interface not in lua_names:
            raise ValueError(f"SIZE_LIMITS names no such interface: {interface}")
    enum_types = {constant.enum_type for constant in model.constants}
    for enum_type, interface in BASE_CONSTANT_HOMES.items():
        if enum_type not in enum_types:
            raise ValueError(f"BASE_CONSTANT_HOMES names a missing enum: {enum_type}")
        if interface not in lua_names:
            raise ValueError(f"BASE_CONSTANT_HOMES names no such interface: {interface}")


def constant_tables(model: HeaderModel) -> dict[str, list[tuple[str, Constant]]]:
    """Every constant, placed and named, grouped by wotb.* table.

    Raises ConstantConflict if one table would receive the same Lua name
    twice. A collision is not a cosmetic problem: whichever assignment runs
    second wins silently, and every script comparing against the loser reads a
    value that is never produced.
    """
    validate_constant_catalogue(model)
    tables: dict[str, list[tuple[str, Constant]]] = {}
    owners: dict[tuple[str, str], Constant] = {}
    conflicts: list[str] = []
    for constant in model.constants:
        interface, _ = place_constant(constant)
        if interface is None:
            continue
        lua_name = constant_lua_name(interface, constant.c_name)
        previous = owners.get((interface, lua_name))
        if previous is not None:
            conflicts.append(
                f"wotb.{interface}.{lua_name}: {previous.c_name} "
                f"({previous.header}) and {constant.c_name} ({constant.header})"
            )
            continue
        owners[(interface, lua_name)] = constant
        tables.setdefault(interface, []).append((lua_name, constant))
    if conflicts:
        raise ConstantConflict(
            "duplicate Lua constant names:\n  " + "\n  ".join(sorted(conflicts))
        )
    return tables


def excluded_constants(model: HeaderModel) -> dict[str, str]:
    """Every parsed constant that reaches no table, and why."""
    excluded: dict[str, str] = {}
    for constant in model.constants:
        interface, reason = place_constant(constant)
        if interface is None:
            excluded[constant.c_name] = reason
    return excluded


def clean_type(c_type: str) -> str:
    return " ".join(c_type.replace(" *", "*").split())


def base_type(c_type: str) -> str:
    value = clean_type(c_type)
    value = re.sub(r"^const\s+", "", value)
    return value[:-1].rstrip() if value.endswith("*") else value


def struct_version_macro(c_type: str, fallback: str) -> str:
    return STRUCT_VERSION_MACROS.get(base_type(c_type), fallback)


def is_handle(c_type: str) -> bool:
    value = base_type(c_type)
    return value == "WotbModV3Token" or value == "WotbModV3EventToken" or (
        value.startswith("WotbModV3") and value.endswith("Handle")
    )


def symbol(slot: Slot) -> str:
    return re.sub(r"\W", "_", f"{slot.interface.lua_name}_{slot.name}")


def is_output_array_pair(params, index: int) -> bool:
    """Is params[index] a counted output array, counted by params[index + 1]?

    Adjacency and the two C types are necessary and nowhere near sufficient;
    see NON_ELEMENT_TYPES and COUNT_PARAMETER_PREFIX for why each of the two
    extra tests is here and what shipped broken without them.
    """
    if index + 1 >= len(params):
        return False
    array, count = params[index], params[index + 1]
    c_type = clean_type(array.c_type)
    if not c_type.endswith("*") or base_type(c_type) in NON_ELEMENT_TYPES:
        return False
    return (
        clean_type(count.c_type) == "uint32_t*"
        and count.name.startswith(COUNT_PARAMETER_PREFIX)
    )


def is_input_array_pair(params, index: int) -> bool:
    """Is params[index] a counted input array, counted by params[index + 1]?"""
    if index + 1 >= len(params):
        return False
    array, count = params[index], params[index + 1]
    c_type = clean_type(array.c_type)
    return (
        c_type.startswith("const ")
        and c_type.endswith("*")
        and base_type(c_type) not in NON_ELEMENT_TYPES
        and clean_type(count.c_type) == "uint32_t"
    )


def is_input_array_field_pair(fields, index: int) -> bool:
    """Is fields[index] a counted input array, counted by fields[index + 1]?

    The same ABI shape as is_input_array_pair - `const T*` immediately
    followed by a `uint32_t` count - one level down, inside a struct rather
    than a parameter list, and it is delegated to that function rather than
    restated so the two can never drift apart. NON_ELEMENT_TYPES comes with
    it: `const char*` next to a `uint32_t flags` is a string beside an
    unrelated number, not an array of characters.

    Three fields in the current headers have this shape:
    WotbModV3SettingPreset.values/value_count,
    WotbModV3InputActionDesc.default_bindings/default_binding_count and
    WotbModV3VehicleSkinPack.assets/asset_count.

    Read as anything other than a pair, they are an out-of-bounds read a
    sandboxed script can ask for: emit_read_field used to arena-allocate a
    single element for the pointer and then read the count out of the script's
    own table, so `{values = {v}, value_count = 512}` handed the client one
    element and told it there were five hundred and twelve. The client dutifully
    walks all of them (data_services.cpp:10754), reading struct_size out of
    memory that was never allocated.

    Two extra tests beyond the parameter-level rule, because a struct field
    list has shapes a parameter list does not:

    * neither field may be a fixed array or a union member. `const T* x[4]` is
      not a pointer to N elements and a union member has no meaningful
      neighbour, so adjacency proves nothing about either.
    * the count's name must end in `count`. Parameter adjacency is an ABI
      calling convention and carries its own meaning; struct adjacency is only
      declaration order, and `const T* thing; uint32_t flags;` would otherwise
      be silently reinterpreted as an array. Reading a genuine single-element
      pointer as an array is the mirror-image bug of the one this closes, and a
      name test costs nothing to keep it out.
    """
    if index + 1 >= len(fields):
        return False
    array, count = fields[index], fields[index + 1]
    if array.array_extent or array.union_members:
        return False
    if count.array_extent or count.union_members:
        return False
    return is_input_array_pair(fields, index) and count.name.endswith("count")


class Generator:
    def __init__(self, model: HeaderModel):
        self.model = model
        self.lines: list[str] = []
        self.unsupported: dict[str, str] = {}

    def line(self, text: str = "") -> None:
        self.lines.append(text)

    def emit_prelude(self) -> None:
        self.lines.extend(r'''// Generated by tools/generate_lua_bindings.py. Do not edit.
#include "wotb_mod_api_v3.h"
#include "loader/lua/lua_bindings.h"
#include "loader/lua/lua_convert.h"
#include "loader/lua/lua_permissions.h"
#include "loader/lua/lua_script.h"

extern "C" {
#include "third_party/lua/lauxlib.h"
}

#include <cstdint>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace wotbmod { namespace lua { namespace generated {

struct Context {
    const void* api;
    const WotbModV3HandlesApiV1* handles;
    WotbModV3Handle mod;
    LuaScript* script;
};

Context* GetContext(lua_State* state) noexcept {
    return static_cast<Context*>(lua_touserdata(state, GuardedUpvalueIndex(1)));
}

int Unsupported(lua_State* state, const char* slot, const char* why) noexcept {
    lua_pushnil(state);
    lua_pushfstring(state, "%s: unavailable from Lua: %s", slot, why);
    return 2;
}

bool ReadInteger(lua_State* state, int index, int64_t* out) noexcept {
    lua_Integer value = 0;
    if (!CheckArgInteger(state, index, &value)) return false;
    *out = static_cast<int64_t>(value);
    return true;
}

bool ReadUnsigned(lua_State* state, int index, uint64_t* out) noexcept {
    int64_t value = 0;
    if (!ReadInteger(state, index, &value)) return false;
    if (value < 0) { PushArgumentError(state, index, "a non-negative integer"); return false; }
    *out = static_cast<uint64_t>(value);
    return true;
}

bool ReadNumber(lua_State* state, int index, double* out) noexcept {
    lua_Number value = 0;
    if (!CheckArgNumber(state, index, &value)) return false;
    *out = static_cast<double>(value);
    return true;
}

bool ReadAnyHandle(lua_State* state, int index, uint64_t* out) noexcept {
    const char* type = HandleTypeName(state, index);
    WotbModV3Handle value = WOTBMOD_V3_INVALID_HANDLE;
    if (!type || !CheckHandle(state, index, type, &value)) {
        PushArgumentError(state, index, "a wotb handle");
        return false;
    }
    *out = static_cast<uint64_t>(value);
    return true;
}

void PushGeneratedHandle(lua_State* state, uint64_t value, bool token) {
    if (token) PushToken(state, static_cast<WotbModV3Token>(value), "wotb.generated_token");
    else PushHandle(state, static_cast<WotbModV3Handle>(value), "wotb.generated_handle");
}

int AbsoluteIndex(lua_State* state, int index) noexcept {
    return index > 0 || index <= LUA_REGISTRYINDEX ? index : lua_gettop(state) + index + 1;
}

void RawGetField(lua_State* state, int index, const char* name) {
    index = AbsoluteIndex(state, index);
    lua_pushstring(state, name);
    lua_rawget(state, index);
}

// The constant writers. Every call site passes the ABI's own C name, so the
// value Lua sees is evaluated by this compiler against these headers - the
// generator never parses a C expression and therefore cannot disagree with
// them. lua_Integer is 64-bit in this build (third_party/lua/luaconf.h:118,
// 125), so every V3 enumerator and size cap fits without truncation.
void SetConstant(lua_State* state, const char* name, lua_Integer value) {
    lua_pushstring(state, name);
    lua_pushinteger(state, value);
    lua_rawset(state, -3);
}

void SetConstant(lua_State* state, const char* name, const char* value) {
    lua_pushstring(state, name);
    lua_pushstring(state, value);
    lua_rawset(state, -3);
}

// Everything else the ABI spells a constant with - an unscoped enum, an
// unsigned literal, a parenthesised size expression - converts through one
// cast the compiler checks, instead of through a per-type overload somebody
// has to remember to add when a header grows a new underlying type.
template <typename T>
void SetConstant(lua_State* state, const char* name, T value) {
    SetConstant(state, name, static_cast<lua_Integer>(value));
}

// wotb.<name> may already hold a hand-written table, so a constant table is
// merged into whatever is there rather than replacing it, exactly as
// RegisterGeneratedBindings does. Raw get and raw set throughout: constants
// are data, and a metamethod installed on the wotb table must not be able to
// observe or intercept the host publishing them.
void OpenConstantTable(lua_State* state, const char* name) {
    RawGetField(state, -1, name);
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
}

void CloseConstantTable(lua_State* state, const char* name) {
    lua_pushstring(state, name);
    lua_pushvalue(state, -2);
    lua_rawset(state, -4);
    lua_pop(state, 1);
}

// Scratch memory for one binding call, freed on every exit path including the
// failing ones. Fixed at 128 blocks: a counted array is one block whatever its
// length, so the budget is only reachable by nesting - an array of structs that
// themselves hold arrays. Running out is a null pointer, never a short buffer,
// and every caller turns that into nil and a message.
struct Arena {
    void* blocks[128];
    uint32_t count;
};

void ArenaInit(Arena* arena) noexcept { arena->count = 0u; }
void ArenaClear(Arena* arena) noexcept {
    while (arena->count) std::free(arena->blocks[--arena->count]);
}
void* ArenaAllocate(Arena* arena, size_t bytes) noexcept {
    if (arena->count == 128u || bytes == 0u) return nullptr;
    void* block = std::calloc(1u, bytes);
    if (block) arena->blocks[arena->count++] = block;
    return block;
}

bool ReadOpaquePointer(lua_State* state, int index, void** out) noexcept {
    if (lua_isnil(state, index)) { *out = nullptr; return true; }
    if (!lua_islightuserdata(state, index)) {
        PushArgumentError(state, index, "an opaque native pointer returned by this host");
        return false;
    }
    *out = lua_touserdata(state, index);
    return true;
}

enum class CallbackCleanup { kNone, kHandleRelease, kHttpCancel };

struct CallbackRecord {
    uint32_t id = 0u;
    LuaScript* script = nullptr;
    const WotbModV3HandlesApiV1* handles = nullptr;
    const WotbModV3HttpApiV1* http = nullptr;
    WotbModV3Handle mod = WOTBMOD_V3_INVALID_HANDLE;
    WotbModV3Handle cleanup_handle = WOTBMOD_V3_INVALID_HANDLE;
    int callback_ref = LUA_NOREF;
    int in_flight = 0;
    CallbackCleanup cleanup = CallbackCleanup::kNone;
    bool live = true;
    bool one_shot = false;
    bool held_for_registration = true;
    bool held_for_release = false;
};

std::mutex& CallbackLock() { static std::mutex lock; return lock; }
std::condition_variable& CallbackIdle() {
    static std::condition_variable idle;
    return idle;
}
std::vector<CallbackRecord*>& CallbackTable() {
    static std::vector<CallbackRecord*> table;
    return table;
}
uint32_t g_next_callback_id = 1u;

CallbackRecord* FindCallbackLocked(uint32_t id) noexcept {
    for (CallbackRecord* record : CallbackTable())
        if (record->id == id) return record;
    return nullptr;
}

void ReapCallbackLocked(CallbackRecord* record) noexcept {
    if (!record || record->live || record->in_flight ||
        record->held_for_registration || record->held_for_release) return;
    std::vector<CallbackRecord*>& table = CallbackTable();
    for (auto it = table.begin(); it != table.end(); ++it) {
        if (*it == record) { table.erase(it); break; }
    }
    delete record;
}

CallbackRecord* CreateCallback(lua_State* state, int index, Context* ctx,
                               bool one_shot) noexcept {
    if (!CheckArgFunction(state, index)) return nullptr;
    lua_pushvalue(state, index);
    const int callback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    CallbackRecord* record = new (std::nothrow) CallbackRecord();
    if (!record) {
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
        lua_pushnil(state); lua_pushstring(state, "out of memory");
        return nullptr;
    }
    record->callback_ref = callback_ref;
    record->script = ctx->script;
    record->handles = ctx->handles;
    record->mod = ctx->mod;
    record->one_shot = one_shot;
    try {
        std::lock_guard<std::mutex> guard(CallbackLock());
        record->id = g_next_callback_id++;
        if (!record->id) record->id = g_next_callback_id++;
        CallbackTable().push_back(record);
    } catch (...) {
        luaL_unref(state, LUA_REGISTRYINDEX, record->callback_ref);
        delete record;
        lua_pushnil(state); lua_pushstring(state, "out of memory");
        return nullptr;
    }
    return record;
}

CallbackRecord* ClaimCallback(uint32_t id) noexcept {
    std::lock_guard<std::mutex> guard(CallbackLock());
    CallbackRecord* record = FindCallbackLocked(id);
    if (!record || !record->live ||
        record->script->InstructionLimitExceeded()) return nullptr;
    ++record->in_flight;
    return record;
}

int RecheckCallback(CallbackRecord* record, bool active) noexcept {
    std::lock_guard<std::mutex> guard(CallbackLock());
    return active && record->live &&
                   !record->script->InstructionLimitExceeded()
               ? record->callback_ref
               : LUA_NOREF;
}

void RetireOneShot(lua_State* state, CallbackRecord* record) noexcept {
    int callback_ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> guard(CallbackLock());
        record->live = false;
        callback_ref = record->callback_ref;
        record->callback_ref = LUA_NOREF;
    }
    if (callback_ref != LUA_NOREF)
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
}

void FinishCallback(CallbackRecord* record) noexcept {
    {
        std::lock_guard<std::mutex> guard(CallbackLock());
        --record->in_flight;
        ReapCallbackLocked(record);
    }
    CallbackIdle().notify_all();
}

void FinishCallbackRegistration(lua_State* state, CallbackRecord* record,
                                bool success, bool synchronous,
                                CallbackCleanup cleanup,
                                WotbModV3Handle cleanup_handle,
                                const WotbModV3HttpApiV1* http) noexcept {
    int callback_ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> guard(CallbackLock());
        if (success && !synchronous && record->live) {
            record->cleanup = cleanup;
            record->cleanup_handle = cleanup_handle;
            record->http = http;
        } else {
            record->live = false;
            callback_ref = record->callback_ref;
            record->callback_ref = LUA_NOREF;
        }
        record->held_for_registration = false;
        ReapCallbackLocked(record);
    }
    if (callback_ref != LUA_NOREF)
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
}

void ReleaseCallbacks(LuaScript* script) noexcept {
    for (;;) {
        CallbackRecord* record = nullptr;
        {
            std::lock_guard<std::mutex> guard(CallbackLock());
            for (CallbackRecord* candidate : CallbackTable()) {
                if (candidate->script == script && candidate->live) {
                    record = candidate;
                    record->live = false;
                    record->held_for_release = true;
                    break;
                }
            }
        }
        if (!record) break;
        if (record->cleanup == CallbackCleanup::kHandleRelease &&
            record->handles && record->handles->release &&
            record->cleanup_handle != WOTBMOD_V3_INVALID_HANDLE) {
            record->handles->release(record->mod, record->cleanup_handle);
        } else if (record->cleanup == CallbackCleanup::kHttpCancel &&
                   record->http && record->http->request_cancel &&
                   record->cleanup_handle != WOTBMOD_V3_INVALID_HANDLE) {
            record->http->request_cancel(
                record->mod,
                static_cast<WotbModV3HttpHandle>(record->cleanup_handle));
        }
        {
            std::unique_lock<std::mutex> guard(CallbackLock());
            CallbackIdle().wait(guard,
                [record] { return record->in_flight == 0; });
        }
        LuaScript::Entry entry(*script);
        int callback_ref = LUA_NOREF;
        {
            std::lock_guard<std::mutex> guard(CallbackLock());
            callback_ref = record->callback_ref;
            record->callback_ref = LUA_NOREF;
            record->held_for_release = false;
            ReapCallbackLocked(record);
        }
        if (callback_ref != LUA_NOREF)
            luaL_unref(entry.state(), LUA_REGISTRYINDEX, callback_ref);
    }
}

void ForgetCallbackHandle(lua_State* state, LuaScript* script,
                          WotbModV3Handle handle) noexcept {
    CallbackRecord* record = nullptr;
    int callback_ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> guard(CallbackLock());
        for (CallbackRecord* candidate : CallbackTable()) {
            if (candidate->script == script && candidate->live &&
                candidate->cleanup == CallbackCleanup::kHandleRelease &&
                candidate->cleanup_handle == handle) {
                record = candidate;
                break;
            }
        }
        if (!record) return;
        record->live = false;
        record->cleanup = CallbackCleanup::kNone;
        callback_ref = record->callback_ref;
        record->callback_ref = LUA_NOREF;
        ReapCallbackLocked(record);
    }
    if (callback_ref != LUA_NOREF)
        luaL_unref(state, LUA_REGISTRYINDEX, callback_ref);
}

void ForgetCallbackIfDead(lua_State* state, Context* ctx,
                          WotbModV3Handle handle) noexcept {
    if (!ctx->handles || !ctx->handles->is_alive) return;
    uint32_t alive = 1u;
    if (ctx->handles->is_alive(ctx->mod, handle, &alive) == WOTBMOD_V3_OK &&
        !alive) {
        ForgetCallbackHandle(state, ctx->script, handle);
    }
}
'''.splitlines())

    def emit_structs(self) -> None:
        # Forward declarations allow nested structs in either header order.
        data_types = sorted(
            name for name in self.model.struct_bodies if "ApiV" not in name
        )
        for name in data_types:
            self.line(f"bool Read_{name}(lua_State*, int, {name}*, uint32_t, Arena*) noexcept;")
            self.line(f"void Push_{name}(lua_State*, const {name}&);")
        self.line()
        for name in data_types:
            fields = self.model.data_fields(name)
            self.emit_read_struct(name, fields)
            self.emit_push_struct(name, fields)

    def emit_read_struct(self, name: str, fields: tuple[DataField, ...]) -> None:
        self.line(f"bool Read_{name}(lua_State* state, int index, {name}* out, uint32_t version, Arena* arena) noexcept {{")
        self.line("    (void)version; (void)arena;")
        self.line("    if (lua_type(state, index) != LUA_TTABLE) { PushArgumentError(state, index, \"a table\"); return false; }")
        self.line("    std::memset(out, 0, sizeof(*out));")
        field_names = {field.name for field in fields}
        if "struct_size" in field_names:
            self.line("    out->struct_size = sizeof(*out);")
        if "api_version" in field_names:
            self.line("    out->api_version = version;")
        self.emit_read_fields(
            fields,
            "out->",
            "    ",
            skip=frozenset(
                {"struct_size", "api_version", "reserved", "reserved2"}
            ),
        )
        self.line("    return true;")
        self.line("}")
        self.line()

    def emit_read_fields(
        self,
        fields: tuple[DataField, ...],
        owner: str,
        indent: str,
        skip: frozenset[str] = frozenset(),
    ) -> None:
        """Read one declared run of fields, honouring counted-array pairs.

        Pairing is decided here, over the declaration order the headers
        actually have, because a field on its own cannot see the count that
        gives it a length. Both members of a pair are emitted together and the
        walk then steps over both: the count is never read as a free-standing
        number, which is the entire point (see is_input_array_field_pair).
        """
        index = 0
        while index < len(fields):
            field = fields[index]
            if field.name in skip:
                index += 1
                continue
            if is_input_array_field_pair(fields, index):
                count = fields[index + 1]
                self.emit_read_field(field, owner, indent, count)
                self.emit_read_count_field(field, count, indent)
                index += 2
                continue
            self.emit_read_field(field, owner, indent)
            index += 1

    def emit_read_count_field(
        self, array_field: DataField, count_field: DataField, indent: str
    ) -> None:
        """Refuse a count the script supplied that is not the array's length.

        The count itself has already been set from the table - this reads the
        script's own value only to disagree with it. Fail-closed on every
        disagreement, in either direction: a count larger than the array is the
        out-of-bounds read, a count smaller than it silently drops elements the
        script meant to send, and a count with no array at all (`{value_count =
        512}`) is the same attack written differently. Absent is fine and
        means "however many are in the table", which is the only spelling a
        script needs.
        """
        length = f"{array_field.name}_length"
        self.line(f'{indent}RawGetField(state, index, "{count_field.name}");')
        self.line(f"{indent}if (!lua_isnil(state, -1)) {{")
        inner = indent + "    "
        self.line(f"{inner}uint64_t v = 0; if (!ReadUnsigned(state, -1, &v)) return false;")
        self.line(
            f"{inner}if (v != static_cast<uint64_t>({length})) {{ "
            f"PushArgumentError(state, -1, \"no '{count_field.name}' at all, or one equal to "
            f"the number of entries in '{array_field.name}'\"); return false; }}"
        )
        self.line(f"{indent}}}")
        self.line(f"{indent}lua_pop(state, 1);")

    def emit_read_field(
        self,
        field: DataField,
        owner: str,
        indent: str,
        count_field: DataField | None = None,
    ) -> None:
        target = f"{owner}{field.name}"
        if count_field is not None:
            # Function scope, not the field's own block: emit_read_count_field
            # reads it back one field later to check the script's own count
            # against it.
            self.line(f"{indent}size_t {field.name}_length = 0u;")
        self.line(f'{indent}RawGetField(state, index, "{field.name}");')
        self.line(f"{indent}if (!lua_isnil(state, -1)) {{")
        inner = indent + "    "
        if field.union_members:
            # The three ABI unions are discriminated by a sibling `type` and
            # represented as a Lua table named value. Read whichever member is
            # present; the native API validates consistency with type.
            self.line(f"{inner}if (lua_type(state, -1) != LUA_TTABLE) {{ PushArgumentError(state, -1, \"a table\"); return false; }}")
            # Through the same walker as a struct body, so a union that one day
            # holds a counted array gets the pairing rule rather than the
            # single-element read this whole change exists to delete.
            self.emit_read_fields(field.union_members, f"{target}.", inner)
        elif field.array_extent:
            if clean_type(field.c_type) == "char":
                self.line(f"{inner}size_t n = 0u; const char* text = lua_tolstring(state, -1, &n);")
                self.line(f"{inner}if (!text) {{ PushArgumentError(state, -1, \"a string\"); return false; }}")
                self.line(f"{inner}if (n >= sizeof({target})) n = sizeof({target}) - 1u;")
                self.line(f"{inner}std::memcpy({target}, text, n); {target}[n] = '\\0';")
            else:
                self.line(f"{inner}if (lua_type(state, -1) != LUA_TTABLE) {{ PushArgumentError(state, -1, \"an array table\"); return false; }}")
                self.line(f"{inner}const size_t count = sizeof({target}) / sizeof({target}[0]);")
                self.line(f"{inner}for (size_t i = 0; i < count; ++i) {{ lua_rawgeti(state, -1, static_cast<lua_Integer>(i + 1u));")
                self.emit_read_value(field.c_type, f"{target}[i]", inner + "    ")
                self.line(f"{inner}    lua_pop(state, 1); }}")
        elif clean_type(field.c_type) == "const char*":
            self.line(f"{inner}if (lua_type(state, -1) != LUA_TSTRING) {{ PushArgumentError(state, -1, \"a string\"); return false; }}")
            self.line(f"{inner}{target} = lua_tostring(state, -1);")
        elif clean_type(field.c_type) in {"const void*", "void*"}:
            self.line(f"{inner}void* pointer = nullptr; if (!ReadOpaquePointer(state, -1, &pointer)) return false; {target} = pointer;")
        elif clean_type(field.c_type).endswith("*"):
            pointee = base_type(field.c_type)
            if count_field is not None:
                self.emit_read_array_field(field, count_field, owner, inner)
            elif pointee in self.model.struct_bodies:
                # A pointer to exactly one element, and only because no count
                # field follows it - the walker would have paired them. Safe
                # for the same reason the pair is now safe: the length the
                # client will read is the length this host allocated, and here
                # both are one.
                self.line(f"{inner}{pointee}* item = static_cast<{pointee}*>(ArenaAllocate(arena, sizeof({pointee}))); ")
                self.line(f"{inner}if (!item || !Read_{pointee}(state, -1, item, version, arena)) return false; {target} = item;")
            else:
                self.line(f"{inner}PushArgumentError(state, -1, \"a value representable by this Lua host\"); return false;")
        else:
            self.emit_read_value(field.c_type, target, inner)
        self.line(f"{indent}}}")
        self.line(f"{indent}lua_pop(state, 1);")

    def emit_read_array_field(
        self,
        field: DataField,
        count_field: DataField,
        owner: str,
        indent: str,
    ) -> None:
        """N elements into an allocation of N, and a count that says N.

        The invariant, stated once because everything below serves it: the
        number this host writes into the count field is the number of elements
        it allocated and filled, and it comes from the Lua table rather than
        from anything the script says separately.

        Deliberately the same shape as emit_input_array_slot, which has been
        reading counted arrays out of Lua at the parameter level all along -
        same bound check, same allocation, same per-element read, same
        `lua_remove(state, -3)` to keep the (nil, message) pair on top when an
        element is refused. This is that code applied one level down.

        The bounds, in the order they can fail:

        * not a table - refused. A struct-shaped table (`{key = 'x'}`) has
          length zero and reads as an empty array, so it is refused a moment
          later by the count check if the script also claimed a count, and is
          otherwise an empty array. Both are safe; neither invents an element.
        * length past UINT32_MAX, or long enough that N * sizeof(T) would wrap
          - refused. size_t is 32-bit in this build, so the multiplication is
          exactly where a large N would turn into a small allocation.
        * the allocation failing - nil and "out of memory". calloc failing is
          the ordinary way a large N ends, and the arena's own 128-block
          budget is the other: an array is one block, so a struct of arrays,
          or an array of structs that themselves hold arrays, can exhaust it.
          Either way the answer is a value, not a crash and not a short buffer.
        * an element the reader refuses - its own message, unchanged.

        Empty (`{}`) is a null pointer and a count of zero, because
        ArenaAllocate(0) is a null pointer by construction. A client that
        requires at least one element rejects that itself; the memset in
        Read_ already left exactly this state for an absent field.
        """
        target = f"{owner}{field.name}"
        length = f"{field.name}_length"
        item_type = base_type(field.c_type)
        items = f"{field.name}_items"
        self.line(f"{indent}if (lua_type(state, -1) != LUA_TTABLE) {{ PushArgumentError(state, -1, \"an array table\"); return false; }}")
        self.line(f"{indent}{length} = static_cast<size_t>(lua_rawlen(state, -1));")
        self.line(f"{indent}if ({length} > UINT32_MAX || ({length} && {length} > SIZE_MAX / sizeof({item_type}))) {{ PushArgumentError(state, -1, \"a bounded array\"); return false; }}")
        self.line(f"{indent}if ({length}) {{")
        inner = indent + "    "
        self.line(f"{inner}{item_type}* {items} = static_cast<{item_type}*>(ArenaAllocate(arena, {length} * sizeof({item_type})));")
        self.line(f"{inner}if (!{items}) {{ lua_pushnil(state); lua_pushstring(state, \"out of memory\"); return false; }}")
        self.line(f"{inner}for (size_t i = 0; i < {length}; ++i) {{ lua_rawgeti(state, -1, static_cast<lua_Integer>(i + 1u));")
        if item_type in self.model.struct_bodies:
            self.line(f"{inner}    if (!Read_{item_type}(state, -1, &{items}[i], version, arena)) {{ lua_remove(state, -3); return false; }}")
        elif item_type in NUMBER_TYPES:
            self.line(f"{inner}    double item = 0.0; if (!ReadNumber(state, -1, &item)) {{ lua_remove(state, -3); return false; }} {items}[i] = static_cast<{item_type}>(item);")
        else:
            self.line(f"{inner}    int64_t item = 0; if (!ReadInteger(state, -1, &item)) {{ lua_remove(state, -3); return false; }} {items}[i] = static_cast<{item_type}>(item);")
        self.line(f"{inner}    lua_pop(state, 1); }}")
        self.line(f"{inner}{target} = {items};")
        self.line(f"{indent}}}")
        self.line(f"{indent}{owner}{count_field.name} = static_cast<uint32_t>({length});")

    def emit_read_value(self, c_type: str, target: str, indent: str) -> None:
        c_type = clean_type(c_type)
        if is_handle(c_type):
            self.line(f"{indent}uint64_t v = 0u; if (!ReadAnyHandle(state, -1, &v)) return false; {target} = static_cast<{c_type}>(v);")
        elif c_type in INTEGER_TYPES:
            reader = "ReadUnsigned" if c_type.startswith("u") or c_type in {"size_t", "WotbModV3YamlNodeId"} else "ReadInteger"
            temp = "uint64_t" if reader == "ReadUnsigned" else "int64_t"
            self.line(f"{indent}{temp} v = 0; if (!{reader}(state, -1, &v)) return false; {target} = static_cast<{c_type}>(v);")
        elif c_type in NUMBER_TYPES:
            self.line(f"{indent}double v = 0.0; if (!ReadNumber(state, -1, &v)) return false; {target} = static_cast<{c_type}>(v);")
        elif c_type in self.model.struct_bodies:
            self.line(f"{indent}if (!Read_{c_type}(state, -1, &{target}, version, arena)) return false;")
        elif c_type in self.model.function_types:
            self.line(f"{indent}PushArgumentError(state, -1, \"a callback registered through a dedicated API function\"); return false;")
        else:
            self.line(f"{indent}int64_t v = 0; if (!ReadInteger(state, -1, &v)) return false; {target} = static_cast<{c_type}>(v);")

    def emit_push_struct(self, name: str, fields: tuple[DataField, ...]) -> None:
        self.line(f"void Push_{name}(lua_State* state, const {name}& value) {{")
        self.line("    (void)value;")
        self.line("    lua_newtable(state);")
        for field in fields:
            if field.name in {"struct_size", "api_version", "reserved", "reserved2"}:
                continue
            self.emit_push_field(field, "value.", "    ")
        self.line("}")
        self.line()

    def emit_push_field(self, field: DataField, owner: str, indent: str) -> None:
        value = f"{owner}{field.name}"
        if field.union_members:
            self.line(f"{indent}lua_newtable(state);")
            for member in field.union_members:
                self.emit_push_field(member, f"{value}.", indent)
        elif field.array_extent:
            if clean_type(field.c_type) == "char":
                self.line(f"{indent}lua_pushstring(state, {value});")
            else:
                self.line(f"{indent}lua_newtable(state);")
                self.line(f"{indent}for (size_t i = 0; i < sizeof({value}) / sizeof({value}[0]); ++i) {{")
                self.emit_push_value(field.c_type, f"{value}[i]", indent + "    ")
                self.line(f"{indent}    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1u));")
                self.line(f"{indent}}}")
        elif clean_type(field.c_type) == "const char*":
            self.line(f"{indent}if ({value}) lua_pushstring(state, {value}); else lua_pushnil(state);")
        elif clean_type(field.c_type) in {"const void*", "void*"} or clean_type(field.c_type).endswith("*"):
            self.line(f"{indent}if ({value}) lua_pushlightuserdata(state, const_cast<void*>(static_cast<const void*>({value}))); else lua_pushnil(state);")
        else:
            self.emit_push_value(field.c_type, value, indent)
        self.line(f'{indent}lua_setfield(state, -2, "{field.name}");')

    def emit_push_value(self, c_type: str, value: str, indent: str) -> None:
        c_type = clean_type(c_type)
        if is_handle(c_type):
            token = "true" if "Token" in c_type else "false"
            self.line(f"{indent}PushGeneratedHandle(state, static_cast<uint64_t>({value}), {token});")
        elif c_type in INTEGER_TYPES:
            self.line(f"{indent}lua_pushinteger(state, static_cast<lua_Integer>({value}));")
        elif c_type in NUMBER_TYPES:
            self.line(f"{indent}lua_pushnumber(state, static_cast<lua_Number>({value}));")
        elif c_type in self.model.struct_bodies:
            self.line(f"{indent}Push_{c_type}(state, {value});")
        elif c_type in self.model.function_types:
            self.line(f"{indent}lua_pushnil(state);")
        else:
            self.line(f"{indent}lua_pushinteger(state, static_cast<lua_Integer>({value}));")

    def reason_unsupported(self, slot: Slot) -> str | None:
        key = f"{slot.interface.lua_name}.{slot.name}"
        if key in NATIVE_ONLY:
            return "requires a native pointer and FFI is intentionally unavailable"
        return None

    def emit_slot(self, slot: Slot) -> None:
        key = f"{slot.interface.lua_name}.{slot.name}"
        if key in CUSTOM_SLOTS:
            return
        function = f"Bind_{symbol(slot)}"
        why = self.reason_unsupported(slot)
        if why:
            self.unsupported[key] = why
            self.line(f"int {function}(lua_State* state) noexcept {{ return Unsupported(state, \"{key}\", \"{why}\"); }}")
            return
        params = list(slot.function.parameters)
        if any(base_type(parameter.c_type) in self.model.function_types for parameter in params):
            self.emit_callback_slot(slot, function)
            return
        if any(clean_type(parameter.c_type) == "char*" and parameter.array_extent for parameter in params):
            self.emit_fixed_string_slot(slot, function)
            return
        if any(clean_type(parameter.c_type) == "char*" and not parameter.array_extent for parameter in params):
            self.emit_sized_string_slot(slot, function)
            return
        if any(clean_type(parameter.c_type) == "WotbModV3Buffer*" for parameter in params):
            self.emit_buffer_slot(slot, function)
            return
        for index in range(len(params) - 1):
            if is_output_array_pair(params, index):
                self.emit_output_array_slot(slot, function, index)
                return
            if is_input_array_pair(params, index):
                self.emit_input_array_slot(slot, function, index)
                return
        self.emit_direct_slot(slot, function)

    def emit_fixed_string_slot(self, slot: Slot, function: str) -> None:
        params = list(slot.function.parameters)
        output_index = next(index for index, parameter in enumerate(params) if clean_type(parameter.c_type) == "char*" and parameter.array_extent)
        output = params[output_index]
        self.line(f"int {function}(lua_State* state) noexcept {{")
        self.line("    Context* ctx = GetContext(state);")
        self.emit_slot_guard(slot)
        self.line("    Arena arena; ArenaInit(&arena);")
        arguments: list[str] = []
        lua_index = 1
        for index, parameter in enumerate(params):
            c_type = clean_type(parameter.c_type)
            if index == 0 and c_type == "WotbModV3Handle" and parameter.name == "mod":
                arguments.append("ctx->mod")
            elif index == output_index:
                self.line(f"    char {output.name}[{output.array_extent}] = {{}};")
                arguments.append(output.name)
            else:
                argument, lua_index = self.emit_simple_input(slot, parameter, lua_index)
                arguments.append(argument)
        self.line("    const StackMark mark = MarkStack(state);")
        self.line(f"    const WotbModV3Result result = static_cast<const {slot.interface.api_type}*>(ctx->api)->{slot.field_path}({', '.join(arguments)});")
        self.line("    ArenaClear(&arena);")
        self.line(f"    lua_pushstring(state, {output.name});")
        self.line(f"    return PushResultWith(state, result, mark, \"{slot.interface.lua_name}.{slot.name}\");")
        self.line("}")

    def emit_simple_input(self, slot: Slot, parameter, lua_index: int) -> tuple[str, int]:
        c_type = clean_type(parameter.c_type)
        name = parameter.name
        if c_type == "const char*":
            self.line(f"    const char* {name} = nullptr; if (!CheckArgString(state, {lua_index}, &{name})) {{ ArenaClear(&arena); return 2; }}")
            return name, lua_index + 1
        if is_handle(c_type):
            self.line(f"    uint64_t {name}_raw = 0u; if (!ReadAnyHandle(state, {lua_index}, &{name}_raw)) {{ ArenaClear(&arena); return 2; }}")
            self.line(f"    {c_type} {name} = static_cast<{c_type}>({name}_raw);")
            return name, lua_index + 1
        if c_type in INTEGER_TYPES:
            reader = "ReadUnsigned" if c_type.startswith("u") or c_type in {"size_t", "WotbModV3YamlNodeId"} else "ReadInteger"
            temp = "uint64_t" if reader == "ReadUnsigned" else "int64_t"
            self.line(f"    {temp} {name}_raw = 0; if (!{reader}(state, {lua_index}, &{name}_raw)) {{ ArenaClear(&arena); return 2; }}")
            self.line(f"    {c_type} {name} = static_cast<{c_type}>({name}_raw);")
            return name, lua_index + 1
        if c_type in NUMBER_TYPES:
            self.line(f"    double {name}_raw = 0.0; if (!ReadNumber(state, {lua_index}, &{name}_raw)) {{ ArenaClear(&arena); return 2; }}")
            self.line(f"    {c_type} {name} = static_cast<{c_type}>({name}_raw);")
            return name, lua_index + 1
        # A CONST BUFFER IS BYTES, and from a script bytes are a string.
        #
        # Two other emitters here already knew that - emit_sized_string_slot
        # and emit_output_array_slot both read this type with CheckArgBytes -
        # and this one, the path every slot without an output buffer takes,
        # fell through to the generic struct reader instead. That reader wants
        # a table whose `data` field is an OPAQUE POINTER, and a Lua script
        # has no way to make one: `wotb.vfs.write_file` was published, callable
        # and impossible, answering "argument 2: expected a table, got string"
        # to the only argument anybody could pass it. The whole write half of
        # VFS V2 was inert on every client that shipped it.
        if c_type == "const WotbModV3ConstBuffer*":
            self.line(f"    const char* {name}_bytes = nullptr; size_t {name}_size = 0u;")
            self.line(f"    if (!CheckArgBytes(state, {lua_index}, &{name}_bytes, &{name}_size)) {{ ArenaClear(&arena); return 2; }}")
            self.line(f"    WotbModV3ConstBuffer {name} = {{}}; {name}.struct_size = sizeof({name}); {name}.api_version = {slot.interface.version_macro}; {name}.data = {name}_bytes; {name}.size = static_cast<uint32_t>({name}_size);")
            return "&" + name, lua_index + 1
        if c_type.startswith("const ") and c_type.endswith("*") and base_type(c_type) in self.model.struct_bodies:
            pointee = base_type(c_type)
            version = struct_version_macro(pointee, slot.interface.version_macro)
            self.line(f"    {pointee} {name} = {{}};")
            self.line(f"    if (!Read_{pointee}(state, {lua_index}, &{name}, {version}, &arena)) {{ ArenaClear(&arena); return 2; }}")
            return "&" + name, lua_index + 1
        if c_type in self.model.struct_bodies:
            version = struct_version_macro(c_type, slot.interface.version_macro)
            self.line(f"    {c_type} {name} = {{}}; if (!Read_{c_type}(state, {lua_index}, &{name}, {version}, &arena)) {{ ArenaClear(&arena); return 2; }}")
            return name, lua_index + 1
        raise ValueError(f"simple input is not representable: {c_type} {name}")

    def emit_slot_guard(self, slot: Slot) -> None:
        self.line(
            f"    if (!static_cast<const {slot.interface.api_type}*>(ctx->api)->{slot.field_path}) "
            f"return Unsupported(state, \"{slot.interface.lua_name}.{slot.name}\", \"the client did not publish this slot\");"
        )

    def emit_sized_string_slot(self, slot: Slot, function: str) -> None:
        params = list(slot.function.parameters)
        buffer_index = next(index for index, parameter in enumerate(params) if clean_type(parameter.c_type) == "char*")
        self.line(f"int {function}(lua_State* state) noexcept {{")
        self.line("    Context* ctx = GetContext(state);")
        self.emit_slot_guard(slot)
        self.line("    Arena arena; ArenaInit(&arena);")
        arguments: list[str] = []
        outputs: list[tuple[str, str]] = []
        lua_index = 1
        index = 0
        while index < len(params):
            parameter = params[index]
            c_type = clean_type(parameter.c_type)
            if index == 0 and c_type == "WotbModV3Handle" and parameter.name == "mod":
                arguments.append("ctx->mod"); index += 1; continue
            if index == buffer_index:
                arguments.extend(["buffer", "size"]); index += 2; continue
            if c_type.endswith("*") and parameter.name.startswith("out_"):
                pointee = base_type(c_type)
                self.line(f"    {pointee} {parameter.name} = {{}};")
                arguments.append("&" + parameter.name)
                outputs.append((pointee, parameter.name))
                index += 1; continue
            argument, lua_index = self.emit_simple_input(slot, parameter, lua_index)
            arguments.append(argument); index += 1
        api = f"static_cast<const {slot.interface.api_type}*>(ctx->api)"
        field = "->" + slot.field_path
        owned_outputs = [
            out_name for out_type, out_name in outputs if is_handle(out_type)
        ]
        if owned_outputs:
            self.line("    if (!ctx->handles || !ctx->handles->release) { ArenaClear(&arena); return Unsupported(state, \"" + f"{slot.interface.lua_name}.{slot.name}" + "\", \"handles.release is required for ownership\"); }")
        self.line("    ArenaClear(&arena);")
        self.line(f"    const int fetched = PushSizedString(state, [&](char* buffer, uint32_t* size) {{ return {api}{field}({', '.join(arguments)}); }}, \"{slot.interface.lua_name}.{slot.name}\");")
        if outputs:
            self.line("    if (fetched != 1) return fetched;")
            for out_name in owned_outputs:
                self.line(f"    if (!ctx->script->Ownership().RecordHandle(static_cast<WotbModV3Handle>({out_name}))) {{")
                self.line(f"        ctx->handles->release(ctx->mod, static_cast<WotbModV3Handle>({out_name})); lua_pop(state, 1);")
                self.line(f"        lua_pushnil(state); lua_pushstring(state, \"{slot.interface.lua_name}.{slot.name}: this script is being unloaded\"); return 2;")
                self.line("    }")
            for out_type, out_name in outputs:
                if out_type in INTEGER_TYPES:
                    self.line(f"    lua_pushinteger(state, static_cast<lua_Integer>({out_name}));")
                elif is_handle(out_type):
                    token = "true" if "Token" in out_type else "false"
                    self.line(f"    PushGeneratedHandle(state, static_cast<uint64_t>({out_name}), {token});")
                else:
                    self.line(f"    Push_{out_type}(state, {out_name});")
                self.line(f"    lua_insert(state, -{len(outputs) + 1});")
            self.line(f"    return {len(outputs) + 1};")
        else:
            self.line("    return fetched;")
        self.line("}")

    def emit_buffer_slot(self, slot: Slot, function: str) -> None:
        params = list(slot.function.parameters)
        buffer_index = next(index for index, parameter in enumerate(params) if clean_type(parameter.c_type) == "WotbModV3Buffer*")
        self.line(f"int {function}(lua_State* state) noexcept {{")
        self.line("    Context* ctx = GetContext(state);")
        self.emit_slot_guard(slot)
        self.line("    Arena arena; ArenaInit(&arena);")
        arguments: list[str] = []
        lua_index = 1
        for index, parameter in enumerate(params):
            c_type = clean_type(parameter.c_type)
            if index == 0 and c_type == "WotbModV3Handle" and parameter.name == "mod":
                arguments.append("ctx->mod"); continue
            if index == buffer_index:
                arguments.append("&value"); continue
            if c_type == "const WotbModV3ConstBuffer*":
                self.line(f"    const char* {parameter.name}_bytes = nullptr; size_t {parameter.name}_size = 0u;")
                self.line(f"    if (!CheckArgBytes(state, {lua_index}, &{parameter.name}_bytes, &{parameter.name}_size)) {{ ArenaClear(&arena); return 2; }}")
                self.line(f"    WotbModV3ConstBuffer {parameter.name} = {{}}; {parameter.name}.struct_size = sizeof({parameter.name}); {parameter.name}.api_version = {slot.interface.version_macro}; {parameter.name}.data = {parameter.name}_bytes; {parameter.name}.size = static_cast<uint32_t>({parameter.name}_size);")
                arguments.append("&" + parameter.name); lua_index += 1; continue
            argument, lua_index = self.emit_simple_input(slot, parameter, lua_index)
            arguments.append(argument)
        api = f"static_cast<const {slot.interface.api_type}*>(ctx->api)"
        field = "->" + slot.field_path
        self.line("    ArenaClear(&arena);")
        self.line("    return PushByteBuffer(state, [&](char* buffer, uint32_t* size) {")
        self.line(f"        WotbModV3Buffer value = {{}}; value.struct_size = sizeof(value); value.api_version = {slot.interface.version_macro}; value.data = buffer; value.capacity = *size; value.size = 0u;")
        self.line(f"        const WotbModV3Result result = {api}{field}({', '.join(arguments)});")
        self.line("        *size = value.size; return result;")
        self.line(f"    }}, \"{slot.interface.lua_name}.{slot.name}\");")
        self.line("}")

    def emit_input_array_slot(self, slot: Slot, function: str, array_index: int) -> None:
        params = list(slot.function.parameters)
        array_parameter = params[array_index]
        item_type = base_type(array_parameter.c_type)
        self.line(f"int {function}(lua_State* state) noexcept {{")
        self.line("    Context* ctx = GetContext(state);")
        self.emit_slot_guard(slot)
        self.line("    Arena arena; ArenaInit(&arena);")
        arguments: list[str] = []
        outputs: list[tuple[str, str]] = []
        lua_index = 1
        index = 0
        while index < len(params):
            parameter = params[index]
            c_type = clean_type(parameter.c_type)
            if index == 0 and c_type == "WotbModV3Handle" and parameter.name == "mod":
                arguments.append("ctx->mod"); index += 1; continue
            if index == array_index:
                name = parameter.name
                self.line(f"    if (lua_type(state, {lua_index}) != LUA_TTABLE) {{ ArenaClear(&arena); return PushArgumentError(state, {lua_index}, \"an array table\"); }}")
                self.line(f"    const size_t {name}_length = static_cast<size_t>(lua_rawlen(state, {lua_index}));")
                self.line(f"    if ({name}_length > UINT32_MAX || ({name}_length && {name}_length > SIZE_MAX / sizeof({item_type}))) {{ ArenaClear(&arena); return PushArgumentError(state, {lua_index}, \"a bounded array\"); }}")
                self.line(f"    {item_type}* {name} = static_cast<{item_type}*>(ArenaAllocate(&arena, {name}_length * sizeof({item_type}))); ")
                self.line(f"    if ({name}_length && !{name}) {{ ArenaClear(&arena); lua_pushnil(state); lua_pushstring(state, \"out of memory\"); return 2; }}")
                self.line(f"    for (size_t i = 0; i < {name}_length; ++i) {{ lua_rawgeti(state, {lua_index}, static_cast<lua_Integer>(i + 1u));")
                if item_type in self.model.struct_bodies:
                    version = struct_version_macro(item_type, slot.interface.version_macro)
                    self.line(f"        if (!Read_{item_type}(state, -1, &{name}[i], {version}, &arena)) {{ lua_remove(state, -3); ArenaClear(&arena); return 2; }}")
                elif item_type in NUMBER_TYPES:
                    self.line(f"        double item = 0.0; if (!ReadNumber(state, -1, &item)) {{ lua_remove(state, -3); ArenaClear(&arena); return 2; }} {name}[i] = static_cast<{item_type}>(item);")
                else:
                    self.line(f"        int64_t item = 0; if (!ReadInteger(state, -1, &item)) {{ lua_remove(state, -3); ArenaClear(&arena); return 2; }} {name}[i] = static_cast<{item_type}>(item);")
                self.line("        lua_pop(state, 1); }")
                arguments.extend([name, f"static_cast<uint32_t>({name}_length)"])
                lua_index += 1; index += 2; continue
            if c_type.endswith("*") and parameter.name.startswith("out_"):
                pointee = base_type(c_type)
                self.line(f"    {pointee} {parameter.name} = {{}};")
                if pointee in self.model.struct_bodies:
                    field_names = {field.name for field in self.model.data_fields(pointee)}
                    if "struct_size" in field_names:
                        self.line(f"    {parameter.name}.struct_size = sizeof({parameter.name});")
                    if "api_version" in field_names:
                        version = struct_version_macro(pointee, slot.interface.version_macro)
                        self.line(f"    {parameter.name}.api_version = {version};")
                arguments.append("&" + parameter.name)
                outputs.append((pointee, parameter.name))
                index += 1; continue
            argument, lua_index = self.emit_simple_input(slot, parameter, lua_index)
            arguments.append(argument); index += 1
        api = f"static_cast<const {slot.interface.api_type}*>(ctx->api)"
        self.line("    const StackMark mark = MarkStack(state);")
        self.line(f"    const WotbModV3Result result = {api}->{slot.field_path}({', '.join(arguments)});")
        self.line("    ArenaClear(&arena);")
        for out_type, out_name in outputs:
            if out_type in self.model.struct_bodies:
                self.line(f"    Push_{out_type}(state, {out_name});")
            elif is_handle(out_type):
                token = "true" if "Token" in out_type else "false"
                self.line(f"    PushGeneratedHandle(state, static_cast<uint64_t>({out_name}), {token});")
            else:
                self.line(f"    lua_pushinteger(state, static_cast<lua_Integer>({out_name}));")
        self.line(f"    return PushResultWith(state, result, mark, \"{slot.interface.lua_name}.{slot.name}\");")
        self.line("}")

    def emit_output_array_slot(self, slot: Slot, function: str, array_index: int) -> None:
        params = list(slot.function.parameters)
        array_parameter = params[array_index]
        item_type = base_type(array_parameter.c_type)
        self.line(f"int {function}(lua_State* state) noexcept {{")
        self.line("    Context* ctx = GetContext(state);")
        self.emit_slot_guard(slot)
        self.line("    Arena arena; ArenaInit(&arena);")
        arguments: list[str] = []
        lua_index = 1
        index = 0
        while index < len(params):
            parameter = params[index]
            c_type = clean_type(parameter.c_type)
            if index == 0 and c_type == "WotbModV3Handle" and parameter.name == "mod":
                arguments.append("ctx->mod"); index += 1; continue
            if index == array_index:
                arguments.extend(["items", "&count"]); index += 2; continue
            argument, lua_index = self.emit_simple_input(slot, parameter, lua_index)
            arguments.append(argument); index += 1
        api = f"static_cast<const {slot.interface.api_type}*>(ctx->api)"
        field = "->" + slot.field_path
        first_arguments = ["nullptr" if argument == "items" else argument for argument in arguments]
        if is_handle(item_type):
            self.line("    if (!ctx->handles || !ctx->handles->release) { ArenaClear(&arena); return Unsupported(state, \"" + f"{slot.interface.lua_name}.{slot.name}" + "\", \"handles.release is required for ownership\"); }")
        self.line("    uint32_t count = 0u;")
        self.line(f"    WotbModV3Result result = {api}{field}({', '.join(first_arguments)});")
        self.line("    if (result != WOTBMOD_V3_OK && result != WOTBMOD_V3_E_BUFFER_TOO_SMALL) { ArenaClear(&arena); return PushResult(state, result, \"" + slot.interface.lua_name + "." + slot.name + "\"); }")
        self.line("    if (count > (1u << 20) || (count && count > SIZE_MAX / sizeof(" + item_type + "))) { ArenaClear(&arena); lua_pushnil(state); lua_pushstring(state, \"client returned an unreasonable array size\"); return 2; }")
        self.line(f"    {item_type}* items = count ? static_cast<{item_type}*>(std::calloc(count, sizeof({item_type}))) : nullptr;")
        self.line("    if (count && !items) { ArenaClear(&arena); lua_pushnil(state); lua_pushstring(state, \"out of memory\"); return 2; }")
        if item_type in self.model.struct_bodies:
            field_names = {field.name for field in self.model.data_fields(item_type)}
            if "struct_size" in field_names:
                self.line("    for (uint32_t i = 0u; i < count; ++i) items[i].struct_size = sizeof(items[i]);")
            if "api_version" in field_names:
                version = struct_version_macro(item_type, slot.interface.version_macro)
                self.line(f"    for (uint32_t i = 0u; i < count; ++i) items[i].api_version = {version};")
        self.line(f"    if (count) result = {api}{field}({', '.join(arguments)});")
        self.line("    ArenaClear(&arena);")
        self.line(f"    const StackMark mark = MarkStack(state);")
        if is_handle(item_type):
            self.line("    if (result == WOTBMOD_V3_OK) {")
            self.line("        uint32_t recorded = 0u;")
            self.line("        while (recorded < count && ctx->script->Ownership().RecordHandle(static_cast<WotbModV3Handle>(items[recorded]))) ++recorded;")
            self.line("        if (recorded != count) {")
            self.line("            for (uint32_t i = 0u; i < recorded; ++i) ctx->script->Ownership().ForgetHandle(static_cast<WotbModV3Handle>(items[i]));")
            self.line("            for (uint32_t i = 0u; i < count; ++i) ctx->handles->release(ctx->mod, static_cast<WotbModV3Handle>(items[i]));")
            self.line("            std::free(items); lua_pushnil(state); lua_pushstring(state, \"" + f"{slot.interface.lua_name}.{slot.name}: this script is being unloaded" + "\"); return 2;")
            self.line("        }")
            self.line("    }")
        self.line("    if (result == WOTBMOD_V3_OK) {")
        self.line("        lua_createtable(state, static_cast<int>(count), 0);")
        self.line("        for (uint32_t i = 0u; i < count; ++i) {")
        if item_type in self.model.struct_bodies:
            self.line(f"            Push_{item_type}(state, items[i]);")
        elif is_handle(item_type):
            token = "true" if "Token" in item_type else "false"
            self.line(f"            PushGeneratedHandle(state, static_cast<uint64_t>(items[i]), {token});")
        elif item_type in NUMBER_TYPES:
            self.line("            lua_pushnumber(state, static_cast<lua_Number>(items[i]));")
        else:
            self.line("            lua_pushinteger(state, static_cast<lua_Integer>(items[i]));")
        self.line("            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1u));")
        self.line("        }")
        self.line("    }")
        self.line("    std::free(items);")
        self.line(f"    return PushResultWith(state, result, mark, \"{slot.interface.lua_name}.{slot.name}\");")
        self.line("}")

    def emit_callback_slot(self, slot: Slot, function: str) -> None:
        key = f"{slot.interface.lua_name}.{slot.name}"
        params = list(slot.function.parameters)
        callback_index = next(
            index for index, parameter in enumerate(params)
            if base_type(parameter.c_type) in self.model.function_types
        )
        callback_type = base_type(params[callback_index].c_type)
        callback = self.model.function_types[callback_type]
        callback_params = [
            parameter for parameter in callback.parameters
            if parameter.name not in {"mod", "user_data"}
        ]
        output_params = [
            parameter for parameter in params
            if parameter.name.startswith("out_") and clean_type(parameter.c_type).endswith("*")
        ]
        synchronous = not output_params and key != "http.request_send_async"
        one_shot = (
            key.startswith("async.dispatch_to_") or
            key == "http.request_send_async" or
            key == "lifecycle.register_cleanup"
        )
        ident = symbol(slot)

        self.line(f"struct Invocation_{ident} {{")
        self.line("    int callback_ref;")
        for parameter in callback_params:
            self.line(f"    {clean_type(parameter.c_type)} {parameter.name};")
        self.line("    WotbModV3Result callback_result;")
        self.line("};")
        self.line(f"int Invoke_{ident}(lua_State* state) {{")
        self.line(f"    Invocation_{ident}* args = static_cast<Invocation_{ident}*>(lua_touserdata(state, 1));")
        self.line("    lua_rawgeti(state, LUA_REGISTRYINDEX, args->callback_ref);")
        lua_arg_count = 0
        camera_state_parameter = None
        for parameter in callback_params:
            c_type = clean_type(parameter.c_type)
            value = f"args->{parameter.name}"
            if c_type == "const char*":
                self.line(f"    if ({value}) lua_pushstring(state, {value}); else lua_pushnil(state);")
            elif c_type.endswith("*") and base_type(c_type) in self.model.struct_bodies:
                pointee = base_type(c_type)
                self.line(f"    if ({value}) Push_{pointee}(state, *{value}); else lua_pushnil(state);")
                if c_type == "WotbModV3CameraState*":
                    camera_state_parameter = parameter
            elif is_handle(c_type):
                token = "true" if "Token" in c_type else "false"
                self.line(f"    PushGeneratedHandle(state, static_cast<uint64_t>({value}), {token});")
            elif c_type in NUMBER_TYPES:
                self.line(f"    lua_pushnumber(state, static_cast<lua_Number>({value}));")
            else:
                self.line(f"    lua_pushinteger(state, static_cast<lua_Integer>({value}));")
            lua_arg_count += 1
        callback_returns_result = callback.return_type == "WotbModV3Result"
        callback_result_count = 1 if callback_returns_result or camera_state_parameter else 0
        self.line(f"    lua_call(state, {lua_arg_count}, {callback_result_count});")
        if callback_returns_result:
            self.line("    if (lua_isnil(state, -1)) args->callback_result = WOTBMOD_V3_OK;")
            self.line("    else if (lua_isinteger(state, -1)) args->callback_result = static_cast<WotbModV3Result>(lua_tointeger(state, -1));")
            self.line("    else return luaL_error(state, \"callback must return a WotbModV3Result integer or nil\");")
        elif camera_state_parameter:
            name = camera_state_parameter.name
            self.line(f"    if (!lua_isnil(state, -1) && args->{name}) {{")
            self.line("        Arena arena; ArenaInit(&arena);")
            self.line(f"        WotbModV3CameraState updated = *args->{name};")
            self.line(f"        if (!Read_WotbModV3CameraState(state, -1, &updated, WOTBMOD_V3_CAMERA_VERSION, &arena)) {{ ArenaClear(&arena); return lua_error(state); }}")
            self.line(f"        *args->{name} = updated; ArenaClear(&arena);")
            self.line("    }")
        self.line("    return 0;")
        self.line("}")

        callback_signature = ", ".join(
            f"{clean_type(parameter.c_type)} {parameter.name}"
            for parameter in callback.parameters
        )
        self.line(f"{callback.return_type} WOTBMOD_V3_CALL Callback_{ident}({callback_signature}) noexcept {{")
        if any(parameter.name == "mod" for parameter in callback.parameters):
            self.line("    (void)mod;")
        self.line("    const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user_data));")
        self.line("    CallbackRecord* record = ClaimCallback(id);")
        if callback.return_type == "void":
            self.line("    if (!record) return;")
        else:
            self.line("    if (!record) return WOTBMOD_V3_E_CALLBACK_FAULT;")
        invocation_values = ["LUA_NOREF"] + [parameter.name for parameter in callback_params] + ["WOTBMOD_V3_OK"]
        self.line(f"    Invocation_{ident} invocation = {{{', '.join(invocation_values)}}};")
        self.line("    {")
        self.line("        LuaScript::Entry entry(*record->script);")
        self.line("        lua_State* state = entry.state();")
        self.line("        const int callback_ref = RecheckCallback(record, entry.active());")
        self.line("        invocation.callback_ref = callback_ref;")
        self.line("        if (callback_ref != LUA_NOREF) {")
        self.line("            const int top = lua_gettop(state);")
        self.line(f"            if (lua_checkstack(state, {max(8, lua_arg_count + 6)})) {{")
        self.line(f"                lua_pushcfunction(state, &Invoke_{ident});")
        self.line("                lua_pushlightuserdata(state, &invocation);")
        self.line("                if (record->script->ProtectedCall(state, 1, 0) != LuaProtectedCallResult::kOk)")
        self.line("                    invocation.callback_result = WOTBMOD_V3_E_CALLBACK_FAULT;")
        self.line("            } else invocation.callback_result = WOTBMOD_V3_E_CALLBACK_FAULT;")
        self.line("            lua_settop(state, top);")
        self.line("            if (record->one_shot) RetireOneShot(state, record);")
        self.line("        }")
        self.line("    }")
        self.line("    FinishCallback(record);")
        if callback.return_type == "void":
            self.line("    return;")
        else:
            self.line("    return invocation.callback_result;")
        self.line("}")

        self.line(f"int {function}(lua_State* state) noexcept {{")
        self.line("    Context* ctx = GetContext(state);")
        self.emit_slot_guard(slot)
        self.line("    Arena arena; ArenaInit(&arena);")
        if not synchronous and key != "http.request_send_async":
            self.line("    if (!ctx->handles || !ctx->handles->release) { ArenaClear(&arena); lua_pushnil(state); lua_pushstring(state, \"handles.release is required for callback cleanup\"); return 2; }")
        arguments: list[str] = []
        outputs: list[tuple[str, str]] = []
        lua_index = 1
        index = 0
        self.line("    CallbackRecord* callback_record = nullptr;")
        while index < len(params):
            parameter = params[index]
            c_type = clean_type(parameter.c_type)
            if index == 0 and c_type == "WotbModV3Handle" and parameter.name == "mod":
                arguments.append("ctx->mod"); index += 1; continue
            if index == callback_index:
                self.line(f"    callback_record = CreateCallback(state, {lua_index}, ctx, {'true' if one_shot else 'false'});")
                self.line("    if (!callback_record) { ArenaClear(&arena); return 2; }")
                arguments.append(f"&Callback_{ident}")
                lua_index += 1; index += 1; continue
            if parameter.name == "user_data":
                arguments.append("reinterpret_cast<void*>(static_cast<uintptr_t>(callback_record->id))")
                index += 1; continue
            if c_type.endswith("*") and parameter.name.startswith("out_"):
                pointee = base_type(c_type)
                self.line(f"    {pointee} {parameter.name} = {{}};")
                arguments.append("&" + parameter.name)
                outputs.append((pointee, parameter.name))
                index += 1; continue
            argument, lua_index = self.emit_simple_input(slot, parameter, lua_index)
            arguments.append(argument); index += 1
        self.line("    const StackMark mark = MarkStack(state);")
        self.line(f"    const WotbModV3Result result = static_cast<const {slot.interface.api_type}*>(ctx->api)->{slot.field_path}({', '.join(arguments)});")
        cleanup = "CallbackCleanup::kNone"
        cleanup_handle = "WOTBMOD_V3_INVALID_HANDLE"
        http = "nullptr"
        if key == "http.request_send_async":
            cleanup = "CallbackCleanup::kHttpCancel"
            cleanup_handle = "request"
            http = "static_cast<const WotbModV3HttpApiV1*>(ctx->api)"
        elif outputs:
            cleanup = "CallbackCleanup::kHandleRelease"
            cleanup_handle = outputs[0][1]
        self.line(f"    FinishCallbackRegistration(state, callback_record, result == WOTBMOD_V3_OK, {'true' if synchronous else 'false'}, {cleanup}, static_cast<WotbModV3Handle>({cleanup_handle}), {http});")
        self.line("    ArenaClear(&arena);")
        for out_type, out_name in outputs:
            token = "true" if "Token" in out_type else "false"
            self.line(f"    PushGeneratedHandle(state, static_cast<uint64_t>({out_name}), {token});")
        self.line(f"    return PushResultWith(state, result, mark, \"{key}\");")
        self.line("}")

    def emit_direct_slot(self, slot: Slot, function: str) -> None:
        params = list(slot.function.parameters)
        key = f"{slot.interface.lua_name}.{slot.name}"
        start_line = len(self.lines)
        self.line(f"int {function}(lua_State* state) noexcept {{")
        self.line("    Context* ctx = GetContext(state);")
        self.emit_slot_guard(slot)
        self.line("    Arena arena; ArenaInit(&arena);")
        lua_index = 1
        arguments: list[str] = []
        outputs: list[tuple[str, str, bool]] = []
        index = 0
        failed = False
        while index < len(params):
            parameter = params[index]
            c_type = clean_type(parameter.c_type)
            name = parameter.name
            if index == 0 and c_type == "WotbModV3Handle" and name == "mod":
                arguments.append("ctx->mod")
                index += 1
                continue
            # Sized UTF-8 string output. This is the dominant copy-out shape.
            if c_type == "char*" and index + 1 < len(params) and clean_type(params[index + 1].c_type) == "uint32_t*":
                why = "mixed string/output calls use the dedicated copy-out bridge phase"
                self.unsupported[f"{slot.interface.lua_name}.{slot.name}"] = why
                self.lines[start_line:] = [f"int {function}(lua_State* state) noexcept {{ return Unsupported(state, \"{slot.interface.lua_name}.{slot.name}\", \"{why}\"); }}"]
                return
            if c_type == "const char*":
                self.line(f"    const char* {name} = nullptr; if (!CheckArgString(state, {lua_index}, &{name})) {{ ArenaClear(&arena); return 2; }}")
                arguments.append(name); lua_index += 1; index += 1; continue
            if c_type == "const void*" and index + 1 < len(params) and params[index + 1].name.endswith("size"):
                size_name = params[index + 1].name
                self.line(f"    const char* {name}_bytes = nullptr; size_t {name}_size = 0u;")
                self.line(f"    if (!CheckArgBytes(state, {lua_index}, &{name}_bytes, &{name}_size)) {{ ArenaClear(&arena); return 2; }}")
                self.line(f"    if ({name}_size > UINT32_MAX) {{ ArenaClear(&arena); return PushArgumentError(state, {lua_index}, \"at most UINT32_MAX bytes\"); }}")
                arguments.extend([name + "_bytes", f"static_cast<uint32_t>({name}_size)"])
                lua_index += 1; index += 2; continue
            if c_type == "void*":
                self.line(f"    void* {name} = nullptr; if (!ReadOpaquePointer(state, {lua_index}, &{name})) {{ ArenaClear(&arena); return 2; }}")
                arguments.append(name); lua_index += 1; index += 1; continue
            if c_type == "void**" or (c_type.endswith("*") and name.startswith("out_")):
                pointee = base_type(c_type)
                local = name
                if c_type == "void**":
                    self.line(f"    void* {local} = nullptr;")
                    arguments.append("&" + local)
                    outputs.append(("void", local, False))
                else:
                    self.line(f"    {pointee} {local} = {{}};")
                    if pointee in self.model.struct_bodies:
                        field_names = {field.name for field in self.model.data_fields(pointee)}
                        if "struct_size" in field_names:
                            self.line(f"    {local}.struct_size = sizeof({local});")
                        if "api_version" in field_names:
                            version = struct_version_macro(pointee, slot.interface.version_macro)
                            self.line(f"    {local}.api_version = {version};")
                    arguments.append("&" + local)
                    outputs.append((pointee, local, is_handle(pointee)))
                index += 1; continue
            if c_type.endswith("*"):
                pointee = base_type(c_type)
                # BEFORE the generic const-struct branch, not after it.
                #
                # `WotbModV3ConstBuffer` IS a struct, so the generic branch
                # claimed it first and this one was dead code - which is why
                # `vfs.write_file` came out reading a table whose `data` field
                # must be an OPAQUE POINTER. A Lua script cannot make one, so
                # the route was published, callable, and impossible: it
                # answered "argument 2: expected a table, got string" to the
                # only argument anybody could pass. The whole write half of
                # VFS V2 was inert on every client that shipped it.
                if c_type == "const WotbModV3ConstBuffer*":
                    self.line(f"    const char* {name}_bytes = nullptr; size_t {name}_size = 0u;")
                    self.line(f"    if (!CheckArgBytes(state, {lua_index}, &{name}_bytes, &{name}_size)) {{ ArenaClear(&arena); return 2; }}")
                    self.line(f"    WotbModV3ConstBuffer {name} = {{}}; {name}.struct_size = sizeof({name}); {name}.api_version = {slot.interface.version_macro}; {name}.data = {name}_bytes; {name}.size = static_cast<uint32_t>({name}_size);")
                    arguments.append("&" + name); lua_index += 1; index += 1; continue
                if c_type.startswith("const ") and pointee in self.model.struct_bodies:
                    version = struct_version_macro(pointee, slot.interface.version_macro)
                    self.line(f"    {pointee} {name} = {{}};")
                    self.line(f"    if (!Read_{pointee}(state, {lua_index}, &{name}, {version}, &arena)) {{ ArenaClear(&arena); return 2; }}")
                    arguments.append("&" + name); lua_index += 1; index += 1; continue
                why = f"pointer shape {c_type} {name} is not generated yet"
                self.unsupported[f"{slot.interface.lua_name}.{slot.name}"] = why
                self.lines[start_line:] = [f"int {function}(lua_State* state) noexcept {{ return Unsupported(state, \"{slot.interface.lua_name}.{slot.name}\", \"{why}\"); }}"]
                return
            if is_handle(c_type):
                self.line(f"    uint64_t {name}_raw = 0u; if (!ReadAnyHandle(state, {lua_index}, &{name}_raw)) {{ ArenaClear(&arena); return 2; }}")
                self.line(f"    {c_type} {name} = static_cast<{c_type}>({name}_raw);")
                arguments.append(name); lua_index += 1; index += 1; continue
            if c_type in INTEGER_TYPES:
                reader = "ReadUnsigned" if c_type.startswith("u") or c_type in {"size_t", "WotbModV3YamlNodeId"} else "ReadInteger"
                temp = "uint64_t" if reader == "ReadUnsigned" else "int64_t"
                self.line(f"    {temp} {name}_raw = 0; if (!{reader}(state, {lua_index}, &{name}_raw)) {{ ArenaClear(&arena); return 2; }}")
                self.line(f"    {c_type} {name} = static_cast<{c_type}>({name}_raw);")
                arguments.append(name); lua_index += 1; index += 1; continue
            if c_type in NUMBER_TYPES:
                self.line(f"    double {name}_raw = 0.0; if (!ReadNumber(state, {lua_index}, &{name}_raw)) {{ ArenaClear(&arena); return 2; }}")
                self.line(f"    {c_type} {name} = static_cast<{c_type}>({name}_raw);")
                arguments.append(name); lua_index += 1; index += 1; continue
            if c_type in self.model.struct_bodies:
                version = struct_version_macro(c_type, slot.interface.version_macro)
                self.line(f"    {c_type} {name} = {{}}; if (!Read_{c_type}(state, {lua_index}, &{name}, {version}, &arena)) {{ ArenaClear(&arena); return 2; }}")
                arguments.append(name); lua_index += 1; index += 1; continue
            why = f"value type {c_type} is not generated yet"
            self.unsupported[f"{slot.interface.lua_name}.{slot.name}"] = why
            self.lines[start_line:] = [f"int {function}(lua_State* state) noexcept {{ return Unsupported(state, \"{slot.interface.lua_name}.{slot.name}\", \"{why}\"); }}"]
            return

        owned_outputs = [
            out_name for _, out_name, out_handle in outputs if out_handle
        ]
        retain_input = RETAIN_INPUTS.get(key)
        if owned_outputs or retain_input:
            self.line("    if (!ctx->handles || !ctx->handles->release) { ArenaClear(&arena); return Unsupported(state, \"" + key + "\", \"handles.release is required for ownership\"); }")
        api = f"static_cast<const {slot.interface.api_type}*>(ctx->api)"
        field = "->" + slot.field_path.replace(".", ".")
        self.line("    const StackMark mark = MarkStack(state);")
        self.line(f"    const WotbModV3Result result = {api}{field}({', '.join(arguments)});")
        for out_name in owned_outputs:
            self.line(f"    if (result == WOTBMOD_V3_OK && !ctx->script->Ownership().RecordHandle(static_cast<WotbModV3Handle>({out_name}))) {{")
            self.line(f"        ctx->handles->release(ctx->mod, static_cast<WotbModV3Handle>({out_name})); ArenaClear(&arena);")
            self.line(f"        lua_pushnil(state); lua_pushstring(state, \"{key}: this script is being unloaded\"); return 2;")
            self.line("    }")
        if retain_input:
            self.line(f"    if (result == WOTBMOD_V3_OK && !ctx->script->Ownership().RecordHandle(static_cast<WotbModV3Handle>({retain_input}))) {{")
            self.line(f"        ctx->handles->release(ctx->mod, static_cast<WotbModV3Handle>({retain_input})); ArenaClear(&arena);")
            self.line(f"        lua_pushnil(state); lua_pushstring(state, \"{key}: this script is being unloaded\"); return 2;")
            self.line("    }")
        generic_release = GENERIC_RELEASE_INPUTS.get(key)
        if generic_release:
            self.line(f"    if (result == WOTBMOD_V3_OK) ctx->script->Ownership().ForgetHandle(static_cast<WotbModV3Handle>({generic_release}));")
            if key == "handles.release":
                self.line(f"    if (result == WOTBMOD_V3_OK) ForgetCallbackIfDead(state, ctx, static_cast<WotbModV3Handle>({generic_release}));")
        callback_release = CALLBACK_RELEASE_INPUTS.get(key)
        if callback_release:
            self.line(f"    if (result == WOTBMOD_V3_OK) ForgetCallbackHandle(state, ctx->script, static_cast<WotbModV3Handle>({callback_release}));")
        self.line("    ArenaClear(&arena);")
        for out_type, out_name, out_handle in outputs:
            if out_type == "void":
                self.line(f"    if ({out_name}) lua_pushlightuserdata(state, {out_name}); else lua_pushnil(state);")
            elif out_handle:
                token = "true" if "Token" in out_type else "false"
                self.line(f"    PushGeneratedHandle(state, static_cast<uint64_t>({out_name}), {token});")
            elif out_type in INTEGER_TYPES:
                self.line(f"    lua_pushinteger(state, static_cast<lua_Integer>({out_name}));")
            elif out_type in NUMBER_TYPES:
                self.line(f"    lua_pushnumber(state, static_cast<lua_Number>({out_name}));")
            elif out_type in self.model.struct_bodies:
                self.line(f"    Push_{out_type}(state, {out_name});")
            else:
                self.line(f"    lua_pushinteger(state, static_cast<lua_Integer>({out_name}));")
        self.line(f"    return PushResultWith(state, result, mark, \"{slot.interface.lua_name}.{slot.name}\");")
        self.line("}")

    def emit_registration(self) -> None:
        self.line("}}}  // namespace wotbmod::lua::generated")
        self.line("namespace wotbmod { namespace lua {")
        self.line("namespace {")
        self.line("const void* QueryGenerated(const WotbModV3Bootstrap* bootstrap, WotbModV3Handle mod, const char* name, uint32_t version) noexcept {")
        self.line("    if (!bootstrap || !bootstrap->query_interface) return nullptr; const void* api = nullptr;")
        self.line("    return bootstrap->query_interface(mod, name, version, &api) == WOTBMOD_V3_OK ? api : nullptr;")
        self.line("}")
        self.line("}")
        self.line("void RegisterGeneratedBindings(lua_State* state, const WotbModV3Bootstrap* bootstrap, WotbModV3Handle mod, LuaScript* script) {")
        self.line("    const auto* generated_handles = static_cast<const WotbModV3HandlesApiV1*>(QueryGenerated(bootstrap, mod, WOTBMOD_V3_IFACE_HANDLES, WOTBMOD_V3_HANDLES_VERSION));")
        for interface in INTERFACES:
            slots = [slot for slot in self.model.flatten_slots(interface) if f"{interface.lua_name}.{slot.name}" not in CUSTOM_SLOTS]
            if not slots:
                continue
            ident = re.sub(r"\W", "_", interface.lua_name)
            self.line(f"    if (const void* api_{ident} = QueryGenerated(bootstrap, mod, {interface.interface_macro}, {interface.version_macro})) {{")
            self.line("        PushWotbTable(state);")
            self.line(f'        lua_getfield(state, -1, "{interface.lua_name}");')
            self.line("        if (lua_type(state, -1) != LUA_TTABLE) { lua_pop(state, 1); lua_newtable(state); }")
            self.line("        generated::Context* context = static_cast<generated::Context*>(lua_newuserdatauv(state, sizeof(generated::Context), 0));")
            self.line(f"        context->api = api_{ident}; context->handles = generated_handles; context->mod = mod; context->script = script;")
            self.line(f"        static const luaL_Reg funcs_{ident}[] = {{")
            for slot in slots:
                self.line(f'            {{"{slot.name}", &generated::Bind_{symbol(slot)}}},')
            self.line("            {nullptr, nullptr}};")
            permissions = PERMISSIONS[interface.lua_name]
            values = ", ".join(f'"{permission}"' for permission in permissions)
            self.line(f"        static const char* const permissions_{ident}[] = {{{values}}};")
            self.line(f"        SetFuncsGuardedAll(state, funcs_{ident}, 1, script, permissions_{ident}, sizeof(permissions_{ident}) / sizeof(permissions_{ident}[0]), \"{permissions[0]}\");")
            self.line(f'        lua_setfield(state, -2, "{interface.lua_name}");')
            self.line("        lua_pop(state, 1);")
            self.line("    }")
        self.line("}")
        self.line("void ReleaseGeneratedCallbacks(LuaScript* script) { generated::ReleaseCallbacks(script); }")
        self.line("}}  // namespace wotbmod::lua")

    def emit_constants(self) -> None:
        tables = constant_tables(self.model)
        slot_names = {
            (interface.lua_name, slot.name)
            for interface in INTERFACES
            for slot in self.model.flatten_slots(interface)
        }
        shadowed = sorted(
            f"wotb.{interface}.{lua_name}"
            for interface, entries in tables.items()
            for lua_name, _ in entries
            if (interface, lua_name) in slot_names
        )
        if shadowed:
            raise ConstantConflict(
                "constants shadow slots:\n  " + "\n  ".join(shadowed)
            )
        self.line()
        self.lines.extend(r'''namespace wotbmod { namespace lua {

// The ABI's own vocabulary, published so that a script compares against
// wotb.camera.MODE_SNIPER instead of against 3.
//
// Deliberately not part of RegisterGeneratedBindings, for two reasons.
//
// That function wraps each table in `if (QueryGenerated(...))`, which is the
// right test for an operation and the wrong one for a constant. A constant is
// a compile-time fact about the headers this host was built against, not a
// capability of the client it happens to be running in. If wotb.camera.MODE_SNIPER
// were to disappear on a client without the camera interface, every
// `mode == wotb.camera.MODE_SNIPER` in every script would quietly become
// `mode == nil` - a comparison that never fires and never errors. Publishing
// unconditionally means such a comparison is at worst always false because the
// mode was never read, which the script can see.
//
// It also skips interfaces with no non-custom slots, which would drop
// wotb.storage.PATH_* entirely: all 14 storage slots are hand-written.
//
// Takes no Context and no permission fence. A constant is not an operation:
// reading one calls nothing, touches no client state and needs no grant. The
// permission gate stays exactly where the calls are.
//
// Call this AFTER the hand-written registrations (RegisterEvents, RegisterUi,
// RegisterStorage) and after RegisterGeneratedBindings. Those build their
// table with lua_newtable and assign it wholesale, so anything already on
// wotb.events or wotb.ui is discarded; this function merges instead, and is
// safe to run over tables that already exist.
void RegisterGeneratedConstants(lua_State* state) noexcept {
    if (!state || !lua_checkstack(state, 6)) return;
    PushWotbTable(state);'''.splitlines())
        for interface in INTERFACES:
            entries = tables.get(interface.lua_name)
            if not entries:
                continue
            self.line()
            self.line(f'    generated::OpenConstantTable(state, "{interface.lua_name}");')
            group = None
            for lua_name, constant in entries:
                origin = constant.enum_type or f"{constant.header} defines"
                if origin != group:
                    group = origin
                    self.line(f"    // {origin}")
                self.line(
                    f'    generated::SetConstant(state, "{lua_name}", {constant.c_name});'
                )
            self.line(f'    generated::CloseConstantTable(state, "{interface.lua_name}");')
        self.line()
        self.line("    lua_pop(state, 1);")
        self.line("}")
        self.line("}}  // namespace wotbmod::lua")

    def generate(self) -> str:
        self.emit_prelude()
        self.emit_structs()
        for slot in self.model.all_slots():
            self.emit_slot(slot)
        self.emit_registration()
        self.emit_constants()
        return "\n".join(self.lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--include-dir", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()
    mod_api = pathlib.Path(__file__).resolve().parents[1]
    include_dir = args.include_dir or mod_api / "include" / "wotbmod"
    generator = Generator(HeaderModel(include_dir))
    try:
        source = generator.generate()
    except ConstantConflict as conflict:
        # Non-zero, loudly: a duplicate name on one table cannot be allowed to
        # reach a script, because the losing constant is simply gone and every
        # comparison against it silently stops matching.
        print(f"generate_lua_bindings: {conflict}", file=sys.stderr)
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source, encoding="utf-8", newline="\n")
    if args.report:
        total = len(generator.model.all_slots())
        print(f"Lua API slots: {total}")
        print(f"Hand-written: {len(CUSTOM_SLOTS)}")
        print(f"Generated direct: {total - len(CUSTOM_SLOTS) - len(generator.unsupported)}")
        print(f"Generated unavailable: {len(generator.unsupported)}")
        for name, why in sorted(generator.unsupported.items()):
            print(f"  {name}: {why}")
        tables = constant_tables(generator.model)
        excluded = excluded_constants(generator.model)
        published = sum(len(entries) for entries in tables.values())
        print(f"Constants published: {published}")
        print(f"Constants excluded: {len(excluded)}")
        for interface in INTERFACES:
            entries = tables.get(interface.lua_name)
            if entries:
                print(f"  wotb.{interface.lua_name}: {len(entries)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
