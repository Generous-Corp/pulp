#!/usr/bin/env python3
"""Real-DAW (REAPER) functional smoke for Pulp reload / editor / format-adapter work.

WHY THIS EXISTS
    auval / pluginval / clap-validator prove a plugin SCANS + LOADS. They do NOT
    prove a FUNCTIONAL behavior happens inside a real host. Some bugs only appear
    in a host because the format adapter injects state that no headless / standalone
    path has — e.g. a SYNTHESIZED bypass parameter that made the reload param-
    contract gate reject every in-DAW reload (fixed 2026-07-04, commit 4a6e048f4).
    This harness loads a Pulp plugin in REAPER, drives a hot-swap, and asserts the
    reload was ACCEPTED + APPLIED by scraping the plugin's reload log.

WHEN TO USE IT  (see docs/guides/daw-smoke.md — the living rules)
    Opt-in, default OFF. Not every build. Use it for initial functional validation
    of reload/editor behavior (before asking a human to test) and for risky
    reload/editor/format-adapter changes. It COMPLEMENTS the validators; it does not
    replace them.

DESIGN NOTES (adversarial)
    * Zero pollution: runs REAPER from a FRESH temp portable resource dir and scans
      the plugin from a TEMP path — never writes the user's REAPER config or plugin
      folders (VST3/CLAP). AU cannot be scanned from a custom path, so --format au
      installs to ~/Library/Audio/Plug-Ins/Components and ALWAYS uninstalls on exit.
    * Verification is headless-safe: `screencapture` returns BLACK frames without a
      Screen-Recording TCC grant, so we scrape the plugin reload log (captured from
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


def main() -> int:
    ap = argparse.ArgumentParser(description="REAPER functional smoke for Pulp reload/editor changes.")
    ap.add_argument("--plugin-name", required=True, help='FX name as REAPER lists it, e.g. "Pulp Hot-Reload Morph".')
    ap.add_argument("--format", choices=["vst3", "clap", "au"], default="vst3",
                    help="vst3/clap scan from a temp path (no pollution); au installs+uninstalls (last resort).")
    ap.add_argument("--plugin-path", required=True, help="Path to the built .vst3/.clap/.component bundle.")
    ap.add_argument("--watched-logic", required=True,
                    help="Path the plugin watches, e.g. ~/.pulp/hot-reload-morph/logic.dylib.")
    ap.add_argument("--initial-logic", required=True, help="Logic variant to seed before launch (A).")
    ap.add_argument("--swap-logic", required=True, help="Logic variant to hot-swap to (B); must differ from A.")
    ap.add_argument("--timeout", type=int, default=90, help="Per-phase wall-clock cap (s).")
    ap.add_argument("--check-config", action="store_true",
                    help="Honor ~/.config/pulp/daw-smoke.toml: SKIP unless enabled.")
    args = ap.parse_args()

    if args.check_config:
        cfg = load_config()
        if not cfg["enabled"]:
            log("daw-smoke.enabled is false (opt-in) — SKIP. Enable in ~/.config/pulp/daw-smoke.toml.")
            return EXIT_SKIP

    reaper = find_reaper()
    if reaper is None:
        log("REAPER not installed — SKIP (this is not a PASS). See docs/guides/daw-smoke.md.")
        return EXIT_SKIP

    plugin = Path(os.path.expanduser(args.plugin_path))
    watched = Path(os.path.expanduser(args.watched_logic))
    initial = Path(os.path.expanduser(args.initial_logic))
    swap = Path(os.path.expanduser(args.swap_logic))
    for p in (plugin, initial, swap):
        if not p.exists():
            log(f"missing input: {p} — FAIL")
            return EXIT_FAIL
    if not LUA.exists():
        log(f"missing REAPER script: {LUA} — FAIL")
        return EXIT_FAIL

    portable = Path(tempfile.mkdtemp(prefix="pulp-daw-smoke-reaper-"))
    scan_dir = Path(tempfile.mkdtemp(prefix="pulp-daw-smoke-plugins-"))
    au_installed: Path | None = None
    proc: subprocess.Popen | None = None
    reaper_out = portable / "reaper.out.txt"

    try:
        # ---- REAPER portable config: skip splash, point VST/CLAP scan at scan_dir ----
        (portable / "reaper.ini").write_text(
            "[REAPER]\nsplashscreen=0\nvstpath=" + str(scan_dir) + "\nclap_path=" + str(scan_dir) + "\n"
        )
        # Place the plugin where REAPER will find it WITHOUT touching real folders.
        if args.format in ("vst3", "clap"):
            shutil.copytree(plugin, scan_dir / plugin.name)
        else:  # au — no custom-path scan; install to the real folder, uninstall on exit.
            comp = Path(os.path.expanduser("~/Library/Audio/Plug-Ins/Components")) / plugin.name
            comp.parent.mkdir(parents=True, exist_ok=True)
            if comp.exists():
                log(f"refusing to clobber existing {comp} — FAIL"); return EXIT_FAIL
            shutil.copytree(plugin, comp)
            au_installed = comp
            subprocess.run(["killall", "-9", "AudioComponentRegistrar"], capture_output=True)

        # ---- Seed variant A, write the status file target for the Lua ----
        watched.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(initial, watched); os.utime(watched, None)
        status = Path("/tmp/pulp_daw_smoke_status.txt")
        if status.exists():
            status.unlink()

        env = dict(os.environ)
        env["PULP_DAW_SMOKE_FX"] = args.plugin_name
        env["PULP_DAW_SMOKE_STATUS"] = str(status)

        # ---- Pre-warm scan (a launch we kill), so the scripted run finds the FX ----
        kill_reaper()
        with open(reaper_out, "w") as out:
            warm = subprocess.Popen([str(reaper), "-cfgfile", str(portable / "reaper.ini")],
                                    stdout=out, stderr=subprocess.STDOUT, env=env)
        time.sleep(min(20, args.timeout))
        warm.terminate(); time.sleep(2); warm.kill(); kill_reaper(); time.sleep(2)

        # ---- Scripted run: insert + float the FX ----
        with open(reaper_out, "a") as out:
            proc = subprocess.Popen([str(reaper), "-cfgfile", str(portable / "reaper.ini"), str(LUA)],
                                    stdout=out, stderr=subprocess.STDOUT, env=env)
        shown = False
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(2)
            if status.exists():
                s = status.read_text().strip()
                if s.startswith("FX_SHOWN"):
                    shown = True; break
                if s.startswith("FX_NOT_FOUND"):
                    log("REAPER could not find/insert the FX (scan issue) — INCONCLUSIVE.")
                    return EXIT_INCONCLUSIVE
        if not shown:
            log("REAPER did not open the FX within timeout (flaky launch) — INCONCLUSIVE.")
            return EXIT_INCONCLUSIVE

        time.sleep(4)  # editor settles + initial logic loads
        # ---- Drive the hot-swap: copy variant B over the watched path ----
        log(f"hot-swapping {watched.name} -> {swap.name}")
        shutil.copyfile(swap, watched); os.utime(watched, None)
        time.sleep(6)  # watcher fires + reload gate runs + editor pump rebuilds

        proc.terminate(); time.sleep(2); proc.kill(); kill_reaper()

        # ---- Verify from the captured plugin reload log ----
        text = reaper_out.read_text(errors="replace") if reaper_out.exists() else ""
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
        # Cleanup ALWAYS: kill REAPER, remove temp dirs, uninstall any AU we placed.
        try:
            if proc is not None:
                proc.terminate(); time.sleep(1); proc.kill()
        except Exception:  # noqa: BLE001
            pass
        kill_reaper()
        if au_installed and au_installed.exists():
            shutil.rmtree(au_installed, ignore_errors=True)
            subprocess.run(["killall", "-9", "AudioComponentRegistrar"], capture_output=True)
            log(f"uninstalled AU {au_installed}")
        shutil.rmtree(portable, ignore_errors=True)
        shutil.rmtree(scan_dir, ignore_errors=True)
        log("cleanup done (no dev plugins left behind).")


if __name__ == "__main__":
    sys.exit(main())
