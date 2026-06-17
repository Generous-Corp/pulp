#!/usr/bin/env python3
"""component_regression.py — per-primitive visual-regression gate for the Ink &
Signal design system. Renders every component cell with pulp-component-shots,
then diffs each light/dark render against its committed golden via
gallery_visual_diff.changed_fraction. Wired as the `component-visual-regression`
ctest — the enforcement layer that catches widget-fidelity drift on any change.

Skips (exit 77) — not fails — when an honest visual diff isn't possible:
  * the pulp-component-shots binary wasn't built, or
  * Pillow (PIL) isn't importable, or
  * rendering produced no PNG (no screenshot backend on this platform).
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile

SKIP = 77

# Must match the component set rendered by examples/component-shots/main.cpp.
COMPONENTS = [
    "Knob", "Fader", "Slider", "Toggle", "Checkbox", "Button", "Badge",
    "Meter", "ProgressBar", "ComboBox", "Stepper", "Pan", "Tab", "Spectrum",
    "Waveform", "XYPad", "InlineBanner", "Toast", "EmptyState", "Input",
]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, help="path to pulp-component-shots")
    ap.add_argument("--golden-dir", required=True, help="dir with <Name>-{light,dark}.png")
    ap.add_argument("--channel-tol", type=int, default=8)
    ap.add_argument("--fail-fraction", type=float, default=0.02)
    args = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if not os.path.exists(args.binary):
        print(f"SKIP: component-shots binary not built ({args.binary})")
        return SKIP
    try:
        from gallery_visual_diff import _load_rgba, changed_fraction
        _load_rgba  # noqa: ensures the PIL import path is reachable
    except SystemExit:
        print("SKIP: Pillow (PIL) not available for visual diff")
        return SKIP

    tmp = tempfile.mkdtemp(prefix="component-regress-")
    proc = subprocess.run([args.binary, "--out", tmp], capture_output=True, text=True)

    worst = 0.0
    worst_name = ""
    for name in COMPONENTS:
        for theme in ("light", "dark"):
            render = os.path.join(tmp, f"{name}-{theme}.png")
            golden = os.path.join(args.golden_dir, f"{name}-{theme}.png")
            if not os.path.exists(render):
                print(f"SKIP: no {name}-{theme} render (no screenshot backend?). "
                      f"binary stderr: {proc.stderr.strip()}")
                return SKIP
            if not os.path.exists(golden):
                print(f"FAIL: missing golden {golden}", file=sys.stderr)
                return 1
            try:
                rpx, rsize = _load_rgba(render)
                gpx, gsize = _load_rgba(golden)
            except SystemExit:
                print("SKIP: Pillow (PIL) not available for visual diff")
                return SKIP
            if rsize != gsize:
                print(f"FAIL: {name}-{theme} size {rsize} != golden {gsize}", file=sys.stderr)
                return 1
            frac = changed_fraction(rpx, gpx, args.channel_tol)
            if frac > worst:
                worst, worst_name = frac, f"{name}-{theme}"
            if frac > args.fail_fraction:
                print(f"FAIL: {name}-{theme} drifted {frac * 100:.2f}% "
                      f"(> {args.fail_fraction * 100:.1f}%). If intentional, re-bake the "
                      f"golden in assets/design-system/ink-signal/reference/components/.",
                      file=sys.stderr)
                return 1
    print(f"component-visual-regression: ok — worst {worst * 100:.2f}% "
          f"({worst_name}, <= {args.fail_fraction * 100:.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
