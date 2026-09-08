"""Prove the re-anchoring recipes still find every anchor they describe.

This is the guard that makes the recipes trustworthy. A recipe that cannot
re-derive the exact address it was generated from is worse than no recipe at
all, because after a patch it would silently point a hook at the wrong
function. The check therefore runs against the reference build and demands an
exact match for every anchor - not a majority.

It also fails when an anchor is added to the loader sources without a recipe,
so the fixed-RVA surface cannot grow unnoticed.
"""
from pathlib import Path
import sys

TOOLS = Path(__file__).resolve().parent
GAME = TOOLS.parent
sys.path.insert(0, str(TOOLS / "reanchor"))

from anchors import load_anchors  # noqa: E402
from peimage import PEImage  # noqa: E402
from recipes import Recipe, resolve  # noqa: E402

import json  # noqa: E402

RECIPES = TOOLS / "reanchor" / "recipes.json"
assert RECIPES.exists(), "reanchor/recipes.json is missing - run 'generate'"

document = json.loads(RECIPES.read_text(encoding="utf-8"))
recipes = {
    item["anchor_id"]: Recipe.from_json(item) for item in document["recipes"]
}
anchors = load_anchors(TOOLS)
image = PEImage(str(GAME / "wotblitz.exe"))

assert document["reference_sha256"] == image.sha256(), (
    "recipes were generated from a different build than the installed client: "
    f"recipes={document['reference_sha256']} installed={image.sha256()}\n"
    "Re-run: python reanchor/cli.py generate <wotblitz.exe>"
)

missing = [a.id for a in anchors if a.id not in recipes]
assert not missing, (
    "anchors declared in the loader sources have no re-anchoring recipe: "
    + ", ".join(sorted(missing))
)

stale = [name for name in recipes if name not in {a.id for a in anchors}]
assert not stale, (
    "recipes exist for anchors no longer declared in the sources: "
    + ", ".join(sorted(stale))
)

failures: list[str] = []
for anchor in anchors:
    recipe = recipes[anchor.id]
    if recipe.kind == "unresolvable":
        failures.append(f"{anchor.id}: no recipe could be generated")
        continue
    outcome = resolve(recipe, image)
    if not outcome.ok:
        failures.append(f"{anchor.id}: {outcome.reason}")
    elif outcome.rva != anchor.rva:
        failures.append(
            f"{anchor.id}: resolved 0x{outcome.rva:08X}, "
            f"source declares 0x{anchor.rva:08X}"
        )

assert not failures, "re-anchoring recipes failed:\n  " + "\n  ".join(failures)

kinds: dict[str, int] = {}
for recipe in recipes.values():
    kinds[recipe.kind] = kinds.get(recipe.kind, 0) + 1

print(
    f"re-anchoring: {len(anchors)}/{len(anchors)} anchors resolved exactly "
    f"({', '.join(f'{v} {k}' for k, v in sorted(kinds.items()))})"
)
