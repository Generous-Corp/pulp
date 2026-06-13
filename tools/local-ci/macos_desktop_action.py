"""macOS desktop action execution helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import uuid


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
    wait_for_macos_bundle_window_title_fn: Callable[[str, str, float], tuple[int, dict]],
    split_command_fn: Callable[[str], list[str]],
    detect_macos_app_bundle_fn: Callable[[str | None], Path | None],
    macos_bundle_id_for_app_path_fn: Callable[[Path], str | None],
    environ_copy_fn: Callable[[], dict[str, str]],
    cwd_path_fn: Callable[[], Path],
    launch_macos_terminal_proof_command_fn: Callable[..., dict],
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
    start_macos_window_video_recording_fn: Callable[..., dict],
    stop_macos_window_video_recording_fn: Callable[..., dict],
    compose_desktop_video_proof_fn: Callable[[Path, Path], dict],
    create_issue_video_variant_fn: Callable[..., dict],
    attach_desktop_source_to_manifest_fn: Callable[[dict, dict | None], None],
    atomic_write_text_fn: Callable[[Path, str], None],
    write_desktop_run_rollups_fn: Callable[..., None],
    terminate_process_fn: Callable[[object], None],
    record_video: bool = False,
    video_duration_secs: float = 8.0,
    video_fps: float = 30.0,
    video_capture_target: str = "app",
    capture_bundle_id: str | None = None,
    video_attachment_budget_bytes: int = 100_000_000,
    compose_video_proof: bool = False,
    video_template: str | None = None,
    video_source_image: str | None = None,
    video_source_label: str | None = None,
    video_title: str | None = None,
) -> dict:
    bundle_dir = create_desktop_run_bundle_fn(config, "mac", action_name)
    action_paths = desktop_action_artifact_paths_fn(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    video_path = action_paths["video"]
    video_composed_path = action_paths["video_composed"]
    video_issue_path = action_paths["video_issue"]
    video_metadata_path = action_paths["video_metadata"]
    video_composed_metadata_path = action_paths["video_composed_metadata"]
    video_issue_metadata_path = action_paths["video_issue_metadata"]
    video_poster_path = action_paths["video_poster"]
    terminal_returncode_path = bundle_dir / "terminal-returncode.txt"
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
    if video_capture_target not in {"app", "terminal"}:
        raise RuntimeError(f"Unknown video capture target `{video_capture_target}`.")
    if capture_bundle_id and bundle_id:
        raise RuntimeError("--capture-bundle-id is only valid with --command.")
    if capture_bundle_id and video_capture_target == "terminal":
        raise RuntimeError("--capture-bundle-id cannot be combined with --video-capture-target terminal.")
    if capture_bundle_id and capture_ui_snapshot:
        raise RuntimeError("--capture-bundle-id cannot inject UI snapshot environment into the captured app.")
    if video_capture_target == "terminal":
        if not record_video:
            raise RuntimeError("Terminal capture requires --record-video.")
        if bundle_id:
            raise RuntimeError("Terminal capture requires --command, not --bundle-id.")
        if capture_ui_snapshot:
            raise RuntimeError("Terminal capture cannot collect a Pulp ViewInspector UI snapshot.")
        if interaction_requested:
            raise RuntimeError("Terminal capture currently supports smoke actions only; use app capture for clicks.")
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
    video_recording = None
    video_summary = None
    try:
        if bundle_id:
            if capture_ui_snapshot:
                raise RuntimeError(
                    "UI snapshot capture currently requires a direct launch command so PULP_VIEW_TREE_OUT can be injected."
                )
            log_path.write_text("")
            err_path.write_text("")
            quit_macos_bundle_id_fn(bundle_id)
            sleep_fn(0.2)
            run_fn(["open", "-b", bundle_id], capture_output=True, text=True, check=True)
            sleep_fn(0.75)
            activate_macos_bundle_id_fn(bundle_id)
            sleep_fn(0.75)
            pid, window = wait_for_macos_bundle_window_fn(bundle_id, timeout_secs)
            launch_descriptor = {"bundle_id": bundle_id}
        else:
            args = split_command_fn(launch_command or "")
            if not args:
                raise ValueError("Desktop smoke requires either --command or --bundle-id.")
            if video_capture_target == "terminal":
                terminal_title = f"Pulp Video Proof {uuid.uuid4().hex[:8]}"
                terminal_session = launch_macos_terminal_proof_command_fn(
                    args,
                    cwd=Path(launch_cwd) if launch_cwd else cwd_path_fn(),
                    title=terminal_title,
                    stdout_path=log_path,
                    stderr_path=err_path,
                    returncode_path=terminal_returncode_path,
                    keepalive_secs=max(video_duration_secs + 2.0, settle_secs + 2.0),
                )
                sleep_fn(0.75)
                pid, window = wait_for_macos_bundle_window_title_fn("com.apple.Terminal", terminal_title, timeout_secs)
                launch_descriptor = {"command": args, "terminal": terminal_session}
            else:
                app_bundle = detect_macos_app_bundle_fn(launch_command)
                if app_bundle is not None:
                    if capture_ui_snapshot:
                        raise RuntimeError(
                            "UI snapshot capture currently requires a direct launch command so PULP_VIEW_TREE_OUT can be injected."
                        )
                    inferred_bundle_id = macos_bundle_id_for_app_path_fn(app_bundle)
                    if not inferred_bundle_id:
                        raise RuntimeError(f"Could not determine bundle id for app bundle `{app_bundle}`")
                    log_path.write_text("")
                    err_path.write_text("")
                    quit_macos_bundle_id_fn(inferred_bundle_id)
                    sleep_fn(0.2)
                    run_fn(["open", "-a", str(app_bundle)], capture_output=True, text=True, check=True)
                    sleep_fn(0.75)
                    activate_macos_bundle_id_fn(inferred_bundle_id)
                    sleep_fn(0.75)
                    pid, window = wait_for_macos_bundle_window_fn(inferred_bundle_id, timeout_secs)
                    launch_descriptor = {"bundle_id": inferred_bundle_id, "app_path": str(app_bundle)}
                else:
                    stdout_handle = log_path.open("w")
                    stderr_handle = err_path.open("w")
                    env = environ_copy_fn()
                    if capture_ui_snapshot:
                        env["PULP_VIEW_TREE_OUT"] = str(ui_snapshot_path)
                    if use_pulp_app_automation:
                        if click_point:
                            env["PULP_AUTOMATION_CLICK_POINT"] = click_point
                        if click_view_id:
                            env["PULP_AUTOMATION_CLICK_VIEW_ID"] = click_view_id
                        if click_view_type:
                            env["PULP_AUTOMATION_CLICK_VIEW_TYPE"] = click_view_type
                        if click_view_text:
                            env["PULP_AUTOMATION_CLICK_VIEW_TEXT"] = click_view_text
                        if click_view_label:
                            env["PULP_AUTOMATION_CLICK_VIEW_LABEL"] = click_view_label
                        if capture_before:
                            env["PULP_AUTOMATION_BEFORE_OUT"] = str(before_screenshot_path)
                        env["PULP_AUTOMATION_AFTER_OUT"] = str(screenshot_path)
                        env["PULP_AUTOMATION_DELAY_MS"] = "1000"
                        env["PULP_AUTOMATION_AFTER_DELAY_MS"] = str(max(0, int(settle_secs * 1000.0)))
                        env["PULP_AUTOMATION_EXIT_AFTER"] = "1"
                    try:
                        proc = popen_fn(
                            args,
                            stdout=stdout_handle,
                            stderr=stderr_handle,
                            env=env,
                            cwd=launch_cwd,
                        )
                    finally:
                        stdout_handle.close()
                        stderr_handle.close()
                    pid = proc.pid
                    if capture_bundle_id:
                        sleep_fn(0.75)
                        activate_macos_bundle_id_fn(capture_bundle_id)
                        sleep_fn(0.75)
                        pid, window = wait_for_macos_bundle_window_fn(capture_bundle_id, timeout_secs)
                        launch_descriptor = {"command": args, "capture_bundle_id": capture_bundle_id}
                    else:
                        window = wait_for_macos_window_fn(proc.pid, timeout_secs)
                        launch_descriptor = {"command": args}

        inspector_summary = None
        view_tree = None
        content_size = content_size_from_window_fn(window)
        if record_video:
            video_recording = start_macos_window_video_recording_fn(
                window,
                video_path,
                duration_secs=video_duration_secs,
                fps=video_fps,
                prefer_frame_sequence=bool(capture_bundle_id),
            )
        if capture_ui_snapshot and not use_pulp_app_automation:
            wait_for_path_fn(ui_snapshot_path, timeout_secs)
            view_tree = json.loads(ui_snapshot_path.read_text())
            content_size = content_size_from_view_tree_fn(view_tree, content_size)
            inspector_summary = view_tree_inspector_summary_fn(view_tree)

        interaction_summary = None
        if use_pulp_app_automation:
            if capture_before:
                wait_for_path_fn(before_screenshot_path, timeout_secs)
            wait_for_path_fn(screenshot_path, timeout_secs)
            if capture_ui_snapshot:
                wait_for_path_fn(ui_snapshot_path, timeout_secs)
                view_tree = json.loads(ui_snapshot_path.read_text())
                content_size = content_size_from_view_tree_fn(view_tree, content_size)
                inspector_summary = view_tree_inspector_summary_fn(view_tree)
            interaction_summary = pulp_app_interaction_summary_fn(
                click_point=click_point,
                click_view_id=click_view_id,
                click_view_type=click_view_type,
                click_view_text=click_view_text,
                click_view_label=click_view_label,
            )
        else:
            if interaction_requested and capture_before:
                capture_macos_window_fn(int(window["windowId"]), before_screenshot_path)

            if interaction_requested:
                if click_point:
                    content_point = parse_coordinate_pair_fn(click_point, flag_name="--click")
                else:
                    content_point = resolve_view_tree_click_point_fn(
                        view_tree or {},
                        view_id=click_view_id,
                        view_type=click_view_type,
                        view_text=click_view_text,
                        view_label=click_view_label,
                    )
                screen_point = screen_point_for_content_point_fn(window, content_size, content_point)
                activation_payload = activate_macos_pid_fn(int(pid or 0)) if pid else {"activated": False}
                dispatch_payload = dispatch_macos_click_fn(*screen_point)
                interaction_summary = {
                    "mode": "desktop-event",
                    "click": {
                        "content_point": {"x": content_point[0], "y": content_point[1]},
                        "screen_point": {"x": screen_point[0], "y": screen_point[1]},
                        "selector": desktop_click_selector_fn(
                            click_view_id=click_view_id,
                            click_view_type=click_view_type,
                            click_view_text=click_view_text,
                            click_view_label=click_view_label,
                            include_point=False,
                        ),
                        "activation": activation_payload,
                        "dispatch": dispatch_payload,
                    },
                }
                if settle_secs > 0:
                    sleep_fn(settle_secs)

            try:
                capture_macos_window_fn(int(window["windowId"]), screenshot_path)
            except RuntimeError:
                active_bundle_id = bundle_id or launch_descriptor.get("capture_bundle_id") or launch_descriptor.get("bundle_id")
                if not active_bundle_id:
                    raise
                pid, window = wait_for_macos_bundle_window_fn(active_bundle_id, min(timeout_secs, 2.0))
                capture_macos_window_fn(int(window["windowId"]), screenshot_path)

        if video_recording is not None:
            video_summary = stop_macos_window_video_recording_fn(
                video_recording,
                output_path=video_path,
                metadata_path=video_metadata_path,
                poster_path=video_poster_path,
                duration_secs=video_duration_secs,
                fps=video_fps,
                attachment_budget_bytes=video_attachment_budget_bytes,
            )
            video_recording = None

        terminal_returncode = None
        if video_capture_target == "terminal":
            try:
                wait_for_path_fn(terminal_returncode_path, max(2.0, min(timeout_secs, video_duration_secs + 4.0)))
                terminal_returncode = int(terminal_returncode_path.read_text().strip())
            except (OSError, ValueError, RuntimeError):
                terminal_returncode = None

        manifest = {
            "target": "mac",
            "adapter": "macos-local",
            "action": action_name,
            "label": label or (bundle_id or Path((launch_command or "").split()[0]).stem),
            "pid": pid,
            "started_at": started_at,
            "completed_at": now_iso_fn(),
            "window": window,
            **launch_descriptor,
            "artifacts": {
                "bundle_dir": str(bundle_dir),
                "screenshot": str(screenshot_path),
                "stdout": str(log_path),
                "stderr": str(err_path),
            },
        }
        if capture_before and interaction_requested:
            manifest["artifacts"]["before_screenshot"] = str(before_screenshot_path)
            if before_screenshot_path.exists() and screenshot_path.exists():
                manifest["artifacts"]["image_change"] = image_change_summary_fn(
                    before_screenshot_path,
                    screenshot_path,
                    diff_output_path=diff_screenshot_path,
                )
                if diff_screenshot_path.exists():
                    manifest["artifacts"]["diff_screenshot"] = str(diff_screenshot_path)
        if inspector_summary is not None:
            manifest["artifacts"]["ui_snapshot"] = str(ui_snapshot_path)
            manifest["inspector"] = inspector_summary
        if interaction_summary is not None:
            manifest["interaction"] = interaction_summary
        if video_capture_target == "terminal":
            manifest["terminal"] = {
                "returncode": terminal_returncode,
                "returncode_path": str(terminal_returncode_path),
            }
            manifest["artifacts"]["terminal_returncode"] = str(terminal_returncode_path)
        if video_summary is not None:
            manifest["video"] = video_summary
            if video_path.exists():
                manifest["artifacts"]["video"] = str(video_path)
            if video_metadata_path.exists():
                manifest["artifacts"]["video_metadata"] = str(video_metadata_path)
            if video_poster_path.exists():
                manifest["artifacts"]["video_poster"] = str(video_poster_path)
        attach_desktop_source_to_manifest_fn(manifest, source_context or source_request)
        if compose_video_proof and video_path.exists():
            source_manifest_path = bundle_dir / "manifest.video-source.json"
            atomic_write_text_fn(source_manifest_path, json.dumps(manifest, indent=2) + "\n")
            source_image_path = Path(video_source_image).expanduser().resolve() if video_source_image else None
            composed_summary = compose_desktop_video_proof_fn(
                source_manifest_path,
                video_composed_path,
                template=video_template,
                source_image=source_image_path,
                source_label=video_source_label,
                title=video_title,
            )
            manifest["video_composed"] = composed_summary
            if any([video_template, source_image_path, video_source_label, video_title]):
                manifest["video_proof_composition"] = {
                    "template": video_template or "validation-proof",
                    "source_image": str(source_image_path) if source_image_path else None,
                    "source_label": video_source_label,
                    "title": video_title,
                }
            if video_composed_path.exists():
                manifest["artifacts"]["video_composed"] = str(video_composed_path)
            atomic_write_text_fn(video_composed_metadata_path, json.dumps(composed_summary, indent=2) + "\n")
            manifest["artifacts"]["video_composed_metadata"] = str(video_composed_metadata_path)
        if video_summary is not None:
            issue_source_path = video_composed_path if video_composed_path.exists() else video_path
            issue_summary = create_issue_video_variant_fn(
                issue_source_path,
                video_issue_path,
                video_issue_metadata_path,
                attachment_budget_bytes=video_attachment_budget_bytes,
            )
            manifest["video_issue"] = issue_summary
            if video_issue_path.exists():
                manifest["artifacts"]["video_issue"] = str(video_issue_path)
            if video_issue_metadata_path.exists():
                manifest["artifacts"]["video_issue_metadata"] = str(video_issue_metadata_path)
        atomic_write_text_fn(bundle_dir / "manifest.json", json.dumps(manifest, indent=2) + "\n")
        write_desktop_run_rollups_fn(config, target_name="mac")
        write_desktop_run_rollups_fn(config)
        return manifest
    finally:
        if video_recording is not None:
            try:
                stop_macos_window_video_recording_fn(
                    video_recording,
                    output_path=video_path,
                    metadata_path=video_metadata_path,
                    poster_path=video_poster_path,
                    duration_secs=video_duration_secs,
                    fps=video_fps,
                    attachment_budget_bytes=video_attachment_budget_bytes,
                )
            except RuntimeError:
                pass
        if proc is not None:
            terminate_process_fn(proc)
        active_bundle_id = bundle_id
        if not active_bundle_id and "launch_descriptor" in locals():
            active_bundle_id = launch_descriptor.get("capture_bundle_id") or launch_descriptor.get("bundle_id")
        if active_bundle_id:
            quit_macos_bundle_id_fn(active_bundle_id)
