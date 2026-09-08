from __future__ import annotations

import argparse
import struct
from pathlib import Path


def build_dds(width: int, height: int) -> bytes:
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")

    dds_magic = b"DDS "
    header_size = 124
    flags = 0x0000100F
    pitch = width * 4
    depth = 0
    mipmaps = 0
    reserved1 = [0] * 11

    pixel_format = struct.pack(
        "<8I",
        32,
        0x00000041,
        0,
        32,
        0x00FF0000,
        0x0000FF00,
        0x000000FF,
        0xFF000000,
    )
    caps = 0x00001000
    header = struct.pack(
        "<7I11I",
        header_size,
        flags,
        height,
        width,
        pitch,
        depth,
        mipmaps,
        *reserved1,
    )
    header += pixel_format
    header += struct.pack("<5I", caps, 0, 0, 0, 0)

    pixels = bytearray()
    tile = max(8, width // 8)
    for y in range(height):
        for x in range(width):
            cyan = ((x // tile) + (y // tile)) % 2 == 0
            if cyan:
                pixels.extend((0xFF, 0xFF, 0x00, 0xFF))
            else:
                pixels.extend((0xFF, 0x00, 0xFF, 0xFF))
    return dds_magic + header + bytes(pixels)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate a visible BGRA8 checkerboard DDS fixture."
    )
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=256)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(build_dds(args.width, args.height))
    print(f"generated {args.output} ({args.width}x{args.height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
