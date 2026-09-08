"""Extraction of the fixed-RVA anchor table from the loader sources.

The C++ sources are the single source of truth for which addresses the binding
pack depends on. Parsing them (rather than keeping a parallel list) means a new
anchor cannot be added without the re-anchoring tool noticing it.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, asdict
from pathlib import Path

# Files that declare fixed RVAs, relative to _mod_tools/.
ANCHOR_SOURCES = (
    "mod_api/loader/anchor_rvas.h",
    "mod_api/loader/v3_native_bindings.cpp",
    "mod_api/loader/wotb_mod_loader.cpp",
    "mod_api/src/wotb_mod_dava_sound.cpp",
    "mod_api/src/wotb_mod_dava_resources.cpp",
)

# Files that declare struct field offsets. These are not addresses and cannot
# be re-anchored by scanning; they are tracked so a patch review cannot forget
# them.
OFFSET_SOURCES = (
    "mod_api/loader/wotb_mod_loader.cpp",
    "mod_api/loader/v3_native_camera_layout.h",
    "mod_api/loader/v3_native_bindings.cpp",
    "mod_api/loader/v3_native_client_services.cpp",
    "mod_api/src/wotb_mod_dava_resources.cpp",
)

_RVA_RE = re.compile(
    r"^\s*(?:static\s+)?const\s+uint32_t\s+k(?P<name>\w+)Rva\s*=\s*"
    r"(?P<value>0[xX][0-9A-Fa-f]+)u?\s*;",
    re.MULTILINE,
)

_OFFSET_RE = re.compile(
    r"^\s*(?:static\s+)?const\s+(?:uint32_t|size_t|ptrdiff_t)\s+"
    r"k(?P<name>\w+Offset)\s*=\s*(?P<value>0[xX][0-9A-Fa-f]+)u?\s*;",
    re.MULTILINE,
)


@dataclass(frozen=True)
class Anchor:
    """One fixed RVA the binding pack depends on."""

    id: str            # kGameCameraCtorRva -> "GameCameraCtor"
    constant: str      # the C++ identifier, for regeneration
    source: str        # repo-relative source file
    line: int
    rva: int

    def to_json(self) -> dict:
        payload = asdict(self)
        payload["rva"] = f"0x{self.rva:08X}"
        return payload


@dataclass(frozen=True)
class StructOffset:
    id: str
    constant: str
    source: str
    line: int
    value: int

    def to_json(self) -> dict:
        payload = asdict(self)
        payload["value"] = f"0x{self.value:X}"
        return payload


def _line_of(text: str, position: int) -> int:
    return text.count("\n", 0, position) + 1


def load_anchors(mod_tools_root: Path) -> list[Anchor]:
    anchors: list[Anchor] = []
    seen: dict[str, Anchor] = {}
    for relative in ANCHOR_SOURCES:
        path = mod_tools_root / relative
        if not path.exists():
            raise FileNotFoundError(f"anchor source missing: {path}")
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in _RVA_RE.finditer(text):
            anchor = Anchor(
                id=match.group("name"),
                constant=f"k{match.group('name')}Rva",
                source=relative,
                line=_line_of(text, match.start()),
                rva=int(match.group("value"), 16),
            )
            previous = seen.get(anchor.id)
            if previous is not None and previous.rva != anchor.rva:
                raise ValueError(
                    f"anchor {anchor.id} declared twice with different values: "
                    f"{previous.source}:{previous.line}=0x{previous.rva:08X} vs "
                    f"{anchor.source}:{anchor.line}=0x{anchor.rva:08X}"
                )
            if previous is None:
                seen[anchor.id] = anchor
                anchors.append(anchor)
    return anchors


def load_struct_offsets(mod_tools_root: Path) -> list[StructOffset]:
    offsets: list[StructOffset] = []
    seen: set[str] = set()
    for relative in OFFSET_SOURCES:
        path = mod_tools_root / relative
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for match in _OFFSET_RE.finditer(text):
            name = match.group("name")
            if name in seen:
                continue
            seen.add(name)
            offsets.append(
                StructOffset(
                    id=name,
                    constant=f"k{name}",
                    source=relative,
                    line=_line_of(text, match.start()),
                    value=int(match.group("value"), 16),
                )
            )
    return offsets
