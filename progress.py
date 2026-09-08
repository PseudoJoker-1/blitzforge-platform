"""Publishes real install progress where the running screen can see it.

The catalogue cannot be told anything. Its bindings see only the client's own
models, and the actions language reads neither files nor sockets, so there is
no way to hand a *number* to a screen that is already on display.

There is one thing the screen does do on its own: it asks the engine for
sprites by path, and loose files under Data/ resolve. That gives one channel
and only one - which picture is at a path - so the fraction downloaded has to
be carried by a picture rather than by a number.

The screen therefore stacks 24 copies of the game's own download-progress
circle, one per fill level, each carrying a *fixed* progress of its own baked
into the YAML (see build_catalog.progress_bar). This module decides which of
them is real: the stock ring sprite is linked at the one path whose level
matches the bytes actually downloaded, and a transparent sprite at every
other. Exactly one ring draws, radially clipped to its own fraction, and the
other 23 clip nothing visible.

So the circle is the client's own `RadialProgressComponent` fed real
measurements, not an animation on a timer, and both sprites are stock files
hard-linked into place - nothing is drawn at runtime and no image encoder is
needed.

Two details make it work rather than nearly work:

* Sprites are cached per path, so a path can only ever show one thing. The
  screen counts ticks into the path, which turns each poll into a new path.
  The request sequence is in there too, or a second install would be served
  the first one's pictures.
* A path with no file behind it draws a pink placeholder, not nothing. Ticks
  are therefore written slightly before the screen asks for them, and the
  screen delays the bar to let the first ones land.
"""
from __future__ import annotations

import os
import shutil
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME = HERE.parent
GFX = GAME / "Data" / "BLITZFORGE" / "g"

# The hollow circle the client's own download widget fills, and a transparent
# sprite for the levels that must not draw. Taking the ring from the game
# rather than shipping one means the catalogue's circle is the same circle the
# rest of the lobby uses, at whatever resolution the client picked.
RING = GAME / "Data/Gfx/Lobby/backgrounds/bg_circle-hollow_64.packed.webp.dvpl"
EMPTY = GAME / "Data/Gfx/Lobby/icons/icon_empty_32.packed.webp.dvpl"

# Must match the screen: build_catalog.LEVELS and the Wait/repeat in
# ON_MOD_WORKING. They are asserted against each other in test_progress.py.
#
# 24 is not a look-and-feel choice. The screen can only change what it shows
# once per tick, and there are TICKS of those, so more levels than ticks could
# never be told apart on screen - they would only cost YAML and a clipped draw
# each frame.
LEVELS = 24
TICK_SECONDS = 0.15
TICKS = 25
# How far ahead of the screen to write. Covers the jitter between the press and
# the moment this notices it, so the file is always there first.
LEAD_SECONDS = 0.25


def _link(source: Path, target: Path) -> None:
    """Hard-link if the filesystem allows it, copy if not.

    A quarter of a megabyte of duplicated sprite per install would be silly
    when every one of them is the same eight-by-eight block.
    """
    try:
        os.link(source, target)
    except FileExistsError:
        pass
    except (OSError, NotImplementedError):
        try:
            shutil.copy2(source, target)
        except (OSError, shutil.Error):
            pass


def level_for(fraction: float) -> int:
    """Which fill level the measured fraction rounds to, 0 meaning none.

    Level L is drawn by the screen's Fill(L-1) control, whose own progress is
    L/LEVELS. Level 0 has no control and no ring is linked at all, which is
    what an install that has not moved yet should look like.
    """
    return round(max(0.0, min(1.0, fraction)) * LEVELS)


def write_tick(seq: int, tick: int, fraction: float) -> None:
    folder = GFX / f"{seq}-{tick}"
    folder.mkdir(parents=True, exist_ok=True)
    level = level_for(fraction)
    for i in range(LEVELS):
        target = folder / f"{i}.packed.webp.dvpl"
        if target.exists():
            continue
        # One ring, not a run of them. Every control here is the whole circle
        # already, clipped to its own fixed fraction, so linking the sprite at
        # more than one level would stack several arcs and show the largest -
        # right by accident today, wrong the moment a level is ever tinted or
        # sized differently from its neighbours.
        _link(RING if i == level - 1 else EMPTY, target)


def sweep(keep_seq: int | None = None) -> None:
    """Drop the sprites of finished installs."""
    if not GFX.exists():
        return
    for folder in GFX.iterdir():
        if keep_seq is not None and folder.name.startswith(f"{keep_seq}-"):
            continue
        shutil.rmtree(folder, ignore_errors=True)


_active: "Reporter | None" = None


class Reporter:
    """Lays down one tick at a time, each carrying the fraction current then.

    Writing every tick on every update would mean re-linking the whole run ten
    times a second. Each tick is instead written once, just before the screen
    reaches it, which is both cheaper and more truthful: the picture the screen
    picks up is the progress at the moment it was asked for.

    Exactly one thread ever writes a given run. Having the caller finalise the
    run while the ticker was still going raced on the same files - the first
    attempt to fix it up at the end collided mid-write and threw. The end of a
    request now only says "call it finished"; the ticker keeps its own schedule
    and writes the rest out as complete.
    """

    def __init__(self, seq: int):
        self.seq = seq
        self.fraction = 0.0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def set(self, fraction: float) -> None:
        self.fraction = fraction

    def __enter__(self) -> "Reporter":
        global _active
        # Stop the previous run before its files are swept, or it would keep
        # recreating what sweep() has just removed.
        if _active is not None:
            _active._stop.set()
        _active = self
        # modRequestSeq starts from zero again with every newly loaded screen.
        # Keeping a same-numbered old run serves cached completion frames to a
        # new install, so every stopped run is removed before tick zero lands.
        sweep()
        # Tick zero has to exist before the screen's delay is up.
        write_tick(self.seq, 0, 0.0)
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def _run(self) -> None:
        start = time.monotonic()
        for tick in range(1, TICKS):
            due = start + tick * TICK_SECONDS - LEAD_SECONDS
            if self._stop.wait(max(0.0, due - time.monotonic())):
                return
            write_tick(self.seq, tick, self.fraction)

    def __exit__(self, *_exc) -> None:
        # The work is done, so every tick still to come shows a full bar. The
        # ticker writes them; nothing here touches a file the ticker owns.
        self.fraction = 1.0

    def wait(self, timeout: float | None = None) -> None:
        """Only the tests need the run to be finished before they look."""
        if self._thread:
            self._thread.join(timeout)
