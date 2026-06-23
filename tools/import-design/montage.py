#!/usr/bin/env python3
"""Labeled comparison montage for design-import renders.

Stacks N images into one montage with a titled bar above each panel, so a
reference-vs-render(s) comparison is self-documenting (you can tell which panel
is which without an external caption). Labels are ON by default — that's the
point. Used for figma-import fidelity comparisons (reference / sprite-strip /
native-vector / headless-REST, etc.).

Usage:
  # path:Label pairs (label defaults to the file stem if omitted)
  montage.py --out compare.png ref.png:"Figma reference" render.png:"Pulp render"
  montage.py --out compare.png --columns 2 a.png:"A" b.png:"B"   # side-by-side
  montage.py --out compare.png --no-labels a.png b.png            # opt out
  montage.py --out compare.png --config montage.json ...          # defaults via config

Config (JSON, all optional; CLI overrides): {
  "labels": true, "columns": 1, "title_height": 36, "pad": 14,
  "bg": [20,20,24], "title_bg": [12,12,14], "title_fg": [235,235,240],
  "panel_width": 1000, "font_size": 22
}
Importable: build_montage(panels, out_path, **opts) where panels=[(label, path), ...].
"""
import argparse, json, os, sys

def _load_pil():
    try:
        from PIL import Image, ImageDraw, ImageFont
        return Image, ImageDraw, ImageFont
    except ImportError:
        sys.stderr.write("montage.py requires Pillow (pip install pillow)\n")
        raise

def _font(ImageFont, size):
    for p in ("/System/Library/Fonts/SFNSMono.ttf",
              "/System/Library/Fonts/Supplemental/Arial.ttf",
              "/Library/Fonts/Arial.ttf"):
        if os.path.exists(p):
            try: return ImageFont.truetype(p, size)
            except Exception: pass
    return ImageFont.load_default()

def build_montage(panels, out_path, labels=True, columns=1, title_height=36,
                  pad=14, bg=(20, 20, 24), title_bg=(12, 12, 14),
                  title_fg=(235, 235, 240), panel_width=1000, font_size=22):
    """panels: list of (label, image_path). Returns the output path."""
    Image, ImageDraw, ImageFont = _load_pil()
    font = _font(ImageFont, font_size)
    cells = []  # (label, resized-image)
    for label, path in panels:
        im = Image.open(path).convert("RGB")
        w, h = im.size
        if w != panel_width:
            im = im.resize((panel_width, max(1, int(h * panel_width / w))))
        cells.append((label, im))
    th = title_height if labels else 0
    col_w = panel_width + pad * 2
    rows = (len(cells) + columns - 1) // columns
    # Per-row height = max cell height in that row + title + pad.
    row_heights = []
    for r in range(rows):
        rowcells = cells[r * columns:(r + 1) * columns]
        row_heights.append(max(c[1].height for c in rowcells) + th + pad)
    W = col_w * columns + pad
    H = sum(row_heights) + pad
    canvas = Image.new("RGB", (W, H), bg)
    draw = ImageDraw.Draw(canvas)
    y = pad
    for r in range(rows):
        x = pad
        for c in range(columns):
            idx = r * columns + c
            if idx >= len(cells): break
            label, im = cells[idx]
            if labels:
                draw.rectangle([x, y, x + panel_width, y + th], fill=title_bg)
                draw.text((x + 10, y + (th - font_size) // 2), label, fill=title_fg, font=font)
            canvas.paste(im, (x, y + th))
            x += col_w
        y += row_heights[r]
    canvas.save(out_path)
    return out_path

def _parse_panel(spec):
    # "path:Label" → (label, path). Split on the FIRST ':' so a label that itself
    # contains colons survives ("x.png:1. Figma: source" must keep "1. Figma:
    # source" as the label, not truncate at the last colon). Fall back
    # to treating the whole spec as a path when the head isn't an existing file
    # (bare path, or a path with no label).
    if os.path.exists(spec):                      # whole spec is a real path → no label
        return (os.path.splitext(os.path.basename(spec))[0], spec)
    head, sep, label = spec.partition(":")        # first colon
    if sep and head and os.path.exists(head):
        return (label or os.path.splitext(os.path.basename(head))[0], head)
    return (os.path.splitext(os.path.basename(spec))[0], spec)

def main():
    ap = argparse.ArgumentParser(description="Labeled comparison montage for import renders")
    ap.add_argument("panels", nargs="+", help='image specs: path or "path:Label"')
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-labels", action="store_true", help="disable titles (default: labeled)")
    ap.add_argument("--columns", type=int)
    ap.add_argument("--config", help="JSON defaults (CLI overrides)")
    args = ap.parse_args()
    opts = {}
    if args.config and os.path.exists(args.config):
        opts = json.load(open(args.config))
    if args.no_labels: opts["labels"] = False
    if args.columns is not None: opts["columns"] = args.columns
    panels = [_parse_panel(s) for s in args.panels]
    out = build_montage(panels, args.out, **opts)
    print(f"wrote {out}")

if __name__ == "__main__":
    main()
