"""Client for the BlitzForge mod registry.

The catalogue screen is generated from whatever this returns, so a registry
that is unreachable must not stop the hangar from building. Every successful
response is cached to disk and the cache is used when the network fails.

    python registry.py                 # fetch and show what the registry has
    python registry.py --offline       # read the cache only
"""
from __future__ import annotations

import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
CACHE = HERE / "cache" / "registry.json"
DEFAULT_URL = "https://backend-pseudojoker-1s-projects.vercel.app"
TIMEOUT = 10


class RegistryError(RuntimeError):
    pass


def _get(url: str) -> dict:
    request = urllib.request.Request(
        url, headers={"Accept": "application/json", "User-Agent": "blitzforge-tools"})
    with urllib.request.urlopen(request, timeout=TIMEOUT) as response:
        if response.status != 200:
            raise RegistryError(f"{url} -> HTTP {response.status}")
        return json.loads(response.read().decode("utf-8"))


def _read_cache() -> dict | None:
    if not CACHE.exists():
        return None
    try:
        return json.loads(CACHE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def _write_cache(payload: dict) -> None:
    CACHE.parent.mkdir(parents=True, exist_ok=True)
    # Write to a temporary file and replace, so an interrupted run cannot
    # leave a half-written cache that the next offline build would trust.
    temporary = CACHE.with_suffix(".tmp")
    temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2),
                         encoding="utf-8")
    temporary.replace(CACHE)


def fetch_full(base_url: str = DEFAULT_URL,
               offline: bool = False) -> tuple[list[dict], str, dict]:
    """Return (mods, source, payload) - the whole response, not just the mods.

    fetch() below is the two-value form the installer and the catalogue build
    are written against and it keeps working unchanged. Anything that needs the
    rest of the response - the tombstone list, say - asks for it here rather
    than fetching a second time and risking two different answers.

    The payload for source 'cache' is the one on disk, tombstones included:
    _write_cache stores the response verbatim, and stripping keys out of it
    would only move the problem, since a cached list is stale rather than
    absent either way. Whether a `removed` list may be acted on is decided by
    `source`, not by whether the key happens to be there - see
    install._pending_removals.
    """
    if not offline:
        try:
            # The registry is served through a CDN.  A plain request can stay
            # on a stale edge copy after an admin upload, which makes the
            # in-game catalogue lag behind the authoritative Blob registry.
            # Keep the endpoint the same but vary the query key per refresh so
            # every catalogue rebuild observes the current approved set.
            cache_bust = time.time_ns()
            payload = _get(
                base_url.rstrip("/") + f"/api/mods?client_cache={cache_bust}")
            if not isinstance(payload.get("mods"), list):
                raise RegistryError("response has no mods array")
            _write_cache(payload)
            return payload["mods"], "registry", payload
        except (urllib.error.URLError, OSError, json.JSONDecodeError,
                RegistryError) as error:
            print(f"registry unreachable ({error}); falling back to cache",
                  file=sys.stderr)

    cached = _read_cache()
    if cached and isinstance(cached.get("mods"), list):
        return cached["mods"], "cache", cached
    return [], "empty", {}


def fetch(base_url: str = DEFAULT_URL, offline: bool = False) -> tuple[list[dict], str]:
    """Return (mods, source) where source is 'registry', 'cache' or 'empty'."""
    mods, source, _payload = fetch_full(base_url, offline)
    return mods, source


def tombstones(payload: object) -> list[str] | None:
    """Mod ids the registry reports as deleted, or None if it said nothing.

    None and [] are different answers and must stay different:

      None  the response carried no usable `removed` key. An older server, a
            rolled-back deploy or a truncated body all look like this. It means
            "no information about removals", not "nothing was removed".
      []    the key was there and was empty. The server is stating, on the
            record, that nothing is currently deleted.

    Both end up removing nothing, which is exactly why the distinction is
    tempting to flatten into `if not payload.get("removed"):`. Do not. Two
    separate things break when it goes:

      * A falsy test accepts any non-list - a dict, a string - and hands it to
        the loop below, and iterating a str yields its characters while
        iterating a dict yields its keys. A `"removed": {"wet-surfaces": ...}`
        typo on the server would then read as a list of ids to delete. The
        isinstance check, not the truthiness, is what refuses that.
      * The caller can no longer say in its log whether the server declined to
        comment or declared the list empty, which is the first thing anyone
        asks when a mod does or does not disappear.

    Entries that are not the documented {"id": ..., "deleted": ...} object are
    skipped rather than guessed at. For a list whose whole purpose is to delete
    files off a user's disk, "I do not recognise this" has to mean "do nothing".
    """
    if not isinstance(payload, dict):
        return None
    removed = payload.get("removed")
    if not isinstance(removed, list):
        return None
    ids = []
    for entry in removed:
        if isinstance(entry, dict) and isinstance(entry.get("id"), str) and entry["id"]:
            ids.append(entry["id"])
    return ids


def main() -> None:
    mods, source, payload = fetch_full(offline="--offline" in sys.argv)
    print(f"source: {source}   mods: {len(mods)}")
    for mod in mods:
        artifact = mod.get("artifact")
        state = "artifact" if artifact else "metadata only"
        print(f"  {mod['id']:16} {mod['version']:8} {mod['type']:9} {state}")

    deleted = tombstones(payload)
    if deleted is None:
        print("removed: not reported by this response")
    else:
        print(f"removed: {len(deleted)}")
        for mod_id in sorted(deleted):
            print(f"  {mod_id}")


if __name__ == "__main__":
    main()
