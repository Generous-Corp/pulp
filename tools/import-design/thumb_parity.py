#!/usr/bin/env python3
"""Rank where an import's PIXELS disagree with Figma's own raster of the design.

Why this exists
---------------
Every .fig is a ZIP carrying `thumbnail.png` — Figma's render of the design —
and a `meta.json` whose `render_coordinates` + `thumbnail_size` give an EXACT
canvas -> thumbnail transform. No image alignment, no Figma API, no rate limit.
That reference sat unused while bugs were found by a human squinting at
screenshots and reporting them one at a time: a wall of pink tabs, an inverted
toggle, a missing record dot. Each was obvious in the pixels and invisible to
every gate we had.

This is GROSS-COLOUR TRIAGE and nothing more: scale-match the two images and rank
the blocks that disagree. Advisory only — never a gate.

WHAT IT CANNOT SEE (read this before trusting a pass)
-----------------------------------------------------
It compares block MEANS, so it is blind BY CONSTRUCTION to any error that
preserves a region's mean:

  * a flattened gradient — it matches its own mean BY DEFINITION
  * thin-stroke opacity — 20%-vs-100% white strokes average to nearly the same
    grey once downscaled
  * a soft shadow on a dark panel — it vanishes into the mean

Those were three REAL bugs on 2026-07-16, and this tool reported "colour is
close" through every one of them. They were found instead by a human zooming
into Figma. At ~0.4x it also cannot resolve anything under ~3 design px, so a
2px arc or a glyph is beyond it — chasing its last few percent is reading noise.

This docstring used to end "it finds colour faults a geometry check cannot see by
construction", which is true and badly misleading: the sentence names what it
beats rather than what it misses, and I wrote it while believing a 400px
thumbnail was a sufficient reference. It is sufficient for gross colour, presence
and gross placement. It is USELESS for material — opacity, gradients, shadows,
glyph weight — which is exactly where "close but a bit off" lives.

Use `layout_parity.py` for geometry (it sees boxes, not the ink inside them) and
a material audit for property survival. A green from any single instrument is not
"the render is right"; two blind instruments both reporting green is how those
three bugs shipped. A human on a montage is the final say.

What it can and cannot say
--------------------------
The thumbnail is ~0.4x (400x268 for a 1004x672 design). It adjudicates colour,
presence and gross placement. It CANNOT adjudicate a 2px arc or a glyph, and
conclusions drawn from upscaling it are unsound — so we downscale OUR render to
the thumbnail's size rather than upscaling the thumbnail to ours. That also
equalizes anti-aliasing: two different rasterizers converge under a box
downscale, which is what makes a cross-renderer comparison meaningful at all.

Colour distance is CIEDE2000, so a threshold means something perceptual: ~1 is
a just-noticeable difference, ~3 is "a person would call that a different
colour". Naive RGB distance would rank a dark-on-dark shift far below a bright
one that nobody can see.
"""

from __future__ import annotations

import argparse
import json
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover - environment guard
    sys.exit("thumb_parity: needs Pillow (pip install pillow)")


# --- CIEDE2000 ---------------------------------------------------------------
# sRGB -> Lab -> dE2000. Written out rather than pulled from a colour library so
# this stays runnable anywhere the rest of the import tooling runs.

def _srgb_to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _rgb_to_lab(rgb: tuple[float, float, float]) -> tuple[float, float, float]:
    r, g, b = (_srgb_to_linear(v / 255.0) for v in rgb)
    # sRGB D65 -> XYZ
    x = r * 0.4124564 + g * 0.3575761 + b * 0.1804375
    y = r * 0.2126729 + g * 0.7151522 + b * 0.0721750
    z = r * 0.0193339 + g * 0.1191920 + b * 0.9503041
    # D65 white
    xn, yn, zn = 0.95047, 1.00000, 1.08883

    def f(t: float) -> float:
        return t ** (1 / 3) if t > 216 / 24389 else (841 / 108) * t + 4 / 29

    fx, fy, fz = f(x / xn), f(y / yn), f(z / zn)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def delta_e2000(lab1, lab2) -> float:
    import math

    l1, a1, b1 = lab1
    l2, a2, b2 = lab2
    avg_l = (l1 + l2) / 2
    c1 = math.hypot(a1, b1)
    c2 = math.hypot(a2, b2)
    avg_c = (c1 + c2) / 2
    g = 0.5 * (1 - math.sqrt(avg_c**7 / (avg_c**7 + 25**7))) if avg_c > 0 else 0
    a1p, a2p = a1 * (1 + g), a2 * (1 + g)
    c1p, c2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    avg_cp = (c1p + c2p) / 2
    h1p = math.degrees(math.atan2(b1, a1p)) % 360
    h2p = math.degrees(math.atan2(b2, a2p)) % 360
    dlp = l2 - l1
    dcp = c2p - c1p
    if c1p * c2p == 0:
        dhp = 0.0
    elif abs(h2p - h1p) <= 180:
        dhp = h2p - h1p
    else:
        dhp = h2p - h1p - 360 if h2p > h1p else h2p - h1p + 360
    dHp = 2 * math.sqrt(c1p * c2p) * math.sin(math.radians(dhp) / 2)
    if c1p * c2p == 0:
        avg_hp = h1p + h2p
    elif abs(h1p - h2p) > 180:
        avg_hp = (h1p + h2p + 360) / 2 if h1p + h2p < 360 else (h1p + h2p - 360) / 2
    else:
        avg_hp = (h1p + h2p) / 2
    t = (
        1
        - 0.17 * math.cos(math.radians(avg_hp - 30))
        + 0.24 * math.cos(math.radians(2 * avg_hp))
        + 0.32 * math.cos(math.radians(3 * avg_hp + 6))
        - 0.20 * math.cos(math.radians(4 * avg_hp - 63))
    )
    d_ro = 30 * math.exp(-(((avg_hp - 275) / 25) ** 2))
    rc = 2 * math.sqrt(avg_cp**7 / (avg_cp**7 + 25**7)) if avg_cp > 0 else 0
    sl = 1 + (0.015 * (avg_l - 50) ** 2) / math.sqrt(20 + (avg_l - 50) ** 2)
    sc = 1 + 0.045 * avg_cp
    sh = 1 + 0.015 * avg_cp * t
    rt = -math.sin(math.radians(2 * d_ro)) * rc
    return math.sqrt(
        (dlp / sl) ** 2 + (dcp / sc) ** 2 + (dHp / sh) ** 2 + rt * (dcp / sc) * (dHp / sh)
    )


# --- registration ------------------------------------------------------------

@dataclass
class Registration:
    """Canvas -> thumbnail mapping, read from meta.json rather than guessed."""

    thumb_w: int
    thumb_h: int
    canvas_w: float
    canvas_h: float

    @property
    def scale_x(self) -> float:
        return self.thumb_w / self.canvas_w

    @property
    def scale_y(self) -> float:
        return self.thumb_h / self.canvas_h


def read_fig(fig_path: Path) -> tuple[Image.Image, Registration]:
    with zipfile.ZipFile(fig_path) as z:
        names = set(z.namelist())
        if "thumbnail.png" not in names:
            raise SystemExit(f"thumb_parity: {fig_path} has no thumbnail.png")
        with z.open("thumbnail.png") as fh:
            thumb = Image.open(fh).convert("RGB")
        meta = json.loads(z.read("meta.json")) if "meta.json" in names else {}
    # render_coordinates / thumbnail_size live under client_meta, not at the top
    # level. Reading the top level silently yields the thumbnail's own size as
    # the "canvas", i.e. scale 1.0 — every reported coordinate is then wrong by
    # 2.5x while the tool looks like it worked. A registration that lies is worse
    # than no registration, so an absent render_coordinates is fatal, not a
    # fallback.
    cm = meta.get("client_meta") or meta
    rc = cm.get("render_coordinates") or {}
    ts = cm.get("thumbnail_size") or {}
    if not rc.get("width") or not rc.get("height"):
        raise SystemExit(
            "thumb_parity: meta.json has no client_meta.render_coordinates — cannot "
            "register the thumbnail against the canvas, and guessing would report "
            "confident nonsense."
        )
    canvas_w = float(rc["width"])
    canvas_h = float(rc["height"])
    reg = Registration(
        thumb_w=int(ts.get("width") or thumb.width),
        thumb_h=int(ts.get("height") or thumb.height),
        canvas_w=canvas_w,
        canvas_h=canvas_h,
    )
    return thumb, reg


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fig", required=True, type=Path, help="the source .fig (its thumbnail is the reference)")
    ap.add_argument("--render", required=True, type=Path, help="our render PNG")
    ap.add_argument("--block", type=int, default=8, help="block size in thumbnail px (default 8)")
    ap.add_argument("--threshold", type=float, default=3.0,
                    help="mean dE2000 a block must exceed to be reported (default 3 = visibly different)")
    ap.add_argument("--top", type=int, default=20, help="how many worst blocks to list")
    ap.add_argument("--out-dir", type=Path, help="write heatmap + side-by-side here")
    ap.add_argument("--json", type=Path, help="write findings as JSON")
    args = ap.parse_args()

    thumb, reg = read_fig(args.fig)
    render = Image.open(args.render).convert("RGB")

    # Downscale OURS to the reference's size — never upscale the reference. The
    # box filter is also what makes two different rasterizers comparable.
    ours = render.resize((thumb.width, thumb.height), Image.BOX)

    # Per-axis scale: each thumbnail axis rounds to an integer independently, so
    # one uniform factor is subtly wrong.
    # The scope banner prints on every run, because the docstring only reaches
    # someone who opens the file and the output reaches everyone. A tool whose
    # limits are documented where its readers are not is how this one reported
    # "colour is close" through a flat gradient, an opaque-instead-of-20% icon,
    # and a missing shadow — all on the same evening.
    print("thumb_parity: GROSS-COLOUR TRIAGE, advisory only — never a gate.")
    print("  Blind to anything preserving a region's mean: flattened gradients,")
    print("  thin-stroke opacity, shadows on dark panels. Cannot resolve <~3 design px.")
    print("  Geometry → layout_parity.py. Material survival → the material audit.")
    print(f"registration: canvas {reg.canvas_w:.0f}x{reg.canvas_h:.0f} -> thumb {thumb.width}x{thumb.height} "
          f"(scale x={reg.scale_x:.6f} y={reg.scale_y:.6f})")
    if render.width / render.height - reg.canvas_w / reg.canvas_h > 0.01:
        print("  WARNING: render aspect differs from the canvas — is --render-size wrong?")

    tp, op = thumb.load(), ours.load()
    lab_cache: dict[tuple[int, int, int], tuple[float, float, float]] = {}

    def lab(px):
        v = lab_cache.get(px)
        if v is None:
            v = _rgb_to_lab(px)
            lab_cache[px] = v
        return v

    findings = []
    b = args.block
    for by in range(0, thumb.height - 1, b):
        for bx in range(0, thumb.width - 1, b):
            n = 0
            sr = sg = sb = tr = tg = tb = 0
            for y in range(by, min(by + b, thumb.height)):
                for x in range(bx, min(bx + b, thumb.width)):
                    a, c = tp[x, y], op[x, y]
                    tr += a[0]; tg += a[1]; tb += a[2]
                    sr += c[0]; sg += c[1]; sb += c[2]
                    n += 1
            if not n:
                continue
            # dE between the two block MEANS — not the mean of per-pixel dE.
            # The latter measures edge alignment, not colour: two rasterizers put
            # a glyph edge a fraction of a pixel apart and every block holding
            # text scores enormous, which flagged 85% of a frame whose colours
            # were fine. Comparing means asks the question actually being asked —
            # "is this region the right colour" — and is blind to sub-pixel
            # placement by construction. Geometry is layout_parity.py's job.
            mean = delta_e2000(lab((tr // n, tg // n, tb // n)), lab((sr // n, sg // n, sb // n)))
            if mean <= args.threshold:
                continue
            findings.append({
                "design_x": round(bx / reg.scale_x), "design_y": round(by / reg.scale_y),
                "design_w": round(b / reg.scale_x), "design_h": round(b / reg.scale_y),
                "dE2000": round(mean, 2),
                "figma_rgb": f"#{tr//n:02x}{tg//n:02x}{tb//n:02x}",
                "ours_rgb": f"#{sr//n:02x}{sg//n:02x}{sb//n:02x}",
            })

    findings.sort(key=lambda f: -f["dE2000"])
    total_blocks = (thumb.width // b) * (thumb.height // b)
    print(f"\nblocks visibly different (dE2000 > {args.threshold}): {len(findings)} of ~{total_blocks}\n")
    print(f"  {'design x,y':>14}  {'dE':>6}  {'figma':>9}  {'ours':>9}")
    for f in findings[: args.top]:
        print(f"  {f['design_x']:>6},{f['design_y']:<7}  {f["dE2000"]:>6}  "
              f"{f['figma_rgb']:>9}  {f['ours_rgb']:>9}")

    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({
            "fig": str(args.fig), "render": str(args.render),
            "registration": {"canvas_w": reg.canvas_w, "canvas_h": reg.canvas_h,
                             "thumb_w": thumb.width, "thumb_h": thumb.height,
                             "scale_x": reg.scale_x, "scale_y": reg.scale_y},
            "threshold": args.threshold, "block": b,
            "findings": findings,
        }, indent=1))
        print(f"\nJSON -> {args.json}")

    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        # Heatmap: red where the two disagree, over our render, so a finding has
        # somewhere to point.
        heat = ours.copy()
        hp = heat.load()
        for f in findings:
            bx = int(f["design_x"] * reg.scale_x)
            by = int(f["design_y"] * reg.scale_y)
            for y in range(by, min(by + b, thumb.height)):
                for x in range(bx, min(bx + b, thumb.width)):
                    r, g, bl = hp[x, y]
                    hp[x, y] = (min(255, r + 110), g // 2, bl // 2)
        scale = 3
        heat.resize((thumb.width * scale, thumb.height * scale), Image.NEAREST).save(args.out_dir / "heatmap.png")
        thumb.resize((thumb.width * scale, thumb.height * scale), Image.NEAREST).save(args.out_dir / "figma.png")
        ours.resize((thumb.width * scale, thumb.height * scale), Image.NEAREST).save(args.out_dir / "ours.png")
        print(f"heatmap -> {args.out_dir/'heatmap.png'}")

    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
