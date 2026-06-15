"""Desktop action artifact path layout helpers."""

from __future__ import annotations

from pathlib import Path


def desktop_action_artifact_paths(bundle_dir: Path, output_path: str | None = None) -> dict[str, Path]:
    return {
        "screenshot": Path(output_path).expanduser() if output_path else bundle_dir / "screenshots" / "window.png",
        "before_screenshot": bundle_dir / "screenshots" / "before.png",
        "diff_screenshot": bundle_dir / "screenshots" / "diff.png",
        "ui_snapshot": bundle_dir / "ui-tree.json",
        "stdout": bundle_dir / "stdout.log",
        "stderr": bundle_dir / "stderr.log",
    }
