"""Windows desktop action result polling, artifact fetch, and manifest assembly."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path


def wait_for_windows_session_agent_manifest(
    *,
    host: str,
    target_name: str,
    request: dict,
    timeout_secs: float,
    settle_secs: float,
    time_fn: Callable[[], float],
    sleep_fn: Callable[[float], None],
    windows_ssh_read_json_fn: Callable[..., dict | None],
) -> dict:
    deadline = time_fn() + timeout_secs + settle_secs + 15.0
    remote_manifest: dict | None = None
    while time_fn() < deadline:
        remote_manifest = windows_ssh_read_json_fn(
            host,
            request["outputs"]["manifest"],
            timeout=15,
            optional=True,
        )
        if remote_manifest is not None:
            break
        sleep_fn(0.5)
    if remote_manifest is None:
        raise RuntimeError(f"Timed out waiting for Windows desktop agent result for `{target_name}` ({request['job_id']}).")
    return remote_manifest


def fetch_windows_session_agent_outputs(
    *,
    host: str,
    request: dict,
    capture_before: bool,
    capture_ui_snapshot: bool,
    screenshot_path: Path,
    before_screenshot_path: Path,
    ui_snapshot_path: Path,
    log_path: Path,
    err_path: Path,
    windows_ssh_fetch_file_fn: Callable[..., bool],
) -> None:
    fetch_stdout = windows_ssh_fetch_file_fn(
        host,
        request["outputs"]["stdout"],
        log_path,
        optional=True,
        timeout=30,
    )
    fetch_stderr = windows_ssh_fetch_file_fn(
        host,
        request["outputs"]["stderr"],
        err_path,
        optional=True,
        timeout=30,
    )
    if not fetch_stdout:
        log_path.write_text("")
    if not fetch_stderr:
        err_path.write_text("")
    windows_ssh_fetch_file_fn(host, request["outputs"]["screenshot"], screenshot_path, timeout=60)
    if capture_before:
        windows_ssh_fetch_file_fn(
            host,
            request["outputs"]["before_screenshot"],
            before_screenshot_path,
            optional=False,
            timeout=60,
        )
    if capture_ui_snapshot:
        windows_ssh_fetch_file_fn(
            host,
            request["outputs"]["ui_snapshot"],
            ui_snapshot_path,
            optional=False,
            timeout=30,
        )


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
    if capture_before and before_screenshot_path.exists() and screenshot_path.exists():
        manifest["artifacts"]["before_screenshot"] = str(before_screenshot_path)
        manifest["artifacts"]["image_change"] = image_change_summary_fn(
            before_screenshot_path,
            screenshot_path,
            diff_output_path=diff_screenshot_path,
        )
        if diff_screenshot_path.exists():
            manifest["artifacts"]["diff_screenshot"] = str(diff_screenshot_path)
    if capture_ui_snapshot and ui_snapshot_path.exists():
        view_tree = json.loads(ui_snapshot_path.read_text())
        manifest["artifacts"]["ui_snapshot"] = str(ui_snapshot_path)
        manifest["inspector"] = view_tree_inspector_summary_fn(view_tree)
    remote_interaction = remote_manifest.get("interaction")
    if remote_interaction:
        manifest["interaction"] = remote_interaction
    elif interaction_requested:
        manifest["interaction"] = pulp_app_interaction_summary_fn(
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
        )
        if not pulp_app_automation:
            manifest["interaction"]["mode"] = "window-capture"
    return manifest
