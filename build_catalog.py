"""Generate the in-game mod catalog screen from mod manifests.

The catalog is rebuilt from scratch every time: this reads the pristine
Hangar.yaml out of backup/, splices a freshly generated ModCatalogScreen into
it, and installs the result. Nothing is patched on top of a previous patch, so
a broken run can never accumulate.

    python build_catalog.py            # rebuild from the registry and install
    python build_catalog.py --local    # use _mod_tools/mods/*/manifest.yaml instead
    python build_catalog.py --dry-run  # print the generated screen only

Mods live in _mod_tools/mods/<id>/manifest.yaml:

    id: night-mode
    name: Ночной режим
    version: 1.2.0
    author: pseud
    description: Ночная цветокоррекция сцены боя
    long: |
      Multi-line text for the detail page.
    downloads: 1240
    updated: 27.07.2026
    type: resource
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MODS = HERE / "mods"
# The game root is this tools directory's parent, and the host loads Lua mods
# out of mods/lua under it. A different layer from MODS above: that one holds
# BlitzForge artifacts, this one holds folders the host reads directly.
GAME = HERE.parent
LUA_MODS = GAME / "mods" / "lua"
WORK = HERE / "work"
BACKUP = HERE / "backup"
HANGAR_REL = "UI/Screens3/Lobby/Hangar/Hangar.yaml"
PYTHON = sys.executable
# Each patch_dvpl call is a child process, and a console app with no console
# of its own opens a fresh window for it. Four of those flash over the game
# every rebuild, which is exactly what a mod loader should never look like.
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

# cards sit two levels deeper now that the list scrolls:
# ListPage > UIScrollView > UIScrollViewContainer > children
CARD_INDENT = 20

ICON = "~res:/Gfx/Lobby/icons/icon_settings_n"
STYLES = ("~res:/UI/Screens3/Color.style.yaml;"
          "~res:/UI/Screens3/Font.style.yaml;"
          "~res:/UI/Screens3/Lobby/Hangar/DevMenu/SimpleButton.style.yaml")
MOD_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")


# --------------------------------------------------------------- manifests

def read_manifest(path: Path) -> dict:
    """Deliberately tiny parser: key: value plus one `long: |` block.

    Pulling in PyYAML would mean every user of the toolchain needs it, and the
    manifest format is fixed and flat by design.
    """
    data, key, block = {}, None, []
    for raw in path.read_text(encoding="utf-8").splitlines():
        if key:                                   # inside a `|` block
            if raw.startswith(("  ", "\t")) or not raw.strip():
                block.append(raw.strip())
                continue
            data[key] = "\n".join(block).strip()
            key, block = None, []
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        name, _, value = raw.partition(":")
        name, value = name.strip(), value.strip()
        if value == "|":
            key = name
        else:
            data[name] = value
    if key:
        data[key] = "\n".join(block).strip()
    return data


# The two sections of the catalogue screen, and their headings. A mod belongs
# to exactly one: what the registry publishes, and what the player put in the
# game folder themselves.
CATALOG_SECTION = "catalog"
CUSTOM_SECTION = "custom"
SECTION_TITLES = {
    CATALOG_SECTION: "КАТАЛОГ",
    CUSTOM_SECTION: "КАСТОМНЫЕ МОДЫ",
}


def load_mods(source: str = "registry") -> list[dict]:
    """Mod metadata for the catalogue screen.

    The registry is the real source; local manifests stay available for
    authoring a mod before it has been published. registry.fetch already falls
    back to its disk cache, so a server outage degrades to stale data rather
    than an empty hangar.
    """
    if source != "registry":
        return [dict(mod, section=CUSTOM_SECTION) for mod in custom_mods(None)]

    import registry as registry_client
    import install as installer

    entries, origin = registry_client.fetch()
    print(f"registry: {origin}")
    ledger = installer.load_ledger()
    published = [dict(catalog_entry(entry, ledger), section=CATALOG_SECTION)
                 for entry in entries]

    # THE SECOND SECTION: mods the player has in the game folder that the
    # registry does not carry. `--local` used to REPLACE the registry list,
    # which made authoring and browsing two different screens - so a mod
    # dropped into _mod_tools/mods/ was invisible unless the catalogue was
    # rebuilt in a mode that hid everything else.
    #
    # An id the registry already publishes is NOT repeated here. The published
    # entry is the one with an artifact and a version to compare against, and
    # two cards for one mod is two install buttons whose state would disagree
    # the moment one of them was pressed.
    known = {mod["id"] for mod in published}
    custom = [dict(mod, section=CUSTOM_SECTION)
              for mod in custom_mods(ledger)
              if mod["id"] not in known]
    return published + custom


def catalog_entry(entry: dict, ledger: dict, *, local: bool = False) -> dict:
    """Normalise one manifest and overlay the only authoritative install state.

    A manifest describes what is offered; it is never allowed to claim that it
    is installed. That fact belongs to installed.json. Keeping the merge here
    gives registry and local-authoring builds identical state semantics.
    """
    mod = {key: str(entry.get(key, "")) for key in
           ("id", "name", "version", "author", "description",
            "long", "type", "downloads", "updated")}
    mod["long"] = mod["long"] or mod["description"]
    if not MOD_ID.fullmatch(mod["id"]):
        raise SystemExit(f"invalid mod id for catalogue request path: {mod['id']!r}")

    installed = mod["id"] in ledger
    record = ledger.get(mod["id"])
    installed_record = record if isinstance(record, dict) else {}
    mod["installed"] = "true" if installed else "false"
    mod["outdated"] = "true" if installed and is_newer(
        mod["version"], installed_record.get("version", "0.0.0")) else "false"

    # Registry entries without a complete artifact are useful as announcements
    # but cannot be installed. Local authoring can opt in explicitly with
    # `available: true`; an already-installed entry always remains removable.
    artifact = entry.get("artifact")
    downloadable = (isinstance(artifact, dict) and bool(artifact.get("url"))
                    and bool(artifact.get("sha256")))
    if local:
        downloadable = str(entry.get("available", "false")).lower() == "true"
    mod["available"] = "true" if downloadable else "false"
    return mod


def is_newer(offered: str, installed: str) -> bool:
    """Whether the registry is offering something later than what is on disk.

    Newer, not merely different. Pinning a mod back to an earlier version is a
    deliberate act, and a catalogue that answered it by offering to undo it
    would be arguing with the person using it.

    Anything that does not parse as dotted numbers falls back to inequality:
    an unrecognised scheme is a reason to mention an update, not to hide one.
    """
    def parts(value):
        return [int(piece) for piece in value.split(".")]
    try:
        return parts(offered) > parts(installed)
    except (ValueError, AttributeError):
        return offered != installed


def custom_mods(ledger: dict | None, mods_dir: Path = MODS,
                lua_dir: Path = LUA_MODS) -> list[dict]:
    """Everything the player has of their own, from every layer there is.

    THE SECTION IS NAMED FOR THE PLAYER, NOT FOR A DIRECTORY. It listed only
    `_mod_tools/mods/` - BlitzForge artifacts - so a Lua mod in
    <game>/mods/lua/ never appeared, and "Кастомные моды" showed one validation
    package while the player's own mod ran invisibly beside it. Both layers are
    the player's; both belong here.

    Ordered by id so the list does not reshuffle between rebuilds for no
    reason a reader could see.

    Both directories are parameters so a test can point them at fixtures and
    check the WIRING rather than the two readers in isolation. Testing the
    readers alone is how a layer gets built, tested, and never connected -
    which is the exact fault this function exists to fix.
    """
    if ledger is None:
        import install as installer
        ledger = installer.load_ledger()
    seen, out = set(), []
    for mod in list(load_local_mods(mods_dir, ledger)) + list(
            load_lua_mods(lua_dir, ledger)):
        # One card per id across both layers. A mod packaged as an artifact
        # AND present as a Lua folder is one mod, and the artifact entry is
        # the one the installer can act on.
        if mod["id"] in seen:
            continue
        seen.add(mod["id"])
        out.append(mod)
    return sorted(out, key=lambda mod: mod["id"])


def managed_literal(mod: dict) -> str:
    """Whether this catalogue can act on the mod at all.

    A mod the installer has no record of - a Lua script the player dropped into
    <game>/mods/lua/ - is present and running and cannot be installed, updated
    or removed from here. Saying so on the card is the difference between a
    grey УСТАНОВЛЕН and a red УДАЛИТЬ that does nothing when pressed.
    """
    return "false" if str(mod.get("managed", "true")).lower() == "false" else "true"


def load_lua_mods(lua_dir: Path, ledger: dict | None = None) -> list[dict]:
    """Lua mods installed in the game folder.

    A DIFFERENT LAYER FROM `_mod_tools/mods/`, and it was missing entirely:
    that directory holds BlitzForge ARTIFACTS - things with a patch set and a
    ledger record - while a Lua mod is a folder with a manifest.json and a
    main.lua that the host loads directly. The catalogue only ever looked at
    the artifacts, so the player's own Lua mods, including the one this whole
    branch is about, were invisible in a section named for exactly them.

    They are listed as INSTALLED, because they are - the host loads what is in
    this directory - and as NOT MANAGED, because nothing here can add or
    remove one.
    """
    if not lua_dir.exists():
        return []
    mods = []
    for manifest in sorted(lua_dir.glob("*/manifest.json")):
        try:
            data = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            # NAMED, not skipped in silence. A manifest with a typo in it is
            # a mod the player believes is installed, and a catalogue that
            # simply omits it answers no question they might ask.
            print(f"  ! {manifest.parent.name}/manifest.json unreadable: {error}")
            continue
        if not isinstance(data, dict):
            print(f"  ! {manifest.parent.name}/manifest.json is not an object")
            continue
        entry = {
            "id": str(data.get("id") or manifest.parent.name),
            "name": str(data.get("name") or data.get("id") or manifest.parent.name),
            "version": str(data.get("version") or "0.0.0"),
            "author": str(data.get("author") or "unknown"),
            "description": str(data.get("description") or "Lua-мод в папке игры"),
            "long": str(data.get("long") or data.get("description")
                        or "Lua-мод, загружаемый хостом из mods/lua."),
            "type": "lua",
            "downloads": "0",
            "updated": "",
        }
        if not MOD_ID.fullmatch(entry["id"]):
            print(f"  ! {manifest.parent.name}: id {entry['id']!r} is not usable")
            continue
        mod = catalog_entry(entry, ledger or {}, local=True)
        mod["installed"] = "true"
        mod["outdated"] = "false"
        mod["available"] = "false"
        mod["managed"] = "false"
        mods.append(mod)
    return mods


def load_local_mods(mods_dir: Path = MODS, ledger: dict | None = None) -> list[dict]:
    if not mods_dir.exists():
        return []
    if ledger is None:
        import install as installer
        ledger = installer.load_ledger()
    mods = []
    for manifest in sorted(mods_dir.glob("*/manifest.yaml")):
        m = read_manifest(manifest)
        m.setdefault("id", manifest.parent.name)
        m.setdefault("name", m["id"])
        m.setdefault("author", "unknown")
        m.setdefault("version", "1.0.0")
        m.setdefault("description", "")
        m.setdefault("long", m["description"])
        m.setdefault("downloads", "0")
        m.setdefault("updated", "")
        m.setdefault("type", "resource")
        mods.append(catalog_entry(m, ledger, local=True))
    return mods


# ------------------------------------------------------------- yaml pieces

def esc(text: str) -> str:
    """Quote a literal for a DAVA binding expression."""
    return text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def text_control(name, cls, x, y, w, h, literal, indent):
    p = " " * indent
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "{name}"\n'
        f'{p}    size: [{w:.6f}, {h:.6f}]\n'
        f'{p}    input: false\n'
        f'{p}    classes: "{cls}"\n'
        f'{p}    components:\n'
        f'{p}        UITextComponent:\n'
        f'{p}            colorInheritType: "COLOR_IGNORE_PARENT"\n'
        f'{p}            multiline: "MULTILINE_DISABLED"\n'
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
        f'{p}    bindings:\n'
        f'{p}    - ["UITextComponent.text", "\\"{esc(literal)}\\""]\n'
    )


CARD_BUTTON_ANCHOR = ("rightAnchorEnabled: true",
                      "rightAnchor: 20.000000",
                      "vCenterAnchorEnabled: true")
DETAIL_BUTTON_ANCHOR = ("leftAnchorEnabled: true",
                        "leftAnchor: 168.000000",
                        "topAnchorEnabled: true",
                        "topAnchor: 80.000000")


def install_button(indent, index=0, sent_expr=None, stale_expr=None,
                   available_expr=None, width=168.0, height=48.0,
                   anchor=CARD_BUTTON_ANCHOR, managed_expr="true"):
    """The anchor block is a parameter, never post-hoc string surgery.

    Rewriting the emitted YAML with str.replace() hits every occurrence, not
    the one intended: it duplicated a key inside the Caption's Anchor and
    pasted a line at the caller's indentation instead of the block's, which
    is a parse error rather than a layout glitch.
    """
    p = " " * indent
    anchor_block = "".join(f'{p}            {line}\n' for line in anchor)
    # One button owns every lifecycle action. It presents install, update,
    # remove, or a non-interactive unavailable state instead of letting
    # overlapping controls disagree about the same mod.
    # One action per card. An action can only ChangeData a name it spells out,
    # and each card now owns its own variable, so the index cannot be a runtime
    # argument any more - it is baked into the action the generator emits.
    action = f"ON_MOD_TOGGLE_{index}"
    # Only the card that was pressed reacts. modRequestSent alone is one flag
    # for the whole screen, so keying on it without the index latched every
    # button at once and the list stopped showing install state entirely.
    #
    # The condition cannot be written into the captions directly. This button
    # carries a UIDataParamsComponent, which opens its own data scope, and the
    # screen's local variables are not visible inside it - which is why both a
    # `when` on the text and a `visible` binding silently did nothing while a
    # plain literal drew fine. A declared param with an arg is how the game
    # itself passes a value across that boundary.
    shown = sent_expr or state_expression(index)
    # A third action, not a third button. An installed mod with a newer version
    # in the registry has one useful action and it is not removal, so the same
    # control changes what it offers rather than growing a neighbour.
    stale = stale_expr or f"modOutdated{index}"
    available = available_expr or f"modAvailable{index}"
    # MANAGED BY THIS CATALOGUE, or merely present. A Lua mod in
    # <game>/mods/lua/ is installed and running, and the installer knows
    # nothing about it: no artifact to fetch, no ledger record to remove. A
    # LITERAL rather than a runtime variable, because it cannot change while
    # the client is up - a mod does not become managed by being looked at - so
    # there is nothing to seed and nothing to keep in step.
    managed = managed_expr
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "InstallButton"\n'
        f'{p}    size: [{width:.6f}, {height:.6f}]\n'
        f'{p}    classes: "simple-button"\n'
        f'{p}    components:\n'
        f'{p}        UIOpacityComponent: {{}}\n'
        f'{p}        UIInputEventComponent:\n'
        f'{p}            onTouchUpInside: "ON_CLICK"\n'
        f'{p}        UIDataParamsComponent:\n'
        f'{p}            params:\n'
        f'{p}            - ["bool", "shown", "false", "false"]\n'
        f'{p}            - ["bool", "stale", "false", "false"]\n'
        f'{p}            - ["bool", "available", "false", "false"]\n'
        f'{p}            - ["bool", "managed", "true", "true"]\n'
        f'{p}            args:\n'
        f'{p}                "shown": "{shown}"\n'
        f'{p}                "stale": "{stale}"\n'
        f'{p}                "available": "{available}"\n'
        f'{p}                "managed": "{managed}"\n'
        f'{p}            events:\n'
        f'{p}            - "ON_CLICK"\n'
        f'{p}            eventActions:\n'
        f'{p}            - ["ON_CLICK", "{action}", ""]\n'
        f'{p}        Anchor:\n'
        f'{anchor_block}'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "FixedSize"\n'
        f'{p}            horizontalValue: {width:.6f}\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {height:.6f}\n'
        f'{p}    bindings:\n'
        # A BUTTON THAT CANNOT ACT DOES NOT TAKE INPUT. Offering УДАЛИТЬ for a
        # mod the installer has no record of would send a request nothing can
        # service, and the card would sit there looking pressed for ever.
        f'{p}    - ["input", "(shown or available) and managed"]\n'
        f'{p}    children:\n'
        f'{caption_control(p, "FaceInstall", "green-la-palma-bg", "УСТАНОВИТЬ", "available and not shown and managed")}'
        f'{caption_control(p, "FaceRemove", "red-tamarillo-bg", "УДАЛИТЬ", "shown and not stale and managed")}'
        f'{caption_control(p, "FaceUpdate", "blue-curious-blue-bg", "ОБНОВИТЬ", "shown and stale and managed")}'
        f'{caption_control(p, "FacePresent", "grey-shark-70-bg", "УСТАНОВЛЕН", "shown and not managed")}'
        f'{caption_control(p, "FaceUnavailable", "grey-shark-70-bg", "НЕДОСТУПЕН", "not available and not shown and managed")}'
    )


def state_expression(index: int) -> str:
    """Whether the mod reads as installed right now, pending action included.

    Until the client restarts the ledger cannot change, so the button has to
    show the state the press will produce rather than the one on disk. Each
    card gets a variable of its own, seeded from the ledger.

    A single screen-wide pair could not do this. modRequestIndex holds one
    number, so installing a second mod moved it off the first, whose button
    fell straight back to УСТАНОВИТЬ though its install was still pending -
    only ever one button showed УДАЛИТЬ, whichever was pressed last.
    """
    return f"modInstalled{index}"


def caption_control(p: str, name: str, tint: str, text: str, visible: str) -> str:
    """One lifecycle face of the button: its own colour and label.

    The colour has to change with the state as well as the text, and a control
    only carries one set of classes, so each state is a full-size face of its
    own and `visible` picks between them.

    The label used to switch through a `when` expression on
    UITextComponent.text and rendered as nothing in game - a green button with
    no writing on it. The states are now controls toggled by `visible`,
    which this screen already relies on for its pages and its overlay, so it is
    a mechanism known to work here rather than one assumed to.
    """
    return (
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "{name}"\n'
        f'{p}        input: false\n'
        f'{p}        classes: "t-button bold white-wild-sand-text {tint}"\n'
        f'{p}        components:\n'
        f'{p}            Background: {{}}\n'
        f'{p}            UITextComponent:\n'
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
        f'{p}        bindings:\n'
        f'{p}        - ["visible", "{visible}"]\n'
        f'{p}        - ["UITextComponent.text", "\\"{text}\\""]\n'
    )


def card(mod: dict, index: int, indent: int = CARD_INDENT) -> str:
    p = " " * indent
    meta = f'{mod["author"]}  •  {mod["downloads"]} загрузок  •  {mod["updated"]}'
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "Card{index}"\n'
        f'{p}    size: [944.000000, 120.000000]\n'
        f'{p}    classes: "simple-button grey-shark-70-bg"\n'
        f'{p}    components:\n'
        f'{p}        Background: {{}}\n'
        f'{p}        UIOpacityComponent: {{}}\n'
        f'{p}        UIInputEventComponent:\n'
        f'{p}            onTouchUpInside: "ON_CLICK"\n'
        f'{p}        UIDataParamsComponent:\n'
        # The card opens a data scope of its own, so the button inside it
        # cannot reach modRequestSent either - and an arg on the button is
        # evaluated here, in this scope, not at the screen. The value has to be
        # carried across both boundaries, one hop at a time.
        f'{p}            params:\n'
        f'{p}            - ["bool", "shown", "false", "false"]\n'
        f'{p}            - ["bool", "stale", "false", "false"]\n'
        f'{p}            - ["bool", "available", "false", "false"]\n'
        f'{p}            args:\n'
        f'{p}                "shown": "{state_expression(index)}"\n'
        f'{p}                "stale": "modOutdated{index}"\n'
        f'{p}                "available": "modAvailable{index}"\n'
        f'{p}            events:\n'
        f'{p}            - "ON_CLICK"\n'
        f'{p}            eventActions:\n'
        f'{p}            - ["ON_CLICK", "ON_MOD_CARD_CLICKED", "{index}"]\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: 120.000000\n'
        f'{p}    children:\n'
        f'{p}    -   class: "UIControl"\n'
        f'{p}        name: "Icon"\n'
        f'{p}        size: [88.000000, 88.000000]\n'
        f'{p}        input: false\n'
        f'{p}        classes: "black-25-bg"\n'
        f'{p}        components:\n'
        f'{p}            Background:\n'
        f'{p}                drawType: "DRAW_ALIGNED"\n'
        f'{p}                sprite: "{ICON}"\n'
        f'{p}                align: ["HCENTER", "VCENTER"]\n'
        f'{p}            Anchor:\n'
        f'{p}                leftAnchorEnabled: true\n'
        f'{p}                leftAnchor: 16.000000\n'
        f'{p}                vCenterAnchorEnabled: true\n'
        f'{p}            SizePolicy:\n'
        f'{p}                horizontalPolicy: "FixedSize"\n'
        f'{p}                horizontalValue: 88.000000\n'
        f'{p}                verticalPolicy: "FixedSize"\n'
        f'{p}                verticalValue: 88.000000\n'
    )
    out += text_control("Name", "t-subtitle bold align-left white-wild-sand-text",
                        120, 18, 520, 30, mod["name"], indent + 4)
    out += text_control("Meta", "t-caption regular align-left white-wild-sand-50-text",
                        120, 50, 520, 22, meta, indent + 4)
    out += text_control("Description", "t-body regular align-left white-wild-sand-70-text",
                        120, 76, 560, 24, mod["description"], indent + 4)
    out += install_button(indent + 4, index=index, sent_expr="shown",
                          stale_expr="stale", available_expr="available",
                          managed_expr=managed_literal(mod))
    return out


def build_screen(mods: list[dict]) -> str:
    # A HEADING BEFORE THE FIRST CARD OF EACH SECTION, and none where a
    # section is empty. The cards themselves keep their positions in `mods`:
    # every binding a card carries is named by that index (modOutdated{i},
    # ON_MOD_CARD_CLICKED {i}), so a heading is a sibling control and never a
    # list entry.
    pieces, seen_section = [], None
    for index, mod in enumerate(mods):
        section = mod.get("section", CATALOG_SECTION)
        if section != seen_section:
            seen_section = section
            pieces.append(text_control(
                f"Section_{section}",
                "t-subtitle bold align-left white-wild-sand-text",
                0, 0, 944, 34, SECTION_TITLES.get(section, section),
                CARD_INDENT))
        pieces.append(card(mod, index, CARD_INDENT))
    cards = "".join(pieces)
    if not cards:
        cards = text_control("Empty", "t-body regular align-left white-wild-sand-50-text",
                             0, 0, 600, 30, "Установленных модов нет", CARD_INDENT)
    # The header shows the open mod's name on the detail page and the catalog
    # title with a count on the list, so one chained `when` covers both.
    if mods:
        cases = ", ".join(f'modDetailIndex == {i} -> \\"{esc(m["name"]).upper()}\\"'
                          for i, m in enumerate(mods))
        detail_title = f'(when {cases}, \\"МОД\\")'
    else:
        detail_title = '\\"МОД\\"'
    title_expr = (f'when modDetailVisible -> {detail_title}, '
                  f'\\"КАТАЛОГ МОДОВ · {len(mods)}\\"')

    head = f'''    -   class: "UIControl"
        name: "ModCatalogScreen"
        size: [1024.000000, 768.000000]
        input: true
        components:
            Background:
                drawType: "DRAW_FILL"
                color: [0.043137, 0.058824, 0.078431, 0.720000]
            IgnoreLayout: {{}}
            Anchor:
                leftAnchorEnabled: true
                rightAnchorEnabled: true
                topAnchorEnabled: true
                bottomAnchorEnabled: true
            SizePolicy:
                horizontalPolicy: "PercentOfParent"
                verticalPolicy: "PercentOfParent"
            StyleSheet:
                styles: "{STYLES}"
        bindings:
        - ["visible", "modCatalogVisible"]
        children:
        -   class: "UIControl"
            name: "ModCatalogStateMarker"
            size: [1.000000, 1.000000]
            input: false
            components:
                IgnoreLayout: {{}}
            bindings:
            - ["input", "modCatalogVisible"]
        -   class: "UIControl"
            name: "RequestSignal"
            size: [1.000000, 1.000000]
            input: false
            components:
                Background: {{}}
                IgnoreLayout: {{}}
                Anchor:
                    leftAnchorEnabled: true
                    topAnchorEnabled: true
                SizePolicy:
                    horizontalPolicy: "FixedSize"
                    horizontalValue: 1.000000
                    verticalPolicy: "FixedSize"
                    verticalValue: 1.000000
            bindings:
            - ["Background.sprite", "when modRequestSent -> \\"~res:/BLITZFORGE/\\" + str(modRequestVerb) + \\"/\\" + str(modCatalogGeneration) + \\"/\\" + str(modRequestIndex) + \\"-\\" + str(modRequestSeq), \\"\\""]
        -   class: "UIControl"
            name: "HeaderBar"
            size: [1024.000000, 80.000000]
            input: false
            classes: "black-50-bg"
            components:
                Background: {{}}
                Anchor:
                    leftAnchorEnabled: true
                    rightAnchorEnabled: true
                    topAnchorEnabled: true
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "FixedSize"
                    verticalValue: 80.000000
            children:
            -   class: "UIControl"
                name: "BackSquare"
                size: [72.000000, 72.000000]
                input: false
                classes: "grey-shark-70-bg"
                components:
                    Background: {{}}
                    Anchor:
                        leftAnchorEnabled: true
                        leftAnchor: 4.000000
                        vCenterAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 72.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 72.000000
                children:
                -   prototype: "IconButtonWithBadge/IconButton"
                    name: "BackButton"
                    components:
                        Anchor:
                            hCenterAnchorEnabled: true
                            vCenterAnchorEnabled: true
                        UIDataParamsComponent:
                            args:
                                "image": "\\"~res:/Gfx/Lobby/icons/icon_arrow-back\\""
                                "type": "eButtonType.NO_BG"
                            eventActions:
                            - ["ON_CLICK_BUTTON", "ON_MOD_CATALOG_BACK", ""]
            -   class: "UIControl"
                name: "ScreenTitle"
                size: [620.000000, 40.000000]
                input: false
                classes: "t-title bold align-left white-wild-sand-text"
                components:
                    UITextComponent:
                        colorInheritType: "COLOR_IGNORE_PARENT"
                        multiline: "MULTILINE_DISABLED"
                    Anchor:
                        leftAnchorEnabled: true
                        leftAnchor: 96.000000
                        vCenterAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 620.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 40.000000
                bindings:
                - ["UITextComponent.text", "{title_expr}"]
        -   class: "UIControl"
            name: "ListPage"
            size: [944.000000, 616.000000]
            input: false
            components:
                Anchor:
                    leftAnchorEnabled: true
                    leftAnchor: 40.000000
                    rightAnchorEnabled: true
                    rightAnchor: 40.000000
                    topAnchorEnabled: true
                    topAnchor: 104.000000
                    bottomAnchorEnabled: true
                    bottomAnchor: 32.000000
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "PercentOfParent"
            bindings:
            - ["visible", "not modDetailVisible"]
            - ["Anchor.topAnchor", "when modBusy -> {LIST_TOP_WHEN_BUSY}, modRequestSent -> 176, 104"]
            children:
            -   class: "UIScrollView"
                name: "ModScroll"
                size: [944.000000, 616.000000]
                autoUpdate: true
                centerContent: false
                components:
                    SizePolicy:
                        horizontalPolicy: "PercentOfParent"
                        verticalPolicy: "PercentOfParent"
                children:
                -   class: "UIScrollViewContainer"
                    name: "scrollContainerControl"
                    components:
                        LinearLayout:
                            orientation: "TopDown"
                            spacing: 12.000000
                        SizePolicy:
                            horizontalPolicy: "PercentOfParent"
                            verticalPolicy: "PercentOfChildrenSum"
                    children:
{cards}'''

    pages = "".join(detail_page(m, i) for i, m in enumerate(mods))
    return head + pages + progress_bar() + restart_bar() + confirm_overlay()


def detail_page(mod: dict, index: int) -> str:
    """One page per mod, selected by modDetailIndex.

    A single shared page could only ever show mods[0], so every card opened
    the same mod regardless of which was tapped.
    """
    detail_meta = f'Версия {mod["version"]}  •  автор {mod["author"]}'
    detail_stats = (f'{mod["downloads"]} загрузок  •  обновлён {mod["updated"]}'
                    f'  •  {mod["type"]}-мод')

    detail = f'''        -   class: "UIControl"
            name: "DetailPage{index}"
            size: [944.000000, 616.000000]
            input: false
            components:
                Anchor:
                    leftAnchorEnabled: true
                    leftAnchor: 40.000000
                    rightAnchorEnabled: true
                    rightAnchor: 40.000000
                    topAnchorEnabled: true
                    topAnchor: 104.000000
                    bottomAnchorEnabled: true
                    bottomAnchor: 32.000000
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "PercentOfParent"
            bindings:
            - ["visible", "modDetailVisible and modDetailIndex == {index}"]
            - ["Anchor.topAnchor", "when modBusy -> {LIST_TOP_WHEN_BUSY}, modRequestSent -> 176, 104"]
            children:
            -   class: "UIControl"
                name: "DetailIcon"
                size: [140.000000, 140.000000]
                input: false
                classes: "black-25-bg"
                components:
                    Background:
                        drawType: "DRAW_ALIGNED"
                        sprite: "{ICON}"
                        align: ["HCENTER", "VCENTER"]
                    Anchor:
                        leftAnchorEnabled: true
                        topAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 140.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 140.000000
'''
    detail += text_control("DetailMeta", "t-caption regular align-left white-wild-sand-50-text",
                           168, 8, 600, 26, detail_meta, 12)
    detail += text_control("DetailStats", "t-caption regular align-left white-wild-sand-50-text",
                           168, 40, 600, 26, detail_stats, 12)
    detail += install_button(12, index=index,
                             width=200.0, height=52.0,
                             anchor=DETAIL_BUTTON_ANCHOR,
                             managed_expr=managed_literal(mod))
    # esc() owns both quoting and newline encoding. Pre-escaping here turns a
    # real line break into a visible backslash-n sequence on the detail page.
    long_text = mod["long"]
    detail += (
        '            -   class: "UIControl"\n'
        '                name: "DetailDescription"\n'
        '                size: [880.000000, 240.000000]\n'
        '                input: false\n'
        '                classes: "t-body regular align-left white-wild-sand-70-text"\n'
        '                components:\n'
        '                    UITextComponent:\n'
        '                        colorInheritType: "COLOR_IGNORE_PARENT"\n'
        '                        multiline: "MULTILINE_ENABLED"\n'
        '                    Anchor:\n'
        '                        leftAnchorEnabled: true\n'
        '                        topAnchorEnabled: true\n'
        '                        topAnchor: 176.000000\n'
        '                    SizePolicy:\n'
        '                        horizontalPolicy: "FixedSize"\n'
        '                        horizontalValue: 880.000000\n'
        '                        verticalPolicy: "FixedSize"\n'
        '                        verticalValue: 240.000000\n'
        '                bindings:\n'
        f'                - ["UITextComponent.text", "\\"{esc(long_text)}\\""]\n'
    )
    # Resource patches are applied to files the client reads at startup, so a
    # button press cannot take effect in the running session. Saying so is the
    # difference between a working feature and one that looks broken.
    detail += (
        '            -   class: "UIControl"\n'
        '                name: "RestartHint"\n'
        '                size: [880.000000, 28.000000]\n'
        '                input: false\n'
        '                classes: "t-caption regular align-left orange-tango-text"\n'
        '                components:\n'
        '                    UITextComponent:\n'
        '                        colorInheritType: "COLOR_IGNORE_PARENT"\n'
        '                        multiline: "MULTILINE_DISABLED"\n'
        '                    Anchor:\n'
        '                        leftAnchorEnabled: true\n'
        '                        leftAnchor: 168.000000\n'
        '                        topAnchorEnabled: true\n'
        '                        topAnchor: 140.000000\n'
        '                    SizePolicy:\n'
        '                        horizontalPolicy: "FixedSize"\n'
        '                        horizontalValue: 880.000000\n'
        '                        verticalPolicy: "FixedSize"\n'
        '                        verticalValue: 28.000000\n'
        '                bindings:\n'
        '                - ["visible", "modRequestSent"]\n'
        '                - ["UITextComponent.text", '
        '"\\"Команда отправлена. Изменения применятся после перезапуска.\\""]\n'
    )
    return detail


def restart_bar() -> str:
    """Sits on the catalogue screen, not on a mod's page.

    A restart applies whatever has been queued, not one mod, so it belongs
    to the screen. The wrapper carries the visibility condition: it holds no
    UIDataParamsComponent, so the screen's variables are in scope there,
    while inside the button they would not be.
    """
    p = " " * 8
    wrapper = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "RestartBar"\n'
        f'{p}    size: [944.000000, 64.000000]\n'
        f'{p}    input: false\n'
        f'{p}    components:\n'
        f'{p}        IgnoreLayout: {{}}\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            leftAnchor: 40.000000\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            rightAnchor: 40.000000\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: 96.000000\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: 64.000000\n'
        f'{p}    bindings:\n'
        # The progress strip and the restart button share this slot. They are
        # never wanted at once - one means the work is running, the other that
        # it is done - so the strip yields rather than stacking.
        f'{p}    - ["visible", "modRequestSent and not modBusy"]\n'
        f'{p}    children:\n'
    )
    return wrapper + restart_button(12)


def restart_button(indent: int) -> str:
    """Offered only once a request has been sent, since that is the only time
    restarting achieves anything."""
    p = " " * indent
    return (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "RestartButton"\n'
        f'{p}    size: [260.000000, 52.000000]\n'
        f'{p}    classes: "simple-button orange-tango-bg"\n'
        f'{p}    components:\n'
        f'{p}        Background: {{}}\n'
        f'{p}        UIOpacityComponent: {{}}\n'
        f'{p}        UIInputEventComponent:\n'
        f'{p}            onTouchUpInside: "ON_CLICK"\n'
        f'{p}        UIDataParamsComponent:\n'
        f'{p}            events:\n'
        f'{p}            - "ON_CLICK"\n'
        f'{p}            eventActions:\n'
        f'{p}            - ["ON_CLICK", "ON_MOD_RESTART_CLICKED", ""]\n'
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
        f'{p}        name: "Caption"\n'
        f'{p}        input: false\n'
        f'{p}        classes: "t-button bold white-wild-sand-text"\n'
        f'{p}        components:\n'
        f'{p}            UITextComponent:\n'
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
        f'{p}        bindings:\n'
        f'{p}        - ["UITextComponent.text", "\\"ПЕРЕЗАПУСТИТЬ ИГРУ И ПРИМЕНИТЬ\\""]\n'
    )


# The circle is the client's own, not a drawing of one: the same hollow ring
# sprite and the same RadialProgressComponent that Screens3/Lobby/Common/
# DownloadProgress.yaml fills while the game downloads content.
LEVELS = 24
RING_SPRITE = "~res:/Gfx/Lobby/backgrounds/bg_circle-hollow_64"

# Ring geometry. Every layer is the whole circle at the ring's full size and is
# centred in ProgressRing, so the layers line up by construction rather than by
# 25 separately computed anchors that could disagree. The strip's height and
# the offset the list/detail pages drop to while it is on screen (see
# build_screen and detail_page) are derived from these numbers rather than
# hand-typed twice, so a future size change cannot leave either of them stale.
RING_SIZE = 128.0
CAPTION_TOP = 10.0
CAPTION_HEIGHT = 20.0
RING_TOP_GAP = 20.0                 # clearance between the caption and the
                                     # ring, so the fill never draws over it
RING_TOP = CAPTION_TOP + CAPTION_HEIGHT + RING_TOP_GAP
RING_BOTTOM_PAD = 16.0
STRIP_HEIGHT = RING_TOP + RING_SIZE + RING_BOTTOM_PAD
STRIP_TOP = 96.000000                # the slot this strip shares with the
                                      # restart button in restart_bar()
# modBusy is only ever true while modRequestSent is also true (ON_MOD_WORKING
# sets it after the toggle action already has), so this is really three
# states in sequence: idle (104), request sent but the ring not up yet (176,
# unchanged - that gap was already sized for the restart button's own 64px
# slot), and the ring itself, which now needs the room the taller strip takes.
LIST_TOP_WHEN_BUSY = int(STRIP_TOP + STRIP_HEIGHT + 16)


IDLE_SPRITE = "~res:/Gfx/Lobby/icons/icon_empty_32"


def level_path(index: int) -> str:
    """The sprite path fill level `index` asks for.

    modTick is in the path because a sprite is cached per path: without it the
    first frame would be the only one that could ever change. modRequestSeq is
    in it because a second install would otherwise be served the first one's
    pictures. progress.write_tick writes exactly these paths.

    The `when modBusy` is not decoration, and it is not about visibility. A
    binding resolves when its inputs change, whether or not anything is on
    screen, and a path with no file behind it does not draw nothing - the
    engine substitutes a pink placeholder and caches it against that path
    forever. Both halves of that bit the user:

    * At rest the path was g/0-0/*, a run that cannot exist, because the first
      press makes the sequence 1. Every screen load resolved 24 paths that were
      never going to be written, and the placeholders came out tinted by each
      layer's colour class - a solid orange square sitting on the ring.
    * On the press itself, seq changes and the path becomes g/1-0/*, which the
      engine tries to load in the same millisecond the press is written to the
      client log. That log line is how the installer *learns* of the press, so
      it cannot have written the files yet. Tick zero was unwinnable by
      construction.

    Deferring to modBusy fixes both: at rest every layer points at a stock
    transparent sprite that certainly exists, and the run's paths are not
    resolved until half a second after the press, by which time the installer
    has laid tick zero down.
    """
    return (f'when modBusy -> \\"~res:/BLITZFORGE/g/\\" + str(modRequestSeq) '
            f'+ \\"-\\" + str(modTick) + \\"/{index}\\", \\"{IDLE_SPRITE}\\"')


def ring_layer(indent: int, name: str, tint: str, *, sprite: str = "",
               progress: float | None = None, sprite_binding: str = "") -> str:
    """One full-size circle stacked in ProgressRing.

    The track passes a literal `sprite` and no progress, so it draws whole.
    A fill passes a `progress` and binds its sprite to a path instead, so it
    draws only when the installer has put the ring sprite there.
    """
    p = " " * indent
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "{name}"\n'
        f'{p}    size: [{RING_SIZE:.6f}, {RING_SIZE:.6f}]\n'
        f'{p}    pivot: [0.500000, 0.500000]\n'
        f'{p}    input: false\n'
        f'{p}    classes: "{tint}"\n'
        f'{p}    components:\n'
        f'{p}        Background:\n'
        f'{p}            drawType: "DRAW_SCALE_TO_RECT"\n'
    )
    if sprite:
        out += f'{p}            sprite: "{sprite}"\n'
    if progress is not None:
        # Both, and in this order, is how the client's own download widget
        # spells it. RadialProgressComponent states the angle; UIClipPolygon
        # is what actually cuts the background to it.
        out += (
            f'{p}        UIClipPolygon: {{}}\n'
            f'{p}        RadialProgressComponent:\n'
            f'{p}            progress: {progress:.6f}\n'
        )
    out += (
        f'{p}        Anchor:\n'
        f'{p}            hCenterAnchorEnabled: true\n'
        f'{p}            vCenterAnchorEnabled: true\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "FixedSize"\n'
        f'{p}            horizontalValue: {RING_SIZE:.6f}\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {RING_SIZE:.6f}\n'
    )
    if sprite_binding:
        out += (f'{p}    bindings:\n'
                f'{p}    - ["Background.sprite", "{sprite_binding}"]\n')
    return out


def progress_ring(indent: int) -> str:
    """The client's own download circle, filled by measurement.

    Nothing can hand a number to a running screen: bindings see only the
    client's own models, and the actions language reads neither files nor
    network. What the screen *can* do is ask for a sprite by path, and the
    installer decides what is at that path - loose files under Data/ resolve,
    which the probe confirmed by drawing a stock ring from ~res:/BLITZFORGE/.

    A single ring bound to `RadialProgressComponent.progress` is therefore out
    of reach: progress is a number, and there is no channel for one. So the
    fraction is spent on *which* ring rather than on how full one ring is -
    LEVELS copies of the circle are stacked, each with its own fill baked in,
    and the installer links the real sprite at exactly one of them. The other
    layers get a transparent sprite and clip nothing visible.

    The result is a genuine radial fill of the game's own ring, positioned by
    real bytes downloaded. What it costs is granularity: the fill can only land
    on one of LEVELS angles, where the stock widget is continuous.
    """
    p = " " * indent
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "ProgressRing"\n'
        f'{p}    size: [{RING_SIZE:.6f}, {RING_SIZE:.6f}]\n'
        f'{p}    input: false\n'
        f'{p}    components:\n'
        f'{p}        Anchor:\n'
        f'{p}            hCenterAnchorEnabled: true\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: {RING_TOP:.6f}\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "FixedSize"\n'
        f'{p}            horizontalValue: {RING_SIZE:.6f}\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {RING_SIZE:.6f}\n'
        f'{p}    children:\n'
    )
    # Drawn first, so it sits under every fill: the unfilled circle, which is
    # what makes nought per cent look like a ring waiting rather than an empty
    # panel.
    out += ring_layer(indent + 4, "RingTrack", "white-wild-sand-50-bg",
                      sprite=RING_SPRITE)
    for i in range(LEVELS):
        out += ring_layer(indent + 4, f"Fill{i}", "orange-tango-bg",
                          progress=(i + 1) / LEVELS,
                          sprite_binding=level_path(i))
    return out


def progress_bar() -> str:
    """The strip the ring and its caption sit in, shown while work is in
    flight."""
    p = " " * 8
    out = (
        f'{p}-   class: "UIControl"\n'
        f'{p}    name: "ModProgress"\n'
        f'{p}    size: [944.000000, {STRIP_HEIGHT:.6f}]\n'
        f'{p}    input: false\n'
        f'{p}    classes: "grey-shark-70-bg"\n'
        f'{p}    components:\n'
        f'{p}        Background:\n'
        f'{p}            drawType: "DRAW_FILL"\n'
        f'{p}        IgnoreLayout: {{}}\n'
        f'{p}        Anchor:\n'
        f'{p}            leftAnchorEnabled: true\n'
        f'{p}            leftAnchor: 40.000000\n'
        f'{p}            rightAnchorEnabled: true\n'
        f'{p}            rightAnchor: 40.000000\n'
        f'{p}            topAnchorEnabled: true\n'
        f'{p}            topAnchor: {STRIP_TOP:.6f}\n'
        f'{p}        SizePolicy:\n'
        f'{p}            horizontalPolicy: "PercentOfParent"\n'
        f'{p}            verticalPolicy: "FixedSize"\n'
        f'{p}            verticalValue: {STRIP_HEIGHT:.6f}\n'
        f'{p}    bindings:\n'
        f'{p}    - ["visible", "modBusy"]\n'
        f'{p}    children:\n'
    )
    # The caption is a child of the strip. Emitted at the strip's own indent it
    # became a sibling instead, anchored to the screen, and landed on top of
    # the catalogue heading.
    out += text_control("Status", "t-caption bold align-left white-wild-sand-text",
                        16, CAPTION_TOP, 500, CAPTION_HEIGHT, "Устанавливаю мод…", 12)
    return out + progress_ring(12)


def confirm_overlay() -> str:
    """A full-screen confirmation for removal.

    Placed last among the screen's children so it draws over the pages, and it
    takes input so a stray tap cannot reach the list behind it.
    """
    def button(name, caption, action, left, tint):
        p = " " * 16
        return (
            f'{p}-   class: "UIControl"\n'
            f'{p}    name: "{name}"\n'
            f'{p}    size: [200.000000, 52.000000]\n'
            f'{p}    classes: "simple-button {tint}"\n'
            f'{p}    components:\n'
            f'{p}        Background: {{}}\n'
            f'{p}        UIOpacityComponent: {{}}\n'
            f'{p}        UIInputEventComponent:\n'
            f'{p}            onTouchUpInside: "ON_CLICK"\n'
            f'{p}        UIDataParamsComponent:\n'
            f'{p}            events:\n'
            f'{p}            - "ON_CLICK"\n'
            f'{p}            eventActions:\n'
            f'{p}            - ["ON_CLICK", "{action}", ""]\n'
            f'{p}        Anchor:\n'
            f'{p}            leftAnchorEnabled: true\n'
            f'{p}            leftAnchor: {left:.6f}\n'
            f'{p}            bottomAnchorEnabled: true\n'
            f'{p}            bottomAnchor: 40.000000\n'
            f'{p}        SizePolicy:\n'
            f'{p}            horizontalPolicy: "FixedSize"\n'
            f'{p}            horizontalValue: 200.000000\n'
            f'{p}            verticalPolicy: "FixedSize"\n'
            f'{p}            verticalValue: 52.000000\n'
            f'{p}    children:\n'
            f'{p}    -   class: "UIControl"\n'
            f'{p}        name: "Caption"\n'
            f'{p}        input: false\n'
            f'{p}        classes: "t-button bold white-wild-sand-text"\n'
            f'{p}        components:\n'
            f'{p}            UITextComponent:\n'
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
            f'{p}        bindings:\n'
            f'{p}        - ["UITextComponent.text", "\\"{caption}\\""]\n'
        )

    head = '''        -   class: "UIControl"
            name: "ConfirmOverlay"
            size: [1024.000000, 768.000000]
            input: true
            components:
                Background:
                    drawType: "DRAW_FILL"
                    color: [0.020000, 0.030000, 0.040000, 0.880000]
                IgnoreLayout: {}
                Anchor:
                    leftAnchorEnabled: true
                    rightAnchorEnabled: true
                    topAnchorEnabled: true
                    bottomAnchorEnabled: true
                SizePolicy:
                    horizontalPolicy: "PercentOfParent"
                    verticalPolicy: "PercentOfParent"
            bindings:
            - ["visible", "modConfirmVisible"]
            children:
            -   class: "UIControl"
                name: "Dialog"
                size: [520.000000, 220.000000]
                input: false
                classes: "grey-shark-80-bg"
                components:
                    Background: {}
                    Anchor:
                        hCenterAnchorEnabled: true
                        vCenterAnchorEnabled: true
                    SizePolicy:
                        horizontalPolicy: "FixedSize"
                        horizontalValue: 520.000000
                        verticalPolicy: "FixedSize"
                        verticalValue: 220.000000
                children:
'''
    head += text_control("Title", "t-subtitle bold align-parent-center white-wild-sand-text",
                         0, 40, 520, 34, "Удалить мод?", 16)
    head += text_control("Body", "t-body regular align-parent-center white-wild-sand-70-text",
                         0, 82, 520, 26, "Файлы клиента вернутся к исходным.", 16)
    head += button("ConfirmRemove", "УДАЛИТЬ", "ON_MOD_REMOVE_CONFIRMED", 40.0,
                   "red-tamarillo-bg")
    head += button("CancelRemove", "ОТМЕНА", "ON_MOD_REMOVE_CANCELLED", 280.0,
                   "grey-shark-60-bg")
    return head


# ------------------------------------------------------------- hiding the UI

# Container draws its children in document order and FadedBlur covers only what
# precedes it, so these two are the only stock controls that survive the blur
# and land on top of the catalog. Everything above FadedBlur needs no help.
HIDE_WHEN_OPEN = ("TanksPanelHolder", "SideBar")


def add_visible_binding(text: str, name: str, expr: str) -> str:
    """Bind `visible` on a stock control, leaving its other keys untouched.

    Placed after `components:` and before `children:`, matching how the game
    writes its own controls. If the control already binds `visible`, the two
    expressions are combined rather than a second, conflicting entry added.
    """
    marker = f'name: "{name}"'
    if text.count(marker) != 1:
        raise SystemExit(f"expected exactly one {name!r}, found {text.count(marker)}")

    lines = text.splitlines(keepends=True)
    i = next(n for n, l in enumerate(lines) if marker in l)
    indent = len(lines[i]) - len(lines[i].lstrip(" "))
    pad = " " * indent

    j = i + 1
    while j < len(lines):
        line = lines[j]
        if not line.strip():
            j += 1
            continue
        ind = len(line) - len(line.lstrip(" "))
        if ind < indent:                     # this control ended
            break
        if ind == indent:
            key = line.strip()
            if key.startswith("bindings:"):
                k = j + 1
                while k < len(lines) and lines[k].strip().startswith("-"):
                    if '"visible"' in lines[k]:
                        old = lines[k].split('", "', 1)[1].rsplit('"]', 1)[0]
                        lines[k] = f'{pad}- ["visible", "({old}) and {expr}"]\n'
                        return "".join(lines)
                    k += 1
                lines.insert(k, f'{pad}- ["visible", "{expr}"]\n')
                return "".join(lines)
            if key.startswith("children:"):
                break
        j += 1

    lines.insert(j, f'{pad}bindings:\n{pad}- ["visible", "{expr}"]\n')
    return "".join(lines)


# ---------------------------------------------------------------- validation

def _logical(line: str):
    """(indent, text, starts_item) with a leading `-` normalised to spaces.

    `-   class: "UIControl"` at column 4 is logically a key at column 8, so
    treating the dash as indentation makes list items and plain keys directly
    comparable.
    """
    indent = len(line) - len(line.lstrip(" "))
    body = line[indent:]
    if body.startswith("-"):
        rest = body[1:]
        pad = len(rest) - len(rest.lstrip(" "))
        return indent + 1 + pad, rest.lstrip(" "), True
    return indent, body, False


def validate(text: str) -> None:
    """Catch structural damage before it reaches the game.

    DAVA gives no parse diagnostics — a malformed screen is a crash on load,
    so the two failure modes we have actually produced are checked here:
    an indent that rises after a line that opened nothing, and a key repeated
    inside one mapping.
    """
    errors = []
    stack: list[tuple[int, set]] = []
    prev_indent, prev_opens = 0, True

    for n, raw in enumerate(text.splitlines(), 1):
        if not raw.strip() or raw.lstrip().startswith("#"):
            continue
        indent, body, is_item = _logical(raw)

        if indent > prev_indent and not prev_opens:
            errors.append(f"line {n}: indent rises after a line that opens no "
                          f"block -> {raw.strip()[:60]!r}")

        while stack and stack[-1][0] > indent:
            stack.pop()
        if not stack or stack[-1][0] < indent:
            stack.append((indent, set()))
        if is_item:                       # a new sibling mapping starts here
            stack[-1] = (indent, set())

        key = body.split(":", 1)[0].strip() if ":" in body else None
        if key and not key.startswith(("[", '"[')):
            if key in stack[-1][1]:
                errors.append(f"line {n}: duplicate key {key!r} in this mapping")
            stack[-1][1].add(key)

        prev_indent = indent
        prev_opens = body.rstrip().endswith(":") or is_item

    if errors:
        raise SystemExit("generated YAML is structurally invalid:\n  "
                         + "\n  ".join(errors))


def check_locals(yaml_text: str, actions_text: str) -> None:
    """Every mod* name read anywhere must be declared on the screen.

    An expression naming an undeclared variable does not raise - it just stops.
    A binding renders nothing and, far worse, an action whose first condition
    names one never runs at all, so the button changes nothing, sends nothing
    and logs nothing. That is indistinguishable from a tap that missed, and it
    cost three rounds of fixing the wrong thing: modPendingInstalled was added
    to the actions and to twelve bindings, and never to the locals list.
    """
    import re

    component = re.search(
        r"(?m)^(?P<indent>[ \t]*)UIDataLocalVarsComponent:\s*$",
        yaml_text,
    )
    if not component:
        raise SystemExit("generated YAML has no UIDataLocalVarsComponent")
    next_component = re.search(
        rf"(?m)^{re.escape(component.group('indent'))}"
        r"[A-Za-z]\w*Component\d*:\s*$",
        yaml_text[component.end():],
    )
    end = (
        component.end() + next_component.start()
        if next_component
        else len(yaml_text)
    )
    block = yaml_text[component.start():end]
    declared = set(re.findall(r'\["(?:bool|int|string|float)", "(mod\w+)"', block))
    used = set(re.findall(r"\b(mod[A-Z]\w+)", yaml_text + actions_text))
    missing = sorted(used - declared)
    if missing:
        raise SystemExit(
            "these names are read but never declared, so anything that "
            "touches them silently does nothing:\n  " + "\n  ".join(missing))


STYLE_SHEETS = ("UI/Screens3/Color.style.yaml", "UI/Styles/BackgroundStyles.yaml")


def check_classes(yaml_text: str) -> None:
    """Every style class the screen names must exist in the client's stylesheets.

    A class the game has never heard of is not an error to it - the control
    simply draws without whatever the class was going to give it. A button
    tinted by a colour class therefore renders as an unpainted rectangle, which
    reads as a layout bug and sends you looking in the wrong file.

    Found the hard way: "blue-endeavour-bg" was a plausible-looking invention.
    The palette lives in Screens3/Color.style.yaml under names like
    "blue-curious-blue-bg", and nothing in the pipeline would have said so.
    """
    import re
    from dvpl import unpack

    known = set()
    for relative in STYLE_SHEETS:
        source = HERE.parent / "Data" / (relative + ".dvpl")
        if not source.exists():
            continue
        text = unpack(source.read_bytes()).decode("utf-8", errors="replace")
        known |= set(re.findall(r'"\.([a-z0-9][a-z0-9-]*)"', text))
        known |= set(re.findall(r'selector: "\.([a-z0-9][a-z0-9-]*)"', text))

    if not known:
        return          # stylesheets unreadable; not a reason to fail the build

    used = set()
    for value in re.findall(r'classes: "([^"]*)"', yaml_text):
        used |= {name for name in value.split() if "-" in name}

    # Only colour and background classes are checked. Layout classes like
    # "t-button" come from sheets this does not read, and guessing about them
    # would turn a useful check into noise.
    suspect = sorted(name for name in used - known
                     if name.endswith(("-bg", "-text", "-border")))
    if suspect:
        raise SystemExit(
            "these style classes are used but defined nowhere, so the controls "
            "wearing them draw untinted:\n  " + "\n  ".join(suspect))


# ------------------------------------------------------------------- splice

LOCALS_ANCHOR = '            - ["bool", "showXpBonusHint", "false"]\n'
BUTTON_ANCHOR = ('                                                    children:\n'
                 '                                                    -   class: "UIControl"\n'
                 '                                                        name: "StoryAggregatedButtonsHolder"\n')

BUTTON_BLOCK = '''                                                    children:
                                                    -   prototype: "IconButtonWithBadge/IconButton"
                                                        name: "ModCatalogButton"
                                                        components:
                                                            UIDataParamsComponent:
                                                                args:
                                                                    "image": "\\"~res:/Gfx/Lobby/icons/icon_settings_n\\""
                                                                    "type": "eButtonType.OPTIONAL_LIGHT"
                                                                    "visible": "true"
                                                                eventActions:
                                                                - ["ON_CLICK_BUTTON", "ON_MOD_CATALOG_CLICKED", ""]
                                                    -   class: "UIControl"
                                                        name: "StoryAggregatedButtonsHolder"
'''


ACTIONS_REL = "UI/Screens3/Lobby/Hangar/Hangar.actions"

# Generated alongside the screen: the card indices the actions receive come
# from the same enumeration that lays the cards out, so the two files cannot
# be allowed to drift apart.
ACTIONS_BLOCK = '''
action ON_MOD_CATALOG_CLICKED()
{
  if (modCatalogVisible)
  {
    ChangeData(modCatalogVisible, false);
    ChangeData(modDetailVisible, false);
    Opacity("**/FadedBlur", 0.0, time=0.2, interpolation=EASE_OUT);
    Event("ENABLE_BLUR", arg1=false);
    Event("PAUSE_HANGAR_SCENE", arg1=false);
  }
  else
  {
    ChangeData(modCatalogVisible, true);
    ChangeData(modDetailVisible, false);
    ChangeData(modRequestSent, false);
    Event("PAUSE_HANGAR_SCENE", arg1=true);
    // Same blur the stock screens fade the hangar out with.
    RenderPostProcess("**/FadedBlur/BlurAndFade/Blur", force=true);
    Event("ENABLE_BLUR", arg1=true);
    Opacity("**/FadedBlur", 1.0, time=0.2, interpolation=EASE_OUT);
  }
}

// Back steps out of the detail page first, and only closes the whole screen
// once the list is what is on show.
action ON_MOD_CATALOG_BACK()
{
  if (modDetailVisible)
  {
    ChangeData(modDetailVisible, false);
    ChangeData(modRequestSent, false);
  }
  else
  {
    Event("ON_MOD_CATALOG_CLICKED");
  }
}

action ON_MOD_CARD_CLICKED(int index)
{
  PlaySound(sound="GUI/buttons/open");
  ChangeData(modDetailIndex, index);
  ChangeData(modRequestSent, false);
  ChangeData(modDetailVisible, true);
}

// The actions language has no file or network access - its whole vocabulary is
// UI and animation - so a button cannot run the installer itself. What it can
// do is request a deliberately missing sprite, whose error reaches the client
// log that agent.py tails. The request includes the catalogue generation as
// well as the card index. Rebuilding files for the next launch therefore
// cannot remap a still-running old screen's index to a different mod.
// A request is a sprite the client cannot find. Asking for
// ~res:/BLITZFORGE/<verb>/<generation>/<index>-<seq> makes the engine log
//   [error] [ConvertedFileSpriteDataLoader] File "..." not found
// and error level does reach blitz-logs_*.txt, where agent.py reads it. The
// sequence number keeps every press a distinct path, so a repeated action is
// not swallowed by the failed-sprite cache.
//
// Drives the circle, but does not decide what it reads. This loop only counts
// polls: each tick is a fresh sprite path, and how full the ring is at that
// path was measured by the installer, not by this timer. So the loop's length
// is a timeout on how long the ring is offered, and nothing about the fill.
//
// It shows no percentage for the same reason it cannot drive the fill
// directly: a number cannot reach a running screen, and one written here
// would be invented.
action ON_MOD_WORKING()
{
  // Every tick is a fresh sprite path, and a path that has nothing behind it
  // draws a pink placeholder rather than nothing. The installer needs a moment
  // to notice the request and lay the first tick down, so the bar waits before
  // showing itself. The button has already flipped, so nothing looks frozen.
  ChangeData(modTick, 0);
  // Long enough for the installer to notice the press and lay tick zero down.
  // It cannot start earlier than this: the way it hears about the press is the
  // failed-sprite line the press itself writes to the client log, so the wait
  // has to cover its poll interval plus however long the client takes to
  // flush. Everything from tick one on has LEAD_SECONDS of its own margin;
  // this is the one frame that has none.
  Wait(0.9);
  ChangeData(modBusy, true);
  repeat(24)
  {
    Wait(0.15);
    ChangeData(modTick, modTick + 1);
  }
  ChangeData(modBusy, false);
}

action ON_MOD_REMOVE_CANCELLED()
{
  PlaySound(sound="GUI/buttons/close");
  ChangeData(modConfirmVisible, false);
}

action ON_MOD_RESTART_CLICKED()
{
  PlaySound(sound="GUI/buttons/open");
  ChangeData(modRequestVerb, 3);
  ChangeData(modRequestSeq, modRequestSeq + 1);
  ChangeData(modRequestSent, true);
}
'''


def mod_actions(mods: list[dict]) -> str:
    """One toggle action per card, plus the dispatcher the confirm dialog uses.

    An action can only ChangeData a name it spells out, so a shared action
    taking the card index cannot write to that card's variable. The index is
    baked in here instead, which is free: the generator already emits one card
    per mod and knows how many there are.
    """
    out = []
    for i, _mod in enumerate(mods):
        out.append(f'''
// Card {i}. Pressing it flips modInstalled{i} straight away, so the button
// reads as the state the request will produce rather than the state on disk -
// the ledger cannot change until the client restarts.
action ON_MOD_TOGGLE_{i}()
{{
  if (modInstalled{i})
  {{
    // Nested rather than "else if": that spelling appears nowhere in the 446
    // stock action files while plain else appears 481 times, and a construct
    // this grammar does not accept would not fail loudly - the action would
    // just stop, and the button would look like it had missed the tap.
    if (modOutdated{i})
    {{
      // Updating is not removing, so it does not ask. Clearing the flag first
      // turns the button back into УДАЛИТЬ at once, which is the state the
      // request is about to produce.
      PlaySound(sound="GUI/buttons/open");
      ChangeData(modOutdated{i}, false);
      ChangeData(modRequestVerb, 4);
      ChangeData(modRequestIndex, {i});
      ChangeData(modRequestSeq, modRequestSeq + 1);
      ChangeData(modRequestSent, true);
      Event("ON_MOD_WORKING");
    }}
    else
    {{
      ChangeData(modConfirmIndex, {i});
      ChangeData(modConfirmVisible, true);
    }}
  }}
  else
  {{
    PlaySound(sound="GUI/buttons/open");
    ChangeData(modInstalled{i}, true);
    ChangeData(modOutdated{i}, false);
    ChangeData(modRequestVerb, 1);
    ChangeData(modRequestIndex, {i});
    ChangeData(modRequestSeq, modRequestSeq + 1);
    ChangeData(modRequestSent, true);
    Event("ON_MOD_WORKING");
  }}
}}
''')

    # The dialog is one control shared by every card, so which variable to
    # clear is only known at the moment it is confirmed. A chain of tests is
    # the way to reach a name the language will not let us compute.
    clears = "\n".join(
        f'  if (modConfirmIndex == {i}) '
        f'{{ ChangeData(modInstalled{i}, false); '
        f'ChangeData(modOutdated{i}, false); }}'
        for i in range(len(mods)))
    out.append(f'''
action ON_MOD_REMOVE_CONFIRMED()
{{
  PlaySound(sound="GUI/buttons/open");
{clears}
  ChangeData(modConfirmVisible, false);
  ChangeData(modRequestVerb, 2);
  ChangeData(modRequestIndex, modConfirmIndex);
  ChangeData(modRequestSeq, modRequestSeq + 1);
  ChangeData(modRequestSent, true);
  Event("ON_MOD_WORKING");
}}
''')
    return "".join(out)


def rebuild_actions(mods: list[dict]) -> str:
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "extract", ACTIONS_REL],
                   check=True, creationflags=NO_WINDOW)
    src = WORK / "Hangar.actions"
    text = src.read_text(encoding="utf-8")
    if "ON_MOD_CATALOG_CLICKED" in text:
        raise SystemExit("extract did not return a pristine Hangar.actions")
    generated = ACTIONS_BLOCK + mod_actions(mods)
    src.write_text(text.rstrip() + "\n" + generated, encoding="utf-8")
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "install", ACTIONS_REL, str(src)],
                   check=True, creationflags=NO_WINDOW)
    return generated


def catalog_generation(mods: list[dict]) -> int:
    """Stable 31-bit identity for the exact index-to-id mapping."""
    order = "\0".join(mod["id"] for mod in mods).encode("utf-8")
    return (zlib.crc32(order) & 0x7fffffff) or 1


def write_catalog_index(mods: list[dict], path: Path | None = None) -> int:
    """Publish current and recent index mappings atomically.

    The generated files can be rebuilt while an older screen is still alive
    in the client. Keeping mappings by generation means that old screen cannot
    accidentally install the mod that moved into the same numeric position.
    """
    if path is None:
        path = HERE / "cache" / "catalog_index.json"
    generation = catalog_generation(mods)
    order = [mod["id"] for mod in mods]
    try:
        previous = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        previous = {}

    catalogs = previous.get("catalogs")
    if not isinstance(catalogs, dict):
        catalogs = {}
    history = [str(value) for value in previous.get("history", [])
               if str(value) in catalogs]
    key = str(generation)
    catalogs[key] = order
    if key in history:
        history.remove(key)
    history.append(key)
    history = history[-8:]
    catalogs = {item: catalogs[item] for item in history}

    legacy_order = previous.get("legacy_order")
    if not isinstance(legacy_order, list):
        old_order = previous.get("order")
        legacy_order = old_order if isinstance(old_order, list) else order
    payload = {
        "version": 2,
        "active": generation,
        "order": order,
        "legacy_order": legacy_order,
        "history": [int(item) for item in history],
        "catalogs": catalogs,
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    temporary.replace(path)
    return generation


def rebuild(dry_run: bool = False, source: str = "registry",
            autostart: bool = True) -> None:
    mods = load_mods(source)
    generation = catalog_generation(mods)
    print(f"mods: {len(mods)}")
    for m in mods:
        print(f"   {m['id']:16} {m['name']}  ({m['type']})")

    screen = build_screen(mods)
    if dry_run:
        print(screen)
        return

    # agent.py sets this before relaunching, so the catalogue is already open
    # when the hangar comes back. Consuming the flag here stops it reopening on
    # every launch from then on.
    flag = HERE / "cache" / "open_catalog_on_load"
    open_on_load = "true" if flag.exists() else "false"
    if flag.exists():
        flag.unlink()
        print("catalog will open on load (requested by a restart)")

    # start from the pristine screen every time
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "extract", HANGAR_REL],
                   check=True, creationflags=NO_WINDOW)
    src = WORK / "Hangar.yaml"
    text = src.read_text(encoding="utf-8", errors="replace")
    if "ModCatalog" in text:
        raise SystemExit("extract did not return a pristine Hangar.yaml")

    for anchor in (LOCALS_ANCHOR, BUTTON_ANCHOR, "Slots:"):
        if anchor not in text:
            raise SystemExit(f"anchor missing from Hangar.yaml: {anchor[:60]!r}")

    text = text.replace(
        LOCALS_ANCHOR,
        LOCALS_ANCHOR
        + f'            - ["bool", "modCatalogVisible", "{open_on_load}"]\n'
        + '            - ["bool", "modDetailVisible", "false"]\n'
        + '            - ["int", "modDetailIndex", "0"]\n'
        + '            - ["bool", "modRequestSent", "false"]\n'
        + '            - ["bool", "modConfirmVisible", "false"]\n'
        + '            - ["int", "modRequestVerb", "0"]\n'
        + '            - ["int", "modRequestIndex", "0"]\n'
        + '            - ["int", "modRequestSeq", "0"]\n'
        + f'            - ["int", "modCatalogGeneration", "{generation}"]\n'
        + '            - ["int", "modConfirmIndex", "0"]\n'
        + '            - ["bool", "modBusy", "false"]\n'
        # Every path a sprite is asked for is cached, so a bar that never
        # changes its paths could only ever show its first frame. The tick
        # makes each poll a new path, which is what lets the installer put
        # something different there.
        + '            - ["int", "modTick", "0"]\n'
        # One per card, seeded from the ledger. These carry the whole install
        # state of the screen: a single screen-wide flag could only remember
        # the card pressed last, so installing a second mod visibly reset the
        # first one's button.
        #
        # Every name read anywhere has to appear in this list. An expression
        # naming an undeclared variable does not fail loudly - it stops, and an
        # action whose condition names one never runs at all. check_locals()
        # below is what keeps that from shipping again.
        + "".join(f'            - ["bool", "modInstalled{i}", "{m["installed"]}"]\n'
                  for i, m in enumerate(mods))
        # Also one per card: whether the registry is offering a later version
        # than the one installed. Decided when the screen is built, which is
        # every time the client starts, so a mod published while the game was
        # closed is already marked by the time the catalogue opens.
        + "".join(f'            - ["bool", "modOutdated{i}", "{m["outdated"]}"]\n'
                  for i, m in enumerate(mods))
        + "".join(f'            - ["bool", "modAvailable{i}", "{m["available"]}"]\n'
                  for i, m in enumerate(mods)), 1)
    text = text.replace(BUTTON_ANCHOR, BUTTON_BLOCK, 1)

    for name in HIDE_WHEN_OPEN:
        text = add_visible_binding(text, name, "not modCatalogVisible")

    text = text.replace("Slots:", screen + "Slots:", 1)

    validate(text)          # never install a screen the game would die on
    # nor one whose buttons quietly do nothing. The actions have to be built
    # before either is installed, because the check spans both files.
    check_locals(text, ACTIONS_BLOCK + mod_actions(mods))
    check_classes(text)     # nor one whose buttons draw without their colour
    src.write_text(text, encoding="utf-8")
    subprocess.run([PYTHON, str(HERE / "patch_dvpl.py"), "install", HANGAR_REL, str(src)],
                   check=True, creationflags=NO_WINDOW)
    rebuild_actions(mods)

    # The card order is the contract between the buttons and agent.py: the
    # button only knows its index, and this turns it back into an id. Publish
    # the index only after both generated client files were validated and
    # installed; otherwise a rejected build can map an old screen to new ids.
    write_catalog_index(mods)

    # The screen is useless without something to carry out what it asks for.
    # Except when this rebuild was spawned BY the thing that carries them out:
    # the agent passes --no-autostart, because the autostart check judges the
    # calling agent by its source stamp and used to taskkill it mid-request -
    # once between "restart" being flagged and the client actually restarting.
    if autostart:
        import agent
        agent.ensure_autostart()

    print(f"catalog rebuilt with {len(mods)} mod(s)")


if __name__ == "__main__":
    rebuild(dry_run="--dry-run" in sys.argv,
            source="local" if "--local" in sys.argv else "registry",
            autostart="--no-autostart" not in sys.argv)
