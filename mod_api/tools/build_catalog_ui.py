"""Generate the blitzforge.catalog.ui resource package (examples/catalog_ui) from a pristine Hangar.yaml.

    python tools/build_catalog_ui.py <pristine Hangar.yaml> [--game-root <game>]

The package splices two things into the hangar screen, both static:

* `ModCatalogButton` - a stock icon button in the hangar's left icon column
  (`Elements`), the same prototype the tournament and story buttons use;
* `ModCatalogScreen` - the catalogue itself, hidden until the Lua mod
  `blitzforge.catalog` shows it: header bar with a back button, title and
  tabs, eight card frames with one caption "face" per action, a pager, a
  restart bar and a confirmation dialog. Every caption is a literal, because
  11.20 has no live text write for game-owned controls; the Lua mod toggles
  visibility (the real SetVisibilityFlag) and draws the dynamic lines - mod
  names, versions, status - with its own text controls on top.

Nothing here has bindings or `.actions`: clicks reach the Lua mod through the
pointer-release event and a rectangle hit-test, the way blitzforge.cluster_picker
does it, so the hangar's own action file is untouched.

Regenerate after a client patch: extract the new Hangar.yaml
(`python patch_dvpl.py extract UI/Screens3/Lobby/Hangar/Hangar.yaml`), run this,
bump the package version.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parents[1] / "examples" / "catalog_ui"  # the package folder
TARGET = "UI/Screens3/Lobby/Hangar/Hangar.yaml"
PACKAGE_ID = "blitzforge.catalog.ui"
VERSION = "1.0.0"
CLIENT_BUILD = "11.20.0.887"

STYLES = ("~res:/UI/Screens3/Color.style.yaml;"
          "~res:/UI/Screens3/Font.style.yaml;"
          "~res:/UI/Screens3/Lobby/Hangar/DevMenu/SimpleButton.style.yaml")
ICON = "~res:/Gfx/Lobby/icons/icon_settings_n"
BACK_ICON = "~res:/Gfx/Lobby/icons/icon_arrow-back"
CARDS = 8
CARD_HEIGHT = 64.0
CARD_STEP = 72.0

# ------------------------------------------------------------ templates --


def text_control(name: str, cls: str, x: float, y: float, w: float, h: float, literal: str, indent: int,
                 align: str | None = None) -> str:
    p = " " * indent
    align_line = f'{p}            align: [{align}]\n' if align else ""
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "{name}"\n'
        f'{p}    size: [{w:.6f}, {h:.6f}]\n'
        f'{p}    input: false\n'
        f'{p}    classes: "{cls}"\n'
        f'{p}    components:\n'
        f'{p}        UITextComponent:\n'
        f'{p}            text: "{literal}"\n'
        f'{p}            colorInheritType: "COLOR_IGNORE_PARENT"\n'
        f'{p}            multiline: "MULTILINE_DISABLED"\n'
        f'{align_line}'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            leftAnchor: {x:.6f}\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: {y:.6f}\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "FixedSize"\n'
        f'{p}            horizontalValue: {w:.6f}\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {h:.6f}\n'
    )


def caption(indent: int) -> str:
    """A button's label, filling its parent; the literal is set by the caller."""
    p = " " * indent
    return (
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "Caption"\n'
        f'{p}        input: false\n'
        f'{p}        classes: "t-button bold white-wild-sand-text"\n'
        f'{p}        components:\n'
        f'{p}            UITextComponent:\n'
        f'{p}                text: "{{caption}}"\n'
        f'{p}                colorInheritType: "COLOR_IGNORE_PARENT"\n'
        f'{p}                multiline: "MULTILINE_DISABLED"\n'
        f'{p}                align: ["HCENTER", "VCENTER"]\n'
        f'{p}            Anchor:\n'
        f'{p}                leftAnchorEnabled: true\n'
        f'{p}                rightAnchorEnabled: true\n'
        f'{p}                topAnchorEnabled: true\n'
        f'{p}                bottomAnchorEnabled: true\n'
        f'{p}            SizePolicy:\n'
        f'{p}                horizontalPolicy: "PercentOfParent"\n'
        f'{p}                verticalPolicy: "PercentOfParent"\n'
    )


def button(name: str, text: str, tint: str, w: float, h: float, anchor: list[str], indent: int,
           visible: bool = True, extra_classes: str = "") -> str:
    """A stock-styled push button: simple-button + colour class + caption.

    `anchor` lines are copied verbatim under Anchor:, so each call decides how
    the button sits in its parent.
    """
    p = " " * indent
    # Anchor's own properties sit one level deeper than the Anchor: key; at the
    # same depth they become stray component keys and the button loses its
    # anchoring (every button collapsed to its parent's origin on the first
    # live run).
    anchor_block = "".join(f'{p}            {line}\n' for line in anchor)
    visible_line = "" if visible else f'{p}    visible: false\n'
    classes = f"simple-button {tint}" + (f" {extra_classes}" if extra_classes else "")
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "{name}"\n'
        f'{p}    size: [{w:.6f}, {h:.6f}]\n'
        f'{visible_line}'
        f'{p}    classes: "{classes}"\n'
        f'{p}    components:\n'
        f'{p}        Background: {{}}\n'
        f'{p}        UIOpacityComponent: {{}}\n'
        f'{p}        Anchor:\n'
        f'{anchor_block}'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "FixedSize"\n'
        f'{p}            horizontalValue: {w:.6f}\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {h:.6f}\n'
        f'{p}    children:\n'
        + caption(indent).replace("{caption}", text)
    )


FACES = (
    # name, caption, tint
    ("FaceInstall", "УСТАНОВИТЬ", "green-la-palma-bg"),
    ("FaceUpdate", "ОБНОВИТЬ", "blue-curious-blue-bg"),
    ("FaceRemove", "УДАЛИТЬ", "red-tamarillo-bg"),
    ("FaceDisable", "ВЫКЛЮЧИТЬ", "grey-shark-60-bg"),
    ("FaceEnable", "ВКЛЮЧИТЬ", "green-la-palma-bg"),
    ("FaceLocked", "СИСТЕМНЫЙ", "black-25-bg"),
    ("FaceUnsupported", "НЕ ДЛЯ 11.20", "black-25-bg"),
)


def card(index: int, indent: int) -> str:
    p = " " * indent
    top = (index - 1) * CARD_STEP
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "Card{index}"\n'
        f'{p}    size: [944.000000, {CARD_HEIGHT:.6f}]\n'
        f'{p}    input: false\n'
        f'{p}    classes: "grey-shark-70-bg"\n'
        f'{p}    components:\n'
        f'{p}        Background: {{}}\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: {top:.6f}\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {CARD_HEIGHT:.6f}\n'
        f'{p}    children:\n'
    )
    face_anchor = ["rightAnchorEnabled: true", "rightAnchor: 12.000000", "vCenterAnchorEnabled: true"]
    for name, text, tint in FACES:
        out += button(name, text, tint, 200.0, 44.0, face_anchor, indent + 4, visible=False)
    return out


def header(indent: int) -> str:
    p = " " * indent
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "HeaderBar"\n'
        f'{p}    size: [1024.000000, 80.000000]\n'
        f'{p}    input: false\n'
        f'{p}    classes: "black-50-bg"\n'
        f'{p}    components:\n'
        f'{p}        Background: {{}}\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: 80.000000\n'
        f'{p}    children:\n'
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "BackSquare"\n'
        f'{p}        size: [72.000000, 72.000000]\n'
        f'{p}        input: false\n'
        f'{p}        classes: "grey-shark-70-bg"\n'
        f'{p}        components:\n'
        f'{p}            Background: {{}}\n'
        f'{p}            Anchor:\n'
        f'{p}                leftAnchorEnabled: true\n'
        f'{p}                leftAnchor: 4.000000\n'
        f'{p}                vCenterAnchorEnabled: true\n'
        f'{p}            SizePolicy:\n'
        f'{p}                horizontalPolicy: "FixedSize"\n'
        f'{p}                horizontalValue: 72.000000\n'
        f'{p}                verticalPolicy: "FixedSize"\n'
        f'{p}                verticalValue: 72.000000\n'
        f'{p}        children:\n'
        f'{p}        -   prototype: "IconButtonWithBadge/IconButton"\n'
        f'{p}            name: "BackButton"\n'
        f'{p}            components:\n'
        f'{p}                Anchor:\n'
        f'{p}                    hCenterAnchorEnabled: true\n'
        f'{p}                    vCenterAnchorEnabled: true\n'
        f'{p}                UIDataParamsComponent:\n'
        f'{p}                    args:\n'
        f'{p}                        "image": "\\"{BACK_ICON}\\""\n'
        f'{p}                        "type": "eButtonType.NO_BG"\n'
        f'{p}                        "visible": "true"\n'
    )
    out += text_control("ScreenTitle", "t-title bold align-left white-wild-sand-text",
                        96.0, 20.0, 320.0, 40.0, "КАТАЛОГ МОДОВ", indent + 4)
    tab_anchor = ["vCenterAnchorEnabled: true"]
    out += button("TabCatalog", "КАТАЛОГ", "grey-shark-60-bg", 180.0, 48.0,
                  ["leftAnchorEnabled: true", "leftAnchor: 440.000000"] + tab_anchor, indent + 4)
    out += button("TabCustom", "КАСТОМНЫЕ МОДЫ", "grey-shark-60-bg", 240.0, 48.0,
                  ["leftAnchorEnabled: true", "leftAnchor: 632.000000"] + tab_anchor, indent + 4)
    for name, left, width in (("TabCatalogMark", 440.0, 180.0), ("TabCustomMark", 632.0, 240.0)):
        out += (
            f'{p}    -   class: "UIControl"\n'
            f'{p}        name: "{name}"\n'
            f'{p}        size: [{width:.6f}, 4.000000]\n'
            f'{p}        input: false\n'
            f'{p}        classes: "orange-tango-bg"\n'
            f'{p}        components:\n'
            f'{p}            Background: {{}}\n'
            f'{p}            Anchor:\n'
            f'{p}                leftAnchorEnabled: true\n'
            f'{p}                leftAnchor: {left:.6f}\n'
            f'{p}                bottomAnchorEnabled: true\n'
            f'{p}                bottomAnchor: 12.000000\n'
            f'{p}            SizePolicy:\n'
            f'{p}                horizontalPolicy: "FixedSize"\n'
            f'{p}                horizontalValue: {width:.6f}\n'
            f'{p}                verticalPolicy: "FixedSize"\n'
            f'{p}                verticalValue: 4.000000\n'
        )
    out += button("RefreshButton", "ОБНОВИТЬ", "grey-shark-60-bg", 150.0, 48.0,
                  ["rightAnchorEnabled: true", "rightAnchor: 20.000000"] + tab_anchor, indent + 4)
    return out


def list_page(indent: int) -> str:
    p = " " * indent
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "ListPage"\n'
        f'{p}    size: [944.000000, 560.000000]\n'
        f'{p}    input: false\n'
        f'{p}    components:\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            leftAnchor: 40.000000\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            rightAnchor: 40.000000\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: 124.000000\n'
        f'{p}            bottomAnchorEnabled: true\n'
        f'{p}            bottomAnchor: 100.000000\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "PercentOfParent"\n'
        f'{p}    children:\n'
    )
    for index in range(1, CARDS + 1):
        out += card(index, indent + 4)
    return out


def pager_and_restart(indent: int) -> str:
    bottom = ["bottomAnchorEnabled: true", "bottomAnchor: 40.000000"]
    out = button("PrevButton", "<", "grey-shark-60-bg", 56.0, 44.0,
                 ["leftAnchorEnabled: true", "leftAnchor: 40.000000"] + bottom, indent)
    out += button("NextButton", ">", "grey-shark-60-bg", 56.0, 44.0,
                  ["leftAnchorEnabled: true", "leftAnchor: 104.000000"] + bottom, indent)
    out += button("RestartButton", "ПЕРЕЗАПУСТИТЬ КЛИЕНТ", "orange-tango-bg", 360.0, 48.0,
                  ["rightAnchorEnabled: true", "rightAnchor: 40.000000"] + bottom, indent, visible=False)
    return out


def confirm_overlay(indent: int) -> str:
    p = " " * indent
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "ConfirmOverlay"\n'
        f'{p}    size: [1024.000000, 768.000000]\n'
        f'{p}    visible: false\n'
        f'{p}    input: true\n'
        f'{p}    components:\n'
        f'{p}        Background:\n'
        f'{p}            drawType: "DRAW_FILL"\n'
        f'{p}            color: [0.020000, 0.030000, 0.040000, 0.880000]\n'
        f'{p}        IgnoreLayout: {{}}\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            bottomAnchorEnabled: true\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "PercentOfParent"\n'
        f'{p}    children:\n'
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "Dialog"\n'
        f'{p}        size: [520.000000, 220.000000]\n'
        f'{p}        input: false\n'
        f'{p}        classes: "grey-shark-80-bg"\n'
        f'{p}        components:\n'
        f'{p}            Background: {{}}\n'
        f'{p}            Anchor:\n'
        f'{p}                hCenterAnchorEnabled: true\n'
        f'{p}                vCenterAnchorEnabled: true\n'
        f'{p}            SizePolicy:\n'
        f'{p}                horizontalPolicy: "FixedSize"\n'
        f'{p}                horizontalValue: 520.000000\n'
        f'{p}                verticalPolicy: "FixedSize"\n'
        f'{p}                verticalValue: 220.000000\n'
        f'{p}        children:\n'
    )
    out += text_control("Title", "t-subtitle bold white-wild-sand-text", 0.0, 40.0, 520.0, 34.0,
                        "Удалить мод?", indent + 8, align='"HCENTER", "VCENTER"')
    bottom = ["bottomAnchorEnabled: true", "bottomAnchor: 40.000000"]
    out += button("ConfirmRemove", "УДАЛИТЬ", "red-tamarillo-bg", 200.0, 52.0,
                  ["leftAnchorEnabled: true", "leftAnchor: 40.000000"] + bottom, indent + 8)
    out += button("CancelRemove", "ОТМЕНА", "grey-shark-60-bg", 200.0, 52.0,
                  ["leftAnchorEnabled: true", "leftAnchor: 280.000000"] + bottom, indent + 8)
    return out


def screen() -> str:
    """The whole catalogue, one hidden child of the hangar's root control."""
    p = " " * 4
    head = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "ModCatalogScreen"\n'
        f'{p}    size: [1024.000000, 768.000000]\n'
        f'{p}    visible: false\n'
        f'{p}    input: true\n'
        f'{p}    components:\n'
        f'{p}        Background:\n'
        f'{p}            drawType: "DRAW_FILL"\n'
        f'{p}            color: [0.043137, 0.058824, 0.078431, 0.860000]\n'
        f'{p}        IgnoreLayout: {{}}\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            bottomAnchorEnabled: true\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "PercentOfParent"\n'
        f'{p}        StyleSheet:\n'
        f'{p}            styles: "{STYLES}"\n'
        f'{p}    children:\n'
        # The loader reads the MOD_SCREEN context bit off this control's
        # input flag alone (a hidden parent does not clear it), so it starts
        # without input and the Lua mod raises the flag while the screen is
        # shown (control_set_interactable) - the way the old screen bound
        # `input` to its visibility variable.
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "ModCatalogStateMarker"\n'
        f'{p}        size: [1.000000, 1.000000]\n'
        f'{p}        input: false\n'
        f'{p}        components:\n'
        f'{p}            IgnoreLayout: {{}}\n'
    )
    return head + header(8) + list_page(8) + pager_and_restart(8) + confirm_overlay(8)


HANGAR_BUTTON = '''-   prototype: "IconButtonWithBadge/IconButton"
    name: "ModCatalogButton"
    components:
        UIDataParamsComponent:
            args:
                "image": "\\"{icon}\\""
                "type": "eButtonType.OPTIONAL_LIGHT"
                "visible": "true"
'''

ELEMENTS_INDENT = " " * 52
ELEMENTS_ANCHOR = (ELEMENTS_INDENT + '-   class: "UIControl"',
                   ELEMENTS_INDENT + '    name: "StoryAggregatedButtonsHolder"')

# ------------------------------------------------------------ validation --


def _logical(line: str) -> tuple[int, str, bool]:
    indent = len(line) - len(line.lstrip(" "))
    body = line.strip()
    is_item = body.startswith("- ")
    if is_item:
        body = body[1:].lstrip()
        indent += 4
    return indent, body, is_item


def validate(text: str) -> None:
    """Indent rising after a line that opened nothing, or a key repeated in one mapping."""
    errors: list[str] = []
    stack: list[tuple[int, set[str]]] = []
    prev_indent, prev_opens = 0, True
    for number, raw in enumerate(text.splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent, body, is_item = _logical(raw)
        if indent > prev_indent and not prev_opens:
            errors.append(f"line {number}: indent rises after a line that opens no block -> {raw.strip()[:60]!r}")
        while stack and stack[-1][0] > indent:
            stack.pop()
        if not stack or stack[-1][0] < indent:
            stack.append((indent, set()))
        if is_item:
            stack[-1] = (indent, set())
        key = body.split(":", 1)[0].strip() if ":" in body else None
        if key and not key.startswith(("[", '"[')):
            if key in stack[-1][1]:
                errors.append(f"line {number}: duplicate key {key!r} in this mapping")
            stack[-1][1].add(key)
        prev_indent = indent
        prev_opens = body.rstrip().endswith(":") or is_item
    if errors:
        raise SystemExit("generated YAML is structurally invalid:\n  " + "\n  ".join(errors))


def check_classes(yaml_text: str, game_root: pathlib.Path | None) -> None:
    """Every colour class must exist in the client's stylesheets (an unknown one draws untinted)."""
    if game_root is None:
        return
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))  # _mod_tools, for dvpl.py
    try:
        from dvpl import unpack  # type: ignore
    except ImportError:
        return
    known: set[str] = set()
    for relative in ("UI/Screens3/Color.style.yaml", "UI/Styles/BackgroundStyles.yaml"):
        source = game_root / "Data" / (relative + ".dvpl")
        if source.exists():
            text = unpack(source.read_bytes()).decode("utf-8", errors="replace")
            known |= set(re.findall(r'"\.([a-z0-9][a-z0-9-]*)"', text))
            known |= set(re.findall(r'selector: "\.([a-z0-9][a-z0-9-]*)"', text))
    if not known:
        return
    used: set[str] = set()
    for value in re.findall(r'classes: "([^"]*)"', yaml_text):
        used |= {name for name in value.split() if "-" in name}
    suspect = sorted(name for name in used - known if name.endswith(("-bg", "-text", "-border")))
    if suspect:
        raise SystemExit("these style classes are defined nowhere:\n  " + "\n  ".join(suspect))


# ------------------------------------------------------------------ main --


def splice(source: str) -> str:
    nl = "\r\n" if "\r\n" in source else "\n"
    if "ModCatalog" in source:
        raise SystemExit("the input is already patched; extract a pristine Hangar.yaml")
    anchor = ELEMENTS_ANCHOR[0] + nl + ELEMENTS_ANCHOR[1] + nl
    if source.count(anchor) != 1:
        raise SystemExit(f"Elements anchor found {source.count(anchor)} times")
    hangar_button = "".join(ELEMENTS_INDENT + line + nl for line in
                            HANGAR_BUTTON.format(icon=ICON).rstrip("\n").split("\n"))
    patched = source.replace(anchor, hangar_button + anchor, 1)
    slots = nl + "Slots:" + nl
    if patched.count(slots) != 1:
        raise SystemExit(f"Slots anchor found {patched.count(slots)} times")
    patched = patched.replace(slots, nl + screen().replace("\n", nl) + "Slots:" + nl, 1)
    return patched


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("pristine", help="a pristine Hangar.yaml (patch_dvpl.py extract ...)")
    parser.add_argument("--game-root", default=None, help="game folder, to check colour classes against Color.style.yaml")
    parser.add_argument("--version", default=VERSION)
    args = parser.parse_args()
    source_bytes = pathlib.Path(args.pristine).read_bytes()
    stock_sha = hashlib.sha256(source_bytes).hexdigest()
    patched = splice(source_bytes.decode("utf-8"))
    validate(patched)
    check_classes(patched, pathlib.Path(args.game_root) if args.game_root else None)
    out = HERE / "files" / pathlib.PurePosixPath(TARGET)
    out.parent.mkdir(parents=True, exist_ok=True)
    data = patched.encode("utf-8")
    out.write_bytes(data)
    manifest = {
        "client": {"builds": [CLIENT_BUILD]},
        "description": "Каталог модов в ангаре: иконка в левой колонке и экран на штатных стилях "
                       "(скрыт без Lua-пакета blitzforge.catalog).",
        "developer": "pseud",
        "files": [{
            "sha256": hashlib.sha256(data).hexdigest(),
            "source": "files/" + TARGET,
            "stock_sha256": stock_sha,
            "target": TARGET,
        }],
        "id": PACKAGE_ID,
        "manifest_version": 1,
        "name": "Каталог модов: экран в ангаре",
        "permissions": ["resources.overlay.game"],
        "type": "resource",
        "version": args.version,
    }
    (HERE / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                                        encoding="utf-8")
    print(f"wrote {out} ({len(data)} bytes, stock {stock_sha[:12]}...) and manifest.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
