#!/usr/bin/env python3
"""Build the authoritative Lua-binding model from the frozen V3 headers.

This deliberately parses C declarations rather than counting spelling
occurrences.  The public API contains function fields declared both inline and
through typedefs, and its newest version tables embed older binary prefixes.
A grep count therefore both misses slots and counts others more than once.

Only the small, reviewed INTERFACES catalogue below is handwritten.  Slot
names, signatures and embedded prefixes are read from the frozen headers.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
from collections.abc import Iterable


@dataclasses.dataclass(frozen=True)
class Interface:
    lua_name: str
    interface_macro: str
    version_macro: str
    api_type: str
    header: str


@dataclasses.dataclass(frozen=True)
class Parameter:
    c_type: str
    name: str
    array_extent: str | None = None


@dataclasses.dataclass(frozen=True)
class Function:
    return_type: str
    parameters: tuple[Parameter, ...]


@dataclasses.dataclass(frozen=True)
class Slot:
    interface: Interface
    name: str
    field_path: str
    function: Function


@dataclasses.dataclass(frozen=True)
class DataField:
    c_type: str
    name: str
    array_extent: str | None = None
    union_members: tuple["DataField", ...] = ()


@dataclasses.dataclass(frozen=True)
class Constant:
    """One named compile-time value declared by the frozen headers.

    Deliberately carries no value.  The binding generator emits the C name and
    lets the C++ compiler evaluate it, so a constant reaching Lua is the value
    the ABI declares rather than the value a Python re-implementation of the C
    preprocessor believed it to be.  ``header`` is kept because a header, not
    an interface, is what a constant is actually declared in - the newest
    header of an interface is only ever part of its constant surface.
    """

    header: str
    c_name: str
    kind: str  # "enum" | "define"
    enum_type: str | None
    is_string: bool


INTERFACES: tuple[Interface, ...] = (
    Interface("core", "WOTBMOD_V3_IFACE_CORE", "WOTBMOD_V3_CORE_VERSION", "WotbModV3CoreApiV1", "core_v1.h"),
    Interface("capabilities", "WOTBMOD_V3_IFACE_CAPABILITIES", "WOTBMOD_V3_CAPABILITIES_VERSION", "WotbModV3CapabilitiesApiV1", "capabilities_v1.h"),
    Interface("permissions", "WOTBMOD_V3_IFACE_PERMISSIONS", "WOTBMOD_V3_PERMISSIONS_VERSION", "WotbModV3PermissionsApiV1", "permissions_v1.h"),
    Interface("handles", "WOTBMOD_V3_IFACE_HANDLES", "WOTBMOD_V3_HANDLES_VERSION", "WotbModV3HandlesApiV1", "handles_v1.h"),
    Interface("lifecycle", "WOTBMOD_V3_IFACE_LIFECYCLE", "WOTBMOD_V3_LIFECYCLE_VERSION", "WotbModV3LifecycleApiV1", "lifecycle_v1.h"),
    Interface("hooks", "WOTBMOD_V3_IFACE_HOOKS", "WOTBMOD_V3_HOOKS_VERSION", "WotbModV3HooksApiV1", "hooks_v1.h"),
    Interface("unsafe_native", "WOTBMOD_V3_IFACE_UNSAFE_NATIVE", "WOTBMOD_V3_UNSAFE_NATIVE_VERSION", "WotbModV3UnsafeNativeApiV1", "unsafe_native_v1.h"),
    Interface("events", "WOTBMOD_V3_IFACE_EVENTS", "WOTBMOD_V3_EVENTS_VERSION", "WotbModV3EventsApiV1", "events_v1.h"),
    Interface("ui", "WOTBMOD_V3_IFACE_UI", "WOTBMOD_V3_UI_VERSION_3", "WotbModV3UiApiV3", "ui_v3.h"),
    Interface("settings", "WOTBMOD_V3_IFACE_SETTINGS", "WOTBMOD_V3_SETTINGS_VERSION", "WotbModV3SettingsApiV1", "settings_v1.h"),
    Interface("storage", "WOTBMOD_V3_IFACE_STORAGE", "WOTBMOD_V3_STORAGE_VERSION", "WotbModV3StorageApiV1", "storage_v1.h"),
    Interface("input", "WOTBMOD_V3_IFACE_INPUT", "WOTBMOD_V3_INPUT_VERSION", "WotbModV3InputApiV1", "input_v1.h"),
    Interface("vfs", "WOTBMOD_V3_IFACE_VFS", "WOTBMOD_V3_VFS_VERSION_2", "WotbModV3VfsApiV2", "vfs_v2.h"),
    Interface("resources", "WOTBMOD_V3_IFACE_RESOURCES", "WOTBMOD_V3_RESOURCES_VERSION", "WotbModV3ResourcesApiV1", "resources_v1.h"),
    Interface("async", "WOTBMOD_V3_IFACE_ASYNC", "WOTBMOD_V3_ASYNC_VERSION", "WotbModV3AsyncApiV1", "async_v1.h"),
    Interface("http", "WOTBMOD_V3_IFACE_HTTP", "WOTBMOD_V3_HTTP_VERSION", "WotbModV3HttpApiV1", "http_v1.h"),
    Interface("intermod", "WOTBMOD_V3_IFACE_INTERMOD", "WOTBMOD_V3_INTERMOD_VERSION", "WotbModV3IntermodApiV1", "intermod_v1.h"),
    Interface("render", "WOTBMOD_V3_IFACE_RENDER", "WOTBMOD_V3_RENDER_VERSION", "WotbModV3RenderApiV1", "render_v1.h"),
    Interface("render_native", "WOTBMOD_V3_IFACE_RENDER_NATIVE", "WOTBMOD_V3_RENDER_NATIVE_VERSION", "WotbModV3RenderNativeApiV1", "render_v1.h"),
    Interface("camera", "WOTBMOD_V3_IFACE_CAMERA", "WOTBMOD_V3_CAMERA_VERSION", "WotbModV3CameraApiV1", "camera_v1.h"),
    Interface("scene", "WOTBMOD_V3_IFACE_SCENE", "WOTBMOD_V3_SCENE_VERSION", "WotbModV3SceneApiV1", "scene_v1.h"),
    Interface("audio", "WOTBMOD_V3_IFACE_AUDIO", "WOTBMOD_V3_AUDIO_VERSION", "WotbModV3AudioApiV2", "audio_v2.h"),
    Interface("vehicle_visual", "WOTBMOD_V3_IFACE_VEHICLE_VISUAL", "WOTBMOD_V3_VEHICLE_VISUAL_VERSION_2", "WotbModV3VehicleVisualApiV2", "vehicle_visual_v2.h"),
    Interface("gameplay_camera", "WOTBMOD_V3_IFACE_GAMEPLAY_CAMERA", "WOTBMOD_V3_GAMEPLAY_CAMERA_VERSION", "WotbModV3GameplayCameraApiV1", "gameplay_camera_v1.h"),
    Interface("gameplay_hud", "WOTBMOD_V3_IFACE_GAMEPLAY_HUD", "WOTBMOD_V3_GAMEPLAY_HUD_VERSION", "WotbModV3GameplayHudApiV1", "gameplay_hud_v1.h"),
    Interface("gameplay_hangar", "WOTBMOD_V3_IFACE_GAMEPLAY_HANGAR", "WOTBMOD_V3_GAMEPLAY_HANGAR_VERSION", "WotbModV3GameplayHangarApiV1", "gameplay_hangar_v1.h"),
    Interface("gameplay_replay", "WOTBMOD_V3_IFACE_GAMEPLAY_REPLAY", "WOTBMOD_V3_GAMEPLAY_REPLAY_VERSION", "WotbModV3GameplayReplayApiV1", "gameplay_replay_v1.h"),
    Interface("entity_public", "WOTBMOD_V3_IFACE_ENTITY_PUBLIC", "WOTBMOD_V3_ENTITY_PUBLIC_VERSION", "WotbModV3EntityPublicApiV1", "entity_public_v1.h"),
    Interface("bigworld_rpc", "WOTBMOD_V3_IFACE_BIGWORLD_RPC", "WOTBMOD_V3_BIGWORLD_RPC_VERSION", "WotbModV3BigWorldRpcApiV1", "bigworld_rpc_v1.h"),
    Interface("projectile", "WOTBMOD_V3_IFACE_PROJECTILE", "WOTBMOD_V3_PROJECTILE_VERSION_2", "WotbModV3ProjectileApiV2", "projectile_v2.h"),
    Interface("yaml", "WOTBMOD_V3_IFACE_YAML", "WOTBMOD_V3_YAML_VERSION", "WotbModV3YamlApiV1", "yaml_v1.h"),
    Interface("archive", "WOTBMOD_V3_IFACE_ARCHIVE", "WOTBMOD_V3_ARCHIVE_VERSION", "WotbModV3ArchiveApiV1", "archive_v1.h"),
    Interface("loaders", "WOTBMOD_V3_IFACE_LOADERS", "WOTBMOD_V3_LOADERS_VERSION", "WotbModV3LoadersApiV1", "loaders_v1.h"),
    Interface("client", "WOTBMOD_V3_IFACE_CLIENT", "WOTBMOD_V3_CLIENT_VERSION", "WotbModV3ClientApiV1", "client_v1.h"),
    Interface("device", "WOTBMOD_V3_IFACE_DEVICE", "WOTBMOD_V3_DEVICE_VERSION", "WotbModV3DeviceApiV1", "device_v1.h"),
    Interface("diagnostics", "WOTBMOD_V3_IFACE_DIAGNOSTICS", "WOTBMOD_V3_DIAGNOSTICS_VERSION_2", "WotbModV3DiagnosticsApiV2", "diagnostics_v2.h"),
    Interface("devtools", "WOTBMOD_V3_IFACE_DEVTOOLS", "WOTBMOD_V3_DEVTOOLS_VERSION_3", "WotbModV3DevtoolsApiV3", "devtools_v3.h"),
    Interface("manifest", "WOTBMOD_V3_IFACE_MANIFEST", "WOTBMOD_V3_MANIFEST_VERSION", "WotbModV3ManifestApiV1", "manifest_v1.h"),
    Interface("catalog", "WOTBMOD_V3_IFACE_CATALOG", "WOTBMOD_V3_CATALOG_VERSION", "WotbModV3CatalogApiV1", "catalog_v1.h"),
    Interface("content", "WOTBMOD_V3_IFACE_CONTENT", "WOTBMOD_V3_CONTENT_VERSION", "WotbModV3ContentApiV1", "content_v1.h"),
    # Post-RC1 contract, 2026-08-16. Each of these is a NEW interface id with
    # its own availability default and its own permission, not a widening of
    # the frozen table it sits next to -- so each gets its own Lua table
    # rather than merging into ui/camera/audio/scene. The precedent is
    # render/render_native, which likewise share a header and stay separate.
    # Every slot answers NOT_SUPPORTED until its backend lands, and the
    # runtime does not register the interface yet, so no table is created on
    # a client that has not got the backend.
    Interface("ui_read", "WOTBMOD_V3_IFACE_UI_READ", "WOTBMOD_V3_UI_VERSION_4", "WotbModV3UiApiV4", "ui_v4.h"),
    Interface("camera_state", "WOTBMOD_V3_IFACE_CAMERA_STATE", "WOTBMOD_V3_CAMERA_VERSION_2", "WotbModV3CameraApiV2", "camera_v2.h"),
    Interface("audio_intercept", "WOTBMOD_V3_IFACE_AUDIO_INTERCEPT", "WOTBMOD_V3_AUDIO_VERSION_3", "WotbModV3AudioApiV3", "audio_v3.h"),
    Interface("scene_enumerate", "WOTBMOD_V3_IFACE_SCENE_ENUMERATE", "WOTBMOD_V3_SCENE_VERSION_2", "WotbModV3SceneApiV2", "scene_v2.h"),
    Interface("tracer", "WOTBMOD_V3_IFACE_TRACER", "WOTBMOD_V3_TRACER_VERSION", "WotbModV3TracerApiV1", "tracer_v1.h"),
    Interface("ges", "WOTBMOD_V3_IFACE_GES", "WOTBMOD_V3_GES_VERSION", "WotbModV3GesApiV1", "ges_v1.h"),
    # API 1.1, 2026-09-08: the login cluster switch. Its own table, its own
    # permission family; the facade over it is wotb.session.
    Interface("session_cluster", "WOTBMOD_V3_IFACE_SESSION_CLUSTER", "WOTBMOD_V3_SESSION_CLUSTER_VERSION", "WotbModV3SessionClusterApiV1", "session_cluster_v1.h"),
)


_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
_STRUCT_START_RE = re.compile(r"typedef\s+struct\s+(?P<tag>\w+)\s*\{")
_UNION_START_RE = re.compile(r"typedef\s+union\s+(?P<tag>\w+)\s*\{")
_FUNCTION_TYPEDEF_RE = re.compile(
    r"typedef\s+(?P<return>[^;{}]+?)\s*\(\s*WOTBMOD_V3_CALL\s*\*\s*"
    r"(?P<name>\w+)\s*\)\s*\((?P<params>.*?)\)\s*;",
    re.DOTALL,
)
_INLINE_FUNCTION_RE = re.compile(
    r"^(?P<return>.+?)\s*\(\s*WOTBMOD_V3_CALL\s*\*\s*(?P<name>\w+)\s*\)\s*"
    r"\((?P<params>.*)\)$",
    re.DOTALL,
)

# The two shapes a named compile-time value takes in these headers.  Both
# patterns are the ones tools/verify_rc1_contract.py:100-131 already uses for
# the RC1 numeric digest, so the set of names this model sees and the set the
# frozen-ABI digest covers cannot drift apart.  The one deliberate difference
# is that string-valued defines are kept here: an event topic is a constant a
# script needs by name just as much as an enumerator is.
_LINE_CONTINUATION_RE = re.compile(r"\\\r?\n")
_DEFINE_RE = re.compile(
    r"^[ \t]*#[ \t]*define[ \t]+(?P<name>[A-Za-z_]\w*)(?P<call>\()?"
    r"[ \t]*(?P<value>[^\r\n]*)$",
    re.M,
)
_ENUM_TYPEDEF_RE = re.compile(
    r"typedef\s+enum\s+(?P<tag>WotbModV3[A-Za-z0-9_]+)\s*\{(?P<body>.*?)\}\s*"
    r"(?P<name>WotbModV3[A-Za-z0-9_]+)\s*;",
    re.S,
)

ABI_PREFIX = "WOTBMOD_V3_"

# Compilation plumbing: calling convention, linkage and entry-point spelling.
# They expand to C++ tokens rather than to values, so they are not constants at
# all - WOTBMOD_V3_EXPORT is `extern "C" __declspec(dllexport)`.  Named
# explicitly rather than pattern-matched, because the list is short and a new
# member of it must be a reviewed decision.
PLUMBING_MACROS = frozenset(
    {
        "WOTBMOD_V3_CALL",
        "WOTBMOD_V3_EXTERN_C",
        "WOTBMOD_V3_EXPORT",
        "WOTBMOD_V3_ENTRY",
        "WOTBMOD_V3_ENTRY_NAME",
    }
)


def excluded_constant_reason(name: str, function_like: bool) -> str | None:
    """Why this name is not a Lua-visible constant, or None if it is one.

    Every exclusion is a category rather than a list of names, so a header that
    grows a new interface id or a new version macro stays excluded without
    anybody remembering to update a table.
    """
    if function_like:
        # A macro with parameters is code, not a value: WOTBMOD_V3_INIT_STRUCT
        # assigns struct_size and api_version into a caller's struct.
        return "function-like macro"
    if name in PLUMBING_MACROS:
        return "compilation plumbing"
    if name.startswith(ABI_PREFIX + "IFACE_"):
        # Interface ids are the argument to query_interface, which the host
        # calls on the script's behalf.  A script never names one.
        return "interface id"
    if "_VERSION" in name:
        # Interface version numbers are part of the query the host performs,
        # and pinning one from Lua would let a script ask for a table shape
        # this host was not compiled against.
        return "interface version macro"
    return None


def _normalise(text: str) -> str:
    return " ".join(text.replace("\r", " ").replace("\n", " ").split())


def _split_top_level(text: str, delimiter: str) -> list[str]:
    result: list[str] = []
    start = 0
    depth = 0
    for index, character in enumerate(text):
        if character in "([{":
            depth += 1
        elif character in ")]}":
            depth -= 1
        elif character == delimiter and depth == 0:
            result.append(text[start:index])
            start = index + 1
    result.append(text[start:])
    return result


def _parse_parameter(text: str) -> Parameter:
    text = _normalise(text)
    if text == "void":
        raise ValueError("void is a parameter-list marker, not a parameter")
    array = re.match(
        r"^(?P<type>.+?)\s+(?P<name>[A-Za-z_]\w*)\s*\[[^\]]+\]$", text
    )
    if array:
        extent = re.search(r"\[([^\]]+)\]$", text)
        return Parameter(
            _normalise(array.group("type")) + "*",
            array.group("name"),
            _normalise(extent.group(1)) if extent else None,
        )
    match = re.match(r"^(?P<type>.+?)(?P<name>[A-Za-z_]\w*)$", text)
    if not match:
        raise ValueError(f"cannot parse parameter: {text!r}")
    c_type = match.group("type").rstrip()
    name = match.group("name")
    if c_type.endswith("*"):
        c_type = _normalise(c_type)
    return Parameter(c_type, name)


def _parse_function(return_type: str, params: str) -> Function:
    normalised = _normalise(params)
    if not normalised or normalised == "void":
        parameters: tuple[Parameter, ...] = ()
    else:
        parameters = tuple(
            _parse_parameter(piece)
            for piece in _split_top_level(params, ",")
            if _normalise(piece)
        )
    return Function(_normalise(return_type), parameters)


class HeaderModel:
    def __init__(self, include_dir: pathlib.Path):
        self.include_dir = include_dir
        paths = sorted(include_dir.glob("*.h"))
        # Untouched: the slot inventory is derived from this one joined string
        # and is under test at 589 slots.  The constant reader below needs
        # per-file provenance, which this join destroys, so it reads the files
        # again rather than changing how the joined text is built.
        self.text = "\n".join(path.read_text(encoding="utf-8") for path in paths)
        self.text = _COMMENT_RE.sub("", self.text)
        # Same comment stripping, one entry per header.  A constant's header is
        # the only thing that can tell wotb.camera.MODE_SNIPER from
        # wotb.gameplay_camera.TRANSITION_SMOOTH: both spell WOTBMOD_V3_CAMERA_.
        self.sources = {
            path.name: _COMMENT_RE.sub("", path.read_text(encoding="utf-8"))
            for path in paths
        }
        self.function_types = self._function_types()
        self.struct_bodies = self._struct_bodies()
        self.constants = self._constants()

    def _constants(self) -> tuple[Constant, ...]:
        """Every Lua-visible enumerator and value #define, in header order.

        Names are read once per header and in declaration order, so the
        generated file reads like the headers do.  A name declared twice - the
        four event topics that events_v1.h and their owning interface header
        both define under #ifndef guards - is kept once and only if both
        spellings agree; disagreement is a header bug this must not paper over.
        """
        constants: list[Constant] = []
        seen: dict[str, tuple[str, str]] = {}
        for header, source in self.sources.items():
            text = _LINE_CONTINUATION_RE.sub(" ", source)
            found: list[tuple[int, Constant, str]] = []
            for match in _DEFINE_RE.finditer(text):
                name = match.group("name")
                if excluded_constant_reason(name, bool(match.group("call"))):
                    continue
                value = " ".join(match.group("value").split())
                found.append(
                    (
                        match.start(),
                        Constant(header, name, "define", None, value.startswith('"')),
                        value,
                    )
                )
            for match in _ENUM_TYPEDEF_RE.finditer(text):
                tag = match.group("tag")
                if tag != match.group("name"):
                    raise ValueError(f"renamed enum typedef is unsupported: {tag}")
                for item in _split_top_level(match.group("body"), ","):
                    name = _normalise(item).split("=")[0].strip()
                    if not name:
                        continue
                    if excluded_constant_reason(name, False):
                        continue
                    found.append(
                        (match.start(), Constant(header, name, "enum", tag, False), "")
                    )
            for _, constant, value in sorted(found, key=lambda item: item[0]):
                previous = seen.get(constant.c_name)
                if previous is None:
                    seen[constant.c_name] = (header, value)
                    constants.append(constant)
                    continue
                if constant.kind == "enum" or previous[1] != value:
                    raise ValueError(
                        f"{constant.c_name} is declared in both {previous[0]} and "
                        f"{header} with different values"
                    )
        return tuple(constants)

    def _function_types(self) -> dict[str, Function]:
        result: dict[str, Function] = {}
        for match in _FUNCTION_TYPEDEF_RE.finditer(self.text):
            result[match.group("name")] = _parse_function(
                match.group("return"), match.group("params")
            )
        return result

    def _struct_bodies(self) -> dict[str, str]:
        result: dict[str, str] = {}
        for aggregate, pattern in (("struct", _STRUCT_START_RE), ("union", _UNION_START_RE)):
            for match in pattern.finditer(self.text):
                body_start = match.end()
                depth = 1
                cursor = body_start
                while cursor < len(self.text) and depth:
                    if self.text[cursor] == "{":
                        depth += 1
                    elif self.text[cursor] == "}":
                        depth -= 1
                    cursor += 1
                if depth:
                    raise ValueError(f"unterminated {aggregate} {match.group('tag')}")
                tail = re.match(r"\s*(?P<name>\w+)\s*;", self.text[cursor:])
                if not tail:
                    raise ValueError(f"cannot read typedef name for {match.group('tag')}")
                name = tail.group("name")
                if name != match.group("tag"):
                    raise ValueError(f"anonymous/renamed {aggregate} is unsupported: {name}")
                result[name] = self.text[body_start : cursor - 1]
        return result

    def _fields(self, struct_type: str) -> Iterable[tuple[str, str | Function]]:
        try:
            body = self.struct_bodies[struct_type]
        except KeyError as error:
            raise ValueError(f"API struct {struct_type} was not found") from error
        for raw_field in _split_top_level(body, ";"):
            field = _normalise(raw_field)
            if not field:
                continue
            inline = _INLINE_FUNCTION_RE.match(field)
            if inline:
                yield inline.group("name"), _parse_function(
                    inline.group("return"), inline.group("params")
                )
                continue
            typed = re.match(r"^(?P<type>\w+)\s+(?P<name>\w+)$", field)
            if not typed:
                raise ValueError(f"cannot parse {struct_type} field: {field!r}")
            field_type = typed.group("type")
            if field_type in self.function_types:
                yield typed.group("name"), self.function_types[field_type]
            else:
                yield typed.group("name"), field_type

    def data_fields(self, struct_type: str) -> tuple[DataField, ...]:
        """Return non-API struct fields, retaining arrays and simple unions."""
        try:
            body = self.struct_bodies[struct_type]
        except KeyError as error:
            raise ValueError(f"struct {struct_type} was not found") from error
        fields: list[DataField] = []
        for raw_field in _split_top_level(body, ";"):
            field = _normalise(raw_field)
            if not field:
                continue
            union = re.match(
                r"^union\s*\{(?P<body>.*)\}\s*(?P<name>\w+)$", field, re.DOTALL
            )
            if union:
                members: list[DataField] = []
                for raw_member in _split_top_level(union.group("body"), ";"):
                    member = self._parse_data_field(_normalise(raw_member))
                    if member:
                        members.append(member)
                fields.append(
                    DataField("union", union.group("name"), union_members=tuple(members))
                )
                continue
            parsed = self._parse_data_field(field)
            if parsed:
                fields.append(parsed)
        return tuple(fields)

    def _parse_data_field(self, field: str) -> DataField | None:
        if not field:
            return None
        match = re.match(
            r"^(?P<type>(?:const\s+)?[A-Za-z_]\w*(?:\s*\*)?)\s*"
            r"(?P<name>[A-Za-z_]\w*)\s*(?:\[(?P<extent>[^\]]+)\])?$",
            field,
        )
        if not match:
            raise ValueError(f"cannot parse data field: {field!r}")
        return DataField(
            _normalise(match.group("type")),
            match.group("name"),
            _normalise(match.group("extent")) if match.group("extent") else None,
        )

    def flatten_slots(self, interface: Interface) -> list[Slot]:
        slots: list[Slot] = []

        def visit(struct_type: str, prefix: str) -> None:
            for field_name, field in self._fields(struct_type):
                path = f"{prefix}.{field_name}" if prefix else field_name
                if isinstance(field, Function):
                    slots.append(Slot(interface, field_name, path, field))
                elif field in self.struct_bodies and field.endswith(tuple(f"ApiV{i}" for i in range(1, 10))):
                    visit(field, path)

        visit(interface.api_type, "")
        return slots

    def all_slots(self) -> list[Slot]:
        return [slot for interface in INTERFACES for slot in self.flatten_slots(interface)]


def _default_include_dir() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[1] / "include" / "wotbmod"


def _as_json(model: HeaderModel) -> dict[str, object]:
    interfaces = []
    total = 0
    for interface in INTERFACES:
        slots = model.flatten_slots(interface)
        total += len(slots)
        interfaces.append(
            {
                "name": interface.lua_name,
                "interface_macro": interface.interface_macro,
                "version_macro": interface.version_macro,
                "api_type": interface.api_type,
                "header": interface.header,
                "slot_count": len(slots),
                "slots": [
                    {
                        "name": slot.name,
                        "field_path": slot.field_path,
                        "return_type": slot.function.return_type,
                        "parameters": [dataclasses.asdict(parameter) for parameter in slot.function.parameters],
                    }
                    for slot in slots
                ],
            }
        )
    return {"interface_count": len(INTERFACES), "slot_count": total, "interfaces": interfaces}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--include-dir", type=pathlib.Path, default=_default_include_dir())
    parser.add_argument("--json", action="store_true", help="print the complete model")
    args = parser.parse_args()
    model = HeaderModel(args.include_dir)
    payload = _as_json(model)
    if args.json:
        print(json.dumps(payload, indent=2, sort_keys=True))
    else:
        for interface in payload["interfaces"]:
            print(f"{interface['name']}: {interface['slot_count']}")
        print(f"interfaces: {payload['interface_count']}")
        print(f"slots: {payload['slot_count']}")
        print(f"constants: {len(model.constants)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
