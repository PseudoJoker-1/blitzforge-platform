"""Minimal PE reader and scanning primitives for re-anchoring.

Deliberately dependency-free: re-anchoring has to work on a machine that has
nothing installed but Python, right after a game patch lands. Only the parts
of PE32 the anchors actually need are parsed.
"""

from __future__ import annotations

import hashlib
import struct
from dataclasses import dataclass


@dataclass(frozen=True)
class Section:
    name: str
    virtual_address: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int

    @property
    def is_code(self) -> bool:
        return bool(self.characteristics & 0x00000020)  # CNT_CODE

    @property
    def is_executable(self) -> bool:
        return bool(self.characteristics & 0x20000000)  # MEM_EXECUTE

    def contains_rva(self, rva: int) -> bool:
        return self.virtual_address <= rva < self.virtual_address + max(
            self.virtual_size, self.raw_size
        )


class PEImage:
    """A PE32 executable mapped flat, addressed by RVA.

    Every address in this module is an RVA (relative to the image base), which
    is what the binding pack stores. `va()` converts to the absolute address a
    disassembler would show.
    """

    def __init__(self, path: str) -> None:
        self.path = path
        with open(path, "rb") as handle:
            self.data = handle.read()
        self._parse()

    def _parse(self) -> None:
        data = self.data
        if data[:2] != b"MZ":
            raise ValueError(f"{self.path}: not an MZ image")
        pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe_offset : pe_offset + 4] != b"PE\0\0":
            raise ValueError(f"{self.path}: missing PE signature")

        coff = pe_offset + 4
        machine, number_of_sections = struct.unpack_from("<HH", data, coff)
        optional_size = struct.unpack_from("<H", data, coff + 16)[0]
        if machine != 0x014C:
            raise ValueError(
                f"{self.path}: expected i386 (0x014c), got 0x{machine:04x}"
            )

        optional = coff + 20
        magic = struct.unpack_from("<H", data, optional)[0]
        if magic != 0x10B:
            raise ValueError(
                f"{self.path}: expected PE32 optional header, got 0x{magic:04x}"
            )
        self.image_base = struct.unpack_from("<I", data, optional + 28)[0]
        self.size_of_image = struct.unpack_from("<I", data, optional + 56)[0]

        section_table = optional + optional_size
        self.sections: list[Section] = []
        for index in range(number_of_sections):
            entry = section_table + index * 40
            raw_name = data[entry : entry + 8].rstrip(b"\0")
            (
                virtual_size,
                virtual_address,
                raw_size,
                raw_offset,
            ) = struct.unpack_from("<IIII", data, entry + 8)
            characteristics = struct.unpack_from("<I", data, entry + 36)[0]
            self.sections.append(
                Section(
                    name=raw_name.decode("ascii", "replace"),
                    virtual_address=virtual_address,
                    virtual_size=virtual_size,
                    raw_offset=raw_offset,
                    raw_size=raw_size,
                    characteristics=characteristics,
                )
            )

    # -- identity ---------------------------------------------------------

    def sha256(self) -> str:
        return hashlib.sha256(self.data).hexdigest()

    # -- address translation ----------------------------------------------

    def va(self, rva: int) -> int:
        return self.image_base + rva

    def rva_from_va(self, va: int) -> int:
        return va - self.image_base

    def section_for_rva(self, rva: int) -> Section | None:
        for section in self.sections:
            if section.contains_rva(rva):
                return section
        return None

    def offset_for_rva(self, rva: int) -> int | None:
        section = self.section_for_rva(rva)
        if section is None:
            return None
        delta = rva - section.virtual_address
        if delta >= section.raw_size:
            return None  # lives in the zero-filled tail, no file bytes
        return section.raw_offset + delta

    # -- reading -----------------------------------------------------------

    def read(self, rva: int, size: int) -> bytes | None:
        offset = self.offset_for_rva(rva)
        if offset is None or offset + size > len(self.data):
            return None
        return self.data[offset : offset + size]

    def read_u32(self, rva: int) -> int | None:
        raw = self.read(rva, 4)
        return None if raw is None else struct.unpack("<I", raw)[0]

    def read_cstring(self, rva: int, limit: int = 512) -> str | None:
        offset = self.offset_for_rva(rva)
        if offset is None:
            return None
        end = self.data.find(b"\0", offset, offset + limit)
        if end < 0:
            return None
        try:
            return self.data[offset:end].decode("ascii")
        except UnicodeDecodeError:
            return None

    # -- scanning ----------------------------------------------------------

    def code_sections(self) -> list[Section]:
        return [s for s in self.sections if s.is_code or s.is_executable]

    def find_bytes(
        self,
        needle: bytes,
        sections: list[Section] | None = None,
        limit: int = 0,
    ) -> list[int]:
        """Return RVAs of every occurrence of `needle`."""
        results: list[int] = []
        for section in sections if sections is not None else self.sections:
            blob = self.data[
                section.raw_offset : section.raw_offset + section.raw_size
            ]
            start = 0
            while True:
                hit = blob.find(needle, start)
                if hit < 0:
                    break
                results.append(section.virtual_address + hit)
                if limit and len(results) >= limit:
                    return results
                start = hit + 1
        return results

    def find_masked(
        self,
        pattern: bytes,
        mask: bytes,
        sections: list[Section] | None = None,
        limit: int = 0,
    ) -> list[int]:
        """Masked search. `mask` is b'x' for "must match", anything else wild."""
        if len(pattern) != len(mask):
            raise ValueError("pattern and mask length differ")
        anchor_index = mask.find(b"x"[0])
        if anchor_index < 0:
            raise ValueError("mask must fix at least one byte")
        anchor_byte = pattern[anchor_index : anchor_index + 1]

        results: list[int] = []
        for section in sections if sections is not None else self.sections:
            blob = self.data[
                section.raw_offset : section.raw_offset + section.raw_size
            ]
            start = 0
            span = len(pattern)
            while True:
                hit = blob.find(anchor_byte, start)
                if hit < 0:
                    break
                begin = hit - anchor_index
                start = hit + 1
                if begin < 0 or begin + span > len(blob):
                    continue
                window = blob[begin : begin + span]
                if all(
                    mask[i] != 0x78 or window[i] == pattern[i]
                    for i in range(span)
                ):
                    results.append(section.virtual_address + begin)
                    if limit and len(results) >= limit:
                        return results
        return results

    def find_absolute_refs(
        self,
        target_rva: int,
        sections: list[Section] | None = None,
    ) -> list[int]:
        """RVAs of 32-bit absolute references to `target_rva`.

        On x86 a string or vtable reference is a plain little-endian VA
        embedded in the instruction stream (``push offset s`` / ``mov r32,
        offset s``), so a raw dword scan finds them without a disassembler.
        """
        encoded = struct.pack("<I", self.va(target_rva))
        return self.find_bytes(
            encoded, sections if sections is not None else self.code_sections()
        )

    def find_relative_calls(self, target_rva: int) -> list[int]:
        """RVAs of ``call rel32`` instructions targeting `target_rva`."""
        results: list[int] = []
        for section in self.code_sections():
            blob = self.data[
                section.raw_offset : section.raw_offset + section.raw_size
            ]
            start = 0
            while True:
                hit = blob.find(b"\xe8", start)
                if hit < 0 or hit + 5 > len(blob):
                    break
                start = hit + 1
                displacement = struct.unpack_from("<i", blob, hit + 1)[0]
                site = section.virtual_address + hit
                if site + 5 + displacement == target_rva:
                    results.append(site)
        return results
