"""Bindings from the local_ci facade to Windows desktop action helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def run_windows_session_agent_action(
    bindings: Mapping[str, Any],
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
) -> dict:
    desktop_actions = _binding(bindings, "_desktop_actions")
    time_mod = _binding(bindings, "time")

    return _binding(bindings, "_windows_desktop_action").run_windows_session_agent_action(
        config,
        target_name,
        target,
        command,
        action_name=action_name,
        label=label,
        output_path=output_path,
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
        source_request=source_request,
        ensure_host_reachable_fn=_binding(bindings, "ensure_host_reachable"),
        desktop_receipt_for_fn=_binding(bindings, "desktop_receipt_for"),
        desktop_target_contract_fn=_binding(bindings, "desktop_target_contract"),
        probe_windows_session_agent_fn=_binding(bindings, "probe_windows_session_agent"),
        windows_desktop_session_user_fn=_binding(bindings, "windows_desktop_session_user"),
        create_desktop_run_bundle_fn=_binding(bindings, "create_desktop_run_bundle"),
        desktop_action_artifact_paths_fn=desktop_actions.desktop_action_artifact_paths,
        desktop_interaction_requested_fn=desktop_actions.desktop_interaction_requested,
        prepare_windows_exact_sha_source_fn=_binding(bindings, "prepare_windows_exact_sha_source"),
        build_windows_session_agent_request_fn=_binding(bindings, "build_windows_session_agent_request"),
        windows_path_join_fn=_binding(bindings, "windows_path_join"),
        windows_ssh_write_text_fn=_binding(bindings, "windows_ssh_write_text"),
        start_windows_session_agent_task_fn=_binding(bindings, "start_windows_session_agent_task"),
        time_fn=time_mod.time,
        sleep_fn=time_mod.sleep,
        windows_ssh_read_json_fn=_binding(bindings, "windows_ssh_read_json"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        windows_ssh_fetch_file_fn=_binding(bindings, "windows_ssh_fetch_file"),
        windows_ssh_remove_path_fn=_binding(bindings, "windows_ssh_remove_path"),
        default_desktop_label_fn=_binding(bindings, "default_desktop_label"),
        image_change_summary_fn=_binding(bindings, "image_change_summary"),
        view_tree_inspector_summary_fn=desktop_actions.view_tree_inspector_summary,
        pulp_app_interaction_summary_fn=desktop_actions.pulp_app_interaction_summary,
        attach_desktop_source_to_manifest_fn=_binding(bindings, "attach_desktop_source_to_manifest"),
        write_desktop_run_rollups_fn=_binding(bindings, "write_desktop_run_rollups"),
        now_iso_fn=_binding(bindings, "now_iso"),
    )
