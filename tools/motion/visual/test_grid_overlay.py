#!/usr/bin/env python3
"""Self-check for the visual-plus extensions to `analyze_sequence.py`.

Covers:
  * `--grid` emits `grid/`, `diff_grid/`, and `keyframes_grid.png`.
  * `--trim` finds the motion window inside an idle-padded sequence.
  * Affine first→last estimation produces a non-null translation on
    a synthetic sliding-rectangle sequence.

Returns:
    0  → all extension paths produced the expected artifacts
    3  → Python deps missing (numpy / Pillow / scikit-image) — ctest
          SKIP_RETURN_CODE
    1  → unexpected failure
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR.parent.parent.parent) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR.parent.parent.parent))

from tools.motion.visual import analyze_sequence  # noqa: E402


def _synthesize_moving(out_dir: Path, n_motion: int = 6,
                       idle_lead: int = 0, idle_trail: int = 0) -> int:
    """Render a sequence where a colored rectangle slides right. The
    leading + trailing idle frames are pixel-identical copies of the
    first / last motion frame to exercise the auto-trim curve.

    Returns the index of the first true motion frame.
    """
    try:
        from PIL import Image, ImageDraw
    except Exception as e:
        print(f"self-check: missing Pillow ({e})", file=sys.stderr)
        sys.exit(3)
    width, height = 160, 80
    out_dir.mkdir(parents=True, exist_ok=True)

    def render(x: int) -> "Image.Image":
        img = Image.new("RGB", (width, height), color=(20, 20, 30))
        draw = ImageDraw.Draw(img)
        draw.rectangle([(x, 20), (x + 40, 60)], fill=(220, 80, 30))
        return img

    idx = 0
    first = render(0)
    for _ in range(idle_lead):
        first.save(out_dir / f"frame_{idx:04d}.png")
        idx += 1
    motion_start = idx
    for i in range(n_motion):
        render(i * 10).save(out_dir / f"frame_{idx:04d}.png")
        idx += 1
    last = render((n_motion - 1) * 10)
    for _ in range(idle_trail):
        last.save(out_dir / f"frame_{idx:04d}.png")
        idx += 1
    return motion_start


def _check_grid(td_path: Path) -> int:
    frames_dir = td_path / "grid_frames"
    _synthesize_moving(frames_dir, n_motion=4)
    output_dir = td_path / "grid_report"
    rc = analyze_sequence.analyze(
        frames_dir=frames_dir,
        output_dir=output_dir,
        pattern="frame_*.png",
        keyframes_top=1,
        max_diff_frames=2,
        grid=True,
        grid_rows=4,
        grid_cols=6,
        grid_theme="auto",
    )
    if rc == 3:
        return 3
    if rc != 0:
        print(f"grid: analyzer returned {rc}", file=sys.stderr)
        return 1
    grid_dir = output_dir / "grid"
    diff_grid_dir = output_dir / "diff_grid"
    sprite_grid = output_dir / "keyframes_grid.png"
    for required in (grid_dir, diff_grid_dir, sprite_grid):
        if not required.exists():
            print(f"grid: missing {required}", file=sys.stderr)
            return 1
    grid_pngs = list(grid_dir.glob("frame_*.png"))
    diff_grid_pngs = list(diff_grid_dir.glob("diff_*.png"))
    if len(grid_pngs) != 4:
        print(f"grid: expected 4 frame overlays, got {len(grid_pngs)}",
              file=sys.stderr)
        return 1
    if not diff_grid_pngs:
        print("grid: no diff-grid overlays emitted", file=sys.stderr)
        return 1
    if sprite_grid.stat().st_size <= 0:
        print("grid: keyframes_grid.png is empty", file=sys.stderr)
        return 1
    # Confirm row-label helper handles boundary cases.
    if analyze_sequence._row_label(0) != "A":
        print("grid: row label 0 should be 'A'", file=sys.stderr)
        return 1
    if analyze_sequence._row_label(25) != "Z":
        print("grid: row label 25 should be 'Z'", file=sys.stderr)
        return 1
    if analyze_sequence._row_label(26) != "AA":
        print("grid: row label 26 should be 'AA'", file=sys.stderr)
        return 1
    return 0


def _check_trim(td_path: Path) -> int:
    frames_dir = td_path / "trim_frames"
    motion_start = _synthesize_moving(
        frames_dir, n_motion=5, idle_lead=3, idle_trail=4,
    )
    output_dir = td_path / "trim_report"
    rc = analyze_sequence.analyze(
        frames_dir=frames_dir,
        output_dir=output_dir,
        pattern="frame_*.png",
        keyframes_top=1,
        max_diff_frames=4,
        trim=True,
        trim_threshold=0.005,
    )
    if rc == 3:
        return 3
    if rc != 0:
        print(f"trim: analyzer returned {rc}", file=sys.stderr)
        return 1
    data = json.loads((output_dir / "analysis.json").read_text(encoding="utf-8"))
    lead = data["summary"].get("trimmed_leading_frames")
    trail = data["summary"].get("trimmed_trailing_frames")
    if lead is None or trail is None:
        print("trim: summary missing trimmed_* counters", file=sys.stderr)
        return 1
    # Leading idle frames should be detected; require at least the
    # number of pure-duplicate prefixes minus one (the trim algorithm
    # keeps the first true motion pair as the boundary).
    if lead < motion_start - 1:
        print(f"trim: expected lead >= {motion_start - 1}, got {lead}",
              file=sys.stderr)
        return 1
    if trail < 3:
        print(f"trim: expected trail >= 3, got {trail}", file=sys.stderr)
        return 1
    # The kept frame range must not include the duplicate suffix.
    kept = [f["index"] for f in data["frames"]]
    if not kept:
        print("trim: kept-frame set is empty", file=sys.stderr)
        return 1
    if kept[0] >= motion_start + 1:
        # We expect the first kept frame to sit at or just before the
        # first true motion frame.
        print(f"trim: kept frames start too late at {kept[0]}", file=sys.stderr)
        return 1
    return 0


def _check_affine(td_path: Path) -> int:
    frames_dir = td_path / "affine_frames"
    _synthesize_moving(frames_dir, n_motion=6)
    output_dir = td_path / "affine_report"
    rc = analyze_sequence.analyze(
        frames_dir=frames_dir,
        output_dir=output_dir,
        pattern="frame_*.png",
        keyframes_top=1,
        max_diff_frames=2,
        affine=True,
    )
    if rc == 3:
        return 3
    if rc != 0:
        print(f"affine: analyzer returned {rc}", file=sys.stderr)
        return 1
    data = json.loads((output_dir / "analysis.json").read_text(encoding="utf-8"))
    aff = data.get("affine_first_to_last")
    if not aff:
        print("affine: missing affine_first_to_last block", file=sys.stderr)
        return 1
    tr = aff.get("translation") or {}
    if not isinstance(tr.get("dx"), (int, float)):
        print(f"affine: translation.dx not numeric: {tr}", file=sys.stderr)
        return 1
    # Rectangle slides right by 10 px per motion frame → 5 transitions
    # over the 6 motion frames ⇒ dx ≈ 50. Allow generous tolerance for
    # the PIL correlation fallback.
    if abs(tr["dx"]) < 10.0:
        print(f"affine: dx={tr['dx']} too small for known motion",
              file=sys.stderr)
        return 1
    method = aff.get("method")
    if method not in ("opencv", "pil-fallback"):
        print(f"affine: unexpected method {method}", file=sys.stderr)
        return 1
    return 0


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        for fn in (_check_grid, _check_trim, _check_affine):
            rc = fn(td_path)
            if rc == 3:
                return 3
            if rc != 0:
                return rc
    print("visual-plus self-check OK (grid, trim, affine)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
