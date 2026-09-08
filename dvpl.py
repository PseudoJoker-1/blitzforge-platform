"""DVPL (Wargaming) packer/unpacker.

Footer (20 bytes at end of file, little-endian):
    uint32 original_size
    uint32 compressed_size
    uint32 crc32 (of compressed payload)
    uint32 type (0=none, 1=lz4, 2=lz4hc, 3=rfc1951)
    char[4] magic "DVPL"
"""
import struct, sys, zlib, os
import lz4.block

MAGIC = b"DVPL"
FOOTER = struct.Struct("<IIII4s")

def unpack(data: bytes) -> bytes:
    if len(data) < FOOTER.size:
        raise ValueError("file too small")
    osz, csz, crc, typ, magic = FOOTER.unpack(data[-FOOTER.size:])
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic!r}")
    payload = data[:-FOOTER.size]
    if len(payload) != csz:
        raise ValueError("compressed size mismatch")
    if zlib.crc32(payload) != crc:
        raise ValueError("crc mismatch")
    if typ == 0:
        out = payload
    elif typ in (1, 2):
        out = lz4.block.decompress(payload, uncompressed_size=osz)
    else:
        raise ValueError(f"unsupported type {typ}")
    if len(out) != osz:
        raise ValueError("original size mismatch")
    return out

def pack(data: bytes, compress: bool = True) -> bytes:
    osz = len(data)
    if compress:
        payload = lz4.block.compress(data, mode="high_compression", store_size=False)
        # if compressed isn't smaller, store raw
        if len(payload) >= osz:
            payload = data
            typ = 0
        else:
            typ = 2
    else:
        payload = data
        typ = 0
    csz = len(payload)
    crc = zlib.crc32(payload)
    return payload + FOOTER.pack(osz, csz, crc, typ, MAGIC)

def main():
    if len(sys.argv) < 3:
        print("usage: dvpl.py [unpack|pack] <file_in> [file_out]")
        sys.exit(1)
    mode, fin = sys.argv[1], sys.argv[2]
    if mode == "unpack":
        fout = sys.argv[3] if len(sys.argv) > 3 else fin[:-5] if fin.endswith(".dvpl") else fin + ".out"
        with open(fin, "rb") as f: data = f.read()
        with open(fout, "wb") as f: f.write(unpack(data))
        print(f"unpacked -> {fout}")
    elif mode == "pack":
        fout = sys.argv[3] if len(sys.argv) > 3 else fin + ".dvpl"
        with open(fin, "rb") as f: data = f.read()
        with open(fout, "wb") as f: f.write(pack(data))
        print(f"packed -> {fout}")
    else:
        print("unknown mode")
        sys.exit(1)

if __name__ == "__main__":
    main()
