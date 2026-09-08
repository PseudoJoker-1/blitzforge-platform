"""DVPL containers (the game's packed resource files) in pure Python.

Every file under the client's `Data/` folder is stored as `<name>.dvpl`: the
payload, optionally LZ4-compressed, followed by a 20-byte footer

    uint32 original_size
    uint32 compressed_size
    uint32 crc32            of the payload as stored
    uint32 type             0 = stored, 1 = lz4, 2 = lz4hc, 3 = deflate
    char[4] "DVPL"

A resource package replaces such files, so the player's CLI has to read the
stock file (to check it really is the stock one) and write the new one. The
`lz4` extension module is used when it is installed; otherwise the block
format is decoded and encoded here, which is fast enough for shaders, YAML
and small textures and only slow for multi-megabyte files.
"""

from __future__ import annotations

import struct
import zlib

try:  # optional accelerator, same results
    import lz4.block as _lz4  # type: ignore
except Exception:  # pragma: no cover - depends on the environment
    _lz4 = None

MAGIC = b"DVPL"
FOOTER = struct.Struct("<IIII4s")
FOOTER_SIZE = FOOTER.size
TYPE_STORED, TYPE_LZ4, TYPE_LZ4HC, TYPE_DEFLATE = 0, 1, 2, 3

# LZ4 block format rules (from lz4.c): a match may not start within the last
# 12 bytes and the block always ends with at least 5 literal bytes.
_MIN_MATCH = 4
_MF_LIMIT = 12
_LAST_LITERALS = 5
_MAX_DISTANCE = 65535


class DvplError(ValueError):
    """The bytes are not a DVPL container this module can read."""


def _crc(payload: bytes) -> int:
    return zlib.crc32(payload) & 0xFFFFFFFF


def is_dvpl(data: bytes) -> bool:
    return len(data) >= FOOTER_SIZE and data[-4:] == MAGIC


def unpack(data: bytes) -> bytes:
    """The original file bytes, or DvplError when the footer, CRC or size lie."""
    if len(data) < FOOTER_SIZE:
        raise DvplError("file is shorter than a DVPL footer")
    original_size, compressed_size, crc, kind, magic = FOOTER.unpack(data[-FOOTER_SIZE:])
    if magic != MAGIC:
        raise DvplError("missing DVPL magic")
    payload = data[:-FOOTER_SIZE]
    if len(payload) != compressed_size:
        raise DvplError("compressed size does not match the payload")
    if _crc(payload) != crc:
        raise DvplError("payload CRC mismatch")
    if kind == TYPE_STORED:
        out = payload
    elif kind in (TYPE_LZ4, TYPE_LZ4HC):
        out = lz4_block_decompress(payload, original_size)
    elif kind == TYPE_DEFLATE:
        try:
            out = zlib.decompress(payload, -15)
        except zlib.error as exc:
            raise DvplError(f"deflate payload is damaged: {exc}") from exc
    else:
        raise DvplError(f"unknown compression type {kind}")
    if len(out) != original_size:
        raise DvplError("decoded size does not match the footer")
    return out


def pack(data: bytes, *, compress: bool = True) -> bytes:
    """A DVPL container for `data`: LZ4 when it helps, stored otherwise."""
    kind = TYPE_STORED
    payload = data
    if compress and len(data) > _MF_LIMIT:
        candidate = lz4_block_compress(data)
        if len(candidate) < len(data):
            payload, kind = candidate, TYPE_LZ4
    return payload + FOOTER.pack(len(data), len(payload), _crc(payload), kind, MAGIC)


# --- LZ4 block format ------------------------------------------------------------------

def lz4_block_decompress(src: bytes, expected_size: int) -> bytes:
    if _lz4 is not None:
        try:
            return _lz4.decompress(src, uncompressed_size=expected_size)
        except Exception as exc:
            raise DvplError(f"lz4 payload is damaged: {exc}") from exc
    out = bytearray()
    i = 0
    n = len(src)
    try:
        while i < n:
            token = src[i]
            i += 1
            literals = token >> 4
            if literals == 15:
                while True:
                    extra = src[i]
                    i += 1
                    literals += extra
                    if extra != 255:
                        break
            if i + literals > n:
                raise DvplError("literal run overruns the payload")
            out += src[i:i + literals]
            i += literals
            if i >= n:
                break  # the block ends with literals
            offset = src[i] | (src[i + 1] << 8)
            i += 2
            if offset == 0 or offset > len(out):
                raise DvplError("match offset points outside the output")
            length = token & 15
            if length == 15:
                while True:
                    extra = src[i]
                    i += 1
                    length += extra
                    if extra != 255:
                        break
            length += _MIN_MATCH
            start = len(out) - offset
            if offset >= length:
                out += out[start:start + length]
            else:  # overlapping match: the pattern repeats
                for k in range(length):
                    out.append(out[start + k])
    except IndexError as exc:
        raise DvplError("payload ends in the middle of a sequence") from exc
    if len(out) != expected_size:
        raise DvplError("decoded size does not match the footer")
    return bytes(out)


def _append_length(out: bytearray, value: int) -> None:
    while value >= 255:
        out.append(255)
        value -= 255
    out.append(value)


def lz4_block_compress(data: bytes) -> bytes:
    """A valid LZ4 block for `data` (greedy parse over a 4-byte hash table)."""
    if _lz4 is not None:
        return _lz4.compress(data, mode="high_compression", store_size=False)
    n = len(data)
    out = bytearray()
    anchor = 0
    if n > _MF_LIMIT:
        table: dict[bytes, int] = {}
        match_start_limit = n - _MF_LIMIT
        match_end_limit = n - _LAST_LITERALS
        ip = 0
        while ip < match_start_limit:
            key = data[ip:ip + _MIN_MATCH]
            ref = table.get(key)
            table[key] = ip
            if ref is None or ip - ref > _MAX_DISTANCE:
                ip += 1
                continue
            length = _MIN_MATCH
            while ip + length < match_end_limit and data[ref + length] == data[ip + length]:
                length += 1
            literals = ip - anchor
            token_at = len(out)
            out.append(min(literals, 15) << 4)
            if literals >= 15:
                _append_length(out, literals - 15)
            out += data[anchor:ip]
            offset = ip - ref
            out.append(offset & 0xFF)
            out.append(offset >> 8)
            extra = length - _MIN_MATCH
            if extra >= 15:
                out[token_at] |= 15
                _append_length(out, extra - 15)
            else:
                out[token_at] |= extra
            ip += length
            anchor = ip
    literals = n - anchor
    token_at = len(out)
    out.append(min(literals, 15) << 4)
    if literals >= 15:
        _append_length(out, literals - 15)
    out += data[anchor:]
    return bytes(out)
