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
    analysis.json   metrics, keyframes, diff catalogue, version stamp
    summary.md      agent-facing summary
    diff/*.png      pairwise diff heatmaps
    keyframes.png   horizontal sprite of selected keyframes

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
    md.append(f"- schema_version: {report.schema_version}")
    md.append(f"- frames: {len(report.frames)}")
    md.append(f"- pairs: {len(report.pairs)}")
    md.append(f"- keyframes: {len(report.keyframes)}\n")

    md.append("## Summary metrics\n")
    s = report.summary
    md.append(f"- mean SSIM (consecutive pairs): {s.get('mean_ssim', 0):.4f}")
    md.append(f"- min SSIM (consecutive pairs):  {s.get('min_ssim', 0):.4f}")
    md.append(f"- mean pixel diff: {s.get('mean_pixel_diff', 0):.4f}")
    md.append(f"- max pixel diff: {s.get('max_pixel_diff', 0):.4f}\n")

    md.append("## Keyframes\n")
    for k in report.keyframes:
        md.append(f"- `frame_{k.index:04d}` — {k.role}")
    md.append("")

    md.append("## Diff catalogue\n")
    for p in report.pairs:
        if p.diff_png_path:
            md.append(
                f"- pair `{p.from_index:04d}→{p.to_index:04d}`: "
                f"ssim={p.ssim:.4f} mean_diff={p.pixel_diff_mean:.4f} "
                f"max_diff={p.pixel_diff_max:.4f} ([diff]({p.diff_png_path}))"
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

    summary = {
        "mean_ssim": (sum(p.ssim for p in pairs) / len(pairs)) if pairs else 1.0,
        "min_ssim": min((p.ssim for p in pairs), default=1.0),
        "mean_pixel_diff": (
            sum(p.pixel_diff_mean for p in pairs) / len(pairs)
        ) if pairs else 0.0,
        "max_pixel_diff": max((p.pixel_diff_max for p in pairs), default=0.0),
    }
    report = Report(
        schema_version=REPORT_SCHEMA_VERSION,
        frames=frames, pairs=pairs, keyframes=keyframes,
        sprite_path=sprite_path, summary=summary,
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
    args = parser.parse_args(argv)
    return analyze(
        frames_dir=args.frames_dir,
        output_dir=args.output,
        pattern=args.pattern,
        keyframes_top=args.keyframes_top,
        max_diff_frames=args.max_diff_frames,
    )


if __name__ == "__main__":
    sys.exit(main())
