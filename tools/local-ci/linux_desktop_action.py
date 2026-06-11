"""Linux desktop action execution helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import shlex
import subprocess


def fetch_ssh_artifact(
    host: str,
    remote_path: str,
    local_path: Path,
    *,
    optional: bool = False,
    timeout: int = 60,
    run_fn: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> bool:
    local_path.parent.mkdir(parents=True, exist_ok=True)
    result = run_fn(
        ["scp", f"{host}:{remote_path}", str(local_path)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    if result.returncode == 0 and local_path.exists():
        return True
    if optional:
        return False
    detail = result.stderr.strip() or result.stdout.strip() or f"scp exited {result.returncode}"
    raise RuntimeError(f"Failed to copy `{remote_path}` from {host}: {detail}")


def cleanup_remote_ssh_dir(
    host: str,
    remote_dir_expr: str,
    *,
    ssh_command_result_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> None:
    try:
        ssh_command_result_fn(host, f"rm -rf {remote_dir_expr}", timeout=20)
    except Exception:
        pass


def run_linux_xvfb_remote_action(
    config: dict,
    target_name: str,
    target: dict,
    command: str,
    *,
    action_name: str,
    label: str | None,
    output_path: str | None,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    source_request: dict | None = None,
    ensure_host_reachable_fn: Callable[[str, dict, dict], str | None],
    probe_linux_launch_backend_fn: Callable[[str], dict],
    create_desktop_run_bundle_fn: Callable[[dict, str, str], Path],
    desktop_action_artifact_paths_fn: Callable[[Path, str | None], dict[str, Path]],
    desktop_interaction_requested_fn: Callable[..., bool],
    prepare_linux_exact_sha_source_fn: Callable[[Path, str, str, str, dict], dict],
    remote_linux_bundle_relpath_fn: Callable[[str, str, Path], str],
    build_linux_xvfb_remote_command_fn: Callable[..., str],
    build_linux_window_driver_remote_command_fn: Callable[..., str],
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
    fetch_ssh_artifact_fn: Callable[..., bool],
    cleanup_remote_ssh_dir_fn: Callable[[str, str], None],
    default_desktop_label_fn: Callable[[str | None], str],
    image_change_summary_fn: Callable[..., dict],
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
    attach_desktop_source_to_manifest_fn: Callable[[dict, dict | None], None],
    atomic_write_text_fn: Callable[[Path, str], None],
    write_desktop_run_rollups_fn: Callable[..., None],
    now_iso_fn: Callable[[], str],
    view_tree_inspector_summary_fn: Callable[[dict], dict],
    pulp_app_interaction_summary_fn: Callable[..., dict],
) -> dict:
    host = ensure_host_reachable_fn(target_name, target, config.get("defaults", {}))
    if not host:
        raise RuntimeError(f"Desktop target `{target_name}` is not reachable over SSH.")
    repo_path = target.get("repo_path")
    if not repo_path:
        raise RuntimeError(f"Desktop target `{target_name}` is missing repo_path.")
    launch_backend = probe_linux_launch_backend_fn(host)
    if launch_backend.get("mode") == "missing":
        raise RuntimeError(
            f"Desktop target `{target_name}` needs xvfb-run or an existing desktop display session."
        )
    interaction_requested = desktop_interaction_requested_fn(
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
    )
    if not pulp_app_automation:
        if capture_ui_snapshot:
            raise RuntimeError("linux-xvfb desktop inspect supports UI snapshots only with --pulp-app-automation.")
        if any([click_view_id, click_view_type, click_view_text, click_view_label]):
            raise RuntimeError("linux-xvfb view-target selectors currently require --pulp-app-automation.")

    bundle_dir = create_desktop_run_bundle_fn(config, target_name, action_name)
    action_paths = desktop_action_artifact_paths_fn(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    log_path = action_paths["stdout"]
    err_path = action_paths["stderr"]
    pid_path = bundle_dir / "pid.txt"
    window_id_path = bundle_dir / "window-id.txt"
    window_title_path = bundle_dir / "window-title.txt"
    started_at = now_iso_fn()
    remote_bundle_relpath = remote_linux_bundle_relpath_fn(target_name, action_name, bundle_dir)
    remote_bundle_copy_root = f"~/{remote_bundle_relpath}"
    remote_bundle_cleanup_expr = f'"$HOME/{remote_bundle_relpath}"'
    source_context = dict(source_request or {})
    if source_context.get("mode") == "exact-sha":
        source_context = prepare_linux_exact_sha_source_fn(bundle_dir, target_name, host, command, source_context)
    launch_cwd = source_context.get("launch_cwd") or repo_path
    launch_command = source_context.get("launch_command") or command
    if pulp_app_automation:
        remote_cmd = build_linux_xvfb_remote_command_fn(
            repo_path,
            remote_bundle_relpath,
            launch_command,
            launch_backend=launch_backend,
            launch_cwd=launch_cwd,
            capture_ui_snapshot=capture_ui_snapshot,
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
            capture_before=capture_before,
            settle_secs=settle_secs,
        )
    else:
        remote_cmd = build_linux_window_driver_remote_command_fn(
            repo_path,
            remote_bundle_relpath,
            launch_command,
            launch_backend=launch_backend,
            launch_cwd=launch_cwd,
            click_point=click_point,
            capture_before=capture_before,
            settle_secs=settle_secs,
        )

    remote_cmd = 'export PATH="$HOME/.local/bin:$PATH"; ' + remote_cmd
    run = run_fn(
        ["ssh", host, "bash", "-lc", shlex.quote(remote_cmd)],
        capture_output=True,
        text=True,
        timeout=max(30, int(timeout_secs + settle_secs + 20)),
    )
    log_path.write_text(run.stdout or "")
    err_path.write_text(run.stderr or "")

    remote_screenshot = remote_bundle_copy_root + "/screenshots/window.png"
    remote_before = remote_bundle_copy_root + "/screenshots/before.png"
    remote_ui = remote_bundle_copy_root + "/ui-tree.json"
    remote_stdout = remote_bundle_copy_root + "/stdout.log"
    remote_stderr = remote_bundle_copy_root + "/stderr.log"
    remote_pid = remote_bundle_copy_root + "/pid.txt"
    remote_window_id = remote_bundle_copy_root + "/window-id.txt"
    remote_window_title = remote_bundle_copy_root + "/window-title.txt"

    try:
        fetch_ssh_artifact_fn(host, remote_stdout, log_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_stderr, err_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_screenshot, screenshot_path)
        fetch_ssh_artifact_fn(host, remote_pid, pid_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_window_id, window_id_path, optional=True)
        fetch_ssh_artifact_fn(host, remote_window_title, window_title_path, optional=True)
        if capture_before:
            fetch_ssh_artifact_fn(host, remote_before, before_screenshot_path, optional=not pulp_app_automation)
        if capture_ui_snapshot:
            fetch_ssh_artifact_fn(host, remote_ui, ui_snapshot_path)
    finally:
        cleanup_remote_ssh_dir_fn(host, remote_bundle_cleanup_expr)

    if run.returncode != 0:
        detail = err_path.read_text(errors="replace").strip() or log_path.read_text(errors="replace").strip() or f"remote command exited {run.returncode}"
        raise RuntimeError(detail)

    pid_value = None
    if pid_path.exists():
        try:
            pid_value = int(pid_path.read_text().strip())
        except ValueError:
            pid_value = None

    manifest = {
        "target": target_name,
        "adapter": target["adapter"],
        "action": action_name,
        "label": label or default_desktop_label_fn(command),
        "pid": pid_value,
        "host": host,
        "repo_path": repo_path,
        "command": launch_command,
        "started_at": started_at,
        "completed_at": now_iso_fn(),
        "artifacts": {
            "bundle_dir": str(bundle_dir),
            "screenshot": str(screenshot_path),
            "stdout": str(log_path),
            "stderr": str(err_path),
            "remote_bundle_dir": remote_bundle_copy_root,
        },
    }
    if window_id_path.exists() or window_title_path.exists():
        manifest["window"] = {}
        if window_id_path.exists():
            manifest["window"]["window_id"] = window_id_path.read_text().strip()
        if window_title_path.exists():
            manifest["window"]["title"] = window_title_path.read_text().strip()
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
    if interaction_requested:
        if pulp_app_automation:
            manifest["interaction"] = pulp_app_interaction_summary_fn(
                click_point=click_point,
                click_view_id=click_view_id,
                click_view_type=click_view_type,
                click_view_text=click_view_text,
                click_view_label=click_view_label,
            )
        else:
            click_summary = {"point": click_point}
            if click_point:
                content_x, content_y = parse_coordinate_pair_fn(click_point, flag_name="--click")
                click_summary["content_point"] = {"x": content_x, "y": content_y}
            manifest["interaction"] = {"mode": "x11-window-driver", "click": click_summary}
    attach_desktop_source_to_manifest_fn(manifest, source_context or source_request)
    atomic_write_text_fn(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    write_desktop_run_rollups_fn(config, target_name=target_name)
    write_desktop_run_rollups_fn(config)
    return manifest
