"""Windows desktop action manifest assembly."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from windows_desktop_action_artifacts import attach_windows_before_diff_artifacts, attach_windows_ui_snapshot
from windows_desktop_action_interaction import attach_windows_interaction_summary


def build_windows_desktop_action_manifest(
    *,
    target_name: str,
    target: dict,
    command: str,
    launch_command: str,
    host: str,
    action_name: str,
    label: str | None,
    started_at: str,
    completed_at: str,
    remote_manifest: dict,
    bundle_dir: Path,
    agent_manifest_path: Path,
    screenshot_path: Path,
    before_screenshot_path: Path,
    diff_screenshot_path: Path,
    ui_snapshot_path: Path,
    log_path: Path,
    err_path: Path,
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
    view_tree_inspector_summary_fn: Callable[[dict], dict],
    pulp_app_interaction_summary_fn: Callable[..., dict],
) -> dict:
    status = remote_manifest.get("status") or "error"
    manifest = {
        "target": target_name,
        "adapter": target["adapter"],
        "action": action_name,
        "label": label or default_desktop_label_fn(command),
        "pid": remote_manifest.get("pid"),
        "host": host,
        "repo_path": target["repo_path"],
        "command": launch_command,
        "started_at": started_at,
        "completed_at": completed_at,
        "window": remote_manifest.get("window"),
        "artifacts": {
            "bundle_dir": str(bundle_dir),
            "screenshot": str(screenshot_path),
            "stdout": str(log_path),
            "stderr": str(err_path),
            "agent_manifest": str(agent_manifest_path),
        },
        "agent_status": status,
    }
    attach_windows_before_diff_artifacts(
        manifest,
        capture_before=capture_before,
        before_screenshot_path=before_screenshot_path,
        screenshot_path=screenshot_path,
        diff_screenshot_path=diff_screenshot_path,
        image_change_summary_fn=image_change_summary_fn,
    )
    attach_windows_ui_snapshot(
        manifest,
        capture_ui_snapshot=capture_ui_snapshot,
        ui_snapshot_path=ui_snapshot_path,
        view_tree_inspector_summary_fn=view_tree_inspector_summary_fn,
    )
    attach_windows_interaction_summary(
        manifest,
        remote_manifest=remote_manifest,
        interaction_requested=interaction_requested,
        pulp_app_automation=pulp_app_automation,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        pulp_app_interaction_summary_fn=pulp_app_interaction_summary_fn,
    )
    return manifest
