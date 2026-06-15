"""Linux desktop action manifest assembly."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from linux_desktop_action_artifacts import attach_linux_before_diff_artifacts, attach_linux_ui_snapshot
from linux_desktop_action_interaction import attach_linux_interaction_summary
from linux_desktop_action_metadata import attach_linux_window_metadata, read_linux_pid_file


def build_linux_desktop_action_manifest(
    *,
    target_name: str,
    target: dict,
    command: str,
    launch_command: str,
    host: str,
    repo_path: str,
    action_name: str,
    label: str | None,
    started_at: str,
    completed_at: str,
    bundle_dir: Path,
    remote_bundle_copy_root: str,
    screenshot_path: Path,
    before_screenshot_path: Path,
    diff_screenshot_path: Path,
    ui_snapshot_path: Path,
    log_path: Path,
    err_path: Path,
    pid_path: Path,
    window_id_path: Path,
    window_title_path: Path,
    capture_before: bool,
    capture_ui_snapshot: bool,
    interaction_requested: bool,
    pulp_app_automation: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    default_desktop_label_fn: Callable[[str | None], str],
    image_change_summary_fn: Callable[..., dict],
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
    view_tree_inspector_summary_fn: Callable[[dict], dict],
    pulp_app_interaction_summary_fn: Callable[..., dict],
) -> dict:
    manifest = {
        "target": target_name,
        "adapter": target["adapter"],
        "action": action_name,
        "label": label or default_desktop_label_fn(command),
        "pid": read_linux_pid_file(pid_path),
        "host": host,
        "repo_path": repo_path,
        "command": launch_command,
        "started_at": started_at,
        "completed_at": completed_at,
        "artifacts": {
            "bundle_dir": str(bundle_dir),
            "screenshot": str(screenshot_path),
            "stdout": str(log_path),
            "stderr": str(err_path),
            "remote_bundle_dir": remote_bundle_copy_root,
        },
    }
    attach_linux_window_metadata(
        manifest,
        window_id_path=window_id_path,
        window_title_path=window_title_path,
    )
    attach_linux_before_diff_artifacts(
        manifest,
        capture_before=capture_before,
        before_screenshot_path=before_screenshot_path,
        screenshot_path=screenshot_path,
        diff_screenshot_path=diff_screenshot_path,
        image_change_summary_fn=image_change_summary_fn,
    )
    attach_linux_ui_snapshot(
        manifest,
        capture_ui_snapshot=capture_ui_snapshot,
        ui_snapshot_path=ui_snapshot_path,
        view_tree_inspector_summary_fn=view_tree_inspector_summary_fn,
    )
    attach_linux_interaction_summary(
        manifest,
        interaction_requested=interaction_requested,
        pulp_app_automation=pulp_app_automation,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        parse_coordinate_pair_fn=parse_coordinate_pair_fn,
        pulp_app_interaction_summary_fn=pulp_app_interaction_summary_fn,
    )
    return manifest
