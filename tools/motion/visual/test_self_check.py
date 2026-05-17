#!/usr/bin/env python3
"""Self-check for `tools/motion/visual/analyze_sequence.py`.

Synthesizes a small sequence of PNG frames in a temp dir, runs the
analyzer end-to-end, and asserts the expected artifacts + JSON shape.
Returns:
    0  → analysis succeeded and artifacts are well-formed
    3  → Python deps missing (numpy / Pillow / scikit-image) — ctest
          SKIP_RETURN_CODE so CI without the deps installed skips
          cleanly instead of failing
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


def synthesize_frames(out_dir: Path, n_frames: int = 6) -> None:
    """Render `n_frames` frames where a colored rectangle slides right
    by 10 px per frame. Uses Pillow directly (already a required dep)."""
    try:
        from PIL import Image, ImageDraw
    except Exception as e:
        print(f"self-check: missing Pillow ({e})", file=sys.stderr)
        sys.exit(3)
    width, height = 120, 60
    out_dir.mkdir(parents=True, exist_ok=True)
    for i in range(n_frames):
        img = Image.new("RGB", (width, height), color=(20, 20, 30))
        draw = ImageDraw.Draw(img)
        x = i * 10
        draw.rectangle([(x, 10), (x + 30, 50)], fill=(220, 80, 30))
        img.save(out_dir / f"frame_{i:04d}.png")


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        frames_dir = td_path / "frames"
        synthesize_frames(frames_dir, n_frames=6)
        output_dir = td_path / "report"

        rc = analyze_sequence.analyze(
            frames_dir=frames_dir,
            output_dir=output_dir,
            pattern="frame_*.png",
            keyframes_top=2,
            max_diff_frames=4,
        )
        if rc == 3:
            return 3
        if rc != 0:
            print(f"self-check: analyzer returned {rc}", file=sys.stderr)
            return 1

        # ── Assert artifacts exist ─────────────────────────────────
        json_path = output_dir / "analysis.json"
        md_path = output_dir / "summary.md"
        sprite_path = output_dir / "keyframes.png"
        diff_dir = output_dir / "diff"
        for required in (json_path, md_path, sprite_path, diff_dir):
            if not required.exists():
                print(f"self-check: missing artifact {required}", file=sys.stderr)
                return 1

        # ── Assert JSON shape ──────────────────────────────────────
        data = json.loads(json_path.read_text(encoding="utf-8"))
        if data["schema_version"] != analyze_sequence.REPORT_SCHEMA_VERSION:
            print("self-check: schema version mismatch", file=sys.stderr)
            return 1
        if len(data["frames"]) != 6:
            print(f"self-check: expected 6 frames, got {len(data['frames'])}",
                  file=sys.stderr)
            return 1
        if len(data["pairs"]) != 5:
            print(f"self-check: expected 5 pairs, got {len(data['pairs'])}",
                  file=sys.stderr)
            return 1
        if "mean_ssim" not in data["summary"]:
            print("self-check: missing summary.mean_ssim", file=sys.stderr)
            return 1

        # The synthetic frames have constant motion (rectangle shift),
        # so SSIM should be < 1.0 and pixel diff > 0 on every pair.
        for p in data["pairs"]:
            if p["ssim"] >= 1.0:
                print(f"self-check: pair {p} ssim should be < 1.0", file=sys.stderr)
                return 1
            if p["pixel_diff_mean"] <= 0.0:
                print(f"self-check: pair {p} mean diff should be > 0",
                      file=sys.stderr)
                return 1

        # Keyframes must include first + last at minimum.
        roles = {k["role"] for k in data["keyframes"]}
        if "first" not in roles or "last" not in roles:
            print(f"self-check: keyframes missing first/last (got {roles})",
                  file=sys.stderr)
            return 1

        # At least one diff PNG should have been emitted (we capped at 4
        # and there are 5 pairs).
        diff_pngs = list(diff_dir.glob("diff_*.png"))
        if not diff_pngs:
            print("self-check: no diff PNGs emitted", file=sys.stderr)
            return 1

        print(f"self-check OK ({len(data['frames'])} frames, "
              f"{len(data['pairs'])} pairs, "
              f"{len(data['keyframes'])} keyframes, "
              f"{len(diff_pngs)} diff PNGs)")
        return 0


if __name__ == "__main__":
    sys.exit(main())
