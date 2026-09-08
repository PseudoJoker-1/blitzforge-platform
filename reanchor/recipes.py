"""Locator recipes: how to find an anchor again after the client is patched.

A recipe is a build-independent description of *how to locate* a function or
global, as opposed to a raw RVA which is only meaningful for one build. The
generator derives recipes from a known-good build; the resolver replays them
against a new one.

Masking is what makes a code signature survive a recompile. Every byte in the
window that holds an address is wildcarded:

  * a dword that lands inside the image's own VA range is an absolute
    reference (a string, a vtable, a global) and moves with every rebuild;
  * the rel32 operand of ``call``/``jmp``/``jcc`` moves whenever anything
    between the caller and callee changes size.

What is left is opcodes, register encodings and small immediates - the part
the compiler reproduces as long as the source did not change.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Iterable

from peimage import PEImage, Section

WILDCARD = 0x3F  # '?'
FIXED = 0x78     # 'x'

# Window growth schedule for signature generation. Short windows first: the
# fewer bytes a signature pins, the less of the function has to survive the
# next recompile unchanged. The long tail exists for near-twin functions -
# sibling destructors in particular, where two bodies stay identical for
# ~200 bytes before they diverge.
_WINDOW_SCHEDULE = (16, 32, 48, 64, 96, 128, 160, 224, 288, 384)

# A window with less fixed material than this is not a signature, it is a
# coincidence.
_MIN_FIXED_BYTES = 8


@dataclass
class Recipe:
    """How to re-find one anchor on an arbitrary build."""

    anchor_id: str
    kind: str                     # "code_signature" | "data_xref" | "unresolvable"
    pattern: str = ""             # hex, one byte per two chars
    mask: str = ""                # 'x' fixed / '?' wildcard
    section: str = ".text"
    note: str = ""
    # data_xref only: the code signature that references the global, plus the
    # byte offset of the absolute operand inside that signature.
    operand_offset: int = -1
    source_rva: int = 0           # RVA on the build the recipe was generated from

    def to_json(self) -> dict:
        payload = {
            "anchor_id": self.anchor_id,
            "kind": self.kind,
            "section": self.section,
            "source_rva": f"0x{self.source_rva:08X}",
        }
        if self.pattern:
            payload["pattern"] = self.pattern
            payload["mask"] = self.mask
        if self.operand_offset >= 0:
            payload["operand_offset"] = self.operand_offset
        if self.note:
            payload["note"] = self.note
        return payload

    @staticmethod
    def from_json(payload: dict) -> "Recipe":
        return Recipe(
            anchor_id=payload["anchor_id"],
            kind=payload["kind"],
            pattern=payload.get("pattern", ""),
            mask=payload.get("mask", ""),
            section=payload.get("section", ".text"),
            note=payload.get("note", ""),
            operand_offset=payload.get("operand_offset", -1),
            source_rva=int(payload.get("source_rva", "0x0"), 16),
        )

    def pattern_bytes(self) -> bytes:
        return bytes.fromhex(self.pattern)

    def mask_bytes(self) -> bytes:
        return self.mask.encode("ascii").replace(b"?", bytes([WILDCARD]))


def _address_like(value: int, image: PEImage) -> bool:
    """True when a dword plausibly holds an address inside this image."""
    low = image.image_base
    high = image.image_base + image.size_of_image
    return low <= value < high


def build_mask(window: bytes, image: PEImage) -> bytearray:
    """Derive a fixed/wildcard mask for `window`.

    Conservative on purpose: wildcarding a byte that did not need it only
    costs a little uniqueness, while leaving an address byte fixed guarantees
    the signature breaks on the next build.
    """
    mask = bytearray([FIXED] * len(window))

    # Absolute references embedded anywhere in the window.
    for index in range(0, len(window) - 3):
        value = struct.unpack_from("<I", window, index)[0]
        if _address_like(value, image):
            for offset in range(index, index + 4):
                mask[offset] = WILDCARD

    # rel32 operands: E8 call, E9 jmp, 0F 80..8F jcc.
    index = 0
    while index < len(window):
        opcode = window[index]
        if opcode in (0xE8, 0xE9) and index + 5 <= len(window):
            for offset in range(index + 1, index + 5):
                mask[offset] = WILDCARD
            index += 5
            continue
        if (
            opcode == 0x0F
            and index + 6 <= len(window)
            and 0x80 <= window[index + 1] <= 0x8F
        ):
            for offset in range(index + 2, index + 6):
                mask[offset] = WILDCARD
            index += 6
            continue
        index += 1

    return mask


def _mask_to_string(mask: Iterable[int]) -> str:
    return "".join("x" if byte == FIXED else "?" for byte in mask)


def _fixed_count(mask: Iterable[int]) -> int:
    return sum(1 for byte in mask if byte == FIXED)


def generate_code_recipe(
    anchor_id: str,
    rva: int,
    image: PEImage,
    code_sections: list[Section],
) -> Recipe:
    """Grow a masked window at `rva` until it is unique across code sections."""
    for size in _WINDOW_SCHEDULE:
        window = image.read(rva, size)
        if window is None:
            break
        mask = build_mask(window, image)
        # A signature made only of wildcards, or with too little fixed
        # material, is worthless however unique it looks.
        if _fixed_count(mask) < _MIN_FIXED_BYTES:
            continue
        hits = image.find_masked(
            window, bytes(mask), sections=code_sections, limit=2
        )
        if len(hits) == 1 and hits[0] == rva:
            return Recipe(
                anchor_id=anchor_id,
                kind="code_signature",
                pattern=window.hex(),
                mask=_mask_to_string(mask),
                section=".text",
                source_rva=rva,
                note=f"{_fixed_count(mask)}/{size} bytes fixed",
            )
    return Recipe(
        anchor_id=anchor_id,
        kind="unresolvable",
        source_rva=rva,
        note="no unique masked signature within "
        f"{_WINDOW_SCHEDULE[-1]} bytes",
    )


def generate_data_recipe(
    anchor_id: str,
    rva: int,
    image: PEImage,
    code_sections: list[Section],
) -> Recipe:
    """Locate a global by signing the code that references it.

    Globals in the zero-filled tail of .data have no bytes of their own to
    match, so the recipe pins a unique instruction that loads the address and
    records where inside that instruction the operand sits.
    """
    references = image.find_absolute_refs(rva, sections=code_sections)
    for reference_rva in references:
        # Include a few bytes of context before the operand so the signature
        # covers the opcode, not just the address.
        for lead in (1, 2, 3, 5):
            start = reference_rva - lead
            for size in _WINDOW_SCHEDULE:
                window = image.read(start, size)
                if window is None:
                    continue
                mask = bytearray(build_mask(window, image))
                if _fixed_count(mask) < _MIN_FIXED_BYTES:
                    continue
                hits = image.find_masked(
                    window, bytes(mask), sections=code_sections, limit=2
                )
                if len(hits) == 1 and hits[0] == start:
                    return Recipe(
                        anchor_id=anchor_id,
                        kind="data_xref",
                        pattern=window.hex(),
                        mask=_mask_to_string(mask),
                        section=".data",
                        operand_offset=lead,
                        source_rva=rva,
                        note=(
                            f"global referenced from 0x{reference_rva:08X}; "
                            f"{_fixed_count(mask)}/{size} bytes fixed"
                        ),
                    )
    return Recipe(
        anchor_id=anchor_id,
        kind="unresolvable",
        source_rva=rva,
        note=f"no unique referencing instruction ({len(references)} refs)",
    )


@dataclass
class Resolution:
    anchor_id: str
    ok: bool
    rva: int = 0
    reason: str = ""
    candidates: int = 0


def resolve(recipe: Recipe, image: PEImage) -> Resolution:
    """Replay one recipe against `image`."""
    if recipe.kind == "unresolvable":
        return Resolution(
            recipe.anchor_id,
            False,
            reason="no recipe was generated for this anchor",
        )

    code_sections = image.code_sections()
    hits = image.find_masked(
        recipe.pattern_bytes(),
        recipe.mask_bytes(),
        sections=code_sections,
        limit=8,
    )
    if not hits:
        return Resolution(
            recipe.anchor_id, False, reason="signature not found"
        )
    if len(hits) > 1:
        return Resolution(
            recipe.anchor_id,
            False,
            reason="signature is ambiguous on this build",
            candidates=len(hits),
        )

    if recipe.kind == "code_signature":
        return Resolution(recipe.anchor_id, True, rva=hits[0], candidates=1)

    if recipe.kind == "data_xref":
        operand_rva = hits[0] + recipe.operand_offset
        value = image.read_u32(operand_rva)
        if value is None:
            return Resolution(
                recipe.anchor_id, False, reason="operand unreadable"
            )
        target = image.rva_from_va(value)
        if not 0 < target < image.size_of_image:
            return Resolution(
                recipe.anchor_id,
                False,
                reason=f"operand 0x{value:08X} is outside the image",
            )
        return Resolution(recipe.anchor_id, True, rva=target, candidates=1)

    return Resolution(
        recipe.anchor_id, False, reason=f"unknown recipe kind {recipe.kind!r}"
    )
