"""Regression coverage for uninstalling mods the registry says were deleted.

Everything here is about what must *not* happen. The feature deletes files out
of a live game install on the say-so of an unauthenticated HTTP response, so
each case below pins one of the conditions that has to hold before a single
file is touched: a fresh read, a list the server genuinely sent, and an id the
user actually has installed.

Nothing in here reaches the network or the real ledger - the fetch and the
removal are both stubbed, so a failing assert costs a printed line and not
somebody's mods.

    python test_auto_uninstall.py
"""
from __future__ import annotations

import tempfile
import urllib.error
from pathlib import Path

import install
import registry


def response(mod_ids: list[str], removed: object = KeyError) -> dict:
    """A /api/mods body. `removed=KeyError` leaves the key out entirely."""
    payload = {
        "schema": 1,
        "updated": "2026-08-09",
        "count": len(mod_ids),
        "mods": [{"id": mod_id, "name": mod_id, "version": "1.0.0",
                  "type": "native"} for mod_id in mod_ids],
    }
    if removed is not KeyError:
        payload["removed"] = removed
    return payload


def tombstoned(*mod_ids: str) -> list[dict]:
    return [{"id": mod_id, "deleted": "2026-08-09"} for mod_id in mod_ids]


def reconcile(payload: dict, source: str, installed: list[str]) -> tuple:
    """Run the real removal loop against a fake ledger. Returns (gone, left).

    install.remove() un-patches game files, so it is replaced with a stub that
    only records the call and drops the ledger entry - the loop, the ordering
    and every rule in _pending_removals are the genuine code paths.
    """
    ledger = {mod_id: {"version": "1.0.0", "targets": []} for mod_id in installed}
    called: list[str] = []
    original_load_ledger, original_remove = install.load_ledger, install.remove
    try:
        install.load_ledger = lambda: dict(ledger)

        def fake_remove(mod_id: str) -> None:
            called.append(mod_id)
            del ledger[mod_id]

        install.remove = fake_remove
        returned = install.apply_removals(registry.tombstones(payload), source)
    finally:
        install.load_ledger, install.remove = original_load_ledger, original_remove

    assert returned == called, "apply_removals reported something it did not remove"
    return called, sorted(ledger)


# A fresh read uninstalls exactly the tombstones that are installed - no more
# (screen-rain is tombstoned but was never installed) and no less.
fresh = response(["night-mode"], tombstoned("wet-surfaces", "screen-rain", "old-hud"))
gone, left = reconcile(fresh, "registry", ["wet-surfaces", "night-mode", "old-hud"])
assert gone == ["old-hud", "wet-surfaces"], gone
assert left == ["night-mode"], left


# Rule 1: a fetch that fell back to disk decides nothing. The cached payload
# carries the tombstone list that was current when it was written, which is
# exactly the trap - the answer must come from `source`, not from the key.
assert reconcile(fresh, "cache", ["wet-surfaces", "night-mode"]) == (
    [], ["night-mode", "wet-surfaces"])
assert reconcile(fresh, "empty", ["wet-surfaces", "night-mode"]) == (
    [], ["night-mode", "wet-surfaces"])


# Rule 2: no `removed` key is no information. An older server or a rolled-back
# deploy omits it, and that must read as "say nothing", never as "nothing was
# deleted"; a present-but-empty list is the server actually saying the latter.
silent = response(["night-mode"])
assert registry.tombstones(silent) is None
assert reconcile(silent, "registry", ["wet-surfaces"]) == ([], ["wet-surfaces"])

explicit = response(["night-mode"], [])
assert registry.tombstones(explicit) == []
assert reconcile(explicit, "registry", ["wet-surfaces"]) == ([], ["wet-surfaces"])

# Anything that is not a list is not a statement about removals either. A
# truthiness test would let these through to be iterated, and iterating a str
# or a dict yields things that look enough like ids to be acted on.
for malformed in ({"wet-surfaces": "2026-08-09"}, "wet-surfaces", 12, None):
    assert registry.tombstones(response(["night-mode"], malformed)) is None, malformed
assert registry.tombstones(None) is None
# Unrecognised entries inside a real list are skipped, not guessed at.
assert registry.tombstones(response(
    ["night-mode"], ["wet-surfaces", {"id": ""}, {"deleted": "2026-08-09"},
                     {"id": "old-hud"}])) == ["old-hud"]


# Rule 3: absence from `mods` is not deletion. wet-surfaces is installed and is
# not in the published list - it is awaiting re-review - and no tombstone names
# it, so it stays. This is the case that separates a withdrawal from a delete.
assert reconcile(response(["night-mode"], []), "registry",
                 ["wet-surfaces", "night-mode"]) == (
    [], ["night-mode", "wet-surfaces"])
assert reconcile(response([], tombstoned("screen-rain")), "registry",
                 ["wet-surfaces"]) == ([], ["wet-surfaces"])


# The sanity cap: a list that would empty a multi-mod install is refused whole,
# because the shape of that mistake is a bad deploy rather than a real mass
# withdrawal. A single installed mod is the common case and is not capped.
assert reconcile(response([], tombstoned("wet-surfaces", "night-mode")),
                 "registry", ["wet-surfaces", "night-mode"]) == (
    [], ["night-mode", "wet-surfaces"])
assert reconcile(response([], tombstoned("wet-surfaces")), "registry",
                 ["wet-surfaces"]) == (["wet-surfaces"], [])
# One mod surviving is enough for the pass to go ahead as normal.
assert reconcile(response([], tombstoned("wet-surfaces", "old-hud")), "registry",
                 ["wet-surfaces", "old-hud", "night-mode"]) == (
    ["old-hud", "wet-surfaces"], ["night-mode"])


# A mod that will not un-patch does not strand the rest of the list.
def failing_remove_run() -> list[str]:
    ledger = {mod_id: {"version": "1.0.0", "targets": []}
              for mod_id in ("wet-surfaces", "old-hud", "night-mode")}
    original_load_ledger, original_remove = install.load_ledger, install.remove
    try:
        install.load_ledger = lambda: dict(ledger)

        def fake_remove(mod_id: str) -> None:
            if mod_id == "old-hud":
                raise SystemExit("Hangar.yaml is held open by the client")
            del ledger[mod_id]

        install.remove = fake_remove
        return install.apply_removals(
            registry.tombstones(response([], tombstoned("wet-surfaces", "old-hud"))),
            "registry")
    finally:
        install.load_ledger, install.remove = original_load_ledger, original_remove


assert failing_remove_run() == ["wet-surfaces"]


# The plumbing: fetch() keeps its (mods, source) contract for install.py and
# build_catalog.py, fetch_full() adds the payload, and the source it reports is
# what decides whether the tombstones in that payload may be acted on.
with tempfile.TemporaryDirectory(prefix="blitzforge-registry-") as root:
    original_cache, original_get = registry.CACHE, registry._get
    try:
        registry.CACHE = Path(root) / "registry.json"
        served = response(["night-mode"], tombstoned("wet-surfaces"))
        registry._get = lambda url: served

        legacy = registry.fetch()
        assert len(legacy) == 2, "fetch() must still return (mods, source)"
        assert legacy == ([served["mods"][0]], "registry")

        mods, source, payload = registry.fetch_full()
        assert (source, mods) == ("registry", served["mods"])
        assert registry.tombstones(payload) == ["wet-surfaces"]

        # The disk cache holds the response verbatim, tombstones included. That
        # is deliberate, and it is why source is the gate: the same list read
        # back offline must not uninstall anything.
        def unreachable(url: str) -> dict:
            raise urllib.error.URLError("registry unreachable (test)")

        registry._get = unreachable
        cached_mods, cached_source, cached_payload = registry.fetch_full()
        assert (cached_source, cached_mods) == ("cache", served["mods"])
        assert registry.tombstones(cached_payload) == ["wet-surfaces"]
        assert install._pending_removals(
            registry.tombstones(cached_payload), cached_source,
            {"wet-surfaces": {"version": "1.0.0", "targets": []}}) == []

        registry.CACHE = Path(root) / "absent.json"
        empty_mods, empty_source, empty_payload = registry.fetch_full()
        assert (empty_source, empty_mods, empty_payload) == ("empty", [], {})
        assert registry.tombstones(empty_payload) is None
    finally:
        registry.CACHE, registry._get = original_cache, original_get


print("registry tombstone auto-uninstall: all passing")
