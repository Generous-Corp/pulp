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
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EXIT_PASS, EXIT_FAIL, EXIT_SKIP, EXIT_INCONCLUSIVE = 0, 1, 2, 3

HERE = Path(__file__).resolve().parent
LUA = HERE / "insert_and_float.lua"

# Default log markers for the live-plugin-swap mode. The success marker is
# emitted by the host plugin's NodeLiveSwapPolicy::on_instance_swapped observer
# (see test_signal_graph_live_swap_continuity.cpp for the same seam); the core
# SignalGraph logs the refusal marker on a rejected swap.
DEFAULT_SWAP_COMMITTED_MARKER = "[live-swap] committed"
DEFAULT_SWAP_REFUSED_MARKER = "live plugin swap refused"


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
        (self.portable / "reaper.ini").write_text(
            "[REAPER]\nsplashscreen=0\nvstpath=" + str(self.scan_dir)
            + "\nclap_path=" + str(self.scan_dir) + "\n"
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

    def run_until_fx_shown(self, env: dict, status: Path) -> int | None:
        """Pre-warm scan (a launch we kill so the scripted run finds the FX), then
        the scripted insert+float. Returns an EXIT_* code on a non-PASS terminal
        outcome, or None once the FX is shown and floating."""
        kill_reaper()
        with open(self.reaper_out, "w") as out:
            warm = subprocess.Popen(
                [str(self.reaper), "-cfgfile", str(self.portable / "reaper.ini")],
                stdout=out, stderr=subprocess.STDOUT, env=env)
        time.sleep(min(20, self.args.timeout))
        warm.terminate(); time.sleep(2); warm.kill(); kill_reaper(); time.sleep(2)

        with open(self.reaper_out, "a") as out:
            self.proc = subprocess.Popen(
                [str(self.reaper), "-cfgfile", str(self.portable / "reaper.ini"), str(LUA)],
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


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="REAPER functional smoke for Pulp reload/editor/live-swap changes.")
    ap.add_argument("--mode", choices=["reload", "live-plugin-swap"], default="reload",
                    help="reload (default) hot-swaps a watched DSP artifact; "
                         "live-plugin-swap drives a hosted plugin-instance swap.")
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
    else:  # live-plugin-swap
        missing = [f for f, v in (
            ("--watched-swap-request", args.watched_swap_request),
            ("--swap-target", args.swap_target),
        ) if not v]
        if missing:
            ap.error("live-plugin-swap mode requires: " + ", ".join(missing))


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
    return run_live_plugin_swap_mode(reaper, args)


if __name__ == "__main__":
    sys.exit(main())
