#!/usr/bin/env python3
"""Region-aware fidelity check for the HTML->Figma import pipeline.

A single whole-board mean diff hides small defects: a 56px knob ring is ~0.15%
of a 1400px board, so dropping it barely moves the global average (this is
exactly how the pan-knob ring slipped through at 1.03/255). This tiles both
images into a grid and reports the WORST tiles, so localized drift surfaces
even when the global average looks clean.

Usage:
  verify_region.py <source.png> <figma.png> [tile=80] [thresh=8.0]
Exit code 1 if any tile exceeds the threshold.
"""
import sys
from PIL import Image
import numpy as np

def main():
    src_p, fig_p = sys.argv[1], sys.argv[2]
    tile = int(sys.argv[3]) if len(sys.argv) > 3 else 80
    thresh = float(sys.argv[4]) if len(sys.argv) > 4 else 8.0
    fig = Image.open(fig_p).convert("RGB")
    src = Image.open(src_p).convert("RGB").resize(fig.size, Image.LANCZOS)
    a = np.asarray(src).astype(int); b = np.asarray(fig).astype(int)
    H, W = a.shape[:2]
    diff = np.abs(a - b).mean(2)
    print(f"whole-board mean-abs-diff: {diff.mean():.2f}/255")
    rows = []
    for ty in range(0, H, tile):
        for tx in range(0, W, tile):
            t = diff[ty:ty+tile, tx:tx+tile]
            rows.append((t.mean(), tx, ty))
    rows.sort(reverse=True)
    worst = rows[:12]
    flagged = [r for r in rows if r[0] > thresh]
    print(f"tiles={len(rows)} tile={tile}px thresh={thresh}  flagged={len(flagged)}")
    print("worst tiles (mean-abs-diff @ x,y):")
    for m, x, y in worst:
        mark = "  <-- FLAG" if m > thresh else ""
        print(f"  {m:6.2f}  @ ({x:4},{y:4}){mark}")
    sys.exit(1 if flagged else 0)

if __name__ == "__main__":
    main()
