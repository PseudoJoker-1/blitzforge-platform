"""Executes install and remove requests made from inside the game.

The actions language the catalogue screen is written in has no file or network
access - its whole vocabulary is UI and animation - so the buttons cannot run
the installer themselves. What they can do is write a line to the client log.
This tails that log and carries the request out.

    python agent.py             # follow the log and act on requests
    python agent.py --once      # process what is already there and exit
    python agent.py --probe     # report which channel is arriving
    python agent.py --autostart # start it now and register it with Windows

build_catalog.py calls --autostart for you, so there is nothing to launch by
hand.

A request names a catalogue generation and card index. catalog_index.json
keeps that exact mapping even if files for the next launch are rebuilt while
the old screen is still alive, so an index can never drift to another mod.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
import traceback
from pathlib import Path

import install
import modpack
import progress

HERE = Path(__file__).resolve().parent
INDEX = HERE / "cache" / "catalog_index.json"
STATE = HERE / "cache" / "agent_state.json"
LOGS = (Path.home() / "AppData" / "Local" / "wotblitz" / "DAVAProject")
# Two shapes for the same request.
#
# The client filters debug-level output - the log holds error, warning and info
# lines and nothing below - so Log() from the UI may never arrive. What does
# arrive is the engine complaining about a resource it cannot find, and that is
# an error. The catalogue therefore asks for a sprite whose path encodes the
# request, and the resulting "File ... not found" line is the channel.
SPRITE_REQUEST = re.compile(r"BLITZFORGE/(\d)/(\d+)/(\d+)-(\d+)")
LEGACY_SPRITE_REQUEST = re.compile(r"BLITZFORGE/(\d)/(\d+)-(\d+)")
LOG_REQUEST = re.compile(r"BLITZFORGE:(install|remove|restart|update):(\d+)")
VERBS = {"1": "install", "2": "remove", "3": "restart", "4": "update"}
# The gap between pressing a button and anything happening is this number. The
# work behind it takes a few hundred milliseconds, so polling every two seconds
# was most of the wait.
POLL_SECONDS = 0.3
# Watching for the client to close costs a process each time, and nothing is
# waiting on the answer, so it is asked for far less often than the log is read.
CLIENT_POLL_SECONDS = 5.0
# While the client is closed the Hangar file is safe to rebuild.  Refreshing
# the registry periodically keeps the next launch from showing a catalogue
# that was published after the last client shutdown (the UI itself cannot make
# network requests).
REGISTRY_REFRESH_SECONDS = 15.0

# Every helper this runs - tasklist, taskkill, python - opens a console window
# unless told not to. Black boxes flashing over the game while a mod installs
# is what makes a working tool look like a batch script, so nothing below is
# spawned without this flag.
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)

STEAM_APP_ID = "444200"
OPEN_ON_LOAD = HERE / "cache" / "open_catalog_on_load"

# The mod loader's crash detector. It writes this at startup and removes it on
# a clean shutdown; finding it already present at the next startup means the
# previous session died mid-run, and the loader answers by refusing to load
# any third-party mod at all (SAFE MODE). Correct for a crash - but a forced
# taskkill from our own restart button is indistinguishable from one, so the
# button that applies a freshly installed mod would otherwise also be the
# thing that blocks it from ever loading. One such marker, stranded on
# 2026-08-02, silently kept every session for a week in SAFE MODE.
SESSION_MARKER = HERE.parent / "mods" / "cache" / "runtime_session.marker"
# What a crash actually leaves behind. The proxy installs an unhandled-exception
# filter that logs the exception and writes a minidump beside the client, so
# these two are produced by a crash and by nothing else - unlike the marker,
# which is produced by every session (see reconcile_session_marker).
PROXY_LOG = HERE.parent / "wotb_mod.log"
CRASH_DUMPS = HERE.parent


PID_FILE = HERE / "cache" / "agent.pid"
AGENT_LOG = HERE / "cache" / "agent.log"
STARTUP = (Path.home() / "AppData" / "Roaming" / "Microsoft" / "Windows"
           / "Start Menu" / "Programs" / "Startup" / "blitzforge-agent.vbs")
# The on-demand twin of the Startup entry. Scheduled tasks are started by the
# Task Scheduler service, so an agent launched this way belongs to the service,
# not to whoever asked for it. A plain Popen from a build makes the agent a
# child of that build's session - and sessions under job objects (CI shells,
# sandboxed tools) take all their children with them when they close. That is
# not hypothetical: an agent spawned that way died silently within minutes,
# and every button in the catalogue went dead with it.
TASK_NAME = "BlitzForgeAgent"


# The modules the agent carries out requests with. Python holds an imported
# module in memory for the life of the process, so editing any of these leaves
# a long-running agent executing the version it started with - installs kept
# going through the old install.py for a day after native support was added to
# it, with nothing to suggest anything was wrong.
SOURCES = ("agent.py", "build_catalog.py", "install.py", "modpack.py", "patch_dvpl.py",
           "registry.py", "progress.py", "dvpl.py")


def _source_stamp() -> float:
    """The newest edit across everything the agent has loaded into memory."""
    stamps = [(HERE / name).stat().st_mtime
              for name in SOURCES if (HERE / name).exists()]
    return max(stamps, default=0.0)


# What was on disk when this process imported its modules. The watch loop
# compares against the current stamp to notice that it has become the stale
# agent everyone else is entitled to replace, and hands over on its own terms
# - between requests - instead of being taskkilled in the middle of one.
_LOADED_STAMP = _source_stamp()


def _is_running(pid: int) -> bool:
    """Whether that pid is a live process.

    Deliberately reads bytes and never decodes. tasklist writes in the console
    OEM codepage, which on this machine is not what Python decodes with, and
    the mismatch does not fail where it can be seen: `text=True` raises inside
    subprocess's reader thread, which prints a traceback nobody catches and
    hands back stdout=None. The caller then dies on `in None` - a crash in the
    rebuild, from a routine "is the agent up?" check. Under the ANSI codepage
    it silently produced mojibake instead and worked by luck, because a pid is
    ASCII digits either way. Matching bytes needs neither luck nor a codepage.
    """
    result = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                            capture_output=True, check=False,
                            creationflags=NO_WINDOW)
    return str(pid).encode("ascii") in result.stdout


def _claimed() -> dict:
    try:
        text = PID_FILE.read_text(encoding="utf-8").strip()
    except OSError:
        return {}
    try:
        record = json.loads(text)
    except json.JSONDecodeError:
        # The file used to hold a bare pid. An agent that predates the stamp is
        # by definition running code older than this, and reading its file as
        # unparseable would have left it alive and unaccounted for - which it
        # did, once, leaving two agents watching the same log.
        return {"pid": int(text), "stamp": 0.0} if text.isdigit() else {}
    return record if isinstance(record, dict) else {}


def already_running() -> bool:
    """True only if an agent is up *and* running the current code."""
    record = _claimed()
    pid = record.get("pid")
    if not isinstance(pid, int) or not _is_running(pid):
        return False
    if record.get("stamp", 0.0) < _source_stamp():
        print(f"agent {pid} is running code that has since been edited")
        return False
    return True


def claim_pid() -> bool:
    """Take the watch. False means another agent took it and this one should go.

    already_running() is a check followed by an action, and two agents started
    at once - a rebuild and the Windows startup entry, say - both see nothing
    running and both proceed. It happened: three processes ended up watching
    the same log, which means every request handled two or three times.

    Writing and then reading back settles it without a lock. Whoever wrote last
    owns the file, and everyone else sees a pid that is not theirs and leaves.
    """
    PID_FILE.parent.mkdir(parents=True, exist_ok=True)
    # The stamp is taken now, at import time, so it describes the code this
    # process actually loaded rather than whatever is on disk later.
    PID_FILE.write_text(json.dumps({"pid": os.getpid(), "stamp": _source_stamp()}),
                        encoding="utf-8")
    # Long enough for a rival that wrote just before this one to have finished.
    time.sleep(0.5)
    return _claimed().get("pid") == os.getpid()


def _pythonw() -> str:
    """pythonw runs without a console window, so the agent stays out of sight."""
    candidate = Path(sys.executable).with_name("pythonw.exe")
    return str(candidate) if candidate.exists() else sys.executable


def spawn_agent() -> None:
    """Start an agent that belongs to nobody but Windows.

    Through the Task Scheduler when possible: /Run makes the service the
    parent, so the agent survives the caller and everything the caller is
    inside - including a job object that kills its tree on close, which is
    what silently took down an agent Popen'd from a sandboxed build and left
    the user pressing dead buttons. The Popen fallback is for machines where
    creating tasks is forbidden; there the old lifetime rules apply.
    """
    created = subprocess.run(
        ["schtasks", "/Create", "/TN", TASK_NAME, "/F", "/SC", "ONCE",
         "/ST", "00:00", "/TR", f'"{_pythonw()}" "{HERE / "agent.py"}"'],
        capture_output=True, check=False, creationflags=NO_WINDOW)
    if created.returncode == 0:
        ran = subprocess.run(["schtasks", "/Run", "/TN", TASK_NAME],
                             capture_output=True, check=False,
                             creationflags=NO_WINDOW)
        if ran.returncode == 0:
            print("agent started via the task scheduler")
            return
    subprocess.Popen([_pythonw(), str(HERE / "agent.py")],
                     creationflags=getattr(subprocess, "DETACHED_PROCESS", 0)
                     | NO_WINDOW)
    print("agent started in the background (task scheduler unavailable)")


def ensure_autostart() -> None:
    """Start the agent now if it is not up, and arrange for it to start with
    Windows.

    Nothing about the catalogue works without the agent: the buttons still
    light up and still write their request, and it sits in the log unread.
    Leaving that to a batch file the user has to remember made a working
    feature look broken, so it is set up as a side effect of building the
    screen instead.
    """
    script = f'''Set s = CreateObject("WScript.Shell")
s.Run """{_pythonw()}"" ""{HERE / 'agent.py'}""", 0, False
'''
    STARTUP.parent.mkdir(parents=True, exist_ok=True)
    if not STARTUP.exists() or STARTUP.read_text(encoding="utf-8") != script:
        STARTUP.write_text(script, encoding="utf-8")
        print(f"agent registered to start with Windows: {STARTUP.name}")

    if already_running():
        return

    # already_running() also answers false for an agent stuck on stale code.
    # That one is asked to hand over rather than shot: the stale agent sees
    # the source stamp move and exits between requests on its own (see the
    # watch loop), so a kill here could only ever interrupt it mid-install.
    # The one situation that still warrants force is a pre-handover agent so
    # old it does not know how to hand over - recognised by its stamp
    # predating this file's own mtime.
    stale = _claimed()
    pid = stale.get("pid")
    if (isinstance(pid, int) and _is_running(pid)
            and stale.get("stamp", 0.0) < (HERE / "agent.py").stat().st_mtime):
        subprocess.run(["taskkill", "/PID", str(pid), "/F"],
                       capture_output=True, check=False, creationflags=NO_WINDOW)
        print(f"stopped agent {pid}; it predates the handover protocol")

    spawn_agent()


def newest_log() -> Path | None:
    logs = sorted(LOGS.glob("blitz-logs_*.txt"),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    return logs[0] if logs else None


def load_index(generation: int | None = None) -> list[str]:
    if not INDEX.exists():
        return []
    try:
        payload = json.loads(INDEX.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, KeyError):
        return []
    if generation is not None:
        catalogs = payload.get("catalogs", {})
        order = catalogs.get(str(generation)) if isinstance(catalogs, dict) else None
        return order if isinstance(order, list) else []
    # Requests from a screen generated before catalogue generations existed
    # must keep using the order that was active during the migration build.
    order = payload.get("legacy_order", payload.get("order", []))
    return order if isinstance(order, list) else []


def load_state() -> dict:
    if not STATE.exists():
        return {}
    try:
        return json.loads(STATE.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def save_state(state: dict) -> None:
    """Persist the log offsets. Failure here is survivable and must not raise.

    The caller already holds the new offsets in memory and uses those, so this
    file only matters across restarts. That is worth a few retries and worth
    nothing at all beyond them - and it is emphatically not worth the watcher,
    which is what it cost once: os.replace lost a race to something holding the
    destination open for an instant, PermissionError went up through main(),
    and the agent died. Every button in the catalogue died with it, and the
    user spent the evening pressing them.

    The temporary file carries this process's pid so two agents overlapping
    across a handover cannot write the same path and take turns deleting it
    from under each other.
    """
    STATE.parent.mkdir(parents=True, exist_ok=True)
    temporary = STATE.with_suffix(f"{STATE.suffix}.{os.getpid()}.tmp")
    try:
        temporary.write_text(json.dumps(state, indent=2), encoding="utf-8")
        for attempt in range(5):
            try:
                temporary.replace(STATE)
                return
            except OSError:
                if attempt == 4:
                    raise
                time.sleep(0.1 * (attempt + 1))
    except OSError as error:
        print(f"could not save watcher state ({error}); continuing with it in "
              "memory, so only a restart would re-read handled log regions")
        try:
            temporary.unlink()
        except OSError:
            pass


# Set while a restart this process asked for is in flight. The watcher below
# sees the client stop and would otherwise read it as the user quitting, and
# rebuild a second time - dropping the "open the catalogue" flag that the
# rebuild inside restart_client has already consumed, so the client would come
# back to a plain hangar.
_expect_restart = False
_last_registry_signature: str | None = None


def stop_client(graceful_seconds: float = 10.0,
                forced_seconds: float = 10.0) -> str:
    """Bring the client down, politely first. Returns how it went.

    'graceful' is taskkill without /F - a WM_CLOSE the game acts on itself,
    letting it save and shut down on its own terms. 'forced' means it ignored
    that and was killed.

    Which of the two happened turns out not to decide the session marker's
    fate: this client never reaches the loader's teardown either way, so the
    marker survives both. reconcile_session_marker() judges it on crash
    evidence instead. Closing politely is still the right thing to do to a
    game that may want to save something.
    """
    if not client_running():
        return "not-running"
    subprocess.run(["taskkill", "/IM", "wotblitz.exe"],
                   capture_output=True, check=False, creationflags=NO_WINDOW)
    deadline = time.monotonic() + graceful_seconds
    while time.monotonic() < deadline:
        if not client_running():
            return "graceful"
        time.sleep(0.2)
    subprocess.run(["taskkill", "/IM", "wotblitz.exe", "/F"],
                   capture_output=True, check=False, creationflags=NO_WINDOW)
    deadline = time.monotonic() + forced_seconds
    while time.monotonic() < deadline:
        if not client_running():
            break
        time.sleep(0.2)
    return "forced"


def _marker_pid() -> int | None:
    """The process the marker says owns it, if it says."""
    try:
        for line in SESSION_MARKER.read_text(encoding="utf-8",
                                             errors="replace").splitlines():
            key, _, value = line.partition("=")
            if key.strip() == "process_id" and value.strip().isdigit():
                return int(value.strip())
    except OSError:
        return None
    return None


def crash_evidence(since: float) -> str | None:
    """What says the last session crashed, rather than merely ended.

    The proxy's unhandled-exception filter logs the exception and writes a
    minidump next to the client. Both are consequences of a crash and of
    nothing else, which is exactly what the session marker is not.
    """
    try:
        for dump in CRASH_DUMPS.glob("wotb_mod_crash_*.dmp"):
            if dump.stat().st_mtime >= since - 1.0:
                return f"minidump {dump.name}"
    except OSError:
        pass
    try:
        # The proxy rewrites this per session, so while the client is closed it
        # describes the session that just ended. Checked as well as the dump
        # because a dump that failed to write still leaves the log entry.
        text = PROXY_LOG.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    return "UNHANDLED exception in wotb_mod.log" if "UNHANDLED" in text else None


def reconcile_session_marker() -> None:
    """Decide what a surviving session marker actually means.

    The loader writes the marker at load and removes it only on an orderly
    teardown - its atexit handler or DLL_PROCESS_DETACH. This client reaches
    neither: 30 MB of loader log spanning weeks contains no teardown of any
    kind, not one. So the marker survives every ordinary exit, the next launch
    reads that survival as a crash, and SAFE MODE blocks every third-party
    mod. The mod system was effectively single-shot - it worked once after a
    marker was cleared by hand, then disabled itself on the following launch.

    Presence therefore cannot be the crash signal, because it is present after
    every session; a signal that is always raised carries nothing. What does
    distinguish a crash is what the proxy writes when one happens, and that is
    what is consulted here. When it is there the marker is left exactly where
    it is and SAFE MODE does its job.

    Never runs against a live session: the client must be gone and the pid the
    marker names must be dead. The file is renamed rather than deleted, so the
    loader's last recorded state stays readable.
    """
    if not SESSION_MARKER.exists():
        return
    if client_running():
        return
    pid = _marker_pid()
    if isinstance(pid, int) and _is_running(pid):
        print(f"  session marker belongs to live process {pid}; leaving it")
        return

    try:
        stamp = SESSION_MARKER.stat().st_mtime
    except OSError:
        return
    evidence = crash_evidence(stamp)
    if evidence:
        print(f"  session marker kept: the last session crashed ({evidence}). "
              "SAFE MODE will block third-party mods until it is investigated "
              f"and {SESSION_MARKER.name} removed by hand")
        return

    backup = SESSION_MARKER.with_name(
        SESSION_MARKER.name + time.strftime(".ended-%Y%m%d-%H%M%S"))
    try:
        SESSION_MARKER.replace(backup)
        print(f"  session ended without loader teardown and without crash "
              f"evidence; marker archived as {backup.name}")
    except OSError as error:
        print(f"  could not archive the session marker: {error}; the next "
              "session will start in SAFE MODE")


def restart_client() -> None:
    global _expect_restart
    _expect_restart = True
    """Relaunch the client with the catalogue already open.

    The screen cannot be opened from outside the game, so the flag makes the
    next build start with it visible; build_catalog consumes the flag, which
    keeps it from sticking on every later launch.
    """
    OPEN_ON_LOAD.parent.mkdir(parents=True, exist_ok=True)
    OPEN_ON_LOAD.write_text("1", encoding="utf-8")
    # --no-autostart, and not as an optimisation. The rebuild's own autostart
    # check reads the source stamp, and if this agent's code has been edited
    # since it started, that check used to taskkill this very process - here,
    # after the "restart" flag was written and before the client was actually
    # restarted. The user pressed the button ten times against a corpse.
    # Handing over to newer code is the watch loop's job, done between
    # requests, never by a child in the middle of one.
    subprocess.run([_pythonw(), str(HERE / "build_catalog.py"), "--no-autostart"],
                   check=False, creationflags=NO_WINDOW)

    outcome = stop_client()
    # Whichever way it went down, the marker it leaves is judged on crash
    # evidence rather than on its own existence - the client never runs the
    # loader teardown that would remove it, so it is there every time.
    reconcile_session_marker()

    # os.startfile hands the URL to the shell directly. Going through
    # `cmd /c start` opened a console window over the game every restart.
    os.startfile(f"steam://rungameid/{STEAM_APP_ID}")
    print(f"  restarting client with the catalogue open (shutdown: {outcome})")


def client_running() -> bool:
    # Bytes for the same reason as _is_running: tasklist writes in the OEM
    # codepage, and decoding it is a codepage-dependent crash for a question
    # answered by an ASCII substring either way.
    result = subprocess.run(["tasklist", "/FI", "IMAGENAME eq wotblitz.exe", "/NH"],
                            capture_output=True, check=False,
                            creationflags=NO_WINDOW)
    return b"wotblitz.exe" in result.stdout


def pending_updates(entries: list[dict] | None = None) -> list[tuple[str, str, str]]:
    """(id, installed version, offered version) for everything now behind."""
    import build_catalog
    import registry as registry_client

    if entries is None:
        entries, _ = registry_client.fetch()
    ledger = install.load_ledger()
    behind = []
    for entry in entries:
        record = ledger.get(entry["id"])
        if entry["id"] in ledger and build_catalog.is_newer(
                entry.get("version", "0.0.0"), record.get("version", "0.0.0")):
            behind.append((entry["id"], record["version"], entry["version"]))
    return behind


def _registry_signature(entries: list[dict]) -> str:
    """Stable in-process identity for the server catalogue contents."""
    return json.dumps(entries, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":"))


def check_for_updates(reason: str) -> None:
    """Refetch the registry and rebuild the screen so it says what is current.

    Done while the client is closed, which is the only moment it is any use.
    The catalogue is baked into Hangar.yaml when it is built, so a rebuild
    during a session cannot reach the screen already on display - checking at
    startup would show the result one restart late. Between shutdown and the
    next launch it lands in time, and writing the file is safe because nothing
    is reading it.
    """
    global _last_registry_signature
    print(f"checking for mod updates ({reason})")
    try:
        import registry as registry_client

        entries, source, payload = registry_client.fetch_full()
        signature = _registry_signature(entries)
        behind = pending_updates(entries)
        # The registry also publishes what it has deleted, and this is the
        # moment to act on it: the client is closed, so its files are safe to
        # touch. install decides what may go - only on a fresh read, only ids
        # it is actually told about, only ids that are installed - because the
        # ledger and remove() are its business, not the watcher's.
        uninstalled = install.apply_removals(
            registry_client.tombstones(payload), source)
    except Exception as error:                  # network, registry, ledger
        print(f"  update check failed: {error}")
        return

    for mod_id, have, offered in behind:
        print(f"  {mod_id}: {have} -> {offered}")
    if not behind:
        print("  everything installed is current")

    if uninstalled:
        # A deleted mod is already absent from `mods`, so the signature below
        # can be identical to the last poll's while the ledger has just changed
        # underneath it. The cards are captioned from the ledger, so without
        # this the screen would keep offering REMOVE for a mod that is gone.
        _last_registry_signature = None

    # Rebuild only when the payload changed.  The closed-client poll below can
    # then check the server frequently without repeatedly rewriting Hangar.yaml.
    if signature == _last_registry_signature:
        print("  registry unchanged; catalogue rebuild not needed")
        return
    _last_registry_signature = signature
    subprocess.run([_pythonw(), str(HERE / "build_catalog.py"), "--no-autostart"],
                   capture_output=True, check=False, creationflags=NO_WINDOW)


def handle(verb: str, index: int, seq: int = 0,
           generation: int | None = None) -> None:
    if verb == "restart":
        restart_client()
        return

    if verb not in {"install", "update", "remove"}:
        print(f"  ignoring unknown catalogue verb {verb!r}")
        return
    order = load_index(generation)
    if index < 0 or index >= len(order):
        label = f" generation {generation}" if generation is not None else ""
        print(f"  request for card {index}{label}, but its catalogue mapping "
              "is unavailable; rebuild it before retrying")
        return
    mod_id = order[index]
    print(f"  {verb} {mod_id}")
    try:
        # For the length of the request the installer publishes its progress
        # where the catalogue can see it. Outside this block it is a no-op, so
        # install.py run by hand behaves exactly as before.
        with progress.Reporter(seq) as reporter:
            install.set_reporter(reporter)
            try:
                if verb == "install":
                    install.install(mod_id)
                elif verb == "update":
                    install.update(mod_id)
                else:
                    install.remove(mod_id)
            finally:
                install.set_reporter(None)
    except SystemExit as error:
        # A refused install is a normal outcome - a hash mismatch, a conflict -
        # and must not take the agent down with it.
        print(f"  refused: {' '.join(str(error).split())}")
        return
    except Exception as error:
        # Network/filesystem failures are request failures, not a reason to
        # kill the long-running watcher and leave every later button inert.
        print(f"  failed: {type(error).__name__}: "
              f"{' '.join(str(error).split())}")
        return

    # The card captions come from the ledger, so the screen has to be rebuilt
    # or the button still offers the action that was just carried out.
    # --no-autostart: a rebuild's autostart check taskkilled the agent that
    # spawned it whenever the sources had been edited since that agent
    # started - right here, straight after a successful install.
    rebuilt = subprocess.run([_pythonw(), str(HERE / "build_catalog.py"),
                              "--no-autostart"],
                             capture_output=True, text=True, check=False,
                             creationflags=NO_WINDOW)
    if rebuilt.returncode == 0:
        print("  catalogue rebuilt; the new state shows after a restart")
    else:
        detail = " ".join((rebuilt.stderr or rebuilt.stdout or "").split())
        print(f"  catalogue rebuild failed ({rebuilt.returncode}): {detail}")


def parse_requests(text: str) -> list[tuple[str, int, int, int | None]]:
    """Decode requests once even if the engine logs the same sprite repeatedly."""
    requests: list[tuple[str, int, int, int | None]] = []
    seen = set()

    for encoded, generation, index, seq in SPRITE_REQUEST.findall(text):
        verb = VERBS.get(encoded)
        if verb is None:
            continue
        request = (verb, int(index), int(seq), int(generation))
        if request not in seen:
            seen.add(request)
            requests.append(request)
    for encoded, index, seq in LEGACY_SPRITE_REQUEST.findall(text):
        verb = VERBS.get(encoded)
        if verb is None:
            continue
        request = (verb, int(index), int(seq), None)
        if request not in seen:
            seen.add(request)
            requests.append(request)

    # Old generated screens also emitted Log(). If both channels are present,
    # the sprite is the same click with a sequence number, not a second action.
    sprite_targets = {(verb, index) for verb, index, _seq, _gen in requests}
    for verb, index in LOG_REQUEST.findall(text):
        target = (verb, int(index))
        request = (verb, int(index), 0, None)
        if target not in sprite_targets and request not in seen:
            seen.add(request)
            requests.append(request)
    return requests


def scan(path: Path, offset: int, act: bool = True) -> tuple[int, int]:
    """Read from offset, handle any requests, return (new offset, count)."""
    with path.open("r", encoding="utf-8", errors="replace") as handle_:
        handle_.seek(offset)
        text = handle_.read()
        new_offset = handle_.tell()

    # The sequence number is the screen's own counter for this press, and the
    # progress sprites are filed under it - without it a second install would
    # be served the pictures cached from the first.
    requests = parse_requests(text)

    for verb, index, seq, generation in requests:
        if act:
            handle(verb, index, seq, generation)
    return new_offset, len(requests)


def probe() -> int:
    log = newest_log()
    if not log:
        print(f"no client log under {LOGS}")
        return 1
    text = log.read_text(encoding="utf-8", errors="replace")
    sprite = SPRITE_REQUEST.findall(text)
    legacy_sprite = LEGACY_SPRITE_REQUEST.findall(text)
    logged = LOG_REQUEST.findall(text)
    print(f"log: {log.name} ({len(text)} chars)")
    print(f"  sprite channel: {len(sprite)} request(s)  {sprite[:4]}")
    if legacy_sprite:
        print(f"  legacy sprites: {len(legacy_sprite)} request(s)  {legacy_sprite[:4]}")
    print(f"  Log() channel:  {len(logged)} request(s)  {logged[:4]}")

    levels = sorted(set(re.findall(r"\[(info|debug|warning|error|trace)\]", text)))
    print(f"  levels in this log: {levels}")
    if "debug" not in levels:
        print("  -> debug output is filtered, so Log() from the UI cannot"
              " reach this file; the sprite channel is the one that matters")

    if sprite or legacy_sprite or logged:
        return 0
    print("\nNo request found. Press a button in the catalogue once,"
          " then run this again.")
    return 2


def _log_to_file() -> None:
    """Give the agent somewhere to speak.

    Under pythonw there is no console: sys.stdout is None and every print in
    this file is silently discarded. The agent died four times in one
    afternoon and the only record of why was the absence of its effects.
    """
    if sys.stdout is not None and sys.stderr is not None:
        return                      # run by hand; the console is better
    AGENT_LOG.parent.mkdir(parents=True, exist_ok=True)
    try:
        if AGENT_LOG.exists() and AGENT_LOG.stat().st_size > 1_000_000:
            AGENT_LOG.replace(AGENT_LOG.with_suffix(".log.old"))
        handle_ = AGENT_LOG.open("a", encoding="utf-8", buffering=1)
    except OSError:
        return                      # no log is how it always was; carry on
    sys.stdout = sys.stderr = handle_
    print(f"--- agent {os.getpid()} started "
          f"{time.strftime('%Y-%m-%d %H:%M:%S')} ---")


def handover_due() -> bool:
    """Whether this process is now the stale agent someone should replace."""
    return _source_stamp() > _LOADED_STAMP


def hand_over() -> None:
    """Exit in favour of an agent running the code now on disk.

    Between requests, on the agent's own initiative. The alternative - being
    taskkilled by whichever build noticed first - has interrupted an agent
    mid-install and mid-restart, and a kill cannot pick its moment.
    """
    record = _claimed()
    pid = record.get("pid")
    successor_up = (isinstance(pid, int) and pid != os.getpid()
                    and _is_running(pid)
                    and record.get("stamp", 0.0) >= _source_stamp())
    if not successor_up:
        # Not spawn_agent(). The scheduled task still counts this process as
        # its running instance, and the scheduler silently swallows /Run
        # while one is up - it answered success, no successor appeared, and
        # the watch stood empty until the next rebuild. A direct child is
        # safe from here specifically: this process was started outside any
        # caller's job (scheduler service or the logon script), so the child
        # inherits no job either, and is orphaned cleanly when this process
        # exits - which also marks the task instance finished, so future
        # /Run spawns work again.
        subprocess.Popen([_pythonw(), str(HERE / "agent.py")],
                         creationflags=getattr(subprocess, "DETACHED_PROCESS", 0)
                         | NO_WINDOW)
    print(f"agent {os.getpid()} handing over: sources changed on disk")


def main() -> None:
    global _expect_restart
    _log_to_file()
    if "--probe" in sys.argv:
        sys.exit(probe())
    if "--autostart" in sys.argv:
        ensure_autostart()
        return

    if already_running():
        print("another agent is already watching; exiting")
        return
    if not claim_pid():
        print("another agent claimed the watch at the same moment; exiting")
        return

    state = load_state()
    once = "--once" in sys.argv

    # Progress sprites from an earlier run describe an install that is over.
    # They are normally swept when the next run starts, but only a live agent
    # sweeps - and a session that ran while no agent was up has been served a
    # dead run's pictures as if they were its own: every fill level read as
    # "complete" and the ring rendered as a solid block.
    if not once:
        progress.sweep()
        # A marker from a session that ended while nothing was watching would
        # otherwise sit there blocking every mod until someone looked.
        reconcile_session_marker()

    # A restarted watcher must not replay every install/remove request already
    # present in the current session log. A continuously running watcher still
    # reads a newly-created next-session log from byte zero in the loop below.
    if not once:
        current_log = newest_log()
        if current_log and current_log.name not in state:
            state[current_log.name] = current_log.stat().st_size
            save_state(state)

    # The client may have been closed for days. Whatever was published in the
    # meantime is checked now, before the first launch of this session.
    if not once and not client_running():
        check_for_updates("agent started")
    was_running = client_running()
    next_client_poll = 0.0
    next_registry_refresh = time.monotonic() + REGISTRY_REFRESH_SECONDS

    print(f"watching {LOGS}")
    while True:
        try:
            # The moment the loop notices its own code has been edited it stops
            # taking requests and hands the watch to a fresh process. Doing it
            # here, and only here, means it can never happen in the middle of
            # an install or a restart.
            if not once and handover_due():
                hand_over()
                return

            # A restart is two events, and this is the useful one: the moment
            # the client stops is the moment the screen can be rebuilt for its
            # next start.
            if not once and time.monotonic() >= next_client_poll:
                now = time.monotonic()
                next_client_poll = now + CLIENT_POLL_SECONDS
                running = client_running()
                if was_running and not running:
                    # The session those sprites were for is over with the
                    # client, and so is the marker it left behind.
                    progress.sweep()
                    reconcile_session_marker()
                    # Files an update had to push aside because the client had
                    # them mapped. Nothing holds them now.
                    displaced = modpack.sweep_displaced(HERE.parent / "mods")
                    if displaced:
                        print(f"  removed {displaced} file(s) displaced by an "
                              "update while the client held them open")
                    if _expect_restart:
                        _expect_restart = False
                    else:
                        check_for_updates("client closed")
                    next_registry_refresh = now + REGISTRY_REFRESH_SECONDS
                elif not running and now >= next_registry_refresh:
                    # The UI cannot fetch HTTP itself.  Keep Hangar.yaml current
                    # while the client is closed so the next launch sees the
                    # latest approved server catalogue.
                    check_for_updates("client not running")
                    next_registry_refresh = now + REGISTRY_REFRESH_SECONDS
                was_running = running

            log = newest_log()
            if log:
                key = log.name
                # A new session writes a new file, so an unseen name starts at
                # zero rather than inheriting the previous file's offset.
                offset = state.get(key, 0)
                if offset > log.stat().st_size:
                    offset = 0
                offset, found = scan(log, offset)
                if found:
                    print(f"  handled {found} request(s) from {key}")
                state[key] = offset
                save_state(state)
        except (KeyboardInterrupt, SystemExit):
            raise
        except Exception:
            # One bad poll is not a reason to stop watching. This loop runs
            # against a filesystem three other processes are writing to, and
            # an unhandled hiccup here costs the user a dead catalogue for as
            # long as it takes them to notice - a PermissionError from a lost
            # os.replace race did exactly that, and the evening's every button
            # press went unheard. The trace goes to the log, the watch goes on,
            # and the backoff keeps a persistent fault from spinning a core.
            print(f"--- poll failed at {time.strftime('%H:%M:%S')}; watching on")
            traceback.print_exc()
            time.sleep(2.0)
            continue

        if once:
            return
        time.sleep(POLL_SECONDS)


if __name__ == "__main__":
    main()
