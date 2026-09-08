"""Updating a mod whose DLL the running client has mapped.

The catalogue's ОБНОВИТЬ button is only reachable from inside the game, so
every update it triggers happens while the client is running and holding the
mod's DLL mapped. Windows refuses to overwrite a mapped image, and the button
therefore failed outright with PermissionError - it could never have worked in
the situation it exists for.

The first check loads a real DLL into this process and confirms both halves of
the Windows behaviour place_file() relies on: the overwrite is refused, and
the rename is allowed. Asserting the platform rather than assuming it, because
the whole fix rests on that asymmetry.

    python test_locked_update.py
"""
from __future__ import annotations

import ctypes
import os
import shutil
import sys
import tempfile
from pathlib import Path

import modpack

HERE = Path(__file__).resolve().parent
FAILURES: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if condition else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not condition:
        FAILURES.append(name)


def main() -> int:
    print("the platform behaviour the fix rests on")
    with tempfile.TemporaryDirectory(prefix="blitzforge-locked-") as root:
        root = Path(root)
        # A real, loadable DLL of this interpreter's architecture. version.dll
        # is tiny and has no side effects worth worrying about.
        target = root / "victim.dll"
        shutil.copy2(Path(os.environ["SystemRoot"]) / "System32" / "version.dll",
                     target)
        handle = ctypes.WinDLL(str(target))
        check("the test really did map the DLL", handle is not None)

        replacement = root / "new.tmp"
        replacement.write_bytes(b"NEW CONTENT")

        overwrite_refused = False
        try:
            os.replace(replacement, target)
        except OSError:
            overwrite_refused = True
        check("Windows refuses to overwrite a mapped image", overwrite_refused,
              "if this ever stops being true the fix is unnecessary, not wrong")

        # Now the same move through place_file, which should succeed by
        # pushing the mapped file aside first.
        modpack.place_file(replacement, target)
        check("place_file lands the new bytes anyway",
              target.read_bytes() == b"NEW CONTENT")
        pushed = [p for p in root.iterdir() if modpack.DISPLACED in p.name]
        check("the mapped file is still there under a displaced name",
              len(pushed) == 1, f"found {[p.name for p in pushed]}")
        check("and it still holds the original bytes, not a stub",
              pushed and pushed[0].read_bytes()[:2] == b"MZ")

        print("displaced files are swept only when they can be")
        modpack.sweep_displaced(root)
        check("a still-mapped displaced file survives the sweep",
              pushed[0].exists(), "unlink fails while it is mapped; the sweep "
              "must treat that as normal, not as an error")

        # Release the mapping the way closing the client would.
        ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(handle._handle))
        removed = modpack.sweep_displaced(root)
        check("once nothing maps it, the sweep removes it",
              removed == 1 and not pushed[0].exists(), f"removed={removed}")

    print("an ordinary update takes the plain path")
    with tempfile.TemporaryDirectory(prefix="blitzforge-plain-") as root:
        root = Path(root)
        target = root / "mod.dll"
        target.write_bytes(b"OLD")
        staged = root / "staged.tmp"
        staged.write_bytes(b"NEW")
        modpack.place_file(staged, target)
        check("the file is replaced in place", target.read_bytes() == b"NEW")
        check("nothing is pushed aside when nothing is holding it",
              not any(modpack.DISPLACED in p.name for p in root.iterdir()))

    print("a failed update leaves the mod with its old file, not none")
    with tempfile.TemporaryDirectory(prefix="blitzforge-rollback-") as root:
        root = Path(root)
        target = root / "mod.dll"
        target.write_bytes(b"OLD")
        missing = root / "never-written.tmp"      # staging that does not exist
        raised = False
        try:
            modpack.place_file(missing, target)
        except OSError:
            raised = True
        check("the failure is reported, not swallowed", raised)
        check("the original file is back where it belongs",
              target.exists() and target.read_bytes() == b"OLD",
              "a half-done update that removes the mod is worse than no update")
        check("no displaced debris is left behind",
              not any(modpack.DISPLACED in p.name for p in root.iterdir()))

    print("the sweep cannot touch anything else in the mods tree")
    with tempfile.TemporaryDirectory(prefix="blitzforge-sweep-") as root:
        root = Path(root)
        keep = ["mod.dll", "mod.dll.bak", "manifest.json",
                "native_validation_mod.dll.20260802-194809.bak"]
        for name in keep:
            (root / name).write_bytes(b"x")
        (root / f"mod.dll{modpack.DISPLACED}20260809-120000-1234").write_bytes(b"x")
        removed = modpack.sweep_displaced(root)
        check("only the displaced file goes", removed == 1)
        check("hand-made .bak files are left alone",
              sorted(p.name for p in root.iterdir()) == sorted(keep),
              "the mods tree is full of them from earlier work")

    print()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("locked update: a mapped DLL can be replaced after all")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    sys.exit(main())
