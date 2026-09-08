#!/usr/bin/env python3
"""WotbMod SDK developer CLI.

The CLI intentionally uses only the Python standard library.  Its manifest
and archive checks mirror the public V3 preflight contract implemented by
src/v3/data_services.cpp and src/v3/package_loader.cpp.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import zipfile
from dataclasses import asdict, dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterable, Mapping, Sequence


CLI_VERSION = "1.2.0"
MANIFEST_VERSION = 1
EXPECTED_CLIENT_BUILD = "11.20.0.887"
EXPECTED_CLIENT_SHA256 = (
    "4813544d3d6b9f45a357e87f5a14d065bd108c4acd324ac1506cb6d1ad6ff0af"
)
EXPECTED_CLIENT_ARCH = "x86"

MAX_MANIFEST_BYTES = 4 * 1024 * 1024
MAX_ID_BYTES = 95
MAX_NAME_BYTES = 127
MAX_VERSION_BYTES = 47
MAX_PATH_BYTES = 1023
MAX_PERMISSION_BYTES = 127
MAX_MESSAGE_BYTES = 511
MAX_API_REQUIREMENTS = 256
MAX_DEPENDENCIES = 128
MAX_PERMISSIONS = 256
MAX_CLIENT_BUILDS = 64
MAX_CLIENT_HASHES = 64
MAX_ENTRYPOINTS = 32
MAX_RESOURCE_PATTERNS = 4096
MAX_ARCHIVE_ENTRIES = 4096
MAX_ARCHIVE_DEPTH = 16
MAX_ARCHIVE_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_UNPACKED_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_SINGLE_FILE_BYTES = 128 * 1024 * 1024
MAX_DIRECTORY_PACKAGE_BYTES = 512 * 1024 * 1024

SDK_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GAME_ROOT = SDK_ROOT.parents[1]

SAFE_PERMISSIONS = {
    "ges.observe",
    "session.cluster.read",
    "core",
    "ui",
    "ui.create",
    "ui.modify.own",
    "localization",
    "audio",
    "audio.custom",
    "audio.events",
    "resources",
    "resources.mod",
    "filesystem.mod_data",
    "input",
    "input.actions",
    "settings",
    "storage",
    "events.public",
    "entity.public.visible",
    "hangar.scene",
    "vehicle.local.cosmetic",
    "camera.hangar",
    "camera.replay",
    "network.http.allowlisted",
    "content",
}

GAMEPLAY_TWEAK_PERMISSIONS = {
    "gameplay.tweak.camera",
    "gameplay.tweak.hud",
    "gameplay.tweak.hangar",
    "gameplay.tweak.replay",
    "gameplay.tweak.cosmetic",
    "gameplay.tweak.vehicle",
    "gameplay.tweak.projectile_visual",
    "gameplay.tweak.freecam",
}

REVIEWED_PERMISSIONS = {
    "ges.publish",
    "session.cluster.change",
    "packages.manage",
    "battle.ui",
    "battle.render.overlay",
    "camera.battle.read",
    "visible.projectile.events",
    "game.entity.public",
    "ui.modify.game",
    "resources.overlay.game",
    "resources.write.mod_data",
    "hooks.symbol",
    "render.callbacks",
    "bigworld.observe",
    "bigworld.rpc.observe",
    "bigworld.rpc.metadata",
    "client.leave_to_hangar",
    "network.http",
}

UNSAFE_PERMISSIONS = {
    "native.memory",
    "native.memory_patch",
    "native.hook.address",
    "native.hooks",
    "render.native",
    "bigworld.rpc.modify",
}

REGISTERED_PERMISSIONS = (
    SAFE_PERMISSIONS
    | GAMEPLAY_TWEAK_PERMISSIONS
    | REVIEWED_PERMISSIONS
    | UNSAFE_PERMISSIONS
)

WINDOWS_RESERVED_NAMES = {"con", "prn", "aux", "nul"}
SHA256_RE = re.compile(r"^[0-9A-Fa-f]{64}$")
CLIENT_BUILD_RE = re.compile(r"^[A-Za-z0-9._+\-]+$")


class CliError(Exception):
    """A user-facing validation or command error."""


class DuplicateJsonKeyError(ValueError):
    """Raised when a JSON object repeats a key."""


@dataclass(frozen=True)
class PackageFile:
    path: str
    size: int
    sha256: str


@dataclass
class PackageView:
    source_kind: str
    source_path: str
    manifest: dict[str, Any]
    requested_permission_tier: str
    package_sha256: str
    payload_sha256: str
    files: list[PackageFile]
    warnings: list[str]

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["file_count"] = len(self.files)
        result["total_file_bytes"] = sum(item.size for item in self.files)
        result["valid"] = True
        return result


def _utf8_size(value: str) -> int:
    return len(value.encode("utf-8"))


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise CliError(message)


def _json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise DuplicateJsonKeyError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def _load_json_bytes(data: bytes, label: str) -> Any:
    _require(bool(data), f"{label} is empty")
    _require(
        len(data) <= MAX_MANIFEST_BYTES,
        f"{label} exceeds the 4 MiB manifest limit",
    )
    _require(not data.startswith(b"\xef\xbb\xbf"), f"{label} must not use UTF-8 BOM")
    try:
        text = data.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise CliError(f"{label} is not valid UTF-8: {exc}") from exc
    try:
        return json.loads(text, object_pairs_hook=_json_object)
    except (json.JSONDecodeError, DuplicateJsonKeyError) as exc:
        raise CliError(f"{label} is not valid strict JSON: {exc}") from exc


def _read_bounded(path: Path, maximum: int, label: str) -> bytes:
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise CliError(f"cannot stat {label}: {path}: {exc}") from exc
    _require(size <= maximum, f"{label} exceeds {maximum} bytes: {path}")
    try:
        return path.read_bytes()
    except OSError as exc:
        raise CliError(f"cannot read {label}: {path}: {exc}") from exc


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            while True:
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
    except OSError as exc:
        raise CliError(f"cannot hash file: {path}: {exc}") from exc
    return digest.hexdigest()


def _is_ascii_identifier(value: Any, max_bytes: int, allow_slash: bool = False) -> bool:
    if not isinstance(value, str) or not value:
        return False
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError:
        return False
    if len(encoded) >= max_bytes + 1 or value[0] == "." or value[-1] == ".":
        return False
    previous_dot = False
    for index, character in enumerate(value):
        accepted = (
            "a" <= character <= "z"
            or "A" <= character <= "Z"
            or "0" <= character <= "9"
            or character in "_-."
            or (allow_slash and character == "/")
        )
        if not accepted:
            return False
        if allow_slash and character == "/" and (
            index == 0 or index + 1 == len(value)
        ):
            return False
        if character == "." and previous_dot:
            return False
        previous_dot = character == "."
    if allow_slash:
        for segment in value.split("/"):
            if not segment or segment in {".", ".."}:
                return False
    return True


def _parse_semver(value: Any) -> tuple[int, int, int, str] | None:
    if not isinstance(value, str) or not value or _utf8_size(value) > MAX_VERSION_BYTES:
        return None
    without_build = value.split("+", 1)[0]
    if "-" in without_build:
        core, prerelease = without_build.split("-", 1)
        if not prerelease or any(
            not (character.isascii() and (character.isalnum() or character in ".-"))
            for character in prerelease
        ):
            return None
    else:
        core = without_build
        prerelease = ""
    parts = core.split(".")
    if not 1 <= len(parts) <= 3:
        return None
    numbers: list[int] = []
    for part in parts:
        if not part or not part.isascii() or not part.isdigit():
            return None
        if len(part) > 1 and part[0] == "0":
            return None
        try:
            numbers.append(int(part, 10))
        except ValueError:
            return None
    while len(numbers) < 3:
        numbers.append(0)
    return numbers[0], numbers[1], numbers[2], prerelease


def _valid_version_range(value: Any) -> bool:
    if not isinstance(value, str):
        return False
    normalized = value.strip()
    if not normalized:
        return False
    if normalized == "*":
        return True
    if normalized[0] in "^~":
        return _parse_semver(normalized[1:].strip()) is not None
    comparators = normalized.replace(",", " ").split()
    if not comparators:
        return False
    for comparator in comparators:
        if comparator.startswith((">=", "<=")):
            version = comparator[2:].strip()
        elif comparator.startswith((">", "<", "=")):
            version = comparator[1:].strip()
        else:
            version = comparator
        if _parse_semver(version) is None:
            return False
    return True


def _is_windows_absolute(value: str) -> bool:
    if value.startswith(("/", "\\")):
        return True
    return bool(re.match(r"^[A-Za-z]:", value))


def _safe_manifest_pattern(value: Any) -> bool:
    if (
        not isinstance(value, str)
        or not value
        or _utf8_size(value) > MAX_PATH_BYTES
        or "\\" in value
        or "%" in value
        or _is_windows_absolute(value)
    ):
        return False
    for segment in value.split("/"):
        if not segment or segment in {".", ".."}:
            return False
        for character in segment:
            code = ord(character)
            if code < 0x20 or code == 0x7F or character in ":?#":
                return False
    return True


def _safe_manifest_path(value: Any) -> bool:
    return _safe_manifest_pattern(value) and not any(
        marker in value for marker in "*?[]{}"
    )


def _valid_network_permission(value: str) -> bool:
    prefix = "network:https://"
    if not value.startswith(prefix):
        return False
    host = value[len(prefix) :]
    if not host or host[0] == "." or host[-1] == "." or ".." in host:
        return False
    return all(
        character.isascii()
        and (character.isalnum() or character in ".-:")
        for character in host
    )


def _permission_tier(permission: str) -> int:
    if permission in UNSAFE_PERMISSIONS:
        return 3
    if permission in GAMEPLAY_TWEAK_PERMISSIONS:
        return 1
    if _valid_network_permission(permission) or permission in REVIEWED_PERMISSIONS:
        return 2
    if permission in SAFE_PERMISSIONS:
        return 0
    return 3


def _string_array(
    value: Any,
    label: str,
    maximum_items: int,
    maximum_bytes: int,
    *,
    allow_missing: bool = True,
) -> list[str]:
    if value is None and allow_missing:
        return []
    _require(isinstance(value, list), f"{label} must be an array")
    _require(len(value) <= maximum_items, f"{label} exceeds its item limit")
    result: list[str] = []
    seen: set[str] = set()
    for item in value:
        _require(
            isinstance(item, str)
            and bool(item)
            and _utf8_size(item) < maximum_bytes,
            f"{label} contains invalid text",
        )
        _require(item not in seen, f"{label} contains duplicate text: {item}")
        seen.add(item)
        result.append(item)
    return result


def _string_or_array(
    value: Any,
    label: str,
    maximum_items: int,
    maximum_bytes: int,
) -> list[str]:
    if value is None:
        return []
    if isinstance(value, str):
        _require(
            bool(value) and _utf8_size(value) < maximum_bytes,
            f"{label} contains invalid text",
        )
        return [value]
    return _string_array(value, label, maximum_items, maximum_bytes)


def _dependency_section(value: Any, label: str) -> dict[str, str]:
    if value is None:
        return {}
    _require(isinstance(value, dict), f"{label} must be an object")
    _require(len(value) <= MAX_DEPENDENCIES, f"{label} exceeds its item limit")
    result: dict[str, str] = {}
    for dependency_id, version_range in value.items():
        _require(
            _is_ascii_identifier(dependency_id, MAX_ID_BYTES),
            f"{label} contains invalid dependency id: {dependency_id}",
        )
        _require(
            isinstance(version_range, str)
            and _utf8_size(version_range) <= MAX_VERSION_BYTES
            and _valid_version_range(version_range),
            f"{label} contains invalid version range for {dependency_id}",
        )
        result[dependency_id] = version_range
    return result


def validate_manifest(
    manifest: Any,
    *,
    client_build: str | None = EXPECTED_CLIENT_BUILD,
    client_sha256: str | None = EXPECTED_CLIENT_SHA256,
) -> dict[str, Any]:
    _require(isinstance(manifest, dict), "manifest root must be an object")
    version_value = manifest.get("manifest_version")
    _require(
        type(version_value) is int and version_value == MANIFEST_VERSION,
        "manifest_version must equal 1",
    )

    mod_id = manifest.get("id")
    name = manifest.get("name")
    version = manifest.get("version")
    developer = manifest.get("developer")
    _require(
        _is_ascii_identifier(mod_id, MAX_ID_BYTES),
        "manifest id is invalid",
    )
    _require(
        isinstance(name, str) and bool(name) and _utf8_size(name) <= MAX_NAME_BYTES,
        "manifest name is invalid",
    )
    _require(
        isinstance(developer, str)
        and bool(developer)
        and _utf8_size(developer) <= MAX_NAME_BYTES,
        "manifest developer is invalid",
    )
    _require(_parse_semver(version) is not None, "manifest version is invalid")

    package_type = manifest.get("type", "native")
    _require(package_type in {"native", "content"}, "manifest type must be native or content")

    settings_path = manifest.get("settings", "")
    _require(
        isinstance(settings_path, str)
        and (not settings_path or _safe_manifest_path(settings_path)),
        "manifest settings path is unsafe",
    )

    content_path = manifest.get("content", "")
    _require(isinstance(content_path, str), "manifest content path must be text")
    if package_type == "content":
        content_path = content_path or "content.json"
        _require(_safe_manifest_path(content_path), "manifest content path is unsafe")
    else:
        _require(not content_path, "native manifest cannot declare content")

    api = manifest.get("api", {})
    _require(isinstance(api, dict), "manifest api section must be an object")
    _require(len(api) <= MAX_API_REQUIREMENTS, "manifest api section exceeds its item limit")
    for interface_name, version_range in api.items():
        _require(
            _is_ascii_identifier(interface_name, MAX_ID_BYTES),
            f"manifest API name is invalid: {interface_name}",
        )
        _require(
            isinstance(version_range, str)
            and _utf8_size(version_range) <= MAX_VERSION_BYTES
            and _valid_version_range(version_range),
            f"manifest API range is invalid for {interface_name}",
        )

    dependency_sections = {
        "dependencies": _dependency_section(manifest.get("dependencies"), "dependencies"),
        "optional_dependencies": _dependency_section(
            manifest.get("optional_dependencies"), "optional_dependencies"
        ),
        "incompatibilities": _dependency_section(
            manifest.get("incompatibilities"), "incompatibilities"
        ),
    }
    dependency_ids: set[str] = set()
    dependency_count = 0
    for section in dependency_sections.values():
        dependency_count += len(section)
        for dependency_id in section:
            _require(
                dependency_id not in dependency_ids,
                f"dependency id appears in multiple sections: {dependency_id}",
            )
            dependency_ids.add(dependency_id)
    _require(dependency_count <= MAX_DEPENDENCIES, "manifest dependency limit exceeded")

    permissions = _string_array(
        manifest.get("permissions"),
        "permissions",
        MAX_PERMISSIONS,
        MAX_PERMISSION_BYTES + 1,
    )
    normalized_permissions: set[str] = set()
    requested_tier = 0
    for permission in permissions:
        _require(
            _is_ascii_identifier(permission, MAX_PERMISSION_BYTES, allow_slash=True)
            or _valid_network_permission(permission),
            f"manifest permission name is invalid: {permission}",
        )
        lowered = permission.lower()
        _require(
            lowered in REGISTERED_PERMISSIONS or _valid_network_permission(lowered),
            f"manifest permission is not registered: {permission}",
        )
        _require(
            lowered not in normalized_permissions,
            f"manifest permission appears more than once: {permission}",
        )
        normalized_permissions.add(lowered)
        requested_tier = max(requested_tier, _permission_tier(permission))

    resources = _string_array(
        manifest.get("resources"),
        "resources",
        MAX_RESOURCE_PATTERNS,
        MAX_PATH_BYTES + 1,
    )
    for pattern in resources:
        _require(_safe_manifest_pattern(pattern), f"unsafe resource pattern: {pattern}")

    locales = _string_or_array(
        manifest.get("locales"),
        "locales",
        MAX_RESOURCE_PATTERNS,
        MAX_PATH_BYTES + 1,
    )
    locale_patterns: set[str] = set()
    for pattern in locales:
        _require(_safe_manifest_pattern(pattern), f"unsafe locale pattern: {pattern}")
        _require(pattern not in locale_patterns, f"duplicate locale pattern: {pattern}")
        locale_patterns.add(pattern)

    client = manifest.get("client", {})
    _require(isinstance(client, dict), "manifest client section must be an object")
    client_builds = _string_array(
        client.get("builds"),
        "client.builds",
        MAX_CLIENT_BUILDS,
        MAX_NAME_BYTES + 1,
    )
    for build in client_builds:
        _require(
            bool(CLIENT_BUILD_RE.fullmatch(build)),
            f"manifest client build is invalid: {build}",
        )
    client_hashes = _string_array(
        client.get("executable_hashes"),
        "client.executable_hashes",
        MAX_CLIENT_HASHES,
        65,
    )
    normalized_hashes: list[str] = []
    for executable_hash in client_hashes:
        _require(
            bool(SHA256_RE.fullmatch(executable_hash)),
            f"manifest client executable hash is invalid: {executable_hash}",
        )
        normalized_hashes.append(executable_hash.lower())

    if client_build is not None and client_builds:
        _require(
            client_build in client_builds,
            f"client build {client_build} is not in manifest allowlist",
        )
    if client_sha256 is not None and normalized_hashes:
        _require(
            client_sha256.lower() in normalized_hashes,
            "client executable SHA-256 is not in manifest allowlist",
        )

    entrypoints = manifest.get("entrypoints", {})
    _require(isinstance(entrypoints, dict), "manifest entrypoints must be an object")
    _require(len(entrypoints) <= MAX_ENTRYPOINTS, "manifest entrypoint limit exceeded")
    normalized_entrypoints: dict[str, str] = {}
    for platform, relative_path in entrypoints.items():
        _require(
            _is_ascii_identifier(platform, MAX_ID_BYTES),
            f"manifest platform is invalid: {platform}",
        )
        _require(
            isinstance(relative_path, str) and _safe_manifest_path(relative_path),
            f"manifest entrypoint is unsafe for {platform}",
        )
        normalized_entrypoints[platform] = relative_path
    if package_type == "native":
        _require(bool(normalized_entrypoints), "native manifest requires an entrypoint")
        _require(
            "windows-x86" in normalized_entrypoints,
            "native manifest has no windows-x86 entrypoint",
        )
        _require(
            normalized_entrypoints["windows-x86"].lower().endswith(".dll"),
            "windows-x86 entrypoint must be a DLL",
        )
    else:
        _require(
            not normalized_entrypoints,
            "content-only manifest cannot contain native entrypoints",
        )

    signature = manifest.get("signature")
    signature_declared = signature is not None
    if signature_declared:
        _require(isinstance(signature, dict), "manifest signature must be an object")
        for field in ("algorithm", "key_id", "value"):
            field_value = signature.get(field)
            _require(
                isinstance(field_value, str) and bool(field_value),
                f"manifest signature.{field} is required",
            )
        _require(
            _utf8_size(signature["value"]) <= 4096,
            "manifest signature value exceeds 4096 bytes",
        )

    tier_names = ("SAFE", "GAMEPLAY_TWEAK", "REVIEWED", "UNSAFE")
    return {
        "manifest_version": MANIFEST_VERSION,
        "type": package_type,
        "id": mod_id,
        "name": name,
        "version": version,
        "developer": developer,
        "api": dict(api),
        "dependencies": dependency_sections["dependencies"],
        "optional_dependencies": dependency_sections["optional_dependencies"],
        "incompatibilities": dependency_sections["incompatibilities"],
        "permissions": permissions,
        "requested_permission_tier": tier_names[requested_tier],
        "resources": resources,
        "locales": locales,
        "settings": settings_path,
        "content": content_path,
        "entrypoints": normalized_entrypoints,
        "client": {
            "builds": client_builds,
            "executable_hashes": normalized_hashes,
        },
        "signature_declared": signature_declared,
    }


def _is_reparse_or_symlink(path: Path) -> bool:
    try:
        metadata = path.lstat()
    except OSError as exc:
        raise CliError(f"cannot inspect path: {path}: {exc}") from exc
    if stat.S_ISLNK(metadata.st_mode):
        return True
    attributes = getattr(metadata, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400)
    return bool(attributes & reparse_flag)


def _windows_reserved_segment(segment: str) -> bool:
    base = segment.split(".", 1)[0].lower()
    if base in WINDOWS_RESERVED_NAMES:
        return True
    return (
        len(base) == 4
        and base[:3] in {"com", "lpt"}
        and base[3] in "123456789"
    )


def _validate_archive_path(value: str, *, allow_directory: bool = True) -> bool:
    if (
        not value
        or _utf8_size(value) > MAX_PATH_BYTES
        or value[0] == "/"
        or "//" in value
        or "\\" in value
        or "\x00" in value
    ):
        return False
    directory = value.endswith("/")
    if directory and not allow_directory:
        return False
    logical = value[:-1] if directory else value
    if not logical:
        return False
    segments = logical.split("/")
    if len(segments) > MAX_ARCHIVE_DEPTH:
        return False
    for segment in segments:
        if (
            not segment
            or segment in {".", ".."}
            or _utf8_size(segment) > 255
            or segment[-1] in {" ", "."}
            or _windows_reserved_segment(segment)
        ):
            return False
        for character in segment:
            code = ord(character)
            if (
                code < 0x20
                or code == 0x7F
                or character in '<>:"|?*'
            ):
                return False
    return True


def _canonical_relative(root: Path, relative: str, label: str) -> Path:
    _require(_safe_manifest_path(relative), f"{label} path is unsafe: {relative}")
    root_resolved = root.resolve(strict=True)
    candidate = root.joinpath(*PurePosixPath(relative).parts)
    current = root
    for part in PurePosixPath(relative).parts:
        current = current / part
        _require(current.exists(), f"{label} is missing: {relative}")
        _require(
            not _is_reparse_or_symlink(current),
            f"{label} is symlink/reparse-backed: {relative}",
        )
    try:
        resolved = candidate.resolve(strict=True)
        resolved.relative_to(root_resolved)
    except (OSError, ValueError) as exc:
        raise CliError(f"{label} escapes package root: {relative}") from exc
    _require(resolved.is_file(), f"{label} is not a regular file: {relative}")
    return resolved


def _enumerate_directory_files(root: Path) -> list[tuple[str, Path, int]]:
    _require(root.is_dir(), f"package directory does not exist: {root}")
    _require(not _is_reparse_or_symlink(root), f"package root is a symlink/reparse point: {root}")
    result: list[tuple[str, Path, int]] = []
    total = 0

    def visit(directory: Path) -> None:
        nonlocal total
        try:
            entries = sorted(os.scandir(directory), key=lambda item: item.name)
        except OSError as exc:
            raise CliError(f"cannot enumerate package directory: {directory}: {exc}") from exc
        for entry in entries:
            path = Path(entry.path)
            _require(
                not _is_reparse_or_symlink(path),
                f"package contains a symlink/reparse point: {path}",
            )
            try:
                if entry.is_dir(follow_symlinks=False):
                    visit(path)
                    continue
                _require(
                    entry.is_file(follow_symlinks=False),
                    f"package contains a non-regular file: {path}",
                )
                size = entry.stat(follow_symlinks=False).st_size
            except OSError as exc:
                raise CliError(f"cannot inspect package entry: {path}: {exc}") from exc
            relative = path.relative_to(root).as_posix()
            _require(
                _validate_archive_path(relative, allow_directory=False),
                f"package path is unsafe for .wotbmod: {relative}",
            )
            _require(
                size <= MAX_ARCHIVE_SINGLE_FILE_BYTES,
                f"package file exceeds 128 MiB: {relative}",
            )
            total += size
            _require(
                total <= MAX_DIRECTORY_PACKAGE_BYTES,
                "directory package exceeds 512 MiB",
            )
            result.append((relative, path, size))
            _require(
                len(result) <= MAX_ARCHIVE_ENTRIES,
                "directory package exceeds 4096 files",
            )

    visit(root)
    result.sort(key=lambda item: item[0])
    folded: set[str] = set()
    for relative, _, _ in result:
        normalized = relative.casefold()
        _require(
            normalized not in folded,
            f"package contains duplicate Windows path: {relative}",
        )
        folded.add(normalized)
    return result


def _directory_package_hash(files: Iterable[tuple[str, Path, int]]) -> str:
    digest = hashlib.sha256()
    for relative, path, size in files:
        encoded = relative.encode("utf-8")
        digest.update(struct.pack("<I", len(encoded)))
        digest.update(encoded)
        digest.update(struct.pack("<Q", size))
        try:
            with path.open("rb") as stream:
                while True:
                    block = stream.read(1024 * 1024)
                    if not block:
                        break
                    digest.update(block)
        except OSError as exc:
            raise CliError(f"cannot hash package file: {path}: {exc}") from exc
    return digest.hexdigest()


def _loose_package_hash(manifest_path: Path, payload_path: Path) -> str:
    digest = hashlib.sha256()
    for label, path in (("manifest.json", manifest_path), ("payload", payload_path)):
        size = path.stat().st_size
        encoded = label.encode("utf-8")
        digest.update(struct.pack("<I", len(encoded)))
        digest.update(encoded)
        digest.update(struct.pack("<Q", size))
        with path.open("rb") as stream:
            while True:
                block = stream.read(1024 * 1024)
                if not block:
                    break
                digest.update(block)
    return digest.hexdigest()


def _payload_relative(validated: Mapping[str, Any]) -> str:
    if validated["type"] == "native":
        return validated["entrypoints"]["windows-x86"]
    return validated["content"]


def _manifest_warnings(validated: Mapping[str, Any], source_kind: str) -> list[str]:
    warnings: list[str] = []
    if validated["signature_declared"]:
        warnings.append(
            "signature metadata is declared but no trusted signature verifier is available"
        )
    if source_kind == "wotbmod":
        warnings.append(
            "archive integrity is verified by SHA-256/CRC32; signer authenticity is unverified"
        )
    if validated["requested_permission_tier"] == "UNSAFE":
        warnings.append("manifest requests UNSAFE permissions")
    return warnings


def _directory_view(
    root: Path,
    *,
    client_build: str | None,
    client_sha256: str | None,
) -> PackageView:
    root = root.resolve(strict=True)
    files = _enumerate_directory_files(root)
    by_folded = {relative.casefold(): (relative, path, size) for relative, path, size in files}
    _require("manifest.json" in by_folded, "package root manifest.json is missing")
    manifest_relative, manifest_path, _ = by_folded["manifest.json"]
    _require(
        manifest_relative == "manifest.json",
        "directory package manifest must be named exactly manifest.json",
    )
    manifest = _load_json_bytes(
        _read_bounded(manifest_path, MAX_MANIFEST_BYTES, "manifest"),
        "manifest.json",
    )
    validated = validate_manifest(
        manifest,
        client_build=client_build,
        client_sha256=client_sha256,
    )
    payload_relative = _payload_relative(validated)
    payload = _canonical_relative(root, payload_relative, "manifest payload")
    _require(
        payload_relative.casefold() in by_folded,
        f"manifest payload is not part of package: {payload_relative}",
    )
    package_files = [
        PackageFile(relative, size, _sha256_file(path))
        for relative, path, size in files
    ]
    return PackageView(
        source_kind="directory",
        source_path=str(root),
        manifest=validated,
        requested_permission_tier=validated["requested_permission_tier"],
        package_sha256=_directory_package_hash(files),
        payload_sha256=_sha256_file(payload),
        files=package_files,
        warnings=_manifest_warnings(validated, "directory"),
    )


def _loose_manifest_view(
    manifest_path: Path,
    *,
    client_build: str | None,
    client_sha256: str | None,
) -> PackageView:
    manifest_path = manifest_path.resolve(strict=True)
    _require(manifest_path.is_file(), f"manifest does not exist: {manifest_path}")
    _require(
        not _is_reparse_or_symlink(manifest_path),
        f"manifest is symlink/reparse-backed: {manifest_path}",
    )
    manifest = _load_json_bytes(
        _read_bounded(manifest_path, MAX_MANIFEST_BYTES, "manifest"),
        manifest_path.name,
    )
    validated = validate_manifest(
        manifest,
        client_build=client_build,
        client_sha256=client_sha256,
    )
    payload_relative = _payload_relative(validated)
    payload = _canonical_relative(manifest_path.parent, payload_relative, "manifest payload")
    files = [
        PackageFile("manifest.json", manifest_path.stat().st_size, _sha256_file(manifest_path)),
        PackageFile("payload", payload.stat().st_size, _sha256_file(payload)),
    ]
    return PackageView(
        source_kind="loose-manifest",
        source_path=str(manifest_path),
        manifest=validated,
        requested_permission_tier=validated["requested_permission_tier"],
        package_sha256=_loose_package_hash(manifest_path, payload),
        payload_sha256=_sha256_file(payload),
        files=files,
        warnings=_manifest_warnings(validated, "loose-manifest"),
    )


def _zip_entry_is_symlink(info: zipfile.ZipInfo) -> bool:
    mode = (info.external_attr >> 16) & 0xFFFF
    return stat.S_ISLNK(mode)


def _read_store_archive(
    archive_path: Path,
) -> tuple[str, list[PackageFile], dict[str, bytes]]:
    """Open a .wotbmod with the loader's rules and read every entry.

    Returns the archive's SHA-256, its files (sorted by path) and the bytes of
    each file keyed by case-folded path. Store-only, single disk, no ZIP64,
    safe paths, CRC-checked: everything the loader refuses is refused here,
    before any manifest is looked at, so native, content and Lua packages
    share one reader.
    """
    archive_path = archive_path.resolve(strict=True)
    _require(archive_path.is_file(), f"archive does not exist: {archive_path}")
    _require(
        not _is_reparse_or_symlink(archive_path),
        f"archive is symlink/reparse-backed: {archive_path}",
    )
    archive_size = archive_path.stat().st_size
    _require(0 < archive_size <= MAX_ARCHIVE_BYTES, "archive size exceeds 512 MiB")
    package_sha256 = _sha256_file(archive_path)
    try:
        archive = zipfile.ZipFile(archive_path, "r", allowZip64=False)
    except (OSError, zipfile.BadZipFile, zipfile.LargeZipFile) as exc:
        raise CliError(f"invalid .wotbmod ZIP: {exc}") from exc
    with archive:
        entries = archive.infolist()
        _require(bool(entries), "archive has no entries")
        _require(
            len(entries) <= MAX_ARCHIVE_ENTRIES,
            "archive exceeds 4096 entries",
        )
        folded: dict[str, zipfile.ZipInfo] = {}
        total = 0
        package_files: list[PackageFile] = []
        file_bytes_by_folded: dict[str, bytes] = {}
        for info in entries:
            path = info.filename
            _require(
                _validate_archive_path(path),
                f"archive contains unsafe path: {path}",
            )
            normalized = path.casefold()
            _require(
                normalized not in folded,
                f"archive contains duplicate Windows path: {path}",
            )
            folded[normalized] = info
            _require(
                not _zip_entry_is_symlink(info),
                f"archive contains symlink metadata: {path}",
            )
            _require(
                (info.flag_bits & ~0x0800) == 0,
                f"archive entry uses unsupported ZIP flags: {path}",
            )
            _require(
                info.compress_type == zipfile.ZIP_STORED,
                f"archive entry is compressed; store method is required: {path}",
            )
            _require(
                info.compress_size == info.file_size,
                f"stored archive entry size mismatch: {path}",
            )
            _require(
                info.file_size <= MAX_ARCHIVE_SINGLE_FILE_BYTES,
                f"archive entry exceeds 128 MiB: {path}",
            )
            total += info.file_size
            _require(
                total <= MAX_ARCHIVE_UNPACKED_BYTES,
                "archive unpacked size exceeds 512 MiB",
            )
            if info.is_dir():
                continue
            try:
                data = archive.read(info)
            except (OSError, RuntimeError, zipfile.BadZipFile) as exc:
                raise CliError(f"cannot verify archive entry {path}: {exc}") from exc
            _require(
                len(data) == info.file_size,
                f"archive entry has a short read: {path}",
            )
            file_bytes_by_folded[normalized] = data
            package_files.append(PackageFile(path, len(data), _sha256_bytes(data)))
    package_files.sort(key=lambda item: item.path)
    return package_sha256, package_files, file_bytes_by_folded


def _archive_view(
    archive_path: Path,
    *,
    client_build: str | None,
    client_sha256: str | None,
) -> PackageView:
    archive_path = archive_path.resolve(strict=True)
    package_sha256, package_files, file_bytes_by_folded = _read_store_archive(archive_path)
    _require(
        "manifest.json" in file_bytes_by_folded,
        "archive root manifest.json is missing",
    )
    manifest = _load_json_bytes(
        file_bytes_by_folded["manifest.json"],
        "archive manifest.json",
    )
    validated = validate_manifest(
        manifest,
        client_build=client_build,
        client_sha256=client_sha256,
    )
    payload_relative = _payload_relative(validated)
    payload_key = payload_relative.casefold()
    _require(
        payload_key in file_bytes_by_folded,
        f"archive manifest payload is missing: {payload_relative}",
    )
    return PackageView(
        source_kind="wotbmod",
        source_path=str(archive_path),
        manifest=validated,
        requested_permission_tier=validated["requested_permission_tier"],
        package_sha256=package_sha256,
        payload_sha256=_sha256_bytes(file_bytes_by_folded[payload_key]),
        files=package_files,
        warnings=_manifest_warnings(validated, "wotbmod"),
    )



def load_package(
    source: Path,
    *,
    client_build: str | None = EXPECTED_CLIENT_BUILD,
    client_sha256: str | None = EXPECTED_CLIENT_SHA256,
) -> PackageView:
    try:
        source = source.expanduser()
        if source.is_dir():
            return _directory_view(
                source,
                client_build=client_build,
                client_sha256=client_sha256,
            )
        if source.is_file() and source.suffix.lower() == ".wotbmod":
            return _archive_view(
                source,
                client_build=client_build,
                client_sha256=client_sha256,
            )
        if source.is_file() and source.name.lower().endswith(".json"):
            return _loose_manifest_view(
                source,
                client_build=client_build,
                client_sha256=client_sha256,
            )
    except OSError as exc:
        raise CliError(f"cannot inspect package source {source}: {exc}") from exc
    raise CliError(f"expected package directory, manifest JSON, or .wotbmod: {source}")


def _json_print(value: Any) -> None:
    print(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True))


def _print_validation(view: PackageView) -> None:
    print(f"VALID {view.manifest['id']} {view.manifest['version']}")
    print(f"source: {view.source_kind} {view.source_path}")
    print(f"package_sha256: {view.package_sha256}")
    print(f"payload_sha256: {view.payload_sha256}")
    print(f"permission_tier: {view.requested_permission_tier}")
    for warning in view.warnings:
        print(f"warning: {warning}")


def _print_inspection(view: PackageView) -> None:
    manifest = view.manifest
    print(f"ID: {manifest['id']}")
    print(f"Name: {manifest['name']}")
    print(f"Version: {manifest['version']}")
    print(f"Developer: {manifest['developer']}")
    print(f"Type: {manifest['type']}")
    print(f"Source: {view.source_kind}")
    print(f"Source path: {view.source_path}")
    print(f"Permission tier: {view.requested_permission_tier}")
    print(f"Permissions: {', '.join(manifest['permissions']) or '(none)'}")
    print(f"Package SHA-256: {view.package_sha256}")
    print(f"Payload SHA-256: {view.payload_sha256}")
    print(f"Files: {len(view.files)}")
    for item in view.files:
        print(f"  {item.path}  {item.size}  {item.sha256}")
    for warning in view.warnings:
        print(f"Warning: {warning}")


def _client_options(args: argparse.Namespace) -> tuple[str | None, str | None]:
    if args.no_client_check:
        return None, None
    build = args.client_build
    executable_hash = args.client_sha256.lower()
    _require(bool(CLIENT_BUILD_RE.fullmatch(build)), "invalid --client-build")
    _require(bool(SHA256_RE.fullmatch(executable_hash)), "invalid --client-sha256")
    return build, executable_hash


def command_validate(args: argparse.Namespace) -> int:
    build, executable_hash = _client_options(args)
    view = load_package(
        Path(args.source),
        client_build=build,
        client_sha256=executable_hash,
    )
    if args.json:
        _json_print(view.to_dict())
    else:
        _print_validation(view)
    return 0


def command_inspect(args: argparse.Namespace) -> int:
    build, executable_hash = _client_options(args)
    view = load_package(
        Path(args.source),
        client_build=build,
        client_sha256=executable_hash,
    )
    if args.json:
        _json_print(view.to_dict())
    else:
        _print_inspection(view)
    return 0


def _c_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def _native_source(mod_id: str, name: str, developer: str, version: str) -> str:
    return f"""#include "wotb_mod_api_v3.h"

#include <cstring>

namespace {{

void CopyText(char* destination, size_t capacity, const char* source) {{
    if (!destination || capacity == 0u) return;
    destination[0] = '\\0';
    if (!source) return;
#if defined(_MSC_VER)
    strncpy_s(destination, capacity, source, _TRUNCATE);
#else
    std::strncpy(destination, source, capacity - 1u);
    destination[capacity - 1u] = '\\0';
#endif
}}

void WOTBMOD_V3_CALL OnEnable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {{
    if (!bootstrap || !bootstrap->query_interface) return;
    const void* raw_core = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION,
            &raw_core) != WOTBMOD_V3_OK ||
        !raw_core) {{
        return;
    }}
    const auto* core =
        static_cast<const WotbModV3CoreApiV1*>(raw_core);
    core->log(
        mod,
        WOTBMOD_V3_LOG_INFO,
        {_c_string(mod_id)},
        "mod enabled");
}}

void WOTBMOD_V3_CALL OnDisable(
    const WotbModV3Bootstrap* bootstrap,
    WotbModV3Handle mod) {{
    if (!bootstrap || !bootstrap->query_interface) return;
    const void* raw_core = nullptr;
    if (bootstrap->query_interface(
            mod,
            WOTBMOD_V3_IFACE_CORE,
            WOTBMOD_V3_CORE_VERSION,
            &raw_core) != WOTBMOD_V3_OK ||
        !raw_core) {{
        return;
    }}
    const auto* core =
        static_cast<const WotbModV3CoreApiV1*>(raw_core);
    core->log(
        mod,
        WOTBMOD_V3_LOG_INFO,
        {_c_string(mod_id)},
        "mod disabled");
}}

}}  // namespace

WOTBMOD_V3_ENTRY {{
    if (!bootstrap || !out_info ||
        bootstrap->struct_size < sizeof(WotbModV3Bootstrap) ||
        bootstrap->api_version != WOTBMOD_V3_ABI_VERSION ||
        mod == WOTBMOD_V3_INVALID_HANDLE) {{
        return WOTBMOD_V3_E_INVALID_ARGUMENT;
    }}
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = sizeof(*out_info);
    out_info->api_version = WOTBMOD_V3_ABI_VERSION;
    out_info->requested_permission_tier = WOTBMOD_V3_PERMISSION_SAFE;
    CopyText(out_info->id, sizeof(out_info->id), {_c_string(mod_id)});
    CopyText(out_info->name, sizeof(out_info->name), {_c_string(name)});
    CopyText(out_info->version, sizeof(out_info->version), {_c_string(version)});
    CopyText(out_info->author, sizeof(out_info->author), {_c_string(developer)});
    CopyText(
        out_info->description,
        sizeof(out_info->description),
        "WotbMod V3 native mod");
    out_info->on_enable = &OnEnable;
    out_info->on_disable = &OnDisable;
    return WOTBMOD_V3_OK;
}}
"""


def _scaffold_manifest(
    package_type: str,
    mod_id: str,
    name: str,
    version: str,
    developer: str,
    dll_name: str,
) -> dict[str, Any]:
    common: dict[str, Any] = {
        "manifest_version": MANIFEST_VERSION,
        "type": package_type,
        "id": mod_id,
        "name": name,
        "version": version,
        "developer": developer,
        "api": {"wotbmod.core": ">=1 <2"},
        "client": {
            "builds": [EXPECTED_CLIENT_BUILD],
            "executable_hashes": [EXPECTED_CLIENT_SHA256],
        },
        "permissions": ["core"] if package_type == "native" else ["resources.mod"],
        "resources": ["assets/**"],
    }
    if package_type == "native":
        common["entrypoints"] = {
            "windows-x86": f"bin/windows-x86/{dll_name}.dll"
        }
    else:
        common["content"] = "content.json"
    return common


# Lua templates: each is a shipped, host-tested example under examples/,
# copied with the manifest's id/name/version replaced. The example is the
# template, so the two cannot drift apart.
LUA_TEMPLATES: dict[str, str] = {
    "hello": "lua_hello",
    "tour": "lua_facade_tour",
    "panel": "lua_facade_panel",
    "battle": "lua_facade_battle",
    "hud": "lua_hud_tweaks",
    "vehicle": "lua_skin_switcher",
}
LUA_TEMPLATE_FILES = ("main.lua", "README_RU.md", "skins.json")


def _lua_template_root(template: str) -> Path:
    _require(template in LUA_TEMPLATES, f"unknown Lua template: {template}")
    root = SDK_ROOT / "examples" / LUA_TEMPLATES[template]
    _require((root / "manifest.json").is_file(), f"template is missing: {root}")
    return root


def _scaffold_lua_project(
    temporary: Path,
    template: str,
    mod_id: str,
    name: str,
    version: str,
) -> dict[str, Any]:
    root = _lua_template_root(template)
    manifest = _load_json_bytes((root / "manifest.json").read_bytes(), "template manifest")
    _require(isinstance(manifest, dict), "template manifest is not an object")
    manifest["id"] = mod_id
    manifest["name"] = name
    manifest["version"] = version
    manifest.setdefault("entrypoint", "main.lua")
    permissions = manifest.get("permissions", [])
    _require(
        isinstance(permissions, list) and all(isinstance(p, str) for p in permissions),
        "template permissions are not a list of names",
    )
    for permission in permissions:
        _require(
            permission in REGISTERED_PERMISSIONS,
            f"template requests an unregistered permission: {permission}",
        )
    (temporary / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    for filename in LUA_TEMPLATE_FILES:
        source = root / filename
        if source.is_file():
            data = source.read_bytes()
            _require(len(data) <= MAX_ARCHIVE_SINGLE_FILE_BYTES, f"template file too large: {filename}")
            (temporary / filename).write_bytes(data)
    return manifest


def command_new(args: argparse.Namespace) -> int:
    target = Path(args.target).expanduser().resolve(strict=False)
    _require(not target.exists(), f"target already exists: {target}")
    _require(_is_ascii_identifier(args.id, MAX_ID_BYTES), "invalid --id")
    _require(_parse_semver(args.version) is not None, "invalid --version")
    _require(
        bool(args.name) and _utf8_size(args.name) <= MAX_NAME_BYTES,
        "invalid --name",
    )
    _require(
        bool(args.developer) and _utf8_size(args.developer) <= MAX_NAME_BYTES,
        "invalid --developer",
    )
    if args.type == "lua":
        _require(
            args.id.count(".") >= 1 and ".." not in args.id,
            "a Lua mod id needs an author prefix, such as author.mod",
        )
        template = args.template or "hello"
        _lua_template_root(template)
        parent = target.parent
        parent.mkdir(parents=True, exist_ok=True)
        temporary = Path(tempfile.mkdtemp(prefix=f".{target.name}.wotbmod-new-", dir=parent))
        try:
            _scaffold_lua_project(temporary, template, args.id, args.name, args.version)
            temporary.replace(target)
        except BaseException:
            shutil.rmtree(temporary, ignore_errors=True)
            raise
        print(f"Created lua project: {target}")
        print(f"Template: {template} (examples/{LUA_TEMPLATES[template]})")
        print(f"Manifest: {target / 'manifest.json'}")
        print(f"Entrypoint: {target / 'main.lua'}")
        print(f"Install as: <game>\\mods\\lua\\{args.id}\\")
        return 0
    _require(args.template is None, "--template applies to --type lua only")
    dll_name = re.sub(r"[^A-Za-z0-9_]", "_", args.id.rsplit(".", 1)[-1])
    _require(bool(dll_name), "mod id cannot produce a DLL name")
    manifest = _scaffold_manifest(
        args.type,
        args.id,
        args.name,
        args.version,
        args.developer,
        dll_name,
    )
    validate_manifest(manifest)
    parent = target.parent
    parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=f".{target.name}.wotbmod-new-", dir=parent))
    try:
        (temporary / "assets").mkdir()
        (temporary / "manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        if args.type == "native":
            (temporary / "src").mkdir()
            (temporary / "bin" / "windows-x86").mkdir(parents=True)
            (temporary / "src" / "mod.cpp").write_text(
                _native_source(args.id, args.name, args.developer, args.version),
                encoding="utf-8",
                newline="\n",
            )
        else:
            content = {
                "type": "content",
                "id": args.id,
                "name": args.name,
                "version": args.version,
                "overrides": {},
            }
            (temporary / "content.json").write_text(
                json.dumps(content, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
                newline="\n",
            )
        temporary.replace(target)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise
    print(f"Created {args.type} project: {target}")
    print(f"Manifest: {target / 'manifest.json'}")
    if args.type == "native":
        print(f"Source: {target / 'src' / 'mod.cpp'}")
        print(f"Expected DLL: {target / 'bin' / 'windows-x86' / (dll_name + '.dll')}")
    else:
        print(f"Content descriptor: {target / 'content.json'}")
    return 0


def _path_is_within(root: Path, candidate: Path) -> bool:
    try:
        candidate.resolve(strict=False).relative_to(root.resolve(strict=True))
        return True
    except (OSError, ValueError):
        return False


def _write_store_archive(source: Path, archive_path: Path) -> None:
    """Write every file under `source` into a byte-deterministic ZIP-store archive
    (fixed timestamps, no extra fields, sorted paths); removed again on failure."""
    files = _enumerate_directory_files(source)
    try:
        with zipfile.ZipFile(
            archive_path,
            "w",
            compression=zipfile.ZIP_STORED,
            allowZip64=False,
            strict_timestamps=True,
        ) as archive:
            archive.comment = b""
            for relative, physical, _ in files:
                data = _read_bounded(
                    physical,
                    MAX_ARCHIVE_SINGLE_FILE_BYTES,
                    f"package file {relative}",
                )
                info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_STORED
                info.create_system = 0
                info.external_attr = 0o100644 << 16
                info.comment = b""
                info.extra = b""
                archive.writestr(info, data)
    except BaseException:
        try:
            archive_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise


def _create_deterministic_archive(
    source: Path,
    archive_path: Path,
    *,
    client_build: str | None,
    client_sha256: str | None,
) -> PackageView:
    _write_store_archive(source, archive_path)
    try:
        return _archive_view(
            archive_path,
            client_build=client_build,
            client_sha256=client_sha256,
        )
    except BaseException:
        try:
            archive_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise



def command_pack(args: argparse.Namespace) -> int:
    build, executable_hash = _client_options(args)
    source = Path(args.project).expanduser().resolve(strict=True)
    _require(source.is_dir(), f"project is not a directory: {source}")
    view = _directory_view(
        source,
        client_build=build,
        client_sha256=executable_hash,
    )
    if args.output:
        output = Path(args.output).expanduser().resolve(strict=False)
    else:
        filename = f"{view.manifest['id']}-{view.manifest['version']}.wotbmod"
        output = source.parent / filename
    _require(
        not _path_is_within(source, output),
        "archive output must be outside the package source tree",
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary_handle, temporary_name = tempfile.mkstemp(
        prefix=f".{output.name}.",
        suffix=".tmp",
        dir=output.parent,
    )
    os.close(temporary_handle)
    temporary = Path(temporary_name)
    try:
        verified = _create_deterministic_archive(
            source,
            temporary,
            client_build=build,
            client_sha256=executable_hash,
        )
        os.replace(temporary, output)
    except BaseException:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        raise
    result = verified.to_dict()
    result["output"] = str(output)
    result["source_path"] = str(source)
    if args.json:
        _json_print(result)
    else:
        print(f"Packed: {output}")
        print(f"Package SHA-256: {verified.package_sha256}")
        print(f"Payload SHA-256: {verified.payload_sha256}")
        print(f"Files: {len(verified.files)}")
        for item in verified.files:
            print(f"  {item.path}  {item.sha256}")
        for warning in verified.warnings:
            print(f"Warning: {warning}")
    return 0


def _validate_run_client(game_root_value: str) -> tuple[Path, Path]:
    unresolved_root = Path(game_root_value).expanduser()
    _require(unresolved_root.exists(), f"game root does not exist: {unresolved_root}")
    _require(
        not _is_reparse_or_symlink(unresolved_root),
        f"game root is a symlink/reparse point: {unresolved_root}",
    )
    try:
        game_root = unresolved_root.resolve(strict=True)
    except OSError as exc:
        raise CliError(f"cannot resolve game root {unresolved_root}: {exc}") from exc
    _require(game_root.is_dir(), f"game root is not a directory: {game_root}")

    executable = game_root / "wotblitz.exe"
    _require(executable.is_file(), f"client executable is missing: {executable}")
    _require(
        not _is_reparse_or_symlink(executable),
        f"client executable is symlink/reparse-backed: {executable}",
    )
    actual_architecture = _pe_architecture(executable)
    actual_build = _windows_file_version(executable)
    actual_sha256 = _sha256_file(executable)
    _require(
        actual_architecture == EXPECTED_CLIENT_ARCH,
        "target client architecture mismatch: "
        f"expected {EXPECTED_CLIENT_ARCH}, got {actual_architecture or 'unknown'}",
    )
    _require(
        actual_build == EXPECTED_CLIENT_BUILD,
        "target client build mismatch: "
        f"expected {EXPECTED_CLIENT_BUILD}, got {actual_build or 'unknown'}",
    )
    _require(
        actual_sha256 == EXPECTED_CLIENT_SHA256,
        "target client executable SHA-256 mismatch",
    )
    return game_root, executable


def _ensure_run_mods_root(game_root: Path) -> Path:
    mods_root = game_root / "mods"
    try:
        mods_root.mkdir(exist_ok=True)
    except OSError as exc:
        raise CliError(f"cannot create mods directory {mods_root}: {exc}") from exc
    _require(mods_root.is_dir(), f"mods path is not a directory: {mods_root}")
    _require(
        not _is_reparse_or_symlink(mods_root),
        f"mods directory is a symlink/reparse point: {mods_root}",
    )
    return mods_root.resolve(strict=True)


def _copy_file_contents(source: Path, destination: Path, label: str) -> None:
    try:
        with source.open("rb") as input_stream, destination.open("wb") as output_stream:
            shutil.copyfileobj(input_stream, output_stream, length=1024 * 1024)
            output_stream.flush()
            os.fsync(output_stream.fileno())
    except OSError as exc:
        raise CliError(f"cannot copy {label}: {exc}") from exc


def _run_stage_path(mods_root: Path, mod_id: str, suffix: str) -> Path:
    try:
        handle, name = tempfile.mkstemp(
            prefix=f".{mod_id}.wotbmod-run-",
            suffix=suffix,
            dir=mods_root,
        )
    except OSError as exc:
        raise CliError(f"cannot create package staging file in {mods_root}: {exc}") from exc
    os.close(handle)
    return Path(name)


def command_run(args: argparse.Namespace) -> int:
    game_root, executable = _validate_run_client(args.game_root)

    unresolved_source = Path(args.source).expanduser()
    _require(unresolved_source.exists(), f"package source does not exist: {unresolved_source}")
    _require(
        not _is_reparse_or_symlink(unresolved_source),
        f"package source is a symlink/reparse point: {unresolved_source}",
    )
    try:
        source = unresolved_source.resolve(strict=True)
    except OSError as exc:
        raise CliError(f"cannot resolve package source {unresolved_source}: {exc}") from exc
    _require(
        source.is_dir() or (source.is_file() and source.suffix.lower() == ".wotbmod"),
        "run source must be a package directory or .wotbmod archive",
    )
    source_view = load_package(
        source,
        client_build=EXPECTED_CLIENT_BUILD,
        client_sha256=EXPECTED_CLIENT_SHA256,
    )

    prospective_mods_root = game_root / "mods"
    if source.is_dir():
        _require(
            not _path_is_within(source, prospective_mods_root),
            "run staging output must be outside the package source tree",
        )
    mods_root = _ensure_run_mods_root(game_root)
    stage = _run_stage_path(mods_root, source_view.manifest["id"], ".stage")
    backup: Path | None = None
    installed = False
    rollback_required = False
    preserve_backup = False
    target: Path | None = None
    try:
        if source.is_dir():
            staged_view = _create_deterministic_archive(
                source,
                stage,
                client_build=EXPECTED_CLIENT_BUILD,
                client_sha256=EXPECTED_CLIENT_SHA256,
            )
        else:
            _copy_file_contents(source, stage, "package archive to staging")
            staged_view = _archive_view(
                stage,
                client_build=EXPECTED_CLIENT_BUILD,
                client_sha256=EXPECTED_CLIENT_SHA256,
            )
        _require(
            staged_view.manifest["id"] == source_view.manifest["id"],
            "package id changed while creating the staging archive",
        )
        _require(
            staged_view.manifest["version"] == source_view.manifest["version"],
            "package version changed while creating the staging archive",
        )

        target = mods_root / f"{staged_view.manifest['id']}.wotbmod"
        if target.exists():
            _require(target.is_file(), f"installed package path is not a file: {target}")
            _require(
                not _is_reparse_or_symlink(target),
                f"installed package is symlink/reparse-backed: {target}",
            )
            backup = _run_stage_path(mods_root, staged_view.manifest["id"], ".last-good")
            _copy_file_contents(target, backup, "installed last-good package")

        try:
            os.replace(stage, target)
        except OSError as exc:
            raise CliError(f"cannot atomically stage package at {target}: {exc}") from exc
        installed = True
        rollback_required = True

        launch_arguments = list(args.launch_arguments)
        if args.no_launch:
            rollback_required = False
        else:
            try:
                process = subprocess.Popen(
                    [str(executable), *launch_arguments],
                    cwd=game_root,
                    shell=False,
                )
            except OSError as exc:
                raise CliError(f"cannot launch client executable {executable}: {exc}") from exc
            rollback_required = False

        print(
            f"Staged: {staged_view.manifest['id']} "
            f"{staged_view.manifest['version']} -> {target}"
        )
        print(f"Package SHA-256: {staged_view.package_sha256}")
        if args.no_launch:
            print("Launch skipped (--no-launch); package will load on the next client start.")
        else:
            process_id = getattr(process, "pid", None)
            suffix = f" pid={process_id}" if process_id is not None else ""
            print(f"Launched: {executable}{suffix}")
        return 0
    except BaseException as original_error:
        if installed and rollback_required and target is not None:
            try:
                if backup is not None:
                    os.replace(backup, target)
                    backup = None
                else:
                    target.unlink(missing_ok=True)
            except OSError as rollback_error:
                preserve_backup = backup is not None and backup.exists()
                backup_hint = f"; last-good remains at {backup}" if preserve_backup else ""
                raise CliError(
                    f"package staging failed and rollback failed: {rollback_error}{backup_hint}"
                ) from original_error
        raise
    finally:
        try:
            stage.unlink(missing_ok=True)
        except OSError:
            pass
        if backup is not None and not preserve_backup:
            try:
                backup.unlink(missing_ok=True)
            except OSError:
                pass


def command_build(args: argparse.Namespace) -> int:
    project = Path(args.project).expanduser().resolve(strict=True)
    _require(project.is_dir(), f"project is not a directory: {project}")
    manifest_path = project / "manifest.json"
    manifest = _load_json_bytes(
        _read_bounded(manifest_path, MAX_MANIFEST_BYTES, "manifest"),
        "manifest.json",
    )
    validated = validate_manifest(
        manifest,
        client_build=EXPECTED_CLIENT_BUILD,
        client_sha256=EXPECTED_CLIENT_SHA256,
    )
    command = list(args.build_command)
    if command and command[0] == "--":
        command = command[1:]
    _require(
        bool(command),
        "build requires an explicit argv after '--', for example: "
        "wotbmod build . -- cmd /c build.cmd",
    )
    _require(bool(command[0]), "build executable cannot be empty")
    try:
        completed = subprocess.run(
            command,
            cwd=project,
            shell=False,
            check=False,
        )
    except OSError as exc:
        raise CliError(f"cannot start build command {command[0]!r}: {exc}") from exc
    if completed.returncode != 0:
        raise CliError(
            f"build command failed for {validated['id']} with exit code "
            f"{completed.returncode}"
        )
    print(f"Build succeeded: {validated['id']} {validated['version']}")
    return 0


def _pe_architecture(path: Path) -> str | None:
    try:
        with path.open("rb") as stream:
            dos = stream.read(64)
            if len(dos) < 64 or dos[:2] != b"MZ":
                return None
            pe_offset = struct.unpack_from("<I", dos, 0x3C)[0]
            stream.seek(pe_offset)
            header = stream.read(6)
            if len(header) != 6 or header[:4] != b"PE\0\0":
                return None
            machine = struct.unpack_from("<H", header, 4)[0]
    except OSError:
        return None
    return {
        0x014C: "x86",
        0x8664: "x64",
        0xAA64: "arm64",
    }.get(machine, f"machine-0x{machine:04x}")


def _windows_file_version(path: Path) -> str | None:
    if os.name != "nt":
        return None
    system_root = os.environ.get("SystemRoot", r"C:\Windows")
    version_dll = Path(system_root) / "System32" / "version.dll"
    try:
        api = ctypes.WinDLL(str(version_dll), use_last_error=True)
    except OSError:
        return None
    size_fn = api.GetFileVersionInfoSizeW
    size_fn.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)]
    size_fn.restype = ctypes.c_uint32
    get_fn = api.GetFileVersionInfoW
    get_fn.argtypes = [
        ctypes.c_wchar_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_void_p,
    ]
    get_fn.restype = ctypes.c_int
    query_fn = api.VerQueryValueW
    query_fn.argtypes = [
        ctypes.c_void_p,
        ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_uint32),
    ]
    query_fn.restype = ctypes.c_int

    ignored = ctypes.c_uint32(0)
    size = size_fn(str(path), ctypes.byref(ignored))
    if size == 0:
        return None
    buffer = ctypes.create_string_buffer(size)
    if not get_fn(str(path), 0, size, buffer):
        return None
    value_pointer = ctypes.c_void_p()
    value_size = ctypes.c_uint32(0)
    if not query_fn(
        buffer,
        "\\",
        ctypes.byref(value_pointer),
        ctypes.byref(value_size),
    ):
        return None

    class FixedFileInfo(ctypes.Structure):
        _fields_ = [
            ("signature", ctypes.c_uint32),
            ("struct_version", ctypes.c_uint32),
            ("file_version_ms", ctypes.c_uint32),
            ("file_version_ls", ctypes.c_uint32),
            ("product_version_ms", ctypes.c_uint32),
            ("product_version_ls", ctypes.c_uint32),
            ("file_flags_mask", ctypes.c_uint32),
            ("file_flags", ctypes.c_uint32),
            ("file_os", ctypes.c_uint32),
            ("file_type", ctypes.c_uint32),
            ("file_subtype", ctypes.c_uint32),
            ("file_date_ms", ctypes.c_uint32),
            ("file_date_ls", ctypes.c_uint32),
        ]

    info = ctypes.cast(value_pointer, ctypes.POINTER(FixedFileInfo)).contents
    if info.signature != 0xFEEF04BD:
        return None
    return "{}.{}.{}.{}".format(
        info.file_version_ms >> 16,
        info.file_version_ms & 0xFFFF,
        info.file_version_ls >> 16,
        info.file_version_ls & 0xFFFF,
    )


def _compiler_details() -> dict[str, Any]:
    cl_path = shutil.which("cl")
    candidates: list[Path] = []
    program_files_x86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    visual_studio_root = Path(program_files_x86) / "Microsoft Visual Studio" / "2022"
    for edition in ("Community", "BuildTools", "Professional", "Enterprise"):
        candidates.append(
            visual_studio_root
            / edition
            / "VC"
            / "Auxiliary"
            / "Build"
            / "vcvars32.bat"
        )
    vcvars = next((path for path in candidates if path.is_file()), None)
    return {
        "available": bool(cl_path or vcvars),
        "cl": cl_path,
        "vcvars32": str(vcvars) if vcvars else None,
    }


def _path_status(path: Path) -> dict[str, Any]:
    return {"path": str(path), "exists": path.exists()}


def command_doctor(args: argparse.Namespace) -> int:
    game_root = Path(args.game_root).expanduser().resolve(strict=False)
    executable = game_root / "wotblitz.exe"
    executable_exists = executable.is_file() and not _is_reparse_or_symlink(executable)
    actual_sha256 = _sha256_file(executable) if executable_exists else None
    actual_arch = _pe_architecture(executable) if executable_exists else None
    actual_build = _windows_file_version(executable) if executable_exists else None
    fingerprint_match = (
        executable_exists
        and actual_sha256 == EXPECTED_CLIENT_SHA256
        and actual_arch == EXPECTED_CLIENT_ARCH
        and actual_build == EXPECTED_CLIENT_BUILD
    )
    sdk_files = {
        "api_header": _path_status(SDK_ROOT / "include" / "wotb_mod_api_v3.h"),
        "runtime_source": _path_status(SDK_ROOT / "src" / "v3" / "wotb_mod_v3_runtime.cpp"),
        "package_contract": _path_status(
            SDK_ROOT / "docs" / "WOTBMOD_PACKAGE_FORMAT_RU.md"
        ),
    }
    sdk_ok = all(item["exists"] for item in sdk_files.values())
    compiler = _compiler_details()
    runtime = {
        "runtime_library": _path_status(SDK_ROOT / "build" / "wotb_mod_runtime.lib"),
        "loader_library": _path_status(SDK_ROOT / "build" / "wotb_mod_loader.dll"),
        "game_loader": _path_status(game_root / "wotb_mod_loader.dll"),
        "game_proxy": _path_status(game_root / "version.dll"),
        "game_proxy_forwarder": _path_status(game_root / "vorig.dll"),
    }
    runtime_ok = all(item["exists"] for item in runtime.values())
    result = {
        "ok": bool(
            sdk_ok and compiler["available"] and fingerprint_match and runtime_ok),
        "cli_version": CLI_VERSION,
        "python": {
            "executable": sys.executable,
            "version": ".".join(str(value) for value in sys.version_info[:3]),
            "supported": sys.version_info >= (3, 9),
        },
        "sdk": {
            "root": str(SDK_ROOT),
            "ok": sdk_ok,
            "files": sdk_files,
        },
        "compiler": compiler,
        "runtime": runtime,
        "client": {
            "game_root": str(game_root),
            "executable": str(executable),
            "exists": executable_exists,
            "expected_build": EXPECTED_CLIENT_BUILD,
            "actual_build": actual_build,
            "expected_sha256": EXPECTED_CLIENT_SHA256,
            "actual_sha256": actual_sha256,
            "expected_architecture": EXPECTED_CLIENT_ARCH,
            "actual_architecture": actual_arch,
            "fingerprint_match": fingerprint_match,
        },
    }
    if args.json:
        _json_print(result)
    else:
        print(f"WotbMod CLI: {CLI_VERSION}")
        print(f"SDK: {'OK' if sdk_ok else 'MISSING'} {SDK_ROOT}")
        print(
            "Compiler: "
            + ("OK" if compiler["available"] else "MISSING")
            + f" cl={compiler['cl'] or '-'} vcvars32={compiler['vcvars32'] or '-'}"
        )
        print(f"Client: {executable}")
        print(f"  build: {actual_build or '-'} expected={EXPECTED_CLIENT_BUILD}")
        print(f"  arch: {actual_arch or '-'} expected={EXPECTED_CLIENT_ARCH}")
        print(f"  sha256: {actual_sha256 or '-'}")
        print(f"  fingerprint: {'MATCH' if fingerprint_match else 'MISMATCH'}")
        for name, item in runtime.items():
            print(f"Runtime {name}: {'OK' if item['exists'] else 'MISSING'} {item['path']}")
        print(f"Doctor: {'OK' if result['ok'] else 'FAILED'}")
    return 0 if result["ok"] else 2


def _add_client_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--client-build",
        default=EXPECTED_CLIENT_BUILD,
        help=f"client build used for allowlist validation (default: {EXPECTED_CLIENT_BUILD})",
    )
    parser.add_argument(
        "--client-sha256",
        default=EXPECTED_CLIENT_SHA256,
        help="client executable SHA-256 used for allowlist validation",
    )
    parser.add_argument(
        "--no-client-check",
        action="store_true",
        help="validate structure and package contents without client allowlist matching",
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="wotbmod",
        description="WotbMod V3 SDK project, manifest, and package tool",
    )
    parser.add_argument("--version", action="version", version=f"%(prog)s {CLI_VERSION}")
    subparsers = parser.add_subparsers(dest="command", required=True)

    new_parser = subparsers.add_parser("new", help="create a native, content or lua project")
    new_parser.add_argument("target", help="new project directory")
    new_parser.add_argument("--id", required=True, help="stable mod id such as author.mod")
    new_parser.add_argument("--name", required=True, help="display name")
    new_parser.add_argument("--developer", required=True, help="developer name")
    new_parser.add_argument("--version", default="1.0.0", help="initial semantic version")
    new_parser.add_argument(
        "--type",
        choices=("native", "content", "lua"),
        default="native",
        help="project type",
    )
    new_parser.add_argument(
        "--template",
        choices=tuple(sorted(LUA_TEMPLATES)),
        default=None,
        help="Lua template: a shipped facade-first example (default hello)",
    )
    new_parser.set_defaults(handler=command_new)

    validate_parser = subparsers.add_parser(
        "validate", help="strictly validate a manifest, directory, or .wotbmod"
    )
    validate_parser.add_argument("source")
    validate_parser.add_argument("--json", action="store_true")
    _add_client_arguments(validate_parser)
    validate_parser.set_defaults(handler=command_validate)

    inspect_parser = subparsers.add_parser(
        "inspect", help="show validated package metadata and file hashes"
    )
    inspect_parser.add_argument("source")
    inspect_parser.add_argument("--json", action="store_true")
    _add_client_arguments(inspect_parser)
    inspect_parser.set_defaults(handler=command_inspect)

    pack_parser = subparsers.add_parser(
        "pack", help="create a deterministic ZIP-store .wotbmod"
    )
    pack_parser.add_argument("project", help="package directory with root manifest.json")
    pack_parser.add_argument("-o", "--output", help="output .wotbmod path")
    pack_parser.add_argument("--json", action="store_true")
    _add_client_arguments(pack_parser)
    pack_parser.set_defaults(handler=command_pack)

    build_command_parser = subparsers.add_parser(
        "build", help="run an explicit project build argv without shell interpolation"
    )
    build_command_parser.add_argument("project")
    build_command_parser.add_argument(
        "build_command",
        nargs=argparse.REMAINDER,
        help="explicit executable and arguments after '--'",
    )
    build_command_parser.set_defaults(handler=command_build)

    run_parser = subparsers.add_parser(
        "run",
        help="atomically stage a validated package and optionally launch the exact client",
    )
    run_parser.add_argument("source", help="package directory or .wotbmod archive")
    run_parser.add_argument("--game-root", default=str(DEFAULT_GAME_ROOT))
    run_parser.add_argument(
        "--no-launch",
        action="store_true",
        help="stage the package without starting wotblitz.exe",
    )
    run_parser.set_defaults(handler=command_run)

    doctor_parser = subparsers.add_parser(
        "doctor", help="check SDK, compiler, runtime artifacts, and client fingerprint"
    )
    doctor_parser.add_argument("--game-root", default=str(DEFAULT_GAME_ROOT))
    doctor_parser.add_argument("--json", action="store_true")
    doctor_parser.set_defaults(handler=command_doctor)

    # Stage 3 of the platform roadmap: install/uninstall/update/rollback/list,
    # verify/keygen/release/publish/policy. They live in tools/wotbmod_packages.py
    # and use this module's helpers through the object passed here, so a test
    # that patches this module patches what those commands see.
    import importlib
    import importlib.util as _importlib_util

    packages_path = Path(__file__).resolve().parent / "wotbmod_packages.py"
    trust_path = Path(__file__).resolve().parent / "wotbmod_trust.py"
    if "wotbmod_trust" not in sys.modules:
        trust_spec = _importlib_util.spec_from_file_location("wotbmod_trust", trust_path)
        if trust_spec is not None and trust_spec.loader is not None:
            trust_module = _importlib_util.module_from_spec(trust_spec)
            sys.modules["wotbmod_trust"] = trust_module
            trust_spec.loader.exec_module(trust_module)
    scan_path = Path(__file__).resolve().parent / "wotbmod_scan.py"
    if "wotbmod_scan" not in sys.modules:
        scan_spec = _importlib_util.spec_from_file_location("wotbmod_scan", scan_path)
        if scan_spec is not None and scan_spec.loader is not None:
            scan_module = _importlib_util.module_from_spec(scan_spec)
            sys.modules["wotbmod_scan"] = scan_module
            scan_spec.loader.exec_module(scan_module)
    packages_spec = _importlib_util.spec_from_file_location("wotbmod_packages", packages_path)
    if packages_spec is not None and packages_spec.loader is not None:
        packages_module = sys.modules.get("wotbmod_packages")
        if packages_module is None:
            packages_module = _importlib_util.module_from_spec(packages_spec)
            sys.modules["wotbmod_packages"] = packages_module
            packages_spec.loader.exec_module(packages_module)
        packages_module.register(subparsers, sys.modules[__name__])
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    try:
        raw_arguments = list(sys.argv[1:] if argv is None else argv)
        launch_arguments: list[str] = []
        parser_arguments = raw_arguments
        if raw_arguments and raw_arguments[0] == "run":
            try:
                separator = raw_arguments.index("--")
            except ValueError:
                separator = -1
            if separator >= 0:
                parser_arguments = raw_arguments[:separator]
                launch_arguments = raw_arguments[separator + 1 :]
        args = parser.parse_args(parser_arguments)
        if args.command == "run":
            args.launch_arguments = launch_arguments
        handler: Callable[[argparse.Namespace], int] = args.handler
        return handler(args)
    except CliError as exc:
        print(f"wotbmod: error: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("wotbmod: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
