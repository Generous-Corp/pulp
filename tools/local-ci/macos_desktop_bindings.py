"""Bindings from the local_ci facade to macOS desktop action helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def run_macos_local_smoke(
    bindings: Mapping[str, Any],
    config: dict,
    command: str | None,
    *,
    action_name: str = "smoke",
    bundle_id: str | None,
    label: str | None,
    output_path: str | None,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    pulp_app_automation: bool = False,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    source_request: dict | None = None,
    record_video: bool = False,
    video_duration_secs: float = 8.0,
    video_fps: float = 30.0,
    video_attachment_budget_bytes: int = 100_000_000,
    compose_video_proof: bool = False,
    video_template: str | None = None,
    video_source_image: str | None = None,
    video_source_label: str | None = None,
    video_title: str | None = None,
) -> dict:
    desktop_actions = _binding(bindings, "_desktop_actions")
    subprocess_mod = _binding(bindings, "subprocess")
    time_mod = _binding(bindings, "time")
    shlex_mod = _binding(bindings, "shlex")
    os_mod = _binding(bindings, "os")

    return _binding(bindings, "_macos_desktop_action").run_macos_local_smoke(
        config,
        command,
        action_name=action_name,
        bundle_id=bundle_id,
        label=label,
        output_path=output_path,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        pulp_app_automation=pulp_app_automation,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
        source_request=source_request,
        record_video=record_video,
        video_duration_secs=video_duration_secs,
        video_fps=video_fps,
        video_attachment_budget_bytes=video_attachment_budget_bytes,
        compose_video_proof=compose_video_proof,
        video_template=video_template,
        video_source_image=video_source_image,
        video_source_label=video_source_label,
        video_title=video_title,
        create_desktop_run_bundle_fn=_binding(bindings, "create_desktop_run_bundle"),
        desktop_action_artifact_paths_fn=desktop_actions.desktop_action_artifact_paths,
        desktop_interaction_requested_fn=desktop_actions.desktop_interaction_requested,
        macos_accessibility_trusted_fn=_binding(bindings, "macos_accessibility_trusted"),
        now_iso_fn=_binding(bindings, "now_iso"),
        prepare_macos_exact_sha_source_fn=_binding(bindings, "prepare_macos_exact_sha_source"),
        quit_macos_bundle_id_fn=_binding(bindings, "quit_macos_bundle_id"),
        sleep_fn=time_mod.sleep,
        run_fn=subprocess_mod.run,
        activate_macos_bundle_id_fn=_binding(bindings, "activate_macos_bundle_id"),
        wait_for_macos_bundle_window_fn=_binding(bindings, "wait_for_macos_bundle_window"),
        split_command_fn=shlex_mod.split,
        detect_macos_app_bundle_fn=_binding(bindings, "detect_macos_app_bundle"),
        macos_bundle_id_for_app_path_fn=_binding(bindings, "macos_bundle_id_for_app_path"),
        environ_copy_fn=os_mod.environ.copy,
        popen_fn=subprocess_mod.Popen,
        wait_for_macos_window_fn=_binding(bindings, "wait_for_macos_window"),
        content_size_from_window_fn=desktop_actions.content_size_from_window,
        wait_for_path_fn=_binding(bindings, "wait_for_path"),
        content_size_from_view_tree_fn=desktop_actions.content_size_from_view_tree,
        view_tree_inspector_summary_fn=desktop_actions.view_tree_inspector_summary,
        pulp_app_interaction_summary_fn=desktop_actions.pulp_app_interaction_summary,
        capture_macos_window_fn=_binding(bindings, "capture_macos_window"),
        parse_coordinate_pair_fn=_binding(bindings, "parse_coordinate_pair"),
        resolve_view_tree_click_point_fn=_binding(bindings, "resolve_view_tree_click_point"),
        screen_point_for_content_point_fn=_binding(bindings, "screen_point_for_content_point"),
        activate_macos_pid_fn=_binding(bindings, "activate_macos_pid"),
        dispatch_macos_click_fn=_binding(bindings, "dispatch_macos_click"),
        desktop_click_selector_fn=desktop_actions.desktop_click_selector,
        image_change_summary_fn=_binding(bindings, "image_change_summary"),
        start_macos_window_video_recording_fn=_binding(bindings, "start_macos_window_video_recording"),
        stop_macos_window_video_recording_fn=_binding(bindings, "stop_macos_window_video_recording"),
        compose_desktop_video_proof_fn=_binding(bindings, "compose_desktop_video_proof"),
        create_issue_video_variant_fn=_binding(bindings, "create_issue_video_variant"),
        attach_desktop_source_to_manifest_fn=_binding(bindings, "attach_desktop_source_to_manifest"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        write_desktop_run_rollups_fn=_binding(bindings, "write_desktop_run_rollups"),
        terminate_process_fn=_binding(bindings, "terminate_process"),
    )
