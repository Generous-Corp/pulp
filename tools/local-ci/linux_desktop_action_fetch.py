"""Linux desktop action remote artifact fetch helpers."""

from __future__ import annotations

from collections.abc import Callable
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
