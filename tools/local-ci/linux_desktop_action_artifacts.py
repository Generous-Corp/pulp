"""Linux desktop action manifest artifact attachment helpers."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path


def attach_linux_before_diff_artifacts(
    manifest: dict,
    *,
    capture_before: bool,
    before_screenshot_path: Path,
    screenshot_path: Path,
    diff_screenshot_path: Path,
    image_change_summary_fn: Callable[..., dict],
) -> None:
    if not (capture_before and before_screenshot_path.exists() and screenshot_path.exists()):
        return
    manifest["artifacts"]["before_screenshot"] = str(before_screenshot_path)
    manifest["artifacts"]["image_change"] = image_change_summary_fn(
        before_screenshot_path,
        screenshot_path,
        diff_output_path=diff_screenshot_path,
    )
    if diff_screenshot_path.exists():
        manifest["artifacts"]["diff_screenshot"] = str(diff_screenshot_path)


def attach_linux_ui_snapshot(
    manifest: dict,
    *,
    capture_ui_snapshot: bool,
    ui_snapshot_path: Path,
    view_tree_inspector_summary_fn: Callable[[dict], dict],
) -> None:
    if not (capture_ui_snapshot and ui_snapshot_path.exists()):
        return
    view_tree = json.loads(ui_snapshot_path.read_text())
    manifest["artifacts"]["ui_snapshot"] = str(ui_snapshot_path)
    manifest["inspector"] = view_tree_inspector_summary_fn(view_tree)
