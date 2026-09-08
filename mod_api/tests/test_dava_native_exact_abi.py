"""Falsifiable checks for the exact-build private DAVA ABI.

These checks intentionally read the installed client when it is available.
The fixed RVAs are safe only for that one executable fingerprint, and a C++
typedef by itself cannot prove stack cleanup or FastName ownership semantics.
"""

from __future__ import annotations

import hashlib
import re
import struct
import sys
from pathlib import Path


MOD_API = Path(__file__).resolve().parents[1]
GAME_EXE = Path(__file__).resolve().parents[3] / "wotblitz.exe"
# Byte pins below belong to client 11.20.0.887 (re-pinned 2026-09-03 from the 11.19.0.834 set).
EXPECTED_SHA256 = (
    "4813544d3d6b9f45a357e87f5a14d065bd108c4acd324ac1506cb6d1ad6ff0af"
)


class PeImage:
    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise AssertionError("client is not a PE image")
        pe_offset = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_offset : pe_offset + 4] != b"PE\0\0":
            raise AssertionError("client has no PE signature")
        machine, section_count = struct.unpack_from(
            "<HH", self.data, pe_offset + 4
        )
        if machine != 0x014C:
            raise AssertionError(f"expected i386 client, got machine={machine:#x}")
        optional_size = struct.unpack_from("<H", self.data, pe_offset + 20)[0]
        optional = pe_offset + 24
        magic = struct.unpack_from("<H", self.data, optional)[0]
        if magic != 0x010B:
            raise AssertionError(f"expected PE32 client, got magic={magic:#x}")
        self.image_base = struct.unpack_from("<I", self.data, optional + 28)[0]
        section_table = optional + optional_size
        self.sections: list[tuple[int, int, int, int]] = []
        for index in range(section_count):
            header = section_table + index * 40
            virtual_size, virtual_address, raw_size, raw_pointer = (
                struct.unpack_from("<IIII", self.data, header + 8)
            )
            self.sections.append(
                (virtual_address, virtual_size, raw_pointer, raw_size)
            )

    def at_rva(self, rva: int, size: int) -> bytes:
        for virtual_address, virtual_size, raw_pointer, raw_size in self.sections:
            mapped_size = max(virtual_size, raw_size)
            if virtual_address <= rva and rva + size <= virtual_address + mapped_size:
                offset = raw_pointer + rva - virtual_address
                return self.data[offset : offset + size]
        raise AssertionError(f"RVA {rva:#x} is outside mapped sections")


def require_bytes(image: PeImage, rva: int, expected: bytes, reason: str) -> None:
    actual = image.at_rva(rva, len(expected))
    if actual != expected:
        raise AssertionError(
            f"{reason}: RVA {rva:#x} expected {expected.hex(' ')}, "
            f"got {actual.hex(' ')}"
        )


def verify_client_abi() -> None:
    digest = hashlib.sha256(GAME_EXE.read_bytes()).hexdigest()
    if digest != EXPECTED_SHA256:
        raise AssertionError(
            f"client fingerprint mismatch: expected {EXPECTED_SHA256}, got {digest}"
        )
    image = PeImage(GAME_EXE)
    if image.image_base != 0x00400000:
        raise AssertionError(f"unexpected image base {image.image_base:#x}")

    # NMaterial::AddProperty reads [ebp+18h], proving five stack arguments,
    # and returns with ret 14h. The old four-argument typedef corrupted ESP.
    require_bytes(
        image,
        0x008EE738,
        bytes.fromhex("8b 4d 08 8b 09"),
        "AddProperty must take FastName by const reference",
    )
    require_bytes(
        image,
        0x008EE74C,
        bytes.fromhex("8b 45 18"),
        "AddProperty must read its fifth stack argument",
    )
    require_bytes(
        image,
        0x008EE849,
        bytes.fromhex("c2 14 00"),
        "AddProperty must pop five arguments",
    )

    # HasProperty and SetProperty forward the address of a FastName object to
    # the material map lookup. The lookup then dereferences that address to
    # obtain the interned identity stored in the map key.
    require_bytes(
        image,
        0x0090766A,
        bytes.fromhex("ff 75 08"),
        "HasProperty must forward the FastName address",
    )
    require_bytes(
        image,
        0x008DAE96,
        bytes.fromhex("8b 45 10 8b 75 0c 8b ce"),
        "material lookup must load the FastName address argument",
    )
    require_bytes(
        image,
        0x008DAEA5,
        bytes.fromhex("ff 30 50"),
        "material lookup must dereference const FastName before lookup",
    )
    require_bytes(
        image,
        0x00907694,
        bytes.fromhex("8b 4d 08 85 c9 74 0c"),
        "HasProperty must release the found property RefPtr",
    )
    require_bytes(
        image,
        0x00929A1B,
        bytes.fromhex("ff 75 08"),
        "SetProperty must forward the FastName address",
    )
    require_bytes(
        image,
        0x00929A6D,
        bytes.fromhex("8b 4d 08 85 c9 74 0c"),
        "SetProperty must release the found property RefPtr",
    )
    require_bytes(
        image,
        0x00929A90,
        bytes.fromhex("c2 08 00"),
        "SetProperty must pop FastName plus values",
    )

    # Flag/texture queries and property/flag removals all use const FastName&.
    require_bytes(
        image,
        0x00907610,
        bytes.fromhex("8b 4d 08"),
        "HasFlag must receive a FastName address",
    )
    require_bytes(
        image,
        0x00907618,
        bytes.fromhex("ff 31"),
        "HasFlag must dereference const FastName&",
    )
    require_bytes(
        image,
        0x009076D2,
        bytes.fromhex("ff 30"),
        "HasTexture must dereference const FastName&",
    )
    require_bytes(
        image,
        0x009220FF,
        bytes.fromhex("ff 75 08"),
        "RemoveFlag must forward the FastName address",
    )
    require_bytes(
        image,
        0x009221F0,
        bytes.fromhex("8b 75 08 56"),
        "RemoveProperty must forward the FastName address",
    )

    # FastName is a stable string pointer owned by the process-wide intern
    # database. Its constructor stores the lookup result directly; material
    # maps likewise copy that pointer without RefCounted ownership.
    require_bytes(
        image,
        0x005107CE,
        bytes.fromhex("8b cf e8 db 97 00 00 89 06"),
        "FastName must store the intern database string pointer directly",
    )
    require_bytes(
        image,
        0x0051A073,
        bytes.fromhex("8b 7f 08"),
        "FastName lookup must return the interned string entry",
    )
    require_bytes(
        image,
        0x008E094A,
        bytes.fromhex("89 08"),
        "property map insertion must copy a non-owning FastName key",
    )
    require_bytes(
        image,
        0x00933502,
        bytes.fromhex("8b 4f 04 85 c9 74 0c"),
        "property erase must release the mapped value rather than the key",
    )
    require_bytes(
        image,
        0x008E8A05,
        bytes.fromhex("89 01 c7 41 04 00 00 00 00"),
        "flag map insertion must copy a non-owning FastName key",
    )
    require_bytes(
        image,
        0x00933439,
        bytes.fromhex("6a 10 52"),
        "flag erase must free its node without releasing the key",
    )

    # ResourceArchive implementation methods receive an exact-build inline
    # lookup path, not the 24-byte std::string used inside FileInfo. Both pack
    # and zip implementations read the byte count at +0x200.
    require_bytes(
        image,
        0x005C3A40,
        bytes.fromhex("8b 86 00 02 00 00"),
        "PackArchive::LoadFile must read lookup length at +0x200",
    )
    require_bytes(
        image,
        0x005BC298,
        bytes.fromhex("8b be 00 02 00 00"),
        "PackArchive::GetFileInfo must read lookup length at +0x200",
    )
    require_bytes(
        image,
        0x005BC36C,
        bytes.fromhex("8d 82 00 02 00 00"),
        "ZipArchive::GetFileInfo must read lookup length at +0x200 (11.20 hoists it out of the loop)",
    )
    require_bytes(
        image,
        0x005C4387,
        bytes.fromhex("8b 75 08 56 e8 b0 7f ff ff"),
        "ZipArchive::LoadFile must pass the exact lookup path to GetFileInfo",
    )


def verify_wrapper_contract() -> None:
    source = (MOD_API / "src" / "wotb_mod_dava_resources.cpp").read_text(
        encoding="utf-8"
    )
    compact = re.sub(r"\s+", " ", source)
    required = (
        "typedef bool(__thiscall* MaterialHasRefNameFn)( void*, const FastNameScope*);",
        "typedef void(__thiscall* MaterialSetPropertyFn)( void*, const FastNameScope*, const float*);",
        "add(material, name, values, shaderType, arraySize, 0u);",
        "*outPresent = has(material, name);",
        "set(material, name, values);",
        "mutate(material, name);",
        "Calling Retain/Release on this",
    )
    for marker in required:
        if marker not in compact:
            raise AssertionError(f"exact material ABI wrapper marker missing: {marker}")
    if "InvokeRetain(context, name->value)" in compact:
        raise AssertionError(
            "FastName interned string pointers must never use RefCounted Retain"
        )
    forbidden = (
        "MaterialHasNameFn",
        "MaterialHasValueNameFn",
        "MaterialValueNameFn",
        "add(material, name, values, shaderType, arraySize);",
        "set(material, name->value, values);",
        "has(material, name->value)",
        "mutate(material, name->value);",
        "InvokeRelease(context, name->value)",
        "DavaMaterialFastNamePin",
    )
    for marker in forbidden:
        if marker in compact:
            raise AssertionError(f"stale material ABI wrapper remains: {marker}")

    archive_required = (
        "struct DavaArchiveLookupPath32 {",
        "char data[kDavaArchiveLookupPathCapacity]; uint32_t size;",
        "static_assert(sizeof(DavaArchiveLookupPath32) == 516u,",
        "void*, const DavaArchiveLookupPath32*, DavaRawVector32*);",
        "DavaArchiveLookupPath32 davaRelativePath(relativePath);",
        "const bool rootReadable = IsReadableRange(parsed.root, sizeof(uint32_t));",
    )
    for marker in archive_required:
        if marker not in compact:
            raise AssertionError(
                f"exact resource/YAML ABI wrapper marker missing: {marker}"
            )
    if "const bool rootValid = ValidateObject(context, parsed.root, 0u);" in compact:
        raise AssertionError("non-polymorphic YAML root must not require a vtable")

    loader = (MOD_API / "loader" / "wotb_mod_loader.cpp").read_text(
        encoding="utf-8"
    )
    loader_compact = re.sub(r"\s+", " ", loader)
    probe_required = (
        "mutation.mutation_kind = WOTBMOD_DAVA_NATIVE_MATERIAL_REMOVE_PROPERTY; mutation.array_size = 0u;",
        '"Data\\\\3d\\\\Tanks\\\\USSR\\\\T-34-85.sc2.dvpl"',
        '"Data\\\\3d\\\\Tanks\\\\USA\\\\A124_T54E2.sc2.dvpl"',
    )
    for marker in probe_required:
        if marker not in loader_compact:
            raise AssertionError(f"live proof marker missing: {marker}")

    path_required = (
        "InitializeDavaDataDirectory(context);",
        'static const char kResourcePrefix[] = "~res:/";',
        "BuildDavaPath(context, resolvedPath, davaPath, sizeof(davaPath));",
    )
    for marker in path_required:
        if marker not in compact:
            raise AssertionError(f"DAVA path bridge marker missing: {marker}")


def main() -> int:
    verify_wrapper_contract()
    if not GAME_EXE.is_file():
        print(
            "SKIP: exact DAVA machine-code checks require wotblitz.exe; "
            "source contract checks passed"
        )
        return 0
    verify_client_abi()
    print("PASS: exact DAVA private ABI matches client and wrappers")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
