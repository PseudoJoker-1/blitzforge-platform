#!/usr/bin/env python3
"""Verify the frozen WotbMod API V3 RC1 public and package contract."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import runpy
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = ROOT / "rc1" / "contract_snapshot.json"
SCHEMA = ROOT / "schemas" / "wotbmod-manifest-v1.schema.json"
RUNTIME = ROOT / "src" / "v3" / "wotb_mod_v3_runtime.cpp"
DATA_SERVICES = ROOT / "src" / "v3" / "data_services.cpp"
CLI = ROOT / "tools" / "wotbmod.py"
DOCS = ROOT / "docs" / "API_V3_RU.md"
SAMPLE_DIRS = (
    "sample_ui_transaction",
    "sample_vehicle_cosmetic",
    "sample_camera_render",
)


def sha256(path: Path) -> str:
    # Git may materialize these frozen text files as CRLF on Windows while the
    # snapshot was recorded from LF blobs. Hash the text contract, not the
    # checkout's line-ending policy; semantic edits still change the digest.
    canonical = path.read_bytes().replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return hashlib.sha256(canonical).hexdigest()


def digest_json(value: Any) -> str:
    encoded = json.dumps(
        value, ensure_ascii=True, sort_keys=True, separators=(",", ":")
    ).encode("ascii")
    return hashlib.sha256(encoded).hexdigest()


def public_files() -> list[Path]:
    files = sorted((ROOT / "include" / "wotbmod").glob("*.h"))
    files.extend(
        (
            ROOT / "include" / "wotbmod" / "wotbmod.hpp",
            ROOT / "include" / "wotb_mod_api_v3.h",
            ROOT / "include" / "wotb_mod_runtime_v3.h",
        )
    )
    return sorted(files)


def parse_interface_ids() -> dict[str, str]:
    text = (ROOT / "include" / "wotbmod" / "interface_ids.h").read_text(
        encoding="utf-8"
    )
    return dict(
        re.findall(r'^#define\s+(WOTBMOD_V3_IFACE_[A-Z0-9_]+)\s+"([^"]+)"', text, re.M)
    )


def parse_permissions() -> dict[str, str]:
    text = RUNTIME.read_text(encoding="utf-8")
    begin = text.index("const PermissionDefinition kPermissions[]")
    end = text.index("};", begin)
    result: dict[str, str] = {}
    for name, tier in re.findall(
        r'\{"([^"]+)",\s*WOTBMOD_V3_PERMISSION_([A-Z_]+)\}',
        text[begin:end],
    ):
        if name in result:
            raise ValueError(f"duplicate runtime permission: {name}")
        result[name] = tier
    return result


def parse_data_permission_registry() -> set[str]:
    text = DATA_SERVICES.read_text(encoding="utf-8")
    begin = text.index("bool RegisteredManifestPermission")
    begin = text.index("permissions = {", begin)
    end = text.index("};", begin)
    return set(re.findall(r'"([a-z][a-z0-9._-]+)"', text[begin:end]))


def parse_interface_statuses(interface_ids: dict[str, str]) -> dict[str, str]:
    text = RUNTIME.read_text(encoding="utf-8")
    begin = text.index("kInterfaceAvailabilityDefaults[]")
    end = text.index("};", begin)
    result: dict[str, str] = {}
    for macro, status in re.findall(
        r"\{(WOTBMOD_V3_IFACE_[A-Z0-9_]+),\s*"
        r"WOTBMOD_V3_CAPABILITY_([A-Z_]+),",
        text[begin:end],
    ):
        if macro not in interface_ids:
            raise ValueError(f"unknown interface macro in availability registry: {macro}")
        result[interface_ids[macro]] = status
    return result


def numeric_contract(files: list[Path]) -> tuple[str, str]:
    macro_values: dict[str, str] = {}
    enum_values: dict[str, str] = {}
    for path in files:
        text = path.read_text(encoding="utf-8")
        relative = path.relative_to(ROOT).as_posix()
        for name, value in re.findall(
            r"^#define\s+(WOTBMOD_V3_[A-Z0-9_]+)\s+([^\r\n\\]+)$",
            text,
            re.M,
        ):
            value = " ".join(value.strip().split())
            if not value.startswith('"') and "(" not in name:
                macro_values[f"{relative}:{name}"] = value
        for enum_name, body in re.findall(
            r"typedef\s+enum\s+(WotbModV3[A-Za-z0-9_]+)\s*\{(.*?)\}\s*"
            r"WotbModV3[A-Za-z0-9_]+\s*;",
            text,
            re.S,
        ):
            body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
            body = re.sub(r"//.*", "", body)
            for item in body.split(","):
                item = " ".join(item.strip().split())
                if not item:
                    continue
                if "=" in item:
                    name, value = item.split("=", 1)
                    enum_values[f"{enum_name}:{name.strip()}"] = value.strip()
                else:
                    enum_values[f"{enum_name}:{item}"] = "<auto>"
    return digest_json(macro_values), digest_json(enum_values)


def package_constant_digest() -> str:
    text = CLI.read_text(encoding="utf-8")
    names = (
        "MANIFEST_VERSION",
        "MAX_MANIFEST_BYTES",
        "MAX_ID_BYTES",
        "MAX_NAME_BYTES",
        "MAX_VERSION_BYTES",
        "MAX_PATH_BYTES",
        "MAX_PERMISSION_BYTES",
        "MAX_API_REQUIREMENTS",
        "MAX_DEPENDENCIES",
        "MAX_PERMISSIONS",
        "MAX_CLIENT_BUILDS",
        "MAX_CLIENT_HASHES",
        "MAX_ENTRYPOINTS",
        "MAX_RESOURCE_PATTERNS",
        "MAX_ARCHIVE_ENTRIES",
        "MAX_ARCHIVE_DEPTH",
        "MAX_ARCHIVE_BYTES",
        "MAX_ARCHIVE_UNPACKED_BYTES",
        "MAX_ARCHIVE_SINGLE_FILE_BYTES",
        "MAX_DIRECTORY_PACKAGE_BYTES",
    )
    values: dict[str, str] = {}
    for name in names:
        match = re.search(rf"^{name}\s*=\s*(.+)$", text, re.M)
        if not match:
            raise ValueError(f"package constant disappeared: {name}")
        values[name] = " ".join(match.group(1).strip().split())
    return digest_json(values)


def load_samples() -> dict[str, Any]:
    result: dict[str, Any] = {}
    for directory in SAMPLE_DIRS:
        path = ROOT / "examples" / directory / "manifest.json"
        manifest = json.loads(path.read_text(encoding="utf-8"))
        result[manifest["id"]] = {
            "type": manifest.get("type", "native"),
            "version": manifest["version"],
            "api": dict(sorted(manifest.get("api", {}).items())),
            "permissions": sorted(manifest.get("permissions", [])),
            "entrypoints": dict(sorted(manifest.get("entrypoints", {}).items())),
        }
    return dict(sorted(result.items()))


def build_contract() -> dict[str, Any]:
    files = public_files()
    interfaces = parse_interface_ids()
    numeric_macros, enum_values = numeric_contract(files)
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    file_hashes = {
        path.relative_to(ROOT).as_posix(): sha256(path) for path in files
    }
    return {
        "contract": "WotbMod API V3 RC1",
        "architecture": "windows-x86",
        "public_files": sorted(file_hashes),
        "public_file_hash_digest": digest_json(file_hashes),
        "interface_ids": dict(sorted(interfaces.items())),
        "interface_statuses": dict(sorted(parse_interface_statuses(interfaces).items())),
        "permissions": dict(sorted(parse_permissions().items())),
        "manifest_schema_sha256": sha256(SCHEMA),
        "manifest_properties": sorted(schema["properties"]),
        "numeric_macro_digest": numeric_macros,
        "enum_value_digest": enum_values,
        "package_constant_digest": package_constant_digest(),
        "samples": load_samples(),
    }


def consistency_errors(contract: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    interface_values = set(contract["interface_ids"].values())
    status_values = set(contract["interface_statuses"])
    if status_values != interface_values:
        errors.append(
            "capability registry differs from interface IDs: missing="
            f"{sorted(interface_values - status_values)} extra={sorted(status_values - interface_values)}"
        )

    runtime_permissions = set(contract["permissions"])
    cli_symbols = runpy.run_path(str(CLI))
    cli_permissions = set(cli_symbols["REGISTERED_PERMISSIONS"])
    if cli_permissions != runtime_permissions:
        errors.append(
            "CLI permission registry mismatch: missing="
            f"{sorted(runtime_permissions - cli_permissions)} extra={sorted(cli_permissions - runtime_permissions)}"
        )
    data_permissions = parse_data_permission_registry()
    if data_permissions != runtime_permissions:
        errors.append(
            "C++ manifest permission registry mismatch: missing="
            f"{sorted(runtime_permissions - data_permissions)} extra={sorted(data_permissions - runtime_permissions)}"
        )

    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    required_properties = {
        "manifest_version", "type", "id", "name", "version", "developer",
        "api", "dependencies", "optional_dependencies", "incompatibilities",
        "permissions", "resources", "locales", "settings", "content",
        "entrypoints", "client", "signature",
    }
    if set(schema.get("properties", {})) != required_properties:
        errors.append("manifest schema property set differs from RC1 validator contract")

    for sample_id, sample in contract["samples"].items():
        unknown_api = set(sample["api"]) - interface_values
        unknown_permissions = set(sample["permissions"]) - runtime_permissions
        if unknown_api:
            errors.append(f"{sample_id} uses unregistered interfaces: {sorted(unknown_api)}")
        if unknown_permissions:
            errors.append(
                f"{sample_id} uses unregistered permissions: {sorted(unknown_permissions)}"
            )
        if sample["type"] == "native" and "windows-x86" not in sample["entrypoints"]:
            errors.append(f"{sample_id} has no windows-x86 entrypoint")

    docs = DOCS.read_text(encoding="utf-8")
    for interface_id in sorted(interface_values):
        if interface_id not in docs:
            errors.append(f"public docs omit interface {interface_id}")
    status_doc = (ROOT / "docs" / "API_STATUS_RU.md").read_text(encoding="utf-8")
    for status in ("HOST_TESTED", "LIVE_TEST_PENDING", "SUPPORTED", "NOT_SUPPORTED"):
        if status not in status_doc:
            errors.append(f"status docs omit {status}")
    freeze = ROOT / "API_V3_RC1_FREEZE.md"
    if freeze.exists():
        freeze_text = freeze.read_text(encoding="utf-8")
        for marker in ("struct_size", "numeric", "experimental", "mutable"):
            if marker.lower() not in freeze_text.lower():
                errors.append(f"freeze document omits {marker}")
    return errors


def compare(expected: Any, actual: Any, path: str = "contract") -> list[str]:
    if type(expected) is not type(actual):
        return [f"{path}: type changed"]
    if isinstance(expected, dict):
        errors: list[str] = []
        for key in sorted(set(expected) | set(actual)):
            if key not in expected:
                errors.append(f"{path}.{key}: added")
            elif key not in actual:
                errors.append(f"{path}.{key}: removed")
            else:
                errors.extend(compare(expected[key], actual[key], f"{path}.{key}"))
        return errors
    if isinstance(expected, list):
        return [] if expected == actual else [f"{path}: list changed"]
    return [] if expected == actual else [f"{path}: {expected!r} -> {actual!r}"]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emit", action="store_true", help="print the current snapshot JSON")
    args = parser.parse_args()
    try:
        actual = build_contract()
        errors = consistency_errors(actual)
    except Exception as exc:
        print(f"RC1 CONSISTENCY ERROR: {exc}", file=sys.stderr)
        return 1
    if args.emit:
        print(json.dumps(actual, ensure_ascii=False, indent=2, sort_keys=True))
        return 0 if not errors else 1
    if not SNAPSHOT.is_file():
        errors.append(f"frozen snapshot is missing: {SNAPSHOT}")
    else:
        expected = json.loads(SNAPSHOT.read_text(encoding="utf-8"))
        errors.extend(compare(expected, actual))
    if errors:
        print(f"RC1 CONTRACT FAILED: {len(errors)} issue(s)", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print(
        "RC1 CONTRACT OK: "
        f"files={len(actual['public_files'])} interfaces={len(actual['interface_ids'])} "
        f"permissions={len(actual['permissions'])} samples={len(actual['samples'])}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
