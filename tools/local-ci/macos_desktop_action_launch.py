"""macOS desktop action launch-mode helpers."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path

from macos_desktop_action_env import apply_macos_direct_launch_env


def launch_macos_desktop_action(
    *,
    bundle_id: str | None,
    launch_command: str | None,
    launch_cwd: str | None,
    capture_ui_snapshot: bool,
    use_pulp_app_automation: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    ui_snapshot_path: Path,
    before_screenshot_path: Path,
    screenshot_path: Path,
    log_path: Path,
    err_path: Path,
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
) -> dict:
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
        return {
            "proc": None,
            "pid": pid,
            "window": window,
            "launch_descriptor": {"bundle_id": bundle_id},
        }

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
        return {
            "proc": None,
            "pid": pid,
            "window": window,
            "launch_descriptor": {"bundle_id": inferred_bundle_id, "app_path": str(app_bundle)},
        }

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
    return {
        "proc": proc,
        "pid": pid,
        "window": window,
        "launch_descriptor": {"command": args},
    }
