"""Regression coverage for catalogue install/update/remove state."""
from __future__ import annotations

import json
import tempfile
from pathlib import Path

import agent
import build_catalog
import install


def offered(mod_id: str, version: str = "1.0.0", artifact: bool = True) -> dict:
    entry = {
        "id": mod_id,
        "name": mod_id,
        "version": version,
        "author": "test",
        "description": "test",
        "long": "test",
        "type": "native",
        "downloads": 1,
        "updated": "01.08.2026",
    }
    if artifact:
        entry["artifact"] = {"url": f"https://example.test/{mod_id}.zip",
                             "sha256": "a" * 64}
    return entry


# Registry metadata never overrides the installed ledger.
current = build_catalog.catalog_entry(
    offered("current"), {"current": {"version": "1.0.0"}})
outdated = build_catalog.catalog_entry(
    offered("outdated", "2.0.0"), {"outdated": {"version": "1.0.0"}})
available = build_catalog.catalog_entry(offered("available"), {})
unavailable = build_catalog.catalog_entry(offered("unavailable", artifact=False), {})
empty_record = build_catalog.catalog_entry(offered("empty-record"), {"empty-record": {}})

assert (current["installed"], current["outdated"], current["available"]) == (
    "true", "false", "true")
assert (outdated["installed"], outdated["outdated"]) == ("true", "true")
assert (available["installed"], available["available"]) == ("false", "true")
assert (unavailable["installed"], unavailable["available"]) == ("false", "false")
assert empty_record["installed"] == "true"


# Local authoring manifests cannot lie about installed/outdated state.
with tempfile.TemporaryDirectory(prefix="blitzforge-catalog-local-") as root:
    mods_dir = Path(root)
    manifest_dir = mods_dir / "local-test"
    manifest_dir.mkdir()
    (manifest_dir / "manifest.yaml").write_text(
        "\n".join((
            "id: local-test",
            "version: 2.0.0",
            "installed: true",
            "outdated: false",
            "available: true",
            "")),
        encoding="utf-8")
    local_absent = build_catalog.load_local_mods(mods_dir, {})[0]
    local_old = build_catalog.load_local_mods(
        mods_dir, {"local-test": {"version": "1.0.0"}})[0]
    assert (local_absent["installed"], local_absent["outdated"]) == (
        "false", "false")
    assert (local_old["installed"], local_old["outdated"]) == ("true", "true")


mods = [current, outdated, available, unavailable]
screen = build_catalog.build_screen(mods)


def section(text: str, start: str, end: str) -> str:
    return text.split(start, 1)[1].split(end, 1)[0]


# A card opens a data scope. All three state inputs must cross it before the
# nested button can see them; direct modOutdatedN access from there is invalid.
card_one = section(screen, 'name: "Card1"', 'name: "Card2"')
assert '- ["bool", "shown", "false", "false"]' in card_one
assert '- ["bool", "stale", "false", "false"]' in card_one
assert '- ["bool", "available", "false", "false"]' in card_one
assert '"shown": "modInstalled1"' in card_one
assert '"stale": "modOutdated1"' in card_one
assert '"available": "modAvailable1"' in card_one
assert '"shown": "shown"' in card_one
assert '"stale": "stale"' in card_one
assert '"available": "available"' in card_one

# The detail page has no intermediate data scope and binds the same variables
# directly, so list and detail always show the same action.
detail_one = section(screen, 'name: "DetailPage1"', 'name: "DetailPage2"')
assert '"shown": "modInstalled1"' in detail_one
assert '"stale": "modOutdated1"' in detail_one
assert '"available": "modAvailable1"' in detail_one

multiline = dict(current, long="first\nsecond")
multiline_detail = build_catalog.detail_page(multiline, 0)
assert '\\"first\\nsecond\\"' in multiline_detail
assert "first\\\\nsecond" not in multiline_detail

# Every face is now also gated on `managed`: a mod the installer has no record
# of gets УСТАНОВЛЕН and no input at all, rather than a red УДАЛИТЬ that sends
# a request nothing can service.
assert '["input", "(shown or available) and managed"]' in screen
assert 'name: "ModCatalogStateMarker"' in screen
assert '["input", "modCatalogVisible"]' in screen
assert '["visible", "available and not shown and managed"]' in screen
assert '["visible", "shown and not stale and managed"]' in screen
assert '["visible", "shown and stale and managed"]' in screen
assert '["visible", "not available and not shown and managed"]' in screen
assert '["visible", "shown and not managed"]' in screen

actions = build_catalog.mod_actions(mods)
assert "ChangeData(modInstalled2, true);" in actions
assert "ChangeData(modOutdated2, false);" in actions
assert ("ChangeData(modInstalled1, false); "
        "ChangeData(modOutdated1, false);") in actions
assert "BLITZFORGE:install" not in actions
assert "BLITZFORGE:remove" not in actions


# A generation pins an in-memory screen to the order it was built with, even
# after a later rebuild publishes a different order for the next launch.
with tempfile.TemporaryDirectory(prefix="blitzforge-catalog-index-") as root:
    index_path = Path(root) / "catalog_index.json"
    first = [build_catalog.catalog_entry(offered("a"), {}),
             build_catalog.catalog_entry(offered("b"), {})]
    second = [first[1], first[0]]
    first_generation = build_catalog.write_catalog_index(first, index_path)
    second_generation = build_catalog.write_catalog_index(second, index_path)
    payload = json.loads(index_path.read_text(encoding="utf-8"))
    assert payload["catalogs"][str(first_generation)] == ["a", "b"]
    assert payload["catalogs"][str(second_generation)] == ["b", "a"]

    original_index = agent.INDEX
    try:
        agent.INDEX = index_path
        assert agent.load_index(first_generation) == ["a", "b"]
        assert agent.load_index(second_generation) == ["b", "a"]
        assert agent.load_index() == ["a", "b"]  # pre-generation screen
    finally:
        agent.INDEX = original_index

    duplicated = (
        f'BLITZFORGE/1/{first_generation}/0-7\n'
        f'BLITZFORGE/1/{first_generation}/0-7\n'
        'BLITZFORGE:install:0\n')
    assert agent.parse_requests(duplicated) == [
        ("install", 0, 7, first_generation)]
    assert agent.parse_requests("BLITZFORGE/2/1-9\nBLITZFORGE:remove:1") == [
        ("remove", 1, 9, None)]


# Ledger writes are atomic and corrupt state is never reinterpreted as "none".
with tempfile.TemporaryDirectory(prefix="blitzforge-ledger-") as root:
    ledger_path = Path(root) / "installed.json"
    install.save_ledger({"a": {"version": "1.0.0", "targets": []}}, ledger_path)
    assert install.load_ledger(ledger_path)["a"]["version"] == "1.0.0"
    assert not ledger_path.with_suffix(".json.tmp").exists()
    ledger_path.write_text("{", encoding="utf-8")
    try:
        install.load_ledger(ledger_path)
    except install.LedgerError:
        pass
    else:
        raise AssertionError("corrupt ledger was treated as an empty install set")


# Update installs/verifies the offered artifact before removing old-only files.
old_record = {"version": "1.0.0", "targets": ["old", "shared"]}
new_record = {"version": "2.0.0", "targets": ["shared", "new"]}
state = {"safe-update": old_record}
events = []
original_load_ledger = install.load_ledger
original_install = install.install
original_remove_targets = install._remove_targets
try:
    install.load_ledger = lambda: state

    def fake_install(mod_id: str) -> None:
        events.append(("install", mod_id))
        state[mod_id] = new_record

    def fake_remove_targets(record: dict, keep=None) -> None:
        events.append(("cleanup", record, keep))

    install.install = fake_install
    install._remove_targets = fake_remove_targets
    install.update("safe-update")
finally:
    install.load_ledger = original_load_ledger
    install.install = original_install
    install._remove_targets = original_remove_targets

assert events[0] == ("install", "safe-update")
assert events[1] == ("cleanup", old_record, {"shared", "new"})


# --------------------------------------------------------------------------
# The catalogue screen has TWO sections: what the registry publishes, and what
# the player put in the game folder. `--local` used to REPLACE the registry
# list, so a mod dropped into _mod_tools/mods/ was invisible unless the
# catalogue was rebuilt in a mode that hid everything else.
# --------------------------------------------------------------------------


def _mod(mod_id: str, section: str) -> dict:
    entry = build_catalog.catalog_entry(offered(mod_id), {}, local=False)
    entry["section"] = section
    return entry


screen = build_catalog.build_screen([
    _mod("published", build_catalog.CATALOG_SECTION),
    _mod("dropped-in", build_catalog.CUSTOM_SECTION),
])
assert 'name: "Section_catalog"' in screen, "the published section has a heading"
assert 'name: "Section_custom"' in screen, "and so does the custom one"
assert "КАТАЛОГ" in screen and "КАСТОМНЫЕ МОДЫ" in screen, (
    "each heading says which section it is")

# A heading is a SIBLING control, never a list entry: every binding a card
# carries is named by its index in `mods`, so a heading that consumed one
# would shift every card's data off by one.
assert 'name: "Card0"' in screen and 'name: "Card1"' in screen, (
    "the cards keep their positions")
assert screen.index('name: "Section_catalog"') < screen.index('name: "Card0"')
assert screen.index('name: "Card0"') < screen.index('name: "Section_custom"')
assert screen.index('name: "Section_custom"') < screen.index('name: "Card1"')

# NO HEADING WHERE THERE IS NO SECTION. A player with nothing of their own
# should not be shown an empty "custom" shelf.
only_published = build_catalog.build_screen(
    [_mod("published", build_catalog.CATALOG_SECTION)])
assert 'name: "Section_custom"' not in only_published, (
    "an empty section has no heading")

# ONE CARD PER MOD. A local manifest for something the registry also publishes
# is dropped: the published entry is the one with an artifact and a version to
# compare against, and two cards for one mod is two install buttons whose
# state disagrees the moment one of them is pressed.
_dir = Path(tempfile.mkdtemp()) / "mods"
(_dir / "published").mkdir(parents=True)
(_dir / "published" / "manifest.yaml").write_text(
    "id: published" + chr(10) + "name: published" + chr(10) +
    "version: 9.9.9" + chr(10), encoding="utf-8")
_local = build_catalog.load_local_mods(_dir, ledger={})
assert [m["id"] for m in _local] == ["published"], (
    "the local manifest reads on its own")

_published = [dict(build_catalog.catalog_entry(offered("published"), {}),
                   section=build_catalog.CATALOG_SECTION)]
_known = {m["id"] for m in _published}
_custom = [dict(m, section=build_catalog.CUSTOM_SECTION)
           for m in _local if m["id"] not in _known]
assert _custom == [], (
    "and is not repeated under Custom when the registry already carries it")


# --------------------------------------------------------------------------
# The Custom section is named for the PLAYER, not for a directory. It listed
# only _mod_tools/mods/ - BlitzForge artifacts - so a Lua mod in
# <game>/mods/lua/ never appeared, and the section showed one validation
# package while the player's own mod ran invisibly beside it.
# --------------------------------------------------------------------------

_lua_root = Path(tempfile.mkdtemp()) / "lua"
(_lua_root / "blitzforge.skin_atelier").mkdir(parents=True)
(_lua_root / "blitzforge.skin_atelier" / "manifest.json").write_text(
    json.dumps({"id": "blitzforge.skin_atelier",
                "name": "BlitzForge Skin Atelier",
                "version": "0.1.0",
                "entrypoint": "main.lua"}),
    encoding="utf-8")

_lua = build_catalog.load_lua_mods(_lua_root, {})
assert [m["id"] for m in _lua] == ["blitzforge.skin_atelier"], (
    "a Lua mod in the game folder is found: %r" % (_lua,))
assert _lua[0]["name"] == "BlitzForge Skin Atelier"
# INSTALLED, because it is - the host loads what is in this directory - and
# NOT MANAGED, because nothing in this catalogue can add or remove one.
assert _lua[0]["installed"] == "true", "it is running, so it says so"
assert _lua[0]["managed"] == "false", "and the catalogue cannot act on it"
assert _lua[0]["available"] == "false", "there is no artifact to install"

# A BUTTON THAT CANNOT ACT DOES NOT TAKE INPUT. Offering УДАЛИТЬ for a mod the
# installer has no record of would send a request nothing can service, and the
# card would sit there looking pressed.
_card = build_catalog.card(dict(_lua[0], section=build_catalog.CUSTOM_SECTION), 0)
assert '"managed": "false"' in _card, "the card carries the flag"
assert '"input", "(shown or available) and managed"' in _card, (
    "and the button stops taking input")
assert 'name: "FacePresent"' in _card, "with a face that says УСТАНОВЛЕН"
assert "УСТАНОВЛЕН" in _card

_managed_card = build_catalog.card(
    dict(build_catalog.catalog_entry(offered("published"), {"published": {}}),
         section=build_catalog.CATALOG_SECTION), 1)
assert '"managed": "true"' in _managed_card, (
    "an artifact mod is still managed, and still removable")

# A manifest that will not parse is NAMED, not skipped in silence: it is a mod
# the player believes is installed, and a catalogue that simply omits it
# answers no question they might ask.
(_lua_root / "broken").mkdir()
(_lua_root / "broken" / "manifest.json").write_text("{oops", encoding="utf-8")
_after = build_catalog.load_lua_mods(_lua_root, {})
assert [m["id"] for m in _after] == ["blitzforge.skin_atelier"], (
    "the broken one is not listed, and the good one still is")

# BOTH LAYERS, ONE SECTION - and this checks the WIRING, not the two readers.
# An earlier version of this test asked only whether `custom_mods` returned a
# list of unique ids, which stayed true with the Lua layer unplugged: the
# reader was built, tested, and connected to nothing.
_artifacts = Path(tempfile.mkdtemp()) / "mods"
(_artifacts / "an-artifact").mkdir(parents=True)
(_artifacts / "an-artifact" / "manifest.yaml").write_text(
    "id: an-artifact" + chr(10) + "name: An Artifact" + chr(10), encoding="utf-8")
_both = build_catalog.custom_mods({}, mods_dir=_artifacts, lua_dir=_lua_root)
assert [m["id"] for m in _both] == ["an-artifact", "blitzforge.skin_atelier"], (
    "both layers reach the section: %r" % ([m["id"] for m in _both],))
assert len({m["id"] for m in _both}) == len(_both), "no id appears twice"
# Asked through `managed_literal`, which is what the card actually calls. An
# artifact carries no `managed` key at all - absent means managed - so reading
# the key directly would be testing a different rule from the one that ships.
_by_id = {m["id"]: m for m in _both}
assert build_catalog.managed_literal(_by_id["an-artifact"]) == "true", (
    "an artifact stays managed")
assert build_catalog.managed_literal(_by_id["blitzforge.skin_atelier"]) == "false", (
    "and a Lua mod does not")


print("catalog state: all passing")
