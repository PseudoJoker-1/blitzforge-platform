"""Install and remove mods from the BlitzForge registry.

    python install.py list
    python install.py install <id>
    python install.py remove  <id>
    python install.py update  <id>

Patches are built against the pristine client file, so two mods that edit the
same resource cannot both be applied: the second would be diffed against stock
and would quietly erase the first. Installing records which mod owns which
file, and a conflicting install is refused rather than allowed to win.
"""
from __future__ import annotations

import configparser
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

import modpack
import patch_dvpl
import registry

HERE = Path(__file__).resolve().parent
LEDGER = HERE / "cache" / "installed.json"
DOWNLOADS = HERE / "cache" / "artifacts"
LOADER_SETTINGS = HERE.parent / "mods" / "mods.ini"
PROXY_SOURCE = HERE / "proxy_dll" / "version.dll"
LOADER_SOURCE = HERE / "mod_api" / "build" / "wotb_mod_loader.dll"


# The catalogue screen shows a real progress bar, and this is where the number
# comes from. agent.py installs a reporter here for the length of a request;
# on the command line it stays a no-op, so nothing else has to care.
#
# Downloading is most of the wait but not all of it, so it owns most of the
# bar and verifying/applying finishes it. A bar that sits at 100% while work
# is still going on would be a lie of the ordinary kind.
DOWNLOAD_SHARE = 0.85


class LedgerError(RuntimeError):
    """Installed-state storage is unreadable and must not become an empty set."""


def _validate_ledger(ledger: object, path: Path) -> dict:
    if not isinstance(ledger, dict):
        raise LedgerError(f"installed mod ledger {path} is not a JSON object")
    for mod_id, record in ledger.items():
        if not isinstance(mod_id, str) or not isinstance(record, dict):
            raise LedgerError(f"installed mod ledger {path} has an invalid record")
        if not isinstance(record.get("version"), str):
            raise LedgerError(f"installed mod {mod_id!r} has no valid version")
        targets = record.get("targets")
        if not isinstance(targets, list) or not all(
                isinstance(target, str) for target in targets):
            raise LedgerError(f"installed mod {mod_id!r} has no valid targets list")
    return ledger


def report(fraction: float) -> None:
    if _reporter is not None:
        _reporter.set(fraction)


_reporter = None


def set_reporter(reporter) -> None:
    global _reporter
    _reporter = reporter


def load_ledger(path: Path = LEDGER) -> dict:
    if not path.exists():
        return {}
    try:
        ledger = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        # Returning {} here makes every installed mod look uninstalled and
        # turns every REMOVE button into INSTALL. Preserve the last generated
        # catalogue instead by refusing to build from ambiguous state.
        raise LedgerError(f"cannot read installed mod ledger {path}: {error}") from error
    return _validate_ledger(ledger, path)


def save_ledger(ledger: dict, path: Path = LEDGER) -> None:
    _validate_ledger(ledger, path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(ledger, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    temporary.replace(path)


def _loader_settings(path: Path) -> configparser.ConfigParser:
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    parser.optionxform = str.lower
    if path.exists():
        parser.read(path, encoding="utf-8")
    return parser


def _save_loader_settings(parser: configparser.ConfigParser, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as output:
        parser.write(output, space_around_delimiters=False)
    temporary.replace(path)


def configure_loader(loader: dict, path: Path = LOADER_SETTINGS) -> None:
    loader = modpack.validate_loader_metadata(loader)
    if not loader:
        return
    parser = _loader_settings(path)
    if not parser.has_section("mods"):
        parser.add_section("mods")
    if not parser.has_section("permissions"):
        parser.add_section("permissions")
    parser.set("mods", loader["id"], "1" if loader["enabled"] else "0")
    parser.set("permissions", loader["id"], str(loader["permission_tier"]))
    _save_loader_settings(parser, path)


def remove_loader_config(loader: dict, path: Path = LOADER_SETTINGS) -> None:
    loader = modpack.validate_loader_metadata(loader)
    if not loader or not path.exists():
        return
    parser = _loader_settings(path)
    changed = False
    for section in ("mods", "permissions"):
        if parser.has_section(section):
            changed = parser.remove_option(section, loader["id"]) or changed
    if changed:
        _save_loader_settings(parser, path)


def _copy_bootstrap_file(
        source: Path,
        destination: Path,
        label: str,
        owned_marker: bytes | None = None) -> None:
    if not source.is_file():
        raise SystemExit(f"native {label} is not built: {source}")
    payload = source.read_bytes()
    if destination.is_file():
        installed_payload = destination.read_bytes()
        if installed_payload == payload:
            return
        if owned_marker and owned_marker not in installed_payload:
            raise SystemExit(
                f"refusing to overwrite a foreign native {label}: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    try:
        temporary.write_bytes(payload)
        temporary.replace(destination)
    except OSError as error:
        if temporary.exists():
            temporary.unlink()
        raise SystemExit(
            f"cannot install native {label} at {destination}: {error}") from error


def ensure_native_bootstrap(
        game_root: Path = HERE.parent,
        proxy_source: Path = PROXY_SOURCE,
        loader_source: Path = LOADER_SOURCE,
        system_forwarder: Path | None = None) -> None:
    """Install the global native boundary required by package manifests.

    A package directory plus mods.ini is only configuration. The client must
    also load the VERSION proxy, which in turn loads wotb_mod_loader.dll.
    """
    if os.name != "nt" and system_forwarder is None:
        raise SystemExit("native bootstrap installation is supported on Windows only")
    if system_forwarder is None:
        system_root = Path(os.environ.get("SystemRoot", r"C:\Windows"))
        system_forwarder = system_root / "SysWOW64" / "version.dll"
    _copy_bootstrap_file(
        system_forwarder, game_root / "vorig.dll", "VERSION forwarder")
    _copy_bootstrap_file(
        proxy_source,
        game_root / "version.dll",
        "VERSION proxy",
        b"wotb_mod proxy loaded")
    _copy_bootstrap_file(
        loader_source,
        game_root / "wotb_mod_loader.dll",
        "runtime loader",
        b"BlitzForge live loader starting")


def other_owner(ledger: dict, target: str, installing: str) -> str | None:
    """Any mod other than `installing` that already owns this file.

    Returning the first owner found is not enough: reinstalling a mod puts its
    own id in the ledger, and if that id is returned the caller reads it as
    "no conflict" and a genuine second owner goes unnoticed.
    """
    for mod_id, record in ledger.items():
        if mod_id != installing and target in record.get("targets", []):
            return mod_id
    return None


def download(url: str, expected_sha: str) -> Path:
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    destination = DOWNLOADS / Path(url).name

    if destination.exists() and modpack.sha256(destination.read_bytes()) == expected_sha:
        print(f"  cached  {destination.name}")
        return destination

    print(f"  fetching {url}")
    request = urllib.request.Request(url, headers={"User-Agent": "blitzforge-tools"})
    with urllib.request.urlopen(request, timeout=30) as response:
        # Read in chunks rather than in one call, so the fraction reported to
        # the catalogue is measured off the bytes actually in hand.
        total = int(response.headers.get("Content-Length") or 0)
        chunks, received = [], 0
        while True:
            chunk = response.read(16384)
            if not chunk:
                break
            chunks.append(chunk)
            received += len(chunk)
            if total:
                report(received / total * DOWNLOAD_SHARE)
        payload = b"".join(chunks)
    report(DOWNLOAD_SHARE)

    actual = modpack.sha256(payload)
    if actual != expected_sha:
        # Never unpack an artifact whose bytes are not the reviewed bytes.
        raise SystemExit(
            f"artifact hash mismatch, refusing it\n"
            f"  registry says {expected_sha}\n"
            f"  downloaded    {actual}")

    destination.write_bytes(payload)
    print(f"  verified {len(payload)} B, sha256 ok")
    return destination


def find_mod(mod_id: str) -> dict:
    mods, source = registry.fetch()
    print(f"registry: {source}")
    for mod in mods:
        if mod["id"] == mod_id:
            return mod
    raise SystemExit(f"no mod {mod_id!r} in the registry")


def _version_key(value: str) -> tuple:
    """Return a small semver-compatible key for catalogue versions."""
    core, separator, prerelease = value.partition("-")
    try:
        numeric = tuple(int(part) for part in core.split("."))
    except ValueError:
        return ((), 0, value)
    numeric = numeric + (0,) * max(0, 4 - len(numeric))
    return (numeric, 0 if separator else 1, prerelease)


def compare_versions(left: str, right: str) -> int:
    left_key = _version_key(left)
    right_key = _version_key(right)
    return (left_key > right_key) - (left_key < right_key)


def ensure_installable_version(
        mod_id: str,
        offered_version: str,
        installed_record: dict | None) -> None:
    if not installed_record:
        return
    installed_version = installed_record["version"]
    comparison = compare_versions(offered_version, installed_version)
    if comparison < 0:
        raise SystemExit(
            f"refusing to downgrade {mod_id} from {installed_version} "
            f"to {offered_version}")
    if comparison == 0:
        raise SystemExit(f"{mod_id} {installed_version} is already installed")


def install(mod_id: str) -> None:
    mod = find_mod(mod_id)
    artifact_info = mod.get("artifact")
    if not artifact_info:
        raise SystemExit(
            f"{mod_id} is metadata only; the registry has no artifact for it yet")

    ledger = load_ledger()
    ensure_installable_version(mod_id, mod["version"], ledger.get(mod_id))
    archive = download(artifact_info["url"], artifact_info["sha256"])
    metadata = modpack.read_metadata(archive)
    if metadata["id"] != mod_id or metadata["version"] != mod["version"]:
        raise SystemExit(
            "registry/artifact identity mismatch: "
            f"expected {mod_id} {mod['version']}, got "
            f"{metadata['id']} {metadata['version']}")
    ensure_installable_version(mod_id, metadata["version"], ledger.get(mod_id))
    targets = (
        [patch["target"] for patch in metadata["patches"]] +
        metadata["files"] +
        [f"mods/{name}" for name in metadata.get("native", [])])

    for target in targets:
        other = other_owner(ledger, target, mod_id)
        if other:
            raise SystemExit(
                f"{target}\n  is already patched by {other!r}.\n"
                f"Remove it first: python install.py remove {other}")
    loader = metadata.get("loader")
    if loader:
        for installed_id, record in ledger.items():
            installed_loader = record.get("loader")
            if (installed_id != mod_id and installed_loader and
                    installed_loader.get("id", "").lower() ==
                    loader["id"].lower()):
                raise SystemExit(
                    f"loader id {loader['id']!r} is already owned by "
                    f"{installed_id!r}")

    report(0.92)
    if loader or metadata.get("native"):
        ensure_native_bootstrap()
    modpack.apply(archive)
    if loader:
        configure_loader(loader)
    report(1.0)
    ledger[mod_id] = {
        "version": metadata["version"],
        "artifact_sha256": artifact_info["sha256"],
        "targets": targets,
        "loader": loader,
    }
    save_ledger(ledger)
    print(f"installed {mod_id} {metadata['version']} ({len(targets)} file(s))")


def update(mod_id: str) -> None:
    """Replace a mod without uninstalling the working version up front.

    modpack applies patches from the pristine backup, so the new artifact can
    safely overwrite files owned by the same mod. Only after that succeeds are
    targets which disappeared from the new version restored/removed. A failed
    download or hash check therefore leaves the installed version in place.
    """
    ledger = load_ledger()
    previous = ledger.get(mod_id)
    if previous is None:
        install(mod_id)
        return

    install(mod_id)
    current = load_ledger()[mod_id]
    _remove_targets(previous, keep=set(current["targets"]))

    old_loader = previous.get("loader")
    new_loader = current.get("loader")
    if (old_loader and
            (not new_loader or old_loader.get("id", "").lower() !=
             new_loader.get("id", "").lower())):
        remove_loader_config(old_loader)


def _remove_targets(record: dict, keep: set[str] | None = None) -> None:
    keep = keep or set()
    for target in record["targets"]:
        if target in keep:
            continue
        if target.startswith("mods/"):
            native = HERE.parent / target
            if native.exists():
                native.unlink()
                print(f"removed native mod {target}")
            continue
        _, backup = patch_dvpl._slots(target)
        if backup.exists():
            patch_dvpl.restore(target)
        else:
            live, _ = patch_dvpl._slots(target)
            if live.exists():
                live.unlink()
                print(f"removed added file {target}")


def remove(mod_id: str) -> None:
    ledger = load_ledger()
    if mod_id not in ledger:
        raise SystemExit(f"{mod_id} is not installed")
    record = ledger[mod_id]

    _remove_targets(record)

    if record.get("loader"):
        remove_loader_config(record["loader"])

    del ledger[mod_id]
    save_ledger(ledger)
    print(f"removed {mod_id}")


# Sanity cap for the tombstone reconciliation below: refuse the pass entirely
# if it would take out every mod a user has, and they have more than one.
#
# The case against a cap: a wipe can be legitimate. A client patch breaks four
# mods at once, all four are withdrawn, and the cap is then refusing exactly
# the situation the feature exists for - it holds broken mods in place until
# somebody reads the agent's output, which nobody does. It is also no defence
# against a hostile response: /api/mods is unauthenticated and now tells
# clients what to delete, and anyone able to serve that response can simply
# spare one id and delete the rest. A cap that an attacker steps over in one
# line, while blocking honest mass-withdrawals, is worse than nothing.
#
# The case for it, which is the one taken here: the failure this actually
# guards is not an attacker, it is a bad deploy. A migration that tombstones
# every row, a `removed` list accidentally computed as "every id not currently
# approved" - the plausible server faults produce a blanket list, not a
# carefully-chosen one, and the cap catches the shape of the mistake. It costs
# nothing in the common case, because the common case is one or two mods
# installed and a single tombstone; total wipes of a multi-mod install are rare
# enough that being wrong about one and telling the user beats being right
# about it and silently emptying their game. And the client is where the last
# check has to live, because the server just told it to delete things and the
# server is the thing that might be wrong. Note that anyone controlling the
# response controls the deletions regardless: only signed tombstones or an
# in-game confirmation would change that, and neither exists yet.
#
# It refuses rather than trimming the list because there is no honest way to
# choose which of a wrong list to obey.
def _pending_removals(tombstoned: list[str] | None, source: str,
                      ledger: dict) -> list[str]:
    """Installed ids the registry has reported deleted. Decides, touches nothing.

    Three rules, each of which is the difference between this feature and a
    remote wipe of somebody's install:

    1. Only a fresh, successful read decides anything. `source` is 'cache' when
       the fetch failed and the disk copy was used, and 'empty' when there was
       nothing to read at all. The cached payload carries whatever `removed`
       list was current when it was written, so acting on it would let a DNS
       failure or a week offline replay an old deletion list - and a rollback
       that un-deletes a mod would never reach the client that already went
       offline. A network outage must cost the user nothing.
    2. `tombstoned` is None when the server said nothing about removals at all
       (see registry.tombstones), which is not the same as saying nothing is
       deleted, and is not permission to act.
    3. The removal set is intersected with the ledger, and only ever with the
       ledger. It is never inferred from a mod being missing from `mods`: the
       server publishes approved entries there, so a mod pulled for re-review
       disappears from that list while remaining installed and perfectly valid.
       Distinguishing "withdrawn from the shelf" from "deleted" is the entire
       reason the tombstone list is published separately, so treating absence
       as deletion would throw away the only fact worth having.
    """
    if source != "registry":
        return []
    if tombstoned is None:
        return []
    # An empty list intersects to nothing on its own; it needs no special case
    # and must not be given one.
    return sorted(set(tombstoned) & set(ledger))


def apply_removals(tombstoned: list[str] | None, source: str) -> list[str]:
    """Uninstall what the registry says was deleted. Returns what actually went.

    Called while the game client is closed - the only moment its files can be
    touched - so a mod vanishing from the user's install is announced here on
    stdout, which is the agent's output and all the user ever sees of it.
    """
    ledger = load_ledger()
    doomed = _pending_removals(tombstoned, source, ledger)
    if len(doomed) > 1 and len(doomed) == len(ledger):
        print(f"registry says all {len(ledger)} installed mods were deleted; "
              "refusing to act on that")
        print("  if it is really correct, remove them by hand: "
              f"python install.py remove {doomed[0]}")
        return []

    uninstalled = []
    for mod_id in doomed:
        print(f"{mod_id} was deleted from the registry; uninstalling")
        try:
            remove(mod_id)
        except SystemExit as error:
            # One mod that cannot be un-patched - a file the client still holds
            # open, a missing backup - must not strand the rest of the list.
            print(f"  could not remove {mod_id}: {' '.join(str(error).split())}")
            continue
        except Exception as error:
            print(f"  could not remove {mod_id}: {type(error).__name__}: "
                  f"{' '.join(str(error).split())}")
            continue
        uninstalled.append(mod_id)
    return uninstalled


def show() -> None:
    ledger = load_ledger()
    mods, source = registry.fetch()
    print(f"registry: {source}   installed: {len(ledger)}\n")
    for mod in mods:
        record = ledger.get(mod["id"])
        if mod["id"] in ledger:
            state = f"installed {record['version']}"
        elif mod.get("artifact"):
            state = "available"
        else:
            state = "no artifact"
        print(f"  {mod['id']:16} {mod['version']:8} {mod['type']:9} {state}")

    unknown = set(ledger) - {mod["id"] for mod in mods}
    for mod_id in sorted(unknown):
        print(f"  {mod_id:16} {ledger[mod_id]['version']:8} {'?':9} "
              f"installed, no longer in the registry")


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    command = sys.argv[1]
    if command == "list":
        show()
    elif command == "install":
        install(sys.argv[2])
    elif command == "remove":
        remove(sys.argv[2])
    elif command == "update":
        update(sys.argv[2])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
