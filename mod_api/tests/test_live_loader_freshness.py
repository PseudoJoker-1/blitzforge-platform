#!/usr/bin/env python3
"""Reject a live loader linked against stale private DAVA runtime objects."""

from __future__ import annotations

from pathlib import Path


MOD_API = Path(__file__).resolve().parents[1]
SOURCE = MOD_API / "src" / "wotb_mod_dava_resources.cpp"
RUNTIME = MOD_API / "build" / "wotb_mod_runtime.lib"
LOADER = MOD_API / "build" / "wotb_mod_loader.dll"


def main() -> int:
    for path in (SOURCE, RUNTIME, LOADER):
        if not path.is_file():
            raise AssertionError(f"required live-build artifact is missing: {path}")

    source_time = SOURCE.stat().st_mtime_ns
    if RUNTIME.stat().st_mtime_ns < source_time:
        raise AssertionError(
            "wotb_mod_runtime.lib predates wotb_mod_dava_resources.cpp; "
            "run build.cmd before loader\\build_live.cmd"
        )
    if LOADER.stat().st_mtime_ns < RUNTIME.stat().st_mtime_ns:
        raise AssertionError("wotb_mod_loader.dll predates wotb_mod_runtime.lib")

    image = LOADER.read_bytes()
    required = (
        b"root-readable=%u",
        b"~res:/",
        b"Data\\3d\\Tanks\\USSR\\T-34-85.sc2.dvpl",
        b"Data\\3d\\Tanks\\USA\\A124_T54E2.sc2.dvpl",
    )
    forbidden = (b"dispatch-readable=", b"root-valid=")
    for marker in required:
        if marker not in image:
            raise AssertionError(f"fresh live-loader marker is missing: {marker!r}")
    for marker in forbidden:
        if marker in image:
            raise AssertionError(f"stale live-loader marker is present: {marker!r}")

    print("PASS: live loader contains the current private DAVA runtime")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
