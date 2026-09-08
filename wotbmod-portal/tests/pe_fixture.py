"""A hand-built minimal PE image for scanner tests: chosen imports, optional
writable+executable section, 32 or 64 bit. No dependency on the CLI, so any
test may import it without loading tools/wotbmod.py a second time."""

from __future__ import annotations

import struct


def build_pe(imports: dict[str, list[str]], *, bits: int = 32, wx: bool = False, extra: bytes = b"") -> bytes:
    """A minimal PE image with one section holding the import directory and strings."""
    section_rva = 0x1000
    section_raw = 0x400
    body = bytearray()
    descriptors_at = 0
    # layout: descriptors | thunks | names | dll names ; all RVAs relative to section_rva
    dll_names = list(imports)
    descriptor_size = 20 * (len(dll_names) + 1)
    thunk_entry = 8 if bits == 64 else 4
    thunk_tables: list[tuple[int, list[str]]] = []
    offset = descriptor_size
    thunk_offsets = []
    for dll in dll_names:
        thunk_offsets.append(offset)
        offset += thunk_entry * (len(imports[dll]) + 1)
    name_blob = bytearray()
    name_positions: dict[tuple[str, str], int] = {}
    dll_positions: dict[str, int] = {}
    for dll in dll_names:
        for name in imports[dll]:
            name_positions[(dll, name)] = offset + len(name_blob)
            name_blob += b"\0\0" + name.encode("ascii") + b"\0"
        dll_positions[dll] = offset + len(name_blob)
        name_blob += dll.encode("ascii") + b"\0"
    body = bytearray(offset) + name_blob + extra
    for index, dll in enumerate(dll_names):
        struct.pack_into("<IIIII", body, index * 20, section_rva + thunk_offsets[index], 0, 0,
                         section_rva + dll_positions[dll], section_rva + thunk_offsets[index])
        for j, name in enumerate(imports[dll]):
            value = section_rva + name_positions[(dll, name)]
            struct.pack_into("<Q" if bits == 64 else "<I", body, thunk_offsets[index] + j * thunk_entry, value)
    section_size = (len(body) + 0x1FF) & ~0x1FF
    body += b"\0" * (section_size - len(body))
    dos = bytearray(0x40)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    coff = struct.pack("<HHIIIHH", 0x8664 if bits == 64 else 0x14C, 1, 0, 0, 0, 240 if bits == 64 else 224, 0x2102)
    if bits == 64:
        optional = bytearray(240)
        struct.pack_into("<H", optional, 0, 0x20B)
        directories_at = 112
    else:
        optional = bytearray(224)
        struct.pack_into("<H", optional, 0, 0x10B)
        directories_at = 96
    struct.pack_into("<I", optional, directories_at - 4, 16)
    struct.pack_into("<II", optional, directories_at + 8, section_rva, descriptor_size)
    flags = 0x60000020 | (0x80000000 if wx else 0)
    section = bytearray(40)
    section[:5] = b".text"
    struct.pack_into("<IIII", section, 8, len(body), section_rva, len(body), section_raw)
    struct.pack_into("<I", section, 36, flags)
    header = dos + b"PE\0\0" + coff + bytes(optional) + bytes(section)
    header += b"\0" * (section_raw - len(header))
    return bytes(header) + bytes(body)
