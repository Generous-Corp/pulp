"""Windows desktop action execution helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path

from windows_desktop_action_result import (
    build_windows_desktop_action_manifest,
    fetch_windows_session_agent_outputs,
    wait_for_windows_session_agent_manifest,
)
from desktop_remote_action_preflight import (
    require_pulp_app_automation_for_remote_view_options,
    resolve_remote_desktop_action_host,
)


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
    host, repo_path = resolve_remote_desktop_action_host(
        config,
        target_name,
        target,
        ensure_host_reachable_fn=ensure_host_reachable_fn,
    )

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
    require_pulp_app_automation_for_remote_view_options(
        target_name=target_name,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        snapshot_error="Desktop target `{target_name}` currently supports --capture-ui-snapshot only with --pulp-app-automation.",
        selector_error="Desktop target `{target_name}` currently supports view-target selectors only with --pulp-app-automation.",
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
    launch_cwd = source_context.get("launch_cwd") or repo_path
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
        remote_manifest = wait_for_windows_session_agent_manifest(
            host=host,
            target_name=target_name,
            request=request,
            timeout_secs=timeout_secs,
            settle_secs=settle_secs,
            time_fn=time_fn,
            sleep_fn=sleep_fn,
            windows_ssh_read_json_fn=windows_ssh_read_json_fn,
        )

        agent_manifest_path.parent.mkdir(parents=True, exist_ok=True)
        atomic_write_text_fn(agent_manifest_path, json.dumps(remote_manifest, indent=2) + "\n")

        fetch_windows_session_agent_outputs(
            host=host,
            request=request,
            capture_before=capture_before,
            capture_ui_snapshot=capture_ui_snapshot,
            screenshot_path=screenshot_path,
            before_screenshot_path=before_screenshot_path,
            ui_snapshot_path=ui_snapshot_path,
            log_path=log_path,
            err_path=err_path,
            windows_ssh_fetch_file_fn=windows_ssh_fetch_file_fn,
        )
    finally:
        windows_ssh_remove_path_fn(host, remote_request_path)
        windows_ssh_remove_path_fn(host, request["outputs"]["result_root"])

    status = remote_manifest.get("status") or "error"
    error_detail = remote_manifest.get("error")
    manifest = build_windows_desktop_action_manifest(
        target_name=target_name,
        target=target,
        command=command,
        launch_command=launch_command,
        host=host,
        action_name=action_name,
        label=label,
        started_at=started_at,
        completed_at=now_iso_fn(),
        remote_manifest=remote_manifest,
        bundle_dir=bundle_dir,
        agent_manifest_path=agent_manifest_path,
        screenshot_path=screenshot_path,
        before_screenshot_path=before_screenshot_path,
        diff_screenshot_path=diff_screenshot_path,
        ui_snapshot_path=ui_snapshot_path,
        log_path=log_path,
        err_path=err_path,
        capture_before=capture_before,
        capture_ui_snapshot=capture_ui_snapshot,
        interaction_requested=interaction_requested,
        pulp_app_automation=pulp_app_automation,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        default_desktop_label_fn=default_desktop_label_fn,
        image_change_summary_fn=image_change_summary_fn,
        view_tree_inspector_summary_fn=view_tree_inspector_summary_fn,
        pulp_app_interaction_summary_fn=pulp_app_interaction_summary_fn,
    )
    attach_desktop_source_to_manifest_fn(manifest, source_context or source_request)
    atomic_write_text_fn(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
    write_desktop_run_rollups_fn(config, target_name=target_name)
    write_desktop_run_rollups_fn(config)
    if status != "pass":
        detail = error_detail or f"Windows desktop agent returned status `{status}`"
        raise RuntimeError(detail)
    return manifest
