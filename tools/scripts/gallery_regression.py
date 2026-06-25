#!/usr/bin/env python3
"""gallery_regression.py — end-to-end visual-regression driver for the widget
gallery.

Renders the gallery with pulp-widget-gallery, then diffs each theme render
against its committed golden via gallery_visual_diff.changed_fraction. Wired as
the `gallery-visual-regression` ctest.

Skips (exit 77) — not fails — when the prerequisites for an honest visual diff
are absent, so non-Apple / no-Pillow lanes don't false-fail:
  * the pulp-widget-gallery binary wasn't built, or
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary", required=True, help="path to pulp-widget-gallery")
    ap.add_argument("--golden-dir", required=True, help="dir with widget-gallery-{light,dark}.png")
    ap.add_argument("--channel-tol", type=int, default=8)
    ap.add_argument("--fail-fraction", type=float, default=0.02)
    args = ap.parse_args()

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    if not os.path.exists(args.binary):
        print(f"SKIP: gallery binary not built ({args.binary})")
        return SKIP
    try:
        from gallery_visual_diff import _load_rgba, changed_fraction
        _load_rgba  # noqa: ensures PIL import path is reachable
    except SystemExit:
        print("SKIP: Pillow (PIL) not available for visual diff")
        return SKIP

    tmp = tempfile.mkdtemp(prefix="gallery-regress-")
    out = os.path.join(tmp, "g")
    proc = subprocess.run([args.binary, "--out", out, "--theme", "both"],
                          capture_output=True, text=True)
    worst = 0.0
    for theme in ("light", "dark"):
        render = f"{out}-{theme}.png"
        golden = os.path.join(args.golden_dir, f"widget-gallery-{theme}.png")
        if not os.path.exists(render):
            print(f"SKIP: no {theme} render produced (no screenshot backend?). "
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
            print(f"FAIL: {theme} size {rsize} != golden {gsize}", file=sys.stderr)
            return 1
        frac = changed_fraction(rpx, gpx, args.channel_tol)
        worst = max(worst, frac)
        if frac > args.fail_fraction:
            print(f"FAIL: {theme} gallery drifted {frac * 100:.2f}% "
                  f"(> {args.fail_fraction * 100:.1f}%). Re-render the golden in "
                  f"assets/design-system/ink-signal/reference/gallery/ if intentional.",
                  file=sys.stderr)
            return 1
    print(f"gallery-visual-regression: ok — worst {worst * 100:.2f}% "
          f"(<= {args.fail_fraction * 100:.1f}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
