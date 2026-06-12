"""Bindings from the local_ci facade to Linux desktop action helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def fetch_ssh_artifact(
    bindings: Mapping[str, Any],
    host: str,
    remote_path: str,
    local_path,
    *,
    optional: bool = False,
    timeout: int = 60,
) -> bool:
    return _binding(bindings, "_linux_desktop_action").fetch_ssh_artifact(
        host,
        remote_path,
        local_path,
        optional=optional,
        timeout=timeout,
        run_fn=_binding(bindings, "subprocess").run,
    )


def cleanup_remote_ssh_dir(bindings: Mapping[str, Any], host: str, remote_dir_expr: str) -> None:
    return _binding(bindings, "_linux_desktop_action").cleanup_remote_ssh_dir(
        host,
        remote_dir_expr,
        ssh_command_result_fn=_binding(bindings, "ssh_command_result"),
    )


def run_linux_xvfb_remote_action(
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
    subprocess_mod = _binding(bindings, "subprocess")

    return _binding(bindings, "_linux_desktop_action").run_linux_xvfb_remote_action(
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
        probe_linux_launch_backend_fn=_binding(bindings, "probe_linux_launch_backend"),
        create_desktop_run_bundle_fn=_binding(bindings, "create_desktop_run_bundle"),
        desktop_action_artifact_paths_fn=desktop_actions.desktop_action_artifact_paths,
        desktop_interaction_requested_fn=desktop_actions.desktop_interaction_requested,
        prepare_linux_exact_sha_source_fn=_binding(bindings, "prepare_linux_exact_sha_source"),
        remote_linux_bundle_relpath_fn=_binding(bindings, "remote_linux_bundle_relpath"),
        build_linux_xvfb_remote_command_fn=_binding(bindings, "build_linux_xvfb_remote_command"),
        build_linux_window_driver_remote_command_fn=_binding(bindings, "build_linux_window_driver_remote_command"),
        run_fn=subprocess_mod.run,
        fetch_ssh_artifact_fn=_binding(bindings, "fetch_ssh_artifact"),
        cleanup_remote_ssh_dir_fn=_binding(bindings, "cleanup_remote_ssh_dir"),
        default_desktop_label_fn=_binding(bindings, "default_desktop_label"),
        image_change_summary_fn=_binding(bindings, "image_change_summary"),
        parse_coordinate_pair_fn=_binding(bindings, "parse_coordinate_pair"),
        attach_desktop_source_to_manifest_fn=_binding(bindings, "attach_desktop_source_to_manifest"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        write_desktop_run_rollups_fn=_binding(bindings, "write_desktop_run_rollups"),
        now_iso_fn=_binding(bindings, "now_iso"),
        view_tree_inspector_summary_fn=desktop_actions.view_tree_inspector_summary,
        pulp_app_interaction_summary_fn=desktop_actions.pulp_app_interaction_summary,
    )
