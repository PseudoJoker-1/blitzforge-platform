#!/usr/bin/env python3
"""Re-derive DAVA::Entity's name offset from the shipped client, from scratch.

WHY THIS IS IN THE REPOSITORY. `kEntityNameOffset` in
src/wotb_mod_dava_resources.cpp is the one field offset in this project that was
read out of the binary rather than out of a header, and the whole camouflage
route rests on it. A number in a comment cannot be checked; this can. Run it and
it either reproduces 0x1C with its four corroborations or it tells you the build
moved.

    python tools/derive_entity_name_offset.py

Needs `capstone` and `pefile`. Verifies the client fingerprint BEFORE reading a
single offset - every anchor in loader/anchor_rvas.h is valid for exactly one
build, and on any other one this script must refuse rather than report.
"""
from __future__ import annotations

import hashlib
import re
import struct
import sys
from pathlib import Path

EXE = Path(
    r"C:\Program Files (x86)\Steam\steamapps\common"
    r"\World of Tanks Blitz\wotblitz.exe"
)

# The build every anchor in loader/anchor_rvas.h belongs to.
FINGERPRINT = "41960dbd8d1ace21f24ebccbec8c093e61afd5ddb9a04ad398198f5b3162e0ad"

# Anchors this derivation leans on, all already in loader/anchor_rvas.h.
ENTITY_CTOR_THIN = 0x008CE410      # kDefaultEntityCtorRva
ENTITY_VTABLE = 0x032125E8         # kDefaultEntityVtableRva
FASTNAME_CTOR = 0x0050DE50         # kDefaultFastNameCtorRva
CHILDREN_BEGIN = 0x08              # kEntityChildrenBeginOffset
CHILDREN_END = 0x0C                # kEntityChildrenEndOffset
TRANSFORM = 0x3C                   # kEntityTransformComponentOffset

EXPECTED = 0x1C


def load():
    import pefile

    data = EXE.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != FINGERPRINT:
        raise SystemExit(
            "REFUSING: this is not the anchored build.\n"
            f"  expected {FINGERPRINT}\n  found    {digest}"
        )
    return data, pefile.PE(data=data, fast_load=True)


def rva_to_off(pe, rva):
    for section in pe.sections:
        start = section.VirtualAddress
        end = start + max(section.Misc_VirtualSize, section.SizeOfRawData)
        if start <= rva < end:
            return section.PointerToRawData + (rva - start)
    raise SystemExit(f"rva 0x{rva:08X} is in no section")


def disasm(data, pe, rva, count=260):
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    off = rva_to_off(pe, rva)
    out = []
    for insn in md.disasm(data[off:off + count * 16], rva, count):
        out.append(insn)
        if insn.mnemonic == "ret":
            break
    return out


# Capstone prints small displacements WITHOUT the 0x prefix - `[esi + 8]`, not
# `[esi + 0x8]`. A checker that only matched the prefixed form reported a false
# "not initialised" for +0x08, which is exactly the kind of wrong answer a
# verification script must never give.
_DISP = re.compile(r"\+ (0x[0-9a-f]+|\d+)\]")


def displacements(insn):
    return {int(m, 16) if m.startswith("0x") else int(m)
            for m in _DISP.findall(insn.op_str)}


def writes_to(insns, offset):
    """Offsets this function stores into via a `mov [reg + off], ...`."""
    return [i for i in insns
            if i.mnemonic == "mov" and offset in displacements(i)]


def main():
    data, pe = load()
    base = pe.OPTIONAL_HEADER.ImageBase
    print(f"client fingerprint verified: {FINGERPRINT[:16]}...")

    # The thin ctor builds a temporary FastName and forwards to the real one.
    thin = disasm(data, pe, ENTITY_CTOR_THIN)
    forwarded = [
        int(i.op_str, 16)
        for i in thin
        if i.mnemonic == "call" and i.op_str.startswith("0x")
    ]
    vtable_writes = [
        i for i in thin
        if i.mnemonic == "mov" and hex(ENTITY_VTABLE + base) in i.op_str
    ]
    if not vtable_writes:
        raise SystemExit("the thin ctor does not install the Entity vtable")
    print("1. thin ctor installs kDefaultEntityVtableRva  -> it IS Entity's")

    real = None
    for target in forwarded:
        body = disasm(data, pe, target)
        if any(
            i.mnemonic == "mov" and hex(ENTITY_VTABLE + base) in i.op_str
            for i in body
        ):
            real = (target, body)
            break
    if real is None:
        raise SystemExit("no forwarded call installs the Entity vtable")
    target, body = real
    print(f"2. the real ctor is 0x{target:08X}")

    # It stores the dereferenced FastName argument at exactly one offset.
    candidates = []
    for insn in body:
        if (insn.mnemonic == "mov" and insn.op_str.startswith("dword ptr [")
                and insn.op_str.endswith(", eax")):
            candidates.extend(displacements(insn))
    if EXPECTED not in candidates:
        raise SystemExit(
            f"the ctor stores no register at +0x{EXPECTED:X}; it stores at "
            + ", ".join(hex(c) for c in candidates)
        )
    print(f"3. the ctor stores the dereferenced FastName at +0x{EXPECTED:X}")

    # The layout around it matches anchors already used live.
    zeroed = set()
    for insn in body:
        if insn.mnemonic == "mov" and insn.op_str.endswith(", 0"):
            zeroed |= displacements(insn)
    for name, offset in (
        ("children begin", CHILDREN_BEGIN),
        ("children end", CHILDREN_END),
        ("transform component", TRANSFORM),
    ):
        mark = "yes" if offset in zeroed else "NO"
        print(f"   layout check: {name} +0x{offset:02X} initialised: {mark}")

    # A vtable slot that calls the ANCHORED FastName ctor and stores at +0x1C is
    # SetName(const char*), which settles it.
    off = rva_to_off(pe, ENTITY_VTABLE)
    setters = []
    for slot in range(40):
        va = struct.unpack_from("<I", data, off + 4 * slot)[0]
        if va < base or va > base + 0x4000000:
            break
        body = disasm(data, pe, va - base, 60)
        calls_fastname = any(
            i.mnemonic == "call" and i.op_str == hex(FASTNAME_CTOR)
            for i in body
        )
        stores = writes_to(body, EXPECTED)
        if stores:
            setters.append((slot, va - base, calls_fastname))
    if not setters:
        raise SystemExit(f"no vtable slot writes +0x{EXPECTED:X}")
    print(f"4. vtable slots writing +0x{EXPECTED:X}:")
    for slot, rva, calls in setters:
        note = " and calls the ANCHORED FastName ctor" if calls else ""
        print(f"   slot {slot:2d} @ 0x{rva:08X}{note}")
    if not any(calls for _, _, calls in setters):
        raise SystemExit("no setter calls kDefaultFastNameCtorRva")

    print()
    print(f"DERIVED: DAVA::Entity name offset = 0x{EXPECTED:X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
