"""Linux desktop action remote artifact fetch and manifest assembly."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path


def fetch_linux_remote_action_outputs(
    *,
    host: str,
    remote_bundle_copy_root: str,
    remote_bundle_cleanup_expr: str,
    pulp_app_automation: bool,
    capture_before: bool,
    capture_ui_snapshot: bool,
    screenshot_path: Path,
    before_screenshot_path: Path,
    ui_snapshot_path: Path,
    log_path: Path,
    err_path: Path,
    pid_path: Path,
    window_id_path: Path,
    window_title_path: Path,
    fetch_ssh_artifact_fn: Callable[..., bool],
    cleanup_remote_ssh_dir_fn: Callable[[str, str], None],
) -> None:
    try:
        fetch_ssh_artifact_fn(host, remote_bundle_copy_root + "/stdout.log", log_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_bundle_copy_root + "/stderr.log", err_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_bundle_copy_root + "/screenshots/window.png", screenshot_path)
        fetch_ssh_artifact_fn(host, remote_bundle_copy_root + "/pid.txt", pid_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_bundle_copy_root + "/window-id.txt", window_id_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_bundle_copy_root + "/window-title.txt", window_title_path, optional=True)
        if capture_before:
            fetch_ssh_artifact_fn(
                host,
                remote_bundle_copy_root + "/screenshots/before.png",
                before_screenshot_path,
                optional=not pulp_app_automation,
            )
        if capture_ui_snapshot:
            fetch_ssh_artifact_fn(host, remote_bundle_copy_root + "/ui-tree.json", ui_snapshot_path)
    finally:
        cleanup_remote_ssh_dir_fn(host, remote_bundle_cleanup_expr)


def read_linux_pid_file(pid_path: Path) -> int | None:
    if not pid_path.exists():
        return None
    try:
        return int(pid_path.read_text().strip())
    except ValueError:
        return None


def attach_linux_window_metadata(
    manifest: dict,
    *,
    window_id_path: Path,
    window_title_path: Path,
) -> None:
    if not (window_id_path.exists() or window_title_path.exists()):
        return
    manifest["window"] = {}
    if window_id_path.exists():
        manifest["window"]["window_id"] = window_id_path.read_text().strip()
    if window_title_path.exists():
        manifest["window"]["title"] = window_title_path.read_text().strip()


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


def attach_linux_interaction_summary(
    manifest: dict,
    *,
    interaction_requested: bool,
    pulp_app_automation: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
    pulp_app_interaction_summary_fn: Callable[..., dict],
) -> None:
    if not interaction_requested:
        return
    if pulp_app_automation:
        manifest["interaction"] = pulp_app_interaction_summary_fn(
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
        )
        return

    click_summary = {"point": click_point}
    if click_point:
        content_x, content_y = parse_coordinate_pair_fn(click_point, flag_name="--click")
        click_summary["content_point"] = {"x": content_x, "y": content_y}
    manifest["interaction"] = {"mode": "x11-window-driver", "click": click_summary}


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
