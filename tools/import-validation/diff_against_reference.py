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
import json
import math
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


def load_normalized(
    path: Path, target_size: tuple[int, int], mode: str = "RGB"
) -> Image.Image:
    Image = _image_module()
    img = Image.open(path).convert(mode)
    if img.size != target_size:
        img = img.resize(target_size, Image.Resampling.LANCZOS)
    return img


def residual_profile(
    reference: Image.Image,
    candidate: Image.Image,
    threshold: int = 8,
    edge_threshold: int = 16,
    shift_radius: int = 2,
) -> dict[str, object]:
    """Classify residual pixels without changing the canonical fidelity score.

    This is deliberately a diagnostic layer. It separates alpha-only changes,
    likely translation, and edge-concentrated residuals so a sub-threshold
    result can be investigated without relaxing the raw gate. The shift probe
    measures overlap only; it is evidence of translation sensitivity, not a
    proposed correction.
    """
    if reference.size != candidate.size:
        raise ValueError("residual images must have identical dimensions")
    if threshold < 0 or edge_threshold < 0 or shift_radius < 0:
        raise ValueError("residual thresholds must be non-negative")

    ref_pixels = list(reference.getdata())
    cand_pixels = list(candidate.getdata())
    if len(ref_pixels) != len(cand_pixels) or not ref_pixels:
        raise ValueError("residual images must contain the same non-empty pixels")
    channels = len(ref_pixels[0])
    if channels not in (3, 4) or any(len(pixel) != channels for pixel in ref_pixels + cand_pixels):
        raise ValueError("residual images must be RGB or RGBA")
    width, height = reference.size

    def rgba(pixel: tuple[int, ...]) -> tuple[int, int, int, int]:
        return (*pixel[:3], pixel[3] if len(pixel) == 4 else 255)

    ref_rgba = [rgba(pixel) for pixel in ref_pixels]
    cand_rgba = [rgba(pixel) for pixel in cand_pixels]

    def luminance(pixel: tuple[int, int, int, int]) -> float:
        return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]

    ref_luma = [luminance(pixel) for pixel in ref_rgba]

    def index(x: int, y: int) -> int:
        return y * width + x

    def edge_strength(x: int, y: int) -> float:
        center = ref_luma[index(x, y)]
        neighbors = []
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if 0 <= nx < width and 0 <= ny < height:
                neighbors.append(abs(center - ref_luma[index(nx, ny)]))
        return max(neighbors, default=0.0)

    def mismatch(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> bool:
        return max(abs(a[i] - b[i]) for i in range(3)) > threshold or abs(a[3] - b[3]) > threshold

    differing = 0
    alpha_only = 0
    edge_pixels = 0
    channel_abs = [0, 0, 0, 0]
    for ref_pixel, cand_pixel in zip(ref_rgba, cand_rgba):
        deltas = [abs(ref_pixel[i] - cand_pixel[i]) for i in range(4)]
        channel_abs = [left + right for left, right in zip(channel_abs, deltas)]
        rgb_changed = max(deltas[:3]) > threshold
        alpha_changed = deltas[3] > threshold
        if rgb_changed or alpha_changed:
            differing += 1
            if alpha_changed and not rgb_changed:
                alpha_only += 1
        # Edge concentration is evaluated on residual pixels only below.

    for y in range(height):
        for x in range(width):
            offset = index(x, y)
            if mismatch(ref_rgba[offset], cand_rgba[offset]) and edge_strength(x, y) >= edge_threshold:
                edge_pixels += 1

    def diff_rate(dx: int, dy: int) -> float:
        compared = 0
        changed = 0
        for y in range(0, height, probe_stride):
            cy = y + dy
            if not 0 <= cy < height:
                continue
            for x in range(0, width, probe_stride):
                cx = x + dx
                if not 0 <= cx < width:
                    continue
                compared += 1
                if mismatch(ref_rgba[index(x, y)], cand_rgba[index(cx, cy)]):
                    changed += 1
        return changed / compared if compared else math.inf

    # The exact residual counts above remain full-resolution. For the more
    # expensive translated-image probe, sample a deterministic grid on large
    # panels so diagnostics stay bounded without affecting the raw score.
    pixel_count = width * height
    probe_stride = max(1, math.ceil(math.sqrt(pixel_count / 250_000)))
    baseline_rate = diff_rate(0, 0)
    shifts = [
        {"dx": dx, "dy": dy, "rate": diff_rate(dx, dy)}
        for dy in range(-shift_radius, shift_radius + 1)
        for dx in range(-shift_radius, shift_radius + 1)
    ]
    best_shift = min(shifts, key=lambda row: (row["rate"], abs(row["dx"]) + abs(row["dy"])))
    shift_improvement = (
        (baseline_rate - best_shift["rate"]) / baseline_rate
        if baseline_rate > 0 else 0.0
    )
    edge_fraction = edge_pixels / differing if differing else 0.0
    alpha_fraction = alpha_only / differing if differing else 0.0

    if differing == 0:
        classification = "identical"
    elif alpha_only == differing:
        classification = "alpha-only"
    elif shift_improvement >= 0.15:
        classification = "placement-drift"
    elif edge_fraction >= 0.65:
        classification = "text-edge/rasterization"
    else:
        classification = "mixed"

    return {
        "schema": "pulp-visual-residual-profile-v1",
        "dimensions": {"width": width, "height": height},
        "thresholds": {
            "channel": threshold,
            "edge_luminance": edge_threshold,
            "shift_radius": shift_radius,
        },
        "pixel_count": pixel_count,
        "differing_pixels": differing,
        "differing_fraction": differing / pixel_count,
        "alpha_only_pixels": alpha_only,
        "alpha_only_fraction": alpha_fraction,
        "edge_band_pixels": edge_pixels,
        "edge_band_fraction": edge_fraction,
        "mean_absolute_delta": {
            "red": channel_abs[0] / pixel_count,
            "green": channel_abs[1] / pixel_count,
            "blue": channel_abs[2] / pixel_count,
            "alpha": channel_abs[3] / pixel_count,
        },
        "shift_probe": {
            "baseline_rate": baseline_rate,
            "best_shift": best_shift,
            "improvement": shift_improvement,
            "sample_stride": probe_stride,
        },
        "classification": classification,
        "interpretation": (
            "diagnostic residual classification; it does not change the raw "
            "fidelity score or establish importer capability"
        ),
    }


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


def similarity_score(a: Image.Image, b: Image.Image) -> float:
    """Return the canonical weighted similarity score for two same-sized images.

    This is the score used by the CLI. Keeping the calculation in a small
    callable lets calibration tests and future receipt tooling measure planted
    controls without reimplementing (or accidentally changing) the gate.
    """
    return (histogram_similarity(a, b) * 0.4) + (mean_pixel_distance(a, b) * 0.6)


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
    parser.add_argument(
        "--residual-json",
        type=Path,
        help=(
            "Write an alpha-aware residual profile with edge-band and small-shift "
            "diagnostics; this never changes the canonical score"
        ),
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
    score = similarity_score(ref, cand)
    passed = score >= args.threshold and not blank_cand
    residual = None
    if args.residual_json:
        residual_ref = load_normalized(args.reference, target, mode="RGBA")
        residual_cand = load_normalized(args.candidate, target, mode="RGBA")
        residual = residual_profile(residual_ref, residual_cand)
        args.residual_json.parent.mkdir(parents=True, exist_ok=True)
        args.residual_json.write_text(json.dumps(residual, indent=2) + "\n", encoding="utf-8")

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
            **({"residual_profile": residual} if residual is not None else {}),
        }))
    else:
        verdict = "PASS ✓" if passed else "FAIL ✗"
        print(f"{verdict}  score={score:.3f}  threshold={args.threshold}  tier={tier}")
        print(f"  histogram_similarity: {hist_sim:.3f}")
        print(f"  pixel_similarity:     {pix_sim:.3f}")
        print(f"  diagnosis: {diagnosis}")
        if residual is not None:
            print(f"  residual_classification: {residual['classification']}")
            print(f"  residual_differing_pixels: {residual['differing_pixels']}")
        if not passed:
            print(f"  ref dominant colors:  {dom_ref}")
            print(f"  cand dominant colors: {dom_cand}")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
