"""macOS desktop action post-launch capture and interaction flow."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path

from macos_desktop_action_interaction import macos_desktop_event_interaction_summary


def _load_view_tree_capture(
    *,
    ui_snapshot_path: Path,
    timeout_secs: float,
    content_size: tuple[float, float],
    wait_for_path_fn: Callable[[Path, float], None],
    content_size_from_view_tree_fn: Callable[[dict, tuple[float, float]], tuple[float, float]],
    view_tree_inspector_summary_fn: Callable[[dict], dict],
) -> dict:
    wait_for_path_fn(ui_snapshot_path, timeout_secs)
    view_tree = json.loads(ui_snapshot_path.read_text())
    return {
        "view_tree": view_tree,
        "content_size": content_size_from_view_tree_fn(view_tree, content_size),
        "inspector_summary": view_tree_inspector_summary_fn(view_tree),
    }


def complete_macos_desktop_action_capture(
    *,
    window: dict,
    pid: int | None,
    bundle_id: str | None,
    launch_descriptor: dict,
    capture_ui_snapshot: bool,
    use_pulp_app_automation: bool,
    interaction_requested: bool,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    screenshot_path: Path,
    before_screenshot_path: Path,
    ui_snapshot_path: Path,
    content_size_from_window_fn: Callable[[dict], tuple[float, float]],
    wait_for_path_fn: Callable[[Path, float], None],
    content_size_from_view_tree_fn: Callable[[dict, tuple[float, float]], tuple[float, float]],
    view_tree_inspector_summary_fn: Callable[[dict], dict],
    pulp_app_interaction_summary_fn: Callable[..., dict],
    capture_macos_window_fn: Callable[[int, Path], None],
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
    resolve_view_tree_click_point_fn: Callable[..., tuple[float, float]],
    screen_point_for_content_point_fn: Callable[[dict, tuple[float, float], tuple[float, float]], tuple[float, float]],
    activate_macos_pid_fn: Callable[[int], dict],
    dispatch_macos_click_fn: Callable[[float, float], dict],
    desktop_click_selector_fn: Callable[..., dict],
    wait_for_macos_bundle_window_fn: Callable[[str, float], tuple[int, dict]],
    sleep_fn: Callable[[float], None],
) -> dict:
    inspector_summary = None
    interaction_summary = None
    view_tree = None
    content_size = content_size_from_window_fn(window)

    if capture_ui_snapshot and not use_pulp_app_automation:
        snapshot = _load_view_tree_capture(
            ui_snapshot_path=ui_snapshot_path,
            timeout_secs=timeout_secs,
            content_size=content_size,
            wait_for_path_fn=wait_for_path_fn,
            content_size_from_view_tree_fn=content_size_from_view_tree_fn,
            view_tree_inspector_summary_fn=view_tree_inspector_summary_fn,
        )
        view_tree = snapshot["view_tree"]
        content_size = snapshot["content_size"]
        inspector_summary = snapshot["inspector_summary"]

    if use_pulp_app_automation:
        if capture_before:
            wait_for_path_fn(before_screenshot_path, timeout_secs)
        wait_for_path_fn(screenshot_path, timeout_secs)
        if capture_ui_snapshot:
            snapshot = _load_view_tree_capture(
                ui_snapshot_path=ui_snapshot_path,
                timeout_secs=timeout_secs,
                content_size=content_size,
                wait_for_path_fn=wait_for_path_fn,
                content_size_from_view_tree_fn=content_size_from_view_tree_fn,
                view_tree_inspector_summary_fn=view_tree_inspector_summary_fn,
            )
            view_tree = snapshot["view_tree"]
            content_size = snapshot["content_size"]
            inspector_summary = snapshot["inspector_summary"]
        interaction_summary = pulp_app_interaction_summary_fn(
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
        )
    else:
        if interaction_requested and capture_before:
            capture_macos_window_fn(int(window["windowId"]), before_screenshot_path)

        if interaction_requested:
            interaction_summary = macos_desktop_event_interaction_summary(
                window=window,
                content_size=content_size,
                pid=pid,
                click_point=click_point,
                click_view_id=click_view_id,
                click_view_type=click_view_type,
                click_view_text=click_view_text,
                click_view_label=click_view_label,
                view_tree=view_tree,
                parse_coordinate_pair_fn=parse_coordinate_pair_fn,
                resolve_view_tree_click_point_fn=resolve_view_tree_click_point_fn,
                screen_point_for_content_point_fn=screen_point_for_content_point_fn,
                activate_macos_pid_fn=activate_macos_pid_fn,
                dispatch_macos_click_fn=dispatch_macos_click_fn,
                desktop_click_selector_fn=desktop_click_selector_fn,
            )
            if settle_secs > 0:
                sleep_fn(settle_secs)

        try:
            capture_macos_window_fn(int(window["windowId"]), screenshot_path)
        except RuntimeError:
            active_bundle_id = bundle_id or launch_descriptor.get("bundle_id")
            if not active_bundle_id:
                raise
            pid, window = wait_for_macos_bundle_window_fn(active_bundle_id, min(timeout_secs, 2.0))
            capture_macos_window_fn(int(window["windowId"]), screenshot_path)

    return {
        "pid": pid,
        "window": window,
        "content_size": content_size,
        "inspector_summary": inspector_summary,
        "interaction_summary": interaction_summary,
    }
