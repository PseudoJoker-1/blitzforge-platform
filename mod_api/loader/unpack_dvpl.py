#!/usr/bin/env python3
"""Unpack one DAVA LitePack (.dvpl) file for the local live test."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path


FOOTER = struct.Struct("<IIII4s")
MARKER = b"DVPL"
COMPRESSION_NONE = 0
COMPRESSION_LZ4 = 1
COMPRESSION_LZ4HC = 2


def unpack(source: Path, destination: Path) -> None:
    packed = source.read_bytes()
    if len(packed) < FOOTER.size:
        raise ValueError(f"{source} is smaller than a DVPL footer")

    (
        uncompressed_size,
        compressed_size,
        _crc32,
        compression_type,
        marker,
    ) = FOOTER.unpack_from(packed, len(packed) - FOOTER.size)
    if marker != MARKER:
        raise ValueError(f"{source} has no DVPL marker")
    payload = packed[:compressed_size]

    if compression_type == COMPRESSION_NONE:
        unpacked = payload
    elif compression_type in (COMPRESSION_LZ4, COMPRESSION_LZ4HC):
        try:
            import lz4.block
        except ImportError as error:
            raise RuntimeError(
                "Python package 'lz4' is required for the live SC2 fixture"
            ) from error
        unpacked = lz4.block.decompress(
            payload, uncompressed_size=uncompressed_size
        )
    else:
        raise ValueError(
            f"{source} uses unsupported compression type {compression_type}"
        )

    if len(unpacked) != uncompressed_size:
        raise ValueError(
            f"{source} unpacked to {len(unpacked)} bytes, "
            f"expected {uncompressed_size}"
        )
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(unpacked)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    unpack(args.source.resolve(), args.destination.resolve())


if __name__ == "__main__":
    main()
