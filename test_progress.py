"""Checks the two halves of the progress ring still describe the same thing.

The screen and the installer are written in different languages and neither
can see the other. They agree only by convention: how many fill levels there
are, which fraction each level stands for, what the sprite paths look like,
and how fast the ticks go. If those drift the ring does not fail loudly - it
shows pink squares, a frozen picture from the last install, or, worst of all,
an arc that looks plausible and is wrong about how much has downloaded.

    python test_progress.py
"""
from __future__ import annotations

import re
import sys
import tempfile
from pathlib import Path

import agent as agent_module
import build_catalog
import progress

HERE = Path(__file__).resolve().parent
FAILURES: list[str] = []

# Anything between a control's name and the property being read, as long as it
# stays inside that control. Without the lookahead a control missing the
# property silently borrows the next one's.
WITHIN = r'(?:(?!name: ").)*?'


def check(name: str, condition: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if condition else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not condition:
        FAILURES.append(name)


def main() -> int:
    screen = build_catalog.build_screen(build_catalog.load_mods("local")
                                  or [{"id": "x", "name": "x", "author": "a",
                                       "description": "d", "long": "l",
                                       "downloads": "0", "updated": "",
                                       "type": "resource", "installed": "false",
                                       "outdated": "false", "available": "false"}])
    actions = build_catalog.ACTIONS_BLOCK
    levels = progress.LEVELS

    print("catalog locals")
    long_locals = (
        "        UIDataLocalVarsComponent:\n"
        "            data:\n"
        + "".join(
            f'            - ["bool", "filler{i}", "false"]\n'
            for i in range(80)
        )
        + '            - ["bool", "modInstalled10", "false"]\n'
        + '            - ["bool", "modOutdated10", "false"]\n'
        + "        UIDataLocalBindingsComponent:\n"
        + "            data:\n"
    )
    try:
        build_catalog.check_locals(
            long_locals,
            "if (modInstalled10) { ChangeData(modOutdated10, false); }",
        )
        locals_check_passed = True
    except SystemExit:
        locals_check_passed = False
    check(
        "validator reads the complete locals component for large catalogs",
        locals_check_passed,
        "a fixed byte window truncates declarations after the first cards",
    )

    print("level count")
    check("screen stacks as many fill levels as the installer chooses between",
          build_catalog.LEVELS == levels,
          f"{build_catalog.LEVELS} vs {levels}")
    fills = sorted((int(i), float(v)) for i, v in re.findall(
        rf'name: "Fill(\d+)"{WITHIN}progress: ([\d.]+)', screen, re.DOTALL))
    check("every fill level was parsed out of the generated screen",
          len(fills) == levels, f"{len(fills)} vs {levels}")

    print("radial fill")
    # The whole design rests on these three being true together: each layer is
    # a real RadialProgressComponent (not a rectangle pretending), it is
    # actually clipped to that angle, and the angle it names is the level the
    # installer means by that index. Any one of them alone proves nothing.
    if len(fills) == levels:
        # To six decimals, which is all the YAML carries. Comparing against the
        # exact ratio instead fails on the third level and every level after
        # it, for a rounding the screen could not have avoided.
        check("each level's fill is its own index, ascending to a full circle",
              all(value == round((i + 1) / levels, 6) for i, value in fills),
              f"{[v for _, v in fills][:4]}... expected "
              f"{[round((i + 1) / levels, 6) for i in range(4)]}...")
        clipped = re.findall(
            rf'name: "Fill(\d+)"{WITHIN}UIClipPolygon', screen, re.DOTALL)
        check("every fill is clipped, or its ring draws whole at any level",
              len(clipped) == levels, f"{len(clipped)} of {levels}")
    radial = len(re.findall(r"RadialProgressComponent:", screen))
    check("only the fills are radial, never the track behind them",
          radial == levels, f"{radial} components for {levels} fills")
    track = re.search(rf'name: "RingTrack"{WITHIN}sprite: "([^"]+)"',
                      screen, re.DOTALL)
    check("the track names the stock ring outright", track is not None)
    if track:
        check("the track draws the sprite the installer links",
              track.group(1) == build_catalog.RING_SPRITE,
              f"{track.group(1)} vs {build_catalog.RING_SPRITE}")
        check("that sprite resolves to the file the installer links from",
              progress.RING.name.startswith(track.group(1).rsplit("/", 1)[-1]),
              f"{progress.RING.name} does not begin {track.group(1)!r}")

    print("ring geometry")
    ring = re.search(rf'name: "ProgressRing"{WITHIN}topAnchor: ([\d.]+)',
                     screen, re.DOTALL)
    check("the ring's anchor was found", ring is not None)
    if ring:
        top = float(ring.group(1))
        check("the ring fits inside the strip",
              top + build_catalog.RING_SIZE <= build_catalog.STRIP_HEIGHT,
              f"{top} + {build_catalog.RING_SIZE} vs {build_catalog.STRIP_HEIGHT}")
        caption = re.search(rf'name: "Status"{WITHIN}topAnchor: ([\d.]+)'
                            rf'{WITHIN}verticalValue: ([\d.]+)', screen, re.DOTALL)
        check("the caption's anchor was found", caption is not None)
        if caption:
            check("the ring starts below the caption, so the fill never "
                  "draws over the label",
                  float(caption.group(1)) + float(caption.group(2)) <= top)
    # Centred rather than placed at a computed x: the strip is a percentage of
    # a screen whose real width is not the 1024 it is designed against, so an
    # arithmetic centre would be off by half the difference.
    check("the ring is centred by the engine, not by arithmetic",
          re.search(rf'name: "ProgressRing"{WITHIN}hCenterAnchorEnabled: true',
                    screen, re.DOTALL) is not None)

    print("busy-state layout")
    # The list and detail pages both drop out of the ring's way while it (or
    # the restart button sharing its slot) is on screen. Two independently
    # hand-typed numbers is exactly how the old linear bar's 176 could have
    # gone stale here too, so this checks they are still the same number and
    # that it actually clears the ring rather than being a stale guess.
    offsets = set(re.findall(r'modBusy -> (\d+), modRequestSent -> 176, 104', screen))
    check("list and detail pages share one busy-state offset for the ring",
          len(offsets) == 1, f"found {offsets!r}")
    if offsets:
        check("the offset clears the ring's actual height",
              int(next(iter(offsets))) >= build_catalog.STRIP_TOP + build_catalog.STRIP_HEIGHT)

    print("sprite paths")
    paths = re.findall(r'"Background.sprite", "([^\n]*BLITZFORGE/g[^\n]*)"', screen)
    check("every level asks for its own sprite", len(paths) == levels)
    check("the path carries the request sequence",
          all("modRequestSeq" in p for p in paths),
          "without it a second install is served the first one's cache")
    check("the path carries the tick",
          all("modTick" in p for p in paths),
          "without it only the first frame could ever change")
    # Nothing is asked for until the installer has had time to write it. A
    # binding resolves when its inputs change, on screen or not, and the engine
    # caches a pink placeholder against any path it fails to load - so a path
    # resolved one moment too early is wrong for the life of the screen. Both
    # halves of that shipped: g/0-0/* at every screen load, and g/<seq>-0/* in
    # the same millisecond as the press that the installer learns of from that
    # very log line.
    check("no level asks for a run path until the run is under way",
          all(p.startswith("when modBusy -> ") for p in paths),
          "resolving at rest caches a placeholder against a run that will "
          "never be written, tinted by the layer's colour into a solid square")
    idle = {p.rsplit(", ", 1)[1] for p in paths if ", " in p}
    check("at rest every level points at one sprite that certainly exists",
          idle == {f'\\"{build_catalog.IDLE_SPRITE}\\"'}, f"{idle}")
    check("that sprite is the transparent one the installer links",
          build_catalog.IDLE_SPRITE.rsplit("/", 1)[-1]
          == progress.EMPTY.name.split(".")[0],
          f"{build_catalog.IDLE_SPRITE} vs {progress.EMPTY.name}")

    # What the screen will ask for, spelled out, against what gets written.
    for i in range(min(len(paths), levels)):
        want = f"~res:/BLITZFORGE/g/7-3/{i}"
        built = (paths[i].replace('\\"', "").replace(" + str(modRequestSeq) + ", "7")
                 .replace(" + str(modTick) + ", "3")
                 .removeprefix("when modBusy -> ").rsplit(", ", 1)[0])
        if built != want:
            check(f"level {i} resolves to the path the installer writes",
                  False, f"{built!r} != {want!r}")
            break
    else:
        check("all level paths resolve to what the installer writes", True)

    print("timing")
    ticks = int(re.search(r"repeat\((\d+)\)", actions).group(1))
    wait = float(re.search(r"Wait\((0\.\d+)\);\s*ChangeData\(modTick", actions).group(1))
    check("installer writes at least as many ticks as the screen asks for",
          progress.TICKS >= ticks, f"{progress.TICKS} vs {ticks}")
    check("installer's tick interval matches the screen's",
          abs(progress.TICK_SECONDS - wait) < 1e-9,
          f"{progress.TICK_SECONDS} vs {wait}")
    # Tick zero is the one frame with no margin of its own: the installer
    # cannot start before the press, because the press is how it hears. The
    # wait has to cover its poll interval and the client's log flush, so it is
    # measured against the poll rather than against LEAD_SECONDS - which only
    # ever protected tick one onwards.
    show_delay = float(re.search(r"Wait\((\d\.\d+)\);\s*ChangeData\(modBusy, true\)",
                                 actions).group(1))
    check("the ring waits out the installer's own reaction time before showing",
          show_delay >= progress.LEAD_SECONDS + agent_module.POLL_SECONDS + 0.3,
          f"{show_delay}s vs lead {progress.LEAD_SECONDS} + poll "
          f"{agent_module.POLL_SECONDS} + flush margin")
    # More levels than ticks cannot be told apart: the screen only looks once
    # per tick, so the extra ones would cost YAML and a clipped draw each frame
    # and buy nothing.
    check("no more fill levels than there are ticks to show them in",
          levels <= progress.TICKS, f"{levels} levels vs {progress.TICKS} ticks")

    print("assets")
    check("ring sprite exists", progress.RING.exists())
    check("blank sprite exists", progress.EMPTY.exists())

    print("fill maths")
    check("empty download shows nothing filled", progress.level_for(0.0) == 0)
    check("finished download fills the circle", progress.level_for(1.0) == levels)
    check("half shows half", progress.level_for(0.5) == levels // 2)
    check("over-report cannot overflow the circle", progress.level_for(1.7) == levels)
    check("under-report cannot go negative", progress.level_for(-0.4) == 0)

    print("what the ring actually draws")
    # The end-to-end claim, and the only check here that would catch the two
    # halves being individually self-consistent and jointly wrong: run the
    # installer for a fraction, find the one level it made real, and read that
    # level's angle back off the screen.
    drawn = dict(fills)
    original = progress.GFX, progress.RING, progress.EMPTY
    try:
        with tempfile.TemporaryDirectory(prefix="blitzforge-arc-") as root:
            temporary = Path(root)
            progress.GFX = temporary / "g"
            progress.RING = temporary / "ring.dvpl"
            progress.EMPTY = temporary / "empty.dvpl"
            progress.RING.write_bytes(b"ring")
            progress.EMPTY.write_bytes(b"empty")

            def arc_for(fraction: float) -> tuple[int, float]:
                """(levels carrying the ring, the arc the screen shows).

                No level carrying it is a legitimate state rather than a
                missing picture: it is the circle reading nought, which is
                what the track behind the fills already draws.
                """
                tick = int(fraction * 1000)
                progress.write_tick(1, tick, fraction)
                folder = progress.GFX / f"1-{tick}"
                real = [i for i in range(levels)
                        if (folder / f"{i}.packed.webp.dvpl").read_bytes() == b"ring"]
                return len(real), drawn[real[0]] if real else 0.0

            check("nothing is drawn before anything has downloaded",
                  arc_for(0.0) == (0, 0.0))
            check("a finished download draws the whole circle",
                  arc_for(1.0) == (1, 1.0))
            worst, worst_at, stacked = 0.0, 0.0, None
            for step in range(1, 101):
                fraction = step / 100
                count, arc = arc_for(fraction)
                if count > 1:
                    stacked = (fraction, count)
                    break
                if abs(arc - fraction) > worst:
                    worst, worst_at = abs(arc - fraction), fraction
            check("never two rings at once, which would stack two arcs",
                  stacked is None,
                  f"{stacked[1]} levels carry it at {stacked[0]:.2f}"
                  if stacked else "")
            if stacked is None:
                # Rounding to the nearest level can be out by at most half a
                # level, plus what six decimals of YAML costs. Anything worse
                # means the two sides disagree about what an index means - an
                # arc that is confidently wrong.
                check("the arc drawn is never more than half a level from the "
                      "fraction downloaded",
                      worst <= 0.5 / levels + 1e-6,
                      f"worst {worst:.4f} at {worst_at:.2f}, "
                      f"half a level is {0.5 / levels:.4f}")
    finally:
        progress.GFX, progress.RING, progress.EMPTY = original

    print("screen reload")
    original = progress.GFX, progress.RING, progress.EMPTY
    try:
        with tempfile.TemporaryDirectory(prefix="blitzforge-progress-") as root:
            temporary = Path(root)
            progress.GFX = temporary / "g"
            progress.RING = temporary / "ring.dvpl"
            progress.EMPTY = temporary / "empty.dvpl"
            progress.RING.write_bytes(b"ring")
            progress.EMPTY.write_bytes(b"empty")
            stale = progress.GFX / "0-0" / "0.packed.webp.dvpl"
            stale.parent.mkdir(parents=True)
            stale.write_bytes(b"old completed run")
            reporter = progress.Reporter(0)
            reporter.__enter__()
            reporter._stop.set()
            reporter.wait(1.0)
            check("a reloaded screen cannot reuse same-sequence cached frames",
                  stale.read_bytes() == b"empty")
    finally:
        progress.GFX, progress.RING, progress.EMPTY = original
        progress._active = None

    print()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("progress ring: screen and installer agree")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    sys.exit(main())
