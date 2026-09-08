"""Copy the SDK modules the portal needs into backend/vendor/.

    python tools/sync_sdk.py          # copy from ../mod_api/tools and record hashes
    python tools/sync_sdk.py --check  # exit 1 when vendor/ is behind the SDK

The portal validates uploads with the same code the player's CLI uses
(`wotbmod.py`, `wotbmod_packages.py`, `wotbmod_trust.py`, `wotbmod_scan.py`).
When this folder sits next to the SDK checkout the server imports them from
there; a copy uploaded to a host has only `backend/vendor/`, which this script
keeps identical. `vendor/MANIFEST.json` records the SHA-256 of each copy so a
stale vendor is noticed rather than trusted.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import sys
from pathlib import Path

PORTAL_ROOT = Path(__file__).resolve().parents[1]
SDK_TOOLS = PORTAL_ROOT.parent / "mod_api" / "tools"
VENDOR = PORTAL_ROOT / "backend" / "vendor"
MODULES = ("wotbmod.py", "wotbmod_packages.py", "wotbmod_trust.py", "wotbmod_scan.py", "wotbmod_dvpl.py")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main(argv: list[str]) -> int:
    check = "--check" in argv
    if not SDK_TOOLS.is_dir():
        print(f"SDK tools not found at {SDK_TOOLS}; nothing to sync", file=sys.stderr)
        return 1
    VENDOR.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, str] = {}
    behind = []
    for name in MODULES:
        source = SDK_TOOLS / name
        target = VENDOR / name
        manifest[name] = digest(source)
        if check:
            if not target.is_file() or digest(target) != manifest[name]:
                behind.append(name)
            continue
        shutil.copyfile(source, target)
    if check:
        if behind:
            print("vendor is behind the SDK: " + ", ".join(behind) + " (run tools/sync_sdk.py)")
            return 1
        print("vendor matches the SDK")
        return 0
    (VENDOR / "MANIFEST.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (VENDOR / "README.md").write_text(
        "Copies of mod_api/tools modules made by tools/sync_sdk.py; do not edit here.\n"
        "The server imports ../mod_api/tools when it exists and this folder otherwise.\n", encoding="utf-8")
    print("vendored: " + ", ".join(MODULES))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
