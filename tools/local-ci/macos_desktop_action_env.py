"""Environment assembly for macOS desktop action launches."""

from __future__ import annotations

from pathlib import Path


def apply_macos_desktop_capture_env(
    env: dict[str, str],
    *,
    capture_ui_snapshot: bool,
    ui_snapshot_path: Path,
) -> None:
    if capture_ui_snapshot:
        env["PULP_VIEW_TREE_OUT"] = str(ui_snapshot_path)


def apply_macos_pulp_app_automation_env(
    env: dict[str, str],
    *,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    before_screenshot_path: Path,
    screenshot_path: Path,
    settle_secs: float,
) -> None:
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


def apply_macos_direct_launch_env(
    env: dict[str, str],
    *,
    capture_ui_snapshot: bool,
    use_pulp_app_automation: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    ui_snapshot_path: Path,
    before_screenshot_path: Path,
    screenshot_path: Path,
    settle_secs: float,
) -> None:
    apply_macos_desktop_capture_env(
        env,
        capture_ui_snapshot=capture_ui_snapshot,
        ui_snapshot_path=ui_snapshot_path,
    )
    if use_pulp_app_automation:
        apply_macos_pulp_app_automation_env(
            env,
            click_point=click_point,
            click_view_id=click_view_id,
            click_view_type=click_view_type,
            click_view_text=click_view_text,
            click_view_label=click_view_label,
            capture_before=capture_before,
            before_screenshot_path=before_screenshot_path,
            screenshot_path=screenshot_path,
            settle_secs=settle_secs,
        )
