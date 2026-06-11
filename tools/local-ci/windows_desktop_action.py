"""Windows desktop action execution helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path


def run_windows_session_agent_action(
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
    desktop_receipt_for_fn: Callable[[str], dict | None],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    probe_windows_session_agent_fn: Callable[[str, dict], dict],
    windows_desktop_session_user_fn: Callable[[dict | None], str],
    create_desktop_run_bundle_fn: Callable[[dict, str, str], Path],
    desktop_action_artifact_paths_fn: Callable[[Path, str | None], dict[str, Path]],
    desktop_interaction_requested_fn: Callable[..., bool],
    prepare_windows_exact_sha_source_fn: Callable[[Path, str, str, str, dict], dict],
    build_windows_session_agent_request_fn: Callable[..., dict],
    windows_path_join_fn: Callable[..., str],
    windows_ssh_write_text_fn: Callable[[str, str, str], None],
    start_windows_session_agent_task_fn: Callable[[str, dict], None],
    time_fn: Callable[[], float],
    sleep_fn: Callable[[float], None],
    windows_ssh_read_json_fn: Callable[..., dict | None],
    atomic_write_text_fn: Callable[[Path, str], None],
    windows_ssh_fetch_file_fn: Callable[..., bool],
    windows_ssh_remove_path_fn: Callable[[str, str], None],
    default_desktop_label_fn: Callable[[str | None], str],
    image_change_summary_fn: Callable[..., dict],
    view_tree_inspector_summary_fn: Callable[[dict], dict],
    pulp_app_interaction_summary_fn: Callable[..., dict],
    attach_desktop_source_to_manifest_fn: Callable[[dict, dict | None], None],
    write_desktop_run_rollups_fn: Callable[..., None],
    now_iso_fn: Callable[[], str],
) -> dict:
    host = ensure_host_reachable_fn(target_name, target, config.get("defaults", {}))
    if not host:
        raise RuntimeError(f"Desktop target `{target_name}` is not reachable over SSH.")
    if not target.get("repo_path"):
        raise RuntimeError(f"Desktop target `{target_name}` is missing repo_path.")

    receipt = desktop_receipt_for_fn(target_name)
    if not receipt:
        raise RuntimeError(f"Desktop target `{target_name}` is not installed. Run `pulp ci-local desktop install {target_name}`.")

    contract = receipt.get("contract") or desktop_target_contract_fn(target_name, target)
    probe = probe_windows_session_agent_fn(host, contract)
    if not (
        probe.get("task_present")
        and probe.get("agent_root_exists")
        and probe.get("jobs_dir_exists")
        and probe.get("results_dir_exists")
        and probe.get("script_exists")
    ):
        raise RuntimeError(
            f"Desktop target `{target_name}` is not bootstrapped. Run `pulp ci-local desktop install {target_name}`."
        )
    if not windows_desktop_session_user_fn(probe):
        raise RuntimeError(
            f"Desktop target `{target_name}` has no logged-in desktop session. Log into the target desktop, then retry."
        )
    if not pulp_app_automation:
        if capture_ui_snapshot:
            raise RuntimeError(
                f"Desktop target `{target_name}` currently supports --capture-ui-snapshot only with --pulp-app-automation."
            )
        if any([click_view_id, click_view_type, click_view_text, click_view_label]):
            raise RuntimeError(
                f"Desktop target `{target_name}` currently supports view-target selectors only with --pulp-app-automation."
            )

    bundle_dir = create_desktop_run_bundle_fn(config, target_name, action_name)
    action_paths = desktop_action_artifact_paths_fn(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    log_path = action_paths["stdout"]
    err_path = action_paths["stderr"]
    agent_manifest_path = bundle_dir / "agent-manifest.json"
    started_at = now_iso_fn()
    interaction_requested = desktop_interaction_requested_fn(
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
    )
    source_context = dict(source_request or {})
    if source_context.get("mode") == "exact-sha":
        source_context = prepare_windows_exact_sha_source_fn(bundle_dir, target_name, host, command, source_context)
    launch_cwd = source_context.get("launch_cwd") or target["repo_path"]
    launch_command = source_context.get("launch_command") or command

    request = build_windows_session_agent_request_fn(
        target_name,
        contract,
        launch_command,
        repo_path=launch_cwd,
        action_name=action_name,
        label=label,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
    )
    remote_request_path = windows_path_join_fn(contract["jobs_dir"], f"{request['job_id']}.json")
    windows_ssh_write_text_fn(host, remote_request_path, json.dumps(request, indent=2) + "\n")
    try:
        start_windows_session_agent_task_fn(host, contract)
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
            raise RuntimeError(
                f"Timed out waiting for Windows desktop agent result for `{target_name}` ({request['job_id']})."
            )

        agent_manifest_path.parent.mkdir(parents=True, exist_ok=True)
        atomic_write_text_fn(agent_manifest_path, json.dumps(remote_manifest, indent=2) + "\n")

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
    finally:
        windows_ssh_remove_path_fn(host, remote_request_path)
        windows_ssh_remove_path_fn(host, request["outputs"]["result_root"])

    status = remote_manifest.get("status") or "error"
    error_detail = remote_manifest.get("error")
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
        "completed_at": now_iso_fn(),
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
    attach_desktop_source_to_manifest_fn(manifest, source_context or source_request)
    atomic_write_text_fn(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    write_desktop_run_rollups_fn(config, target_name=target_name)
    write_desktop_run_rollups_fn(config)
    if status != "pass":
        detail = error_detail or f"Windows desktop agent returned status `{status}`"
        raise RuntimeError(detail)
    return manifest
