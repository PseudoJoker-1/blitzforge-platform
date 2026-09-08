"""Entry point of the frozen `wotbmod.exe` (built by PyInstaller from
tools/build_public_preview.ps1).

One executable, two jobs:

    wotbmod install <id> --catalog https://blitz-forge.org/api/v1   (the CLI)
    wotbmod launcher open "wotbmod://install/<id>@<version>?..."     (the browser hand-off)

The tools stay plain Python files shipped inside the executable as data
(sys._MEIPASS); this script loads them by path the same way the launcher
already loads the CLI, so nothing in the tools changes for the frozen case.
The executable lives in <game>\\wotbmod\\, so that game folder is the default
--game-root (the source checkout's rule, "two levels above tools/", would
point into the temporary extraction folder instead).
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path


def tools_root() -> Path:
    frozen_root = getattr(sys, "_MEIPASS", None)
    if getattr(sys, "frozen", False) and frozen_root:
        return Path(frozen_root)
    return Path(__file__).resolve().parent


def default_game_root_for(executable: Path) -> Path | None:
    """<game> when the executable sits in <game>\\wotbmod\\ (or anywhere under the game)."""
    for candidate in (executable.parent.parent, executable.parent):
        if (candidate / "wotblitz.exe").is_file():
            return candidate
    return None


def load_tool(name: str):
    module = sys.modules.get(name)
    if module is not None:
        return module
    spec = importlib.util.spec_from_file_location(name, tools_root() / f"{name}.py")
    if spec is None or spec.loader is None:
        raise SystemExit(f"{name}.py is missing next to the executable")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def _pyinstaller_hints() -> None:  # never called: static imports for the dependency scan
    import wotbmod  # noqa: F401
    import wotbmod_dvpl  # noqa: F401
    import wotbmod_launcher  # noqa: F401
    import wotbmod_packages  # noqa: F401
    import wotbmod_scan  # noqa: F401
    import wotbmod_trust  # noqa: F401


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    sys.path.insert(0, str(tools_root()))
    game_root = default_game_root_for(Path(sys.executable).resolve()) if getattr(sys, "frozen", False) else None
    if argv and argv[0] == "launcher":
        launcher = load_tool("wotbmod_launcher")
        if game_root is not None:
            launcher.wotbmod.DEFAULT_GAME_ROOT = game_root
        return int(launcher.main(argv[1:]))
    cli = load_tool("wotbmod")
    if game_root is not None:
        cli.DEFAULT_GAME_ROOT = game_root
    return int(cli.main(argv))


if __name__ == "__main__":
    sys.exit(main())
