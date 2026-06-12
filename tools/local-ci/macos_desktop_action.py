"""macOS desktop action execution helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json
from pathlib import Path

from macos_desktop_action_env import apply_macos_direct_launch_env


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
                apply_macos_direct_launch_env(
                    env,
                    capture_ui_snapshot=capture_ui_snapshot,
                    use_pulp_app_automation=use_pulp_app_automation,
                    click_point=click_point,
                    click_view_id=click_view_id,
                    click_view_type=click_view_type,
                    click_view_text=click_view_text,
                    click_view_label=click_view_label,
                    capture_before=capture_before,
                    ui_snapshot_path=ui_snapshot_path,
                    before_screenshot_path=before_screenshot_path,
                    screenshot_path=screenshot_path,
                    settle_secs=settle_secs,
                )
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
                window = wait_for_macos_window_fn(proc.pid, timeout_secs)
                launch_descriptor = {"command": args}

        inspector_summary = None
        view_tree = None
        content_size = content_size_from_window_fn(window)
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
                active_bundle_id = bundle_id or launch_descriptor.get("bundle_id")
                if not active_bundle_id:
                    raise
                pid, window = wait_for_macos_bundle_window_fn(active_bundle_id, min(timeout_secs, 2.0))
                capture_macos_window_fn(int(window["windowId"]), screenshot_path)

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
