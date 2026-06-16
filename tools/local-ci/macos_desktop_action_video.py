"""Video-orchestration helpers for the macOS desktop smoke/action flow.

Small helpers used by run_macos_local_smoke when recording a video proof:
component focus + action-marker summaries (for the Remotion overlay), generated-
REAPER recipe validation, log-text waiting, and pid liveness/termination.
"""

from __future__ import annotations

from collections.abc import Callable
import os
from pathlib import Path
import signal
import time


def _component_focus_summary(
    *,
    video_template: str | None,
    interaction_summary: dict | None,
    content_size: tuple[float, float],
) -> dict | None:
    if video_template != "component-zoom" or not interaction_summary:
        return None
    click = interaction_summary.get("click") if isinstance(interaction_summary.get("click"), dict) else {}
    selector = click.get("selector") if isinstance(click.get("selector"), dict) else {}
    content_point = click.get("content_point") if isinstance(click.get("content_point"), dict) else None
    focus: dict = {
        "kind": "component",
        "selector": selector,
    }
    label = (
        selector.get("click_view_id")
        or selector.get("id")
        or selector.get("click_view_label")
        or selector.get("label")
        or selector.get("click_view_text")
        or selector.get("text")
        or selector.get("click_view_type")
        or selector.get("type")
    )
    if label:
        focus["label"] = str(label)
    if content_point:
        width, height = content_size
        x = float(content_point.get("x", 0.0))
        y = float(content_point.get("y", 0.0))
        focus["content_point"] = {"x": x, "y": y}
        if width > 0 and height > 0:
            focus["normalized_center"] = {
                "x": max(0.0, min(1.0, x / width)),
                "y": max(0.0, min(1.0, y / height)),
            }
            focus["normalized_size"] = {"width": 0.26, "height": 0.24}
    return focus


def _action_marker_summary(
    *,
    interaction_summary: dict | None,
    content_size: tuple[float, float],
) -> dict | None:
    if not interaction_summary:
        return None
    click = interaction_summary.get("click") if isinstance(interaction_summary.get("click"), dict) else {}
    content_point = click.get("content_point") if isinstance(click.get("content_point"), dict) else None
    if not content_point:
        return None
    width, height = content_size
    x = float(content_point.get("x", 0.0))
    y = float(content_point.get("y", 0.0))
    marker = {
        "kind": "click",
        "content_point": {"x": x, "y": y},
    }
    selector = click.get("selector") if isinstance(click.get("selector"), dict) else {}
    label = (
        selector.get("click_view_id")
        or selector.get("id")
        or selector.get("click_view_label")
        or selector.get("label")
        or selector.get("click_view_text")
        or selector.get("text")
        or selector.get("click_view_type")
        or selector.get("type")
    )
    if label:
        marker["label"] = str(label)
    if width > 0 and height > 0:
        marker["normalized_point"] = {
            "x": max(0.0, min(1.0, x / width)),
            "y": max(0.0, min(1.0, y / height)),
        }
    return marker


def _validate_generated_reaper_recipe_status(video_context: dict | None, log_path: Path) -> None:
    if not video_context or video_context.get("reaper_recipe") != "generated":
        return
    try:
        text = log_path.read_text()
    except OSError as exc:
        raise RuntimeError("Generated REAPER proof recipe did not write a status log.") from exc
    if "plugin not found" in text:
        raise RuntimeError("Generated REAPER proof recipe did not find the requested plugin.")
    if "fx name ok=true" not in text:
        raise RuntimeError("Generated REAPER proof recipe did not confirm that the plugin loaded.")
    if "TrackFX_Show floating-editor mode=3" not in text:
        raise RuntimeError("Generated REAPER proof recipe did not confirm that the floating plugin editor was requested.")


def _wait_for_log_text(log_path: Path, needle: str, timeout_secs: float, *, sleep_fn: Callable[[float], None]) -> None:
    deadline = time.monotonic() + timeout_secs
    last_error = ""
    while time.monotonic() < deadline:
        try:
            if needle in log_path.read_text(errors="replace"):
                return
        except OSError as exc:
            last_error = str(exc)
        sleep_fn(0.2)
    raise RuntimeError(last_error or f"timed out waiting for `{needle}` in `{log_path}`")


def _should_capture_generated_reaper_secondary_window(video_context: dict | None) -> bool:
    return bool(video_context and video_context.get("reaper_recipe") == "generated")


def _pid_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _terminate_pid(pid: int, *, sleep_fn: Callable[[float], None]) -> None:
    if pid <= 0:
        return
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    except PermissionError:
        return
    for _ in range(10):
        if not _pid_exists(pid):
            return
        sleep_fn(0.2)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        return
    except PermissionError:
        return

