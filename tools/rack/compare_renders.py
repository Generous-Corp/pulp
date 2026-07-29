#!/usr/bin/env python3
"""Put the design, our render, and VCV Rack beside each other.

Three surfaces have to agree and each is captured differently, so comparing
them was being done by eye across three files -- which is how a shell that
matched the design on structure and missed it on every finish detail got called
finished twice.

    compare_renders.py --out sheet.png LABEL=path [LABEL=path ...]

Scales each panel to a common height, labels it, and stacks them left to right.
Also reports the mean per-pixel difference between the first two panels after
resizing to a common size, which is a blunt number but a monotone one: it goes
down when they converge and up when they diverge, and it cannot be argued with
the way "looks close enough" can.
"""

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:                                     # pragma: no cover
    sys.exit("compare_renders needs Pillow: python3 -m pip install pillow")

BG = (12, 14, 18)
INK = (243, 246, 249)
MUTED = (147, 156, 169)
GUTTER = 24
LABEL_H = 34
TARGET_H = 900


def _load(path):
    if not os.path.exists(path):
        return None
    img = Image.open(path).convert("RGB")
    scale = TARGET_H / img.height
    return img.resize((max(1, round(img.width * scale)), TARGET_H), Image.LANCZOS)


def mean_difference(a, b):
    """Mean absolute per-channel difference, 0-255, after a common resize.

    Deliberately crude. It will never say *why* two renders differ, but it says
    whether a change moved them together or apart, which is the question that
    kept being answered by opinion.
    """
    size = (min(a.width, b.width), min(a.height, b.height))
    a2, b2 = a.resize(size, Image.LANCZOS), b.resize(size, Image.LANCZOS)
    pa, pb = a2.load(), b2.load()
    step = max(1, size[0] // 400)          # sample; a full walk is needlessly slow
    total = count = 0
    for y in range(0, size[1], step):
        for x in range(0, size[0], step):
            ra, ga, ba = pa[x, y]
            rb, gb, bb = pb[x, y]
            total += abs(ra - rb) + abs(ga - gb) + abs(ba - bb)
            count += 3
    return total / count if count else 0.0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("panels", nargs="+", metavar="LABEL=PATH")
    args = ap.parse_args(argv)

    loaded, missing = [], []
    for spec in args.panels:
        if "=" not in spec:
            sys.exit(f"expected LABEL=PATH, got {spec!r}")
        label, path = spec.split("=", 1)
        img = _load(path)
        if img is None:
            missing.append((label, path))
        else:
            loaded.append((label, img))

    for label, path in missing:
        # Naming what is absent matters: a two-panel sheet looks exactly like a
        # complete comparison unless it says which surface is not in it.
        print(f"MISSING  {label}: {path}")
    if not loaded:
        sys.exit("nothing to compare")

    width = sum(i.width for _, i in loaded) + GUTTER * (len(loaded) + 1)
    sheet = Image.new("RGB", (width, TARGET_H + LABEL_H + GUTTER * 2), BG)
    draw = ImageDraw.Draw(sheet)

    x = GUTTER
    for label, img in loaded:
        draw.text((x, GUTTER // 2), label.upper(), fill=INK)
        sheet.paste(img, (x, GUTTER // 2 + LABEL_H))
        draw.text((x, GUTTER // 2 + LABEL_H + TARGET_H + 4),
                  f"{img.width}x{img.height}", fill=MUTED)
        x += img.width + GUTTER

    sheet.save(args.out)
    print(f"wrote {args.out}  ({len(loaded)} panel(s), {len(missing)} missing)")

    if len(loaded) >= 2:
        d = mean_difference(loaded[0][1], loaded[1][1])
        print(f"mean per-pixel difference {loaded[0][0]} vs {loaded[1][0]}: {d:.1f}/255")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
