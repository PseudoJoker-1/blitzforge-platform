"""Static scan of a package before it is published or installed (Stage 7).

    wotbmod scan <package dir | .wotbmod> [--json]

What it looks at, without running anything:

- native packages: every PE file (the entrypoint DLL and any other .dll/.exe)
  is parsed by hand - imports, sections, strings. Imports that inject into
  other processes, download or execute things, hook the keyboard, or touch
  the registry are named; a section that is writable and executable, a
  packed/encrypted section (high entropy), embedded URLs and IPs are noted;
  and what the binary imports is compared with what the manifest asks for
  (a DLL that speaks HTTP without a `network.*` permission is a warning,
  process injection without `native.*` is a blocker);
- Lua packages: every .lua file is read for calls the host never offers
  (`os.execute`, `io.popen`, `package.loadlib`, `debug.*`, `load`/`dofile`
  of strings), for `wotb.<facade>` use that the manifest's permissions do
  not cover, and for obfuscated blobs;
- content packages: only the archive rules already enforced by the reader.

The result is a report with findings of three severities: `block` (the
portal refuses the upload and `wotbmod release` refuses without
`--allow-scan-findings`), `warn` (shown to moderators and on the mod page)
and `info`. A scan is evidence, not a verdict: it cannot prove a package
harmless, it can only show what is plainly there.

Stdlib only.
"""

from __future__ import annotations

import json
import math
import re
import struct
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCAN_SCHEMA = 1

# Imported functions that deserve a name in the report. Severity when the
# manifest does not hold a permission that explains them.
NATIVE_IMPORTS: dict[str, tuple[str, str, str]] = {
    # name: (severity, code, explanation)
    "CreateRemoteThread": ("block", "inject", "creates threads in other processes"),
    "CreateRemoteThreadEx": ("block", "inject", "creates threads in other processes"),
    "WriteProcessMemory": ("block", "inject", "writes another process's memory"),
    "VirtualAllocEx": ("block", "inject", "allocates memory in another process"),
    "NtCreateThreadEx": ("block", "inject", "creates threads in other processes"),
    "SetWindowsHookExA": ("block", "hook", "installs a system-wide hook (keylogger pattern)"),
    "SetWindowsHookExW": ("block", "hook", "installs a system-wide hook (keylogger pattern)"),
    "URLDownloadToFileA": ("block", "download", "downloads and writes files"),
    "URLDownloadToFileW": ("block", "download", "downloads and writes files"),
    "ShellExecuteA": ("warn", "execute", "launches programs"),
    "ShellExecuteW": ("warn", "execute", "launches programs"),
    "ShellExecuteExA": ("warn", "execute", "launches programs"),
    "ShellExecuteExW": ("warn", "execute", "launches programs"),
    "CreateProcessA": ("warn", "execute", "launches programs"),
    "CreateProcessW": ("warn", "execute", "launches programs"),
    "WinExec": ("warn", "execute", "launches programs"),
    "system": ("warn", "execute", "runs shell commands"),
    "WinHttpOpen": ("warn", "network", "speaks HTTP"),
    "WinHttpConnect": ("warn", "network", "speaks HTTP"),
    "InternetOpenA": ("warn", "network", "speaks HTTP"),
    "InternetOpenW": ("warn", "network", "speaks HTTP"),
    "InternetOpenUrlA": ("warn", "network", "speaks HTTP"),
    "InternetOpenUrlW": ("warn", "network", "speaks HTTP"),
    "HttpOpenRequestA": ("warn", "network", "speaks HTTP"),
    "HttpOpenRequestW": ("warn", "network", "speaks HTTP"),
    "WSAStartup": ("warn", "network", "opens sockets"),
    "socket": ("warn", "network", "opens sockets"),
    "connect": ("warn", "network", "opens sockets"),
    "RegSetValueExA": ("warn", "registry", "writes the registry"),
    "RegSetValueExW": ("warn", "registry", "writes the registry"),
    "RegCreateKeyExA": ("warn", "registry", "writes the registry"),
    "RegCreateKeyExW": ("warn", "registry", "writes the registry"),
    "CryptEncrypt": ("warn", "crypto", "encrypts data (ransomware pattern when combined with file writes)"),
    "GetAsyncKeyState": ("info", "input", "polls keys system-wide"),
    "OpenProcess": ("warn", "process", "opens other processes"),
    "AdjustTokenPrivileges": ("warn", "privilege", "changes process privileges"),
    "IsDebuggerPresent": ("info", "antidebug", "checks for a debugger"),
    "LoadLibraryA": ("info", "loadlibrary", "loads DLLs at run time"),
    "LoadLibraryW": ("info", "loadlibrary", "loads DLLs at run time"),
    "LoadLibraryExA": ("info", "loadlibrary", "loads DLLs at run time"),
    "LoadLibraryExW": ("info", "loadlibrary", "loads DLLs at run time"),
}
# Which manifest permission families explain which import codes.
EXPLAINED_BY: dict[str, tuple[str, ...]] = {
    "network": ("network:",),
    "inject": ("native.memory_patch", "native.hook.address", "native.hooks"),
    "hook": ("native.hooks", "input.actions"),
    "process": ("native.",),
    "loadlibrary": ("native.",),
}

LUA_BLOCKERS = {
    r"\bos\s*\.\s*execute\b": ("block", "lua.os", "os.execute runs shell commands"),
    r"\bio\s*\.\s*popen\b": ("block", "lua.io", "io.popen runs shell commands"),
    r"\bpackage\s*\.\s*loadlib\b": ("block", "lua.loadlib", "package.loadlib loads native code"),
    r"\brequire\s*\(?\s*['\"]ffi['\"]": ("block", "lua.ffi", "FFI is not part of the host"),
    r"\bdebug\s*\.\s*(sethook|setupvalue|setlocal|setmetatable|getregistry)\b": ("block", "lua.debug", "debug library escapes the sandbox"),
}
LUA_WARNINGS = {
    r"\bos\s*\.\s*(remove|rename|tmpname|getenv)\b": ("warn", "lua.os", "os file/environment access"),
    r"\bio\s*\.\s*(open|lines|output|write)\b": ("warn", "lua.io", "direct file access outside wotb.files"),
    r"\b(loadstring|load|dofile|loadfile)\s*\(": ("warn", "lua.load", "loads code at run time"),
    r"\bsetfenv\b|\b_ENV\b": ("warn", "lua.env", "rewrites the environment"),
    r"\bstring\s*\.\s*dump\b": ("warn", "lua.dump", "serialises functions"),
}
# wotb facade / raw table -> permissions that cover it (any one is enough).
LUA_MODULE_PERMISSIONS: dict[str, tuple[str, ...]] = {
    "ui": ("ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"),
    "ui_read": ("ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"),
    "screen": ("ui.modify.game", "ui.create", "ui.modify.own", "battle.ui"),
    "panel": ("ui.create", "ui.modify.own"),
    "hud": ("gameplay.tweak.hud",),
    "gameplay_hud": ("gameplay.tweak.hud",),
    "keys": ("input.actions",),
    "input": ("input.actions",),
    "files": ("resources.mod", "resources.overlay.game"),
    "resources": ("resources.mod",),
    "vfs": ("resources.mod", "resources.overlay.game"),
    "loaders": ("resources.mod",),
    "vehicle": ("gameplay.tweak.vehicle", "vehicle.local.cosmetic"),
    "vehicle_visual": ("gameplay.tweak.vehicle", "vehicle.local.cosmetic"),
    "sound": ("audio.custom", "audio.events"),
    "audio": ("audio.custom", "audio.events"),
    "view": ("camera.battle.read", "camera.hangar", "camera.replay", "gameplay.tweak.camera"),
    "camera": ("camera.battle.read", "camera.hangar", "camera.replay", "gameplay.tweak.camera"),
    "gameplay_camera": ("gameplay.tweak.camera", "gameplay.tweak.freecam"),
    "shells": ("gameplay.tweak.projectile_visual", "visible.projectile.events"),
    "projectile": ("gameplay.tweak.projectile_visual", "visible.projectile.events"),
    "store": ("storage",),
    "storage": ("storage",),
    "ges": ("ges.observe",),
    "players": ("events.public", "entity.public.visible", "game.entity.public"),
    "battle": ("events.public",),
    "events": ("events.public",),
    "dava": ("native.hooks", "native.hook.address", "native.memory_patch", "render.native"),
}

URL_RE = re.compile(rb"https?://[A-Za-z0-9._~:/?#\[\]@!$&'()*+,;=%-]{4,200}")
IP_RE = re.compile(rb"\b(?:\d{1,3}\.){3}\d{1,3}(?::\d{2,5})?\b")
BLOB_RE = re.compile(r"[A-Za-z0-9+/=]{240,}")
ESCAPE_RE = re.compile(r"(?:\\x[0-9A-Fa-f]{2}|\\\d{1,3}){24,}")


@dataclass
class Finding:
    severity: str
    code: str
    message: str
    where: str

    def to_dict(self) -> dict[str, str]:
        return {"severity": self.severity, "code": self.code, "message": self.message, "where": self.where}


@dataclass
class ScanReport:
    target: str
    kind: str
    findings: list[Finding] = field(default_factory=list)
    files_scanned: int = 0

    def add(self, severity: str, code: str, message: str, where: str) -> None:
        self.findings.append(Finding(severity, code, message, where))

    @property
    def counts(self) -> dict[str, int]:
        result = {"block": 0, "warn": 0, "info": 0}
        for finding in self.findings:
            result[finding.severity] = result.get(finding.severity, 0) + 1
        return result

    @property
    def risk(self) -> str:
        counts = self.counts
        if counts["block"]:
            return "high"
        if counts["warn"]:
            return "medium"
        return "low"

    @property
    def blocked(self) -> bool:
        return self.counts["block"] > 0

    def to_dict(self) -> dict[str, Any]:
        return {"schema": SCAN_SCHEMA, "target": self.target, "kind": self.kind, "risk": self.risk,
                "summary": self.counts, "files_scanned": self.files_scanned,
                "findings": [finding.to_dict() for finding in self.findings]}


# --- PE parsing -------------------------------------------------------------------

def _entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = [0] * 256
    for byte in data:
        counts[byte] += 1
    total = len(data)
    return -sum((c / total) * math.log2(c / total) for c in counts if c)


def parse_pe(data: bytes) -> dict[str, Any] | None:
    """Sections and imports of a PE image, or None when `data` is not one."""
    if len(data) < 0x40 or data[:2] != b"MZ":
        return None
    (pe_offset,) = struct.unpack_from("<I", data, 0x3C)
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        return None
    machine, section_count, _stamp, _symtab, _symcount, optional_size, _characteristics = struct.unpack_from("<HHIIIHH", data, pe_offset + 4)
    optional = pe_offset + 24
    if optional + 2 > len(data):
        return None
    (magic,) = struct.unpack_from("<H", data, optional)
    if magic == 0x10B:
        directories_at = optional + 96
        bits = 32
    elif magic == 0x20B:
        directories_at = optional + 112
        bits = 64
    else:
        return None
    (directory_count,) = struct.unpack_from("<I", data, directories_at - 4)
    imports_rva = imports_size = 0
    if directory_count > 1 and directories_at + 16 <= len(data):
        imports_rva, imports_size = struct.unpack_from("<II", data, directories_at + 8)
    sections = []
    section_table = optional + optional_size
    for index in range(section_count):
        at = section_table + index * 40
        if at + 40 > len(data):
            break
        name = data[at:at + 8].rstrip(b"\0").decode("ascii", errors="replace")
        virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, at + 8)
        (flags,) = struct.unpack_from("<I", data, at + 36)
        raw = data[raw_pointer:raw_pointer + raw_size] if raw_pointer + raw_size <= len(data) else b""
        sections.append({"name": name, "virtual_address": virtual_address, "virtual_size": virtual_size,
                         "raw_pointer": raw_pointer, "raw_size": raw_size, "flags": flags,
                         "entropy": round(_entropy(raw), 2) if raw else 0.0,
                         "writable": bool(flags & 0x80000000), "executable": bool(flags & 0x20000000)})

    def rva_to_offset(rva: int) -> int | None:
        for section in sections:
            start = section["virtual_address"]
            size = max(section["virtual_size"], section["raw_size"])
            if start <= rva < start + size:
                return section["raw_pointer"] + (rva - start)
        return None

    def read_string(rva: int) -> str:
        offset = rva_to_offset(rva)
        if offset is None or offset >= len(data):
            return ""
        end = data.find(b"\0", offset, offset + 512)
        return data[offset:end if end >= 0 else offset + 512].decode("ascii", errors="replace")

    imports: dict[str, list[str]] = {}
    descriptor = rva_to_offset(imports_rva) if imports_rva else None
    guard = 0
    while descriptor is not None and descriptor + 20 <= len(data) and guard < 512:
        guard += 1
        original_first_thunk, _stamp, _forwarder, name_rva, first_thunk = struct.unpack_from("<IIIII", data, descriptor)
        if name_rva == 0 and first_thunk == 0:
            break
        dll = read_string(name_rva).lower()
        names: list[str] = []
        thunk_rva = original_first_thunk or first_thunk
        thunk = rva_to_offset(thunk_rva)
        entry_size = 8 if bits == 64 else 4
        count = 0
        while thunk is not None and thunk + entry_size <= len(data) and count < 4096:
            count += 1
            value = struct.unpack_from("<Q" if bits == 64 else "<I", data, thunk)[0]
            if value == 0:
                break
            ordinal_flag = 1 << (63 if bits == 64 else 31)
            if value & ordinal_flag:
                names.append(f"#{value & 0xFFFF}")
            else:
                names.append(read_string((value & 0x7FFFFFFF) + 2))
            thunk += entry_size
        imports[dll] = names
        descriptor += 20
    return {"bits": bits, "machine": machine, "sections": sections, "imports": imports}


def scan_pe(report: ScanReport, where: str, data: bytes, permissions: set[str]) -> None:
    image = parse_pe(data)
    if image is None:
        report.add("warn", "pe.invalid", "file has a .dll/.exe name but is not a PE image", where)
        return
    if image["bits"] != 32:
        report.add("block", "pe.bits", f"{image['bits']}-bit image in a 32-bit client", where)
    for section in image["sections"]:
        if section["writable"] and section["executable"]:
            report.add("warn", "pe.wx", f"section {section['name']} is writable and executable", where)
        if section["raw_size"] > 4096 and section["entropy"] >= 7.2:
            report.add("warn", "pe.packed", f"section {section['name']} looks packed or encrypted (entropy {section['entropy']})", where)
    seen_codes: set[str] = set()
    for dll, names in image["imports"].items():
        for name in names:
            rule = NATIVE_IMPORTS.get(name)
            if rule is None:
                continue
            severity, code, explanation = rule
            explained = any(any(p.startswith(prefix) for p in permissions) for prefix in EXPLAINED_BY.get(code, ()))
            if explained and severity != "info":
                severity = "info"
                explanation += " (covered by the manifest's permissions)"
            key = f"{code}:{name}"
            if key in seen_codes:
                continue
            seen_codes.add(key)
            report.add(severity, f"pe.import.{code}", f"imports {dll}!{name}: {explanation}", where)
    for match in URL_RE.finditer(data):
        text = match.group(0).decode("ascii", errors="replace")
        if not any(p.startswith("network:") for p in permissions):
            report.add("warn", "pe.url", f"embedded URL {text[:80]} without a network permission", where)
        else:
            report.add("info", "pe.url", f"embedded URL {text[:80]}", where)
    ips = {match.group(0).decode("ascii", errors="replace") for match in IP_RE.finditer(data)}
    ips = {ip for ip in ips if not ip.startswith(("0.", "127.", "255.")) and not re.fullmatch(r"(\d+\.){3}\d+", ip) is None}
    for ip in sorted(ips)[:5]:
        report.add("info", "pe.ip", f"embedded address {ip}", where)


# --- Lua ------------------------------------------------------------------------------

def scan_lua(report: ScanReport, where: str, text: str, permissions: set[str]) -> None:
    stripped = re.sub(r"--\[\[.*?\]\]", "", text, flags=re.S)
    stripped = re.sub(r"--[^\n]*", "", stripped)
    for pattern, (severity, code, message) in LUA_BLOCKERS.items():
        if re.search(pattern, stripped):
            report.add(severity, code, message, where)
    for pattern, (severity, code, message) in LUA_WARNINGS.items():
        if re.search(pattern, stripped):
            report.add(severity, code, message, where)
    used = {match.group(1) for match in re.finditer(r"\bwotb\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)", stripped)}
    for module in sorted(used):
        needed = LUA_MODULE_PERMISSIONS.get(module)
        if needed and not any(p in permissions for p in needed):
            report.add("warn", "lua.permission", f"uses wotb.{module} but the manifest holds none of: {', '.join(needed)}", where)
    if BLOB_RE.search(stripped):
        report.add("warn", "lua.blob", "long base64-like blob (obfuscated payload?)", where)
    if ESCAPE_RE.search(stripped):
        report.add("warn", "lua.escapes", "long run of byte escapes (obfuscated string?)", where)
    if len(text) > 512 * 1024:
        report.add("info", "lua.size", f"large source file ({len(text) // 1024} KiB)", where)


# --- packages -------------------------------------------------------------------------

def _iter_files(source: Path):
    """(relative path, bytes) for a package directory or a store archive."""
    if source.is_dir():
        for path in sorted(source.rglob("*")):
            if path.is_file() and not path.is_symlink():
                yield path.relative_to(source).as_posix(), path.read_bytes()
        return
    with zipfile.ZipFile(source, "r") as archive:
        for info in sorted(archive.infolist(), key=lambda item: item.filename):
            if info.is_dir() or ".." in info.filename or info.filename.startswith(("/", "\\")):
                continue
            yield info.filename, archive.read(info)


def scan_package(source: Path, manifest: dict[str, Any] | None = None) -> ScanReport:
    """Scan a package directory or .wotbmod; `manifest` is the validated manifest when known."""
    files = list(_iter_files(source))
    by_name = {name.casefold(): data for name, data in files}
    if manifest is None and "manifest.json" in by_name:
        try:
            manifest = json.loads(by_name["manifest.json"].decode("utf-8"))
        except ValueError:
            manifest = {}
    manifest = manifest or {}
    permissions = set(manifest.get("permissions", []) or [])
    kind = manifest.get("type") or ("lua" if "manifest_version" not in manifest else "native")
    report = ScanReport(str(source), kind)
    for name, data in files:
        lower = name.lower()
        if lower.endswith((".dll", ".exe", ".sys")) or data[:2] == b"MZ":
            scan_pe(report, name, data, permissions)
            if kind != "native":
                report.add("block", "pe.unexpected", f"a {kind} package must not carry native code", name)
        elif lower.endswith(".lua"):
            scan_lua(report, name, data.decode("utf-8", errors="replace"), permissions)
        elif lower.endswith((".bat", ".cmd", ".ps1", ".vbs", ".js", ".py", ".sh", ".scr", ".com", ".msi", ".lnk")):
            report.add("block", "file.script", "scripts and installers have no place in a mod package", name)
        report.files_scanned += 1
    if kind == "native":
        entrypoint = (manifest.get("entrypoints") or {}).get("windows-x86", "")
        if entrypoint and entrypoint.casefold() not in by_name:
            report.add("warn", "manifest.entrypoint", f"manifest entrypoint {entrypoint} is not in the package", "manifest.json")
    if kind == "resource":
        for item in manifest.get("files") or []:
            if isinstance(item, dict):
                report.add("info", "game.file", f"replaces the game file Data/{item.get('target')}", str(item.get("source")))
    return report


RISK_LABELS = {
    "SAFE": "безопасно: только собственные данные мода",
    "GAMEPLAY_TWEAK": "меняет HUD, камеру или вид техники на этом клиенте",
    "REVIEWED": "нужна проверка: сеть, ресурсы игры или чужие контролы",
    "UNSAFE": "полный доступ к процессу игры",
}


def render_text(report: ScanReport) -> str:
    lines = [f"scan: {report.target} ({report.kind}) risk={report.risk} "
             f"block={report.counts['block']} warn={report.counts['warn']} info={report.counts['info']} files={report.files_scanned}"]
    for finding in report.findings:
        lines.append(f"  [{finding.severity}] {finding.code}: {finding.message} ({finding.where})")
    if not report.findings:
        lines.append("  no findings")
    return "\n".join(lines)
