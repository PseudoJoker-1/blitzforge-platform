"""Pins the rules that keep exactly one live agent watching the log.

Each check here is a real incident, not a hypothetical. The agent is spawned
by builds, replaced on code edits, and shared between a game, a scheduler and
whatever shell ran the build - and every rule below was learned by watching a
user press dead buttons:

* A rebuild spawned by the agent taskkilled that same agent mid-request
  whenever the sources had been edited since it started. It died between
  writing the "restart" flag and restarting the client.
* An agent Popen'd from a sandboxed shell died with that shell's job object,
  minutes after birth, silently.
* tasklist output decoded as text crashed the pid probe under a UTF-8
  environment - in a reader thread, so the error surfaced four frames away
  as `stdout=None`.
* Progress sprites from a finished run were served to the next session while
  no agent was up to sweep them, and the ring rendered as a solid block.
* The loader removes its session marker only on an orderly teardown, and
  this client reaches one never - 30 MB of loader log holds no teardown at
  all. So the marker survived every session, the next launch read that as a
  crash, and SAFE MODE refused to load any third-party mod. The system was
  single-shot: clear the marker by hand, get one good session, then it
  disabled itself again.
* An unhandled PermissionError from a lost os.replace race in save_state
  propagated out of the watch loop and killed the agent outright. The user
  spent an evening pressing buttons nothing was listening to.

Most checks are source-shape assertions; the client-shutdown and marker
rules are exercised behaviourally with the real functions.

    python test_agent_lifecycle.py
"""
from __future__ import annotations

import os
import re
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FAILURES: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    print(f"  {'ok  ' if condition else 'FAIL'}  {name}{'  ' + detail if detail else ''}")
    if not condition:
        FAILURES.append(name)


def call_sites(source: str, needle: str) -> list[str]:
    """Every subprocess call expression in `source` that mentions `needle`."""
    sites = []
    for match in re.finditer(r"subprocess\.\w+\(", source):
        depth, i = 1, match.end()
        while depth and i < len(source):
            depth += {"(": 1, ")": -1}.get(source[i], 0)
            i += 1
        site = source[match.start():i]
        if needle in site:
            sites.append(site)
    return sites


def main() -> int:
    agent_src = (HERE / "agent.py").read_text(encoding="utf-8")
    catalog_src = (HERE / "build_catalog.py").read_text(encoding="utf-8")

    print("agent-spawned rebuilds must not run the autostart check")
    rebuilds = call_sites(agent_src, "build_catalog.py")
    check("the agent spawns build_catalog somewhere", len(rebuilds) >= 3,
          f"found {len(rebuilds)}")
    for site in rebuilds:
        if "--no-autostart" not in site:
            check("every agent-spawned rebuild passes --no-autostart", False,
                  site.splitlines()[0] + "...")
            break
    else:
        check("every agent-spawned rebuild passes --no-autostart", True,
              "without it the rebuild taskkills the agent that spawned it "
              "the moment the sources are newer than that agent")

    print("build_catalog honours the flag")
    check("--no-autostart reaches rebuild()",
          re.search(r'autostart="--no-autostart" not in sys\.argv',
                    catalog_src) is not None)
    check("ensure_autostart is gated on it",
          re.search(r"if autostart:\s*\n\s*import agent\s*\n\s*"
                    r"agent\.ensure_autostart\(\)", catalog_src) is not None)

    print("tasklist output is never decoded")
    for site in call_sites(agent_src, "tasklist"):
        if "text=True" in site:
            check("no tasklist call uses text=True", False,
                  site.splitlines()[0] + "...")
            break
    else:
        check("no tasklist call uses text=True", True,
              "tasklist writes the OEM codepage; decoding it crashed the pid "
              "probe in a reader thread under a UTF-8 environment")

    print("the agent survives the shell that started it")
    check("spawn goes through the task scheduler",
          "schtasks" in agent_src and '"/Run"' in agent_src,
          "a Popen'd agent dies with the spawning shell's job object")
    check("ensure_autostart uses the shared spawn path",
          re.search(r"def ensure_autostart.*?spawn_agent\(\)", agent_src,
                    re.DOTALL) is not None)

    print("stale agents hand over rather than get shot mid-request")
    check("the watch loop checks for handover",
          re.search(r"while True:.*?handover_due\(\)", agent_src,
                    re.DOTALL) is not None)
    check("handover happens before request handling in the loop",
          agent_src.find("handover_due()", agent_src.find("while True:"))
          < agent_src.find("scan(log", agent_src.find("while True:")))
    hand_over_body = agent_src[agent_src.find("def hand_over"):
                               agent_src.find("def main(")]
    # The comments in that function name the rejected call while explaining
    # why it is rejected, so only code lines count here.
    hand_over_code = "\n".join(line for line in hand_over_body.splitlines()
                               if not line.strip().startswith("#"))
    check("the successor is a direct child, never a task /Run",
          "Popen" in hand_over_code and "spawn_agent()" not in hand_over_code,
          "the scheduler silently swallows /Run while the predecessor still "
          "counts as the task's running instance; it answered success, no "
          "successor appeared, and the watch stood empty")

    print("stale progress sprites cannot outlive their run")
    main_body = agent_src[agent_src.find("def main("):]
    check("agent start sweeps the sprite folders",
          "progress.sweep()" in main_body.split("while True:")[0],
          "a session with no agent up was served a dead run's pictures and "
          "drew every fill level as complete")
    check("client shutdown sweeps them too",
          "progress.sweep()" in main_body.split("while True:")[1])

    print("restarts let the mod loader shut down")
    import agent
    real_run = agent.subprocess.run
    real_running = agent.client_running
    real_sleep = agent.time.sleep
    kills: list[list[str]] = []

    class _Done:
        returncode = 0
        stdout = b""
        stderr = b""

    try:
        agent.subprocess.run = lambda cmd, **kw: (kills.append(list(cmd)),
                                                  _Done())[1]
        agent.time.sleep = lambda seconds: None

        polite = iter([True, False])   # running at entry, gone after WM_CLOSE
        agent.client_running = lambda: next(polite, False)
        outcome = agent.stop_client(graceful_seconds=0.3, forced_seconds=0.3)
        check("a client that honours WM_CLOSE is never force-killed",
              outcome == "graceful" and kills
              and all("/F" not in cmd for cmd in kills),
              "a forced kill strands the loader's session marker and the "
              "next session blocks every native mod")

        kills.clear()
        agent.client_running = lambda: True
        outcome = agent.stop_client(graceful_seconds=0.2, forced_seconds=0.2)
        check("a client that ignores WM_CLOSE is force-killed after the wait",
              outcome == "forced" and any("/F" in cmd for cmd in kills))
        check("the polite attempt still came first",
              kills and "/F" not in kills[0])

        kills.clear()
        agent.client_running = lambda: False
        check("no kill is sent when the client is not running",
              agent.stop_client(0.2, 0.2) == "not-running" and not kills)
    finally:
        agent.subprocess.run = real_run
        agent.client_running = real_running
        agent.time.sleep = real_sleep

    print("the session marker is judged on crash evidence, not on existing")
    saved = (agent.SESSION_MARKER, agent.PROXY_LOG, agent.CRASH_DUMPS,
             agent.client_running)
    marker_body = "[session]\nprocess_id=999999\nphase=running\n"

    def sandbox(root: Path, *, proxy_log: str = "loaded ok\n") -> Path:
        agent.SESSION_MARKER = root / "runtime_session.marker"
        agent.PROXY_LOG = root / "wotb_mod.log"
        agent.CRASH_DUMPS = root
        agent.SESSION_MARKER.write_text(marker_body, encoding="utf-8")
        agent.PROXY_LOG.write_text(proxy_log, encoding="utf-8")
        return agent.SESSION_MARKER

    try:
        agent.client_running = lambda: False

        with tempfile.TemporaryDirectory(prefix="blitzforge-marker-") as root:
            marker = sandbox(Path(root))
            agent.reconcile_session_marker()
            leftovers = sorted(p.name for p in Path(root).iterdir()
                               if p.name.startswith("runtime_session"))
            check("an ordinary session's marker is archived, not left to block",
                  not marker.exists() and len(leftovers) == 1
                  and ".ended-" in leftovers[0],
                  f"found {leftovers}")
            check("its content survives for reading",
                  "phase=running" in (Path(root) / leftovers[0])
                  .read_text(encoding="utf-8"))
            agent.reconcile_session_marker()      # absent now
            check("reconciling an absent marker is a no-op",
                  sorted(p.name for p in Path(root).iterdir()
                         if p.name.startswith("runtime_session")) == leftovers)

        with tempfile.TemporaryDirectory(prefix="blitzforge-crash-") as root:
            marker = sandbox(Path(root),
                             proxy_log="UNHANDLED exception at 0x0\n")
            agent.reconcile_session_marker()
            check("a crash recorded by the proxy keeps the marker, so SAFE "
                  "MODE still fires", marker.exists())

        with tempfile.TemporaryDirectory(prefix="blitzforge-dump-") as root:
            marker = sandbox(Path(root))
            (Path(root) / "wotb_mod_crash_20260809_183000_1234.dmp").write_bytes(b"MDMP")
            agent.reconcile_session_marker()
            check("a minidump alone keeps the marker too", marker.exists())

        with tempfile.TemporaryDirectory(prefix="blitzforge-live-") as root:
            marker = sandbox(Path(root))
            agent.client_running = lambda: True
            agent.reconcile_session_marker()
            check("a running client's marker is never touched", marker.exists())
            agent.client_running = lambda: False
            agent.SESSION_MARKER.write_text(
                f"[session]\nprocess_id={os.getpid()}\n", encoding="utf-8")
            agent.reconcile_session_marker()
            check("a marker owned by a live process is never touched",
                  marker.exists())
    finally:
        (agent.SESSION_MARKER, agent.PROXY_LOG, agent.CRASH_DUMPS,
         agent.client_running) = saved

    print("the watcher outlives its own failures")
    check("the poll body is wrapped, and KeyboardInterrupt/SystemExit are not "
          "swallowed",
          re.search(r"except \(KeyboardInterrupt, SystemExit\):\s*\n\s*raise\s*"
                    r"\n\s*except Exception:", agent_src) is not None,
          "an unhandled PermissionError from save_state killed the agent and "
          "every catalogue button with it")
    check("a failed poll backs off rather than spinning",
          re.search(r"except Exception:.*?time\.sleep\(2\.0\)\s*\n\s*continue",
                    agent_src, re.DOTALL) is not None)
    check("save_state cannot raise into the loop",
          re.search(r"def save_state.*?except OSError as error:.*?print\(",
                    agent_src, re.DOTALL) is not None)
    check("its temp file is per-process, so two agents cannot collide",
          "os.getpid()}.tmp" in agent_src,
          "a shared .tmp path is a race between a handover's two agents")

    print()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s): {', '.join(FAILURES)}")
        return 1
    print("agent lifecycle: all incident guards in place")
    return 0


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    sys.exit(main())
