#!/usr/bin/env python3
"""diff_against_reference.py — compare a native-render screenshot against the
canonical REFERENCE render of spectr/resources/editor.html (or any HTML).

Outputs a single similarity score (0.0..1.0) and a short verdict. Designed
to be called autonomously by the agent BEFORE showing the user any native
screenshot: if the score is below the threshold, the agent knows the import
is broken and must NOT claim "it works."

Usage:
    diff_against_reference.py <reference.png> <candidate.png> [--threshold 0.85]

Exit codes:
    0  - candidate matches reference within threshold (PASS)
    1  - candidate diverges from reference (FAIL — don't show user as working)
    2  - cannot compare (e.g., file missing, size mismatch beyond resize)

Metrics computed:
    - histogram cosine similarity (gross color distribution match)
    - mean per-channel L2 distance (overall pixel closeness)
    - blank-detection (is the candidate essentially empty?)
    - dominant-color check (is the candidate the right "vibe"?)

Note: this is intentionally crude — no SSIM, no perceptual hashing. Goal is
"obviously broken" detection (different layout, missing UI, blank screen,
fallback-vs-real), NOT pixel-level antialiasing match.
"""

from __future__ import annotations
import argparse
import sys
from pathlib import Path

import warnings
warnings.filterwarnings("ignore", category=DeprecationWarning)


def _image_module():
    try:
        from PIL import Image
    except ImportError as exc:
        raise RuntimeError("PIL/Pillow required (pip install Pillow)") from exc
    return Image


def load_normalized(path: Path, target_size: tuple[int, int]) -> Image.Image:
    Image = _image_module()
    img = Image.open(path).convert("RGB")
    if img.size != target_size:
        img = img.resize(target_size, Image.Resampling.LANCZOS)
    return img


def histogram_similarity(a: Image.Image, b: Image.Image) -> float:
    """Cosine similarity over per-channel histograms.

    Returns 1.0 for identical color distributions, ~0.0 for completely
    disjoint. Insensitive to layout — only color/density.
    """
    ha = a.histogram()
    hb = b.histogram()
    dot = sum(x * y for x, y in zip(ha, hb))
    na = sum(x * x for x in ha) ** 0.5
    nb = sum(y * y for y in hb) ** 0.5
    if na == 0 or nb == 0:
        return 0.0
    return dot / (na * nb)


def mean_pixel_distance(a: Image.Image, b: Image.Image) -> float:
    """Mean per-pixel L2 distance, normalized to 0..1 (1=identical)."""
    pa = list(a.getdata())
    pb = list(b.getdata())
    if len(pa) != len(pb):
        return 0.0
    total = 0
    for (r1, g1, b1), (r2, g2, b2) in zip(pa, pb):
        total += ((r1 - r2) ** 2 + (g1 - g2) ** 2 + (b1 - b2) ** 2) ** 0.5
    avg = total / len(pa)
    # Max possible per-pixel distance is sqrt(3 * 255^2) ≈ 441.7
    return 1.0 - (avg / 441.7)


def is_blank(img: Image.Image, dark_threshold: int = 30) -> bool:
    """True if the image is essentially blank (>95% pixels near-black)."""
    pixels = list(img.getdata())
    near_black = sum(
        1 for (r, g, b) in pixels if max(r, g, b) < dark_threshold
    )
    return near_black / len(pixels) > 0.95


def dominant_colors(img: Image.Image, k: int = 5) -> list[tuple[int, int, int]]:
    """Top-k most common quantized colors (coarse, 32-step quantization)."""
    pixels = [(r // 32 * 32, g // 32 * 32, b // 32 * 32) for (r, g, b) in img.getdata()]
    counts: dict[tuple[int, int, int], int] = {}
    for p in pixels:
        counts[p] = counts.get(p, 0) + 1
    return sorted(counts, key=lambda c: -counts[c])[:k]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="Reference PNG (ground truth)")
    parser.add_argument("candidate", type=Path, help="Candidate PNG to compare")
    parser.add_argument(
        "--threshold",
        type=float,
        default=0.85,
        help="Similarity threshold (0..1) — below this is FAIL (default 0.85)",
    )
    parser.add_argument(
        "--size",
        default="1320x860",
        help="Normalize both images to this WxH before comparing (default 1320x860)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output machine-readable JSON instead of human text",
    )
    args = parser.parse_args()

    for p in (args.reference, args.candidate):
        if not p.exists():
            print(f"error: file not found: {p}", file=sys.stderr)
            return 2

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

    hist_sim = histogram_similarity(ref, cand)
    pix_sim = mean_pixel_distance(ref, cand)
    blank_ref = is_blank(ref)
    blank_cand = is_blank(cand)
    dom_ref = dominant_colors(ref)
    dom_cand = dominant_colors(cand)
    score = (hist_sim * 0.4) + (pix_sim * 0.6)
    passed = score >= args.threshold and not blank_cand

    # Severity tier — diagnostic categorization on top of pass/fail
    if blank_cand:
        tier = "blank"
        diagnosis = "candidate is essentially blank (native bridge didn't mount, or window not visible)"
    elif score < 0.40:
        tier = "fundamental"
        diagnosis = "candidate is fundamentally different from reference (wrong app or wrong UI mode)"
    elif score < 0.70:
        tier = "wrong-variant"
        diagnosis = "candidate has wrong UI variant (fallback layout, or partial render)"
    elif score < args.threshold:
        tier = "close"
        diagnosis = f"candidate close but missing details (score {score:.3f} < threshold {args.threshold})"
    else:
        tier = "pass"
        diagnosis = "candidate matches reference within tolerance"

    if args.json:
        import json
        print(json.dumps({
            "reference": str(args.reference),
            "candidate": str(args.candidate),
            "histogram_similarity": round(hist_sim, 4),
            "pixel_similarity": round(pix_sim, 4),
            "score": round(score, 4),
            "threshold": args.threshold,
            "passed": passed,
            "tier": tier,
            "diagnosis": diagnosis,
            "blank_candidate": blank_cand,
            "blank_reference": blank_ref,
            "dominant_colors_ref": [list(c) for c in dom_ref],
            "dominant_colors_cand": [list(c) for c in dom_cand],
        }))
    else:
        verdict = "PASS ✓" if passed else "FAIL ✗"
        print(f"{verdict}  score={score:.3f}  threshold={args.threshold}  tier={tier}")
        print(f"  histogram_similarity: {hist_sim:.3f}")
        print(f"  pixel_similarity:     {pix_sim:.3f}")
        print(f"  diagnosis: {diagnosis}")
        if not passed:
            print(f"  ref dominant colors:  {dom_ref}")
            print(f"  cand dominant colors: {dom_cand}")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
