#!/usr/bin/env python3
"""Golden re-import regression check for the design-import pipeline.

Re-imports a design FROM SCRATCH and compares its render against a committed
baseline, so a code change can't silently regress an existing design ("two
steps forward, three steps back"). The comparison is deliberately TOLERANT +
STRUCTURAL, not exact-pixel — a naive equality check false-positives on (a) GPU
antialiasing/dither noise and (b) legitimate sub-pixel sizing improvements (we
measured max 15/255, localized to soft knob shadows, when the import was in fact
unchanged). So pass/fail keys on:

  * fraction of pixels differing by MORE than `--per-pixel-tol` (default 24/255),
  * a coarse edge/structure agreement score (Sobel on luminance), which is
    robust to color/theme and to soft-shadow noise.

It is source-agnostic: it drives the same `pulp-import-design` + `pulp-screenshot`
path for any `--from` (figma, figma-plugin, pencil, stitch, v0, claude). Proprietary
baselines stay local (per the harness no-proprietary-in-CI rule); CI uses
sanitized/synthetic baselines.

Usage:
  golden_regression.py --from figma-plugin --file scene.pulp.json \
      --baseline golden/elysium-sprite.png --knob-style sprite \
      --import-bin build/tools/import-design/pulp-import-design \
      --shot-bin   build/tools/screenshot/pulp-screenshot \
      [--width 1000 --height 600 --scale 2 --theme dark] \
      [--per-pixel-tol 24 --max-diff-frac 0.01 --min-edge-agree 0.85] \
      [--out-compare out/compare.png]
Exit 0 = within tolerance (no regression); 3 = regression; 2 = setup error.
"""
from __future__ import annotations
import argparse, subprocess, sys, tempfile, os

def _np():
    import numpy as np  # local import so --help works without numpy
    return np

def render(args, out_png) -> bool:
    js = out_png + ".js"
    imp = [args.import_bin, "--from", args.source, "--file", args.file, "--output", js]
    if args.knob_style: imp += ["--knob-style", args.knob_style]
    if subprocess.run(imp, capture_output=True).returncode != 0:
        print("golden: import failed", file=sys.stderr); return False
    shot = [args.shot_bin, "--script", js, "--output", out_png,
            "--width", str(args.width), "--height", str(args.height),
            "--scale", str(args.scale), "--theme", args.theme]
    return subprocess.run(shot, capture_output=True).returncode == 0

def edges(gray):
    np = _np()
    # Cast to signed before differencing: on uint8 luminance np.diff wraps
    # negative deltas (a 255->0 dark-on-light edge becomes +1, below threshold),
    # so the strongest edges in dark-text/light-bg designs would vanish from the
    # structural map. int16 keeps the true signed magnitude.
    g = gray.astype(np.int16)
    gx = np.abs(np.diff(g, axis=1, prepend=g[:, :1]))
    gy = np.abs(np.diff(g, axis=0, prepend=g[:1, :]))
    return ((gx + gy) > 32)  # binary edge map, threshold robust to soft noise

def _dilate(mask, r=2):
    """Grow a binary mask by r pixels (4-neighbor), so structural agreement
    tolerates sub-pixel edge shifts from GPU AA / legit rounding — without
    which a 1px-shifted thin edge wrongly reads as a structural mismatch."""
    np = _np(); out = mask.copy()
    for _ in range(r):
        g = out.copy()
        g[1:, :] |= out[:-1, :]; g[:-1, :] |= out[1:, :]
        g[:, 1:] |= out[:, :-1]; g[:, :-1] |= out[:, 1:]
        out = g
    return out

def compare(baseline_png, current_png, args) -> int:
    from PIL import Image, ImageChops, ImageOps
    np = _np()
    base = Image.open(baseline_png).convert("RGB")
    cur  = Image.open(current_png).convert("RGB")
    if base.size != cur.size: cur = cur.resize(base.size)
    diff = np.asarray(ImageChops.difference(base, cur))
    over = (diff.max(2) > args.per_pixel_tol)
    frac = float(over.sum()) / (base.size[0] * base.size[1])

    bg = np.asarray(base.convert("L")); cg = np.asarray(cur.convert("L"))
    be, ce = edges(bg), edges(cg)
    # Shift-tolerant structural agreement: an edge counts as matched if there is
    # an edge within 2px in the other image. Symmetric, so neither side can hide
    # a missing/extra structure. Robust to GPU noise + sub-pixel rounding.
    be_d, ce_d = _dilate(be), _dilate(ce)
    agree_b = 1.0 if be.sum() == 0 else float((be & ce_d).sum()) / float(be.sum())
    agree_c = 1.0 if ce.sum() == 0 else float((ce & be_d).sum()) / float(ce.sum())
    edge_agree = min(agree_b, agree_c)

    ok = (frac <= args.max_diff_frac) and (edge_agree >= args.min_edge_agree)
    print(f"golden: pixels>{args.per_pixel_tol}: {frac*100:.3f}% "
          f"(max {args.max_diff_frac*100:.2f}%) | edge-agree: {edge_agree:.4f} "
          f"(min {args.min_edge_agree:.3f}) | {'PASS' if ok else 'REGRESSION'}")

    if args.out_compare:
        _write_compare(base, cur, ImageChops.difference(base, cur),
                       frac, edge_agree, args.out_compare)
    return 0 if ok else 3

def _write_compare(base, cur, diff, frac, edge_agree, path):
    from PIL import Image, ImageDraw, ImageFont, ImageOps
    g = ImageOps.autocontrast(diff.convert("L"))
    heat = Image.merge("RGB", (g, Image.eval(g, lambda v: v // 3), Image.eval(g, lambda v: 0)))
    W = 760
    fit = lambda im: im.resize((W, int(im.height * W / im.width)))
    panels = [(fit(base), "RENDERED (Pulp)  ·  baseline (golden)"),
              (fit(cur),  "RENDERED (Pulp)  ·  current code — re-imported from scratch"),
              (fit(heat), f"DIFF (contrast-normalized)  ·  {frac*100:.3f}% over tol, edge-agree {edge_agree:.3f}")]
    labH, pad = 30, 16; ph = panels[0][0].height
    canvas = Image.new("RGB", (W + pad*2, (ph+labH)*3 + pad*4), (22, 22, 26))
    d = ImageDraw.Draw(canvas)
    try: f = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 18)
    except Exception: f = ImageFont.load_default()
    y = pad
    for im, name in panels:
        d.text((pad, y), name, fill=(238, 238, 242), font=f)
        canvas.paste(im, (pad, y + labH)); y += ph + labH + pad
    canvas.save(path)
    print(f"golden: wrote comparison {path}")

def _selftest() -> int:
    """Verify edges() captures strong dark-on-light transitions (the uint8
    wraparound regression). Skips cleanly if numpy is unavailable."""
    try:
        np = _np()
    except Exception as e:  # numpy not installed in this environment
        print(f"golden: selftest skipped (numpy unavailable: {e})")
        return 77  # ctest SKIP_RETURN_CODE
    # A 255 -> 0 column transition is the strongest possible dark-on-light edge.
    # Under the old uint8 np.diff it wrapped to +1 and fell below the threshold,
    # so the edge vanished; with the int16 cast it must be detected.
    row = np.array([[255, 255, 0, 0]], dtype=np.uint8)
    e = edges(row)
    assert bool(e[0, 2]), "edges() missed a 255->0 dark-on-light transition"
    # An ascending 0 -> 255 edge must likewise be detected (symmetry).
    row2 = np.array([[0, 0, 255, 255]], dtype=np.uint8)
    assert bool(edges(row2)[0, 2]), "edges() missed a 0->255 transition"
    print("golden: selftest passed (edges() handles signed luminance deltas)")
    return 0

def main() -> int:
    if "--selftest" in sys.argv:
        return _selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="source", required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--knob-style", default="")
    ap.add_argument("--import-bin", required=True)
    ap.add_argument("--shot-bin", required=True)
    ap.add_argument("--width", type=int, default=1000)
    ap.add_argument("--height", type=int, default=600)
    ap.add_argument("--scale", type=float, default=2.0)
    ap.add_argument("--theme", default="dark")
    ap.add_argument("--per-pixel-tol", type=int, default=24)
    ap.add_argument("--max-diff-frac", type=float, default=0.01)
    ap.add_argument("--min-edge-agree", type=float, default=0.85)
    ap.add_argument("--out-compare", default="")
    args = ap.parse_args()
    if not os.path.exists(args.baseline):
        print(f"golden: baseline missing: {args.baseline}", file=sys.stderr); return 2
    with tempfile.TemporaryDirectory() as td:
        cur = os.path.join(td, "current.png")
        if not render(args, cur): return 2
        return compare(args.baseline, cur, args)

if __name__ == "__main__":
    raise SystemExit(main())
