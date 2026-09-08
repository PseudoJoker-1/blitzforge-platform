"""Re-anchoring CLI.

    python -m reanchor.cli generate <known-good.exe>
    python -m reanchor.cli verify   <known-good.exe>
    python -m reanchor.cli resolve  <new.exe>

`generate` derives locator recipes from the build the binding pack was
authored against. `verify` proves those recipes find the exact same addresses
again - if that fails, the recipes are wrong and must never be trusted on a
new build. `resolve` replays them against a patched client and reports the new
RVA table plus everything that could not be found.

Nothing here writes to the loader sources. Re-anchoring produces a report and
a candidate table; promoting it into a binding pack stays a reviewed step,
because a wrong address is a crash in someone else's game.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

from anchors import load_anchors, load_struct_offsets  # noqa: E402
from peimage import PEImage  # noqa: E402
from recipes import (  # noqa: E402
    Recipe,
    generate_code_recipe,
    generate_data_recipe,
    resolve,
)

MOD_TOOLS_ROOT = _HERE.parent
RECIPES_PATH = _HERE / "recipes.json"

# Below this, the result is not a re-anchor at all and nothing is written.
HARD_REJECT_FRACTION = 0.50
# Below this, the table is a research starting point rather than a candidate.
TRUST_FRACTION = 0.90


def _load_image(path: str) -> PEImage:
    image = PEImage(path)
    print(f"image      : {path}")
    print(f"sha256     : {image.sha256()}")
    print(f"image base : 0x{image.image_base:08X}")
    return image


def command_generate(args: argparse.Namespace) -> int:
    image = _load_image(args.executable)
    anchors = load_anchors(MOD_TOOLS_ROOT)
    offsets = load_struct_offsets(MOD_TOOLS_ROOT)
    code_sections = image.code_sections()
    print(f"anchors    : {len(anchors)} ({len(offsets)} struct offsets tracked)")
    print()

    recipes: list[Recipe] = []
    failures = 0
    for anchor in anchors:
        section = image.section_for_rva(anchor.rva)
        in_code = section is not None and (section.is_code or section.is_executable)
        if in_code:
            recipe = generate_code_recipe(
                anchor.id, anchor.rva, image, code_sections
            )
        else:
            recipe = generate_data_recipe(
                anchor.id, anchor.rva, image, code_sections
            )
        recipes.append(recipe)
        if recipe.kind == "unresolvable":
            failures += 1
            print(f"  FAIL  {anchor.id:<34} 0x{anchor.rva:08X}  {recipe.note}")

    document = {
        "schema": "wotbmod.reanchor-recipes/v1",
        "reference_sha256": image.sha256(),
        "reference_image_base": f"0x{image.image_base:08X}",
        "anchor_count": len(anchors),
        "resolvable": len(anchors) - failures,
        "recipes": [recipe.to_json() for recipe in recipes],
        "struct_offsets": [offset.to_json() for offset in offsets],
    }
    RECIPES_PATH.write_text(
        json.dumps(document, indent=2) + "\n", encoding="utf-8"
    )
    print()
    print(
        f"generated  : {len(anchors) - failures}/{len(anchors)} recipes "
        f"-> {RECIPES_PATH.name}"
    )
    if failures:
        print(f"unresolved : {failures} anchor(s) need a hand-written recipe")
    return 0


def _read_recipes() -> tuple[dict, list[Recipe]]:
    if not RECIPES_PATH.exists():
        raise SystemExit(
            f"{RECIPES_PATH.name} not found - run 'generate' first"
        )
    document = json.loads(RECIPES_PATH.read_text(encoding="utf-8"))
    return document, [Recipe.from_json(item) for item in document["recipes"]]


def command_verify(args: argparse.Namespace) -> int:
    image = _load_image(args.executable)
    document, recipes = _read_recipes()
    anchors = {anchor.id: anchor for anchor in load_anchors(MOD_TOOLS_ROOT)}

    if document["reference_sha256"] != image.sha256():
        print()
        print("NOTE: this is not the build the recipes were generated from.")
        print("      'verify' only proves correctness on the reference build.")

    exact = 0
    wrong = 0
    missing = 0
    skipped = 0
    print()
    for recipe in recipes:
        anchor = anchors.get(recipe.anchor_id)
        if anchor is None:
            print(f"  STALE {recipe.anchor_id}: no longer declared in sources")
            continue
        if recipe.kind == "unresolvable":
            skipped += 1
            continue
        outcome = resolve(recipe, image)
        if not outcome.ok:
            missing += 1
            print(
                f"  MISS  {recipe.anchor_id:<34} {outcome.reason}"
                + (f" ({outcome.candidates} candidates)" if outcome.candidates else "")
            )
        elif outcome.rva != anchor.rva:
            wrong += 1
            print(
                f"  WRONG {recipe.anchor_id:<34} "
                f"expected 0x{anchor.rva:08X}, resolved 0x{outcome.rva:08X}"
            )
        else:
            exact += 1

    total = len(recipes)
    print()
    print(f"exact      : {exact}/{total}")
    if skipped:
        print(f"no recipe  : {skipped}")
    if missing:
        print(f"not found  : {missing}")
    if wrong:
        print(f"WRONG      : {wrong}")
    return 1 if (wrong or missing) else 0


def command_resolve(args: argparse.Namespace) -> int:
    image = _load_image(args.executable)
    document, recipes = _read_recipes()
    anchors = {anchor.id: anchor for anchor in load_anchors(MOD_TOOLS_ROOT)}

    if document["reference_sha256"] == image.sha256():
        print()
        print("This is the reference build; nothing to re-anchor.")
        return 0

    resolved: dict[str, int] = {}
    unresolved: list[tuple[str, str]] = []
    print()
    for recipe in recipes:
        if recipe.kind == "unresolvable":
            unresolved.append((recipe.anchor_id, "no recipe"))
            continue
        outcome = resolve(recipe, image)
        if outcome.ok:
            resolved[recipe.anchor_id] = outcome.rva
        else:
            unresolved.append((recipe.anchor_id, outcome.reason))

    moved = 0
    for anchor_id, rva in sorted(resolved.items()):
        anchor = anchors.get(anchor_id)
        old = anchor.rva if anchor else 0
        if old != rva:
            moved += 1
        marker = " " if old == rva else "*"
        print(f" {marker} {anchor_id:<34} 0x{old:08X} -> 0x{rva:08X}")

    for anchor_id, reason in unresolved:
        print(f" ! {anchor_id:<34} UNRESOLVED: {reason}")

    total = len(recipes)
    fraction = len(resolved) / total if total else 0.0

    # A handful of anchors are CRT boilerplate (operator new and friends) whose
    # signature matches in any binary built with the same toolchain. Judging a
    # re-anchor by individual hits would therefore accept nonsense: pointed at
    # an unrelated executable, the scan still "finds" one or two. Only the
    # aggregate is meaningful.
    if fraction < HARD_REJECT_FRACTION:
        print()
        print(
            f"REJECTED: only {len(resolved)}/{total} anchors resolved "
            f"({fraction:.0%}). This is not a re-anchor - either the target is "
            "not this game, or the build diverged far enough that the recipes "
            "must be re-derived from a fresh manual pass."
        )
        print("No candidate table was written.")
        return 2

    trustworthy = fraction >= TRUST_FRACTION
    output = {
        "schema": "wotbmod.reanchor-result/v1",
        "executable_sha256": image.sha256(),
        "image_base": f"0x{image.image_base:08X}",
        "resolved_fraction": round(fraction, 4),
        "trustworthy": trustworthy,
        "resolved": {k: f"0x{v:08X}" for k, v in sorted(resolved.items())},
        "unresolved": [
            {"anchor_id": a, "reason": r} for a, r in unresolved
        ],
        "struct_offsets_need_manual_review": document.get("struct_offsets", []),
    }
    destination = Path(args.output) if args.output else _HERE / "reanchor_result.json"
    destination.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")

    print()
    print(f"resolved   : {len(resolved)}/{total}  ({moved} moved)")
    print(f"unresolved : {len(unresolved)}")
    print(f"written    : {destination}")
    if not trustworthy:
        print()
        print(
            f"WARNING: {fraction:.0%} resolved, below the {TRUST_FRACTION:.0%} "
            "bar. Treat this table as a starting point for manual work, not as "
            "a binding pack."
        )
    print()
    print(
        "Struct field offsets are NOT re-anchored by scanning and must be "
        "re-confirmed by hand before this table is promoted to a binding pack."
    )
    return 1 if unresolved else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="reanchor",
        description="Re-anchor fixed-RVA bindings after a client patch.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    for name, handler, help_text in (
        ("generate", command_generate, "derive recipes from a known-good build"),
        ("verify", command_verify, "prove recipes re-find the reference addresses"),
        ("resolve", command_resolve, "apply recipes to a patched client"),
    ):
        sub = subparsers.add_parser(name, help=help_text)
        sub.add_argument("executable", help="path to wotblitz.exe")
        if name == "resolve":
            sub.add_argument("-o", "--output", help="result JSON path")
        sub.set_defaults(handler=handler)

    args = parser.parse_args(argv)
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
