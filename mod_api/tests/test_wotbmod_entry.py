"""The frozen wotbmod.exe entry: dispatch between the CLI and the launcher,
and the game folder it assumes when it sits in <game>\\wotbmod\\."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
SPEC = importlib.util.spec_from_file_location("wotbmod_entry", TOOLS / "wotbmod_entry.py")
assert SPEC is not None and SPEC.loader is not None
entry = importlib.util.module_from_spec(SPEC)
sys.modules["wotbmod_entry"] = entry
SPEC.loader.exec_module(entry)


class EntryTests(unittest.TestCase):
    def test_game_root_is_the_folder_above_wotbmod(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wotbmod_entry_") as temporary:
            game = Path(temporary) / "World of Tanks Blitz"
            (game / "wotbmod").mkdir(parents=True)
            exe = game / "wotbmod" / "wotbmod.exe"
            exe.write_bytes(b"MZ")
            self.assertIsNone(entry.default_game_root_for(exe))
            (game / "wotblitz.exe").write_bytes(b"MZ")
            self.assertEqual(entry.default_game_root_for(exe), game)
            self.assertEqual(entry.default_game_root_for(game / "wotbmod.exe"), game)

    def test_dispatch_reaches_the_cli_and_the_launcher(self) -> None:
        calls: list[tuple[str, list[str]]] = []
        cli = mock.Mock(main=lambda argv: calls.append(("cli", argv)) or 3)
        launcher = mock.Mock(main=lambda argv: calls.append(("launcher", argv)) or 4, wotbmod=mock.Mock())
        with mock.patch.object(entry, "load_tool", side_effect=lambda name: {"wotbmod": cli, "wotbmod_launcher": launcher}[name]):
            self.assertEqual(entry.main(["list", "--json"]), 3)
            self.assertEqual(entry.main(["launcher", "status"]), 4)
        self.assertEqual(calls, [("cli", ["list", "--json"]), ("launcher", ["status"])])

    def test_source_tree_tools_root_is_the_tools_folder(self) -> None:
        self.assertEqual(entry.tools_root(), TOOLS)


if __name__ == "__main__":
    unittest.main()
