#!/usr/bin/env python3
"""text_raster_decomposition.py -- split a text render's disagreement with its
Chromium oracle into ink mass, placement and tone transfer.

A whole-node pixel diff cannot say WHY a glyph run fails: the same failing
pixel count comes from glyphs that are too heavy, glyphs that sit a fraction of
a pixel off, or a coverage curve with a different shape. This decomposes one
text box at a time into numbers that each name one of those mechanisms:

  ink_ratio      sum of background-subtracted luminance, candidate / reference.
                 1.00 is equal ink. Position-blind by construction.
  count_ratio    pixels with ink above a fixed level, candidate / reference.
                 Separates "more pixels lit" (dilation) from "same pixels, more
                 coverage each" (tone curve).
  dx, dy         ink-weighted centroid difference, candidate minus reference,
                 in device pixels.
  shift          the integer (dx, dy) in [-3, 3] that minimises the L1 between
                 the two ink maps. A non-zero shift is a placement error, not a
                 rasterization one.
  transfer       mean candidate coverage per reference-coverage decile over
                 pixels where either side has ink, plus the spread within each
                 decile. A monotone curve with small spread is tone transfer
                 (fixable by a rasterization knob); a large spread is
                 positioning.

Subcommands:

  html     write a Chromium fixture page for a labels file
  spec     turn the page's measured baselines into a probe spec (TSV)
  analyze  decompose every labelled box of a candidate render against a
           reference render
  crops    decompose the text nodes of an imported panel, reading boxes from
           the capture's DOM snapshot

The labels file is JSON: {"font": {"family", "file"}, "background": "#rrggbb",
"line_height": px, "labels": [{"id", "text", "size", "weight",
"letter_spacing", "color": "rgba(r,g,b,a)", "x", "top", "direction"}]};
"direction" is optional and defaults to "ltr". Coordinates are CSS
pixels; renders are device pixels at --scale.

The ink measurement assumes light text on a darker, locally flat background,
which is what the background estimate (the box border's median) relies on.
Boxes whose border is not flat are reported with `background_spread` so a
reader can discount them.
"""

from __future__ import annotations

import argparse
import html as html_lib
import json
import sys
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # pragma: no cover - environment guard
    sys.stderr.write(f"text_raster_decomposition: needs numpy and Pillow ({exc})\n")
    sys.exit(7)

EX_INPUT = 2
EX_HARNESS = 7

INK_LEVEL = 40.0
SHIFT_RANGE = 3
PAD = 3


def load_luma(path: Path) -> np.ndarray:
    img = Image.open(path).convert("RGB")
    rgb = np.asarray(img).astype(np.float64)
    return 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]


def ink_map(luma: np.ndarray, box: tuple[int, int, int, int]) -> tuple[np.ndarray, float, float]:
    """Background-subtracted ink inside `box` (x0, y0, x1, y1), clamped at 0."""
    x0, y0, x1, y1 = box
    region = luma[y0:y1, x0:x1]
    border = np.concatenate([region[0, :], region[-1, :], region[:, 0], region[:, -1]])
    bg = float(np.median(border))
    spread = float(np.percentile(border, 90) - np.percentile(border, 10))
    return np.clip(region - bg, 0.0, None), bg, spread


def centroid(ink: np.ndarray) -> tuple[float, float]:
    total = ink.sum()
    if total <= 0:
        return float("nan"), float("nan")
    ys, xs = np.indices(ink.shape)
    return float((xs * ink).sum() / total), float((ys * ink).sum() / total)


def best_shift(ref: np.ndarray, cand: np.ndarray) -> tuple[int, int, float]:
    best = (0, 0, float("inf"))
    h, w = ref.shape
    r = SHIFT_RANGE
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            a = ref[r:h - r, r:w - r]
            b = cand[r + dy:h - r + dy, r + dx:w - r + dx]
            cost = float(np.abs(a - b).sum())
            if cost < best[2]:
                best = (dx, dy, cost)
    return best


def transfer_curve(ref: np.ndarray, cand: np.ndarray) -> dict:
    peak = max(float(ref.max()), 1e-9)
    rc, cc = ref / peak, cand / peak
    mask = (rc > 0.02) | (cc > 0.02)
    bins = []
    edges = np.linspace(0.0, 1.0, 11)
    for lo, hi in zip(edges[:-1], edges[1:]):
        sel = mask & (rc >= lo) & ((rc < hi) if hi < 1.0 else (rc <= hi))
        if sel.sum() < 3:
            bins.append(None)
            continue
        bins.append({
            "ref": round(float(rc[sel].mean()), 3),
            "cand": round(float(cc[sel].mean()), 3),
            "spread": round(float(cc[sel].std()), 3),
            "n": int(sel.sum()),
        })
    filled = [b for b in bins if b]
    monotone = all(b2["cand"] >= b1["cand"] - 0.02 for b1, b2 in zip(filled, filled[1:]))
    spread = float(np.average([b["spread"] for b in filled], weights=[b["n"] for b in filled])) if filled else float("nan")
    return {"bins": bins, "monotone": monotone, "mean_spread": round(spread, 3)}


def decompose(ref_luma: np.ndarray, cand_luma: np.ndarray, box: tuple[int, int, int, int]) -> dict:
    h, w = ref_luma.shape
    x0, y0, x1, y1 = box
    box = (max(0, x0 - PAD), max(0, y0 - PAD), min(w, x1 + PAD), min(h, y1 + PAD))
    ref, ref_bg, ref_spread = ink_map(ref_luma, box)
    cand, cand_bg, cand_spread = ink_map(cand_luma, box)
    ref_mass, cand_mass = float(ref.sum()), float(cand.sum())
    if ref_mass <= 0:
        return {"box": list(box), "error": "reference box has no ink"}
    rcx, rcy = centroid(ref)
    ccx, ccy = centroid(cand)
    sdx, sdy, _ = best_shift(ref, cand)
    ref_count = int((ref > INK_LEVEL).sum())
    return {
        "box": list(box),
        "ink_ratio": round(cand_mass / ref_mass, 4),
        "count_ratio": round(int((cand > INK_LEVEL).sum()) / ref_count, 4) if ref_count else None,
        "peak": [round(float(ref.max()), 1), round(float(cand.max()), 1)],
        "dx": round(ccx - rcx, 3),
        "dy": round(ccy - rcy, 3),
        "shift": [sdx, sdy],
        "background": [round(ref_bg, 1), round(cand_bg, 1)],
        "background_spread": round(max(ref_spread, cand_spread), 1),
        "transfer": transfer_curve(ref, cand),
    }


def summarize(rows: list[dict]) -> dict:
    good = [r for r in rows if "ink_ratio" in r]
    if not good:
        return {"labels": 0}
    ratios = np.array([r["ink_ratio"] for r in good])
    cent = np.array([np.hypot(r["dx"], r["dy"]) for r in good])
    return {
        "labels": len(good),
        "ink_ratio_median": round(float(np.median(ratios)), 4),
        "ink_ratio_min": round(float(ratios.min()), 4),
        "ink_ratio_max": round(float(ratios.max()), 4),
        "centroid_median": round(float(np.median(cent)), 3),
        "centroid_max": round(float(cent.max()), 3),
        "nonzero_shift": sum(1 for r in good if r["shift"] != [0, 0]),
    }


def print_table(rows: list[dict], out=sys.stdout) -> None:
    out.write(f"{'label':<24} {'ink':>6} {'count':>6} {'dx':>6} {'dy':>6} {'shift':>7} {'mono':>5} {'sprd':>5}\n")
    for r in rows:
        if "ink_ratio" not in r:
            out.write(f"{r['id'][:24]:<24} {r.get('error', '')}\n")
            continue
        out.write(
            f"{r['id'][:24]:<24} {r['ink_ratio']:>6.3f} {r['count_ratio'] or 0:>6.3f} "
            f"{r['dx']:>6.2f} {r['dy']:>6.2f} {str(tuple(r['shift'])):>7} "
            f"{'y' if r['transfer']['monotone'] else 'n':>5} {r['transfer']['mean_spread']:>5.2f}\n")


def load_json(path: str):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError) as exc:
        sys.stderr.write(f"text_raster_decomposition: cannot read {path}: {exc}\n")
        sys.exit(EX_INPUT)


def cmd_html(args) -> int:
    spec = load_json(args.labels)
    lh = spec["line_height"]
    font = spec["font"]
    rows = []
    for lab in spec["labels"]:
        rows.append(
            f'<div class="l" id="{html_lib.escape(lab["id"])}" dir="{lab.get("direction", "ltr")}" style="left:{lab["x"]}px;top:{lab["top"]}px;'
            f'font-size:{lab["size"]}px;font-weight:{lab.get("weight", 400)};'
            f'letter-spacing:{lab.get("letter_spacing", 0)}px;color:{lab["color"]}">'
            f'{html_lib.escape(lab["text"])}<span class="m"></span></div>')
    smoothing = f"-webkit-font-smoothing:{args.smoothing};" if args.smoothing else ""
    page = f"""<!doctype html>
<html><head><meta charset="utf-8"><style>
@font-face {{ font-family: "{font['family']}"; src: url("{args.font_url}"); font-weight: {font.get('weight_range', '100 900')}; }}
html, body {{ margin:0; background:{spec['background']}; {smoothing} }}
.l {{ position:absolute; white-space:pre; line-height:{lh}px; font-family:"{font['family']}"; }}
.m {{ display:inline-block; width:0; height:0; vertical-align:baseline; }}
#baselines {{ position:absolute; left:-9999px; }}
</style></head><body>
{chr(10).join(rows)}
<pre id="baselines"></pre>
<script>
document.fonts.ready.then(() => {{
  const out = {{}};
  for (const el of document.querySelectorAll('.l')) {{
    const m = el.querySelector('.m').getBoundingClientRect();
    out[el.id] = {{ x: el.getBoundingClientRect().left, baseline: m.top }};
  }}
  document.getElementById('baselines').textContent = JSON.stringify(out);
}});
</script>
</body></html>
"""
    Path(args.out).write_text(page)
    return 0


def parse_rgba(text: str) -> str:
    inner = text[text.index("(") + 1:text.rindex(")")]
    parts = [p.strip() for p in inner.split(",")]
    r, g, b = (int(float(p)) for p in parts[:3])
    a = float(parts[3]) if len(parts) > 3 else 1.0
    return f"{r:02x}{g:02x}{b:02x}{int(round(a * 255)):02x}"


def cmd_spec(args) -> int:
    spec = load_json(args.labels)
    raw = Path(args.baselines).read_text()
    start, end = raw.find("{"), raw.rfind("}")
    if start < 0:
        sys.stderr.write("text_raster_decomposition: no baselines JSON in the dump\n")
        return EX_INPUT
    measured = json.loads(raw[start:end + 1])
    lines = []
    for lab in spec["labels"]:
        m = measured.get(lab["id"])
        if m is None:
            sys.stderr.write(f"text_raster_decomposition: no baseline for {lab['id']}\n")
            return EX_INPUT
        lines.append("\t".join([
            f"{m['x']:.4f}", f"{m['baseline']:.4f}", str(lab["size"]), str(lab.get("weight", 400)),
            str(lab.get("letter_spacing", 0)), parse_rgba(lab["color"]), lab["text"],
            lab.get("direction", "ltr")]))
    Path(args.out).write_text("\n".join(lines) + "\n")
    return 0


def label_boxes(spec: dict, measured: dict, scale: float) -> list[tuple[str, tuple[int, int, int, int]]]:
    lh = spec["line_height"]
    boxes = []
    for lab in spec["labels"]:
        m = measured[lab["id"]]
        width = len(lab["text"]) * (lab["size"] * 0.6 + lab.get("letter_spacing", 0)) + lab["size"]
        top = m["baseline"] - lab["size"] * 1.1
        boxes.append((lab["id"], (int(m["x"] * scale) - 2, int(top * scale),
                                  int((m["x"] + width) * scale), int((top + lh) * scale))))
    return boxes


def run_boxes(ref_path: str, cand_path: str, boxes) -> list[dict]:
    ref_luma, cand_luma = load_luma(Path(ref_path)), load_luma(Path(cand_path))
    if ref_luma.shape != cand_luma.shape:
        sys.stderr.write(f"text_raster_decomposition: size mismatch {ref_luma.shape} vs {cand_luma.shape}\n")
        sys.exit(EX_INPUT)
    rows = []
    for ident, box in boxes:
        row = {"id": ident}
        row.update(decompose(ref_luma, cand_luma, box))
        rows.append(row)
    return rows


def emit(rows: list[dict], args) -> int:
    report = {"schema": "pulp-text-raster-decomposition-v1", "summary": summarize(rows), "labels": rows}
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1))
    print_table(rows)
    s = report["summary"]
    print(f"summary: {json.dumps(s)}")
    return 0 if s.get("labels") else EX_HARNESS


def cmd_analyze(args) -> int:
    spec = load_json(args.labels)
    raw = Path(args.baselines).read_text()
    measured = json.loads(raw[raw.find("{"):raw.rfind("}") + 1])
    return emit(run_boxes(args.ref, args.cand, label_boxes(spec, measured, args.scale)), args)


def cmd_crops(args) -> int:
    cap = Path(args.capture)
    snap = load_json(str(cap / "dom-snapshot.json"))
    strings = snap["strings"]
    doc = snap["documents"][0]
    names, values = doc["nodes"]["nodeName"], doc["nodes"]["nodeValue"]
    layout = doc["layout"]
    wanted = set(args.only or [])
    boxes = []
    for k, ni in enumerate(layout["nodeIndex"]):
        if strings[names[ni]] != "#text" or values[ni] < 0:
            continue
        text = strings[values[ni]]
        if not text.strip() or (wanted and text not in wanted):
            continue
        x, y, w, h = layout["bounds"][k]
        boxes.append((f"{ni}:{text}", tuple(int(round(v * args.scale)) for v in (x, y, x + w, y + h))))
    ref = args.ref or str(cap / "browser.png")
    return emit(run_boxes(ref, args.cand, boxes), args)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    h = sub.add_parser("html")
    h.add_argument("labels")
    h.add_argument("--font-url", required=True)
    h.add_argument("--smoothing", choices=["antialiased", "auto", "subpixel-antialiased"])
    h.add_argument("--out", required=True)
    s = sub.add_parser("spec")
    s.add_argument("labels")
    s.add_argument("baselines")
    s.add_argument("--out", required=True)
    for name in ("analyze", "crops"):
        a = sub.add_parser(name)
        a.add_argument("--cand", required=True)
        a.add_argument("--scale", type=float, default=2.0)
        a.add_argument("--json")
        if name == "analyze":
            a.add_argument("--labels", required=True)
            a.add_argument("--baselines", required=True)
            a.add_argument("--ref", required=True)
        else:
            a.add_argument("--capture", required=True)
            a.add_argument("--ref")
            a.add_argument("--only", action="append")
    args = p.parse_args(argv)
    return {"html": cmd_html, "spec": cmd_spec, "analyze": cmd_analyze, "crops": cmd_crops}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
