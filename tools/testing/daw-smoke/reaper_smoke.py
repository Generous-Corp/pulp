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
  editor-open        insert the plugin, open its editor, confirm it rendered
    keyboard-routing   Open the editor, put it in front, and press keys AT it. Asserts
                       from the HOST's own state — not the plugin's — that Space still
                       reached the transport while the editor had the keyboard, and
                       that a key the editor consumes did not also move the host. This
                       is the half no headless test can reach: whether the editor
                       consumed a key is invisible from inside the editor, and a plugin
                       that silently swallows Space is indistinguishable from one that
                       routes correctly until a musician presses it.
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
    gpu-fault          Load the GPU fault-continuity test plug-in on its shared-I/O
                       GPU path, float its editor, and render the track online
                       (realtime) while the plug-in fires one GPU fault
                       (--gpu-fault) at a recorded project sample. The plug-in's
                       own capture and REAPER's realtime render are nulled against
                       an independent CPU reference across the fault window, and a
                       later offline bounce is nulled as an offline/PDC cross-check.
                       Per-block delivery counters must show the GPU carrying the
                       audio before the fault and the fallback taking over after
                       it. A pulp.gpu-audio.daw-continuity.v1 receipt is written
                       (verify it with
                       tools/validation/verify_gpu_audio_daw_continuity_receipt.py).
                       A REAPER crash on render_force_loss is recorded as FAIL, not
                       hidden. --planted / --gpu-off are the negatives; --dry-run
                       runs the evidence chain with no REAPER.
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
import json
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
KEY_ROUTING_LUA = HERE / "keyboard_routing.lua"
GPU_FAULT_LUA = HERE / "gpu_fault.lua"

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


def copy_bundle_preserving_signature(src: Path, dst: Path) -> None:
    """Copy a macOS plugin bundle without breaking its code signature.

    A signed bundle can carry per-file ad-hoc signatures on non-executable
    sidecars that live in Contents/MacOS (codesign treats every file there as
    nested code) — those signatures are stored as `com.apple.cs.*` extended
    attributes, not in the file bytes. `shutil.copytree` does not preserve
    those xattrs, so a byte-identical copy still fails
    `codesign --verify --deep --strict` with "code object is not signed at
    all" on the sidecar, even though the original bundle verifies clean.
    `ditto` is Apple's own tool for exactly this case: it preserves extended
    attributes, resource forks, and ACLs, so the copy verifies the same as
    the original. Code signatures exist only on macOS, and only macOS ships
    `ditto`, so elsewhere a symlink-preserving tree copy is the whole job.
    """
    if sys.platform != "darwin":
        shutil.copytree(src, dst, symlinks=True)
        return
    subprocess.run(["ditto", str(src), str(dst)], check=True)


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


def exact_reaper(path: str) -> Path | None:
    """Resolve a caller-selected REAPER executable without falling back."""
    candidate = Path(os.path.expanduser(path))
    if (
        not candidate.is_absolute() or candidate.is_symlink()
        or not candidate.is_file() or not os.access(candidate, os.X_OK)
    ):
        return None
    return candidate.resolve()


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


def stop_reaper_process(proc: subprocess.Popen, timeout: float = 2.0) -> None:
    """Stop and reap the exact REAPER child represented by ``proc``.

    Keep ownership attached to the ``Popen`` object instead of saving its
    numeric PID. Once a child exits and is reaped, that number may identify an
    unrelated process; sending a second raw ``kill`` to it is unsafe.
    """
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=timeout)


def kill_reaper() -> None:
    """Optionally stop every REAPER on a dedicated, explicitly opted-in host.

    `pkill -x REAPER` killed every REAPER on the machine, including the one
    the person at the keyboard was working in. On a shared machine that is
    somebody's session and unsaved work, not a test fixture. Reported as
    "REAPER keeps restarting"; it was being killed and relaunched underneath
    them.
    """
    if os.environ.get("PULP_DAW_SMOKE_KILL_ALL") == "1":
        # The old behaviour, for a dedicated CI box with nobody at it.
        subprocess.run(["pkill", "-9", "-x", "REAPER"], capture_output=True)
        return
    # Nothing to do: without a pid there is no REAPER this harness owns, and
    # killing one it did not start is not its business.
    log("not killing REAPER: this harness only stops the instance it started "
        "(set PULP_DAW_SMOKE_KILL_ALL=1 on a machine with nobody at it)")


def cache_publishes(cache_text: str, target: str) -> bool:
    """True once a REAPER plug-in cache holds a scanned ENTRY naming `target`.

    REAPER writes a bundle's `[<file>.clap]` section header before it has
    scanned the bundle, and a bundle is usually named after its plug-in, so
    matching the header let the pre-warm kill REAPER with an empty section.
    The scripted launch then did not know the plug-in and the run ended
    INCONCLUSIVE ("could not find/insert the FX"). Only an entry line
    (`id=...|Name (Vendor)`) counts.
    """
    target = target.casefold()
    for line in cache_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith("[") or "=" not in stripped:
            continue
        if target in stripped.split("=", 1)[1].casefold():
            return True
    return False


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
        # Extra portable reaper.ini keys a mode needs (never written back).
        self.extra_ini: dict[str, str] = {}

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
        # Seed the portable config FROM the real one, rather than starting
        # from nothing.
        #
        # The throwaway config is deliberate: it is what keeps a run from
        # polluting somebody's REAPER install. But an EMPTY one makes REAPER
        # come up as a fresh install every single launch -- licence prompt,
        # first-run preferences, and the audio-hardware setup dialog, over and
        # over. That is what this did to the person whose machine it ran on,
        # and this harness ships in Pulp, so it would have done it to everyone
        # who ran it.
        #
        # Their settings are COPIED, never written back: the licence, the audio
        # device, every preference they have chosen. Only the plugin-scan paths
        # are overridden, which is the one thing this run legitimately needs to
        # differ.
        real_dir = Path(os.path.expanduser("~/Library/Application Support/REAPER"))
        for extra in ("reaper-license.rk", "reaper-license.txt",
                      "reaper-menu.ini", "reaper-fxfolders.ini"):
            src = real_dir / extra
            if src.exists():
                try:
                    shutil.copy2(src, self.portable / extra)
                except Exception:
                    pass

        ours = {"vstpath": str(self.scan_dir), "clap_path": str(self.scan_dir),
                "splashscreen": "0",
                # Never reopen the user's last project.
                #
                # Seeding from the real reaper.ini carries `lastproject` with
                # it, so this "isolated" REAPER opened somebody's actual
                # session — and because that session referenced a plugin the
                # smoke had not installed, REAPER came up on a modal "Project
                # Load Warning" and sat there. The scripted insert never ran
                # and the smoke reported INCONCLUSIVE, which named neither the
                # dialog nor the project. Worse, it put a stranger's work on
                # screen on a machine somebody was using.
                "lastproject": "",
                "loadlastproj": "0"}
        for line in arch_lines:
            key, _, value = line.partition("=")
            ours[key] = value
        ours.update(getattr(self, "extra_ini", {}))

        base = ""
        real_ini = real_dir / "reaper.ini"
        if real_ini.exists():
            try:
                base = real_ini.read_text(errors="replace")
            except Exception:
                base = ""
        if base:
            # `[Recent]` is the user's recent-project list. It is not needed to
            # run a smoke, and copying it puts their filenames into a temp dir
            # and into REAPER's menus for the run.
            kept, section = [], ""
            for ln in base.splitlines():
                stripped = ln.strip()
                if stripped.startswith("[") and stripped.endswith("]"):
                    section = stripped.lower()
                if section == "[recent]" and not stripped.startswith("["):
                    continue
                if ln.split("=", 1)[0].strip() in ours:
                    continue
                kept.append(ln)
            out, inserted = [], False
            for ln in kept:
                out.append(ln)
                if not inserted and ln.strip().lower() == "[reaper]":
                    out.extend(f"{k}={v}" for k, v in ours.items())
                    inserted = True
            if not inserted:
                out = ["[REAPER]"] + [f"{k}={v}" for k, v in ours.items()] + out
            text = "\n".join(out) + "\n"
        else:
            text = "[REAPER]\n" + "\n".join(f"{k}={v}" for k, v in ours.items()) + "\n"
        (self.portable / "reaper.ini").write_text(text)

        if self.args.format in ("vst3", "clap"):
            copy_bundle_preserving_signature(plugin, self.scan_dir / plugin.name)
        else:  # au — no custom-path scan; install to the real folder, uninstall on exit.
            # An AU is found through the system's AudioComponent registry, and
            # that registry is not visible to a process outside a GUI login
            # session. Over SSH the component is installed, correct and
            # completely invisible: `auval -a` lists nothing, REAPER's scan
            # finds nothing, and the run ends INCONCLUSIVE — which reads as
            # flakiness and sends the next person looking for a scan bug that
            # is not there. VST3 and CLAP are unaffected because they are
            # scanned from a directory.
            if os.environ.get("SSH_CONNECTION") or os.environ.get("SSH_TTY"):
                log("AU cannot be proven from this session: the AudioComponent "
                    "registry is not visible outside a GUI login session, so "
                    "the component is installed and unfindable. Run this from "
                    "a Terminal on the machine itself. (VST3 and CLAP do work "
                    "over SSH — they are scanned from a directory.)")
                return EXIT_SKIP
            comp = Path(os.path.expanduser("~/Library/Audio/Plug-Ins/Components")) / plugin.name
            comp.parent.mkdir(parents=True, exist_ok=True)
            if comp.exists():
                # Asking about the component that is ALREADY installed is the
                # normal case, not a collision: an AU is proven where the host
                # will actually find it. Use it in place and leave
                # `au_installed` unset, so teardown does not uninstall
                # something this run did not install. Refusing outright made
                # the AU leg unrunnable on any machine with the plugin
                # installed -- which is every machine anyone would test on.
                if comp.resolve() == plugin.resolve():
                    log(f"using the component already installed at {comp}")
                else:
                    log(f"refusing to clobber existing {comp} — FAIL")
                    return EXIT_FAIL
            else:
                copy_bundle_preserving_signature(plugin, comp)
                self.au_installed = comp
            subprocess.run(["killall", "-9", "AudioComponentRegistrar"], capture_output=True)
        return None

    def run_until_fx_shown(self, env: dict, status: Path, script: Path = LUA,
                           before_scripted=None) -> int | None:
        """Pre-warm scan (a launch we kill so the scripted run finds the FX), then
        the scripted REAPER run (`script`, default insert+float). Returns an EXIT_*
        code on a non-PASS terminal outcome, or None once the FX is shown and
        floating. The script writes an `FX_SHOWN`/`FX_NOT_FOUND` handshake to
        `status`; a mode with a longer scripted drive continues in its own lua and
        signals completion separately."""
        # Nothing of ours is running yet, and somebody else's REAPER is not
        # this harness's to close.
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
                    if cache_publishes(cache.read_text(encoding="utf-8", errors="replace"), target):
                        cache_ready = True
                        break
                except OSError:
                    continue
            if cache_ready:
                break
        if not cache_ready:
            log("pre-warm scan did not publish the target into REAPER's plugin "
                "cache before timeout; scripted launch may remain inconclusive.")
        stop_reaper_process(warm)

        if before_scripted is not None:
            before_scripted()
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
                stop_reaper_process(self.proc)
                self.proc = None
        except Exception:  # noqa: BLE001
            pass

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
    # QUALIFIED BY FORMAT. TrackFX_AddByName with a bare name lets REAPER pick
    # whichever format it finds first, so `--format clap` inserted the
    # installed AU and the run reported a CLAP pass for an AU -- proven by a
    # crash report naming com.generous.forge.modular.au during a clap run.
    # Three legs that all load the same plugin are one leg wearing three hats.
    env["PULP_DAW_SMOKE_FX"] = {
        "vst3": "VST3:", "clap": "CLAP:", "au": "AU:",
    }.get(args.format, "") + args.plugin_name
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
    status = session.portable / "status.txt"
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


def _osa(script: str) -> str:
    r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True)
    return r.stdout.strip()


def _floating_editor_bounds(plugin_name: str) -> tuple[int, int, int, int] | None:
    """Where REAPER put the plugin's floating editor, on screen.

    Read from the window server rather than assumed: REAPER places an FX window
    wherever it likes, and a hardcoded coordinate is a click into the desktop.
    """
    for probe in (f'whose name contains "{plugin_name}"',
                  'whose name contains "FX:"'):
        pos = _osa(f'tell application "System Events" to tell process "REAPER" '
                   f'to get position of window 1 {probe}')
        size = _osa(f'tell application "System Events" to tell process "REAPER" '
                    f'to get size of window 1 {probe}')
        if pos and size:
            try:
                x, y = (int(v) for v in pos.split(", "))
                w, h = (int(v) for v in size.split(", "))
                return x, y, w, h
            except ValueError:
                continue
    return None


def _dismiss_own_screenshot_ui() -> None:
    """Put away the Screenshot window this harness's own captures leave behind.

    `screencapture` leaves the Screenshot app running with a floating window,
    and that window then sits OVER the plugin editor -- so a run that failed
    and grabbed a screenshot armed the obstruction that blocked the next run.
    The harness cleans up after itself rather than learning to ignore a window
    that is genuinely in the way.
    """
    # screencaptureui, NOT "Screenshot". The window `screencapture` leaves
    # behind belongs to the transient capture UI, and quitting the Screenshot
    # *app* does not touch it -- which is why the dismiss appeared to do
    # nothing while the window went on covering the editor for run after run.
    # It relaunches on demand, so stopping it costs nobody anything.
    subprocess.run(["pkill", "-x", "screencaptureui"], capture_output=True)
    time.sleep(0.6)


def _editor_is_really_in_front(uidriver: str, x: int, y: int,
                               w: int, h: int, owner: str = "REAPER") -> bool:
    """Is the plugin's editor what a click at these points would actually hit?

    Asked of the WINDOW SERVER, which knows, rather than inferred from pixels
    or from which process calls itself frontmost. Believing "frontmost" did
    real harm: REAPER reported itself frontmost while its editor sat under a
    remote-desktop session and a terminal full of live agent sessions, so the
    clicks and the typed prompt went into those. On a shared machine that is
    not a failed test; it is typing into somebody's work.

    A pixel heuristic replaced it first and was worse in the other direction:
    it refused a perfectly visible editor because the rack's panels are
    lighter than the surface it was looking for. Guessing in either direction
    is avoidable when the exact question can be asked.
    """
    for fx, fy in ((0.567, 0.345), (0.52, 0.48), (0.73, 0.53)):
        px, py = int(x + w * fx), int(y + h * fy)
        r = subprocess.run([uidriver, "at", str(px), str(py)],
                           capture_output=True, text=True)
        who = r.stdout.strip()
        if who != owner:
            log(f"the window at {px},{py} belongs to {who or 'nothing'}, "
                f"not {owner}")
            return False
    return True


def _editor_rect_in(window: tuple[int, int, int, int],
                    scratch: Path) -> tuple[int, int, int, int] | None:
    """Where the PLUGIN's editor sits inside the host's window.

    A hosted editor is not the window. REAPER wraps it in a preset strip and
    pads it, so the plugin's surface is a smaller box offset down and inset --
    and window-relative fractions land in the padding. That is why a Build
    click kept missing while the window was plainly in front: one press hit
    the Module tab and started a module build nobody asked for.

    Measured rather than assumed: Forge's editor is a dark surface on REAPER's
    light grey chrome, so its bounding box is found from the pixels. A guess
    that is off by a preset strip is a click into somebody else's control.
    """
    x, y, w, h = window
    shot = scratch / "editor-rect.png"
    subprocess.run(["screencapture", "-x", f"-R{x},{y},{w},{h}", str(shot)],
                   check=False)
    _dismiss_own_screenshot_ui()
    try:
        from PIL import Image
        im = Image.open(shot).convert("RGB")
    except Exception as e:
        log(f"cannot measure the editor ({e})")
        return None
    finally:
        shot.unlink(missing_ok=True)
    factor = 2 if im.width > w * 1.5 else 1
    left, top = 0, 0
    right, bottom = im.width, im.height
    px = im.load()
    xs, ys = [], []
    colors = set()
    step = max(2, (right - left) // 200)
    for sx in range(left, right, step):
        for sy in range(top, bottom, step):
            r, g, b = px[sx, sy]
            colors.add((r, g, b))
            if r < 60 and g < 66 and b < 74:          # Forge's dark surface
                xs.append(sx)
                ys.append(sy)
    if len(colors) < 8:
        log("the editor capture is blank or nearly uniform")
        return None
    if len(xs) < 50:
        log("could not find the editor's surface inside the host window")
        return None
    ex, ey = x + min(xs) // factor, y + min(ys) // factor
    ew, eh = (max(xs) - min(xs)) // factor, (max(ys) - min(ys)) // factor
    if ew < 200 or eh < 200:
        log(f"the editor measured {ew}x{eh}, too small to be the editor")
        return None
    return ex, ey, ew, eh


def _new_run_log(runs_dir: Path, existing: set[Path]) -> Path | None:
    """Newest per-run generator log not present before this smoke began."""
    try:
        created = [p for p in runs_dir.glob("*.log") if p not in existing]
        return max(created, key=lambda p: p.stat().st_mtime_ns) if created else None
    except OSError:
        return None


def run_editor_build_mode(reaper: Path, args: argparse.Namespace) -> int:
    """Generate a patch from INSIDE the host, by pressing the editor's Build.

    editor-open proves the window comes up. It does not prove the product
    works there: the generator is spawned by the plugin, and a plugin whose
    editor draws perfectly can still fail to reach it -- the standalone once
    did exactly that, because an app launched from Finder inherits no PATH.
    Nothing short of pressing the button inside the host tests that path.

    The verdict is the generator's own success line plus the file it names,
    scraped from the plugin's log, never the click appearing to land.
    """
    plugin = Path(os.path.expanduser(args.plugin_path))
    bad = _validate_inputs([plugin])
    if bad is not None:
        return bad

    runs_dir = Path(os.path.expanduser(
        "~/Library/Application Support/Forge Modular/runs"))
    existing_logs = set(runs_dir.glob("*.log")) if runs_dir.exists() else set()
    prompt = "a classic subtractive voice with a filter envelope"
    want = re.sub(r"[^a-z0-9]+", "-", prompt.lower()).strip("-")[:40]
    _announce(f'"{args.plugin_name}" ({args.format}) editor-build smoke '
              f'(it will run a generation inside REAPER)', args.timeout)
    session = ReaperSession(reaper, args)
    trigger = session.portable / "forge-modular-test-prompt.txt"
    status = session.portable / "status.txt"
    try:
        placed = session.place_plugin()
        if placed is not None:
            return placed
        if status.exists():
            status.unlink()

        env = _common_env(args, status)
        # The shell reads this file once and builds what it names. NO synthetic
        # clicks: driving a screen typed a prompt into somebody's terminal
        # twice, because every guess about what is on screen finds a new way
        # to be wrong. The claim being proven is that the generator runs when
        # the PLUGIN spawns it, inheriting the host's environment -- and that
        # is just as true asked through a file as asked through a button.
        env["FORGE_MODULAR_TEST_PROMPT"] = str(trigger)
        not_shown = session.run_until_fx_shown(env, status)
        if not_shown is not None:
            return not_shown
        time.sleep(4)                     # let the editor settle and poll
        trigger.write_text("patch: " + prompt + "\n")
        log(f"asked the hosted plugin to build: {prompt!r}")

        started = False
        for _ in range(60):
            time.sleep(1)
            if not trigger.exists():      # the plugin consumed the request
                started = True
                break
        if not started:
            log("the hosted plugin never read the request — the build seam is "
                "not reaching it inside this host — FAIL")
            return EXIT_FAIL
        log("the hosted plugin took the request")

        plugin_log = None
        for _ in range(100):
            plugin_log = _new_run_log(runs_dir, existing_logs)
            if plugin_log is not None:
                break
            time.sleep(0.1)
        if plugin_log is None:
            log("the hosted plugin consumed the request but created no per-run log — FAIL")
            return EXIT_FAIL
        before = 0
        log(f"following this build's log: {plugin_log.name}")

        # Wait for the generation the way a person would: until the plugin's
        # log says it finished, or the cap runs out.
        deadline = time.time() + max(args.timeout, 600)
        built = ""
        while time.time() < deadline:
            time.sleep(5)
            if not plugin_log.exists():
                continue
            size = plugin_log.stat().st_size
            if size < before:
                # TRUNCATED. The app clears this log when a build starts, so
                # the file is SMALLER than where this run began reading -- and
                # an offset guard that only skips forward then skips forever.
                # The build had already succeeded; the gate simply never
                # looked. Everything in the file is now this run's by
                # definition, so start from the beginning.
                before = 0
            elif size == before:
                continue
            with open(plugin_log, errors="replace") as f:
                f.seek(before)
                text = f.read()          # only what THIS run wrote
            m = re.search(r"built (\d+) modules?, (\d+) cables? \u2192 (.+)$",
                          text, re.M)
            if m:
                built = m.group(0)
                path = Path(m.group(3).strip())
                if not path.exists():
                    log(f"the log says it built {path}, and it is not there — FAIL")
                    return EXIT_FAIL
                if not path.name.startswith(want):
                    log(f"the log names {path.name}, which is not what this run "
                        f"asked for ({want}…) — FAIL")
                    return EXIT_FAIL
                log(f"a patch was generated from inside REAPER: "
                    f"{m.group(1)} modules, {m.group(2)} cables, "
                    f"{path.name} — PASS.")
                return EXIT_PASS
            if "gave up" in text or "Traceback" in text:
                log("the generator ran inside the host and produced nothing — FAIL")
                return EXIT_FAIL
        log("the generation did not finish inside the cap; nothing is being "
            "killed and no verdict is being invented — INCONCLUSIVE")
        return EXIT_INCONCLUSIVE
    finally:
        session.cleanup()


def _reaper_editor_is_front(plugin_name: str) -> bool:
    """Is REAPER's floating plugin editor the window a key would land in?

    Two separate questions, both asked of the window server: is REAPER the
    frontmost PROCESS, and is its frontmost WINDOW the plugin's editor. A
    synthetic key is posted globally, so answering "probably" here is how a
    harness types into a window that belongs to someone else.
    """
    _osa('tell application "REAPER" to activate')
    time.sleep(1.5)
    front = _osa('tell application "System Events" to get name of first '
                 'process whose frontmost is true')
    if front != "REAPER":
        log(f"the frontmost process is {front or 'unknown'}, not REAPER")
        return False
    title = _osa('tell application "System Events" to tell process "REAPER" '
                 'to get name of front window')
    if plugin_name.casefold() not in title.casefold() and "fx:" not in title.casefold():
        log(f"REAPER's front window is {title!r}, not the plugin editor")
        return False
    return True


def _press(key_code: int, modifier: str = "") -> bool:
    """Send one key press to whatever is frontmost, via the window server.

    Returns False when macOS refused the synthetic event (no Accessibility
    grant), so the caller can SKIP with a real reason instead of reading the
    silence as a routing failure.
    """
    using = f" using {modifier}" if modifier else ""
    r = subprocess.run(
        ["osascript", "-e",
         f'tell application "System Events" to key code {key_code}{using}'],
        capture_output=True, text=True)
    if r.returncode != 0:
        log(f"the window server refused a synthetic key press: "
            f"{r.stderr.strip() or 'unknown error'}")
        return False
    return True


def _journal(path: Path) -> dict:
    """The host's own state, as its running Lua last wrote it."""
    try:
        text = path.read_text(errors="replace").strip()
    except OSError:
        return {}
    out = {}
    for token in text.split(" ", 1):
        if "=" in token:
            k, v = token.split("=", 1)
            out[k] = v
    return out


def _await_play_state(path: Path, want: int, seconds: float = 6.0) -> int | None:
    """Wait for the host transport to report `want`; return what it settled on."""
    deadline = time.time() + seconds
    last = None
    while time.time() < deadline:
        time.sleep(0.25)
        state = _journal(path).get("play")
        if state is None:
            continue
        last = int(state) & 1
        if last == want:
            return last
    return last


def run_keyboard_routing_mode(reaper: Path, args: argparse.Namespace) -> int:
    """Press keys at a real, focused plugin editor and ask the HOST what it got.

    The failure this exists for is one-directional and silent. An editor that
    consumes its own shortcuts is easy to verify and easy to get right; an
    editor that ALSO swallows the keys it did not handle looks identical from
    every vantage point inside the plugin, and the first report is a musician
    saying the spacebar stopped working. Only the host can answer, and only
    while the editor actually has the keyboard.
    """
    plugin = Path(os.path.expanduser(args.plugin_path))
    bad = _validate_inputs([plugin])
    if bad is not None:
        return bad
    if not KEY_ROUTING_LUA.exists():
        log(f"missing REAPER script: {KEY_ROUTING_LUA} — FAIL")
        return EXIT_FAIL

    _announce(f'"{args.plugin_name}" ({args.format}) keyboard-routing smoke '
              f'(it starts and stops the transport)', args.timeout)
    session = ReaperSession(reaper, args)
    status = session.portable / "status.txt"
    journal = session.portable / "transport.txt"
    try:
        placed = session.place_plugin()
        if placed is not None:
            return placed
        if status.exists():
            status.unlink()

        env = _common_env(args, status)
        env["PULP_DAW_SMOKE_JOURNAL"] = str(journal)
        not_shown = session.run_until_fx_shown(env, status, script=KEY_ROUTING_LUA)
        if not_shown is not None:
            return not_shown
        time.sleep(5)  # let the editor settle and the journal start ticking

        if not _journal(journal):
            log("REAPER never wrote a transport journal — the smoke has no "
                "instrument, so it reports nothing — INCONCLUSIVE")
            return EXIT_INCONCLUSIVE

        bounds = _floating_editor_bounds(args.plugin_name)
        if bounds is None:
            log("no floating editor window was measurable — INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        # A synthetic key goes to whatever is frontmost, so prove REAPER's
        # editor is BEFORE sending one. On a shared machine a wrong guess is
        # not a failed test; it is a Space and a ⌘Z typed into somebody's
        # terminal.
        if not _reaper_editor_is_front(args.plugin_name):
            log("could not prove REAPER's plugin editor is the frontmost "
                "window; refusing to send synthetic keys — INCONCLUSIVE, "
                "not a routing verdict")
            return EXIT_INCONCLUSIVE

        # Baseline: the host must be stopped, or "Space started playback" is
        # not a measurement.
        if (_await_play_state(journal, 0, 3.0) or 0) != 0:
            log("the host was already playing before any key was sent — "
                "INCONCLUSIVE")
            return EXIT_INCONCLUSIVE

        # THE positive control. Space, at a focused plugin editor, with
        # nothing in the editor claiming it: the transport must move.
        if not _press(49):
            log("synthetic key presses are not permitted on this machine "
                "(Accessibility) — SKIP. A SKIP is not a PASS.")
            return EXIT_SKIP
        if _await_play_state(journal, 1) != 1:
            log("Space reached a focused plugin editor and the host transport "
                "never moved — the editor swallowed it — FAIL")
            return EXIT_FAIL
        log("Space reached the host transport with the editor focused")

        if not _press(49):
            return EXIT_INCONCLUSIVE
        if _await_play_state(journal, 0) != 0:
            log("the transport started but a second Space did not stop it — "
                "INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        log("a second Space stopped it again — the host owns the key both ways")

        # The other half: a chord the editor claims must not ALSO move the
        # host. Read from the host's undo history, which is what ⌘Z moves
        # there, rather than from the plugin's own account of itself.
        before = _journal(journal).get("undo", "")
        if not _press(6, "command down"):   # 6 = Z
            return EXIT_INCONCLUSIVE
        time.sleep(2)
        after = _journal(journal).get("undo", "")
        if after != before:
            log(f"⌘Z at the plugin editor moved the HOST's undo history "
                f"({before!r} -> {after!r}) — the editor handed back a key it "
                f"should own — FAIL")
            return EXIT_FAIL
        log("⌘Z did not reach the host's undo history")

        session.terminate()
        log("keyboard routing held in a real host — PASS.")
        return EXIT_PASS
    finally:
        session.cleanup()


def run_editor_open_mode(reaper: Path, args: argparse.Namespace) -> int:
    """Insert the plugin, open its editor, confirm it rendered.

    The other three modes each assume a hot-swap or transport scenario. A
    product whose editor IS the product needs neither: the question is only
    whether the thing loads in a real host and draws. That is also the check
    the format validators cannot make -- auval and clap-validator prove a
    plugin scans and instantiates, not that its window comes up.

    Built on the same place-then-wait-for-fx-shown path the other modes use,
    so this is a fourth mode over tested machinery rather than new
    infrastructure.
    """
    plugin = Path(os.path.expanduser(args.plugin_path))
    bad = _validate_inputs([plugin])
    if bad is not None:
        return bad

    _announce(f'"{args.plugin_name}" ({args.format}) editor-open smoke', args.timeout)
    session = ReaperSession(reaper, args)
    status = session.portable / "status.txt"
    try:
        placed = session.place_plugin()
        if placed is not None:
            return placed
        if status.exists():
            status.unlink()

        env = _common_env(args, status)
        not_shown = session.run_until_fx_shown(env, status)
        if not_shown is not None:
            return not_shown

        # The window is up. Give it long enough to actually paint a frame --
        # an editor that opens and then throws is still a failure, and it
        # reports itself in the seconds after the window appears.
        time.sleep(5)
        bounds = _floating_editor_bounds(args.plugin_name)
        if bounds is None:
            log("REAPER reported the FX shown, but no floating editor window was measurable — "
                "INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        first_frame = _editor_rect_in(bounds, session.portable)
        time.sleep(1)
        second_frame = _editor_rect_in(bounds, session.portable)
        if first_frame is None or second_frame is None:
            log("the editor window did not produce two nonblank frames — INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        if any(abs(a - b) > 8 for a, b in zip(first_frame, second_frame)):
            log(f"the painted editor region was unstable: {first_frame} then {second_frame} — "
                "INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        session.terminate()

        text = session.captured_log()
        crashed = [ln for ln in text.splitlines()
                   if "PULP_DAW_SMOKE_EDITOR_FAILED" in ln
                   or "editor failed" in ln.lower()]
        if crashed:
            log("editor opened then reported failure — FAIL. reason:")
            for ln in crashed[-2:]:
                log("  " + ln.strip())
            return EXIT_FAIL
        log("editor opened and stayed up — PASS.")
        return EXIT_PASS
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
    status = session.portable / "status.txt"
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
    status = session.portable / "status.txt"
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


# ── gpu-fault mode ───────────────────────────────────────────────────────
#
# The plug-in under test is the GPU fault-continuity test plug-in
# (examples/gpu-fault-continuity). It reads env vars REAPER passes through:
# the run directory it writes its capture and events into, the impulse
# response the harness chose, the fault request file, and a per-run nonce it
# stamps into its events. The analysis lives in gpu_fault_continuity.py so the
# dry run and the verifier share it with the live run.

# The shortest post-fault windows the verifier accepts (1 s live, 100 ms for a
# synthetic dry run); see gpu_fault_continuity.MIN_WINDOW_POST*.
LIVE_WINDOW_POST_FLOOR = 48000
DRY_RUN_WINDOW_POST_FLOOR = 4800


def _gpu_fault_lib():
    import importlib.util
    spec = importlib.util.spec_from_file_location("gpu_fault_continuity",
                                                  HERE / "gpu_fault_continuity.py")
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def _host_preflight() -> dict:
    """The P2 quiet-host preflight document written to preflight.json. An
    unavailable preflight is a not-quiet host, never a quiet one; the receipt's
    summary is derived from this document so the verifier can re-derive it."""
    import importlib.util
    path = HERE.parents[1] / "scripts" / "gpu_audio_p2_host_preflight.py"
    try:
        spec = importlib.util.spec_from_file_location("gpu_audio_p2_host_preflight", path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod.collect()
    except Exception as exc:  # noqa: BLE001 - the label must survive any probe failure
        return {"status": "unavailable", "quiet_host": False,
                "reasons": [f"preflight_error: {exc}"]}


def _reaper_version(reaper: Path) -> str | None:
    try:
        import plistlib
        plist = reaper.parents[1] / "Info.plist"
        with open(plist, "rb") as f:
            return str(plistlib.load(f).get("CFBundleShortVersionString"))
    except Exception:  # noqa: BLE001
        return None


def _bundle_executable(bundle: Path) -> Path | None:
    macos = bundle / "Contents" / "MacOS"
    found = [p for p in macos.iterdir() if p.is_file()] if macos.is_dir() else []
    return found[0] if len(found) == 1 else None


def _wait_for_event(events: Path, names, timeout: float, proc) -> dict | None:
    lib = sys.modules.get("gpu_fault_continuity") or _gpu_fault_lib()
    names = (names,) if isinstance(names, str) else tuple(names)
    deadline = time.time() + timeout
    while time.time() < deadline:
        found = next((e for e in lib.load_events(events) if e.get("event") in names), None)
        if found is not None:
            return found
        if proc is not None and proc.poll() is not None:
            return None
        time.sleep(0.25)
    return None


def _wait_for_stable_file(path: Path, deadline: float, proc, settle: float = 3.0) -> bool:
    """The render action can hand control back while REAPER is still writing
    the file, and stopping REAPER then leaves a bounce whose second half is
    silence. Wait until the file has stopped changing for `settle` seconds."""
    last, since = None, time.time()
    while time.time() < deadline:
        size = path.stat().st_size if path.exists() else -1
        if size != last:
            last, since = size, time.time()
        elif size > 0 and time.time() - since >= settle:
            return True
        if proc is not None and proc.poll() is not None:
            return size > 0
        time.sleep(0.5)
    return False


def _finish_gpu_fault(lib, run_dir: Path, args: argparse.Namespace, *, transport: str,
                      host_alive: bool, bounce: Path | None, identity: dict, nonce: str,
                      reaper_pid: int | None) -> int:
    preflight = lib.summarize_preflight(
        json.loads((run_dir / "preflight.json").read_text(encoding="utf-8")))
    names = ["stimulus.wav", "ir.f32", "events.jsonl", "capture.bin", "preflight.json",
             "reaper.out.txt", "claim", "realtime.wav", "realtime_render.json"] + (
                 [bounce.name] if bounce is not None else [])
    artifacts = lib.artifact_digests(run_dir, names)
    render_frames = args.stimulus_frames + args.window_post
    evidence = lib.evaluate_run(run_dir, fault=args.gpu_fault, fault_sample=args.fault_at_sample,
                                tolerance=args.null_tolerance, window_pre=args.window_pre,
                                window_post=args.window_post, host_alive=host_alive,
                                bounce_path=bounce, render_frames=render_frames, nonce=nonce,
                                reaper_pid=reaper_pid, listed=list(artifacts),
                                realtime_path=run_dir / "realtime.wav",
                                host_quiet=None if transport == "dry_run" else preflight["quiet_host"])
    receipt = lib.build_receipt(
        created_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        trigger_transport=transport,
        control_op=None,
        nonce=nonce,
        analysis={"tolerance": args.null_tolerance, "render_frames": render_frames,
                  "window": {"pre": args.window_pre, "post": args.window_post}},
        bounce_artifact=bounce.name if bounce is not None and bounce.name in artifacts else None,
        host_realtime_artifact="realtime.wav" if "realtime.wav" in artifacts else None,
        preflight=preflight,
        host_quiet=preflight["quiet_host"],
        artifacts=artifacts,
        **identity, **evidence)
    out = run_dir / "receipt.json"
    out.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    log(f"receipt: {out}")
    log(f"host quiet: {receipt['host_quiet']} ({', '.join(preflight.get('reasons', [])) or 'no reasons'})")
    for name, result in evidence["nulls"].items():
        if isinstance(result, dict):
            log(f"{name} null: passed={result['passed']} max_abs_error={result['max_abs_error']} "
                f"window_max={result['window_max_abs_error']} "
                f"first={result['first_deviating_sample']} last={result['last_deviating_sample']}")
    log(f"health: {evidence['health']['note']}")
    verdict = evidence["verdict"]
    reasons = "; ".join(evidence["verdict_reasons"]) or "all checks held"
    log(f"gpu-fault {args.gpu_fault}: {verdict.upper()} — {reasons}")
    return {"pass": EXIT_PASS, "fail": EXIT_FAIL}.get(verdict, EXIT_INCONCLUSIVE)


def run_gpu_fault_dry_run(args: argparse.Namespace) -> int:
    """The whole evidence chain without REAPER: synthetic capture and bounce
    built from the reference, then the same analysis, receipt and verifier a
    live run uses. --dry-run-break-fallback plants the negative (silence after
    the fault) and must come back FAIL."""
    import secrets
    lib = _gpu_fault_lib()
    run_dir = Path(args.out_dir or tempfile.mkdtemp(prefix="pulp-gpu-fault-dry-"))
    nonce = "dry-run-" + secrets.token_hex(8)
    lib.synthesize_run(run_dir, fault=args.gpu_fault, fault_sample=args.fault_at_sample,
                       frames=args.stimulus_frames, nonce=nonce, tail=args.window_post,
                       planted="gpu_off" if args.gpu_off else args.planted)
    preflight = {"status": "dry_run", "quiet_host": False, "reasons": ["dry_run"]}
    (run_dir / "preflight.json").write_text(json.dumps(preflight) + "\n", encoding="utf-8")
    identity = {"host": {"reaper_version": None, "machine": platform.machine(),
                         "os": platform.platform()},
                "plugin": {"format": args.format, "name": args.plugin_name or "dry-run",
                           "executable_sha256": None}}
    return _finish_gpu_fault(lib, run_dir, args, transport="dry_run", host_alive=True,
                             bounce=run_dir / "bounce.wav", identity=identity, nonce=nonce,
                             reaper_pid=None)


def run_gpu_fault_mode(reaper: Path, args: argparse.Namespace) -> int:
    lib = _gpu_fault_lib()
    plugin = Path(os.path.expanduser(args.plugin_path))
    bad = _validate_inputs([plugin, GPU_FAULT_LUA])
    if bad is not None:
        return bad
    executable = _bundle_executable(plugin)
    if executable is None:
        log(f"{plugin} has no single bundle executable — FAIL")
        return EXIT_FAIL

    import secrets
    run_dir = Path(args.out_dir or tempfile.mkdtemp(prefix="pulp-gpu-fault-"))
    run_dir.mkdir(parents=True, exist_ok=True)
    preflight = _host_preflight()
    (run_dir / "preflight.json").write_text(json.dumps(preflight, indent=2, sort_keys=True,
                                                       default=str) + "\n", encoding="utf-8")
    log(f"host preflight: status={preflight.get('status')} "
        f"quiet={preflight.get('quiet_host') is True}")
    # Stamped by the plug-in into every `prepared` event, so the events this
    # receipt rests on provably come from this run.
    nonce = secrets.token_hex(16)

    frames = args.stimulus_frames
    stim = lib.make_stimulus(frames)
    lib.write_wav_f32(run_dir / "stimulus.wav", [stim] * lib.CHANNELS)
    lib.write_f32(run_dir / "ir.f32", lib.make_impulse_response())
    bounce = run_dir / "bounce.wav"
    realtime = run_dir / "realtime.wav"
    # Render far enough past the fault for the `after` snapshot.
    render_end = (frames + args.window_post) / 48000.0
    # RENDER_1X 2 = "Online Render": the fault pass renders in realtime, with the
    # plug-in's GPU path live. No API call can set it; only a project file can.
    project = run_dir / "gpu-fault.rpp"
    project.write_text('<REAPER_PROJECT 0.1 "7.0" 0\n  SAMPLERATE 48000 1 0\n'
                       "  RENDER_1X 2\n>\n", encoding="utf-8")

    # The fault request reaches the plug-in through a file. WP-A2's
    # gpu_audio_fault_inject control op cannot reach a plug-in inside REAPER
    # (only the standalone host joins the control broker), so the receipt says
    # in_process_file and is not control-op evidence.
    transport = "in_process_file"
    request = run_dir / "fault-request.json"
    fault_request = {"fault": args.gpu_fault, "at_project_sample": args.fault_at_sample,
                     "after_project_sample": args.fault_at_sample + args.window_post}
    if args.fault_delay_ms is not None:
        fault_request["delay_ms"] = args.fault_delay_ms
    if args.planted:
        fault_request["planted_negative"] = args.planted

    request.write_text(json.dumps(fault_request) + "\n", encoding="utf-8")
    go = run_dir / "go"

    _announce(f'"{args.plugin_name}" ({args.format}) gpu-fault {args.gpu_fault} smoke '
              "(online realtime render, then an offline bounce)", args.timeout)
    session = ReaperSession(reaper, args)
    session.extra_ini["renderclosewhendone"] = "4"
    status = session.portable / "status.txt"
    identity = {"host": {"reaper_version": _reaper_version(reaper), "reaper_bin": str(reaper),
                         "machine": platform.machine(), "os": platform.platform()},
                "plugin": {"format": args.format, "name": args.plugin_name,
                           "bundle": str(plugin.resolve()),
                           "executable_sha256": lib.sha256_file(executable)},
                "anticipative_fx": bool(args.anticipative_fx)}
    host_alive = True
    try:
        placed = session.place_plugin()
        if placed is not None:
            return placed
        env = _common_env(args, status)
        env.update({
            "PULP_GPU_FAULT_RUN_DIR": str(run_dir),
            "PULP_GPU_FAULT_IR": str(run_dir / "ir.f32"),
            "PULP_GPU_FAULT_REQUEST": str(request),
            "PULP_GPU_FAULT_GO": str(go),
            "PULP_GPU_FAULT_BOUNCE": str(bounce),
            "PULP_GPU_FAULT_RENDER_END": f"{render_end:.6f}",
            "PULP_GPU_FAULT_PROJECT": str(project),
            "PULP_GPU_FAULT_REALTIME": str(realtime),
            "PULP_GPU_FAULT_ANTICIPATIVE": "1" if args.anticipative_fx else "0",
            # The harness may wait one timeout for `prepared` and another for
            # `fault_armed` after FX_SHOWN; the script must outlast both.
            "PULP_GPU_FAULT_GO_TIMEOUT": str(2 * args.timeout + 30),
            "PULP_GPU_FAULT_NONCE": nonce,
        })
        def forget_scan_instances() -> None:
            # The pre-warm scan launch instantiated the plug-in with the same
            # environment; whatever it claimed or wrote is not this run's.
            for name in ("claim", "capture.bin", "events.jsonl"):
                (run_dir / name).unlink(missing_ok=True)

        env["PULP_GPU_FAULT_STIMULUS"] = str(run_dir / "stimulus.wav")
        if args.gpu_off:
            env["PULP_GPU_FAULT_PROVIDER"] = "staged"
        if args.stimulus == "internal":
            # Read in the plug-in's constructor, before its first callback.
            env["PULP_GPU_FAULT_INPUT"] = "internal"
        not_shown = session.run_until_fx_shown(env, status, script=GPU_FAULT_LUA,
                                               before_scripted=forget_scan_instances)
        if not_shown is not None:
            return not_shown

        events = run_dir / "events.jsonl"
        if _wait_for_event(events, "prepared", args.timeout, session.proc) is None:
            log("the plug-in never logged `prepared` (not the fault-continuity build, or the "
                "shared-IO GPU path refused) — INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        armed = _wait_for_event(events, ("fault_armed", "fault_refused"), args.timeout,
                                session.proc)
        if armed is None or armed.get("event") != "fault_armed":
            log("the plug-in did not arm the fault"
                + (f": {armed.get('reason')}" if armed else "") + " — INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        go.write_text("go\n", encoding="utf-8")

        # An online render of the whole stimulus in realtime, then the bounce.
        # Its wall-clock span is the provenance that it was a realtime pass.
        deadline = time.time() + args.timeout + render_end * 4
        done, text = False, ""
        realtime_started = realtime_finished = None
        while time.time() < deadline:
            time.sleep(0.05)
            text = status.read_text(encoding="utf-8").strip() if status.exists() else ""
            now_us = int(time.time() * 1e6)
            if text.startswith("RENDERING_REALTIME") and realtime_started is None:
                realtime_started = now_us
            if text.startswith(("RENDERING_OFFLINE", "RENDER_")) and realtime_started is not None \
                    and realtime_finished is None:
                realtime_finished = now_us
            if text.startswith(("RENDER_DONE", "RENDER_MISSING", "GO_TIMEOUT")):
                done = True
                break
            if session.proc is not None and session.proc.poll() is not None:
                break
        host_alive = session.proc is not None and session.proc.poll() is None
        if not host_alive:
            log(f"REAPER exited during the run (code {session.proc.returncode}) — recording "
                "a host crash as a FAIL finding")
        elif not done:
            log("REAPER did not finish the render within timeout — INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        elif text.startswith("RENDER_DONE") and not _wait_for_stable_file(
                bounce, deadline, session.proc):
            log("the bounce never stopped growing before the timeout — INCONCLUSIVE")
            return EXIT_INCONCLUSIVE
        host_alive = session.proc is not None and session.proc.poll() is None
        reaper_pid = session.proc.pid if session.proc is not None else None
        if realtime_started is not None and realtime_finished is not None:
            (run_dir / "realtime_render.json").write_text(json.dumps({
                "artifact": realtime.name, "mode": "realtime", "host": "reaper",
                "sample_rate": 48000, "started_unix_us": realtime_started,
                "finished_unix_us": realtime_finished}) + "\n", encoding="utf-8")
        identity["host"]["reaper_pid"] = reaper_pid
        session.terminate()
        shutil.copyfile(session.reaper_out, run_dir / "reaper.out.txt")
        return _finish_gpu_fault(lib, run_dir, args, transport=transport,
                                 host_alive=host_alive,
                                 bounce=bounce if bounce.exists() else None,
                                 identity=identity, nonce=nonce, reaper_pid=reaper_pid)
    finally:
        session.cleanup()


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="REAPER functional smoke for Pulp reload/editor/live-swap changes.")
    ap.add_argument("--mode",
                    choices=["reload", "live-plugin-swap", "sequence-loop-seek",
                             "editor-open", "editor-build", "keyboard-routing", "gpu-fault"],
                    default="reload",
                    help="reload (default) hot-swaps a watched DSP artifact; "
                         "live-plugin-swap drives a hosted plugin-instance swap; "
                         "sequence-loop-seek loops + seeks the transport and asserts an "
                         "embedded sequence followed the host playhead with no dropout; "
                         "keyboard-routing presses keys at the focused editor and asserts "
                         "from the host's own state which ones reached it; "
                         "gpu-fault injects one GPU-audio fault during playback and nulls the "
                         "plug-in output and the bounce against a CPU reference.")
    ap.add_argument("--plugin-name", default="",
                    help='FX name as REAPER lists it, e.g. "Pulp Hot-Reload Morph".')
    ap.add_argument("--format", choices=["vst3", "clap", "au"], default="vst3",
                    help="vst3/clap scan from a temp path (no pollution); au installs+uninstalls (last resort).")
    ap.add_argument("--plugin-path", default="",
                    help="Path to the built .vst3/.clap/.component bundle.")
    ap.add_argument("--timeout", type=int, default=90, help="Per-phase wall-clock cap (s).")
    ap.add_argument("--reaper-bin",
                    help="Exact absolute REAPER executable; invalid paths fail instead of falling back.")
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

    fault_group = ap.add_argument_group("gpu-fault mode")
    fault_group.add_argument("--gpu-fault",
                             choices=["force_loss_before", "force_loss_between",
                                      "force_loss_after", "delay_completion", "worker_stall",
                                      "missing_completion", "render_force_loss", "positive_control"],
                             help="The fault to inject. render_force_loss loses the editor's "
                                  "render device, not the audio device; positive_control is the "
                                  "control.")
    fault_group.add_argument("--planted",
                             choices=["no_fallback", "fallback_misaligned", "fallback_stale"],
                             help="Planted negative (test-only plug-in shape); the run must FAIL.")
    fault_group.add_argument("--gpu-off", action="store_true",
                             help="Path negative: run the plug-in on the staged path; the run "
                                  "must be INCONCLUSIVE.")
    fault_group.add_argument("--stimulus", choices=["host", "internal"], default="host",
                             help="internal: the plug-in makes its own input, so nothing from a "
                                  "host input can reach the capture.")
    fault_group.add_argument("--fault-at-sample", type=int, default=96000,
                             help="Project sample at which the plug-in fires the fault.")
    fault_group.add_argument("--stimulus-frames", type=int, default=240000,
                             help="Length of the stimulus item (48 kHz frames).")
    fault_group.add_argument("--null-tolerance", type=float, default=1e-4,
                             help="Max |output - reference| per sample.")
    fault_group.add_argument("--window-pre", type=int, default=4800,
                             help="Samples before the fault in the fault-window null.")
    fault_group.add_argument("--window-post", type=int, default=48000,
                             help="Samples after the fault in the fault-window null; also "
                                  "when the plug-in takes its `after` health snapshot.")
    fault_group.add_argument("--anticipative-fx", action="store_true",
                             help="Leave REAPER's anticipative FX processing on for the track. "
                                  "It runs callbacks back to back, so the GPU never meets its "
                                  "lead and every block is CPU fallback.")
    fault_group.add_argument("--fault-delay-ms", type=int,
                             help="Worker stall for delay_completion/worker_stall (plug-in defaults: "
                                  "100 ms and 3000 ms).")
    fault_group.add_argument("--out-dir", help="Run directory for artifacts and the receipt.")
    fault_group.add_argument("--dry-run", action="store_true",
                             help="Exercise the analysis, receipt and verifier on synthetic "
                                  "captures; needs no REAPER and no plug-in.")
    fault_group.add_argument("--dry-run-break-fallback", dest="planted",
                             action="store_const", const="no_fallback",
                             help="Alias of --planted no_fallback.")
    return ap


def validate_mode_args(ap: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if args.mode == "gpu-fault":
        if not args.gpu_fault:
            ap.error("gpu-fault mode requires --gpu-fault")
        if not args.dry_run and not (args.plugin_name and args.plugin_path):
            ap.error("gpu-fault mode requires --plugin-name and --plugin-path (or --dry-run)")
        if not 0 < args.fault_at_sample < args.stimulus_frames:
            ap.error("--fault-at-sample must fall inside the stimulus")
        floor = DRY_RUN_WINDOW_POST_FLOOR if args.dry_run else LIVE_WINDOW_POST_FLOOR
        if args.window_post < floor:
            ap.error(f"--window-post must be at least {floor} samples (1 s for a live run, "
                     "100 ms for a dry run): the verifier refuses a shorter window")
        if args.fault_at_sample < args.window_pre:
            ap.error("--fault-at-sample must leave room for --window-pre before it: the "
                     "fault window has to lie entirely inside the compared audio")
        if args.stimulus == "internal" and args.dry_run:
            ap.error("--stimulus internal needs a live run")
        return
    if args.mode != "gpu-fault" and (args.dry_run or args.gpu_fault):
        ap.error("--gpu-fault/--dry-run apply only to --mode gpu-fault")
    if not (args.plugin_name and args.plugin_path):
        ap.error("--plugin-name and --plugin-path are required")
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
    elif args.mode in ("editor-open", "editor-build", "keyboard-routing"):
        # Needs nothing beyond the plugin itself: that is the point of it.
        pass
    else:  # sequence-loop-seek
        if args.loop_end <= args.loop_start:
            ap.error("sequence-loop-seek mode requires --loop-end > --loop-start "
                     f"(got start={args.loop_start}, end={args.loop_end}).")


def main() -> int:
    ap = build_parser()
    args = ap.parse_args()
    validate_mode_args(ap, args)

    if args.mode == "gpu-fault" and args.dry_run:
        return run_gpu_fault_dry_run(args)

    if args.check_config:
        cfg = load_config()
        if not cfg["enabled"]:
            log("daw-smoke.enabled is false (opt-in) — SKIP. Enable in ~/.config/pulp/daw-smoke.toml.")
            return EXIT_SKIP

    reaper = exact_reaper(args.reaper_bin) if args.reaper_bin else find_reaper()
    if reaper is None:
        if args.reaper_bin:
            log("the exact --reaper-bin is not a non-symlink executable — FAIL")
            return EXIT_FAIL
        log("REAPER not installed — SKIP (this is not a PASS). See docs/guides/daw-smoke.md.")
        return EXIT_SKIP

    if args.mode == "reload":
        return run_reload_mode(reaper, args)
    if args.mode == "live-plugin-swap":
        return run_live_plugin_swap_mode(reaper, args)
    if args.mode == "editor-open":
        return run_editor_open_mode(reaper, args)
    if args.mode == "editor-build":
        return run_editor_build_mode(reaper, args)
    if args.mode == "keyboard-routing":
        return run_keyboard_routing_mode(reaper, args)
    if args.mode == "gpu-fault":
        return run_gpu_fault_mode(reaper, args)
    return run_sequence_loop_seek_mode(reaper, args)


if __name__ == "__main__":
    sys.exit(main())
