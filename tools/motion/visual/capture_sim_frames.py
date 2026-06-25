#!/usr/bin/env python3
"""Motion-gated frame capture for the visual-analysis pipeline.

Captures a sequence of PNG frames from a host source (macOS window
region or a booted iOS Simulator) but **only starts saving once
motion is detected** — i.e. the mean per-pixel diff between two
consecutive grabs exceeds ``--gate-threshold`` for
``--gate-consecutive`` consecutive samples.

The output directory matches the on-disk shape expected by
``analyze_sequence.py``: ``<output_dir>/frame_NNNN.png``, zero-padded
to four digits, contiguous indices starting at 0.

Sources:
    macos       — uses ``screencapture -R X,Y,W,H`` (bounds required).
    simulator   — uses ``xcrun simctl io booted screenshot``.

Exit codes:
    0  Saved at least one frame.
    2  Bad arguments (e.g. missing --bounds on macOS).
    3  Required external tool not installed / simulator not booted.
       Treated as ctest SKIP_RETURN_CODE for the smoke test so CI
       hosts without screencapture / Xcode don't false-fail.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import List, Optional


# Re-use the analyzer's optional-deps loader so error messages stay
# consistent across the visual pipeline.
SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR.parent.parent.parent) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR.parent.parent.parent))


def _load_deps():
    try:
        import numpy as np
        from PIL import Image
        return np, Image
    except Exception as e:
        print(
            f"capture_sim_frames: missing dependency ({e}); "
            "install with `pip install -r tools/motion/visual/requirements.txt`",
            file=sys.stderr,
        )
        return None


# ── Source backends ──────────────────────────────────────────────────

def _which_or_none(name: str) -> Optional[str]:
    return shutil.which(name)


def _capture_macos(dest: Path, bounds: str) -> bool:
    """Run ``screencapture -x -R X,Y,W,H dest``. Returns True on success."""
    bin_path = _which_or_none("screencapture")
    if not bin_path:
        return False
    proc = subprocess.run(
        [bin_path, "-x", "-R", bounds, str(dest)],
        check=False, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    return proc.returncode == 0 and dest.exists() and dest.stat().st_size > 0


def _capture_simulator(dest: Path) -> bool:
    """Run ``xcrun simctl io booted screenshot dest``. Returns True on success."""
    bin_path = _which_or_none("xcrun")
    if not bin_path:
        return False
    proc = subprocess.run(
        [bin_path, "simctl", "io", "booted", "screenshot", str(dest)],
        check=False, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    return proc.returncode == 0 and dest.exists() and dest.stat().st_size > 0


def _source_available(source: str, bounds: Optional[str]) -> bool:
    if source == "macos":
        return _which_or_none("screencapture") is not None and bool(bounds)
    if source == "simulator":
        return _which_or_none("xcrun") is not None
    return False


# ── Motion gate ──────────────────────────────────────────────────────

def _mean_diff(prev_arr, next_arr, np_mod) -> float:
    """Return the mean per-channel abs diff in [0..255]."""
    if prev_arr.shape != next_arr.shape:
        # Resize next to prev's footprint via numpy slicing — keeps the
        # gate cheap and avoids importing skimage for a smoke utility.
        h = min(prev_arr.shape[0], next_arr.shape[0])
        w = min(prev_arr.shape[1], next_arr.shape[1])
        prev_arr = prev_arr[:h, :w]
        next_arr = next_arr[:h, :w]
    return float(
        np_mod.abs(prev_arr.astype("float32") - next_arr.astype("float32")).mean()
    )


def _load_array(path: Path, np_mod, Image):
    img = Image.open(str(path)).convert("RGB")
    return np_mod.asarray(img)


# ── Main loop ────────────────────────────────────────────────────────

def capture(
    source: str,
    output_dir: Path,
    *,
    fps: float = 10.0,
    frame_count: int = 60,
    gate_threshold: float = 4.0,
    gate_consecutive: int = 1,
    idle_timeout_s: float = 10.0,
    bounds: Optional[str] = None,
) -> int:
    deps = _load_deps()
    if deps is None:
        return 3
    np_mod, Image = deps
    if not _source_available(source, bounds):
        print(
            f"capture_sim_frames: source `{source}` unavailable "
            "(missing tool, unbooted simulator, or no --bounds)",
            file=sys.stderr,
        )
        return 3

    output_dir.mkdir(parents=True, exist_ok=True)
    interval = 1.0 / max(0.1, fps)
    saved = 0
    consec_motion = 0
    last_saved_arr = None
    started = False
    started_t = time.monotonic()
    last_motion_t = started_t
    # Pre-gate timeout — bail out if the motion gate never opens
    # within `idle_timeout_s`. Without this the loop would spin
    # forever on a quiescent source.
    pre_gate_deadline = started_t + idle_timeout_s

    with tempfile.TemporaryDirectory(prefix="pulp-motion-grab-") as scratch:
        scratch_path = Path(scratch)
        grab_idx = 0
        prev_arr = None
        while saved < frame_count:
            grab_path = scratch_path / f"grab_{grab_idx:06d}.png"
            grab_idx += 1
            ok = (
                _capture_macos(grab_path, bounds or "")
                if source == "macos"
                else _capture_simulator(grab_path)
            )
            if not ok:
                # Exit code semantics (#2168, #2152):
                #   0 — full requested capture completed
                #   3 — nothing was saved (e.g. source misconfigured at start)
                #   4 — partial capture (grabbed some frames then aborted)
                # The partial case used to silently exit 0, hiding a
                # truncated run from downstream automation. Distinguish it.
                if saved == 0:
                    print(
                        f"capture_sim_frames: grab failed for source `{source}` "
                        f"before any frames captured",
                        file=sys.stderr,
                    )
                    return 3
                print(
                    f"capture_sim_frames: grab failed for source `{source}` "
                    f"mid-run — captured {saved}/{frame_count} frames",
                    file=sys.stderr,
                )
                return 4
            arr = _load_array(grab_path, np_mod, Image)
            if prev_arr is not None:
                diff = _mean_diff(prev_arr, arr, np_mod)
            else:
                diff = 0.0

            if not started:
                if diff > gate_threshold:
                    consec_motion += 1
                else:
                    consec_motion = 0
                if consec_motion >= gate_consecutive:
                    started = True
                    last_motion_t = time.monotonic()
            if started:
                # Skip frames that exactly match the previous saved one
                # (cuts duplicates while the host is still drawing).
                if (
                    last_saved_arr is not None
                    and _mean_diff(last_saved_arr, arr, np_mod) < 1e-3
                ):
                    pass
                else:
                    out = output_dir / f"frame_{saved:04d}.png"
                    Image.fromarray(arr).save(str(out), format="PNG")
                    saved += 1
                    last_saved_arr = arr
                    last_motion_t = time.monotonic()
            prev_arr = arr

            # Idle window — if we've started but nothing new has
            # arrived for `idle_timeout_s`, stop early. Also bail if
            # the gate never opens within the pre-gate deadline so
            # quiescent sources don't spin the loop forever.
            now = time.monotonic()
            if started and (now - last_motion_t) > idle_timeout_s:
                break
            if not started and now > pre_gate_deadline:
                break
            time.sleep(interval)

    if saved == 0:
        print(
            "capture_sim_frames: motion gate never opened "
            f"(threshold={gate_threshold}, consecutive={gate_consecutive})",
            file=sys.stderr,
        )
        return 3
    print(f"capture_sim_frames: saved {saved} frames → {output_dir}")
    return 0


# ── CLI ──────────────────────────────────────────────────────────────

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pulp-motion-capture",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--source", required=True,
                        choices=("macos", "simulator"))
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--frame-count", type=int, default=60)
    parser.add_argument("--gate-threshold", type=float, default=4.0,
                        help="Mean-pixel diff (0..255) to open the gate")
    parser.add_argument("--gate-consecutive", type=int, default=1,
                        help="Consecutive samples above threshold")
    parser.add_argument("--idle-timeout", type=float, default=10.0,
                        dest="idle_timeout_s",
                        help="Seconds without saved motion → stop")
    parser.add_argument("--bounds", default=None,
                        help="macOS region as X,Y,W,H (required for --source macos)")
    args = parser.parse_args(argv)

    if args.source == "macos" and not args.bounds:
        print("capture_sim_frames: --bounds X,Y,W,H is required with --source macos",
              file=sys.stderr)
        return 2
    return capture(
        source=args.source,
        output_dir=args.output_dir,
        fps=args.fps,
        frame_count=args.frame_count,
        gate_threshold=args.gate_threshold,
        gate_consecutive=args.gate_consecutive,
        idle_timeout_s=args.idle_timeout_s,
        bounds=args.bounds,
    )


if __name__ == "__main__":
    sys.exit(main())
