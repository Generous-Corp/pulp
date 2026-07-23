#!/usr/bin/env python3
"""Real-DAW (REAPER) functional smoke for Pulp reload / editor / live-swap work.

WHY THIS EXISTS
    auval / pluginval / clap-validator prove a plugin SCANS + LOADS. They do NOT
    prove a FUNCTIONAL behavior happens inside a real host. Some bugs only appear
    in a host because the format adapter injects state that no headless / standalone
    path has — e.g. a SYNTHESIZED bypass parameter that made the reload param-
    contract gate reject every in-DAW reload (fixed 2026-07-04, commit 4a6e048f4).
    This harness loads a Pulp plugin in REAPER, drives a change, and asserts the
    result by scraping the plugin's runtime log.

MODES  (--mode, default reload)
    reload             Hot-swap a watched DSP artifact (the original flow): seed
                       variant A, insert+float the FX, copy variant B over the
                       watched path, assert the reload was ACCEPTED + APPLIED.
    live-plugin-swap   Drive a live plugin-INSTANCE swap inside a Pulp host that
                       hosts another plugin in a SignalGraph: insert+float the
                       host FX, write a swap request to the path the host watches,
                       and assert the swap COMMITTED with no dropout by scraping
                       the host's "[live-swap] committed" marker (a refusal logs
                       "live plugin swap refused" → FAIL). The CI-runnable mirror
                       of this mode is the headless continuity test
                       (test/test_signal_graph_live_swap_continuity.cpp); this
                       mode is the local-only in-host proof.
    sequence-loop-seek Load a Pulp plugin that embeds a sequence, set a loop region
                       on the REAPER timeline, start playback, and perform a scripted
                       series of seeks (into/out of the loop and across the loop
                       wrap). The plugin emits a per-block transport marker carrying
                       BOTH the host playhead and its own sequence read position;
                       this harness asserts the sequence FOLLOWED the host across
                       every wrap/seek with no dropout (a free-running counter that
                       ignores the host jump is caught as position drift → FAIL).
                       This is the harness half of Phase-2 DoD Proof #2 — completing
                       the proof additionally needs the embedded-sequence plugin and
                       a real REAPER run; without both it degrades to SKIP/INCONCLUSIVE.

WHEN TO USE IT  (see docs/guides/daw-smoke.md — the living rules)
    Opt-in, default OFF. Not every build. Use it for initial functional validation
    of reload/editor/live-swap behavior (before asking a human to test) and for
    risky reload/editor/format-adapter/host changes. It COMPLEMENTS the
    validators; it does not replace them.

AUDIO ETIQUETTE (interim contract, see CLAUDE.md "Local-dev audio etiquette")
    This launches REAPER with a Pulp plugin on a track and presses Play, so audio
    may be active out of the default device for the run. We ANNOUNCE the source +
    expected duration before launching, CAP the run at --timeout wall-clock
    seconds, and TEAR DOWN (kill REAPER) in a finally on every exit path.

DESIGN NOTES (adversarial)
    * Zero pollution: runs REAPER from a FRESH temp portable resource dir and scans
      the plugin from a TEMP path — never writes the user's REAPER config or plugin
      folders (VST3/CLAP). AU cannot be scanned from a custom path, so --format au
      installs to ~/Library/Audio/Plug-Ins/Components and ALWAYS uninstalls on exit.
    * Verification is headless-safe: `screencapture` returns BLACK frames without a
      Screen-Recording TCC grant, so we scrape the plugin log (captured from
      REAPER's stdout/stderr), never screencapture. Use render_to_png elsewhere for
      human-viewable visuals.
    * Cleanup always runs (finally): kill spawned REAPER, remove temp dirs, uninstall
      any AU we installed.
    * Distinct outcomes so a SKIP is NEVER mistaken for a PASS:
        0 PASS   1 FAIL   2 SKIP (REAPER not installed)   3 INCONCLUSIVE (flaky)
"""
from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import NamedTuple

EXIT_PASS, EXIT_FAIL, EXIT_SKIP, EXIT_INCONCLUSIVE = 0, 1, 2, 3

HERE = Path(__file__).resolve().parent
LUA = HERE / "insert_and_float.lua"
SEQ_LOOP_LUA = HERE / "sequence_loop_seek.lua"

# Default log markers for the live-plugin-swap mode. The success marker is
# emitted by the host plugin's NodeLiveSwapPolicy::on_instance_swapped observer
# (see test_signal_graph_live_swap_continuity.cpp for the same seam); the core
# SignalGraph logs the refusal marker on a rejected swap.
DEFAULT_SWAP_COMMITTED_MARKER = "[live-swap] committed"
DEFAULT_SWAP_REFUSED_MARKER = "live plugin swap refused"

# Markers for the sequence-loop-seek mode. The plugin under test embeds a
# sequence and, on every process block while the transport plays, emits a
# per-block marker carrying BOTH the host playhead position and the plugin's own
# sequence read position (in the same quarter-note timebase), plus a per-block
# discontinuity flag and dropout flag:
#     [seq-loop] loaded events=<int> len_qn=<float>
#     [seq-loop] play
#     [seq-loop] blk host_qn=<float> seq_qn=<float> active=<int> jump=<0|1> dropout=<0|1>
# The scraper asserts the sequence position TRACKS the host playhead across every
# loop wrap and seek (jump=1 blocks included) with no dropout. A free-running
# counter that ignores the host jump shows up as seq_qn diverging from host_qn.
DEFAULT_SEQ_LOADED_MARKER = "[seq-loop] loaded"
DEFAULT_SEQ_PLAY_MARKER = "[seq-loop] play"
DEFAULT_SEQ_BLK_PREFIX = "[seq-loop] blk"


def log(msg: str) -> None:
    print(f"[daw-smoke] {msg}", flush=True)


def find_reaper() -> Path | None:
    """Locate a REAPER binary without installing anything. Returns None if absent."""
    candidates = [
        "/Applications/REAPER.app/Contents/MacOS/REAPER",
        os.path.expanduser("~/Applications/REAPER.app/Contents/MacOS/REAPER"),
        "/tmp/REAPER.app/Contents/MacOS/REAPER",  # a scratch copy a prior run left
    ]
    for c in candidates:
        if os.path.exists(c):
            return Path(c)
    which = shutil.which("reaper") or shutil.which("REAPER")
    return Path(which) if which else None


def load_config() -> dict:
    """Read ~/.config/pulp/daw-smoke.toml (enabled/gate/strict). Missing => all off."""
    cfg = {"enabled": False, "gate": False, "strict": False}
    path = Path(os.path.expanduser("~/.config/pulp/daw-smoke.toml"))
    if not path.exists():
        return cfg
    try:
        import tomllib  # py3.11+
        with open(path, "rb") as f:
            data = tomllib.load(f)
    except Exception as e:  # noqa: BLE001 - config is best-effort
        log(f"config unreadable ({e}); treating as all-off")
        return cfg
    section = data.get("daw-smoke", data)
    for k in cfg:
        if k in section:
            cfg[k] = bool(section[k])
    return cfg


def kill_reaper() -> None:
    subprocess.run(["pkill", "-9", "-x", "REAPER"], capture_output=True)


class ReaperSession:
    """Shared REAPER lifecycle for every mode: fresh portable dir, temp plugin
    scan path (or a transient AU install), a pre-warm scan launch, and the
    scripted insert+float run — plus guaranteed teardown. Both modes differ only
    in what they SEED before launch, the TRIGGER they fire once the FX is shown,
    and how they VERIFY the captured log; the REAPER plumbing is identical."""

    def __init__(self, reaper: Path, args: argparse.Namespace):
        self.reaper = reaper
        self.args = args
        self.portable = Path(tempfile.mkdtemp(prefix="pulp-daw-smoke-reaper-"))
        self.scan_dir = Path(tempfile.mkdtemp(prefix="pulp-daw-smoke-plugins-"))
        self.reaper_out = self.portable / "reaper.out.txt"
        self.au_installed: Path | None = None
        self.proc: subprocess.Popen | None = None

    def place_plugin(self) -> int | None:
        """Put the plugin where REAPER finds it without touching real folders.
        Returns an EXIT_* code on failure, else None."""
        plugin = Path(os.path.expanduser(self.args.plugin_path))
        machine = platform.machine().lower()
        arch_lines: list[str] = []
        if sys.platform == "darwin":
            # Mark plugin-path preferences initialized. Without this, a fresh
            # cfgfile appends every system VST3 directory and can spend the
            # entire bounded pre-warm rescanning unrelated installed plugins.
            arch_lines.append("vstfullstate=33605")
            if machine in ("arm64", "aarch64"):
                arch_lines.extend([
                    "vstpath_arm64=" + str(self.scan_dir),
                    "clap_path_macos-aarch64=" + str(self.scan_dir),
                ])
            elif machine in ("x86_64", "amd64"):
                arch_lines.extend([
                    "vstpath64=" + str(self.scan_dir),
                    "clap_path_macos-x86_64=" + str(self.scan_dir),
                ])
        (self.portable / "reaper.ini").write_text(
            "[REAPER]\nsplashscreen=0\nvstpath=" + str(self.scan_dir)
            + "\nclap_path=" + str(self.scan_dir) + "\n"
            + "\n".join(arch_lines) + "\n"
        )
        if self.args.format in ("vst3", "clap"):
            shutil.copytree(plugin, self.scan_dir / plugin.name)
        else:  # au — no custom-path scan; install to the real folder, uninstall on exit.
            comp = Path(os.path.expanduser("~/Library/Audio/Plug-Ins/Components")) / plugin.name
            comp.parent.mkdir(parents=True, exist_ok=True)
            if comp.exists():
                log(f"refusing to clobber existing {comp} — FAIL")
                return EXIT_FAIL
            shutil.copytree(plugin, comp)
            self.au_installed = comp
            subprocess.run(["killall", "-9", "AudioComponentRegistrar"], capture_output=True)
        return None

    def run_until_fx_shown(self, env: dict, status: Path, script: Path = LUA) -> int | None:
        """Pre-warm scan (a launch we kill so the scripted run finds the FX), then
        the scripted REAPER run (`script`, default insert+float). Returns an EXIT_*
        code on a non-PASS terminal outcome, or None once the FX is shown and
        floating. The script writes an `FX_SHOWN`/`FX_NOT_FOUND` handshake to
        `status`; a mode with a longer scripted drive continues in its own lua and
        signals completion separately."""
        kill_reaper()
        with open(self.reaper_out, "w") as out:
            warm = subprocess.Popen(
                [str(self.reaper), "-cfgfile", str(self.portable / "reaper.ini")],
                stdout=out, stderr=subprocess.STDOUT, env=env)
        # A fixed warm-up sleep can repeatedly kill a fresh REAPER instance
        # while it is still scanning a large bundle, before its cache is
        # durable. The scripted launch then starts the same scan from scratch
        # and never reaches the startup Lua. Wait until the target is visible
        # in a plugin cache (bounded by the ordinary mode timeout) instead.
        warm_deadline = time.time() + self.args.timeout
        target = self.args.plugin_name.casefold()
        cache_ready = False
        while time.time() < warm_deadline:
            time.sleep(0.5)
            caches = list(self.portable.glob("reaper-*clap*.ini"))
            caches.extend(self.portable.glob("reaper-*vst*.ini"))
            for cache in caches:
                try:
                    if target in cache.read_text(errors="replace").casefold():
                        cache_ready = True
                        break
                except OSError:
                    continue
            if cache_ready:
                break
        if not cache_ready:
            log("pre-warm scan did not publish the target into REAPER's plugin "
                "cache before timeout; scripted launch may remain inconclusive.")
        warm.terminate(); time.sleep(2); warm.kill(); kill_reaper(); time.sleep(2)

        with open(self.reaper_out, "a") as out:
            self.proc = subprocess.Popen(
                [str(self.reaper), "-cfgfile", str(self.portable / "reaper.ini"), str(script)],
                stdout=out, stderr=subprocess.STDOUT, env=env)
        deadline = time.time() + self.args.timeout
        while time.time() < deadline:
            time.sleep(2)
            if status.exists():
                s = status.read_text().strip()
                if s.startswith("FX_SHOWN"):
                    return None
                if s.startswith("FX_NOT_FOUND"):
                    log("REAPER could not find/insert the FX (scan issue) — INCONCLUSIVE.")
                    return EXIT_INCONCLUSIVE
        log("REAPER did not open the FX within timeout (flaky launch) — INCONCLUSIVE.")
        captured = self.captured_log().splitlines()
        if captured:
            log("last captured REAPER output lines:")
            for line in captured[-12:]:
                log("  " + line)
        else:
            log("REAPER produced no captured stdout/stderr.")
        return EXIT_INCONCLUSIVE

    def captured_log(self) -> str:
        return self.reaper_out.read_text(errors="replace") if self.reaper_out.exists() else ""

    def terminate(self) -> None:
        try:
            if self.proc is not None:
                self.proc.terminate(); time.sleep(2); self.proc.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_reaper()

    def cleanup(self) -> None:
        self.terminate()
        if self.au_installed and self.au_installed.exists():
            shutil.rmtree(self.au_installed, ignore_errors=True)
            subprocess.run(["killall", "-9", "AudioComponentRegistrar"], capture_output=True)
            log(f"uninstalled AU {self.au_installed}")
        shutil.rmtree(self.portable, ignore_errors=True)
        shutil.rmtree(self.scan_dir, ignore_errors=True)
        log("cleanup done (no dev plugins left behind).")


def _common_env(args: argparse.Namespace, status: Path) -> dict:
    env = dict(os.environ)
    env["PULP_DAW_SMOKE_FX"] = args.plugin_name
    env["PULP_DAW_SMOKE_STATUS"] = str(status)
    return env


def _announce(source: str, timeout: int) -> None:
    log(f"heads up — launching REAPER with {source}; audio may be active out of "
        f"the default device for up to ~{timeout}s while the smoke runs.")


def _validate_inputs(paths: list[Path]) -> int | None:
    for p in paths:
        if not p.exists():
            log(f"missing input: {p} — FAIL")
            return EXIT_FAIL
    if not LUA.exists():
        log(f"missing REAPER script: {LUA} — FAIL")
        return EXIT_FAIL
    return None


def run_reload_mode(reaper: Path, args: argparse.Namespace) -> int:
    plugin = Path(os.path.expanduser(args.plugin_path))
    watched = Path(os.path.expanduser(args.watched_logic))
    initial = Path(os.path.expanduser(args.initial_logic))
    swap = Path(os.path.expanduser(args.swap_logic))
    bad = _validate_inputs([plugin, initial, swap])
    if bad is not None:
        return bad

    _announce(f'"{args.plugin_name}" ({args.format}) hot-reload smoke', args.timeout)
    session = ReaperSession(reaper, args)
    status = Path("/tmp/pulp_daw_smoke_status.txt")
    try:
        placed = session.place_plugin()
        if placed is not None:
            return placed

        watched.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(initial, watched); os.utime(watched, None)
        if status.exists():
            status.unlink()

        env = _common_env(args, status)
        not_shown = session.run_until_fx_shown(env, status)
        if not_shown is not None:
            return not_shown

        time.sleep(4)  # editor settles + initial logic loads
        log(f"hot-swapping {watched.name} -> {swap.name}")
        shutil.copyfile(swap, watched); os.utime(watched, None)
        time.sleep(6)  # watcher fires + reload gate runs + editor pump rebuilds

        session.terminate()
        text = session.captured_log()
        rejected = [ln for ln in text.splitlines() if "reload rejected" in ln]
        applied = [ln for ln in text.splitlines() if "swapped DSP" in ln]
        loaded = [ln for ln in text.splitlines() if "loaded initial logic" in ln]
        if not loaded:
            log("no 'loaded initial logic' in the REAPER log — the plugin never loaded its logic. INCONCLUSIVE.")
            return EXIT_INCONCLUSIVE
        if rejected:
            log("RELOAD REJECTED in-host — FAIL. reject-diff:")
            for ln in rejected:
                log("  " + ln.split("]", 1)[-1].strip())
            for ln in text.splitlines():
                if "reject-diff" in ln:
                    log("  " + ln.split("]", 1)[-1].strip())
            return EXIT_FAIL
        if applied:
            log(f"reload ACCEPTED + APPLIED in-host ({len(applied)} swap(s)) — PASS.")
            for ln in applied[-2:]:
                log("  " + ln.split("]", 1)[-1].strip())
            return EXIT_PASS
        log("no 'swapped DSP' and no rejection — the swap did not take. INCONCLUSIVE.")
        return EXIT_INCONCLUSIVE
    finally:
        session.cleanup()


def run_live_plugin_swap_mode(reaper: Path, args: argparse.Namespace) -> int:
    plugin = Path(os.path.expanduser(args.plugin_path))
    watched = Path(os.path.expanduser(args.watched_swap_request))
    bad = _validate_inputs([plugin])
    if bad is not None:
        return bad

    _announce(f'"{args.plugin_name}" ({args.format}) live plugin-instance-swap smoke',
              args.timeout)
    session = ReaperSession(reaper, args)
    status = Path("/tmp/pulp_daw_smoke_status.txt")
    try:
        placed = session.place_plugin()
        if placed is not None:
            return placed

        # Seed the watched swap-request path. If the host expects an initial
        # hosted-plugin target it reads this before we trigger the swap; if not,
        # an empty seed is harmless (the host builds its own default graph).
        watched.parent.mkdir(parents=True, exist_ok=True)
        if args.initial_hosted:
            watched.write_text(args.initial_hosted)
        elif watched.exists():
            watched.unlink()
        if status.exists():
            status.unlink()

        env = _common_env(args, status)
        env["PULP_DAW_SMOKE_SWAP_REQUEST"] = str(watched)
        not_shown = session.run_until_fx_shown(env, status)
        if not_shown is not None:
            return not_shown

        time.sleep(4)  # host settles + builds its hosted SignalGraph
        log(f"requesting live plugin swap via {watched.name} -> {args.swap_target}")
        watched.write_text(args.swap_target); os.utime(watched, None)
        time.sleep(6)  # host watcher fires + stage + prepare_swap commits

        session.terminate()
        text = session.captured_log()
        committed = [ln for ln in text.splitlines() if args.committed_marker in ln]
        refused = [ln for ln in text.splitlines() if args.refused_marker in ln]
        if refused:
            log("LIVE PLUGIN SWAP REFUSED in-host — FAIL. reason:")
            for ln in refused[-2:]:
                log("  " + ln.split("]", 1)[-1].strip())
            return EXIT_FAIL
        if committed:
            log(f"live plugin swap COMMITTED in-host ({len(committed)}) with no "
                f"reported dropout — PASS.")
            for ln in committed[-2:]:
                log("  " + ln.strip())
            return EXIT_PASS
        log("no committed and no refusal marker — the host never ran the swap "
            "(missing host build or unwired watch path). INCONCLUSIVE.")
        return EXIT_INCONCLUSIVE
    finally:
        session.cleanup()


_NUM = r"[-+]?\d+(?:\.\d+)?"


class SeqLoopVerdict(NamedTuple):
    """Pure result of scraping a sequence-loop-seek run: an EXIT_* code, a one-line
    verdict, and diagnostic detail lines. Separated from I/O so it is unit-testable
    against synthetic REAPER log text with no REAPER and no plugin present."""
    code: int
    reason: str
    details: list[str]


def _num_field(name: str, line: str) -> float | None:
    m = re.search(rf"\b{re.escape(name)}=({_NUM})", line)
    return float(m.group(1)) if m else None


def analyze_seq_loop_log(
    text: str,
    *,
    tol_qn: float = 0.05,
    min_blocks: int = 8,
    loaded_marker: str = DEFAULT_SEQ_LOADED_MARKER,
    play_marker: str = DEFAULT_SEQ_PLAY_MARKER,
    blk_prefix: str = DEFAULT_SEQ_BLK_PREFIX,
) -> SeqLoopVerdict:
    """Decide whether an embedded-sequence plugin played correctly under loop/seek
    from the captured REAPER stdout. Pure function — no REAPER, no plugin.

    Verdict ladder (a SKIP/INCONCLUSIVE is NEVER a PASS):
      * INCONCLUSIVE  — the plugin never loaded a sequence, never played, the drive
                        produced too little data, or did not exercise BOTH a loop
                        wrap and a seek. We could not observe the behavior.
      * FAIL          — we observed something WRONG: a dropout on a wrap/seek
                        reposition, or the sequence position drifting away from the
                        host playhead (a free-running counter ignoring the jump).
      * PASS          — every block's sequence position tracked the host playhead
                        within `tol_qn`, across >=1 loop wrap and >=1 seek, note
                        activity was observed, and no block reported a dropout.
    """
    details: list[str] = []
    lines = text.splitlines()

    loaded = [ln for ln in lines if loaded_marker in ln]
    if not loaded:
        return SeqLoopVerdict(
            EXIT_INCONCLUSIVE,
            f"no '{loaded_marker}' marker — the plugin never loaded its sequence. INCONCLUSIVE.",
            details)
    events = _num_field("events", loaded[-1])
    if events is not None and events <= 0:
        return SeqLoopVerdict(
            EXIT_INCONCLUSIVE,
            "the plugin loaded an EMPTY sequence (events=0) — nothing to play. INCONCLUSIVE.",
            details)

    if not any(play_marker in ln for ln in lines):
        return SeqLoopVerdict(
            EXIT_INCONCLUSIVE,
            f"no '{play_marker}' marker — the transport never started. INCONCLUSIVE.",
            details)

    blocks: list[dict] = []
    malformed = 0
    for ln in lines:
        if blk_prefix not in ln:
            continue
        host = _num_field("host_qn", ln)
        seq = _num_field("seq_qn", ln)
        if host is None or seq is None:
            malformed += 1
            continue
        active = _num_field("active", ln)
        jump = _num_field("jump", ln)
        dropout = _num_field("dropout", ln)
        blocks.append({
            "host": host,
            "seq": seq,
            "active": int(active) if active is not None else 0,
            "jump": int(jump) if jump is not None else 0,
            "dropout": int(dropout) if dropout is not None else 0,
            "raw": ln.strip(),
        })

    if malformed:
        details.append(f"{malformed} malformed '{blk_prefix}' line(s) ignored (missing host_qn/seq_qn).")
    if len(blocks) < min_blocks:
        return SeqLoopVerdict(
            EXIT_INCONCLUSIVE,
            f"only {len(blocks)} '{blk_prefix}' markers (< {min_blocks}) — not enough "
            f"transport data to judge loop/seek. INCONCLUSIVE.",
            details)

    # DROPOUT first: an underrun on any block — especially a wrap/seek reposition
    # block — is a hard FAIL. This is an observed defect, not a missing observation.
    dropped = [b for b in blocks if b["dropout"] != 0]
    if dropped:
        details.append(f"{len(dropped)} block(s) reported dropout=1 (audio underran while "
                       f"repositioning on a loop wrap or seek):")
        for b in dropped[:3]:
            details.append("  " + b["raw"])
        return SeqLoopVerdict(
            EXIT_FAIL,
            "DROPOUT during loop/seek — the sequence underran while repositioning. FAIL.",
            details)

    # Coverage: classify the discontinuities the drive actually produced. A jump
    # block whose host playhead moved BACKWARD is a loop wrap (or backward seek);
    # a jump block whose host moved FORWARD is an explicit forward seek. We require
    # at least one of each so a PASS genuinely exercised loop AND seek — otherwise
    # we cannot honestly claim to have proven the behavior.
    wraps = 0
    seeks = 0
    prev: dict | None = None
    for b in blocks:
        if prev is not None and b["jump"] != 0:
            if b["host"] < prev["host"]:
                wraps += 1
            else:
                seeks += 1
        prev = b
    if wraps == 0 or seeks == 0:
        return SeqLoopVerdict(
            EXIT_INCONCLUSIVE,
            f"drive did not exercise BOTH a loop wrap and a seek (backward-jumps={wraps}, "
            f"forward-jumps={seeks}) — cannot prove loop/seek behavior. INCONCLUSIVE.",
            details)

    # CORRECTNESS: the plugin's sequence read position must equal the host playhead
    # every block — INCLUDING the wrap/seek blocks. A free-running counter that
    # ignores the host jump diverges here.
    drift = [b for b in blocks if abs(b["seq"] - b["host"]) > tol_qn]
    if drift:
        worst = max(drift, key=lambda b: abs(b["seq"] - b["host"]))
        details.append(f"{len(drift)} block(s) drifted > {tol_qn} qn between the host playhead "
                       f"and the plugin's sequence position:")
        for b in sorted(drift, key=lambda b: -abs(b["seq"] - b["host"]))[:3]:
            details.append(f"  drift={abs(b['seq'] - b['host']):.3f}qn  " + b["raw"])
        return SeqLoopVerdict(
            EXIT_FAIL,
            f"SEQUENCE DRIFT — the embedded sequence did not follow the transport across "
            f"loop/seek (worst {abs(worst['seq'] - worst['host']):.3f} qn). FAIL.",
            details)

    # Note activity: prove we actually observed the sequence produce output. Zero
    # activity is INCONCLUSIVE (short/flaky capture), not FAIL — FAIL is reserved
    # for observed-wrong (drift/dropout).
    if not any(b["active"] > 0 for b in blocks):
        return SeqLoopVerdict(
            EXIT_INCONCLUSIVE,
            "no block reported active notes (active>0) — never observed the sequence produce "
            "output. INCONCLUSIVE.",
            details)

    details.append(f"{len(blocks)} blocks, {wraps} loop-wrap + {seeks} seek discontinuities, "
                   f"max drift <= {tol_qn} qn, note activity observed, no dropout.")
    return SeqLoopVerdict(
        EXIT_PASS,
        "sequence FOLLOWED the transport across every loop wrap and seek with no dropout — PASS.",
        details)


def run_sequence_loop_seek_mode(reaper: Path, args: argparse.Namespace) -> int:
    plugin = Path(os.path.expanduser(args.plugin_path))
    bad = _validate_inputs([plugin])
    if bad is not None:
        return bad
    if not SEQ_LOOP_LUA.exists():
        log(f"missing REAPER script: {SEQ_LOOP_LUA} — FAIL")
        return EXIT_FAIL

    _announce(f'"{args.plugin_name}" ({args.format}) sequence loop/seek smoke', args.timeout)
    session = ReaperSession(reaper, args)
    status = Path("/tmp/pulp_daw_smoke_status.txt")
    try:
        placed = session.place_plugin()
        if placed is not None:
            return placed

        if status.exists():
            status.unlink()

        env = _common_env(args, status)
        env["PULP_DAW_SMOKE_LOOP_START"] = str(args.loop_start)
        env["PULP_DAW_SMOKE_LOOP_END"] = str(args.loop_end)
        not_shown = session.run_until_fx_shown(env, status, script=SEQ_LOOP_LUA)
        if not_shown is not None:
            return not_shown

        # The lua drives the loop + seeks over ~10s via deferred pumps and writes
        # SEEKS_DONE when the scripted drive completes. Wait for that (bounded by
        # --timeout) so we capture the full run, then scrape.
        log(f"driving loop region [{args.loop_start}s, {args.loop_end}s] + seeks; "
            f"waiting for the scripted drive to finish (<= {args.timeout}s).")
        deadline = time.time() + args.timeout
        drive_done = False
        while time.time() < deadline:
            time.sleep(2)
            if status.exists() and status.read_text().strip().startswith("SEEKS_DONE"):
                drive_done = True
                break

        session.terminate()
        text = session.captured_log()
        if not drive_done:
            log("REAPER did not report SEEKS_DONE within timeout (flaky drive) — INCONCLUSIVE.")
            return EXIT_INCONCLUSIVE

        verdict = analyze_seq_loop_log(
            text,
            tol_qn=args.pos_tolerance_qn,
            min_blocks=args.min_blocks,
            loaded_marker=args.loaded_marker,
            play_marker=args.play_marker,
            blk_prefix=args.blk_prefix,
        )
        for line in verdict.details:
            log("  " + line)
        log(verdict.reason)
        return verdict.code
    finally:
        session.cleanup()


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="REAPER functional smoke for Pulp reload/editor/live-swap changes.")
    ap.add_argument("--mode",
                    choices=["reload", "live-plugin-swap", "sequence-loop-seek"],
                    default="reload",
                    help="reload (default) hot-swaps a watched DSP artifact; "
                         "live-plugin-swap drives a hosted plugin-instance swap; "
                         "sequence-loop-seek loops + seeks the transport and asserts an "
                         "embedded sequence followed the host playhead with no dropout.")
    ap.add_argument("--plugin-name", required=True,
                    help='FX name as REAPER lists it, e.g. "Pulp Hot-Reload Morph".')
    ap.add_argument("--format", choices=["vst3", "clap", "au"], default="vst3",
                    help="vst3/clap scan from a temp path (no pollution); au installs+uninstalls (last resort).")
    ap.add_argument("--plugin-path", required=True,
                    help="Path to the built .vst3/.clap/.component bundle.")
    ap.add_argument("--timeout", type=int, default=90, help="Per-phase wall-clock cap (s).")
    ap.add_argument("--check-config", action="store_true",
                    help="Honor ~/.config/pulp/daw-smoke.toml: SKIP unless enabled.")

    reload_group = ap.add_argument_group("reload mode")
    reload_group.add_argument("--watched-logic",
                              help="Path the plugin watches, e.g. ~/.pulp/hot-reload-morph/logic.dylib.")
    reload_group.add_argument("--initial-logic", help="Logic variant to seed before launch (A).")
    reload_group.add_argument("--swap-logic", help="Logic variant to hot-swap to (B); must differ from A.")

    swap_group = ap.add_argument_group("live-plugin-swap mode")
    swap_group.add_argument("--watched-swap-request",
                            help="Path the host watches for a swap request (a file it re-reads).")
    swap_group.add_argument("--swap-target",
                            help="Replacement plugin identity/token/path written to the watched "
                                 "path to trigger the live instance swap.")
    swap_group.add_argument("--initial-hosted", default="",
                            help="Optional initial hosted-plugin target seeded before launch.")
    swap_group.add_argument("--committed-marker", default=DEFAULT_SWAP_COMMITTED_MARKER,
                            help="Log substring proving the swap committed (PASS).")
    swap_group.add_argument("--refused-marker", default=DEFAULT_SWAP_REFUSED_MARKER,
                            help="Log substring proving the swap was refused (FAIL).")

    seq_group = ap.add_argument_group("sequence-loop-seek mode")
    seq_group.add_argument("--loop-start", type=float, default=1.0,
                           help="Loop region start on the REAPER timeline (seconds).")
    seq_group.add_argument("--loop-end", type=float, default=3.0,
                           help="Loop region end on the REAPER timeline (seconds); must be > start.")
    seq_group.add_argument("--pos-tolerance-qn", type=float, default=0.05,
                           help="Max allowed |host_qn - seq_qn| per block before it counts as "
                                "drift (quarter notes).")
    seq_group.add_argument("--min-blocks", type=int, default=8,
                           help="Minimum '[seq-loop] blk' markers required to render a verdict.")
    seq_group.add_argument("--loaded-marker", default=DEFAULT_SEQ_LOADED_MARKER,
                           help="Log substring proving the plugin loaded its sequence.")
    seq_group.add_argument("--play-marker", default=DEFAULT_SEQ_PLAY_MARKER,
                           help="Log substring proving the transport started.")
    seq_group.add_argument("--blk-prefix", default=DEFAULT_SEQ_BLK_PREFIX,
                           help="Prefix of the per-block transport marker the plugin emits.")
    return ap


def validate_mode_args(ap: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if args.mode == "reload":
        missing = [f for f, v in (
            ("--watched-logic", args.watched_logic),
            ("--initial-logic", args.initial_logic),
            ("--swap-logic", args.swap_logic),
        ) if not v]
        if missing:
            ap.error("reload mode requires: " + ", ".join(missing))
    elif args.mode == "live-plugin-swap":
        missing = [f for f, v in (
            ("--watched-swap-request", args.watched_swap_request),
            ("--swap-target", args.swap_target),
        ) if not v]
        if missing:
            ap.error("live-plugin-swap mode requires: " + ", ".join(missing))
    else:  # sequence-loop-seek
        if args.loop_end <= args.loop_start:
            ap.error("sequence-loop-seek mode requires --loop-end > --loop-start "
                     f"(got start={args.loop_start}, end={args.loop_end}).")


def main() -> int:
    ap = build_parser()
    args = ap.parse_args()
    validate_mode_args(ap, args)

    if args.check_config:
        cfg = load_config()
        if not cfg["enabled"]:
            log("daw-smoke.enabled is false (opt-in) — SKIP. Enable in ~/.config/pulp/daw-smoke.toml.")
            return EXIT_SKIP

    reaper = find_reaper()
    if reaper is None:
        log("REAPER not installed — SKIP (this is not a PASS). See docs/guides/daw-smoke.md.")
        return EXIT_SKIP

    if args.mode == "reload":
        return run_reload_mode(reaper, args)
    if args.mode == "live-plugin-swap":
        return run_live_plugin_swap_mode(reaper, args)
    return run_sequence_loop_seek_mode(reaper, args)


if __name__ == "__main__":
    sys.exit(main())
