from __future__ import annotations

import pathlib
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_probe_archive.py OUTPUT.zip")
    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    info = zipfile.ZipInfo("probe.txt", (2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    with zipfile.ZipFile(output, "w") as archive:
        archive.writestr(info, b"blitzforge-native-archive-probe\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
