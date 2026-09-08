from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock


SDK_ROOT = Path(__file__).resolve().parents[1]
CLI_PATH = SDK_ROOT / "tools" / "wotbmod.py"
SPEC = importlib.util.spec_from_file_location("wotbmod_cli", CLI_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load CLI module: {CLI_PATH}")
wotbmod = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = wotbmod
SPEC.loader.exec_module(wotbmod)


def native_manifest(
    *,
    mod_id: str = "tests.cli",
    entrypoint: str = "bin/windows-x86/cli.dll",
) -> dict[str, object]:
    return {
        "manifest_version": 1,
        "type": "native",
        "id": mod_id,
        "name": "CLI Test",
        "version": "1.2.3",
        "developer": "WotbMod Tests",
        "api": {"wotbmod.core": ">=1 <2"},
        "client": {
            "builds": [wotbmod.EXPECTED_CLIENT_BUILD],
            "executable_hashes": [wotbmod.EXPECTED_CLIENT_SHA256],
        },
        "entrypoints": {"windows-x86": entrypoint},
        "permissions": ["core", "resources.mod"],
        "resources": ["assets/**"],
    }


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def create_native_project(root: Path) -> None:
    write_json(root / "manifest.json", native_manifest())
    payload = root / "bin" / "windows-x86" / "cli.dll"
    payload.parent.mkdir(parents=True)
    payload.write_bytes(b"MZ deterministic test DLL bytes\n")
    assets = root / "assets"
    assets.mkdir()
    (assets / "sample.txt").write_bytes(b"sample asset\n")


def create_fake_game(root: Path) -> Path:
    root.mkdir()
    executable = root / "wotblitz.exe"
    executable.write_bytes(b"MZ fake exact-client fixture\n")
    return executable


@contextlib.contextmanager
def matching_client_fingerprint(executable: Path):
    real_sha256_file = wotbmod._sha256_file

    def sha256_file(path: Path) -> str:
        if Path(path) == executable:
            return wotbmod.EXPECTED_CLIENT_SHA256
        return real_sha256_file(Path(path))

    with (
        mock.patch.object(
            wotbmod,
            "_pe_architecture",
            return_value=wotbmod.EXPECTED_CLIENT_ARCH,
        ),
        mock.patch.object(
            wotbmod,
            "_windows_file_version",
            return_value=wotbmod.EXPECTED_CLIENT_BUILD,
        ),
        mock.patch.object(wotbmod, "_sha256_file", side_effect=sha256_file),
    ):
        yield


class WotbModCliTests(unittest.TestCase):
    def test_new_native_scaffolds_manifest_and_source(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_new_") as temporary:
            project = Path(temporary) / "native-project"
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = wotbmod.main(
                    [
                        "new",
                        str(project),
                        "--id",
                        "tests.scaffold",
                        "--name",
                        "Scaffold Test",
                        "--developer",
                        "WotbMod Tests",
                    ]
                )
            self.assertEqual(result, 0)
            self.assertTrue((project / "manifest.json").is_file())
            self.assertTrue((project / "src" / "mod.cpp").is_file())
            manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["id"], "tests.scaffold")
            self.assertEqual(
                manifest["client"]["executable_hashes"],
                [wotbmod.EXPECTED_CLIENT_SHA256],
            )
            source = (project / "src" / "mod.cpp").read_text(encoding="utf-8")
            self.assertIn("WOTBMOD_V3_ENTRY", source)
            self.assertIn("OnEnable", source)
            self.assertIn("Created native project", output.getvalue())

    def test_new_content_scaffolds_loadable_descriptor(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_content_") as temporary:
            project = Path(temporary) / "content-project"
            result = wotbmod.main(
                [
                    "new",
                    str(project),
                    "--id",
                    "tests.content",
                    "--name",
                    "Content Test",
                    "--developer",
                    "WotbMod Tests",
                    "--type",
                    "content",
                ]
            )
            self.assertEqual(result, 0)
            self.assertTrue((project / "content.json").is_file())
            view = wotbmod.load_package(project)
            self.assertEqual(view.manifest["type"], "content")
            self.assertEqual(view.manifest["content"], "content.json")

    def test_new_lua_copies_a_shipped_template(self) -> None:
        for template, example in wotbmod.LUA_TEMPLATES.items():
            with tempfile.TemporaryDirectory(prefix="wotbmod_cli_lua_") as temporary:
                project = Path(temporary) / f"lua-{template}"
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = wotbmod.main(
                        [
                            "new",
                            str(project),
                            "--id",
                            "tests.lua_mod",
                            "--name",
                            "Lua Test",
                            "--developer",
                            "WotbMod Tests",
                            "--type",
                            "lua",
                            "--template",
                            template,
                        ]
                    )
                self.assertEqual(result, 0, template)
                manifest = json.loads((project / "manifest.json").read_text(encoding="utf-8"))
                self.assertEqual(manifest["id"], "tests.lua_mod")
                self.assertEqual(manifest["name"], "Lua Test")
                self.assertEqual(manifest["entrypoint"], "main.lua")
                source = (project / "main.lua").read_text(encoding="utf-8")
                example_source = (
                    SDK_ROOT / "examples" / example / "main.lua"
                ).read_text(encoding="utf-8")
                self.assertEqual(source, example_source, template)
                for permission in manifest["permissions"]:
                    self.assertIn(permission, wotbmod.REGISTERED_PERMISSIONS)
                self.assertIn("Created lua project", output.getvalue())

    def test_new_lua_defaults_to_hello_and_needs_an_author_prefix(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_lua_") as temporary:
            project = Path(temporary) / "lua-default"
            result = wotbmod.main(
                [
                    "new", str(project), "--id", "tests.default", "--name", "Default",
                    "--developer", "WotbMod Tests", "--type", "lua",
                ]
            )
            self.assertEqual(result, 0)
            self.assertIn("on_enable", (project / "main.lua").read_text(encoding="utf-8"))
            bad = Path(temporary) / "lua-bad"
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertNotEqual(
                    wotbmod.main(
                        ["new", str(bad), "--id", "noprefix", "--name", "Bad",
                         "--developer", "WotbMod Tests", "--type", "lua"]
                    ),
                    0,
                )
            self.assertFalse(bad.exists())

    def test_validate_rejects_duplicate_json_key(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_duplicate_") as temporary:
            manifest = Path(temporary) / "manifest.json"
            manifest.write_text(
                '{"manifest_version":1,"id":"tests.one","id":"tests.two"}',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(wotbmod.CliError, "duplicate JSON object key"):
                wotbmod.load_package(manifest, client_build=None, client_sha256=None)

    def test_validate_rejects_manifest_traversal(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_traversal_") as temporary:
            root = Path(temporary) / "project"
            root.mkdir()
            write_json(
                root / "manifest.json",
                native_manifest(entrypoint="../outside.dll"),
            )
            (Path(temporary) / "outside.dll").write_bytes(b"MZ")
            with self.assertRaisesRegex(wotbmod.CliError, "entrypoint is unsafe"):
                wotbmod.load_package(root)

    def test_pack_is_byte_deterministic_and_store_only(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_pack_") as temporary:
            root = Path(temporary)
            project = root / "project"
            project.mkdir()
            create_native_project(project)
            first = root / "first.wotbmod"
            second = root / "second.wotbmod"
            self.assertEqual(
                wotbmod.main(["pack", str(project), "-o", str(first)]),
                0,
            )
            os.utime(project / "assets" / "sample.txt", None)
            self.assertEqual(
                wotbmod.main(["pack", str(project), "-o", str(second)]),
                0,
            )
            self.assertEqual(first.read_bytes(), second.read_bytes())
            with zipfile.ZipFile(first, "r") as archive:
                self.assertTrue(archive.infolist())
                self.assertTrue(
                    all(
                        info.compress_type == zipfile.ZIP_STORED
                        for info in archive.infolist()
                    )
                )
                self.assertTrue(
                    all(info.date_time == (1980, 1, 1, 0, 0, 0) for info in archive.infolist())
                )
            first_view = wotbmod.load_package(first)
            second_view = wotbmod.load_package(second)
            self.assertEqual(first_view.package_sha256, second_view.package_sha256)
            self.assertEqual(first_view.payload_sha256, second_view.payload_sha256)

    def test_archive_rejects_traversal_entry(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_zip_traversal_") as temporary:
            archive_path = Path(temporary) / "bad.wotbmod"
            with zipfile.ZipFile(
                archive_path,
                "w",
                compression=zipfile.ZIP_STORED,
                allowZip64=False,
            ) as archive:
                archive.writestr(
                    "manifest.json",
                    json.dumps(native_manifest(entrypoint="bin/windows-x86/cli.dll")),
                )
                archive.writestr("bin/windows-x86/cli.dll", b"MZ")
                archive.writestr("../escape.txt", b"escape")
            with self.assertRaisesRegex(wotbmod.CliError, "unsafe path"):
                wotbmod.load_package(archive_path)

    def test_archive_rejects_symlink_metadata(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_zip_symlink_") as temporary:
            archive_path = Path(temporary) / "symlink.wotbmod"
            with zipfile.ZipFile(
                archive_path,
                "w",
                compression=zipfile.ZIP_STORED,
                allowZip64=False,
            ) as archive:
                archive.writestr("manifest.json", json.dumps(native_manifest()))
                archive.writestr("bin/windows-x86/cli.dll", b"MZ")
                link = zipfile.ZipInfo("assets/link")
                link.compress_type = zipfile.ZIP_STORED
                link.create_system = 3
                link.external_attr = 0o120777 << 16
                archive.writestr(link, b"target")
            with self.assertRaisesRegex(wotbmod.CliError, "symlink metadata"):
                wotbmod.load_package(archive_path)

    def test_directory_rejects_reparse_detection(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_dir_symlink_") as temporary:
            project = Path(temporary) / "project"
            project.mkdir()
            create_native_project(project)
            blocked = project / "assets" / "blocked.txt"
            blocked.write_text("blocked", encoding="utf-8")
            real_detector = wotbmod._is_reparse_or_symlink

            def detector(path: Path) -> bool:
                if Path(path) == blocked:
                    return True
                return real_detector(Path(path))

            with mock.patch.object(
                wotbmod,
                "_is_reparse_or_symlink",
                side_effect=detector,
            ):
                with self.assertRaisesRegex(wotbmod.CliError, "symlink/reparse"):
                    wotbmod.load_package(project)

    def test_inspect_json_contains_metadata_and_hashes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_inspect_") as temporary:
            project = Path(temporary) / "project"
            project.mkdir()
            create_native_project(project)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                result = wotbmod.main(["inspect", str(project), "--json"])
            self.assertEqual(result, 0)
            metadata = json.loads(output.getvalue())
            self.assertTrue(metadata["valid"])
            self.assertEqual(metadata["manifest"]["id"], "tests.cli")
            self.assertRegex(metadata["package_sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(metadata["payload_sha256"], r"^[0-9a-f]{64}$")
            self.assertEqual(metadata["file_count"], 3)

    def test_build_passes_literal_argv_without_shell_interpolation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_build_") as temporary:
            project = Path(temporary) / "project"
            project.mkdir()
            write_json(project / "manifest.json", native_manifest())
            result_file = project / "argv.txt"
            literal = "literal & echo shell-injection-would-change-this"
            command = [
                "build",
                str(project),
                "--",
                sys.executable,
                "-c",
                (
                    "from pathlib import Path; import sys; "
                    "Path(sys.argv[1]).write_text(sys.argv[2], encoding='utf-8')"
                ),
                str(result_file),
                literal,
            ]
            self.assertEqual(wotbmod.main(command), 0)
            self.assertEqual(result_file.read_text(encoding="utf-8"), literal)

    def test_pack_rejects_output_inside_source_tree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_output_") as temporary:
            project = Path(temporary) / "project"
            project.mkdir()
            create_native_project(project)
            output = project / "dist" / "bad.wotbmod"
            with self.assertRaisesRegex(
                wotbmod.CliError,
                "outside the package source tree",
            ):
                wotbmod.command_pack(
                    type(
                        "Args",
                        (),
                        {
                            "project": str(project),
                            "output": str(output),
                            "json": False,
                            "no_client_check": False,
                            "client_build": wotbmod.EXPECTED_CLIENT_BUILD,
                            "client_sha256": wotbmod.EXPECTED_CLIENT_SHA256,
                        },
                    )()
                )

    def test_run_no_launch_atomically_overwrites_exact_package(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_run_no_launch_") as temporary:
            root = Path(temporary)
            project = root / "project"
            project.mkdir()
            create_native_project(project)
            game_root = root / "game"
            executable = create_fake_game(game_root)
            mods_root = game_root / "mods"
            mods_root.mkdir()
            target = mods_root / "tests.cli.wotbmod"
            target.write_bytes(b"previous package bytes")

            output = io.StringIO()
            with (
                matching_client_fingerprint(executable),
                mock.patch.object(wotbmod.subprocess, "Popen") as popen,
                mock.patch.object(
                    wotbmod.os,
                    "replace",
                    wraps=os.replace,
                ) as replace,
                contextlib.redirect_stdout(output),
            ):
                result = wotbmod.main(
                    [
                        "run",
                        str(project),
                        "--game-root",
                        str(game_root),
                        "--no-launch",
                    ]
                )

            self.assertEqual(result, 0)
            popen.assert_not_called()
            installed = wotbmod.load_package(target)
            self.assertEqual(installed.manifest["id"], "tests.cli")
            self.assertTrue(
                any(
                    Path(call.args[1]) == target
                    for call in replace.call_args_list
                    if len(call.args) >= 2
                )
            )
            self.assertFalse(list(mods_root.glob("*.stage")))
            self.assertFalse(list(mods_root.glob("*.last-good")))
            self.assertIn("Launch skipped", output.getvalue())

    def test_run_launches_exact_executable_with_literal_forwarded_argv(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_run_argv_") as temporary:
            root = Path(temporary)
            project = root / "project"
            project.mkdir()
            create_native_project(project)
            archive = root / "input.wotbmod"
            self.assertEqual(
                wotbmod.main(["pack", str(project), "--output", str(archive)]),
                0,
            )
            game_root = root / "game"
            executable = create_fake_game(game_root)
            literal = "literal & echo shell-injection-would-change-this"
            process = mock.Mock(pid=4242)

            with (
                matching_client_fingerprint(executable),
                mock.patch.object(
                    wotbmod.subprocess,
                    "Popen",
                    return_value=process,
                ) as popen,
            ):
                result = wotbmod.main(
                    [
                        "run",
                        str(archive),
                        "--game-root",
                        str(game_root),
                        "--",
                        "--test-argument",
                        literal,
                    ]
                )

            self.assertEqual(result, 0)
            popen.assert_called_once_with(
                [str(executable), "--test-argument", literal],
                cwd=game_root.resolve(),
                shell=False,
            )
            self.assertTrue((game_root / "mods" / "tests.cli.wotbmod").is_file())

    def test_run_restores_last_good_when_launch_preparation_fails(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_run_rollback_") as temporary:
            root = Path(temporary)
            project = root / "project"
            project.mkdir()
            create_native_project(project)
            game_root = root / "game"
            executable = create_fake_game(game_root)
            mods_root = game_root / "mods"
            mods_root.mkdir()
            target = mods_root / "tests.cli.wotbmod"
            last_good = b"last-good package bytes"
            target.write_bytes(last_good)

            errors = io.StringIO()
            with (
                matching_client_fingerprint(executable),
                mock.patch.object(
                    wotbmod.subprocess,
                    "Popen",
                    side_effect=OSError("fixture launch failure"),
                ),
                contextlib.redirect_stderr(errors),
            ):
                result = wotbmod.main(
                    [
                        "run",
                        str(project),
                        "--game-root",
                        str(game_root),
                    ]
                )

            self.assertEqual(result, 2)
            self.assertEqual(target.read_bytes(), last_good)
            self.assertFalse(list(mods_root.glob("*.stage")))
            self.assertFalse(list(mods_root.glob("*.last-good")))
            self.assertIn("cannot launch client executable", errors.getvalue())

    def test_run_invalid_package_does_not_overwrite_installed_package(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_run_invalid_") as temporary:
            root = Path(temporary)
            project = root / "project"
            project.mkdir()
            write_json(project / "manifest.json", native_manifest())
            game_root = root / "game"
            executable = create_fake_game(game_root)
            mods_root = game_root / "mods"
            mods_root.mkdir()
            target = mods_root / "tests.cli.wotbmod"
            installed_bytes = b"installed package remains untouched"
            target.write_bytes(installed_bytes)

            with (
                matching_client_fingerprint(executable),
                mock.patch.object(wotbmod.subprocess, "Popen") as popen,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = wotbmod.main(
                    [
                        "run",
                        str(project),
                        "--game-root",
                        str(game_root),
                    ]
                )

            self.assertEqual(result, 2)
            self.assertEqual(target.read_bytes(), installed_bytes)
            popen.assert_not_called()

    def test_run_client_fingerprint_mismatch_does_not_overwrite(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_run_fingerprint_") as temporary:
            root = Path(temporary)
            project = root / "project"
            project.mkdir()
            create_native_project(project)
            game_root = root / "game"
            executable = create_fake_game(game_root)
            mods_root = game_root / "mods"
            mods_root.mkdir()
            target = mods_root / "tests.cli.wotbmod"
            installed_bytes = b"installed package remains untouched"
            target.write_bytes(installed_bytes)

            with (
                mock.patch.object(
                    wotbmod,
                    "_pe_architecture",
                    return_value=wotbmod.EXPECTED_CLIENT_ARCH,
                ),
                mock.patch.object(
                    wotbmod,
                    "_windows_file_version",
                    return_value=wotbmod.EXPECTED_CLIENT_BUILD,
                ),
                mock.patch.object(wotbmod, "_sha256_file", return_value="0" * 64),
                mock.patch.object(wotbmod.subprocess, "Popen") as popen,
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = wotbmod.main(
                    [
                        "run",
                        str(project),
                        "--game-root",
                        str(game_root),
                    ]
                )

            self.assertEqual(result, 2)
            self.assertEqual(target.read_bytes(), installed_bytes)
            popen.assert_not_called()

    def test_run_rejects_staging_inside_source_tree(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_cli_run_inside_") as temporary:
            project = Path(temporary) / "project"
            project.mkdir()
            create_native_project(project)
            game_root = project / "game"
            executable = create_fake_game(game_root)

            with (
                matching_client_fingerprint(executable),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                result = wotbmod.main(
                    [
                        "run",
                        str(project),
                        "--game-root",
                        str(game_root),
                        "--no-launch",
                    ]
                )

            self.assertEqual(result, 2)
            self.assertFalse((game_root / "mods").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
