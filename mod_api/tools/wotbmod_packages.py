"""Install, update, roll back, release and publish `.wotbmod` packages.

This module holds the Stage 3 commands of the WotbMod developer CLI:

    wotbmod install    <source> --game-root <dir> [--yes] [--catalog <src>]
    wotbmod uninstall  <id>     --game-root <dir> [--force]
    wotbmod update     <id>|--all --catalog <src> --game-root <dir> [--check]
    wotbmod rollback   <id>     --game-root <dir>
    wotbmod list                --game-root <dir> [--json]
    wotbmod verify     <artifact> [--sidecar <file>] [--trust-root <dir>]
    wotbmod keygen     --out <private key file> --key-id <id>
    wotbmod release    <project> [-o <dir>] [--sign|--sign-with-key <file>]
    wotbmod publish    <release.json> --to <catalog dir | https://portal/api/v1>
    wotbmod policy              --game-root <dir> [--require-signature on|off]

Everything the client will later judge is judged here first, with the same
rules: the manifest (`wotbmod.load_package`), the client allowlist, the
detached ECDSA P-256 signature against `mods/trust` (`wotbmod_trust`), the
dependency ranges (the loader's semver grammar, mirrored in `matches_range`)
and the incompatibility declarations. A package is copied into `mods/`
only after the user has seen its hash, its permissions and its dependency
plan, and every replacement keeps the previous file so `rollback` can put
it back byte for byte.

The module is wired into `tools/wotbmod.py` through `register(subparsers,
cli)`; it uses the CLI's own helpers (validation, hashing, staging) through
the injected module object so that the tests' patches apply to one module
only. Stdlib only.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any

import wotbmod_scan as scan
import wotbmod_trust as trust

_cli: Any = None  # the wotbmod CLI module, injected by register()

LEDGER_NAME = "wotbmod_installs.json"
BACKUP_PARTS = ("cache", "install_backups", "packages")
TRUST_DIR = "trust"
RELEASE_SCHEMA = 1
INDEX_SCHEMA = 1
LEDGER_SCHEMA = 1
MAX_DOWNLOAD_BYTES = 256 * 1024 * 1024
MAX_INDEX_BYTES = 64 * 1024 * 1024
HTTP_TIMEOUT_SECONDS = 60
TIER_NAMES = ("SAFE", "GAMEPLAY_TWEAK", "REVIEWED", "UNSAFE")
LUA_DIR = "lua"                      # <mods>/lua/<id>/ is what the Lua host loads
LUA_CACHE_PARTS = ("cache", "lua_packages")   # the installed Lua archives (+ .sig) for verification
LUA_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,125}[A-Za-z0-9]$")
LUA_ENTRYPOINT_RE = re.compile(r"^[A-Za-z0-9._-]+\.[Ll][Uu][Aa]$")
RELEASE_SUFFIX = ".release.json"
PUBLISH_ENDPOINT = "releases"
USER_AGENT = "wotbmod-cli"


class CliError(Exception):
    """Raised through the CLI's own CliError once the module is registered."""


def _error(message: str) -> Exception:
    return _cli.CliError(message) if _cli is not None else CliError(message)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise _error(message)


def _now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# --- semantic versions (the loader's grammar) ----------------------------------

def parse_version(version: str) -> tuple[int, int, int, str]:
    parsed = _cli._parse_semver(version)
    _require(parsed is not None, f"invalid semantic version: {version!r}")
    return parsed


def version_key(version: str) -> tuple[int, int, int, int, str]:
    """Sort key with the loader's CompareSemVer order: a release beats its prereleases."""
    major, minor, patch, prerelease = parse_version(version)
    return (major, minor, patch, 0 if prerelease else 1, prerelease)


def compare_versions(left: str, right: str) -> int:
    left_key, right_key = version_key(left), version_key(right)
    return (left_key > right_key) - (left_key < right_key)


def _match_comparator(version: str, comparator: str) -> bool:
    text = comparator
    if text.startswith((">=", "<=")):
        operation, text = text[:2], text[2:].strip()
    elif text[:1] in (">", "<", "="):
        operation, text = text[:1], text[1:].strip()
    else:
        operation = "="
    if _cli._parse_semver(text) is None:
        return False
    comparison = compare_versions(version, text)
    return {
        ">=": comparison >= 0,
        "<=": comparison <= 0,
        ">": comparison > 0,
        "<": comparison < 0,
        "=": comparison == 0,
    }[operation]


def matches_range(version: str, range_text: str) -> bool:
    """`*`, `^1.2.3`, `~1.2.3` or space/comma separated comparators, as the loader reads them."""
    if _cli._parse_semver(version) is None:
        return False
    text = range_text.strip()
    if text in ("", "*"):
        return True
    if text[0] in "^~":
        lower = _cli._parse_semver(text[1:].strip())
        if lower is None or compare_versions(version, text[1:].strip()) < 0:
            return False
        major, minor, _patch, _pre = lower
        if text[0] == "^" and major != 0:
            upper = f"{major + 1}.0.0"
        else:
            upper = f"{major}.{minor + 1}.0"
        return compare_versions(version, upper) < 0
    comparators = text.replace(",", " ").split()
    return bool(comparators) and all(_match_comparator(version, item) for item in comparators)


# --- mods.ini -------------------------------------------------------------------

class ModsIni:
    """Line-preserving edits of the loader's `mods/mods.ini`.

    Only the keys the CLI owns are touched (`[mods] <id>`, `[permissions]
    <id>`, `[policy] require_trusted_signature`); every other line, comment
    and the file's newline style survive a save.
    """

    def __init__(self, path: Path) -> None:
        self.path = path
        self.newline = "\r\n"
        self.lines: list[str] = []
        if path.is_file():
            raw = path.read_bytes().decode("utf-8", errors="surrogateescape")
            self.newline = "\r\n" if "\r\n" in raw else "\n"
            self.lines = raw.splitlines()

    @staticmethod
    def _is_header(line: str, section: str) -> bool:
        stripped = line.strip()
        return stripped.startswith("[") and stripped.endswith("]") and \
            stripped[1:-1].strip().lower() == section.lower()

    def _section(self, section: str) -> tuple[int, int] | None:
        start = None
        for index, line in enumerate(self.lines):
            if start is None:
                if self._is_header(line, section):
                    start = index
            elif line.strip().startswith("["):
                return start, index
        return None if start is None else (start, len(self.lines))

    @staticmethod
    def _split(line: str) -> tuple[str, str] | None:
        if "=" not in line or line.lstrip().startswith((";", "#", "[")):
            return None
        key, value = line.split("=", 1)
        return key.strip(), value.strip()

    def get(self, section: str, key: str) -> str | None:
        bounds = self._section(section)
        if bounds is None:
            return None
        for line in self.lines[bounds[0] + 1:bounds[1]]:
            pair = self._split(line)
            if pair is not None and pair[0].lower() == key.lower():
                return pair[1]
        return None

    def set(self, section: str, key: str, value: str) -> None:
        bounds = self._section(section)
        if bounds is None:
            if self.lines and self.lines[-1].strip():
                self.lines.append("")
            self.lines.extend([f"[{section}]", f"{key}={value}"])
            return
        start, end = bounds
        for index in range(start + 1, end):
            pair = self._split(self.lines[index])
            if pair is not None and pair[0].lower() == key.lower():
                self.lines[index] = f"{key}={value}"
                return
        insert_at = end
        while insert_at > start + 1 and not self.lines[insert_at - 1].strip():
            insert_at -= 1
        self.lines.insert(insert_at, f"{key}={value}")

    def remove(self, section: str, key: str) -> bool:
        bounds = self._section(section)
        if bounds is None:
            return False
        for index in range(bounds[0] + 1, bounds[1]):
            pair = self._split(self.lines[index])
            if pair is not None and pair[0].lower() == key.lower():
                del self.lines[index]
                return True
        return False

    def save(self) -> None:
        text = self.newline.join(self.lines) + self.newline if self.lines else ""
        _atomic_write_bytes(self.path, text.encode("utf-8", errors="surrogateescape"))


def _atomic_write_bytes(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(name, path)
    except BaseException:
        try:
            os.unlink(name)
        except OSError:
            pass
        raise


def _write_json(path: Path, value: Any) -> None:
    _atomic_write_bytes(
        path, (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")
    )


def _read_json(path: Path, label: str, maximum: int = 16 * 1024 * 1024) -> Any:
    data = _cli._read_bounded(path, maximum, label)
    return _cli._load_json_bytes(data, label)


# --- the ledger and backups -------------------------------------------------------

def ledger_path(mods_root: Path) -> Path:
    return mods_root / "cache" / LEDGER_NAME


def load_ledger(mods_root: Path) -> dict[str, Any]:
    path = ledger_path(mods_root)
    if not path.is_file():
        return {"schema": LEDGER_SCHEMA, "packages": {}}
    ledger = _read_json(path, "install ledger")
    _require(isinstance(ledger, dict) and ledger.get("schema") == LEDGER_SCHEMA,
             f"install ledger has an unknown schema: {path}")
    ledger.setdefault("packages", {})
    _require(isinstance(ledger["packages"], dict), "install ledger packages must be an object")
    return ledger


def save_ledger(mods_root: Path, ledger: dict[str, Any]) -> None:
    ledger["schema"] = LEDGER_SCHEMA
    ledger["updated_at"] = _now()
    _write_json(ledger_path(mods_root), ledger)


def backup_root(mods_root: Path) -> Path:
    return mods_root.joinpath(*BACKUP_PARTS)


def _safe_backup_name(version: str, sha256: str) -> str:
    safe_version = re.sub(r"[^A-Za-z0-9._+-]", "_", version)
    return f"{safe_version}-{sha256[:8]}"


def _backup_installed(mods_root: Path, mod_id: str, target: Path, sidecar: Path) -> dict[str, Any]:
    """Copy the currently installed archive (and its sidecar) under cache/install_backups.

    A Lua mod is installed as a folder; its archive lives in cache/lua_packages
    (kept by apply_step) and is what gets backed up. A folder someone copied by
    hand, without an archive, is zipped on the spot so rollback can restore it.
    """
    if target.is_dir():
        cached, cached_sig = lua_cache_paths(mods_root, mod_id)
        if cached.is_file():
            return _backup_installed(mods_root, mod_id, cached, cached_sig)
        folder = backup_root(mods_root) / mod_id
        folder.mkdir(parents=True, exist_ok=True)
        zipped = folder / f".{mod_id}-folder-{secrets.token_hex(4)}.wotbmod"
        _cli._write_store_archive(target, zipped)
        sha256 = _cli._sha256_file(zipped)
        version = "unknown"
        try:
            version = load_lua_archive(zipped).manifest["version"]
        except Exception:
            pass
        final = folder / f"{_safe_backup_name(version, sha256)}.wotbmod"
        os.replace(zipped, final)
        return {"version": version, "sha256": sha256, "backup": str(final.relative_to(mods_root)).replace("\\", "/"),
                "signature_backup": None, "replaced_at": _now(), "kind": "lua"}
    sha256 = _cli._sha256_file(target)
    version = "unknown"
    try:
        version = load_any_package(target, client_check=False).manifest["version"]
    except Exception:  # a corrupt file is still worth keeping for rollback
        pass
    folder = backup_root(mods_root) / mod_id
    folder.mkdir(parents=True, exist_ok=True)
    name = _safe_backup_name(version, sha256)
    backup = folder / f"{name}.wotbmod"
    if not (backup.is_file() and _cli._sha256_file(backup) == sha256):
        _cli._copy_file_contents(target, backup, "installed package backup")
    entry: dict[str, Any] = {
        "version": version,
        "sha256": sha256,
        "backup": str(backup.relative_to(mods_root)).replace("\\", "/"),
        "signature_backup": None,
        "replaced_at": _now(),
    }
    if sidecar.is_file():
        sidecar_backup = folder / f"{name}.wotbmod.sig"
        _cli._copy_file_contents(sidecar, sidecar_backup, "installed signature backup")
        entry["signature_backup"] = str(sidecar_backup.relative_to(mods_root)).replace("\\", "/")
    return entry


# --- what is installed --------------------------------------------------------------

@dataclass
class InstalledPackage:
    id: str
    version: str
    path: Path
    kind: str  # archive | directory | loose
    view: Any


def scan_installed(mods_root: Path) -> dict[str, InstalledPackage]:
    """Every package the loader would consider, by id (client checks off: what
    is on disk is what counts for dependency resolution)."""
    found: dict[str, InstalledPackage] = {}
    if not mods_root.is_dir():
        return found
    candidates: list[tuple[Path, str]] = []
    for entry in sorted(mods_root.iterdir()):
        if entry.name.startswith("."):
            continue
        if entry.is_file() and entry.suffix.lower() == ".wotbmod":
            candidates.append((entry, "archive"))
        elif entry.is_dir() and (entry / "manifest.json").is_file():
            candidates.append((entry, "directory"))
        elif entry.is_file() and entry.name.lower().endswith(".manifest.json"):
            candidates.append((entry, "loose"))
    lua_root = mods_root / LUA_DIR
    if lua_root.is_dir():
        for entry in sorted(lua_root.iterdir()):
            if entry.is_dir() and not entry.name.startswith(".") and (entry / "manifest.json").is_file():
                candidates.append((entry, "lua"))
    resource_root = mods_root.joinpath(*RESOURCE_CACHE_PARTS)
    if resource_root.is_dir():
        for entry in sorted(resource_root.iterdir()):
            if entry.is_file() and entry.suffix.lower() == ".wotbmod" and not entry.name.startswith("."):
                candidates.append((entry, "resource"))
    for path, kind in candidates:
        try:
            view = load_lua_directory(path) if kind == "lua" else load_any_package(path, client_check=False)
        except Exception:
            continue
        mod_id = view.manifest["id"]
        if mod_id in found:
            continue  # the loader keeps the first candidate too
        found[mod_id] = InstalledPackage(mod_id, view.manifest["version"], path, kind, view)
    return found


def _sidecar_for_installed(package: InstalledPackage) -> Path:
    return trust.sidecar_path_for(package.path)


def signature_status_of(view: Any, sidecar: Path, trust_root: Path) -> trust.VerifyResult:
    """The loader's verdict for a validated package: its sidecar against the trust store."""
    release = f"{view.manifest['id']}@{view.manifest['version']}"
    return trust.verify_digest(view.package_sha256, sidecar, trust_root, release=release)


def _trust_root(mods_root: Path) -> Path:
    return mods_root / TRUST_DIR


# --- catalogs -----------------------------------------------------------------------

def _is_url(value: str) -> bool:
    return value.lower().startswith(("http://", "https://"))


def _http_get(url: str, maximum: int, label: str, *, headers: dict[str, str] | None = None) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, **(headers or {})})
    try:
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
            chunks: list[bytes] = []
            total = 0
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                total += len(chunk)
                _require(total <= maximum, f"{label} exceeds {maximum} bytes: {url}")
                chunks.append(chunk)
            return b"".join(chunks)
    except urllib.error.HTTPError as exc:
        raise _error(f"{label}: HTTP {exc.code} from {url}") from exc
    except (urllib.error.URLError, OSError) as exc:
        raise _error(f"{label}: cannot fetch {url}: {exc}") from exc


@dataclass
class CatalogVersion:
    id: str
    version: str
    record: dict[str, Any]

    @property
    def sha256(self) -> str:
        return str(self.record.get("artifact", {}).get("sha256", "")).lower()

    @property
    def status(self) -> str:
        return str(self.record.get("status", "published"))

    @property
    def client_builds(self) -> list[str]:
        return list(self.record.get("client", {}).get("builds", []))

    @property
    def dependencies(self) -> dict[str, str]:
        return dict(self.record.get("dependencies", {}))

    @property
    def prerelease(self) -> bool:
        return bool(parse_version(self.version)[3])


class Catalog:
    """A directory with `index.json` (see `publish --to <dir>`) or an HTTP base
    whose `<base>/index.json` has the same shape; artifacts are fetched relative
    to that index."""

    def __init__(self, source: str) -> None:
        self.source = source
        self.remote = _is_url(source)
        self._index: dict[str, Any] | None = None
        if self.remote:
            base = source if source.endswith("/") else source + "/"
            self.index_url = base if base.lower().endswith(".json/") else urllib.parse.urljoin(base, "index.json")
            if self.index_url.endswith("/"):
                self.index_url = self.index_url[:-1]
            self.root: Path | None = None
        else:
            path = Path(source).expanduser()
            if path.is_file() and path.name.lower().endswith(".json"):
                self.root = path.parent
                self.index_file = path
            else:
                _require(path.is_dir(), f"catalog directory does not exist: {path}")
                self.root = path
                self.index_file = path / "index.json"

    def index(self) -> dict[str, Any]:
        if self._index is None:
            if self.remote:
                data = _http_get(self.index_url, MAX_INDEX_BYTES, "catalog index")
                loaded = _cli._load_json_bytes(data, "catalog index")
            elif self.index_file.is_file():
                loaded = _read_json(self.index_file, "catalog index", MAX_INDEX_BYTES)
            else:
                loaded = {"schema": INDEX_SCHEMA, "packages": {}}
            _require(isinstance(loaded, dict) and loaded.get("schema") == INDEX_SCHEMA,
                     f"catalog index has an unknown schema: {self.source}")
            packages = loaded.setdefault("packages", {})
            _require(isinstance(packages, dict), "catalog index packages must be an object")
            self._index = loaded
        return self._index

    def notice(self) -> str:
        """The operator's site-wide announcement (a client patch, maintenance); empty when none."""
        value = self.index().get("notice", "")
        return value.strip() if isinstance(value, str) else ""

    def revoked(self) -> list[dict[str, str]]:
        """Releases the portal pulled after they were published: [{id, version|*, reason}].
        A catalogue without the field (schema 1 before 2026-09-08) revokes nothing."""
        raw = self.index().get("revoked", [])
        result: list[dict[str, str]] = []
        if isinstance(raw, list):
            for item in raw:
                if isinstance(item, dict) and isinstance(item.get("id"), str) and item["id"]:
                    result.append({"id": item["id"], "version": str(item.get("version", "*") or "*"),
                                   "reason": str(item.get("reason", "revoked"))})
        return result

    def is_revoked(self, mod_id: str, version: str) -> str | None:
        """The reason when `mod_id@version` (or the whole mod) is revoked, else None."""
        for item in self.revoked():
            if item["id"].lower() == mod_id.lower() and item["version"] in ("*", version):
                return item["reason"]
        return None

    def versions(self, mod_id: str) -> list[CatalogVersion]:
        entry = self.index()["packages"].get(mod_id)
        if not isinstance(entry, dict):
            return []
        versions = entry.get("versions", {})
        _require(isinstance(versions, dict), f"catalog entry for {mod_id} is malformed")
        result: list[CatalogVersion] = []
        for version, record in versions.items():
            if isinstance(record, dict) and _cli._parse_semver(version) is not None:
                result.append(CatalogVersion(mod_id, version, record))
        result.sort(key=lambda item: version_key(item.version), reverse=True)
        return result

    def best(self, mod_id: str, version_range: str = "*", *, prerelease: bool = False,
             client_build: str | None = None) -> CatalogVersion | None:
        for candidate in self.versions(mod_id):
            if candidate.status not in ("published", "listed"):
                continue
            if candidate.prerelease and not prerelease:
                continue
            if not matches_range(candidate.version, version_range):
                continue
            if client_build is not None and candidate.client_builds and \
                    client_build not in candidate.client_builds:
                continue
            return candidate
        return None

    def _reference(self, section: dict[str, Any]) -> str | None:
        for key in ("url", "path", "file"):
            value = section.get(key)
            if isinstance(value, str) and value:
                return value
        return None

    def fetch(self, candidate: CatalogVersion, into: Path) -> tuple[Path, Path | None]:
        """The artifact (hash-checked against the record) and its sidecar, if any."""
        artifact_section = candidate.record.get("artifact", {})
        _require(isinstance(artifact_section, dict), "catalog record artifact must be an object")
        reference = self._reference(artifact_section)
        _require(reference is not None, f"catalog record for {candidate.id}@{candidate.version} names no artifact")
        expected = candidate.sha256
        _require(bool(_cli.SHA256_RE.fullmatch(expected)), "catalog record has no valid artifact sha256")
        artifact = into / f"{candidate.id}-{candidate.version}.wotbmod"
        self._fetch_into(reference, artifact, MAX_DOWNLOAD_BYTES, "package download")
        actual = _cli._sha256_file(artifact)
        _require(actual == expected,
                 f"downloaded package hash mismatch for {candidate.id}@{candidate.version}: "
                 f"expected {expected}, got {actual}")
        sidecar = None
        signature_section = candidate.record.get("signature")
        if isinstance(signature_section, dict):
            signature_reference = self._reference(signature_section)
            if signature_reference is not None:
                sidecar = into / (artifact.name + ".sig")
                self._fetch_into(signature_reference, sidecar, trust.MAX_SIDECAR_BYTES, "signature download")
        return artifact, sidecar

    def _fetch_into(self, reference: str, destination: Path, maximum: int, label: str) -> None:
        if self.remote or _is_url(reference):
            url = reference if _is_url(reference) else urllib.parse.urljoin(self.index_url, reference)
            data = _http_get(url, maximum, label)
            _atomic_write_bytes(destination, data)
            return
        assert self.root is not None
        source = (self.root / reference).resolve()
        _require(source.is_file(), f"{label}: file is missing: {source}")
        _require(source.stat().st_size <= maximum, f"{label}: file is oversized: {source}")
        _cli._copy_file_contents(source, destination, label)


# --- the install plan --------------------------------------------------------------

@dataclass
class PlannedInstall:
    view: Any
    artifact: Path            # a `.wotbmod` file ready to be copied into mods/
    sidecar: Path | None
    origin: str               # "source" | "catalog"
    signature: trust.VerifyResult
    replaces: str | None = None


@dataclass
class InstallPlan:
    steps: list[PlannedInstall] = field(default_factory=list)   # dependencies first
    notes: list[str] = field(default_factory=list)
    recommended: list[str] = field(default_factory=list)


def _permission_lines(manifest: dict[str, Any]) -> list[str]:
    grouped: dict[int, list[str]] = {}
    for permission in manifest.get("permissions", []):
        grouped.setdefault(_cli._permission_tier(permission), []).append(permission)
    lines = []
    for tier in sorted(grouped):
        lines.append(f"  {TIER_NAMES[tier]:<15} {', '.join(sorted(grouped[tier]))}")
    return lines or ["  (none)"]


def _tier_index(name: str) -> int:
    return TIER_NAMES.index(name)


def describe_package(view: Any, signature: trust.VerifyResult) -> list[str]:
    """What the user sees before saying yes: identity, hash, signature, permissions."""
    manifest = view.manifest
    total = sum(item.size for item in view.files)
    lines = [
        f"Package: {manifest['id']} {manifest['version']} \"{manifest['name']}\" "
        f"by {manifest['developer']} ({manifest['type']})",
        f"SHA-256: {view.package_sha256}",
        f"Size: {total} bytes in {len(view.files)} files",
        f"Signature: {signature.status}"
        + (f" (key {signature.key_id})" if signature.key_id else "")
        + (f": {signature.detail}" if signature.status not in ("valid", "unsigned") else ""),
        f"Permissions (tier {view.requested_permission_tier}: "
        f"{scan.RISK_LABELS.get(view.requested_permission_tier, '')}):",
        *_permission_lines(manifest),
    ]
    if manifest.get("type") == "resource":
        lines.append("Game files replaced (the originals are saved and put back on uninstall):")
        lines.extend(f"  Data/{item['target']}" + (" (new file)" if item.get("stock_sha256") is None else "")
                     for item in manifest.get("files", []))
    for label, section in (("Requires", "dependencies"), ("Recommends", "optional_dependencies"),
                           ("Conflicts with", "incompatibilities")):
        items = manifest.get(section) or {}
        if items:
            lines.append(f"{label}: " + ", ".join(f"{key} {value}" for key, value in items.items()))
    return lines


# --- Lua packages --------------------------------------------------------------------
#
# A Lua mod is a folder with a small manifest (id, name, version, entrypoint,
# permissions - see schemas/wotbmod-lua-manifest-v1.schema.json) that the Lua
# host loads from <mods>/lua/<id>/. Packed as a .wotbmod it travels, signs and
# verifies exactly like a native package; the difference is only where install
# puts it: extracted into that folder rather than left as an archive in mods/,
# where the native loader would refuse the unfamiliar manifest.

@dataclass
class LuaPackageView:
    source_kind: str
    source_path: str
    manifest: dict[str, Any]
    requested_permission_tier: str
    package_sha256: str
    payload_sha256: str
    files: list[Any]
    warnings: list[str]
    kind: str = "lua"

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["file_count"] = len(self.files)
        result["total_file_bytes"] = sum(item.size for item in self.files)
        result["valid"] = True
        return result


def is_lua_manifest(manifest: Any) -> bool:
    """A native/content manifest always carries manifest_version; a Lua one never does."""
    return isinstance(manifest, dict) and "manifest_version" not in manifest


def validate_lua_manifest(manifest: Any) -> dict[str, Any]:
    _require(isinstance(manifest, dict), "Lua manifest must be an object")
    mod_id = manifest.get("id")
    _require(isinstance(mod_id, str) and LUA_ID_RE.fullmatch(mod_id) is not None and ".." not in mod_id,
             "Lua manifest id is invalid (author.mod_name, letters/digits/._-)")
    version = manifest.get("version", "0.0.0")
    _require(_cli._parse_semver(version) is not None, "Lua manifest version must be a semantic version")
    name = manifest.get("name", mod_id)
    _require(isinstance(name, str) and bool(name) and _cli._utf8_size(name) <= _cli.MAX_NAME_BYTES,
             "Lua manifest name is invalid")
    entrypoint = manifest.get("entrypoint", "main.lua")
    _require(isinstance(entrypoint, str) and LUA_ENTRYPOINT_RE.fullmatch(entrypoint) is not None
             and ".." not in entrypoint, "Lua manifest entrypoint must be a .lua file name in the mod root")
    permissions = manifest.get("permissions", [])
    _require(isinstance(permissions, list) and all(isinstance(item, str) for item in permissions),
             "Lua manifest permissions must be an array of strings")
    _require(len(set(permissions)) == len(permissions), "Lua manifest permissions repeat a name")
    for permission in permissions:
        _require(permission in _cli.REGISTERED_PERMISSIONS or _cli._valid_network_permission(permission),
                 f"Lua manifest names an unregistered permission: {permission}")
    developer = manifest.get("developer") or manifest.get("author") or ""
    _require(isinstance(developer, str) and _cli._utf8_size(developer) <= _cli.MAX_NAME_BYTES,
             "Lua manifest developer is invalid")
    tier = max((_cli._permission_tier(item) for item in permissions), default=0)
    return {
        "type": "lua",
        "id": mod_id,
        "name": name,
        "version": version,
        "developer": developer,
        "entrypoint": entrypoint,
        "permissions": list(permissions),
        "requested_permission_tier": TIER_NAMES[tier],
        "dependencies": _cli._dependency_section(manifest.get("dependencies"), "dependencies"),
        "optional_dependencies": _cli._dependency_section(manifest.get("optional_dependencies"), "optional_dependencies"),
        "incompatibilities": _cli._dependency_section(manifest.get("incompatibilities"), "incompatibilities"),
        "api": {},
        "client": {"builds": [], "executable_hashes": []},
        "signature_declared": False,
    }


def load_lua_archive(archive_path: Path) -> LuaPackageView:
    package_sha256, files, by_folded = _cli._read_store_archive(archive_path)
    _require("manifest.json" in by_folded, "archive root manifest.json is missing")
    manifest = _cli._load_json_bytes(by_folded["manifest.json"], "archive manifest.json")
    validated = validate_lua_manifest(manifest)
    key = validated["entrypoint"].casefold()
    _require(key in by_folded, f"Lua entrypoint is missing from the package: {validated['entrypoint']}")
    return LuaPackageView("wotbmod", str(archive_path), validated, validated["requested_permission_tier"],
                          package_sha256, hashlib.sha256(by_folded[key]).hexdigest(), files, [])


def load_lua_directory(root: Path) -> LuaPackageView:
    root = root.resolve(strict=True)
    files = _cli._enumerate_directory_files(root)
    by_folded = {relative.casefold(): (relative, path, size) for relative, path, size in files}
    _require("manifest.json" in by_folded, "Lua mod folder has no manifest.json")
    manifest = _cli._load_json_bytes(_cli._read_bounded(by_folded["manifest.json"][1], _cli.MAX_MANIFEST_BYTES, "manifest"),
                                     "manifest.json")
    validated = validate_lua_manifest(manifest)
    key = validated["entrypoint"].casefold()
    _require(key in by_folded, f"Lua entrypoint is missing from the folder: {validated['entrypoint']}")
    package_files = [_cli.PackageFile(relative, size, _cli._sha256_file(path)) for relative, path, size in files]
    return LuaPackageView("directory", str(root), validated, validated["requested_permission_tier"],
                          _cli._directory_package_hash(files), _cli._sha256_file(by_folded[key][1]), package_files, [])


def _peek_manifest(path: Path) -> Any:
    if path.is_dir():
        manifest_path = path / "manifest.json"
        _require(manifest_path.is_file(), f"package directory has no manifest.json: {path}")
        return _cli._load_json_bytes(_cli._read_bounded(manifest_path, _cli.MAX_MANIFEST_BYTES, "manifest"), "manifest.json")
    _sha, _files, by_folded = _cli._read_store_archive(path)
    _require("manifest.json" in by_folded, "archive root manifest.json is missing")
    return _cli._load_json_bytes(by_folded["manifest.json"], "archive manifest.json")


def load_any_package(path: Path, *, client_check: bool = True) -> Any:
    """A native/content PackageView or a LuaPackageView, decided by the manifest."""
    path = path.expanduser()
    if path.is_file() and path.suffix.lower() != ".wotbmod" and not path.name.lower().endswith(".json"):
        raise _error(f"expected a .wotbmod archive or a package directory: {path}")
    if path.is_dir() or path.suffix.lower() == ".wotbmod":
        manifest = _peek_manifest(path)
        if is_lua_manifest(manifest):
            return load_lua_directory(path) if path.is_dir() else load_lua_archive(path)
        if is_resource_manifest(manifest):
            return load_resource_directory(path) if path.is_dir() else load_resource_archive(path)
    if client_check:
        return _cli.load_package(path)
    return _cli.load_package(path, client_build=None, client_sha256=None)


def package_kind(view: Any) -> str:
    return getattr(view, "kind", "native")


def pack_any(source: Path, artifact: Path, *, client_check: bool = True) -> Any:
    """Deterministic archive of a native, content or Lua project, validated after writing."""
    manifest = _peek_manifest(source)
    if is_lua_manifest(manifest) or is_resource_manifest(manifest):
        _cli._write_store_archive(source, artifact)
        try:
            return load_lua_archive(artifact) if is_lua_manifest(manifest) else load_resource_archive(artifact)
        except BaseException:
            artifact.unlink(missing_ok=True)
            raise
    build = _cli.EXPECTED_CLIENT_BUILD if client_check else None
    digest = _cli.EXPECTED_CLIENT_SHA256 if client_check else None
    return _cli._create_deterministic_archive(source, artifact, client_build=build, client_sha256=digest)


# --- resource packages ---------------------------------------------------------------
#
# A resource package replaces files under the game's Data/ folder (shaders,
# YAML, textures the loader cannot overlay at runtime). The manifest names
# every target with the SHA-256 of the stock file it expects and of the
# replacement; install refuses anything else on disk, keeps the stock DVPL
# under mods/cache/game_files/ and puts it back on uninstall. The archive
# itself stays under mods/cache/resource_packages/ so list/info/rollback see
# it like any other installed package; the loader never reads it.

try:
    import wotbmod_dvpl as dvpl
except ImportError:  # the CLI is run from a copy without sys.path help
    import importlib.util as _importlib_util
    import sys as _sys
    _spec = _importlib_util.spec_from_file_location("wotbmod_dvpl", Path(__file__).with_name("wotbmod_dvpl.py"))
    assert _spec is not None and _spec.loader is not None
    dvpl = _importlib_util.module_from_spec(_spec)
    _sys.modules["wotbmod_dvpl"] = dvpl
    _spec.loader.exec_module(dvpl)

RESOURCE_CACHE_PARTS = ("cache", "resource_packages")
GAME_FILES_PARTS = ("cache", "game_files")
RESOURCE_PERMISSION = "resources.overlay.game"
MAX_RESOURCE_FILES = 64
MAX_DESCRIPTION_BYTES = 4000
RESOURCE_TARGET_RE = re.compile(r"[A-Za-z0-9_][A-Za-z0-9_. -]*(?:/[A-Za-z0-9_][A-Za-z0-9_. -]*)*")
SHADER_CACHE_DIR = Path.home() / "AppData" / "Local" / "wotblitz" / "DAVAProject" / "shader_cache"


@dataclass
class ResourcePackageView:
    source_kind: str
    source_path: str
    manifest: dict[str, Any]
    requested_permission_tier: str
    package_sha256: str
    payload_sha256: str
    files: list[Any]
    warnings: list[str]
    kind: str = "resource"

    def to_dict(self) -> dict[str, Any]:
        result = asdict(self)
        result["file_count"] = len(self.files)
        result["total_file_bytes"] = sum(item.size for item in self.files)
        result["valid"] = True
        return result


def is_resource_manifest(manifest: Any) -> bool:
    return isinstance(manifest, dict) and manifest.get("manifest_version") == 1 and manifest.get("type") == "resource"


def _valid_game_target(value: Any) -> bool:
    if not isinstance(value, str) or not value or len(value.encode("utf-8", "replace")) > 240:
        return False
    if RESOURCE_TARGET_RE.fullmatch(value) is None or value.lower().endswith(".dvpl"):
        return False
    return ".." not in value.split("/")


def validate_resource_manifest(manifest: Any, contents: dict[str, bytes]) -> dict[str, Any]:
    _require(is_resource_manifest(manifest), "resource manifest must declare manifest_version 1 and type resource")
    mod_id = manifest.get("id")
    _require(_cli._is_ascii_identifier(mod_id, _cli.MAX_ID_BYTES), "manifest id is invalid")
    version = manifest.get("version")
    _require(isinstance(version, str) and _cli._parse_semver(version) is not None,
             "manifest version must be a semantic version")
    name = manifest.get("name")
    _require(isinstance(name, str) and bool(name) and _cli._utf8_size(name) <= _cli.MAX_NAME_BYTES,
             "manifest name is invalid")
    developer = manifest.get("developer")
    _require(isinstance(developer, str) and bool(developer) and _cli._utf8_size(developer) <= _cli.MAX_NAME_BYTES,
             "manifest developer is invalid")
    description = manifest.get("description", "")
    _require(isinstance(description, str) and _cli._utf8_size(description) <= MAX_DESCRIPTION_BYTES,
             "manifest description is too long")
    permissions = manifest.get("permissions", [])
    _require(isinstance(permissions, list) and all(isinstance(item, str) for item in permissions),
             "manifest permissions must be an array of strings")
    _require(len(set(permissions)) == len(permissions), "manifest permissions repeat a name")
    for permission in permissions:
        _require(permission in _cli.REGISTERED_PERMISSIONS or _cli._valid_network_permission(permission),
                 f"manifest names an unregistered permission: {permission}")
    _require(RESOURCE_PERMISSION in permissions,
             f"a resource package must declare the {RESOURCE_PERMISSION} permission")
    files = manifest.get("files")
    _require(isinstance(files, list) and 1 <= len(files) <= MAX_RESOURCE_FILES,
             f"manifest files must list 1..{MAX_RESOURCE_FILES} game files")
    seen: set[str] = set()
    validated_files: list[dict[str, str]] = []
    for item in files:
        _require(isinstance(item, dict), "manifest files entries must be objects")
        target = item.get("target")
        _require(_valid_game_target(target), f"manifest file target is unsafe: {target!r}")
        key = target.casefold()
        _require(key not in seen, f"manifest lists {target} twice")
        seen.add(key)
        source = item.get("source")
        _require(isinstance(source, str) and _cli._validate_archive_path(source, allow_directory=False),
                 f"manifest file source is unsafe: {source!r}")
        data = contents.get(source.casefold())
        _require(data is not None, f"manifest file source is missing from the package: {source}")
        digest = hashlib.sha256(data).hexdigest()
        _require(item.get("sha256") == digest, f"{source}: sha256 in the manifest does not match the file")
        stock = item.get("stock_sha256")
        _require(stock is None or (isinstance(stock, str) and re.fullmatch(r"[0-9a-f]{64}", stock) is not None),
                 f"{target}: stock_sha256 must be a lowercase SHA-256 hex digest of the stock file, or null for a new file")
        _require(stock != digest, f"{target}: the replacement equals the stock file")
        validated_files.append({"target": target, "source": source, "sha256": digest, "stock_sha256": stock})
    client = manifest.get("client", {})
    _require(isinstance(client, dict), "manifest client section must be an object")
    builds = client.get("builds") or []
    _require(isinstance(builds, list) and all(isinstance(item, str) for item in builds),
             "manifest client builds must be an array of strings")
    tier = max((_cli._permission_tier(item) for item in permissions), default=0)
    return {
        "type": "resource",
        "id": mod_id,
        "name": name,
        "version": version,
        "developer": developer,
        "description": description,
        "permissions": list(permissions),
        "requested_permission_tier": TIER_NAMES[tier],
        "dependencies": _cli._dependency_section(manifest.get("dependencies"), "dependencies"),
        "optional_dependencies": _cli._dependency_section(manifest.get("optional_dependencies"), "optional_dependencies"),
        "incompatibilities": _cli._dependency_section(manifest.get("incompatibilities"), "incompatibilities"),
        "api": {},
        "client": {"builds": list(builds), "executable_hashes": list(client.get("executable_hashes") or [])},
        "signature_declared": False,
        "files": validated_files,
    }


def _resource_payload_hash(validated: dict[str, Any], contents: dict[str, bytes]) -> str:
    digest = hashlib.sha256()
    for item in validated["files"]:
        digest.update(contents[item["source"].casefold()])
    return digest.hexdigest()


def load_resource_archive(archive_path: Path) -> ResourcePackageView:
    package_sha256, files, by_folded = _cli._read_store_archive(archive_path)
    _require("manifest.json" in by_folded, "archive root manifest.json is missing")
    manifest = _cli._load_json_bytes(by_folded["manifest.json"], "archive manifest.json")
    validated = validate_resource_manifest(manifest, by_folded)
    return ResourcePackageView("wotbmod", str(archive_path), validated, validated["requested_permission_tier"],
                               package_sha256, _resource_payload_hash(validated, by_folded), files, [])


def load_resource_directory(root: Path) -> ResourcePackageView:
    root = root.resolve(strict=True)
    files = _cli._enumerate_directory_files(root)
    contents = {relative.casefold(): _cli._read_bounded(path, _cli.MAX_ARCHIVE_SINGLE_FILE_BYTES, f"package file {relative}")
                for relative, path, _size in files}
    _require("manifest.json" in contents, "resource package folder has no manifest.json")
    manifest = _cli._load_json_bytes(contents["manifest.json"], "manifest.json")
    validated = validate_resource_manifest(manifest, contents)
    package_files = [_cli.PackageFile(relative, size, hashlib.sha256(contents[relative.casefold()]).hexdigest())
                     for relative, _path, size in files]
    return ResourcePackageView("directory", str(root), validated, validated["requested_permission_tier"],
                               _cli._directory_package_hash(files), _resource_payload_hash(validated, contents),
                               package_files, [])


def resource_cache_paths(mods_root: Path, mod_id: str) -> tuple[Path, Path]:
    folder = mods_root.joinpath(*RESOURCE_CACHE_PARTS)
    return folder / f"{mod_id}.wotbmod", folder / f"{mod_id}.wotbmod.sig"


def _cached_archive_paths(mods_root: Path, kind: str, mod_id: str) -> tuple[Path, Path]:
    return lua_cache_paths(mods_root, mod_id) if kind == "lua" else resource_cache_paths(mods_root, mod_id)


def game_data_root(mods_root: Path) -> Path:
    return mods_root.parent / "Data"


def _live_game_file(mods_root: Path, target: str) -> Path:
    parts = target.split("/")
    parts[-1] += ".dvpl"
    return game_data_root(mods_root).joinpath(*parts)


def _pristine_path(mods_root: Path, target: str) -> Path:
    return mods_root.joinpath(*GAME_FILES_PARTS) / (target.replace("/", "__") + ".dvpl")


def _unpacked_sha256(live: Path) -> str:
    _require(live.is_file(), f"game file is missing: {live}")
    try:
        return hashlib.sha256(dvpl.unpack(live.read_bytes())).hexdigest()
    except dvpl.DvplError as exc:
        raise _error(f"{live}: not a readable DVPL file ({exc})") from exc


def _clear_shader_cache(targets: list[str]) -> None:
    """The client caches compiled shaders by path; a replaced shader must not
    be served from that cache (the old installer cleared it the same way)."""
    if not any(target.lower().startswith("materials/shaders/") for target in targets):
        return
    if not SHADER_CACHE_DIR.is_dir():
        return
    for child in SHADER_CACHE_DIR.iterdir():
        try:
            if child.is_dir():
                shutil.rmtree(child, ignore_errors=True)
            else:
                child.unlink()
        except OSError:
            pass


def _apply_resource_step(
    mods_root: Path,
    step: PlannedInstall,
    ledger: dict[str, Any],
    ini: ModsIni,
    *,
    granted_tier: int,
    source_label: str,
    catalog_label: str | None,
) -> Path:
    """Replace the package's game files under Data/, after checking every one
    of them is the stock file (or this package's own earlier version)."""
    mod_id = step.view.manifest["id"]
    files = step.view.manifest["files"]
    data_root = game_data_root(mods_root)
    _require(data_root.is_dir(), f"the game's Data folder was not found next to mods/: {data_root}")
    _require(_cli._sha256_file(step.artifact) == step.view.package_sha256,
             "staged package hash differs from the validated package")
    _sha, _files, contents = _cli._read_store_archive(step.artifact)
    entry = ledger["packages"].get(mod_id) or {"history": []}
    entry.setdefault("history", [])
    owned = {item["target"].casefold(): item for item in entry.get("targets", [])}         if entry.get("state") == "installed" else {}
    for other_id, other in ledger["packages"].items():
        if other_id == mod_id or other.get("state") != "installed" or other.get("kind") != "resource":
            continue
        for taken in other.get("targets", []):
            for item in files:
                _require(taken["target"].casefold() != item["target"].casefold(),
                         f"Data/{item['target']} is already replaced by {other_id} {other.get('version')}; uninstall it first")
    planned: list[tuple[dict[str, str], Path, Path, str]] = []
    for item in files:
        live = _live_game_file(mods_root, item["target"])
        pristine = _pristine_path(mods_root, item["target"])
        if item["stock_sha256"] is None:
            # a file the stock game does not have: nothing to save, nothing may be in the way
            if live.is_file():
                current = _unpacked_sha256(live)
                _require(current == item["sha256"] or owned.get(item["target"].casefold(), {}).get("sha256") == current,
                         f"Data/{item['target']} already exists although the stock game has no such file; "
                         "another mod put it there - remove that mod first")
            planned.append((item, live, pristine, "new"))
            continue
        current = _unpacked_sha256(live)
        if current == item["stock_sha256"]:
            state = "stock"
        elif current == item["sha256"] or owned.get(item["target"].casefold(), {}).get("sha256") == current:
            state = "ours"
            _require(pristine.is_file(),
                     f"Data/{item['target']} already carries a modified file and no saved original exists; "
                     "restore the stock file first (Steam: verify integrity of game files) and install again")
        else:
            raise _error(f"Data/{item['target']} on disk is neither the stock file this package expects "
                         f"({item['stock_sha256'][:12]}...) nor this package's own version: a game update or "
                         "another tool changed it; not touching it")
        planned.append((item, live, pristine, state))
    for item, live, pristine, state in planned:
        if state == "stock" and not pristine.is_file():
            _atomic_write_bytes(pristine, live.read_bytes())
        data = contents[item["source"].casefold()]
        _require(hashlib.sha256(data).hexdigest() == item["sha256"],
                 f"{item['source']}: package content changed after validation")
        _atomic_write_bytes(live, dvpl.pack(data))
    cached, cached_sig = resource_cache_paths(mods_root, mod_id)
    if cached.is_file():
        entry["history"].append(_backup_installed(mods_root, mod_id, cached, cached_sig))
    _atomic_write_bytes(cached, step.artifact.read_bytes())
    if step.sidecar is not None:
        _atomic_write_bytes(cached_sig, step.sidecar.read_bytes())
    else:
        cached_sig.unlink(missing_ok=True)
    _clear_shader_cache([item["target"] for item in files])
    entry.update({
        "version": step.view.manifest["version"],
        "sha256": step.view.package_sha256,
        "installed_at": _now(),
        "source": source_label,
        "catalog": catalog_label,
        "signature": {"status": step.signature.status, "key_id": step.signature.key_id},
        "tier_granted": granted_tier,
        "state": "installed",
        "kind": "resource",
        "targets": [{
            "target": item["target"], "sha256": item["sha256"], "stock_sha256": item["stock_sha256"],
            "pristine": None if item["stock_sha256"] is None
            else str(_pristine_path(mods_root, item["target"]).relative_to(mods_root)).replace("\\", "/"),
        } for item in files],
    })
    ledger["packages"][mod_id] = entry
    return data_root


def restore_resource_targets(mods_root: Path, entry: dict[str, Any], *, force: bool = False) -> list[str]:
    """Put the saved stock files back for every target the ledger entry owns."""
    restored: list[str] = []
    targets = entry.get("targets", [])
    for item in targets:
        live = _live_game_file(mods_root, item["target"])
        if item.get("stock_sha256") is None:
            if live.is_file():
                current = _unpacked_sha256(live)
                _require(current == item["sha256"] or force,
                         f"Data/{item['target']} changed since it was installed; pass --force to delete it anyway")
                live.unlink()
            restored.append(item["target"])
            continue
        pristine = mods_root / item["pristine"]
        _require(pristine.is_file(), f"the saved original for Data/{item['target']} is missing: {pristine}")
        if live.is_file():
            current = _unpacked_sha256(live)
            if current == item["stock_sha256"]:
                pristine.unlink(missing_ok=True)
                restored.append(item["target"])
                continue
            _require(current == item["sha256"] or force,
                     f"Data/{item['target']} changed since it was installed (a game update?); "
                     "pass --force to put the saved original back anyway")
        _atomic_write_bytes(live, pristine.read_bytes())
        pristine.unlink(missing_ok=True)
        restored.append(item["target"])
    _clear_shader_cache([item["target"] for item in targets])
    return restored


# --- importing an existing mod ---------------------------------------------------------
#
# Most community mods for the game are plain file replacements: a folder (or
# zip) that mirrors Data/, holding packed .dvpl files or the raw files. `wotbmod
# import` turns one into a resource-package project: every file becomes a
# manifest entry with the SHA-256 of the local game's stock file (or null when
# the stock game has no such file), so install/uninstall stay reversible.

IMPORT_SKIP_NAMES = {"manifest.yaml", "manifest.yml", "manifest.json", "blitzforge.json", "readme.md", "readme.txt",
                     "readme_ru.md", "license", "license.txt", "changelog.md", "changelog.txt", "thumbs.db",
                     ".ds_store", "desktop.ini"}
IMPORT_SKIP_SUFFIXES = {".diff", ".patch", ".url", ".lnk"}
IMPORT_LAYOUT_ROOTS = ("data", "patched", "files")


def _import_source_files(source: Path, workdir: Path) -> list[tuple[str, Path]]:
    """(relative path with forward slashes, file) for everything in a folder or zip."""
    root = source
    if source.is_file():
        _require(source.suffix.lower() in (".zip", ".wotbmod"), f"import expects a folder or a .zip: {source}")
        import zipfile
        root = workdir / "unzipped"
        with zipfile.ZipFile(source, "r") as opened:
            for info in opened.infolist():
                _require(_cli._validate_archive_path(info.filename.replace("\\", "/")),
                         f"archive contains an unsafe path: {info.filename}")
                if info.is_dir():
                    continue
                target = root / Path(*info.filename.replace("\\", "/").split("/"))
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(opened.read(info))
    _require(root.is_dir(), f"import source does not exist: {source}")
    found: list[tuple[str, Path]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        name = path.name.lower()
        if name in IMPORT_SKIP_NAMES or path.suffix.lower() in IMPORT_SKIP_SUFFIXES or name.startswith("."):
            continue
        found.append((relative, path))
    return found


def _import_target_of(relative: str) -> str | None:
    """Map a source path onto a Data/-relative target: strip a leading Data/,
    patched/ or files/ folder (one level of wrapper folder above it is fine
    too) and the .dvpl suffix."""
    parts = relative.split("/")
    lowered = [part.lower() for part in parts]
    for index, part in enumerate(lowered[:2]):
        if part in IMPORT_LAYOUT_ROOTS:
            parts = parts[index + 1:]
            break
    else:
        if len(parts) > 1 and lowered[0] not in ("materials", "3d", "ui", "sounds", "fx", "maps", "vehicles", "strings"):
            pass  # unknown wrapper: keep the path as is; the manifest check will refuse unsafe ones
    if not parts:
        return None
    if parts[-1].lower().endswith(".dvpl"):
        parts[-1] = parts[-1][:-5]
    target = "/".join(parts)
    return target if _valid_game_target(target) else None


def _import_content(path: Path) -> bytes:
    data = _cli._read_bounded(path, _cli.MAX_ARCHIVE_SINGLE_FILE_BYTES, f"import file {path.name}")
    if path.suffix.lower() == ".dvpl" or dvpl.is_dvpl(data):
        try:
            return dvpl.unpack(data)
        except dvpl.DvplError as exc:
            raise _error(f"{path}: not a readable DVPL container ({exc})") from exc
    return data


def import_legacy_mod(source: Path, game_root: Path, output: Path, *, mod_id: str, name: str, version: str,
                      developer: str, description: str, client_build: str | None, workdir: Path) -> dict[str, Any]:
    """Write a resource-package project under `output`; returns the manifest."""
    _require(_cli._is_ascii_identifier(mod_id, _cli.MAX_ID_BYTES), "--id must look like author.mod_name")
    _require(_cli._parse_semver(version) is not None, "--version must be a semantic version")
    data_root = game_root / "Data"
    _require(data_root.is_dir(), f"the game's Data folder was not found: {data_root}")
    files_root = output / "files"
    _require(not output.exists() or (output.is_dir() and not any(output.iterdir())),
             f"output folder must not exist or must be empty: {output}")
    entries: list[dict[str, Any]] = []
    skipped: list[str] = []
    seen: set[str] = set()
    for relative, path in _import_source_files(source, workdir):
        target = _import_target_of(relative)
        if target is None:
            skipped.append(f"{relative}: not a Data/ path")
            continue
        if target.casefold() in seen:
            skipped.append(f"{relative}: {target} listed twice")
            continue
        content = _import_content(path)
        digest = hashlib.sha256(content).hexdigest()
        live = data_root.joinpath(*target.split("/"))
        live = live.with_name(live.name + ".dvpl")
        stock: str | None = None
        if live.is_file():
            stock = _unpacked_sha256(live)
            if stock == digest:
                skipped.append(f"{relative}: identical to the game's file")
                continue
        seen.add(target.casefold())
        destination = files_root.joinpath(*target.split("/"))
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        entries.append({"target": target, "source": "files/" + target, "stock_sha256": stock, "sha256": digest})
    _require(bool(entries), "nothing to import: no game files found in the source"
             + (" (" + "; ".join(skipped[:5]) + ")" if skipped else ""))
    manifest: dict[str, Any] = {
        "manifest_version": 1,
        "type": "resource",
        "id": mod_id,
        "name": name,
        "version": version,
        "developer": developer,
        "description": description,
        "permissions": [RESOURCE_PERMISSION],
        "client": {"builds": [client_build] if client_build else []},
        "files": entries,
    }
    output.mkdir(parents=True, exist_ok=True)
    _write_json(output / "manifest.json", manifest)
    readme = [f"# {name}", "", f"Импортировано командой `wotbmod import` из `{source.name}`: {len(entries)} файл(ов) игры.",
              "Стоковые хеши сняты с локального клиента" + (f" {client_build}" if client_build else "") + ".", "",
              "| Файл игры | Стоковый есть |", "| --- | --- |"]
    readme += [f"| `Data/{item['target']}` | {'да' if item['stock_sha256'] else 'нет (новый файл)'} |" for item in entries]
    if skipped:
        readme += ["", "Пропущено:", *[f"- {line}" for line in skipped]]
    (output / "README_RU.md").write_text("\n".join(readme) + "\n", encoding="utf-8")
    load_resource_directory(output)  # the project must validate like any package
    manifest["_skipped"] = skipped
    return manifest


def command_import(args: argparse.Namespace) -> int:
    game_root = Path(args.game_root).expanduser()
    source = Path(args.source).expanduser()
    output = Path(args.output).expanduser() if args.output else Path.cwd() / args.id.replace(".", "_")
    client_build = args.client_build
    if client_build is None:
        try:
            client_build = _cli._windows_file_version(game_root / "wotblitz.exe")
        except Exception:
            client_build = _cli.EXPECTED_CLIENT_BUILD
    with tempfile.TemporaryDirectory(prefix="wotbmod-import-") as temporary:
        manifest = import_legacy_mod(source, game_root, output, mod_id=args.id, name=args.name or args.id,
                                     version=args.version, developer=args.developer or "unknown",
                                     description=args.description or "", client_build=client_build or None,
                                     workdir=Path(temporary))
    entries = manifest["files"]
    new_files = sum(1 for item in entries if item["stock_sha256"] is None)
    print(f"Imported: {manifest['id']} {manifest['version']} -> {output}")
    print(f"Game files: {len(entries)} ({len(entries) - new_files} replace stock files, {new_files} new)")
    for item in entries:
        print(f"  Data/{item['target']}" + ("" if item["stock_sha256"] else "  (new file)"))
    for line in manifest.get("_skipped", []):
        print(f"skipped: {line}")
    print("Next: wotbmod release " + str(output) + " --sign-with-key <key> --key-id <id>")
    return 0


# --- importing an existing mod ---------------------------------------------------------
#
# Most community mods for the game are plain file replacements: a folder (or
# zip) that mirrors Data/, holding packed .dvpl files or the raw files. `wotbmod
# import` turns one into a resource-package project: every file becomes a
# manifest entry with the SHA-256 of the local game's stock file (or null when
# the stock game has no such file), so install/uninstall stay reversible.

IMPORT_SKIP_NAMES = {"manifest.yaml", "manifest.yml", "manifest.json", "blitzforge.json", "readme.md", "readme.txt",
                     "readme_ru.md", "license", "license.txt", "changelog.md", "changelog.txt", "thumbs.db",
                     ".ds_store", "desktop.ini"}
IMPORT_SKIP_SUFFIXES = {".diff", ".patch", ".url", ".lnk"}
IMPORT_LAYOUT_ROOTS = ("data", "patched", "files")


def _import_source_files(source: Path, workdir: Path) -> list[tuple[str, Path]]:
    """(relative path with forward slashes, file) for everything in a folder or zip."""
    root = source
    if source.is_file():
        _require(source.suffix.lower() in (".zip", ".wotbmod"), f"import expects a folder or a .zip: {source}")
        import zipfile
        root = workdir / "unzipped"
        with zipfile.ZipFile(source, "r") as opened:
            for info in opened.infolist():
                _require(_cli._validate_archive_path(info.filename.replace("\\", "/")),
                         f"archive contains an unsafe path: {info.filename}")
                if info.is_dir():
                    continue
                target = root / Path(*info.filename.replace("\\", "/").split("/"))
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(opened.read(info))
    _require(root.is_dir(), f"import source does not exist: {source}")
    found: list[tuple[str, Path]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        name = path.name.lower()
        if name in IMPORT_SKIP_NAMES or path.suffix.lower() in IMPORT_SKIP_SUFFIXES or name.startswith("."):
            continue
        found.append((relative, path))
    return found


def _import_target_of(relative: str) -> str | None:
    """Map a source path onto a Data/-relative target: strip a leading Data/,
    patched/ or files/ folder (one level of wrapper folder above it is fine
    too) and the .dvpl suffix."""
    parts = relative.split("/")
    lowered = [part.lower() for part in parts]
    for index, part in enumerate(lowered[:2]):
        if part in IMPORT_LAYOUT_ROOTS:
            parts = parts[index + 1:]
            break
    else:
        if len(parts) > 1 and lowered[0] not in ("materials", "3d", "ui", "sounds", "fx", "maps", "vehicles", "strings"):
            pass  # unknown wrapper: keep the path as is; the manifest check will refuse unsafe ones
    if not parts:
        return None
    if parts[-1].lower().endswith(".dvpl"):
        parts[-1] = parts[-1][:-5]
    target = "/".join(parts)
    return target if _valid_game_target(target) else None


def _import_content(path: Path) -> bytes:
    data = _cli._read_bounded(path, _cli.MAX_ARCHIVE_SINGLE_FILE_BYTES, f"import file {path.name}")
    if path.suffix.lower() == ".dvpl" or dvpl.is_dvpl(data):
        try:
            return dvpl.unpack(data)
        except dvpl.DvplError as exc:
            raise _error(f"{path}: not a readable DVPL container ({exc})") from exc
    return data


def import_legacy_mod(source: Path, game_root: Path, output: Path, *, mod_id: str, name: str, version: str,
                      developer: str, description: str, client_build: str | None, workdir: Path) -> dict[str, Any]:
    """Write a resource-package project under `output`; returns the manifest."""
    _require(_cli._is_ascii_identifier(mod_id, _cli.MAX_ID_BYTES), "--id must look like author.mod_name")
    _require(_cli._parse_semver(version) is not None, "--version must be a semantic version")
    data_root = game_root / "Data"
    _require(data_root.is_dir(), f"the game's Data folder was not found: {data_root}")
    files_root = output / "files"
    _require(not output.exists() or (output.is_dir() and not any(output.iterdir())),
             f"output folder must not exist or must be empty: {output}")
    entries: list[dict[str, Any]] = []
    skipped: list[str] = []
    seen: set[str] = set()
    for relative, path in _import_source_files(source, workdir):
        target = _import_target_of(relative)
        if target is None:
            skipped.append(f"{relative}: not a Data/ path")
            continue
        if target.casefold() in seen:
            skipped.append(f"{relative}: {target} listed twice")
            continue
        content = _import_content(path)
        digest = hashlib.sha256(content).hexdigest()
        live = data_root.joinpath(*target.split("/"))
        live = live.with_name(live.name + ".dvpl")
        stock: str | None = None
        if live.is_file():
            stock = _unpacked_sha256(live)
            if stock == digest:
                skipped.append(f"{relative}: identical to the game's file")
                continue
        seen.add(target.casefold())
        destination = files_root.joinpath(*target.split("/"))
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        entries.append({"target": target, "source": "files/" + target, "stock_sha256": stock, "sha256": digest})
    _require(bool(entries), "nothing to import: no game files found in the source"
             + (" (" + "; ".join(skipped[:5]) + ")" if skipped else ""))
    manifest: dict[str, Any] = {
        "manifest_version": 1,
        "type": "resource",
        "id": mod_id,
        "name": name,
        "version": version,
        "developer": developer,
        "description": description,
        "permissions": [RESOURCE_PERMISSION],
        "client": {"builds": [client_build] if client_build else []},
        "files": entries,
    }
    output.mkdir(parents=True, exist_ok=True)
    _write_json(output / "manifest.json", manifest)
    readme = [f"# {name}", "", f"Импортировано командой `wotbmod import` из `{source.name}`: {len(entries)} файл(ов) игры.",
              "Стоковые хеши сняты с локального клиента" + (f" {client_build}" if client_build else "") + ".", "",
              "| Файл игры | Стоковый есть |", "| --- | --- |"]
    readme += [f"| `Data/{item['target']}` | {'да' if item['stock_sha256'] else 'нет (новый файл)'} |" for item in entries]
    if skipped:
        readme += ["", "Пропущено:", *[f"- {line}" for line in skipped]]
    (output / "README_RU.md").write_text("\n".join(readme) + "\n", encoding="utf-8")
    load_resource_directory(output)  # the project must validate like any package
    manifest["_skipped"] = skipped
    return manifest


def command_import(args: argparse.Namespace) -> int:
    game_root = Path(args.game_root).expanduser()
    source = Path(args.source).expanduser()
    output = Path(args.output).expanduser() if args.output else Path.cwd() / args.id.replace(".", "_")
    client_build = args.client_build
    if client_build is None:
        try:
            client_build = _cli._windows_file_version(game_root / "wotblitz.exe")
        except Exception:
            client_build = _cli.EXPECTED_CLIENT_BUILD
    with tempfile.TemporaryDirectory(prefix="wotbmod-import-") as temporary:
        manifest = import_legacy_mod(source, game_root, output, mod_id=args.id, name=args.name or args.id,
                                     version=args.version, developer=args.developer or "unknown",
                                     description=args.description or "", client_build=client_build or None,
                                     workdir=Path(temporary))
    entries = manifest["files"]
    new_files = sum(1 for item in entries if item["stock_sha256"] is None)
    print(f"Imported: {manifest['id']} {manifest['version']} -> {output}")
    print(f"Game files: {len(entries)} ({len(entries) - new_files} replace stock files, {new_files} new)")
    for item in entries:
        print(f"  Data/{item['target']}" + ("" if item["stock_sha256"] else "  (new file)"))
    for line in manifest.get("_skipped", []):
        print(f"skipped: {line}")
    print("Next: wotbmod release " + str(output) + " --sign-with-key <key> --key-id <id>")
    return 0


# --- importing an existing mod ---------------------------------------------------------
#
# Most community mods for the game are plain file replacements: a folder (or
# zip) that mirrors Data/, holding packed .dvpl files or the raw files. `wotbmod
# import` turns one into a resource-package project: every file becomes a
# manifest entry with the SHA-256 of the local game's stock file (or null when
# the stock game has no such file), so install/uninstall stay reversible.

IMPORT_SKIP_NAMES = {"manifest.yaml", "manifest.yml", "manifest.json", "blitzforge.json", "readme.md", "readme.txt",
                     "readme_ru.md", "license", "license.txt", "changelog.md", "changelog.txt", "thumbs.db",
                     ".ds_store", "desktop.ini"}
IMPORT_SKIP_SUFFIXES = {".diff", ".patch", ".url", ".lnk"}
IMPORT_LAYOUT_ROOTS = ("data", "patched", "files")


def _import_source_files(source: Path, workdir: Path) -> list[tuple[str, Path]]:
    """(relative path with forward slashes, file) for everything in a folder or zip."""
    root = source
    if source.is_file():
        _require(source.suffix.lower() in (".zip", ".wotbmod"), f"import expects a folder or a .zip: {source}")
        import zipfile
        root = workdir / "unzipped"
        with zipfile.ZipFile(source, "r") as opened:
            for info in opened.infolist():
                _require(_cli._validate_archive_path(info.filename.replace("\\", "/")),
                         f"archive contains an unsafe path: {info.filename}")
                if info.is_dir():
                    continue
                target = root / Path(*info.filename.replace("\\", "/").split("/"))
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(opened.read(info))
    _require(root.is_dir(), f"import source does not exist: {source}")
    found: list[tuple[str, Path]] = []
    for path in sorted(root.rglob("*")):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        name = path.name.lower()
        if name in IMPORT_SKIP_NAMES or path.suffix.lower() in IMPORT_SKIP_SUFFIXES or name.startswith("."):
            continue
        found.append((relative, path))
    return found


def _import_target_of(relative: str) -> str | None:
    """Map a source path onto a Data/-relative target: strip a leading Data/,
    patched/ or files/ folder (one level of wrapper folder above it is fine
    too) and the .dvpl suffix."""
    parts = relative.split("/")
    lowered = [part.lower() for part in parts]
    for index, part in enumerate(lowered[:2]):
        if part in IMPORT_LAYOUT_ROOTS:
            parts = parts[index + 1:]
            break
    else:
        if len(parts) > 1 and lowered[0] not in ("materials", "3d", "ui", "sounds", "fx", "maps", "vehicles", "strings"):
            pass  # unknown wrapper: keep the path as is; the manifest check will refuse unsafe ones
    if not parts:
        return None
    if parts[-1].lower().endswith(".dvpl"):
        parts[-1] = parts[-1][:-5]
    target = "/".join(parts)
    return target if _valid_game_target(target) else None


def _import_content(path: Path) -> bytes:
    data = _cli._read_bounded(path, _cli.MAX_ARCHIVE_SINGLE_FILE_BYTES, f"import file {path.name}")
    if path.suffix.lower() == ".dvpl" or dvpl.is_dvpl(data):
        try:
            return dvpl.unpack(data)
        except dvpl.DvplError as exc:
            raise _error(f"{path}: not a readable DVPL container ({exc})") from exc
    return data


def import_legacy_mod(source: Path, game_root: Path, output: Path, *, mod_id: str, name: str, version: str,
                      developer: str, description: str, client_build: str | None, workdir: Path) -> dict[str, Any]:
    """Write a resource-package project under `output`; returns the manifest."""
    _require(_cli._is_ascii_identifier(mod_id, _cli.MAX_ID_BYTES), "--id must look like author.mod_name")
    _require(_cli._parse_semver(version) is not None, "--version must be a semantic version")
    data_root = game_root / "Data"
    _require(data_root.is_dir(), f"the game's Data folder was not found: {data_root}")
    files_root = output / "files"
    _require(not output.exists() or (output.is_dir() and not any(output.iterdir())),
             f"output folder must not exist or must be empty: {output}")
    entries: list[dict[str, Any]] = []
    skipped: list[str] = []
    seen: set[str] = set()
    for relative, path in _import_source_files(source, workdir):
        target = _import_target_of(relative)
        if target is None:
            skipped.append(f"{relative}: not a Data/ path")
            continue
        if target.casefold() in seen:
            skipped.append(f"{relative}: {target} listed twice")
            continue
        content = _import_content(path)
        digest = hashlib.sha256(content).hexdigest()
        live = data_root.joinpath(*target.split("/"))
        live = live.with_name(live.name + ".dvpl")
        stock: str | None = None
        if live.is_file():
            stock = _unpacked_sha256(live)
            if stock == digest:
                skipped.append(f"{relative}: identical to the game's file")
                continue
        seen.add(target.casefold())
        destination = files_root.joinpath(*target.split("/"))
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        entries.append({"target": target, "source": "files/" + target, "stock_sha256": stock, "sha256": digest})
    _require(bool(entries), "nothing to import: no game files found in the source"
             + (" (" + "; ".join(skipped[:5]) + ")" if skipped else ""))
    manifest: dict[str, Any] = {
        "manifest_version": 1,
        "type": "resource",
        "id": mod_id,
        "name": name,
        "version": version,
        "developer": developer,
        "description": description,
        "permissions": [RESOURCE_PERMISSION],
        "client": {"builds": [client_build] if client_build else []},
        "files": entries,
    }
    output.mkdir(parents=True, exist_ok=True)
    _write_json(output / "manifest.json", manifest)
    readme = [f"# {name}", "", f"Импортировано командой `wotbmod import` из `{source.name}`: {len(entries)} файл(ов) игры.",
              "Стоковые хеши сняты с локального клиента" + (f" {client_build}" if client_build else "") + ".", "",
              "| Файл игры | Стоковый есть |", "| --- | --- |"]
    readme += [f"| `Data/{item['target']}` | {'да' if item['stock_sha256'] else 'нет (новый файл)'} |" for item in entries]
    if skipped:
        readme += ["", "Пропущено:", *[f"- {line}" for line in skipped]]
    (output / "README_RU.md").write_text("\n".join(readme) + "\n", encoding="utf-8")
    load_resource_directory(output)  # the project must validate like any package
    manifest["_skipped"] = skipped
    return manifest


def command_import(args: argparse.Namespace) -> int:
    game_root = Path(args.game_root).expanduser()
    source = Path(args.source).expanduser()
    output = Path(args.output).expanduser() if args.output else Path.cwd() / args.id.replace(".", "_")
    client_build = args.client_build
    if client_build is None:
        try:
            client_build = _cli._windows_file_version(game_root / "wotblitz.exe")
        except Exception:
            client_build = _cli.EXPECTED_CLIENT_BUILD
    with tempfile.TemporaryDirectory(prefix="wotbmod-import-") as temporary:
        manifest = import_legacy_mod(source, game_root, output, mod_id=args.id, name=args.name or args.id,
                                     version=args.version, developer=args.developer or "unknown",
                                     description=args.description or "", client_build=client_build or None,
                                     workdir=Path(temporary))
    entries = manifest["files"]
    new_files = sum(1 for item in entries if item["stock_sha256"] is None)
    print(f"Imported: {manifest['id']} {manifest['version']} -> {output}")
    print(f"Game files: {len(entries)} ({len(entries) - new_files} replace stock files, {new_files} new)")
    for item in entries:
        print(f"  Data/{item['target']}" + ("" if item["stock_sha256"] else "  (new file)"))
    for line in manifest.get("_skipped", []):
        print(f"skipped: {line}")
    print("Next: wotbmod release " + str(output) + " --sign-with-key <key> --key-id <id>")
    return 0


def lua_cache_paths(mods_root: Path, mod_id: str) -> tuple[Path, Path]:
    folder = mods_root.joinpath(*LUA_CACHE_PARTS)
    return folder / f"{mod_id}.wotbmod", folder / f"{mod_id}.wotbmod.sig"


def _extract_lua_archive(archive: Path, destination: Path) -> None:
    """Write the archive's files under `destination` (paths were validated by the reader)."""
    import zipfile
    with zipfile.ZipFile(archive, "r", allowZip64=False) as opened:
        for info in opened.infolist():
            _require(_cli._validate_archive_path(info.filename), f"archive contains unsafe path: {info.filename}")
            if info.is_dir():
                continue
            target = destination / Path(*info.filename.split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(opened.read(info))


def _prepare_artifact(source: Path, into: Path) -> tuple[Any, Path]:
    """A validated view plus a `.wotbmod` file for it (a directory is packed deterministically)."""
    view = load_any_package(source)
    if source.is_dir():
        artifact = into / f"{view.manifest['id']}-{view.manifest['version']}.wotbmod"
        return pack_any(source, artifact), artifact
    _require(source.suffix.lower() == ".wotbmod", f"install expects a .wotbmod archive or a package directory: {source}")
    return view, source


def build_plan(
    source: Path,
    installed: dict[str, InstalledPackage],
    trust_root: Path,
    workdir: Path,
    *,
    sidecar: Path | None = None,
    catalog: Catalog | None = None,
    prerelease: bool = False,
) -> InstallPlan:
    """Resolve `source` and its required dependencies into an ordered plan.

    Dependencies come from what is installed, else from the catalog; a
    required dependency that neither has is an error, not a warning. The
    order is dependencies first, so a `for step in plan.steps` install
    satisfies the loader's "required dependency present" check at every point.
    """
    plan = InstallPlan()
    view, artifact = _prepare_artifact(source, workdir)
    sidecar_path = sidecar if sidecar is not None else trust.sidecar_path_for(source)
    if sidecar is None and source.is_dir() and not sidecar_path.is_file():
        sidecar_path = None
    if sidecar_path is not None and not sidecar_path.is_file():
        _require(sidecar is None, f"signature sidecar does not exist: {sidecar_path}")
        sidecar_path = None
    root_step = PlannedInstall(
        view, artifact, sidecar_path, "source",
        signature_status_of(view, sidecar_path or Path(os.devnull), trust_root))
    # The plan is a tree walk: children (dependencies) before the parent.
    resolved: dict[str, PlannedInstall] = {}
    visiting: list[str] = []

    def visit(step: PlannedInstall) -> None:
        mod_id = step.view.manifest["id"]
        if mod_id in resolved:
            return
        _require(mod_id not in visiting, "dependency cycle: " + " -> ".join([*visiting, mod_id]))
        visiting.append(mod_id)
        manifest = step.view.manifest
        for dep_id, dep_range in (manifest.get("dependencies") or {}).items():
            planned = next((s for s in plan.steps if s.view.manifest["id"] == dep_id), None)
            if planned is not None:
                _require(matches_range(planned.view.manifest["version"], dep_range),
                         f"{mod_id} requires {dep_id} {dep_range}, but the plan installs "
                         f"{planned.view.manifest['version']}")
                continue
            present = installed.get(dep_id)
            if present is not None and matches_range(present.version, dep_range):
                plan.notes.append(f"{mod_id} requires {dep_id} {dep_range}: installed {present.version}")
                continue
            candidate = catalog.best(dep_id, dep_range, prerelease=prerelease,
                                     client_build=_cli.EXPECTED_CLIENT_BUILD) if catalog else None
            if candidate is None:
                have = f"installed {present.version} does not match" if present else "not installed"
                hint = "" if catalog else "; pass --catalog <dir|url> to fetch it"
                raise _error(f"required dependency missing: {mod_id} needs {dep_id} {dep_range} ({have}{hint})")
            fetched_dir = workdir / f"dep-{len(plan.steps)}"
            fetched_dir.mkdir(parents=True, exist_ok=True)
            dep_artifact, dep_sidecar = catalog.fetch(candidate, fetched_dir)
            dep_view = _cli.load_package(dep_artifact)
            _require(dep_view.manifest["id"] == dep_id and dep_view.manifest["version"] == candidate.version,
                     f"catalog artifact for {dep_id}@{candidate.version} carries another identity")
            dep_step = PlannedInstall(
                dep_view, dep_artifact, dep_sidecar, "catalog",
                signature_status_of(dep_view, dep_sidecar or Path(os.devnull), trust_root),
                replaces=present.version if present else None)
            plan.notes.append(
                f"{mod_id} requires {dep_id} {dep_range}: will install {candidate.version} from the catalog"
                + (f" (replacing {present.version})" if present else ""))
            visit(dep_step)
        for dep_id, dep_range in (manifest.get("optional_dependencies") or {}).items():
            present = installed.get(dep_id)
            state = (f"installed {present.version}" + ("" if matches_range(present.version, dep_range) else ", outside the range")) \
                if present else "not installed"
            plan.recommended.append(f"{mod_id} recommends {dep_id} {dep_range}: {state}")
        visiting.pop()
        resolved[mod_id] = step
        plan.steps.append(step)

    visit(root_step)
    for step in plan.steps:
        _check_incompatibilities(step.view.manifest, installed, plan)
    return plan


def _check_incompatibilities(manifest: dict[str, Any], installed: dict[str, InstalledPackage],
                             plan: InstallPlan) -> None:
    mod_id, version = manifest["id"], manifest["version"]
    planned_ids = {step.view.manifest["id"] for step in plan.steps}
    for other_id, other_range in (manifest.get("incompatibilities") or {}).items():
        present = installed.get(other_id)
        if present is not None and other_id not in planned_ids and matches_range(present.version, other_range):
            raise _error(f"{mod_id} {version} is incompatible with installed {other_id} {present.version} "
                         f"(declared {other_range}); uninstall it first")
        planned = next((s for s in plan.steps if s.view.manifest["id"] == other_id), None)
        if planned is not None and matches_range(planned.view.manifest["version"], other_range):
            raise _error(f"{mod_id} {version} is incompatible with {other_id} "
                         f"{planned.view.manifest['version']} in the same plan")
    for other in installed.values():
        if other.id == mod_id:
            continue
        declared = other.view.manifest.get("incompatibilities") or {}
        if mod_id in declared and matches_range(version, declared[mod_id]):
            raise _error(f"installed {other.id} {other.version} declares {mod_id} {declared[mod_id]} "
                         f"incompatible; {version} cannot be installed next to it")


# --- applying a step ----------------------------------------------------------------

def _policy_requires_signature(ini: ModsIni) -> bool:
    value = ini.get("policy", "require_trusted_signature")
    return value is not None and value.strip() not in ("0", "", "off", "false", "no")


def apply_step(
    mods_root: Path,
    step: PlannedInstall,
    ledger: dict[str, Any],
    ini: ModsIni,
    *,
    granted_tier: int,
    source_label: str,
    catalog_label: str | None,
) -> Path:
    """Copy one validated artifact into `mods/` atomically, keeping the old one."""
    mod_id = step.view.manifest["id"]
    if package_kind(step.view) == "lua":
        return _apply_lua_step(mods_root, step, ledger, ini, granted_tier=granted_tier,
                               source_label=source_label, catalog_label=catalog_label)
    if package_kind(step.view) == "resource":
        return _apply_resource_step(mods_root, step, ledger, ini, granted_tier=granted_tier,
                                    source_label=source_label, catalog_label=catalog_label)
    target = mods_root / f"{mod_id}.wotbmod"
    target_sidecar = mods_root / f"{mod_id}.wotbmod.sig"
    directory_package = mods_root / mod_id
    _require(not (directory_package.is_dir() and (directory_package / "manifest.json").is_file()),
             f"{mod_id} is installed as a directory package at {directory_package}; "
             "the CLI manages .wotbmod archives only - remove the directory first")
    if target.exists():
        _require(target.is_file() and not _cli._is_reparse_or_symlink(target),
                 f"installed package path is not a plain file: {target}")
    stage = _cli._run_stage_path(mods_root, mod_id, ".stage")
    entry = ledger["packages"].get(mod_id) or {"history": []}
    entry.setdefault("history", [])
    replaced = False
    try:
        _cli._copy_file_contents(step.artifact, stage, "package archive to staging")
        staged_sha = _cli._sha256_file(stage)
        _require(staged_sha == step.view.package_sha256,
                 "staged package hash differs from the validated package")
        if target.exists():
            entry["history"].append(_backup_installed(mods_root, mod_id, target, target_sidecar))
        try:
            os.replace(stage, target)
        except OSError as exc:
            raise _error(f"cannot atomically install package at {target}: {exc}") from exc
        replaced = True
        if step.sidecar is not None:
            _atomic_write_bytes(target_sidecar, step.sidecar.read_bytes())
        else:
            try:
                target_sidecar.unlink(missing_ok=True)
            except OSError as exc:
                raise _error(f"cannot remove the stale signature {target_sidecar}: {exc}") from exc
    except BaseException:
        if replaced and entry["history"]:
            previous = entry["history"][-1]
            try:
                shutil.copyfile(mods_root / previous["backup"], target)
            except OSError:
                pass
        raise
    finally:
        try:
            stage.unlink(missing_ok=True)
        except OSError:
            pass
    ini.set("mods", mod_id, "1")
    ini.set("permissions", mod_id, str(granted_tier))
    entry.update({
        "version": step.view.manifest["version"],
        "sha256": step.view.package_sha256,
        "installed_at": _now(),
        "source": source_label,
        "catalog": catalog_label,
        "signature": {"status": step.signature.status, "key_id": step.signature.key_id},
        "tier_granted": granted_tier,
        "state": "installed",
    })
    ledger["packages"][mod_id] = entry
    return target


def _apply_lua_step(
    mods_root: Path,
    step: PlannedInstall,
    ledger: dict[str, Any],
    ini: ModsIni,
    *,
    granted_tier: int,
    source_label: str,
    catalog_label: str | None,
) -> Path:
    """Extract a Lua package into <mods>/lua/<id>/ atomically (stage folder, swap),
    keep its archive and sidecar under cache/lua_packages, back the old one up."""
    mod_id = step.view.manifest["id"]
    lua_root = mods_root / LUA_DIR
    lua_root.mkdir(parents=True, exist_ok=True)
    target = lua_root / mod_id
    _require(not target.exists() or (target.is_dir() and not _cli._is_reparse_or_symlink(target)),
             f"installed Lua mod path is not a plain folder: {target}")
    cached, cached_sig = lua_cache_paths(mods_root, mod_id)
    _require(_cli._sha256_file(step.artifact) == step.view.package_sha256,
             "staged package hash differs from the validated package")
    stage = lua_root / f".{mod_id}.stage-{secrets.token_hex(4)}"
    entry = ledger["packages"].get(mod_id) or {"history": []}
    entry.setdefault("history", [])
    try:
        _extract_lua_archive(step.artifact, stage)
        if target.exists():
            entry["history"].append(_backup_installed(mods_root, mod_id, target, cached_sig))
            shutil.rmtree(target)
        os.replace(stage, target)
    except BaseException:
        shutil.rmtree(stage, ignore_errors=True)
        raise
    cached.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write_bytes(cached, step.artifact.read_bytes())
    if step.sidecar is not None:
        _atomic_write_bytes(cached_sig, step.sidecar.read_bytes())
    else:
        cached_sig.unlink(missing_ok=True)
    entry.update({
        "version": step.view.manifest["version"],
        "sha256": step.view.package_sha256,
        "installed_at": _now(),
        "source": source_label,
        "catalog": catalog_label,
        "signature": {"status": step.signature.status, "key_id": step.signature.key_id},
        "tier_granted": granted_tier,
        "state": "installed",
        "kind": "lua",
    })
    ledger["packages"][mod_id] = entry
    return target


def _confirm(prompt: str, yes: bool) -> None:
    if yes:
        return
    if not sys.stdin.isatty():
        raise _error("confirmation needed; re-run with --yes after reading the summary above")
    answer = input(prompt + " [y/N] ").strip().lower()
    _require(answer in ("y", "yes"), "cancelled")


def _game(args: argparse.Namespace) -> tuple[Path, Path]:
    game_root, _executable = _cli._validate_run_client(args.game_root)
    mods_root = _cli._ensure_run_mods_root(game_root)
    return game_root, mods_root


# --- commands ------------------------------------------------------------------------

def command_install(args: argparse.Namespace) -> int:
    _game_root, mods_root = _game(args)
    ini = ModsIni(mods_root / "mods.ini")
    require_signature = bool(args.require_signature) or _policy_requires_signature(ini)
    installed = scan_installed(mods_root)
    catalog = Catalog(args.catalog) if args.catalog else None
    ledger = load_ledger(mods_root)
    with tempfile.TemporaryDirectory(prefix="wotbmod-install-") as temporary:
        workdir = Path(temporary)
        source = Path(args.source).expanduser()
        source_label = args.source
        if _is_url(args.source):
            downloaded = workdir / "download"
            downloaded.mkdir()
            # The file keeps the URL's last segment so messages stay recognisable,
            # and always the .wotbmod suffix load_package expects (a portal serves
            # artifacts at .../artifact).
            name = Path(urllib.parse.urlparse(args.source).path).name or "package"
            if not name.lower().endswith(".wotbmod"):
                name += ".wotbmod"
            source = downloaded / name
            _atomic_write_bytes(source, _http_get(args.source, MAX_DOWNLOAD_BYTES, "package download"))
            sidecar_url = args.source + ".sig"
            try:
                _atomic_write_bytes(source.parent / (source.name + ".sig"),
                                    _http_get(sidecar_url, trust.MAX_SIDECAR_BYTES, "signature download"))
            except Exception:
                pass
        _require(source.exists(), f"package source does not exist: {source}")
        sidecar = Path(args.sidecar).expanduser() if args.sidecar else None
        plan = build_plan(source, installed, _trust_root(mods_root), workdir,
                          sidecar=sidecar, catalog=catalog, prerelease=args.pre)

        # Everything the user needs to decide, before any file in mods/ changes.
        for index, step in enumerate(plan.steps):
            print(f"[{index + 1}/{len(plan.steps)}] " + ("dependency from the catalog" if step.origin == "catalog" else "requested package"))
            for line in describe_package(step.view, step.signature):
                print("  " + line)
        for note in plan.notes:
            print(f"note: {note}")
        for note in plan.recommended:
            print(f"recommended: {note}")

        for step in plan.steps:
            mod_id, version = step.view.manifest["id"], step.view.manifest["version"]
            present = installed.get(mod_id)
            if present is not None and present.version == version and \
                    present.view.package_sha256 == step.view.package_sha256 and not args.force:
                print(f"{mod_id} {version} is already installed with the same hash")
            if present is not None and present.version == version and \
                    present.view.package_sha256 != step.view.package_sha256:
                _require(bool(args.force),
                         f"{mod_id} {version} is already installed with a different hash "
                         f"({present.view.package_sha256[:12]}...); a published version is immutable - "
                         "bump the version, or pass --force to replace it deliberately")
            if present is not None and compare_versions(version, present.version) < 0:
                _require(bool(args.allow_downgrade),
                         f"{mod_id} {version} is older than installed {present.version}; "
                         "pass --allow-downgrade to install it anyway")
            if step.signature.status in ("invalid", "revoked", "unsupported"):
                raise _error(f"{mod_id} {version}: signature {step.signature.status}: {step.signature.detail}")
            if require_signature and step.signature.status != "valid":
                raise _error(f"{mod_id} {version}: a trusted signature is required "
                             f"(policy), but the package is {step.signature.status}")
            if present is not None:
                for other in installed.values():
                    if other.id == mod_id:
                        continue
                    needed = (other.view.manifest.get("dependencies") or {}).get(mod_id)
                    if needed is not None and not matches_range(version, needed):
                        _require(bool(args.force),
                                 f"installed {other.id} {other.version} requires {mod_id} {needed}; "
                                 f"{version} would break it (pass --force to do it anyway)")

        # What changes for the player compared with what is installed: new
        # permissions are named before the question, never discovered after.
        for step in plan.steps:
            mod_id = step.view.manifest["id"]
            present = installed.get(mod_id)
            if present is None:
                continue
            old_permissions = set(present.view.manifest.get("permissions", []))
            new_permissions = [p for p in step.view.manifest.get("permissions", []) if p not in old_permissions]
            old_tier = _tier_index(present.view.requested_permission_tier)
            new_tier = _tier_index(step.view.requested_permission_tier)
            if new_permissions or new_tier > old_tier:
                print(f"PERMISSION ESCALATION: {mod_id} {present.version} -> {step.view.manifest['version']} asks for "
                      f"{', '.join(new_permissions) or 'a higher tier'}"
                      + (f" (tier {TIER_NAMES[old_tier]} -> {TIER_NAMES[new_tier]}: {scan.RISK_LABELS[TIER_NAMES[new_tier]]})"
                         if new_tier > old_tier else ""))
        if not args.no_scan:
            for step in plan.steps:
                report = scan.scan_package(step.artifact, step.view.manifest)
                if report.findings:
                    print(f"scan {step.view.manifest['id']}: risk {report.risk} "
                          f"({report.counts['block']} blocking, {report.counts['warn']} warnings)")
                    for finding in report.findings:
                        if finding.severity != "info":
                            print(f"  [{finding.severity}] {finding.code}: {finding.message} ({finding.where})")
                if report.blocked and not args.force:
                    raise _error(f"{step.view.manifest['id']}: the static scan found blocking problems; "
                                 "not installing (pass --force to override deliberately)")

        requested_tier = _tier_index(plan.steps[-1].view.requested_permission_tier)
        granted_tier = requested_tier if args.tier is None else int(args.tier)
        _require(0 <= granted_tier <= 3, "--tier must be 0..3")
        if granted_tier < requested_tier:
            print(f"warning: granting tier {granted_tier} ({TIER_NAMES[granted_tier]}) below the requested "
                  f"{TIER_NAMES[requested_tier]}: the loader will refuse the higher-tier slots")
        if package_kind(plan.steps[-1].view) == "lua":
            print(f"Lua mod: extracted into mods/{LUA_DIR}/{plan.steps[-1].view.manifest['id']}/ "
                  f"(the Lua host grants tier {TIER_NAMES[granted_tier]} from its manifest)")
        elif package_kind(plan.steps[-1].view) == "resource":
            count = len(plan.steps[-1].view.manifest["files"])
            print(f"Game files: replaces {count} file(s) under Data/; the originals are kept in "
                  f"mods/{'/'.join(GAME_FILES_PARTS)}/ and restored by uninstall")
        else:
            print(f"mods.ini: [mods] {plan.steps[-1].view.manifest['id']}=1, "
                  f"[permissions] {plan.steps[-1].view.manifest['id']}={granted_tier} ({TIER_NAMES[granted_tier]})")
        _confirm("Install?", args.yes)

        for step in plan.steps:
            mod_id, version = step.view.manifest["id"], step.view.manifest["version"]
            present = installed.get(mod_id)
            if present is not None and present.version == version and \
                    present.view.package_sha256 == step.view.package_sha256 and not args.force:
                continue
            tier = granted_tier if step is plan.steps[-1] else _tier_index(step.view.requested_permission_tier)
            target = apply_step(mods_root, step, ledger, ini, granted_tier=tier,
                                source_label=source_label if step.origin == "source" else "catalog",
                                catalog_label=args.catalog)
            print(f"Installed: {mod_id} {version} -> {target}"
                  + {"lua": " (Lua mod folder)", "resource": " (game files replaced)"}.get(package_kind(step.view), ""))
            print(f"Package SHA-256: {step.view.package_sha256}")
        ini.save()
        save_ledger(mods_root, ledger)
    return 0


def _dependents_of(mod_id: str, installed: dict[str, InstalledPackage]) -> list[InstalledPackage]:
    return [other for other in installed.values()
            if other.id != mod_id and mod_id in (other.view.manifest.get("dependencies") or {})]


def command_uninstall(args: argparse.Namespace) -> int:
    _game_root, mods_root = _game(args)
    mod_id = args.id
    installed = scan_installed(mods_root)
    ledger = load_ledger(mods_root)
    ini = ModsIni(mods_root / "mods.ini")
    package = installed.get(mod_id)
    _require(package is not None, f"{mod_id} is not installed")
    assert package is not None
    _require(package.kind in ("archive", "lua", "resource"),
             f"{mod_id} is a {package.kind} package at {package.path}; the CLI removes .wotbmod archives, "
             "Lua folders and resource packages only")
    dependents = _dependents_of(mod_id, installed)
    if dependents and not args.force:
        names = ", ".join(f"{item.id} {item.version}" for item in dependents)
        raise _error(f"{mod_id} is required by {names}; uninstall them first or pass --force")
    target = package.path
    sidecar = trust.sidecar_path_for(target)
    print(f"Uninstall: {mod_id} {package.version} ({target})")
    _confirm("Remove it?", args.yes)
    entry = ledger["packages"].get(mod_id) or {"history": []}
    entry.setdefault("history", [])
    if package.kind == "lua":
        cached, cached_sig = lua_cache_paths(mods_root, mod_id)
        entry["history"].append(_backup_installed(mods_root, mod_id, target, cached_sig))
        try:
            shutil.rmtree(target)
            cached.unlink(missing_ok=True)
            cached_sig.unlink(missing_ok=True)
        except OSError as exc:
            raise _error(f"cannot remove {target}: {exc}") from exc
    elif package.kind == "resource":
        cached, cached_sig = resource_cache_paths(mods_root, mod_id)
        restored = restore_resource_targets(mods_root, entry, force=bool(args.force))
        entry["history"].append(_backup_installed(mods_root, mod_id, cached, cached_sig))
        try:
            cached.unlink(missing_ok=True)
            cached_sig.unlink(missing_ok=True)
        except OSError as exc:
            raise _error(f"cannot remove {cached}: {exc}") from exc
        for restored_target in restored:
            print(f"Restored original: Data/{restored_target}")
        entry.pop("targets", None)
    else:
        entry["history"].append(_backup_installed(mods_root, mod_id, target, sidecar))
        for path in (sidecar, target):
            try:
                path.unlink(missing_ok=True)
            except OSError as exc:
                raise _error(f"cannot remove {path}: {exc}") from exc
    ini.remove("mods", mod_id)
    ini.remove("permissions", mod_id)
    ini.save()
    quarantine = mods_root / "cache" / "auto_disabled_mod.ini"
    if quarantine.is_file():
        quarantine_ini = ModsIni(quarantine)
        if (quarantine_ini.get("auto_disable", "id") or "").lower() == mod_id.lower():
            quarantine_ini.remove("auto_disable", "id")
            quarantine_ini.save()
    entry.update({"state": "removed", "removed_at": _now(), "version": package.version,
                  "sha256": package.view.package_sha256})
    ledger["packages"][mod_id] = entry
    save_ledger(mods_root, ledger)
    print(f"Removed: {mod_id} {package.version}; a backup stays under {backup_root(mods_root) / mod_id}")
    return 0


def command_rollback(args: argparse.Namespace) -> int:
    _game_root, mods_root = _game(args)
    mod_id = args.id
    ledger = load_ledger(mods_root)
    entry = ledger["packages"].get(mod_id)
    _require(entry is not None and entry.get("history"), f"{mod_id} has no previous version to roll back to")
    assert entry is not None
    previous = entry["history"][-1]
    backup = mods_root / previous["backup"]
    _require(backup.is_file(), f"backup is missing: {backup}")
    _require(_cli._sha256_file(backup) == previous["sha256"], f"backup was modified on disk: {backup}")
    view = load_any_package(backup)  # the client allowlist still has to hold
    sidecar_backup = mods_root / previous["signature_backup"] if previous.get("signature_backup") else None
    if sidecar_backup is not None and not sidecar_backup.is_file():
        sidecar_backup = None
    signature = signature_status_of(view, sidecar_backup or Path(os.devnull), _trust_root(mods_root))
    ini = ModsIni(mods_root / "mods.ini")
    if _policy_requires_signature(ini) or args.require_signature:
        _require(signature.status == "valid", f"the previous version is {signature.status}; policy requires a trusted signature")
    current = entry.get("version") if entry.get("state") == "installed" else None
    print(f"Rollback: {mod_id} {current or '(not installed)'} -> {view.manifest['version']}")
    for line in describe_package(view, signature):
        print("  " + line)
    _confirm("Restore it?", args.yes)
    entry["history"].pop()
    step = PlannedInstall(view, backup, sidecar_backup, "backup", signature)
    tier = entry.get("tier_granted")
    if not isinstance(tier, int):
        tier = _tier_index(view.requested_permission_tier)
    apply_step(mods_root, step, ledger, ini, granted_tier=tier, source_label="rollback",
               catalog_label=entry.get("catalog"))
    ini.save()
    save_ledger(mods_root, ledger)
    print(f"Restored: {mod_id} {view.manifest['version']}")
    print(f"Package SHA-256: {view.package_sha256}")
    return 0


def command_update(args: argparse.Namespace) -> int:
    _game_root, mods_root = _game(args)
    installed = scan_installed(mods_root)
    ledger = load_ledger(mods_root)
    targets: list[str]
    if args.all:
        targets = [mod_id for mod_id, item in installed.items() if item.kind == "archive"]
    else:
        _require(bool(args.id), "pass a package id or --all")
        targets = [args.id]
    updates: list[tuple[str, str, CatalogVersion, str]] = []
    for mod_id in targets:
        present = installed.get(mod_id)
        _require(present is not None, f"{mod_id} is not installed")
        assert present is not None
        catalog_source = args.catalog or (ledger["packages"].get(mod_id) or {}).get("catalog")
        if not catalog_source:
            print(f"{mod_id}: no catalog known (pass --catalog)")
            continue
        catalog = Catalog(catalog_source)
        best = catalog.best(mod_id, "*", prerelease=args.pre, client_build=_cli.EXPECTED_CLIENT_BUILD)
        if best is None:
            print(f"{mod_id}: not in the catalog {catalog_source}")
            continue
        if compare_versions(best.version, present.version) <= 0:
            print(f"{mod_id}: {present.version} is current")
            continue
        updates.append((mod_id, present.version, best, catalog_source))
        print(f"{mod_id}: {present.version} -> {best.version} ({catalog_source})")
    if args.check or not updates:
        return 0
    for mod_id, _old, best, catalog_source in updates:
        catalog = Catalog(catalog_source)
        with tempfile.TemporaryDirectory(prefix="wotbmod-update-") as temporary:
            workdir = Path(temporary)
            artifact, sidecar = catalog.fetch(best, workdir)
            sub_args = argparse.Namespace(
                game_root=args.game_root, source=str(artifact), sidecar=str(sidecar) if sidecar else None,
                catalog=catalog_source, yes=args.yes, force=False, allow_downgrade=False,
                require_signature=args.require_signature, tier=None, pre=args.pre, no_scan=False)
            result = command_install(sub_args)
            if result != 0:
                return result
    return 0


def _quarantined(mods_root: Path) -> str | None:
    quarantine = mods_root / "cache" / "auto_disabled_mod.ini"
    if not quarantine.is_file():
        return None
    return ModsIni(quarantine).get("auto_disable", "id") or None


def command_list(args: argparse.Namespace) -> int:
    game_root = Path(args.game_root).expanduser()
    mods_root = game_root / "mods"
    installed = scan_installed(mods_root)
    ledger = load_ledger(mods_root) if mods_root.is_dir() else {"schema": LEDGER_SCHEMA, "packages": {}}
    ini = ModsIni(mods_root / "mods.ini")
    quarantined = _quarantined(mods_root)
    stale_marker = (mods_root / "cache" / "runtime_session.marker").exists()
    rows: list[dict[str, Any]] = []
    for mod_id, item in sorted(installed.items()):
        enabled_value = ini.get("mods", mod_id)
        granted_value = ini.get("permissions", mod_id)
        if item.kind in ("lua", "resource"):
            cached, cached_sig = _cached_archive_paths(mods_root, item.kind, mod_id)
            if cached.is_file():
                signature = trust.verify_artifact(cached, _trust_root(mods_root), sidecar=cached_sig,
                                                  release=f"{mod_id}@{item.version}")
            else:
                signature = trust.VerifyResult("unsigned", None, "a Lua folder without its archive")
        else:
            signature = signature_status_of(item.view, _sidecar_for_installed(item), _trust_root(mods_root))
        entry = ledger["packages"].get(mod_id) or {}
        rows.append({
            "id": mod_id,
            "version": item.version,
            "kind": item.kind,
            "path": str(item.path),
            "enabled": enabled_value is None or enabled_value.strip() != "0",
            "requested_tier": item.view.requested_permission_tier,
            "granted_tier": TIER_NAMES[max(0, min(3, int(granted_value)))] if granted_value and granted_value.strip().isdigit() else "SAFE",
            "signature": signature.status,
            "signature_key": signature.key_id,
            "quarantined": quarantined is not None and quarantined.lower() == mod_id.lower(),
            "managed": entry.get("state") == "installed",
            "backups": len(entry.get("history", [])),
            "dependencies": item.view.manifest.get("dependencies") or {},
        })
    result = {
        "mods_root": str(mods_root),
        "policy_require_trusted_signature": _policy_requires_signature(ini),
        "quarantined": quarantined,
        "stale_session_marker": stale_marker,
        "packages": rows,
    }
    if args.json:
        _cli._json_print(result)
        return 0
    print(f"mods: {mods_root}")
    print(f"policy: trusted signature {'required' if result['policy_require_trusted_signature'] else 'not required'}")
    if quarantined:
        print(f"quarantine: {quarantined} is auto-disabled after repeated crashes")
    if stale_marker:
        print("safe mode: a stale session marker is present; the next start loads no third-party mods")
    if not rows:
        print("(no packages)")
    for row in rows:
        flags = []
        if not row["enabled"]:
            flags.append("disabled")
        if row["quarantined"]:
            flags.append("quarantined")
        if row["backups"]:
            flags.append(f"{row['backups']} backup(s)")
        print(f"{row['id']:<40} {row['version']:<12} {row['kind']:<9} tier {row['requested_tier']}/{row['granted_tier']}"
              f"  signature {row['signature']}" + (f" ({row['signature_key']})" if row["signature_key"] else "")
              + ("  " + ", ".join(flags) if flags else ""))
    return 0


def _crash_state(mods_root: Path) -> dict[str, Any]:
    """What the loader's crash-loop guard recorded: the quarantined id and the history."""
    cache = mods_root / "cache"
    quarantine = ModsIni(cache / "auto_disabled_mod.ini")
    history = ModsIni(cache / "crash_history.ini")
    return {
        "quarantined": quarantine.get("auto_disable", "id") or None,
        "last_mod": history.get("crash_history", "last_mod"),
        "count": int(history.get("crash_history", "count") or 0),
        "stale_session_marker": (cache / "runtime_session.marker").exists(),
        "quarantine_file": str(cache / "auto_disabled_mod.ini"),
        "history_file": str(cache / "crash_history.ini"),
    }


def command_quarantine(args: argparse.Namespace) -> int:
    """Show or lift the loader's crash-loop quarantine.

    The loader disables a mod on its own after it crashed the client twice in
    a row (mods/cache/auto_disabled_mod.ini) so the game keeps starting; this
    command explains that state and, with --clear, gives the mod another
    chance without editing files by hand.
    """
    game_root = Path(args.game_root).expanduser()
    mods_root = game_root / "mods"
    state = _crash_state(mods_root)
    if args.clear:
        _require(state["quarantined"] is not None and state["quarantined"].lower() == args.clear.lower(),
                 f"{args.clear} is not the quarantined mod (quarantined: {state['quarantined'] or 'none'})")
        quarantine = ModsIni(mods_root / "cache" / "auto_disabled_mod.ini")
        quarantine.remove("auto_disable", "id")
        quarantine.save()
        history = ModsIni(mods_root / "cache" / "crash_history.ini")
        history.set("crash_history", "count", "0")
        history.save()
        state = _crash_state(mods_root)
        state["lifted"] = args.clear
        if not args.json:
            print(f"quarantine lifted for {args.clear}; the loader will load it again on the next start")
    if args.json:
        _cli._json_print(state)
        return 0
    if state["quarantined"]:
        print(f"quarantined: {state['quarantined']} (auto-disabled after {state['count']} crash(es) in a row; "
              f"lift with: wotbmod quarantine --clear {state['quarantined']})")
    else:
        print("quarantined: none")
    if state["last_mod"]:
        print(f"last crash attributed to: {state['last_mod']} (count {state['count']})")
    if state["stale_session_marker"]:
        print("stale session marker: the previous session did not end cleanly; the next start runs in SAFE MODE "
              "(no third-party mods) unless the marker is removed")
    return 0


def _log_evidence(game_root: Path, mod_id: str, limit: int = 12) -> list[str]:
    """The last loader log lines that mention the mod (blocked/quarantine/crash reasons live there)."""
    log = game_root / "wotb_mod_loader.log"
    if not log.is_file():
        return []
    try:
        data = log.read_bytes()
    except OSError:
        return []
    if len(data) > 8 * 1024 * 1024:
        data = data[-8 * 1024 * 1024:]
    needle = mod_id.lower()
    lines = [line for line in data.decode("utf-8", errors="replace").splitlines() if needle in line.lower()]
    return lines[-limit:]


def command_info(args: argparse.Namespace) -> int:
    """Everything the CLI knows about one mod, in one place."""
    game_root = Path(args.game_root).expanduser()
    mods_root = game_root / "mods"
    mod_id = args.id
    installed = scan_installed(mods_root)
    ledger = load_ledger(mods_root) if mods_root.is_dir() else {"schema": LEDGER_SCHEMA, "packages": {}}
    entry = ledger["packages"].get(mod_id) or {}
    ini = ModsIni(mods_root / "mods.ini")
    package = installed.get(mod_id)
    state = _crash_state(mods_root)
    catalog_source = entry.get("catalog")
    page = None
    if catalog_source and _is_url(catalog_source):
        base = catalog_source.rstrip("/")
        base = base[: -len("/api/v1")] if base.endswith("/api/v1") else base
        page = f"{base}/mods/{mod_id}"
    signature = None
    if package is not None:
        if package.kind in ("lua", "resource"):
            cached, cached_sig = _cached_archive_paths(mods_root, package.kind, mod_id)
            signature = trust.verify_artifact(cached, _trust_root(mods_root), sidecar=cached_sig,
                                              release=f"{mod_id}@{package.version}") if cached.is_file() else None
        else:
            signature = signature_status_of(package.view, _sidecar_for_installed(package), _trust_root(mods_root))
    enabled_value = ini.get("mods", mod_id)
    granted_value = ini.get("permissions", mod_id)
    result = {
        "id": mod_id,
        "installed": package is not None,
        "version": package.version if package else entry.get("version"),
        "kind": package.kind if package else entry.get("kind"),
        "path": str(package.path) if package else None,
        "enabled": None if package is None else (enabled_value is None or enabled_value.strip() != "0"),
        "requested_tier": package.view.requested_permission_tier if package else None,
        "granted_tier": TIER_NAMES[max(0, min(3, int(granted_value)))] if granted_value and granted_value.strip().isdigit() else None,
        "permissions": list(package.view.manifest.get("permissions", [])) if package else [],
        "dependencies": dict(package.view.manifest.get("dependencies") or {}) if package else {},
        "signature": signature.status if signature else None,
        "signature_key": signature.key_id if signature else None,
        "ledger": {key: entry.get(key) for key in ("state", "installed_at", "source", "catalog", "sha256", "tier_granted", "removed_at")},
        "backups": [{"version": item.get("version"), "sha256": item.get("sha256"), "backup": item.get("backup")}
                    for item in entry.get("history", [])],
        "quarantined": state["quarantined"] is not None and state["quarantined"].lower() == mod_id.lower(),
        "crash_count": state["count"] if (state["last_mod"] or "").lower() == mod_id.lower() else 0,
        "page": page,
        "log": _log_evidence(game_root, mod_id),
        "log_file": str(game_root / "wotb_mod_loader.log"),
    }
    if args.json:
        _cli._json_print(result)
        return 0
    print(f"{mod_id}: {'installed ' + str(result['version']) if result['installed'] else 'not installed'}"
          + (f" ({result['kind']}, {result['path']})" if result["installed"] else ""))
    if result["installed"]:
        print(f"enabled: {'yes' if result['enabled'] else 'no (mods.ini)'}; tier requested {result['requested_tier']}, "
              f"granted {result['granted_tier'] or 'SAFE (default)'}")
        print(f"signature: {result['signature'] or 'unknown'}" + (f" (key {result['signature_key']})" if result["signature_key"] else ""))
        print("permissions: " + (", ".join(result["permissions"]) or "(none)"))
        if result["dependencies"]:
            print("requires: " + ", ".join(f"{k} {v}" for k, v in result["dependencies"].items()))
    if entry:
        print(f"ledger: {entry.get('state')} from {entry.get('source')} at {entry.get('installed_at')}"
              + (f", catalog {entry.get('catalog')}" if entry.get("catalog") else ""))
        if result["backups"]:
            print("backups: " + ", ".join(f"{b['version']} ({b['sha256'][:8]})" for b in result["backups"]))
    if result["quarantined"]:
        print(f"QUARANTINED: crashed the client {result['crash_count']} time(s) in a row; "
              f"wotbmod quarantine --clear {mod_id} lifts it")
    if page:
        print(f"page: {page}")
    if result["log"]:
        print(f"loader log ({result['log_file']}), last {len(result['log'])} mention(s):")
        for line in result["log"]:
            print("  " + line[:200])
    else:
        print("loader log: no mention of this id")
    return 0


def command_report_crash(args: argparse.Namespace) -> int:
    """Tell the portal that this mod crashed the client (what quarantine recorded)."""
    game_root = Path(args.game_root).expanduser()
    mods_root = game_root / "mods"
    state = _crash_state(mods_root)
    installed = scan_installed(mods_root)
    ledger = load_ledger(mods_root) if mods_root.is_dir() else {"schema": LEDGER_SCHEMA, "packages": {}}
    mod_id = args.id
    package = installed.get(mod_id)
    entry = ledger["packages"].get(mod_id) or {}
    version = args.version or (package.version if package else entry.get("version"))
    _require(bool(version), f"{mod_id}: no installed version known; pass --version")
    count = args.count
    if count is None:
        count = state["count"] if (state["last_mod"] or "").lower() == mod_id.lower() else 1
    portal = args.to or entry.get("catalog")
    _require(bool(portal) and _is_url(portal), "pass --to https://<portal>/api/v1 (or install from a portal catalogue first)")
    base = portal if portal.endswith("/") else portal + "/"
    url = urllib.parse.urljoin(base, f"mods/{urllib.parse.quote(mod_id)}/crashes")
    payload = json.dumps({"version": version, "client_build": _cli.EXPECTED_CLIENT_BUILD, "count": int(count)}).encode("utf-8")
    headers = {"Content-Type": "application/json", "User-Agent": USER_AGENT}
    token = args.token or os.environ.get("WOTBMOD_PORTAL_TOKEN")
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, data=payload, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
            answer = json.loads(response.read(65536).decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise _error(f"portal refused the crash report: HTTP {exc.code}: {exc.read(4096).decode('utf-8', errors='replace')[:300]}") from exc
    except (urllib.error.URLError, OSError, ValueError) as exc:
        raise _error(f"cannot reach the portal at {url}: {exc}") from exc
    print(f"Reported: {mod_id} {version} on client {_cli.EXPECTED_CLIENT_BUILD}, {count} crash(es) -> {url}")
    if args.json:
        _cli._json_print(answer)
    return 0


def _sync_errors() -> tuple[type[BaseException], ...]:
    """The error classes a sync step may raise: this module's own and the CLI's once registered."""
    return (CliError, _cli.CliError) if _cli is not None else (CliError,)


def _telemetry_enabled(ini: ModsIni) -> bool:
    """`[telemetry] crashes=0` in mods.ini opts out of sending the loader's crash record."""
    value = (ini.get("telemetry", "crashes") or "1").strip().lower()
    return value not in ("0", "no", "off", "false")


def _telemetry_state_path(mods_root: Path) -> Path:
    return mods_root / "cache" / "telemetry.json"


def command_sync(args: argparse.Namespace) -> int:
    """One pass a player can run (the launcher runs it after every install):
    print the portal's notice, remove releases the portal revoked, and send the
    loader's crash record for the mod it quarantined - once per record."""
    game_root, mods_root = _game(args)
    ini = ModsIni(mods_root / "mods.ini")
    installed = scan_installed(mods_root)
    ledger = load_ledger(mods_root)
    sources: dict[str, list[str]] = {}
    for mod_id, entry in ledger.get("packages", {}).items():
        source = (entry or {}).get("catalog")
        if source and (_is_url(source) or Path(source).expanduser().exists()):
            sources.setdefault(source, []).append(mod_id)
    if args.catalog:
        sources.setdefault(args.catalog, [])
    if not sources:
        print("sync: no portal catalogue known (install from one, or pass --catalog)")
        return 0
    exit_code = 0
    removed = 0
    for source, mod_ids in sources.items():
        try:
            catalog = Catalog(source)
            notice = catalog.notice()
            revoked = catalog.revoked()
        except _sync_errors() as exc:
            print(f"sync: {source}: {exc}")
            exit_code = 1
            continue
        if notice:
            print(f"{source}: {notice}")
        for item in revoked:
            package = installed.get(item["id"])
            if package is None or item["version"] not in ("*", package.version):
                continue
            entry = ledger.get("packages", {}).get(item["id"]) or {}
            if entry.get("catalog") not in (None, source) and item["id"] not in mod_ids:
                continue
            print(f"{item['id']} {package.version}: revoked by the portal ({item['reason']})")
            if args.check:
                continue
            sub = argparse.Namespace(game_root=args.game_root, id=item["id"], yes=args.yes, force=True)
            try:
                result = command_uninstall(sub)
            except _sync_errors() as exc:
                print(f"  not removed: {exc}")
                exit_code = 1
                continue
            if result == 0:
                removed += 1
            else:
                exit_code = result
    # The loader's crash-loop record, sent once per (mod, count).
    if _telemetry_enabled(ini):
        state = _crash_state(mods_root)
        last_mod = (state.get("last_mod") or "").strip()
        count = int(state.get("count") or 0)
        marker = _telemetry_state_path(mods_root)
        reported: dict[str, Any] = {}
        if marker.is_file():
            try:
                reported = json.loads(marker.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                reported = {}
        if last_mod and count > 0 and not (reported.get("last_mod") == last_mod and int(reported.get("count", 0)) >= count):
            entry = ledger.get("packages", {}).get(last_mod) or {}
            # Only the portal that installed the mod knows it; a dev-folder mod
            # or a hand-copied package has nowhere to report to.
            portal = entry.get("catalog")
            package = installed.get(last_mod)
            version = package.version if package else entry.get("version")
            if portal and _is_url(portal) and version:  # crash reports go to portals only
                if args.check:
                    print(f"{last_mod} {version}: {count} crash(es) recorded by the loader, would be reported to {portal}")
                else:
                    sub = argparse.Namespace(game_root=str(game_root), id=last_mod, to=portal, version=version,
                                             count=count, token=None, json=False)
                    try:
                        command_report_crash(sub)
                        marker.parent.mkdir(parents=True, exist_ok=True)
                        marker.write_text(json.dumps({"last_mod": last_mod, "count": count, "reported_at": _now()}),
                                          encoding="utf-8")
                    except _sync_errors() as exc:
                        print(f"crash report not sent: {exc}")
    else:
        print("sync: crash telemetry is off ([telemetry] crashes=0)")
    if removed:
        print(f"sync: removed {removed} revoked package(s)")
    return exit_code


def command_verify(args: argparse.Namespace) -> int:
    artifact = Path(args.artifact).expanduser()
    _require(artifact.exists(), f"artifact does not exist: {artifact}")
    if args.trust_root:
        trust_root = Path(args.trust_root).expanduser()
    else:
        trust_root = Path(args.game_root).expanduser() / "mods" / TRUST_DIR
    sidecar = Path(args.sidecar).expanduser() if args.sidecar else trust.sidecar_path_for(artifact)
    release = None
    digest: str
    if artifact.is_dir() or artifact.suffix.lower() == ".wotbmod":
        view = load_any_package(artifact, client_check=False)
        release = f"{view.manifest['id']}@{view.manifest['version']}"
        digest = view.package_sha256
    else:
        digest = _cli._sha256_file(artifact)
    result = trust.verify_digest(digest, sidecar, trust_root, release=release)
    payload = {"artifact": str(artifact), "sha256": digest, "sidecar": str(sidecar),
               "trust_root": str(trust_root), "status": result.status, "key_id": result.key_id,
               "detail": result.detail, "release": release}
    if args.json:
        _cli._json_print(payload)
    else:
        print(f"{result.status.upper()} {artifact}")
        print(f"sha256: {digest}")
        if result.key_id:
            print(f"key_id: {result.key_id}")
        print(f"detail: {result.detail}")
    return 0 if result.status == "valid" else 3


def command_scan(args: argparse.Namespace) -> int:
    source = Path(args.source).expanduser()
    _require(source.exists(), f"package source does not exist: {source}")
    manifest = None
    try:
        manifest = load_any_package(source, client_check=False).manifest
    except Exception:
        pass  # an invalid package is still scanned for what is plainly there
    report = scan.scan_package(source, manifest)
    if args.json:
        _cli._json_print(report.to_dict())
    else:
        print(scan.render_text(report))
    return 3 if report.blocked else 0


def command_keygen(args: argparse.Namespace) -> int:
    _require(trust.KEY_ID_RE.fullmatch(args.key_id) is not None,
             "--key-id: letters, digits, '_', '.', '-', up to 127 characters")
    out = Path(args.out).expanduser()
    _require(not out.exists(), f"refusing to overwrite an existing key file: {out}")
    private_key = trust.generate_private_key()
    public_x, public_y = trust.public_key_of(private_key)
    out.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    handle = os.open(out, flags, 0o600)
    with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(f"{private_key:064x}\n")
    public_path = out.parent / f"{args.key_id}.p256"
    public_path.write_text(trust.public_key_to_hex(public_x, public_y) + "\n", encoding="utf-8", newline="\n")
    print(f"Private key: {out} (keep it secret; anyone holding it can sign as {args.key_id})")
    print(f"Public key:  {public_path} (copy it to <game>/mods/trust/keys/ on every client that should trust it)")
    return 0


def _read_private_key(path: Path) -> int:
    text = path.read_text(encoding="utf-8").strip()
    _require(len(text) == 64 and trust.HEX_RE.fullmatch(text) is not None,
             f"private key file must hold 64 hex characters: {path}")
    value = int(text, 16)
    _require(1 <= value < trust.N, "private key is out of range")
    return value


def _sign_with_cng(artifact: Path, sidecar: Path, public_key_out: Path, key_id: str, key_name: str | None) -> None:
    script = Path(_cli.SDK_ROOT) / "tools" / "sign_release_artifact.ps1"
    _require(script.is_file(), f"signer script is missing: {script}")
    command = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
               "-Artifact", str(artifact), "-SignatureOutput", str(sidecar),
               "-PublicKeyOutput", str(public_key_out), "-KeyId", key_id]
    if key_name:
        command += ["-KeyName", key_name]
    try:
        completed = subprocess.run(command, capture_output=True, text=True, check=False)
    except OSError as exc:
        raise _error(f"cannot run the CNG signer: {exc}") from exc
    _require(completed.returncode == 0,
             f"CNG signer failed ({completed.returncode}): {completed.stderr.strip()[:400]}")


def release_record(view: Any, artifact: Path, sidecar: Sidecar | None, *, public_key: str | None,
                   notes: str | None, suggested: dict[str, str]) -> dict[str, Any]:
    manifest = view.manifest
    record: dict[str, Any] = {
        "schema": RELEASE_SCHEMA,
        "id": manifest["id"],
        "version": manifest["version"],
        "name": manifest["name"],
        "developer": manifest["developer"],
        "type": manifest["type"],
        "created_at": _now(),
        "status": "published",
        "artifact": {
            "file": artifact.name,
            "sha256": view.package_sha256,
            "payload_sha256": view.payload_sha256,
            "size": artifact.stat().st_size,
            "file_count": len(view.files),
        },
        "signature": None,
        "api": dict(manifest.get("api", {})),
        "client": dict(manifest.get("client", {})),
        "permissions": list(manifest.get("permissions", [])),
        "permission_tier": view.requested_permission_tier,
        "dependencies": dict(manifest.get("dependencies") or {}),
        "optional_dependencies": dict(manifest.get("optional_dependencies") or {}),
        "incompatibilities": dict(manifest.get("incompatibilities") or {}),
        "suggested": suggested,
        "notes": notes or "",
    }
    if sidecar is not None:
        record["signature"] = {
            "file": artifact.name + ".sig",
            "algorithm": sidecar.algorithm,
            "key_id": sidecar.key_id,
            "public_key": public_key,
        }
    return record


Sidecar = trust.Sidecar


def command_release(args: argparse.Namespace) -> int:
    project = Path(args.project).expanduser().resolve(strict=True)
    _require(project.is_dir(), f"project is not a directory: {project}")
    view = load_any_package(project)
    mod_id, version = view.manifest["id"], view.manifest["version"]
    output_dir = Path(args.output_dir).expanduser().resolve(strict=False) if args.output_dir \
        else project.parent / "releases"
    _require(not _cli._path_is_within(project, output_dir), "the release directory must be outside the project")
    output_dir.mkdir(parents=True, exist_ok=True)
    artifact = output_dir / f"{mod_id}-{version}.wotbmod"
    record_path = output_dir / f"{mod_id}-{version}{RELEASE_SUFFIX}"
    sidecar_path = output_dir / (artifact.name + ".sig")

    with tempfile.TemporaryDirectory(prefix="wotbmod-release-", dir=output_dir) as temporary:
        staged = Path(temporary) / artifact.name
        packed = pack_any(project, staged)
        # A directory hashes as a tree, the archive as its bytes: the record
        # and the sidecar carry the archive digest, the one the loader checks.
        _require(packed.payload_sha256 == view.payload_sha256, "packed payload differs from the project view")
        # A version, once released, does not change: the same bytes may be
        # re-released (idempotent), different bytes need a new version.
        if record_path.is_file():
            previous = _read_json(record_path, "previous release record")
            previous_sha = str(previous.get("artifact", {}).get("sha256", "")).lower()
            if previous_sha and previous_sha != packed.package_sha256:
                raise _error(f"{mod_id} {version} was already released with hash {previous_sha[:12]}...; "
                             "a released version is immutable - bump the version in manifest.json")
        os.replace(staged, artifact)

    sidecar: Sidecar | None = None
    public_key: str | None = None
    suggested = {}
    for item in args.suggest or []:
        _require("=" in item, "--suggest expects id=range")
        dep_id, dep_range = item.split("=", 1)
        _require(_cli._is_ascii_identifier(dep_id, _cli.MAX_ID_BYTES) and _cli._valid_version_range(dep_range),
                 f"--suggest has an invalid id or range: {item}")
        suggested[dep_id] = dep_range
    if args.sign_with_key:
        private_key = _read_private_key(Path(args.sign_with_key).expanduser())
        _require(bool(args.key_id), "--sign-with-key needs --key-id")
        sidecar = trust.sign_file(artifact, private_key, args.key_id)
        _atomic_write_bytes(sidecar_path, sidecar.render().encode("utf-8"))
        public_key = trust.public_key_to_hex(*trust.public_key_of(private_key))
        (output_dir / f"{args.key_id}.p256").write_text(public_key + "\n", encoding="utf-8", newline="\n")
    elif args.sign:
        key_id = args.key_id or "blitzforge-preview-2026"
        public_key_path = output_dir / f"{key_id}.p256"
        _sign_with_cng(artifact, sidecar_path, public_key_path, key_id, args.key_name)
        sidecar = trust.parse_sidecar(sidecar_path.read_text(encoding="utf-8"))
        public_key = public_key_path.read_text(encoding="utf-8").strip()
        _require(sidecar.sha256.lower() == packed.package_sha256, "the signer hashed different bytes")
        _require(trust.verify_p256_sha256(public_key, sidecar.sha256, sidecar.signature),
                 "the signer's signature does not verify under its own public key")
    else:
        try:
            sidecar_path.unlink(missing_ok=True)
        except OSError:
            pass
    notes = Path(args.notes).expanduser().read_text(encoding="utf-8") if args.notes else None
    report = scan.scan_package(artifact, packed.manifest)
    if report.blocked and not args.allow_scan_findings:
        for finding in report.findings:
            if finding.severity == "block":
                print(f"  [block] {finding.code}: {finding.message} ({finding.where})")
        try:
            artifact.unlink(missing_ok=True)
            sidecar_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise _error("the static scan found blocking problems; fix them or pass --allow-scan-findings "
                     "(the catalogue will show them to every player)")
    record = release_record(packed, artifact, sidecar, public_key=public_key, notes=notes, suggested=suggested)
    record["scan"] = report.to_dict()
    _write_json(record_path, record)
    if args.json:
        _cli._json_print({**record, "record": str(record_path), "artifact_path": str(artifact),
                          "signature_path": str(sidecar_path) if sidecar else None})
    else:
        print(f"Released: {mod_id} {version}")
        print(f"Artifact: {artifact}")
        print(f"Package SHA-256: {packed.package_sha256}")
        print(f"Signature: {sidecar_path if sidecar else '(unsigned - pass --sign or --sign-with-key)'}")
        print(f"Record: {record_path}")
    return 0


def _load_release(record_path: Path) -> tuple[dict[str, Any], Path, Path | None, Any]:
    record = _read_json(record_path, "release record")
    _require(isinstance(record, dict) and record.get("schema") == RELEASE_SCHEMA, "release record has an unknown schema")
    artifact = record_path.parent / str(record["artifact"]["file"])
    _require(artifact.is_file(), f"release artifact is missing: {artifact}")
    view = load_any_package(artifact, client_check=False)
    _require(view.package_sha256 == str(record["artifact"]["sha256"]).lower(),
             "release artifact hash does not match its record")
    _require(view.manifest["id"] == record["id"] and view.manifest["version"] == record["version"],
             "release artifact identity does not match its record")
    sidecar = None
    signature = record.get("signature")
    if isinstance(signature, dict):
        sidecar = record_path.parent / str(signature["file"])
        _require(sidecar.is_file(), f"release signature is missing: {sidecar}")
        parsed = trust.parse_sidecar(sidecar.read_text(encoding="utf-8"))
        _require(parsed.sha256.lower() == view.package_sha256, "release signature covers other bytes")
        public_key = signature.get("public_key")
        if public_key:
            _require(trust.verify_p256_sha256(public_key, parsed.sha256, parsed.signature),
                     "release signature does not verify under the public key in its record")
    return record, artifact, sidecar, view


def _multipart(fields: dict[str, str], files: dict[str, tuple[str, bytes, str]]) -> tuple[bytes, str]:
    boundary = "----wotbmod" + secrets.token_hex(12)
    body = bytearray()
    for name, value in fields.items():
        body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n").encode()
        body += value.encode("utf-8") + b"\r\n"
    for name, (filename, data, content_type) in files.items():
        body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"; "
                 f"filename=\"{filename}\"\r\nContent-Type: {content_type}\r\n\r\n").encode()
        body += data + b"\r\n"
    body += f"--{boundary}--\r\n".encode()
    return bytes(body), f"multipart/form-data; boundary={boundary}"


def publish_to_directory(catalog_dir: Path, record: dict[str, Any], artifact: Path, sidecar: Path | None) -> Path:
    """Copy a release into `<catalog>/releases/<id>/<version>/` and index it (immutably)."""
    catalog = Catalog(str(catalog_dir))
    index = catalog.index()
    mod_id, version = record["id"], record["version"]
    packages = index["packages"]
    entry = packages.setdefault(mod_id, {"name": record["name"], "developer": record["developer"], "versions": {}})
    existing = entry.get("versions", {}).get(version)
    if existing is not None:
        existing_sha = str(existing.get("artifact", {}).get("sha256", "")).lower()
        _require(existing_sha == record["artifact"]["sha256"].lower(),
                 f"{mod_id} {version} is already in the catalog with another hash; versions are immutable")
    destination = catalog_dir / "releases" / mod_id / version
    destination.mkdir(parents=True, exist_ok=True)
    _cli._copy_file_contents(artifact, destination / artifact.name, "release artifact")
    if sidecar is not None:
        _cli._copy_file_contents(sidecar, destination / sidecar.name, "release signature")
    relative = f"releases/{mod_id}/{version}/"
    published = dict(record)
    published["artifact"] = {**record["artifact"], "path": relative + artifact.name}
    if sidecar is not None and isinstance(record.get("signature"), dict):
        published["signature"] = {**record["signature"], "path": relative + sidecar.name}
    published["published_at"] = _now()
    entry["name"] = record["name"]
    entry["developer"] = record["developer"]
    entry.setdefault("versions", {})[version] = published
    entry["latest"] = max(entry["versions"], key=version_key)
    index["schema"] = INDEX_SCHEMA
    index["generated_at"] = _now()
    _write_json(catalog_dir / "index.json", index)
    return destination


def publish_to_portal(base_url: str, token: str | None, record: dict[str, Any], artifact: Path,
                      sidecar: Path | None) -> dict[str, Any]:
    base = base_url if base_url.endswith("/") else base_url + "/"
    url = urllib.parse.urljoin(base, PUBLISH_ENDPOINT)
    files = {"artifact": (artifact.name, artifact.read_bytes(), "application/octet-stream")}
    if sidecar is not None:
        files["signature"] = (sidecar.name, sidecar.read_bytes(), "text/plain")
    body, content_type = _multipart({"record": json.dumps(record, ensure_ascii=False, sort_keys=True)}, files)
    headers = {"Content-Type": content_type, "User-Agent": USER_AGENT}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_SECONDS) as response:
            payload = response.read(1024 * 1024)
    except urllib.error.HTTPError as exc:
        detail = exc.read(4096).decode("utf-8", errors="replace")
        raise _error(f"portal refused the release: HTTP {exc.code}: {detail[:400]}") from exc
    except (urllib.error.URLError, OSError) as exc:
        raise _error(f"cannot reach the portal at {url}: {exc}") from exc
    try:
        return json.loads(payload.decode("utf-8"))
    except (ValueError, UnicodeDecodeError) as exc:
        raise _error("portal answered with something other than JSON") from exc


def command_publish(args: argparse.Namespace) -> int:
    record_path = Path(args.record).expanduser()
    _require(record_path.is_file() and record_path.name.endswith(RELEASE_SUFFIX),
             f"expected a *{RELEASE_SUFFIX} written by `wotbmod release`: {record_path}")
    record, artifact, sidecar, _view = _load_release(record_path)
    if sidecar is None and not args.allow_unsigned:
        raise _error("the release is unsigned; sign it (wotbmod release --sign) or pass --allow-unsigned")
    if _is_url(args.to):
        token = args.token or os.environ.get("WOTBMOD_PORTAL_TOKEN")
        answer = publish_to_portal(args.to, token, record, artifact, sidecar)
        print(f"Published: {record['id']} {record['version']} -> {answer.get('url') or args.to}")
        for key in ("status", "review"):
            if key in answer:
                print(f"{key}: {answer[key]}")
        return 0
    catalog_dir = Path(args.to).expanduser()
    try:
        catalog_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        raise _error(f"cannot create the catalog directory {catalog_dir}: {exc}") from exc
    destination = publish_to_directory(catalog_dir, record, artifact, sidecar)
    print(f"Published: {record['id']} {record['version']} -> {destination}")
    print(f"Index: {Path(args.to).expanduser() / 'index.json'}")
    return 0


def command_policy(args: argparse.Namespace) -> int:
    _game_root, mods_root = _game(args)
    ini = ModsIni(mods_root / "mods.ini")
    if args.require_signature is not None:
        ini.set("policy", "require_trusted_signature", "1" if args.require_signature == "on" else "0")
        ini.save()
    print(f"require_trusted_signature={'1' if _policy_requires_signature(ini) else '0'} ({mods_root / 'mods.ini'})")
    trust_keys = _trust_root(mods_root) / "keys"
    keys = sorted(path.stem for path in trust_keys.glob("*.p256")) if trust_keys.is_dir() else []
    print(f"trusted keys: {', '.join(keys) if keys else '(none)'}")
    return 0


# --- argparse wiring --------------------------------------------------------------------

def _add_game_root(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--game-root", default=str(_cli.DEFAULT_GAME_ROOT))


def register(subparsers: Any, cli: Any) -> None:
    global _cli
    _cli = cli

    install = subparsers.add_parser(
        "install", help="verify, show hash/permissions/dependencies, then install a .wotbmod into <game>/mods")
    install.add_argument("source", help=".wotbmod file, package directory, or https:// URL of a .wotbmod")
    _add_game_root(install)
    install.add_argument("--sidecar", help="detached signature (default: <source>.sig)")
    install.add_argument("--catalog", help="catalog directory or https:// base to fetch required dependencies from")
    install.add_argument("--yes", "-y", action="store_true", help="do not ask for confirmation")
    install.add_argument("--force", action="store_true",
                         help="replace the same version with different bytes or break a dependent's range")
    install.add_argument("--allow-downgrade", action="store_true")
    install.add_argument("--require-signature", action="store_true",
                         help="refuse anything but a trusted signature (mods.ini [policy] does the same)")
    install.add_argument("--tier", type=int, default=None, help="permission tier to grant (default: requested)")
    install.add_argument("--pre", action="store_true", help="allow prerelease dependency versions from the catalog")
    install.add_argument("--no-scan", action="store_true", help="skip the static scan (shown, never trusted blindly)")
    install.set_defaults(handler=command_install)

    uninstall = subparsers.add_parser("uninstall", help="remove an installed .wotbmod (a backup is kept)")
    uninstall.add_argument("id")
    _add_game_root(uninstall)
    uninstall.add_argument("--yes", "-y", action="store_true")
    uninstall.add_argument("--force", action="store_true", help="remove even when other packages require it")
    uninstall.set_defaults(handler=command_uninstall)

    update = subparsers.add_parser("update", help="install newer versions from a catalog")
    update.add_argument("id", nargs="?")
    update.add_argument("--all", action="store_true")
    update.add_argument("--catalog", help="catalog directory or https:// base (default: the one recorded at install)")
    _add_game_root(update)
    update.add_argument("--check", action="store_true", help="only report what would change")
    update.add_argument("--yes", "-y", action="store_true")
    update.add_argument("--pre", action="store_true")
    update.add_argument("--require-signature", action="store_true")
    update.set_defaults(handler=command_update)

    rollback = subparsers.add_parser("rollback", help="restore the previous installed version from its backup")
    rollback.add_argument("id")
    _add_game_root(rollback)
    rollback.add_argument("--yes", "-y", action="store_true")
    rollback.add_argument("--require-signature", action="store_true")
    rollback.set_defaults(handler=command_rollback)

    listing = subparsers.add_parser("list", help="installed packages with enabled state, tiers, signatures, quarantine")
    _add_game_root(listing)
    listing.add_argument("--json", action="store_true")
    listing.set_defaults(handler=command_list)

    verify = subparsers.add_parser("verify", help="judge a detached signature the way the loader does")
    verify.add_argument("artifact")
    verify.add_argument("--sidecar")
    verify.add_argument("--trust-root", help="directory with keys/ (default: <game-root>/mods/trust)")
    _add_game_root(verify)
    verify.add_argument("--json", action="store_true")
    verify.set_defaults(handler=command_verify)

    keygen = subparsers.add_parser("keygen", help="create a developer P-256 signing key pair")
    keygen.add_argument("--out", required=True, help="private key file to create (64 hex characters)")
    keygen.add_argument("--key-id", required=True)
    keygen.set_defaults(handler=command_keygen)

    import_parser = subparsers.add_parser("import", help="turn an existing file-replacement mod (folder or zip mirroring Data/) into a resource-package project")
    import_parser.add_argument("source")
    import_parser.add_argument("--id", required=True, help="package id, author.mod_name")
    import_parser.add_argument("--name")
    import_parser.add_argument("--version", default="1.0.0")
    import_parser.add_argument("--developer")
    import_parser.add_argument("--description", default="")
    import_parser.add_argument("--client-build", dest="client_build", default=None)
    import_parser.add_argument("-o", "--output", help="project folder to create (default: ./<id with _>)")
    _add_game_root(import_parser)
    import_parser.set_defaults(handler=command_import)
    release = subparsers.add_parser("release", help="pack a project deterministically, sign it, write a release record")
    release.add_argument("project")
    release.add_argument("-o", "--output-dir")
    release.add_argument("--sign", action="store_true", help="sign with the Windows CNG key (sign_release_artifact.ps1)")
    release.add_argument("--sign-with-key", help="sign with a private key file from `wotbmod keygen`")
    release.add_argument("--key-id")
    release.add_argument("--key-name", help="CNG key container name (with --sign)")
    release.add_argument("--notes", help="release notes text file, copied into the record")
    release.add_argument("--suggest", action="append", help="id=range suggested (catalog-level) dependency")
    release.add_argument("--allow-scan-findings", action="store_true",
                         help="release even when the static scan found blocking problems")
    release.add_argument("--json", action="store_true")
    release.set_defaults(handler=command_release)

    scanner = subparsers.add_parser("scan", help="static scan of a package: imports, sections, Lua calls, permissions")
    scanner.add_argument("source", help="package directory or .wotbmod")
    scanner.add_argument("--json", action="store_true")
    scanner.set_defaults(handler=command_scan)

    publish = subparsers.add_parser("publish", help="put a release into a catalog directory or a portal")
    publish.add_argument("record", help="<id>-<version>.release.json from `wotbmod release`")
    publish.add_argument("--to", required=True, help="catalog directory, or https://portal/api/v1")
    publish.add_argument("--token", help="portal API token (or WOTBMOD_PORTAL_TOKEN)")
    publish.add_argument("--allow-unsigned", action="store_true")
    publish.set_defaults(handler=command_publish)

    quarantine = subparsers.add_parser("quarantine", help="show or lift the loader's crash-loop quarantine")
    _add_game_root(quarantine)
    quarantine.add_argument("--clear", metavar="ID", help="lift the quarantine of this mod id")
    quarantine.add_argument("--json", action="store_true")
    quarantine.set_defaults(handler=command_quarantine)

    info = subparsers.add_parser("info", help="ledger, settings, signature, quarantine, page link and log evidence for one mod")
    info.add_argument("id")
    _add_game_root(info)
    info.add_argument("--json", action="store_true")
    info.set_defaults(handler=command_info)

    crash = subparsers.add_parser("report-crash", help="send the loader's crash record for a mod to the portal dashboard")
    crash.add_argument("id")
    _add_game_root(crash)
    crash.add_argument("--to", help="portal API base (default: the catalogue recorded at install)")
    crash.add_argument("--version")
    crash.add_argument("--count", type=int, default=None)
    crash.add_argument("--token")
    crash.add_argument("--json", action="store_true")
    crash.set_defaults(handler=command_report_crash)

    sync = subparsers.add_parser("sync", help="apply the portal's revocations, send the loader's crash record, show its notice")
    _add_game_root(sync)
    sync.add_argument("--catalog", help="portal API base (default: every catalogue recorded in the ledger)")
    sync.add_argument("--check", action="store_true", help="only report what would change")
    sync.add_argument("--yes", "-y", action="store_true")
    sync.set_defaults(handler=command_sync)

    policy = subparsers.add_parser("policy", help="show or set the loader's signature policy in mods.ini")
    _add_game_root(policy)
    policy.add_argument("--require-signature", choices=("on", "off"), default=None)
    policy.set_defaults(handler=command_policy)
