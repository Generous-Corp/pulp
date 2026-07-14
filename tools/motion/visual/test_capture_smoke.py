#!/usr/bin/env python3
"""Smoke test for ``capture_sim_frames.py``.

The script depends on either ``/usr/sbin/screencapture`` (macOS) or
``xcrun simctl io booted screenshot`` (a booted iOS Simulator). On
hosts where neither tool is usable we exit 3 (ctest SKIP) — that's the
documented contract for this smoke.

When `screencapture` is available, the test invokes the CLI against
``--source macos`` with a 1×1 region so the actual grab is cheap, but
asks for only a single frame and a low gate threshold so the loop
finishes in well under a second.

In addition (always-run) we exercise the motion-gate algebra by
calling `_mean_diff` directly with synthetic arrays — that gives the
test real coverage of the gating math even on hosts without a
capture source.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR.parent.parent.parent) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR.parent.parent.parent))

from tools.motion.visual import capture_sim_frames  # noqa: E402


def _have_screencapture() -> bool:
    return shutil.which("screencapture") is not None


def _have_simulator() -> bool:
    if shutil.which("xcrun") is None:
        return False
    proc = subprocess.run(
        ["xcrun", "simctl", "list", "devices", "booted"],
        check=False, capture_output=True, text=True,
    )
    return proc.returncode == 0 and "Booted" in (proc.stdout or "")


def _check_gate_math() -> int:
    """Exercise `_mean_diff` directly so the gating logic always runs
    even on hosts where neither capture source is usable."""
    try:
        import numpy as np
        from PIL import Image  # noqa: F401
    except Exception as e:
        print(f"capture-smoke: missing dep ({e})", file=sys.stderr)
        return 3
    a = np.zeros((40, 40, 3), dtype="uint8")
    b = a.copy()
    if capture_sim_frames._mean_diff(a, b, np) != 0.0:
        print("capture-smoke: identical arrays should diff to 0", file=sys.stderr)
        return 1
    b[:, :, 0] = 200
    diff = capture_sim_frames._mean_diff(a, b, np)
    expected = 200.0 / 3.0  # one channel of three.
    if abs(diff - expected) > 0.5:
        print(f"capture-smoke: diff {diff} != expected {expected}", file=sys.stderr)
        return 1
    # Different shapes should not crash; the helper crops to the
    # overlapping rectangle.
    c = np.zeros((20, 30, 3), dtype="uint8")
    if capture_sim_frames._mean_diff(a, c, np) != 0.0:
        print("capture-smoke: mismatched shapes diff should be 0", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    rc = _check_gate_math()
    if rc != 0:
        return rc

    have_macos = _have_screencapture()
    have_sim = _have_simulator()
    if not (have_macos or have_sim):
        print("capture-smoke: no capture source available (skip)", file=sys.stderr)
        return 3

    # Exercise the CLI end-to-end against a 4×4 static screen region.
    # We expect the motion gate to *never* open (the region is
    # quiescent) so the script returns 3 with a clear stderr message.
    # That's the load-bearing behavior — gating actually gates — and
    # it runs cheaply without requiring foreground motion.
    with tempfile.TemporaryDirectory() as td:
        out_dir = Path(td) / "out"
        if have_macos:
            cli = [
                sys.executable,
                str(SCRIPT_DIR / "capture_sim_frames.py"),
                "--source", "macos",
                "--output-dir", str(out_dir),
                "--fps", "20",
                "--frame-count", "1",
                "--gate-threshold", "200.0",  # impossibly high
                "--gate-consecutive", "1",
                "--idle-timeout", "1",
                "--bounds", "0,0,4,4",
            ]
        else:
            cli = [
                sys.executable,
                str(SCRIPT_DIR / "capture_sim_frames.py"),
                "--source", "simulator",
                "--output-dir", str(out_dir),
                "--fps", "20",
                "--frame-count", "1",
                "--gate-threshold", "200.0",
                "--gate-consecutive", "1",
                "--idle-timeout", "1",
            ]
        # Cap wall time at 30s — three frames at 20fps is well under
        # a second, plus the idle-timeout window above.
        proc = subprocess.run(
            cli, check=False, capture_output=True, text=True, timeout=30,
        )
        if proc.returncode != 3:
            sys.stderr.write(proc.stderr or "")
            print(
                f"capture-smoke: expected gate-never-opened exit 3, "
                f"got {proc.returncode}",
                file=sys.stderr,
            )
            return 1
    print("capture-smoke OK (gate behavior verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
