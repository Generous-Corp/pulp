"""macOS desktop action execution helpers for local CI.

run_macos_local_smoke is the smoke/click/inspect orchestrator: it launches the
target (direct command, app bundle, bundle id, or a Terminal.app proof session),
optionally records a video proof around the interaction, captures screenshots /
ViewInspector state, and assembles the run manifest (including composed +
issue-ready video variants). Video-only helpers live in
macos_desktop_action_video; launch, capture, and manifest orchestration live
here.
"""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path
import shutil
import uuid

from macos_desktop_action_video import (
    _action_marker_summary,
    _component_focus_summary,
    _should_capture_generated_reaper_secondary_window,
    _terminate_pid,
    _validate_generated_reaper_recipe_status,
    _wait_for_log_text,
)


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
    wait_for_macos_bundle_secondary_window_fn: Callable[[str, float], tuple[int, dict]],
    split_command_fn: Callable[[str], list[str]],
    detect_macos_app_bundle_fn: Callable[[str | None], Path | None],
    macos_bundle_id_for_app_path_fn: Callable[[Path], str | None],
    environ_copy_fn: Callable[[], dict[str, str]],
    cwd_path_fn: Callable[[], Path],
    launch_macos_terminal_proof_command_fn: Callable[..., dict],
    close_macos_terminal_windows_with_title_fn: Callable[[str], dict],
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
    mux_desktop_video_audio_fn: Callable[..., dict],
    generate_interaction_focus_fn: Callable[..., dict] | None = None,
    compose_desktop_video_proof_fn: Callable[[Path, Path], dict],
    create_issue_video_variant_fn: Callable[..., dict],
    attach_desktop_source_to_manifest_fn: Callable[[dict, dict | None], None],
    atomic_write_text_fn: Callable[[Path, str], None],
    write_desktop_run_rollups_fn: Callable[..., None],
    terminate_process_fn: Callable[[object], None],
    record_video: bool = False,
    video_duration_secs: float = 8.0,
    video_fps: float = 30.0,
    video_audio_source: str = "none",
    video_audio_file: str | None = None,
    video_audio_device: str | None = None,
    video_recorder: str = "auto",
    video_focus: str = "auto",
    video_capture_target: str = "app",
    capture_bundle_id: str | None = None,
    video_attachment_budget_bytes: int = 100_000_000,
    small_video: bool = False,
    small_video_budget_bytes: int = 10_000_000,
    compose_video_proof: bool = False,
    video_template: str | None = None,
    video_source_image: str | None = None,
    video_source_label: str | None = None,
    video_title: str | None = None,
    video_notes: list[str] | None = None,
    video_context: dict | None = None,
) -> dict:
    bundle_dir = create_desktop_run_bundle_fn(config, "mac", action_name)
    action_paths = desktop_action_artifact_paths_fn(bundle_dir, output_path)
    screenshot_path = action_paths["screenshot"]
    before_screenshot_path = action_paths["before_screenshot"]
    diff_screenshot_path = action_paths["diff_screenshot"]
    ui_snapshot_path = action_paths["ui_snapshot"]
    video_path = action_paths["video"]
    video_audio_path = action_paths["video_audio"]
    video_composed_path = action_paths["video_composed"]
    video_issue_path = action_paths["video_issue"]
    video_small_path = action_paths["video_small"]
    video_metadata_path = action_paths["video_metadata"]
    video_composed_metadata_path = action_paths["video_composed_metadata"]
    video_issue_metadata_path = action_paths["video_issue_metadata"]
    video_small_metadata_path = action_paths["video_small_metadata"]
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
    if video_recorder not in {"auto", "avfoundation", "frame-sequence"}:
        raise RuntimeError(f"Unknown video recorder `{video_recorder}`.")
    if video_focus not in {"auto", "off"}:
        raise RuntimeError(f"Unknown video focus mode `{video_focus}` (expected auto or off).")
    if video_recorder == "frame-sequence" and video_audio_source == "system":
        raise RuntimeError("--video-audio system requires --video-recorder auto or avfoundation.")
    if video_audio_source == "plugin":
        if not video_audio_file:
            raise RuntimeError("--video-audio plugin requires --video-audio-file.")
        source_audio_path = Path(video_audio_file).expanduser().resolve()
        if not source_audio_path.exists():
            raise RuntimeError(f"--video-audio-file does not exist: {source_audio_path}")
    elif video_audio_file:
        raise RuntimeError("--video-audio-file is only valid with --video-audio plugin.")
    else:
        source_audio_path = None
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
        raise RuntimeError(
            "macOS desktop interaction (synthetic clicks) requires Accessibility permission for the "
            "controlling app. Grant it in System Settings > Privacy & Security > Accessibility, enable "
            "Terminal.app (the same app that needs Screen Recording for capture), then quit and reopen "
            "Terminal so the permission takes effect. Re-run with --run-in-terminal."
        )
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
    interaction_content_point: tuple[float, float] | None = None
    terminal_title: str | None = None
    terminal_cleanup: dict | None = None
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
                        env["PULP_AUTOMATION_EXIT_AFTER"] = "0" if record_video else "1"
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

        if capture_bundle_id and _should_capture_generated_reaper_secondary_window(video_context):
            _wait_for_log_text(
                log_path,
                "TrackFX_Show floating-editor mode=3",
                min(timeout_secs, 15.0),
                sleep_fn=sleep_fn,
            )
            original_window = dict(window)
            try:
                pid, window = wait_for_macos_bundle_secondary_window_fn(capture_bundle_id, min(timeout_secs, 10.0))
            except RuntimeError as exc:
                raise RuntimeError(
                    "Generated REAPER proof requested a floating editor, but no secondary REAPER window was visible to capture."
                ) from exc
            launch_descriptor["capture_window_refinement"] = {
                "strategy": "floating-editor-secondary-window-after-reaper-marker",
                "from": original_window,
                "to": window,
            }

        inspector_summary = None
        view_tree = None
        content_size = content_size_from_window_fn(window)
        if record_video:
            recorder_audio_source = "none" if video_audio_source == "plugin" else video_audio_source
            # Keep the captured window frontmost during frame-sequence capture so
            # macOS does not pause its (occluded) render loop and freeze the
            # recording on a single frame. The proof is driven from a terminal,
            # which would otherwise stay frontmost and occlude the target.
            recording_activate_fn = (
                (lambda _pid=int(pid): activate_macos_pid_fn(_pid)) if pid else None
            )
            video_recording = start_macos_window_video_recording_fn(
                window,
                video_path,
                duration_secs=video_duration_secs,
                fps=video_fps,
                audio_source=recorder_audio_source,
                audio_device=None if video_audio_source == "plugin" else video_audio_device,
                prefer_frame_sequence=video_recorder == "frame-sequence" or bool(capture_bundle_id),
                activate_fn=recording_activate_fn,
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
                interaction_content_point = (float(content_point[0]), float(content_point[1]))
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
            if video_audio_source == "plugin" and source_audio_path is not None:
                video_audio_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source_audio_path, video_audio_path)
                audio_mux = mux_desktop_video_audio_fn(
                    video_path,
                    video_audio_path,
                    attachment_budget_bytes=video_attachment_budget_bytes,
                )
                video_summary["has_audio"] = True
                video_summary["audio_source"] = "plugin"
                video_summary["audio_file"] = str(video_audio_path)
                video_summary["audio_mux"] = audio_mux
                if "size" in audio_mux:
                    video_summary["size"] = audio_mux["size"]
                video_metadata_path.write_text(json.dumps(video_summary, indent=2) + "\n")
            # Codified "zoom to the demoed control": when an interaction has a
            # known click point, auto-produce a focused clip cropped+scaled to
            # that control so the on-screen change is clearly visible, not a
            # speck in the full window. No-interaction smoke runs skip this, and
            # `--video-focus off` opts out to keep the full-window framing.
            if (
                video_focus != "off"
                and generate_interaction_focus_fn is not None
                and interaction_content_point is not None
                and video_summary is not None
                and Path(video_path).exists()
            ):
                focus_path = Path(video_path).with_name("proof.focus.mp4")
                focus_summary = generate_interaction_focus_fn(
                    Path(video_path),
                    focus_path,
                    content_point=interaction_content_point,
                    window_bounds=window.get("bounds") or {},
                    scale=float(window.get("scale") or 1.0),
                )
                video_summary["focus"] = focus_summary
                if focus_summary.get("status") == "created":
                    video_summary.setdefault("artifacts", {})["video_focus"] = str(focus_path)
                video_metadata_path.write_text(json.dumps(video_summary, indent=2) + "\n")
        _validate_generated_reaper_recipe_status(video_context, log_path)

        terminal_returncode = None
        if video_capture_target == "terminal":
            try:
                wait_for_path_fn(terminal_returncode_path, max(2.0, min(timeout_secs, video_duration_secs + 4.0)))
                terminal_returncode = int(terminal_returncode_path.read_text().strip())
            except (OSError, ValueError, RuntimeError):
                terminal_returncode = None
            if terminal_title:
                terminal_cleanup = close_macos_terminal_windows_with_title_fn(terminal_title)

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
        focus_summary = _component_focus_summary(
            video_template=video_template,
            interaction_summary=interaction_summary,
            content_size=content_size,
        )
        action_marker = _action_marker_summary(
            interaction_summary=interaction_summary,
            content_size=content_size,
        )
        if focus_summary is not None:
            composition = manifest.setdefault("video_proof_composition", {})
            composition["template"] = video_template or composition.get("template") or "component-zoom"
            composition["focus"] = focus_summary
        if action_marker is not None:
            composition = manifest.setdefault("video_proof_composition", {})
            composition["action_marker"] = action_marker
        cleaned_video_context = {
            str(key): str(value)
            for key, value in (video_context or {}).items()
            if key and value is not None and str(value)
        }
        if cleaned_video_context:
            composition = manifest.setdefault("video_proof_composition", {})
            composition["context"] = cleaned_video_context
        if video_capture_target == "terminal":
            manifest["terminal"] = {
                "returncode": terminal_returncode,
                "returncode_path": str(terminal_returncode_path),
            }
            if terminal_cleanup is not None:
                manifest["terminal"]["cleanup"] = terminal_cleanup
            manifest["artifacts"]["terminal_returncode"] = str(terminal_returncode_path)
        if video_summary is not None:
            manifest["video"] = video_summary
            if video_path.exists():
                manifest["artifacts"]["video"] = str(video_path)
            if video_metadata_path.exists():
                manifest["artifacts"]["video_metadata"] = str(video_metadata_path)
            if video_poster_path.exists():
                manifest["artifacts"]["video_poster"] = str(video_poster_path)
            focus_clip_path = video_path.with_name("proof.focus.mp4")
            if (
                isinstance(video_summary, dict)
                and (video_summary.get("focus") or {}).get("status") == "created"
                and focus_clip_path.exists()
            ):
                manifest["artifacts"]["video_focus"] = str(focus_clip_path)
            if video_audio_path.exists():
                manifest["artifacts"]["video_audio"] = str(video_audio_path)
        attach_desktop_source_to_manifest_fn(manifest, source_context or source_request)
        if compose_video_proof and video_path.exists():
            source_manifest_path = bundle_dir / "manifest.video-source.json"
            cleaned_video_notes = [note for note in (video_notes or []) if note]
            if cleaned_video_notes:
                manifest["video_proof_notes"] = cleaned_video_notes
                composition = manifest.setdefault("video_proof_composition", {})
                composition["notes"] = cleaned_video_notes
            # SINGLE SOURCE OF TRUTH for what the Remotion proof embeds: the
            # auto-focused interaction crop when it exists, otherwise the
            # full-window capture. `embed_video` drives BOTH the embedded
            # recording and whether overlay coordinates are remapped, so the tap
            # marker / zoom-center always match whatever is shown (full vs
            # zoomed). Keep these coupled — they are the thing that breaks if
            # they ever drift apart.
            focus_clip_for_compose = video_path.with_name("proof.focus.mp4")
            focus_meta = video_summary.get("focus") if isinstance(video_summary, dict) else None
            embed_focus = bool(
                isinstance(focus_meta, dict)
                and focus_meta.get("status") == "created"
                and focus_clip_for_compose.exists()
            )
            embed_video = focus_clip_for_compose if embed_focus else None
            # When (and only when) the focus crop is embedded, remap the tap
            # marker + zoom-center from full-window coordinates into the crop's
            # frame. For a full-window embed the original full-window coordinates
            # are already correct, so they are left untouched.
            if (
                embed_focus
                and isinstance(focus_meta.get("crop"), dict)
                and isinstance(focus_meta.get("content_point"), dict)
            ):
                crop = focus_meta["crop"]
                cp = focus_meta["content_point"]
                fscale = float(window.get("scale") or 1.0) or 1.0
                cw = float(crop.get("width") or 0.0)
                ch = float(crop.get("height") or 0.0)
                if cw > 0 and ch > 0:
                    px = float(cp.get("x", 0.0)) * fscale
                    py = float(cp.get("y", 0.0)) * fscale
                    nx = max(0.0, min(1.0, (px - float(crop.get("x", 0.0))) / cw))
                    ny = max(0.0, min(1.0, (py - float(crop.get("y", 0.0))) / ch))
                    composition = manifest.setdefault("video_proof_composition", {})
                    if isinstance(composition.get("action_marker"), dict):
                        composition["action_marker"]["normalized_point"] = {"x": nx, "y": ny}
                    if isinstance(composition.get("focus"), dict):
                        composition["focus"]["normalized_center"] = {"x": nx, "y": ny}
            atomic_write_text_fn(source_manifest_path, json.dumps(manifest, indent=2) + "\n")
            source_image_path = Path(video_source_image).expanduser().resolve() if video_source_image else None
            composed_summary = compose_desktop_video_proof_fn(
                source_manifest_path,
                video_composed_path,
                template=video_template,
                source_image=source_image_path,
                source_label=video_source_label,
                title=video_title,
                notes=cleaned_video_notes,
                video=embed_video,
            )
            manifest["video_composed"] = composed_summary
            if any([video_template, source_image_path, video_source_label, video_title, cleaned_video_notes, cleaned_video_context]):
                composition = manifest.setdefault("video_proof_composition", {})
                composition.update(
                    {
                        "template": video_template or composition.get("template") or "validation-proof",
                        "source_image": str(source_image_path) if source_image_path else composition.get("source_image"),
                        "source_label": video_source_label,
                        "title": video_title,
                    }
                )
                if cleaned_video_notes:
                    composition["notes"] = cleaned_video_notes
                if cleaned_video_context:
                    composition["context"] = cleaned_video_context
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
            if small_video:
                small_summary = create_issue_video_variant_fn(
                    issue_source_path,
                    video_small_path,
                    video_small_metadata_path,
                    attachment_budget_bytes=small_video_budget_bytes,
                )
                manifest["video_small"] = small_summary
                if video_small_path.exists():
                    manifest["artifacts"]["video_small"] = str(video_small_path)
                if video_small_metadata_path.exists():
                    manifest["artifacts"]["video_small_metadata"] = str(video_small_metadata_path)
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
            if capture_bundle_id and _should_capture_generated_reaper_secondary_window(video_context) and pid is not None:
                _terminate_pid(int(pid), sleep_fn=sleep_fn)
            terminate_process_fn(proc)
        active_bundle_id = bundle_id
        if not active_bundle_id and "launch_descriptor" in locals():
            active_bundle_id = launch_descriptor.get("capture_bundle_id") or launch_descriptor.get("bundle_id")
        if active_bundle_id:
            try:
                quit_macos_bundle_id_fn(active_bundle_id)
            except Exception:
                pass
