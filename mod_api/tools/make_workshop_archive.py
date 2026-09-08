from __future__ import annotations

import os
import pathlib
import sys
import zipfile


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: make_workshop_archive.py INPUT OUTPUT.zip")

    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    if not source.is_file():
        raise SystemExit(f"workshop archive input is missing: {source}")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.unlink(missing_ok=True)

    info = zipfile.ZipInfo(source.name, (2026, 1, 1, 0, 0, 0))
    info.compress_type = zipfile.ZIP_STORED
    info.external_attr = 0o644 << 16
    with zipfile.ZipFile(temporary, "w") as archive:
        archive.writestr(info, source.read_bytes())
    os.replace(temporary, output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
