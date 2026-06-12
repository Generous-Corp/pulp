"""macOS desktop action execution helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path

from macos_desktop_action_capture import complete_macos_desktop_action_capture
from macos_desktop_action_launch import launch_macos_desktop_action
from macos_desktop_action_manifest import build_macos_action_manifest


def run_macos_local_smoke(
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
    create_desktop_run_bundle_fn: Callable[[dict, str, str], Path],
    desktop_action_artifact_paths_fn: Callable[[Path, str | None], dict[str, Path]],
    desktop_interaction_requested_fn: Callable[..., bool],
    macos_accessibility_trusted_fn: Callable[[], bool],
    now_iso_fn: Callable[[], str],
    prepare_macos_exact_sha_source_fn: Callable[[Path, str, str, dict], dict],
    quit_macos_bundle_id_fn: Callable[[str], None],
    sleep_fn: Callable[[float], None],
    run_fn: Callable[..., object],
    activate_macos_bundle_id_fn: Callable[[str], None],
    wait_for_macos_bundle_window_fn: Callable[[str, float], tuple[int, dict]],
    split_command_fn: Callable[[str], list[str]],
    detect_macos_app_bundle_fn: Callable[[str | None], Path | None],
    macos_bundle_id_for_app_path_fn: Callable[[Path], str | None],
    environ_copy_fn: Callable[[], dict[str, str]],
    popen_fn: Callable[..., object],
    wait_for_macos_window_fn: Callable[[int, float], dict],
    content_size_from_window_fn: Callable[[dict], tuple[float, float]],
    wait_for_path_fn: Callable[[Path, float], None],
    content_size_from_view_tree_fn: Callable[[dict, tuple[float, float]], tuple[float, float]],
    view_tree_inspector_summary_fn: Callable[[dict], dict],
    pulp_app_interaction_summary_fn: Callable[..., dict],
    capture_macos_window_fn: Callable[[int, Path], None],
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
    resolve_view_tree_click_point_fn: Callable[..., tuple[float, float]],
    screen_point_for_content_point_fn: Callable[[dict, tuple[float, float], tuple[float, float]], tuple[float, float]],
    activate_macos_pid_fn: Callable[[int], dict],
    dispatch_macos_click_fn: Callable[[float, float], dict],
    desktop_click_selector_fn: Callable[..., dict],
    image_change_summary_fn: Callable[..., dict],
    attach_desktop_source_to_manifest_fn: Callable[[dict, dict | None], None],
    atomic_write_text_fn: Callable[[Path, str], None],
    write_desktop_run_rollups_fn: Callable[..., None],
    terminate_process_fn: Callable[[object], None],
) -> dict:
    bundle_dir = create_desktop_run_bundle_fn(config, "mac", action_name)
    action_paths = desktop_action_artifact_paths_fn(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    log_path = action_paths["stdout"]
    err_path = action_paths["stderr"]

    interaction_requested = desktop_interaction_requested_fn(
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
    )
    use_pulp_app_automation = bool(pulp_app_automation and interaction_requested)
    if use_pulp_app_automation and bundle_id:
        raise RuntimeError("Pulp app automation requires a direct --command launch so automation env vars can be injected.")
    if interaction_requested and not use_pulp_app_automation and not macos_accessibility_trusted_fn():
        raise RuntimeError("macOS desktop interaction requires Accessibility access for the terminal/runner.")
    if (click_view_id or click_view_type or click_view_text or click_view_label) and not capture_ui_snapshot and not use_pulp_app_automation:
        raise RuntimeError("View-targeted click requires --capture-ui-snapshot so the app writes a ViewInspector tree.")

    started_at = now_iso_fn()
    source_context = dict(source_request or {})
    launch_cwd: str | None = None
    launch_command = command
    if source_context.get("mode") == "exact-sha":
        if bundle_id:
            raise RuntimeError("Exact-SHA desktop source mode currently requires --command, not --bundle-id.")
        if not command:
            raise RuntimeError("Exact-SHA desktop source mode requires --command.")
        source_context = prepare_macos_exact_sha_source_fn(bundle_dir, "mac", command, source_context)
        launch_cwd = source_context.get("launch_cwd")
        launch_command = source_context.get("launch_command") or command

    proc = None
    pid = None
    try:
        launch_result = launch_macos_desktop_action(
            bundle_id=bundle_id,
            launch_command=launch_command,
            launch_cwd=launch_cwd,
            capture_ui_snapshot=capture_ui_snapshot,
            use_pulp_app_automation=use_pulp_app_automation,
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
            capture_before=capture_before,
            settle_secs=settle_secs,
            timeout_secs=timeout_secs,
            ui_snapshot_path=ui_snapshot_path,
            before_screenshot_path=before_screenshot_path,
            screenshot_path=screenshot_path,
            log_path=log_path,
            err_path=err_path,
            quit_macos_bundle_id_fn=quit_macos_bundle_id_fn,
            sleep_fn=sleep_fn,
            run_fn=run_fn,
            activate_macos_bundle_id_fn=activate_macos_bundle_id_fn,
            wait_for_macos_bundle_window_fn=wait_for_macos_bundle_window_fn,
            split_command_fn=split_command_fn,
            detect_macos_app_bundle_fn=detect_macos_app_bundle_fn,
            macos_bundle_id_for_app_path_fn=macos_bundle_id_for_app_path_fn,
            environ_copy_fn=environ_copy_fn,
            popen_fn=popen_fn,
            wait_for_macos_window_fn=wait_for_macos_window_fn,
        )
        proc = launch_result["proc"]
        pid = launch_result["pid"]
        window = launch_result["window"]
        launch_descriptor = launch_result["launch_descriptor"]

        capture_result = complete_macos_desktop_action_capture(
            window=window,
            pid=pid,
            bundle_id=bundle_id,
            launch_descriptor=launch_descriptor,
            capture_ui_snapshot=capture_ui_snapshot,
            use_pulp_app_automation=use_pulp_app_automation,
            interaction_requested=interaction_requested,
            capture_before=capture_before,
            settle_secs=settle_secs,
            timeout_secs=timeout_secs,
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
            screenshot_path=screenshot_path,
            before_screenshot_path=before_screenshot_path,
            ui_snapshot_path=ui_snapshot_path,
            content_size_from_window_fn=content_size_from_window_fn,
            wait_for_path_fn=wait_for_path_fn,
            content_size_from_view_tree_fn=content_size_from_view_tree_fn,
            view_tree_inspector_summary_fn=view_tree_inspector_summary_fn,
            pulp_app_interaction_summary_fn=pulp_app_interaction_summary_fn,
            capture_macos_window_fn=capture_macos_window_fn,
            parse_coordinate_pair_fn=parse_coordinate_pair_fn,
            resolve_view_tree_click_point_fn=resolve_view_tree_click_point_fn,
            screen_point_for_content_point_fn=screen_point_for_content_point_fn,
            activate_macos_pid_fn=activate_macos_pid_fn,
            dispatch_macos_click_fn=dispatch_macos_click_fn,
            desktop_click_selector_fn=desktop_click_selector_fn,
            wait_for_macos_bundle_window_fn=wait_for_macos_bundle_window_fn,
            sleep_fn=sleep_fn,
        )
        pid = capture_result["pid"]
        window = capture_result["window"]
        inspector_summary = capture_result["inspector_summary"]
        interaction_summary = capture_result["interaction_summary"]

        manifest = build_macos_action_manifest(
            action_name=action_name,
            label=label,
            bundle_id=bundle_id,
            launch_command=launch_command,
            pid=pid,
            started_at=started_at,
            completed_at=now_iso_fn(),
            window=window,
            launch_descriptor=launch_descriptor,
            bundle_dir=bundle_dir,
            screenshot_path=screenshot_path,
            before_screenshot_path=before_screenshot_path,
            diff_screenshot_path=diff_screenshot_path,
            ui_snapshot_path=ui_snapshot_path,
            log_path=log_path,
            err_path=err_path,
            capture_before=capture_before,
            interaction_requested=interaction_requested,
            inspector_summary=inspector_summary,
            interaction_summary=interaction_summary,
            image_change_summary_fn=image_change_summary_fn,
        )
        attach_desktop_source_to_manifest_fn(manifest, source_context or source_request)
        atomic_write_text_fn(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
        write_desktop_run_rollups_fn(config, target_name="mac")
        write_desktop_run_rollups_fn(config)
        return manifest
    finally:
        if proc is not None:
            terminate_process_fn(proc)
        else:
            active_bundle_id = bundle_id
            if not active_bundle_id and "launch_descriptor" in locals():
                active_bundle_id = launch_descriptor.get("bundle_id")
            if active_bundle_id:
                quit_macos_bundle_id_fn(active_bundle_id)
