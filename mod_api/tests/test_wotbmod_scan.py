"""Stage 7: the static scanner and the places it gates.

- a hand-built 32-bit PE with a chosen import table drives the PE rules
  (process injection blocks, HTTP without a network permission warns, a
  writable+executable section warns, a 64-bit image blocks);
- Lua sources: `os.execute` blocks, a facade used without its permission
  warns, a clean script is low risk;
- `wotbmod scan` exit code, `wotbmod release` refusing blocking findings,
  the scan summary in the release record, and `wotbmod install` printing a
  PERMISSION ESCALATION line when an update asks for more than the
  installed version had.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SDK_ROOT = Path(__file__).resolve().parents[1]
TOOLS = SDK_ROOT / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
SPEC = importlib.util.spec_from_file_location("wotbmod_cli", TOOLS / "wotbmod.py")
assert SPEC is not None and SPEC.loader is not None
wotbmod = importlib.util.module_from_spec(SPEC)
sys.modules["wotbmod_cli"] = wotbmod
SPEC.loader.exec_module(wotbmod)
wotbmod.build_parser()
packages = sys.modules["wotbmod_packages"]
scan = sys.modules["wotbmod_scan"]


from pe_fixture import build_pe  # noqa: E402

def cli(argv: list[str]) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = wotbmod.main(argv)
    return code, out.getvalue(), err.getvalue()


def native_project(root: Path, mod_id: str, version: str, dll: bytes, *, permissions: list[str] | None = None) -> Path:
    manifest = {
        "manifest_version": 1, "type": "native", "id": mod_id, "name": f"Scan {mod_id}", "version": version,
        "developer": "Scan Tests", "api": {"wotbmod.core": ">=1 <2"},
        "client": {"builds": [wotbmod.EXPECTED_CLIENT_BUILD], "executable_hashes": [wotbmod.EXPECTED_CLIENT_SHA256]},
        "entrypoints": {"windows-x86": "bin/windows-x86/mod.dll"}, "permissions": permissions or ["core"],
        "resources": ["assets/**"],
    }
    (root / "bin" / "windows-x86").mkdir(parents=True, exist_ok=True)
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True), encoding="utf-8")
    (root / "bin" / "windows-x86" / "mod.dll").write_bytes(dll)
    (root / "assets").mkdir(exist_ok=True)
    (root / "assets" / "a.txt").write_bytes(b"a\n")
    return root


class ScannerTests(unittest.TestCase):
    def test_pe_parser_and_rules(self) -> None:
        clean = build_pe({"kernel32.dll": ["GetTickCount", "LoadLibraryA"]})
        image = scan.parse_pe(clean)
        assert image is not None
        self.assertEqual(image["imports"]["kernel32.dll"], ["GetTickCount", "LoadLibraryA"])
        self.assertEqual(image["bits"], 32)
        report = scan.ScanReport("x", "native")
        scan.scan_pe(report, "mod.dll", clean, {"core"})
        self.assertEqual(report.counts["block"], 0)
        self.assertEqual(report.counts["warn"], 0)
        self.assertTrue(any(f.code == "pe.import.loadlibrary" for f in report.findings))

        injector = build_pe({"kernel32.dll": ["WriteProcessMemory", "CreateRemoteThread"], "winhttp.dll": ["WinHttpOpen"]},
                            extra=b"https://evil.example/collect\0")
        report = scan.ScanReport("x", "native")
        scan.scan_pe(report, "mod.dll", injector, {"core"})
        codes = {(f.severity, f.code) for f in report.findings}
        self.assertIn(("block", "pe.import.inject"), codes)
        self.assertIn(("warn", "pe.import.network"), codes)
        self.assertIn(("warn", "pe.url"), codes)
        self.assertEqual(report.risk, "high")
        # The manifest explains the network use: the same import is only informational.
        report = scan.ScanReport("x", "native")
        scan.scan_pe(report, "mod.dll", injector, {"core", "network:https://api.example"})
        self.assertIn(("info", "pe.import.network"), {(f.severity, f.code) for f in report.findings})
        self.assertIn(("info", "pe.url"), {(f.severity, f.code) for f in report.findings})

        report = scan.ScanReport("x", "native")
        scan.scan_pe(report, "mod.dll", build_pe({"kernel32.dll": ["Sleep"]}, wx=True), set())
        self.assertIn("pe.wx", {f.code for f in report.findings})
        report = scan.ScanReport("x", "native")
        scan.scan_pe(report, "mod.dll", build_pe({"kernel32.dll": ["Sleep"]}, bits=64), set())
        self.assertIn(("block", "pe.bits"), {(f.severity, f.code) for f in report.findings})
        report = scan.ScanReport("x", "native")
        scan.scan_pe(report, "mod.dll", b"MZ not really a PE", set())
        self.assertIn("pe.invalid", {f.code for f in report.findings})

    def test_lua_rules(self) -> None:
        report = scan.ScanReport("x", "lua")
        scan.scan_lua(report, "main.lua", 'local h = wotb.hud\nos.execute("calc")\n-- os.execute in a comment does not count twice\n', {"core"})
        codes = {(f.severity, f.code) for f in report.findings}
        self.assertIn(("block", "lua.os"), codes)
        self.assertIn(("warn", "lua.permission"), codes)
        report = scan.ScanReport("x", "lua")
        scan.scan_lua(report, "main.lua", 'wotb.log.info("hi")\nwotb.battle.on_shot(function() end)\n', {"core", "events.public"})
        self.assertEqual(report.findings, [])
        report = scan.ScanReport("x", "lua")
        scan.scan_lua(report, "main.lua", 'local blob = "' + "QUJD" * 80 + '"\n', {"core"})
        self.assertIn("lua.blob", {f.code for f in report.findings})

    def test_scan_release_and_install_gates(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_scan_") as temporary:
            root = Path(temporary)
            bad = native_project(root / "bad", "scan.bad", "1.0.0", build_pe({"kernel32.dll": ["CreateRemoteThread"]}))
            code, out, err = cli(["scan", str(bad)])
            self.assertEqual(code, 3, out)
            self.assertIn("[block] pe.import.inject", out)
            code, out, err = cli(["scan", str(bad), "--json"])
            self.assertEqual(json.loads(out)["risk"], "high")
            code, out, err = cli(["release", str(bad), "-o", str(root / "rel")])
            self.assertEqual(code, 2)
            self.assertIn("static scan", err)
            self.assertFalse((root / "rel" / "scan.bad-1.0.0.wotbmod").exists())
            code, out, err = cli(["release", str(bad), "-o", str(root / "rel"), "--allow-scan-findings", "--json"])
            self.assertEqual(code, 0, err)
            self.assertEqual(json.loads(out)["scan"]["risk"], "high")

            good = native_project(root / "good", "scan.good", "1.0.0", build_pe({"kernel32.dll": ["Sleep"]}))
            code, out, err = cli(["scan", str(good)])
            self.assertEqual(code, 0, out)
            self.assertIn("risk=low", out)
            code, out, err = cli(["release", str(good), "-o", str(root / "rel"), "--json"])
            self.assertEqual(code, 0, err)
            self.assertEqual(json.loads(out)["scan"]["summary"]["block"], 0)

            lua = root / "lua"
            lua.mkdir()
            (lua / "manifest.json").write_text(json.dumps({"id": "scan.lua", "name": "L", "version": "1.0.0",
                                                            "entrypoint": "main.lua", "permissions": ["core"]}), encoding="utf-8")
            (lua / "main.lua").write_text('io.popen("dir")\n', encoding="utf-8")
            code, out, err = cli(["scan", str(lua)])
            self.assertEqual(code, 3)
            self.assertIn("lua.io", out)

            # Escalation: the update asks for a permission the installed version never had.
            game = root / "game"
            game.mkdir()
            (game / "wotblitz.exe").write_bytes(b"MZ fake\n")
            (game / "mods").mkdir()
            executable = game / "wotblitz.exe"
            real = wotbmod._sha256_file

            def sha256_file(path: Path) -> str:
                return wotbmod.EXPECTED_CLIENT_SHA256 if Path(path) == executable else real(Path(path))

            with (mock.patch.object(wotbmod, "_pe_architecture", return_value=wotbmod.EXPECTED_CLIENT_ARCH),
                  mock.patch.object(wotbmod, "_windows_file_version", return_value=wotbmod.EXPECTED_CLIENT_BUILD),
                  mock.patch.object(wotbmod, "_sha256_file", side_effect=sha256_file)):
                code, out, err = cli(["install", str(root / "rel" / "scan.good-1.0.0.wotbmod"), "--game-root", str(game), "--yes"])
                self.assertEqual(code, 0, err)
                self.assertIn("SAFE: безопасно", out)
                more = native_project(root / "more", "scan.good", "1.1.0", build_pe({"winhttp.dll": ["WinHttpOpen"]}),
                                      permissions=["core", "network:https://stats.example"])
                code, out, err = cli(["release", str(more), "-o", str(root / "rel")])
                self.assertEqual(code, 0, err)
                code, out, err = cli(["install", str(root / "rel" / "scan.good-1.1.0.wotbmod"), "--game-root", str(game), "--yes"])
                self.assertEqual(code, 0, err)
                self.assertIn("PERMISSION ESCALATION: scan.good 1.0.0 -> 1.1.0 asks for network:https://stats.example", out)
                self.assertIn("tier SAFE -> REVIEWED", out)
                code, out, err = cli(["install", str(root / "rel" / "scan.bad-1.0.0.wotbmod"), "--game-root", str(game), "--yes"])
                self.assertEqual(code, 2)
                self.assertIn("static scan found blocking problems", err)
                self.assertFalse((game / "mods" / "scan.bad.wotbmod").exists())


if __name__ == "__main__":
    unittest.main()
