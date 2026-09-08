"""Stage 5 of the platform roadmap: the wotbmod:// browser-to-client bridge,
and Lua packages travelling through the same release/install path.

Acceptance, in the roadmap's words:

- Install in the browser starts the launcher with an immutable release
  (`wotbmod://install/<id>@<version>?source=<catalogue>`), never a DLL or a
  local path;
- a package is not installed on a wrong hash, a bad signature or a client
  mismatch;
- pressing the button twice is idempotent;
- uninstall touches no other mod id's files;
- installation works for native and Lua packages (content-only shares the
  native path).

Stdlib only: a loopback http.server serves a catalogue directory written by
`wotbmod publish --to <dir>`; Windows registration is exercised with a
throw-away scheme name under HKCU and removed again.
"""

from __future__ import annotations

import contextlib
import functools
import http.server
import importlib.util
import io
import json
import os
import secrets
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

SDK_ROOT = Path(__file__).resolve().parents[1]
TOOLS = SDK_ROOT / "tools"
sys.path.insert(0, str(TOOLS))
SPEC = importlib.util.spec_from_file_location("wotbmod_cli", TOOLS / "wotbmod.py")
assert SPEC is not None and SPEC.loader is not None
wotbmod = importlib.util.module_from_spec(SPEC)
sys.modules["wotbmod_cli"] = wotbmod
SPEC.loader.exec_module(wotbmod)
wotbmod.build_parser()
packages = sys.modules["wotbmod_packages"]
trust = sys.modules["wotbmod_trust"]
import wotbmod_launcher as launcher  # noqa: E402

KEY = trust.generate_private_key()
PUBLIC = trust.public_key_to_hex(*trust.public_key_of(KEY))


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def native_project(root: Path, mod_id: str, version: str, *, dependencies: dict[str, str] | None = None,
                   client_builds: list[str] | None = None) -> Path:
    manifest = {
        "manifest_version": 1, "type": "native", "id": mod_id, "name": f"Launch {mod_id}", "version": version,
        "developer": "Launcher Tests", "api": {"wotbmod.core": ">=1 <2"},
        "client": {"builds": client_builds or [wotbmod.EXPECTED_CLIENT_BUILD],
                   "executable_hashes": [wotbmod.EXPECTED_CLIENT_SHA256]},
        "entrypoints": {"windows-x86": "bin/windows-x86/mod.dll"}, "permissions": ["core"], "resources": ["assets/**"],
    }
    if dependencies:
        manifest["dependencies"] = dependencies
    write_json(root / "manifest.json", manifest)
    (root / "bin" / "windows-x86").mkdir(parents=True, exist_ok=True)
    (root / "bin" / "windows-x86" / "mod.dll").write_bytes(f"MZ {mod_id} {version}\n".encode())
    (root / "assets").mkdir(exist_ok=True)
    (root / "assets" / "a.txt").write_bytes(b"asset\n")
    return root


def lua_project(root: Path, mod_id: str, version: str) -> Path:
    write_json(root / "manifest.json", {"id": mod_id, "name": f"Lua {mod_id}", "version": version,
                                        "entrypoint": "main.lua", "permissions": ["core", "storage"],
                                        "developer": "Launcher Tests"})
    (root / "main.lua").write_text('wotb.log.info("hello from %s")\n' % mod_id, encoding="utf-8")
    (root / "lib").mkdir(exist_ok=True)
    (root / "lib" / "util.lua").write_text("return {}\n", encoding="utf-8")
    return root


def cli(argv: list[str], game: Path | None = None) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with (fingerprint(game) if game else contextlib.nullcontext()), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = wotbmod.main(argv)
    return code, out.getvalue(), err.getvalue()


def launch(argv: list[str], game: Path) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with fingerprint(game), contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = launcher.main(argv)
    return code, out.getvalue(), err.getvalue()


@contextlib.contextmanager
def fingerprint(game: Path):
    executable = game / "wotblitz.exe"
    real = wotbmod._sha256_file

    def sha256_file(path: Path) -> str:
        return wotbmod.EXPECTED_CLIENT_SHA256 if Path(path) == executable else real(Path(path))

    with (mock.patch.object(wotbmod, "_pe_architecture", return_value=wotbmod.EXPECTED_CLIENT_ARCH),
          mock.patch.object(wotbmod, "_windows_file_version", return_value=wotbmod.EXPECTED_CLIENT_BUILD),
          mock.patch.object(wotbmod, "_sha256_file", side_effect=sha256_file)):
        yield


class LauncherTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="wotbmod_launch_")
        self.root = Path(self._temporary.name)
        self.env = mock.patch.dict(os.environ, {"WOTBMOD_LAUNCHER_HOME": str(self.root / "home")})
        self.env.start()
        (self.root / "dev.key").write_text(f"{KEY:064x}\n", encoding="utf-8")
        self.game = self.root / "game"
        self.game.mkdir()
        (self.game / "wotblitz.exe").write_bytes(b"MZ fake\n")
        keys = self.game / "mods" / "trust" / "keys"
        keys.mkdir(parents=True)
        (keys / "launch-key.p256").write_text(PUBLIC + "\n", encoding="utf-8")
        self.catalog = self.root / "catalog"
        self.catalog.mkdir()
        for project in (native_project(self.root / "src" / "dep", "launch.dep", "1.0.0"),
                        native_project(self.root / "src" / "main", "launch.main", "1.0.0", dependencies={"launch.dep": "^1.0.0"}),
                        native_project(self.root / "src" / "other", "launch.other", "1.0.0"),
                        lua_project(self.root / "src" / "lua", "launch.lua", "1.0.0")):
            self.publish(project)
        # launch.other is declared for another client build in the catalogue record.
        index_path = self.catalog / "index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        index["packages"]["launch.other"]["versions"]["1.0.0"]["client"]["builds"] = ["1.0.0.1"]
        index_path.write_text(json.dumps(index), encoding="utf-8")
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(self.catalog))
        handler.log_message = lambda *args, **kwargs: None  # type: ignore[attr-defined]
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.source = f"http://127.0.0.1:{self.server.server_address[1]}/"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.env.stop()
        self._temporary.cleanup()

    def publish(self, project: Path) -> None:
        manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
        argv = ["release", str(project), "-o", str(self.root / "rel"), "--sign-with-key", str(self.root / "dev.key"),
                "--key-id", "launch-key"]
        code, out, err = cli(argv)
        self.assertEqual(code, 0, err)
        record = self.root / "rel" / f"{manifest['id']}-{manifest['version']}.release.json"
        code, out, err = cli(["publish", str(record), "--to", str(self.catalog)])
        self.assertEqual(code, 0, err)

    def link(self, mod_id: str, version: str, source: str | None = None) -> str:
        return f"wotbmod://install/{mod_id}@{version}?source={source or self.source}"

    def ledger(self) -> dict:
        return json.loads((self.game / "mods" / "cache" / packages.LEDGER_NAME).read_text(encoding="utf-8"))["packages"]

    def test_links_are_parsed_strictly(self) -> None:
        good = launcher.parse_link("wotbmod://install/author.mod@1.2.3?source=https://portal.example/api/v1")
        self.assertEqual(good, {"action": "install", "id": "author.mod", "version": "1.2.3",
                                "source": "https://portal.example/api/v1"})
        self.assertEqual(launcher.parse_link("wotbmod://uninstall/author.mod"), {"action": "uninstall", "id": "author.mod"})
        for bad in (
            "https://portal.example/install/author.mod@1.2.3",
            "wotbmod://install/author.mod?source=https://portal.example/",
            "wotbmod://install/author.mod@notaversion?source=https://portal.example/",
            "wotbmod://install/../mod@1.0.0?source=https://portal.example/",
            "wotbmod://install/author.mod@1.0.0?source=C:/Games/mods/evil.dll",
            "wotbmod://install/author.mod@1.0.0?source=file:///C:/evil",
            "wotbmod://install/author.mod@1.0.0?source=https://portal.example/&dll=x",
            "wotbmod://install/author.mod@1.0.0",
            "wotbmod://run/author.mod@1.0.0?source=https://portal.example/",
            "wotbmod://install/author.mod@1.0.0?source=https://portal.example/#frag",
        ):
            with self.assertRaises(launcher.LaunchError, msg=bad):
                launcher.parse_link(bad)

    def test_open_installs_the_named_release_with_its_dependency_and_is_idempotent(self) -> None:
        code, out, err = launch(["open", self.link("launch.main", "1.0.0"), "--yes", "--no-pause", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 0, err)
        self.assertLess(out.index("Installed: launch.dep 1.0.0"), out.index("Installed: launch.main 1.0.0"))
        self.assertIn("Signature: valid (key launch-key)", out)
        self.assertIn("SHA-256:", out)
        target = self.game / "mods" / "launch.main.wotbmod"
        installed_bytes = target.read_bytes()
        self.assertTrue((self.game / "mods" / "launch.dep.wotbmod.sig").is_file())
        code, out, err = launch(["open", self.link("launch.main", "1.0.0"), "--yes", "--no-pause", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("already installed with the same hash", out)
        self.assertEqual(target.read_bytes(), installed_bytes)
        self.assertEqual(len(self.ledger()["launch.main"]["history"]), 0)
        log = (self.root / "home" / "WotbMod" / "launcher.log").read_text(encoding="utf-8")
        self.assertIn("result 0", log)

    def test_open_refuses_wrong_hash_missing_version_and_other_client(self) -> None:
        index_path = self.catalog / "index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        original = index["packages"]["launch.main"]["versions"]["1.0.0"]["artifact"]["sha256"]
        index["packages"]["launch.main"]["versions"]["1.0.0"]["artifact"]["sha256"] = "0" * 64
        index_path.write_text(json.dumps(index), encoding="utf-8")
        code, out, err = launch(["open", self.link("launch.main", "1.0.0"), "--yes", "--no-pause", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 2)
        self.assertIn("hash mismatch", err)
        self.assertFalse((self.game / "mods" / "launch.main.wotbmod").exists())
        index["packages"]["launch.main"]["versions"]["1.0.0"]["artifact"]["sha256"] = original
        index_path.write_text(json.dumps(index), encoding="utf-8")
        code, out, err = launch(["open", self.link("launch.main", "9.9.9"), "--yes", "--no-pause", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 2)
        self.assertIn("not in the catalogue", err)
        code, out, err = launch(["open", self.link("launch.other", "1.0.0"), "--yes", "--no-pause", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 2)
        self.assertIn("built for 1.0.0.1", err)
        self.assertFalse((self.game / "mods" / "launch.other.wotbmod").exists())
        # A signature the trust store does not know is refused under the mandatory policy.
        (self.game / "mods" / "trust" / "keys" / "launch-key.p256").unlink()
        code, out, err = launch(["open", self.link("launch.dep", "1.0.0"), "--yes", "--no-pause", "--require-signature",
                                 "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 2)
        self.assertIn("untrusted", err)
        self.assertFalse((self.game / "mods" / "launch.dep.wotbmod").exists())

    def test_lua_packages_install_into_the_lua_folder_and_roll_back(self) -> None:
        code, out, err = launch(["open", self.link("launch.lua", "1.0.0"), "--yes", "--no-pause", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 0, err)
        folder = self.game / "mods" / "lua" / "launch.lua"
        self.assertTrue((folder / "main.lua").is_file())
        self.assertTrue((folder / "lib" / "util.lua").is_file())
        self.assertEqual(json.loads((folder / "manifest.json").read_text(encoding="utf-8"))["id"], "launch.lua")
        self.assertIn("(Lua mod folder)", out)
        self.assertFalse((self.game / "mods" / "launch.lua.wotbmod").exists(), "no archive in mods/ for the native loader to refuse")
        cached = self.game / "mods" / "cache" / "lua_packages" / "launch.lua.wotbmod"
        self.assertTrue(cached.is_file() and cached.with_name("launch.lua.wotbmod.sig").is_file())
        code, out, err = cli(["list", "--game-root", str(self.game), "--json"])
        rows = {row["id"]: row for row in json.loads(out)["packages"]}
        self.assertEqual((rows["launch.lua"]["kind"], rows["launch.lua"]["signature"]), ("lua", "valid"))
        # A newer version replaces the folder; rollback brings the old files back.
        self.publish(lua_project(self.root / "src" / "lua2", "launch.lua", "1.1.0"))
        (self.root / "src" / "lua2" / "main.lua").write_text("-- v1.1\n", encoding="utf-8")
        code, out, err = cli(["update", "launch.lua", "--game-root", str(self.game), "--yes", "--catalog", str(self.catalog)], self.game)
        self.assertEqual(code, 0, err)
        self.assertEqual(self.ledger()["launch.lua"]["version"], "1.1.0")
        code, out, err = cli(["rollback", "launch.lua", "--game-root", str(self.game), "--yes"], self.game)
        self.assertEqual(code, 0, err)
        self.assertIn("hello from", (folder / "main.lua").read_text(encoding="utf-8"))
        self.assertEqual(self.ledger()["launch.lua"]["version"], "1.0.0")
        # Uninstall through a link removes this folder only.
        self.assertEqual(launch(["open", self.link("launch.dep", "1.0.0"), "--yes", "--no-pause", "--game-root", str(self.game)], self.game)[0], 0)
        code, out, err = launch(["open", "wotbmod://uninstall/launch.lua", "--yes", "--no-pause", "--game-root", str(self.game)], self.game)
        self.assertEqual(code, 0, err)
        self.assertFalse(folder.exists())
        self.assertFalse(cached.exists())
        self.assertTrue((self.game / "mods" / "launch.dep.wotbmod").is_file())
        self.assertTrue((self.game / "mods" / "launch.dep.wotbmod.sig").is_file())

    @unittest.skipUnless(sys.platform == "win32", "URL scheme registration is Windows-only")
    def test_register_and_unregister_a_scheme(self) -> None:
        scheme = "wotbmod-t" + secrets.token_hex(3)
        try:
            code, out, err = launch(["register", "--scheme", scheme, "--game-root", str(self.game)], self.game)
            self.assertEqual(code, 0, err)
            command = launcher.registered_command(scheme)
            self.assertIsNotNone(command)
            self.assertIn("wotbmod_launcher.py", command)
            self.assertIn('open "%1"', command)
            config = json.loads((self.root / "home" / "WotbMod" / "launcher.json").read_text(encoding="utf-8"))
            self.assertEqual(config["game_root"], str(self.game))
            code, out, err = launch(["status", "--scheme", scheme], self.game)
            self.assertIn("registered", out)
        finally:
            code, out, err = launch(["unregister", "--scheme", scheme], self.game)
        self.assertEqual(code, 0, err)
        self.assertIsNone(launcher.registered_command(scheme))


class FrozenCommandTests(unittest.TestCase):
    def test_frozen_executable_registers_itself(self) -> None:
        with mock.patch.object(sys, "frozen", True, create=True), \
                mock.patch.object(sys, "executable", r"C:\\Games\\WoT Blitz\\wotbmod\\wotbmod.exe"):
            command = launcher._launcher_command(None)
        self.assertTrue(command.endswith('wotbmod.exe" launcher open "%1"'), command)
        self.assertNotIn(".py", command)
        explicit = launcher._launcher_command("python")
        self.assertIn('wotbmod_launcher.py" open "%1"', explicit)


if __name__ == "__main__":
    unittest.main()
