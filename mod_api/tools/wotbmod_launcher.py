"""wotbmod:// launcher - the browser-to-client bridge (Stage 5).

    python tools/wotbmod_launcher.py register   [--game-root <dir>] [--scheme wotbmod]
    python tools/wotbmod_launcher.py unregister [--scheme wotbmod]
    python tools/wotbmod_launcher.py status
    python tools/wotbmod_launcher.py open "wotbmod://install/<id>@<version>?source=<catalog url>" [--yes]
    python tools/wotbmod_launcher.py open "wotbmod://uninstall/<id>" [--yes]

For a newcomer: a web page cannot write into your game folder, and it
should not be able to. What it can do is open a link whose scheme
(`wotbmod://`) Windows hands to a program you installed yourself. That
program is this launcher. It reads the link, fetches the *catalogue*
(the same `index.json` the `wotbmod install --catalog` command reads),
finds exactly the release the link names, checks the download's SHA-256
against the catalogue record, and then runs the Stage 3 installer, which
verifies the package and its signature again, shows the hash, the
permissions and the dependency plan, and asks before touching `mods/`.

The link may name only a catalogue release (`id@version` + `source` URL).
It cannot point at a DLL, a local path or an arbitrary file: anything else
is refused before any network request. Pressing the button twice is
idempotent (the second run finds the same hash installed and stops).

Stdlib only. Registration writes HKCU\\Software\\Classes\\<scheme> (no
administrator rights needed); `unregister` removes it.
"""

from __future__ import annotations

import argparse
import datetime
import importlib.util
import json
import os
import re
import sys
import tempfile
import urllib.parse
from pathlib import Path
from typing import Any

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

DEFAULT_SCHEME = "wotbmod"
SCHEME_RE = re.compile(r"^[a-z][a-z0-9+.-]{2,31}$")
CONFIG_DIR_NAME = "WotbMod"


def _load_cli():
    module = sys.modules.get("wotbmod_cli") or sys.modules.get("wotbmod")
    if module is None:
        spec = importlib.util.spec_from_file_location("wotbmod_cli", TOOLS / "wotbmod.py")
        assert spec is not None and spec.loader is not None
        module = importlib.util.module_from_spec(spec)
        sys.modules["wotbmod_cli"] = module
        spec.loader.exec_module(module)
    module.build_parser()
    return module


wotbmod = _load_cli()
packages = sys.modules["wotbmod_packages"]


class LaunchError(Exception):
    pass


# --- configuration --------------------------------------------------------------------

def config_dir() -> Path:
    base = os.environ.get("WOTBMOD_LAUNCHER_HOME") or os.environ.get("LOCALAPPDATA") or str(Path.home())
    return Path(base) / CONFIG_DIR_NAME


def load_config() -> dict[str, Any]:
    path = config_dir() / "launcher.json"
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def save_config(config: dict[str, Any]) -> Path:
    path = config_dir() / "launcher.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(config, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def log_line(text: str) -> None:
    try:
        path = config_dir() / "launcher.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        with path.open("a", encoding="utf-8") as stream:
            stream.write(f"[{stamp}] {text}\n")
    except OSError:
        pass


# --- the link -----------------------------------------------------------------------------

def parse_link(url: str, *, scheme: str = DEFAULT_SCHEME) -> dict[str, Any]:
    """`<scheme>://install/<id>@<version>?source=<https catalogue>` or `<scheme>://uninstall/<id>`.

    Strict on purpose: a browser can send anything. Only a mod id, an exact
    semantic version and an http(s) catalogue base come through; a path, a
    file name or a query the launcher does not know is an error.
    """
    parsed = urllib.parse.urlsplit(url.strip())
    if parsed.scheme.lower() != scheme:
        raise LaunchError(f"link scheme must be {scheme}://, got {parsed.scheme or '(none)'}://")
    action = parsed.netloc.lower()
    target = parsed.path.strip("/")
    query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
    unknown = set(query) - {"source"}
    if unknown:
        raise LaunchError(f"unknown link parameters: {', '.join(sorted(unknown))}")
    if parsed.fragment:
        raise LaunchError("a link fragment is not allowed")
    if action == "install":
        mod_id, at, version = target.partition("@")
        if not at or not version:
            raise LaunchError("install links look like install/<id>@<version>")
        if not wotbmod._is_ascii_identifier(mod_id, wotbmod.MAX_ID_BYTES) or ".." in mod_id or "/" in mod_id:
            raise LaunchError(f"invalid mod id in link: {mod_id!r}")
        if wotbmod._parse_semver(version) is None:
            raise LaunchError(f"invalid version in link: {version!r}")
        sources = query.get("source", [])
        if len(sources) != 1 or not sources[0]:
            raise LaunchError("install links need exactly one source=<catalogue url>")
        source = sources[0]
        if not packages._is_url(source):
            raise LaunchError("source must be an http(s) catalogue URL, never a local path")
        source_parts = urllib.parse.urlsplit(source)
        if not source_parts.netloc or source_parts.query or source_parts.fragment:
            raise LaunchError("source must be a plain catalogue base URL")
        return {"action": "install", "id": mod_id, "version": version, "source": source}
    if action == "uninstall":
        if query or not target or "@" in target:
            raise LaunchError("uninstall links look like uninstall/<id>")
        if not wotbmod._is_ascii_identifier(target, wotbmod.MAX_ID_BYTES) or ".." in target or "/" in target:
            raise LaunchError(f"invalid mod id in link: {target!r}")
        return {"action": "uninstall", "id": target}
    raise LaunchError(f"unknown link action {action!r}; expected install or uninstall")


# --- actions ----------------------------------------------------------------------------------

def resolve_game_root(explicit: str | None) -> Path:
    candidate = explicit or load_config().get("game_root") or str(wotbmod.DEFAULT_GAME_ROOT)
    root = Path(candidate).expanduser()
    if not (root / "wotblitz.exe").is_file():
        raise LaunchError(f"game root has no wotblitz.exe: {root} (run: wotbmod_launcher register --game-root <dir>)")
    return root


def run_install(link: dict[str, Any], game_root: Path, *, yes: bool, require_signature: bool = False,
                prerelease: bool = False) -> int:
    """Fetch exactly the named release from the catalogue, then hand it to `wotbmod install`."""
    catalog = packages.Catalog(link["source"])
    candidate = next((item for item in catalog.versions(link["id"]) if item.version == link["version"]), None)
    if candidate is None:
        raise LaunchError(f"{link['id']} {link['version']} is not in the catalogue {link['source']}")
    if candidate.status not in ("published", "listed"):
        raise LaunchError(f"{link['id']} {link['version']} is {candidate.status} in the catalogue")
    reason = catalog.is_revoked(link["id"], link["version"])
    if reason:
        raise LaunchError(f"{link['id']} {link['version']} was revoked by the portal ({reason})")
    if candidate.client_builds and wotbmod.EXPECTED_CLIENT_BUILD not in candidate.client_builds:
        raise LaunchError(f"{link['id']} {link['version']} is built for {', '.join(candidate.client_builds)}, "
                          f"this launcher serves client {wotbmod.EXPECTED_CLIENT_BUILD}")
    print(f"Catalogue: {link['source']}")
    print(f"Release:   {link['id']} {link['version']} (record SHA-256 {candidate.sha256})")
    with tempfile.TemporaryDirectory(prefix="wotbmod-launch-") as temporary:
        workdir = Path(temporary)
        artifact, sidecar = catalog.fetch(candidate, workdir)  # hash-checked against the record
        argv = ["install", str(artifact), "--game-root", str(game_root), "--catalog", link["source"]]
        if sidecar is not None:
            argv += ["--sidecar", str(sidecar)]
        if yes:
            argv.append("--yes")
        if require_signature:
            argv.append("--require-signature")
        if prerelease:
            argv.append("--pre")
        result = wotbmod.main(argv)
    if result == 0:
        # Best effort, never a failure of the install: other revocations from
        # this portal and the loader's crash record, if any.
        try:
            wotbmod.main(["sync", "--game-root", str(game_root), "--catalog", link["source"], "--yes"])
        except SystemExit:
            pass
        except Exception as exc:  # noqa: BLE001
            log_line(f"sync after install failed: {exc}")
    return result


def run_uninstall(link: dict[str, Any], game_root: Path, *, yes: bool) -> int:
    argv = ["uninstall", link["id"], "--game-root", str(game_root)]
    if yes:
        argv.append("--yes")
    return wotbmod.main(argv)


def command_open(args: argparse.Namespace) -> int:
    scheme = (args.scheme or load_config().get("scheme") or DEFAULT_SCHEME).lower()
    # Automation (tests, an end-to-end run driven from a script) cannot type
    # into the console the browser opened: these two variables stand in for
    # --yes and --no-pause. A person clicking in a browser never sets them.
    if os.environ.get("WOTBMOD_LAUNCHER_YES") == "1":
        args.yes = True
    if os.environ.get("WOTBMOD_LAUNCHER_NO_PAUSE") == "1":
        args.no_pause = True
    try:
        link = parse_link(args.url, scheme=scheme)
        game_root = resolve_game_root(args.game_root)
        log_line(f"open {args.url} -> {json.dumps(link, ensure_ascii=False)} game_root={game_root}")
        if link["action"] == "install":
            result = run_install(link, game_root, yes=args.yes, require_signature=args.require_signature, prerelease=args.pre)
        else:
            result = run_uninstall(link, game_root, yes=args.yes)
        log_line(f"result {result} for {args.url}")
    except LaunchError as exc:
        print(f"wotbmod launcher: refused: {exc}", file=sys.stderr)
        log_line(f"refused {args.url}: {exc}")
        result = 2
    if not args.no_pause and sys.stdin is not None and sys.stdin.isatty():
        try:
            input("Press Enter to close this window.")
        except EOFError:
            pass
    return result


# --- registration (Windows) ------------------------------------------------------------------

def _launcher_command(python: str | None) -> str:
    if python is None and getattr(sys, "frozen", False):
        # Inside wotbmod.exe: the executable is both the CLI and the launcher.
        return f'"{Path(sys.executable).resolve()}" launcher open "%1"'
    executable = python or sys.executable
    return f'"{executable}" "{Path(__file__).resolve()}" open "%1"'


def command_register(args: argparse.Namespace) -> int:
    scheme = args.scheme.lower()
    if SCHEME_RE.fullmatch(scheme) is None:
        raise LaunchError("scheme: 3-32 lowercase letters, digits, '+', '.', '-'")
    game_root = Path(args.game_root).expanduser() if args.game_root else Path(load_config().get("game_root") or wotbmod.DEFAULT_GAME_ROOT)
    if not (game_root / "wotblitz.exe").is_file():
        raise LaunchError(f"game root has no wotblitz.exe: {game_root}")
    config = load_config()
    config.update({"game_root": str(game_root), "scheme": scheme, "registered_at": datetime.datetime.now().isoformat(timespec="seconds")})
    config_path = save_config(config)
    command = _launcher_command(args.python)
    if sys.platform != "win32":
        print(f"config written: {config_path}; URL scheme registration is Windows-only (command would be: {command})")
        return 0
    import winreg

    root = winreg.CreateKey(winreg.HKEY_CURRENT_USER, rf"Software\Classes\{scheme}")
    try:
        winreg.SetValueEx(root, "", 0, winreg.REG_SZ, "URL:WotbMod package installer")
        winreg.SetValueEx(root, "URL Protocol", 0, winreg.REG_SZ, "")
        with winreg.CreateKey(root, r"shell\open\command") as command_key:
            winreg.SetValueEx(command_key, "", 0, winreg.REG_SZ, command)
    finally:
        winreg.CloseKey(root)
    print(f"registered {scheme}:// -> {command}")
    print(f"game root: {game_root} (config {config_path})")
    return 0


def command_unregister(args: argparse.Namespace) -> int:
    scheme = args.scheme.lower()
    if sys.platform != "win32":
        print("URL scheme registration is Windows-only; nothing to remove")
        return 0
    import winreg

    base = rf"Software\Classes\{scheme}"
    removed = False
    for sub in (rf"{base}\shell\open\command", rf"{base}\shell\open", rf"{base}\shell", base):
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, sub)
            removed = True
        except OSError:
            pass
    print(f"{'removed' if removed else 'not registered'}: {scheme}://")
    return 0


def registered_command(scheme: str = DEFAULT_SCHEME) -> str | None:
    if sys.platform != "win32":
        return None
    import winreg

    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, rf"Software\Classes\{scheme}\shell\open\command") as key:
            value, _kind = winreg.QueryValueEx(key, "")
            return str(value)
    except OSError:
        return None


def command_status(args: argparse.Namespace) -> int:
    config = load_config()
    scheme = (args.scheme or config.get("scheme") or DEFAULT_SCHEME).lower()
    command = registered_command(scheme)
    print(f"scheme:    {scheme}:// {'registered' if command else 'not registered'}")
    if command:
        print(f"command:   {command}")
    print(f"game root: {config.get('game_root') or wotbmod.DEFAULT_GAME_ROOT}")
    print(f"config:    {config_dir() / 'launcher.json'}")
    print(f"log:       {config_dir() / 'launcher.log'}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="wotbmod_launcher", description="wotbmod:// browser-to-client installer")
    sub = parser.add_subparsers(dest="command", required=True)
    register = sub.add_parser("register", help="register the URL scheme for the current user")
    register.add_argument("--game-root")
    register.add_argument("--scheme", default=DEFAULT_SCHEME)
    register.add_argument("--python", help="interpreter to run the launcher with (default: this one)")
    register.set_defaults(handler=command_register)
    unregister = sub.add_parser("unregister", help="remove the URL scheme registration")
    unregister.add_argument("--scheme", default=DEFAULT_SCHEME)
    unregister.set_defaults(handler=command_unregister)
    status = sub.add_parser("status")
    status.add_argument("--scheme", default=None)
    status.set_defaults(handler=command_status)
    open_parser = sub.add_parser("open", help="handle a wotbmod:// link")
    open_parser.add_argument("url")
    open_parser.add_argument("--game-root")
    open_parser.add_argument("--scheme", default=None)
    open_parser.add_argument("--yes", "-y", action="store_true")
    open_parser.add_argument("--require-signature", action="store_true")
    open_parser.add_argument("--pre", action="store_true")
    open_parser.add_argument("--no-pause", action="store_true", help="do not wait for Enter at the end")
    open_parser.set_defaults(handler=command_open)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.handler(args)
    except LaunchError as exc:
        print(f"wotbmod launcher: error: {exc}", file=sys.stderr)
        return 2
    except wotbmod.CliError as exc:
        print(f"wotbmod: error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
