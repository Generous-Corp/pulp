#!/usr/bin/env python3
"""diff_against_reference_regions.py — per-region masked similarity for
import-validation harness.

Companion to diff_against_reference.py. The full-image score (the existing
script) hides broken UI sub-regions: a canvas that's empty drags down the
score while the chrome looks fine, and a broken chrome region can pass if
the canvas happens to match. This script computes a per-region score so
the harness can fail on the FIRST broken region rather than averaging
everything into one number.

Codex review of planning/spectr-validated-runtime-import-product-spec.md
flagged this as a hard-gate requirement (2026-05-12).

Regions are defined as percent-of-window rects, so they scale across
different output sizes (the reference may be 2640x1620, the native render
1213x812 — the same region maps proportionally to each).

Default regions are Spectr's editor.html shape. Override with
`--regions <path-to-json>` for other fixtures.

Usage:
    diff_against_reference_regions.py <reference.png> <candidate.png>
    diff_against_reference_regions.py <reference.png> <candidate.png> --json
    diff_against_reference_regions.py <reference.png> <candidate.png> \
        --regions tools/import-validation/regions/v0-audio-panel.json

Exit codes:
    0  - every region meets its threshold, OR --strict was not passed
         (region failures are informational without --strict)
    1  - one or more regions below threshold AND --strict was passed
    2  - cannot compare (missing file, size mismatch, malformed regions JSON)
"""

from __future__ import annotations
import argparse
import json
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("error: PIL/Pillow required (pip install Pillow)", file=sys.stderr)
    sys.exit(2)

import warnings
warnings.filterwarnings("ignore", category=DeprecationWarning)


# Default regions for Spectr's editor.html. Coordinates are percent of
# window (0..1) — (x, y) is top-left, (w, h) is width/height. Thresholds
# are per-region similarity scores below which the region is judged
# broken. Canvas regions tolerate more variance because Skia raster + AA
# vary; chrome regions are tighter because text + button layout should
# match deterministically.
SPECTR_REGIONS = {
    "title_chrome": {
        "x": 0.00, "y": 0.00, "w": 0.30, "h": 0.05,
        "threshold": 0.80,
        "notes": "SPECTR title + ZOOMABLE FILTER BANK",
    },
    "mode_toggles": {
        "x": 0.30, "y": 0.00, "w": 0.40, "h": 0.05,
        "threshold": 0.75,
        "notes": "LIVE / PRECISION / IIR / FFT / HYBRID buttons",
    },
    "band_dropdown": {
        "x": 0.70, "y": 0.00, "w": 0.30, "h": 0.05,
        "threshold": 0.75,
        "notes": "64 bands dropdown + zoom indicator",
    },
    "central_canvas": {
        "x": 0.00, "y": 0.05, "w": 1.00, "h": 0.85,
        "threshold": 0.55,
        "notes": "Spectrum gradient + band columns + ruler — canvas paint, looser threshold for raster variance",
    },
    "bottom_toolbar": {
        "x": 0.00, "y": 0.90, "w": 1.00, "h": 0.10,
        "threshold": 0.75,
        "notes": "CLEAR/SCULPT/PEAK/PRESETS/SNAPSHOT/A/B + morph slider",
    },
}


def load_normalized(path: Path, target_size: tuple[int, int]) -> Image.Image:
    img = Image.open(path).convert("RGB")
    if img.size != target_size:
        img = img.resize(target_size, Image.Resampling.LANCZOS)
    return img


def crop_region(img: Image.Image, region: dict) -> Image.Image:
    """Crop image to a percent-of-window rect."""
    W, H = img.size
    x = int(region["x"] * W)
    y = int(region["y"] * H)
    w = max(1, int(region["w"] * W))
    h = max(1, int(region["h"] * H))
    return img.crop((x, y, x + w, y + h))


def histogram_similarity(a: Image.Image, b: Image.Image) -> float:
    ha = a.histogram()
    hb = b.histogram()
    dot = sum(x * y for x, y in zip(ha, hb))
    na = sum(x * x for x in ha) ** 0.5
    nb = sum(y * y for y in hb) ** 0.5
    if na == 0 or nb == 0:
        return 0.0
    return dot / (na * nb)


def mean_pixel_distance(a: Image.Image, b: Image.Image) -> float:
    if a.size != b.size:
        # Resize candidate region to reference region; cheap.
        b = b.resize(a.size, Image.Resampling.LANCZOS)
    pa = list(a.getdata())
    pb = list(b.getdata())
    if len(pa) != len(pb):
        return 0.0
    total = 0
    for (r1, g1, b1), (r2, g2, b2) in zip(pa, pb):
        total += ((r1 - r2) ** 2 + (g1 - g2) ** 2 + (b1 - b2) ** 2) ** 0.5
    avg = total / len(pa)
    return 1.0 - (avg / 441.7)


def is_blank(img: Image.Image, dark_threshold: int = 30) -> bool:
    pixels = list(img.getdata())
    if not pixels:
        return True
    near_black = sum(
        1 for (r, g, b) in pixels if max(r, g, b) < dark_threshold
    )
    return near_black / len(pixels) > 0.95


def region_score(ref_full: Image.Image, cand_full: Image.Image, region: dict) -> dict:
    ref_crop = crop_region(ref_full, region)
    cand_crop = crop_region(cand_full, region)
    hist = histogram_similarity(ref_crop, cand_crop)
    pix = mean_pixel_distance(ref_crop, cand_crop)
    score = (hist * 0.4) + (pix * 0.6)
    blank_cand = is_blank(cand_crop)
    blank_ref = is_blank(ref_crop)
    threshold = region.get("threshold", 0.75)
    passed = score >= threshold and not blank_cand
    return {
        "score": round(score, 4),
        "histogram_similarity": round(hist, 4),
        "pixel_similarity": round(pix, 4),
        "threshold": threshold,
        "passed": passed,
        "blank_candidate": blank_cand,
        "blank_reference": blank_ref,
        "rect_pct": {k: region[k] for k in ("x", "y", "w", "h")},
        "notes": region.get("notes", ""),
    }


def load_regions(path: Path | None) -> dict:
    if path is None:
        return SPECTR_REGIONS
    if not path.exists():
        print(f"error: regions JSON not found: {path}", file=sys.stderr)
        sys.exit(2)
    try:
        with open(path) as fh:
            data = json.load(fh)
    except json.JSONDecodeError as e:
        print(f"error: malformed regions JSON: {e}", file=sys.stderr)
        sys.exit(2)
    if not isinstance(data, dict) or not data:
        print(f"error: regions JSON must be a non-empty object", file=sys.stderr)
        sys.exit(2)
    # Validate each region has the required fields and that values are the
    # right shape. Without this, a malformed value (e.g. "x": "0.10" or a
    # string threshold) propagates into region_score() and crashes mid-run
    # instead of giving the user a clear configuration error up front.
    for name, region in data.items():
        if not isinstance(region, dict):
            print(f"error: region '{name}' must be an object", file=sys.stderr)
            sys.exit(2)
        for required in ("x", "y", "w", "h"):
            if required not in region:
                print(f"error: region '{name}' missing '{required}'", file=sys.stderr)
                sys.exit(2)
            value = region[required]
            # bool is a subclass of int in Python — reject it explicitly so
            # `"x": true` doesn't silently become 1.
            if isinstance(value, bool) or not isinstance(value, (int, float)):
                print(
                    f"error: region '{name}' has non-numeric {required}={value!r}",
                    file=sys.stderr,
                )
                sys.exit(2)
        if "threshold" in region:
            thr = region["threshold"]
            if isinstance(thr, bool) or not isinstance(thr, (int, float)) or not (0.0 <= float(thr) <= 1.0):
                print(
                    f"error: region '{name}' has invalid threshold={thr!r} (expected number in 0..1)",
                    file=sys.stderr,
                )
                sys.exit(2)
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="Reference PNG (ground truth)")
    parser.add_argument("candidate", type=Path, help="Candidate PNG to compare")
    parser.add_argument(
        "--size",
        default="1320x860",
        help="Normalize both images to this WxH before regioning (default 1320x860)",
    )
    parser.add_argument(
        "--regions",
        type=Path,
        default=None,
        help="JSON file with region definitions (default: built-in Spectr regions)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output machine-readable JSON instead of human text",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit non-zero if ANY region fails its threshold; without --strict, region failures are informational and the script exits 0",
    )
    args = parser.parse_args()

    for p in (args.reference, args.candidate):
        if not p.exists():
            print(f"error: file not found: {p}", file=sys.stderr)
            return 2

    regions = load_regions(args.regions)
    try:
        w, h = (int(x) for x in args.size.split("x"))
    except ValueError:
        print(f"error: malformed --size {args.size}", file=sys.stderr)
        return 2
    target = (w, h)

    try:
        ref = load_normalized(args.reference, target)
        cand = load_normalized(args.candidate, target)
    except Exception as e:
        print(f"error: failed to load images: {e}", file=sys.stderr)
        return 2

    results = {name: region_score(ref, cand, region) for name, region in regions.items()}
    failed_names = [n for n, r in results.items() if not r["passed"]]
    all_passed = not failed_names

    if args.json:
        print(json.dumps({
            "reference": str(args.reference),
            "candidate": str(args.candidate),
            "size_normalized_to": args.size,
            "regions": results,
            "failed_regions": failed_names,
            "all_passed": all_passed,
        }, indent=2))
    else:
        verdict = "PASS ✓" if all_passed else "FAIL ✗"
        print(f"{verdict}  {len(results) - len(failed_names)}/{len(results)} regions pass")
        print()
        for name, r in results.items():
            mark = "✓" if r["passed"] else "✗"
            blank = " (BLANK)" if r["blank_candidate"] else ""
            print(f"  {mark} {name:18s} score={r['score']:.3f} threshold={r['threshold']:.3f}{blank}")
            print(f"    rect: x={r['rect_pct']['x']:.2f} y={r['rect_pct']['y']:.2f} w={r['rect_pct']['w']:.2f} h={r['rect_pct']['h']:.2f}")
            if r["notes"]:
                print(f"    {r['notes']}")
        if failed_names:
            print()
            print(f"  Failed regions: {', '.join(failed_names)}")

    # --strict gates the non-zero exit. Without it, region failures are
    # informational only (still printed / still in JSON output) but the
    # script exits 0 so callers can run the diff without forcing a fail.
    if all_passed or not args.strict:
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
