#!/usr/bin/env python3
"""Pulp Motion visual-analysis pipeline.

Given a directory of sequential PNG frames (`frame_0000.png`,
`frame_0001.png`, …), compute per-frame metrics, select keyframes,
generate pairwise pixel-diff heatmaps and a keyframe sprite, and write
a structured JSON + Markdown report.

Usage:
    python3 -m tools.motion.visual.analyze_sequence \\
        --frames-dir ./frames \\
        --output     ./report

CLI flags:
    --frames-dir DIR    Directory of sequential PNG frames (required).
    --output     DIR    Output directory for the report (required).
    --pattern    GLOB   Frame filename glob (default `frame_*.png`).
    --keyframes  N      Number of top-delta keyframes to include in the
                        sprite, in addition to first/mid/last (default 2).
    --max-diff-frames N Cap on how many pairwise diff PNGs to emit; the
                        N pairs with the largest SSIM drop are kept
                        (default 8). Use 0 for unlimited.

Outputs (under --output):
    analysis.json        metrics, keyframes, diff catalogue, version stamp
    summary.md           agent-facing summary
    diff/*.png           pairwise diff heatmaps
    keyframes.png        horizontal sprite of selected keyframes
    grid/*.png           per-frame grid overlay (with --grid)
    diff_grid/*.png      per-diff grid overlay (with --grid)
    keyframes_grid.png   grid-overlaid sprite (with --grid)

Exit codes:
    0  Analysis succeeded.
    2  Frames directory missing / empty.
    3  Dependency missing or load failed (numpy / Pillow /
       scikit-image). Treats as ctest SKIP_RETURN_CODE.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import List, Optional, Tuple

# Schema version for the on-disk report. Bump on breaking changes;
# downstream tools (Phase 5 assertion CLI) read this and refuse
# unknown versions.
REPORT_SCHEMA_VERSION = 1

# Column-label alphabet for the grid overlay (A..Z). Rows are letters,
# columns are 1-based integers; cells render as e.g. "B7", "D12".
_GRID_ROW_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"


# ── Dependency import (graceful skip) ────────────────────────────────

def _try_import_deps() -> Optional[Tuple[object, object, object]]:
    """Import numpy + PIL + scikit-image. Return None on failure so the
    caller can exit 3 (ctest skip) rather than crash."""
    try:
        import numpy as np  # noqa: F401
        from PIL import Image  # noqa: F401
        from skimage.metrics import structural_similarity  # noqa: F401
        return np, Image, structural_similarity
    except Exception as e:
        print(
            f"pulp-motion-visual: missing dependency ({e}); "
            "install with `pip install -r tools/motion/visual/requirements.txt`",
            file=sys.stderr,
        )
        return None


# ── Data classes ─────────────────────────────────────────────────────

@dataclass
class FrameInfo:
    index: int
    path: str
    width: int
    height: int
    mean_brightness: float  # 0..1


@dataclass
class PairMetrics:
    from_index: int
    to_index: int
    ssim: float            # 1.0 = identical, lower = more different
    pixel_diff_mean: float # 0..1, mean abs diff
    pixel_diff_max: float  # 0..1, max abs diff
    diff_png_path: Optional[str] = None
    # Confidence score 0..1 — see compute_confidence() for the
    # heuristic. < 0.7 means the analyzer is unsure and the caller
    # should escalate (more pairs, runtime trace, etc.).
    confidence: float = 1.0


@dataclass
class KeyframeInfo:
    index: int
    role: str   # "first" | "mid" | "last" | "top-delta"
    path: str


@dataclass
class Report:
    schema_version: int
    frames: List[FrameInfo]
    pairs: List[PairMetrics]
    keyframes: List[KeyframeInfo]
    sprite_path: Optional[str]
    summary: dict
    affine_first_to_last: Optional[dict] = None


# ── Helpers ──────────────────────────────────────────────────────────

def list_frames(frames_dir: Path, pattern: str) -> List[Path]:
    return sorted(frames_dir.glob(pattern))


def load_frame_array(path: Path, np_mod, Image):
    """Load a frame as an H×W×3 uint8 numpy array (RGB)."""
    img = Image.open(str(path)).convert("RGB")
    return np_mod.asarray(img), img.size


def compute_pair_metrics(
    from_arr, to_arr, np_mod, ssim_fn
) -> Tuple[float, float, float, "np_mod.ndarray"]:
    """Returns (ssim, mean_diff, max_diff, diff_uint8_h×w)."""
    # Use luminance for SSIM (mirrors common motion-diff practice).
    luma_from = from_arr @ np_mod.array([0.299, 0.587, 0.114], dtype=np_mod.float32)
    luma_to   = to_arr   @ np_mod.array([0.299, 0.587, 0.114], dtype=np_mod.float32)
    ssim = float(ssim_fn(luma_from, luma_to, data_range=255.0))
    diff_abs = np_mod.abs(luma_from - luma_to)
    mean_diff = float(diff_abs.mean()) / 255.0
    max_diff = float(diff_abs.max()) / 255.0
    diff_uint8 = np_mod.clip(diff_abs * 3.0, 0, 255).astype(np_mod.uint8)
    return ssim, mean_diff, max_diff, diff_uint8


def write_diff_heatmap(diff_uint8, dest_path: Path, np_mod, Image) -> None:
    """Write a grayscale diff heatmap. Larger differences → brighter."""
    img = Image.fromarray(diff_uint8, mode="L")
    img.save(str(dest_path), format="PNG")


# ── Grid overlay ─────────────────────────────────────────────────────

def _row_label(row_idx: int) -> str:
    """Map a zero-based row index to an alpha label (A..Z, AA..AZ, ...)."""
    if row_idx < 0:
        return "?"
    chars = []
    n = row_idx
    while True:
        chars.append(_GRID_ROW_ALPHABET[n % 26])
        n = n // 26 - 1
        if n < 0:
            break
    return "".join(reversed(chars))


def _grid_pick_theme(img, rows: int, cols: int, theme: str) -> str:
    """Choose `light` (black text on white) vs `dark` (white text on black).

    When `theme == 'auto'`, sample the four corner cells and pick the
    theme that maximizes contrast against the underlying pixels.
    """
    if theme in ("light", "dark"):
        return theme
    w, h = img.size
    cw = max(1, w // cols)
    ch = max(1, h // rows)
    samples = []
    for (cx, cy) in (
        (0, 0), (w - cw, 0), (0, h - ch), (w - cw, h - ch),
    ):
        box = img.crop((cx, cy, cx + cw, cy + ch)).convert("L")
        px = box.getdata()
        if not px:
            continue
        samples.append(sum(px) / len(px))
    if not samples:
        return "dark"
    mean = sum(samples) / len(samples)
    # If corners are bright (mean > 128) use dark text on a light wash.
    return "light" if mean > 128 else "dark"


def _grid_load_font(size: int):
    """Best-effort load of a small bitmap-or-TTF font for grid labels.

    Falls back to PIL's built-in bitmap font so the overlay always
    renders even on hosts without TrueType fonts available.
    """
    from PIL import ImageFont
    candidates = [
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/SFNSMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
    ]
    for p in candidates:
        try:
            return ImageFont.truetype(p, size=size)
        except Exception:
            continue
    return ImageFont.load_default()


def render_grid_overlay(
    src_path: Path,
    dest_path: Path,
    Image,
    *,
    rows: int,
    cols: int,
    theme: str,
) -> None:
    """Draw a `rows x cols` overlay of cell labels (e.g. `B7`) on top
    of `src_path` and write the composite to `dest_path`.

    The implementation is intentionally tiny — Pillow primitives only,
    no third-party glyph atlas, no submodule. Larger image dimensions
    auto-scale the font size; tiny frames fall back to a 1-pixel grid.
    """
    from PIL import ImageDraw
    base = Image.open(str(src_path)).convert("RGB")
    w, h = base.size
    rows = max(1, min(rows, 26))
    cols = max(1, cols)
    chosen = _grid_pick_theme(base, rows, cols, theme)
    if chosen == "light":
        line_rgba = (0, 0, 0, 160)
        text_fg = (0, 0, 0, 255)
        text_bg = (255, 255, 255, 180)
    else:
        line_rgba = (255, 255, 255, 160)
        text_fg = (255, 255, 255, 255)
        text_bg = (0, 0, 0, 180)

    overlay = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    cell_w = w / cols
    cell_h = h / rows
    # Grid lines.
    for c in range(1, cols):
        x = int(round(c * cell_w))
        draw.line([(x, 0), (x, h - 1)], fill=line_rgba, width=1)
    for r in range(1, rows):
        y = int(round(r * cell_h))
        draw.line([(0, y), (w - 1, y)], fill=line_rgba, width=1)
    # Cell labels (anchored top-left with a small background chip so
    # they remain readable over any underlying pixels).
    font_size = max(8, int(min(cell_w, cell_h) * 0.32))
    font = _grid_load_font(font_size)
    pad = max(1, font_size // 4)
    for r in range(rows):
        for c in range(cols):
            label = f"{_row_label(r)}{c + 1}"
            x0 = int(round(c * cell_w)) + pad
            y0 = int(round(r * cell_h)) + pad
            try:
                tb = draw.textbbox((x0, y0), label, font=font)
            except Exception:
                # Older Pillow without textbbox — fall back to textsize.
                tw, th = draw.textsize(label, font=font)
                tb = (x0, y0, x0 + tw, y0 + th)
            draw.rectangle(
                (tb[0] - 1, tb[1] - 1, tb[2] + 1, tb[3] + 1),
                fill=text_bg,
            )
            draw.text((x0, y0), label, fill=text_fg, font=font)

    composite = Image.alpha_composite(base.convert("RGBA"), overlay)
    composite.convert("RGB").save(str(dest_path), format="PNG")


def make_keyframe_sprite(
    keyframe_paths: List[Path], dest_path: Path, Image
) -> None:
    """Compose a horizontal sprite of the selected keyframes."""
    if not keyframe_paths:
        return
    imgs = [Image.open(str(p)).convert("RGB") for p in keyframe_paths]
    w = max(im.size[0] for im in imgs)
    h = max(im.size[1] for im in imgs)
    n = len(imgs)
    sprite = Image.new("RGB", (w * n, h), color=(0, 0, 0))
    for i, im in enumerate(imgs):
        sprite.paste(im, (i * w, 0))
    sprite.save(str(dest_path), format="PNG")


def compute_confidence(
    pairs: List[PairMetrics], affine_block: Optional[dict] = None,
) -> None:
    """In-place assign `confidence ∈ [0..1]` to each pair.

    Heuristic (cheap, deterministic, no extra deps):

    * **SSIM band** — pairs in the moderately-changing band
      (0.6 ≤ ssim < 0.99) get the highest base score. Near-identical
      pairs (ssim ≥ 0.99) are confident-but-uninteresting; pairs
      below 0.3 hint at noise / scene cut and lose confidence.
    * **Local agreement** — a pair whose SSIM agrees with its
      neighbours within 0.1 keeps full credit; outliers lose some.
    * **Affine consistency** — when an affine block is available,
      pairs whose mean diff is roughly consistent with the overall
      translation magnitude get a small bonus; pairs that disagree
      lose a small amount.
    """
    if not pairs:
        return

    def base_band(ssim: float) -> float:
        if ssim >= 0.99:
            return 0.85   # almost identical — confident but boring
        if ssim >= 0.6:
            return 0.95   # clean motion signal
        if ssim >= 0.3:
            return 0.7    # noisy
        return 0.45       # scene cut / heavy noise — escalate

    expected_diff: Optional[float] = None
    if affine_block:
        tr = affine_block.get("translation") or {}
        mag = (float(tr.get("dx", 0.0)) ** 2
               + float(tr.get("dy", 0.0)) ** 2) ** 0.5
        if mag > 0 and len(pairs) > 0:
            expected_diff = mag / max(1, len(pairs))

    for i, p in enumerate(pairs):
        score = base_band(p.ssim)
        # Neighbour agreement (compare to the average of immediate neighbours).
        neighbours = []
        if i > 0:
            neighbours.append(pairs[i - 1].ssim)
        if i + 1 < len(pairs):
            neighbours.append(pairs[i + 1].ssim)
        if neighbours:
            avg = sum(neighbours) / len(neighbours)
            disagreement = abs(p.ssim - avg)
            if disagreement < 0.1:
                score += 0.02
            elif disagreement > 0.3:
                score -= 0.15
        if expected_diff is not None and expected_diff > 0:
            # Normalise the per-pair mean diff into a pixel-space
            # proxy and compare against the affine estimate.
            proxy = p.pixel_diff_mean * 255.0
            ratio = (proxy + 1.0) / (expected_diff + 1.0)
            if 0.5 <= ratio <= 2.0:
                score += 0.03
            else:
                score -= 0.05
        p.confidence = max(0.0, min(1.0, score))


def estimate_affine_first_to_last(
    first_arr, last_arr, np_mod, Image,
) -> dict:
    """Estimate the affine transform that maps the first keyframe onto
    the last. Uses OpenCV's `estimateAffinePartial2D` when available
    (rotation + uniform scale + translation); falls back to a PIL /
    numpy cross-correlation that only recovers translation when cv2
    is not installed.

    Returns a dict with translation / rotation_deg / scale_ratio /
    opacity_delta / method. Rotation + scale are 0 / 1.0 in the
    fallback (we explicitly document this as a translation-only mode).
    """
    luma_first = (
        first_arr @ np_mod.array([0.299, 0.587, 0.114], dtype=np_mod.float32)
    )
    luma_last = (
        last_arr @ np_mod.array([0.299, 0.587, 0.114], dtype=np_mod.float32)
    )
    opacity_delta = (
        float(luma_last.mean()) - float(luma_first.mean())
    ) / 255.0

    # ── OpenCV path ────────────────────────────────────────────────
    try:
        import cv2  # type: ignore
    except Exception:
        cv2 = None

    if cv2 is not None:
        gray_first = luma_first.astype("uint8")
        gray_last = luma_last.astype("uint8")
        try:
            orb = cv2.ORB_create(nfeatures=500)
            kp1, des1 = orb.detectAndCompute(gray_first, None)
            kp2, des2 = orb.detectAndCompute(gray_last, None)
            if des1 is not None and des2 is not None and len(kp1) >= 3 and len(kp2) >= 3:
                bf = cv2.BFMatcher(cv2.NORM_HAMMING, crossCheck=True)
                matches = sorted(bf.match(des1, des2), key=lambda m: m.distance)
                matches = matches[: max(10, len(matches) // 2)]
                if len(matches) >= 3:
                    src = np_mod.float32(
                        [kp1[m.queryIdx].pt for m in matches]
                    ).reshape(-1, 1, 2)
                    dst = np_mod.float32(
                        [kp2[m.trainIdx].pt for m in matches]
                    ).reshape(-1, 1, 2)
                    M, _ = cv2.estimateAffinePartial2D(src, dst)
                    if M is not None:
                        dx = float(M[0, 2])
                        dy = float(M[1, 2])
                        rot = float(
                            np_mod.degrees(np_mod.arctan2(M[1, 0], M[0, 0]))
                        )
                        scale = float(
                            (M[0, 0] ** 2 + M[1, 0] ** 2) ** 0.5
                        )
                        return {
                            "translation": {"dx": dx, "dy": dy},
                            "rotation_deg": rot,
                            "scale_ratio": scale,
                            "opacity_delta": opacity_delta,
                            "method": "opencv",
                        }
        except Exception:
            # Fall through to the PIL fallback if the OpenCV path
            # blows up on a malformed pair.
            pass

    # ── PIL fallback: translation only via FFT phase correlation ──
    # We use numpy's FFT to estimate the integer pixel shift that
    # best aligns the two luminance images. Rotation + scale are
    # explicitly 0 / 1.0 — callers see method="pil-fallback" and
    # know not to read more into the numbers than translation.
    h = min(luma_first.shape[0], luma_last.shape[0])
    w = min(luma_first.shape[1], luma_last.shape[1])
    a = luma_first[:h, :w] - float(luma_first.mean())
    b = luma_last[:h, :w] - float(luma_last.mean())
    fa = np_mod.fft.fft2(a)
    fb = np_mod.fft.fft2(b)
    cross = fa * np_mod.conj(fb)
    denom = np_mod.abs(cross)
    denom[denom == 0] = 1.0
    r = np_mod.fft.ifft2(cross / denom).real
    peak = np_mod.unravel_index(np_mod.argmax(r), r.shape)
    dy = int(peak[0])
    dx = int(peak[1])
    if dy > h // 2:
        dy -= h
    if dx > w // 2:
        dx -= w
    return {
        "translation": {"dx": float(dx), "dy": float(dy)},
        "rotation_deg": 0.0,
        "scale_ratio": 1.0,
        "opacity_delta": opacity_delta,
        "method": "pil-fallback",
    }


def detect_motion_window(
    pairs: List[PairMetrics], threshold: float,
) -> Tuple[int, int]:
    """Return `(lead, trail)` — counts of frames to drop from each end.

    A pair is "idle" when its `pixel_diff_mean` is below `threshold`.
    The motion window starts at the first non-idle pair and ends at
    the last non-idle pair; the kept frame range therefore spans
    `[first_active.from_index, last_active.to_index]`.

    With no pairs (single frame) nothing is trimmed.
    """
    if not pairs:
        return 0, 0
    n_frames = pairs[-1].to_index + 1
    first_active = next(
        (p for p in pairs if p.pixel_diff_mean > threshold), None,
    )
    last_active = next(
        (p for p in reversed(pairs) if p.pixel_diff_mean > threshold), None,
    )
    if first_active is None or last_active is None:
        # Sequence is entirely idle — nothing to analyse meaningfully.
        return 0, 0
    lead = first_active.from_index
    trail = (n_frames - 1) - last_active.to_index
    return max(0, lead), max(0, trail)


def select_keyframes(
    frames: List[FrameInfo],
    pairs: List[PairMetrics],
    top_delta: int,
) -> List[KeyframeInfo]:
    """Pick first, mid, last, plus the `top_delta` highest-delta frames."""
    if not frames:
        return []
    chosen: dict[int, str] = {}
    chosen[frames[0].index] = "first"
    chosen[frames[-1].index] = "last"
    if len(frames) > 2:
        chosen.setdefault(frames[len(frames) // 2].index, "mid")
    # rank pairs by lowest SSIM (== largest change) and pick their `to` index.
    by_change = sorted(pairs, key=lambda p: p.ssim)
    for p in by_change[:top_delta]:
        chosen.setdefault(p.to_index, "top-delta")
    out: List[KeyframeInfo] = []
    by_idx = {f.index: f for f in frames}
    for idx in sorted(chosen.keys()):
        f = by_idx[idx]
        out.append(KeyframeInfo(index=idx, role=chosen[idx], path=f.path))
    return out


def write_summary_md(report: Report, output_dir: Path) -> None:
    md = []
    md.append("# Motion visual analysis\n")
    md.append("> **Claim-evidence contract.** Every claim in this report cites "
              "`pair NN→NN+1`, the artifact used (`frames/`, `diff/`, `grid/`, "
              "`diff_grid/`, `keyframes.png`, `affine_first_to_last`), and a "
              "confidence score `0.0..1.0`. **Confidence < 0.7 means the "
              "analyzer is unsure** — escalate to `--max-diff-frames 0` "
              "(all pairs), longer capture window, or fall back to a runtime "
              "trace if instrumentation is available.\n")
    md.append(f"- schema_version: {report.schema_version}")
    md.append(f"- frames: {len(report.frames)}")
    md.append(f"- pairs: {len(report.pairs)}")
    md.append(f"- keyframes: {len(report.keyframes)}\n")

    md.append("## Summary metrics\n")
    s = report.summary
    md.append(f"- mean SSIM (consecutive pairs): {s.get('mean_ssim', 0):.4f}")
    md.append(f"- min SSIM (consecutive pairs):  {s.get('min_ssim', 0):.4f}")
    md.append(f"- mean pixel diff: {s.get('mean_pixel_diff', 0):.4f}")
    md.append(f"- max pixel diff: {s.get('max_pixel_diff', 0):.4f}")
    if s.get("trimmed_leading_frames") or s.get("trimmed_trailing_frames"):
        md.append(
            f"- trimmed: {s.get('trimmed_leading_frames', 0)} leading, "
            f"{s.get('trimmed_trailing_frames', 0)} trailing"
        )
    md.append("")

    if report.affine_first_to_last:
        a = report.affine_first_to_last
        tr = a.get("translation") or {}
        md.append("## Net motion\n")
        md.append(
            f"- translation: dx={tr.get('dx', 0):.1f}px, "
            f"dy={tr.get('dy', 0):.1f}px"
        )
        md.append(f"- rotation: {a.get('rotation_deg', 0):.2f} deg")
        md.append(f"- scale: {a.get('scale_ratio', 1.0):.3f}")
        md.append(f"- opacity delta: {a.get('opacity_delta', 0.0):.4f}")
        md.append(f"- method: `{a.get('method', 'unknown')}`\n")

    md.append("## Keyframes\n")
    for k in report.keyframes:
        md.append(f"- `frame_{k.index:04d}` — {k.role}")
    md.append("")

    md.append("## Diff catalogue\n")
    md.append("Each row cites: pair, artifact (`diff/<file>`), and "
              "confidence. Confidence < 0.7 → escalate.\n")
    for p in report.pairs:
        if p.diff_png_path:
            md.append(
                f"- pair `{p.from_index:04d}→{p.to_index:04d}`: "
                f"ssim={p.ssim:.4f} mean_diff={p.pixel_diff_mean:.4f} "
                f"max_diff={p.pixel_diff_max:.4f} "
                f"confidence={p.confidence:.2f} "
                f"([diff]({p.diff_png_path}))"
            )

    low = [p for p in report.pairs if p.confidence < 0.7]
    if low:
        md.append("")
        md.append("## Low-confidence pairs (escalate)\n")
        for p in low:
            md.append(
                f"- pair `{p.from_index:04d}→{p.to_index:04d}` "
                f"confidence={p.confidence:.2f} — re-run with "
                "`--max-diff-frames 0` or capture a runtime trace."
            )

    (output_dir / "summary.md").write_text("\n".join(md) + "\n", encoding="utf-8")


def write_json(report: Report, output_dir: Path) -> None:
    payload = {
        "schema_version": report.schema_version,
        "frames": [asdict(f) for f in report.frames],
        "pairs": [asdict(p) for p in report.pairs],
        "keyframes": [asdict(k) for k in report.keyframes],
        "sprite_path": report.sprite_path,
        "summary": report.summary,
        "affine_first_to_last": report.affine_first_to_last,
    }
    (output_dir / "analysis.json").write_text(
        json.dumps(payload, indent=2, sort_keys=True), encoding="utf-8"
    )


# ── Main analysis ────────────────────────────────────────────────────

def analyze(
    frames_dir: Path,
    output_dir: Path,
    pattern: str = "frame_*.png",
    keyframes_top: int = 2,
    max_diff_frames: int = 8,
    *,
    grid: bool = False,
    grid_rows: int = 8,
    grid_cols: int = 12,
    grid_theme: str = "auto",
    trim: bool = False,
    trim_threshold: float = 0.01,
    affine: bool = False,
) -> int:
    deps = _try_import_deps()
    if deps is None:
        return 3
    np_mod, Image, ssim_fn = deps

    frame_paths = list_frames(frames_dir, pattern)
    if not frame_paths:
        print(
            f"pulp-motion-visual: no frames matched `{pattern}` in {frames_dir}",
            file=sys.stderr,
        )
        return 2

    output_dir.mkdir(parents=True, exist_ok=True)
    diff_dir = output_dir / "diff"
    diff_dir.mkdir(exist_ok=True)

    # Load all frames + compute per-frame metrics.
    arrays = []
    frames: List[FrameInfo] = []
    for i, path in enumerate(frame_paths):
        arr, (w, h) = load_frame_array(path, np_mod, Image)
        arrays.append(arr)
        mean = float(arr.mean()) / 255.0
        frames.append(FrameInfo(index=i, path=str(path),
                                width=w, height=h, mean_brightness=mean))

    # Compute pairwise metrics.
    pairs: List[PairMetrics] = []
    for i in range(len(arrays) - 1):
        ssim, mean_diff, max_diff, diff_uint8 = compute_pair_metrics(
            arrays[i].astype("float32"),
            arrays[i + 1].astype("float32"),
            np_mod,
            ssim_fn,
        )
        pairs.append(PairMetrics(
            from_index=i, to_index=i + 1,
            ssim=ssim, pixel_diff_mean=mean_diff, pixel_diff_max=max_diff,
        ))

    # Optional auto-trim: drop idle prefix + suffix from the analysis
    # window. Frames stay on disk; we just narrow the in-report slice
    # so the JSON / sprite / keyframe selection reflect the actual
    # motion window. Indexes are preserved so cross-references back to
    # the source frame numbers remain meaningful.
    trimmed_lead = 0
    trimmed_trail = 0
    if trim:
        trimmed_lead, trimmed_trail = detect_motion_window(
            pairs, threshold=trim_threshold,
        )
        if trimmed_lead or trimmed_trail:
            keep_first = trimmed_lead
            keep_last = (len(frames) - 1) - trimmed_trail
            frames = [f for f in frames
                      if keep_first <= f.index <= keep_last]
            pairs = [p for p in pairs
                     if p.from_index >= keep_first and p.to_index <= keep_last]

    # Emit diff PNGs for the top-N highest-change pairs (or all if 0).
    if max_diff_frames <= 0:
        diff_targets = pairs
    else:
        diff_targets = sorted(pairs, key=lambda p: p.ssim)[:max_diff_frames]
    for p in diff_targets:
        out_path = diff_dir / f"diff_{p.from_index:04d}_{p.to_index:04d}.png"
        # Recompute the diff array (cheap; only for the chosen pairs).
        _, _, _, diff_uint8 = compute_pair_metrics(
            arrays[p.from_index].astype("float32"),
            arrays[p.to_index].astype("float32"),
            np_mod, ssim_fn,
        )
        write_diff_heatmap(diff_uint8, out_path, np_mod, Image)
        p.diff_png_path = str(out_path.relative_to(output_dir))

    # Select keyframes + emit sprite.
    keyframes = select_keyframes(frames, pairs, top_delta=keyframes_top)
    sprite_path: Optional[str] = None
    if keyframes:
        sprite_dest = output_dir / "keyframes.png"
        make_keyframe_sprite([Path(k.path) for k in keyframes], sprite_dest, Image)
        sprite_path = str(sprite_dest.relative_to(output_dir))

    # Optional grid overlay artifacts (off-by-default — opt in with --grid).
    if grid:
        grid_dir = output_dir / "grid"
        grid_dir.mkdir(exist_ok=True)
        for f in frames:
            render_grid_overlay(
                Path(f.path),
                grid_dir / f"frame_{f.index:04d}.png",
                Image,
                rows=grid_rows, cols=grid_cols, theme=grid_theme,
            )
        diff_grid_dir = output_dir / "diff_grid"
        diff_grid_dir.mkdir(exist_ok=True)
        for p in pairs:
            if not p.diff_png_path:
                continue
            src = output_dir / p.diff_png_path
            render_grid_overlay(
                src,
                diff_grid_dir / f"diff_{p.from_index:04d}_{p.to_index:04d}.png",
                Image,
                rows=grid_rows, cols=grid_cols, theme=grid_theme,
            )
        if keyframes and sprite_path:
            render_grid_overlay(
                output_dir / sprite_path,
                output_dir / "keyframes_grid.png",
                Image,
                rows=grid_rows, cols=grid_cols, theme=grid_theme,
            )

    # Optional affine first → last (uses keyframes if available so the
    # estimate reflects the analysis window, not stray idle padding).
    affine_block: Optional[dict] = None
    if affine and keyframes:
        first_arr = arrays[keyframes[0].index].astype("float32")
        last_arr = arrays[keyframes[-1].index].astype("float32")
        affine_block = estimate_affine_first_to_last(
            first_arr, last_arr, np_mod, Image,
        )

    # Confidence heuristic — assigns each pair a score in [0..1] that
    # downstream agents can cite in the claim-evidence contract.
    compute_confidence(pairs, affine_block=affine_block)

    summary = {
        "mean_ssim": (sum(p.ssim for p in pairs) / len(pairs)) if pairs else 1.0,
        "min_ssim": min((p.ssim for p in pairs), default=1.0),
        "mean_pixel_diff": (
            sum(p.pixel_diff_mean for p in pairs) / len(pairs)
        ) if pairs else 0.0,
        "max_pixel_diff": max((p.pixel_diff_max for p in pairs), default=0.0),
        "trimmed_leading_frames": trimmed_lead,
        "trimmed_trailing_frames": trimmed_trail,
        "mean_confidence": (
            sum(p.confidence for p in pairs) / len(pairs)
        ) if pairs else 1.0,
        "min_confidence": min((p.confidence for p in pairs), default=1.0),
        "low_confidence_pairs": [
            {"from_index": p.from_index, "to_index": p.to_index,
             "confidence": p.confidence}
            for p in pairs if p.confidence < 0.7
        ],
    }
    report = Report(
        schema_version=REPORT_SCHEMA_VERSION,
        frames=frames, pairs=pairs, keyframes=keyframes,
        sprite_path=sprite_path, summary=summary,
        affine_first_to_last=affine_block,
    )
    write_json(report, output_dir)
    write_summary_md(report, output_dir)
    return 0


# ── CLI ──────────────────────────────────────────────────────────────

def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        prog="pulp-motion-visual",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--frames-dir", required=True, type=Path,
                        help="Directory of sequential PNG frames")
    parser.add_argument("--output", required=True, type=Path,
                        help="Output directory for the report")
    parser.add_argument("--pattern", default="frame_*.png",
                        help="Frame filename glob (default `frame_*.png`)")
    parser.add_argument("--keyframes", type=int, default=2, dest="keyframes_top",
                        help="Top-delta keyframes in addition to first/mid/last")
    parser.add_argument("--max-diff-frames", type=int, default=8,
                        help="Cap on diff PNGs emitted; 0 = unlimited")
    parser.add_argument("--grid", action="store_true",
                        help="Also emit alphanumeric grid overlays "
                             "(grid/, diff_grid/, keyframes_grid.png)")
    parser.add_argument("--grid-rows", type=int, default=8,
                        help="Grid overlay rows (A..Z, default 8)")
    parser.add_argument("--grid-cols", type=int, default=12,
                        help="Grid overlay columns (1..N, default 12)")
    parser.add_argument("--grid-theme", default="auto",
                        choices=("auto", "light", "dark"),
                        help="Grid label/line theme (default auto)")
    parser.add_argument("--trim", action="store_true",
                        help="Drop idle prefix/suffix from the analysis "
                             "window (frames stay on disk)")
    parser.add_argument("--trim-threshold", type=float, default=0.01,
                        help="Mean-diff threshold for --trim "
                             "(0..1 luminance fraction, default 0.01)")
    parser.add_argument("--affine", action="store_true",
                        help="Estimate affine transform first→last "
                             "(opencv if installed, else PIL translation)")
    args = parser.parse_args(argv)
    return analyze(
        frames_dir=args.frames_dir,
        output_dir=args.output,
        pattern=args.pattern,
        keyframes_top=args.keyframes_top,
        max_diff_frames=args.max_diff_frames,
        grid=args.grid,
        grid_rows=args.grid_rows,
        grid_cols=args.grid_cols,
        grid_theme=args.grid_theme,
        trim=args.trim,
        trim_threshold=args.trim_threshold,
        affine=args.affine,
    )


if __name__ == "__main__":
    sys.exit(main())
