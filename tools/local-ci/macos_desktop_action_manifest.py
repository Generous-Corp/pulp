"""Manifest assembly for macOS desktop action runs."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


def macos_action_label(*, label: str | None, bundle_id: str | None, launch_command: str | None) -> str:
    return label or (bundle_id or Path((launch_command or "").split()[0]).stem)


def macos_action_base_manifest(
    *,
    action_name: str,
    label: str | None,
    bundle_id: str | None,
    launch_command: str | None,
    pid: int | None,
    started_at: str,
    completed_at: str,
    window: dict,
    launch_descriptor: dict,
    bundle_dir: Path,
    screenshot_path: Path,
    log_path: Path,
    err_path: Path,
) -> dict:
    return {
        "target": "mac",
        "adapter": "macos-local",
        "action": action_name,
        "label": macos_action_label(label=label, bundle_id=bundle_id, launch_command=launch_command),
        "pid": pid,
        "started_at": started_at,
        "completed_at": completed_at,
        "window": window,
        **launch_descriptor,
        "artifacts": {
            "bundle_dir": str(bundle_dir),
            "screenshot": str(screenshot_path),
            "stdout": str(log_path),
            "stderr": str(err_path),
        },
    }


def attach_macos_before_diff_artifacts(
    manifest: dict,
    *,
    capture_before: bool,
    interaction_requested: bool,
    before_screenshot_path: Path,
    screenshot_path: Path,
    diff_screenshot_path: Path,
    image_change_summary_fn: Callable[..., dict],
) -> None:
    if not (capture_before and interaction_requested):
        return
    manifest["artifacts"]["before_screenshot"] = str(before_screenshot_path)
    if before_screenshot_path.exists() and screenshot_path.exists():
        manifest["artifacts"]["image_change"] = image_change_summary_fn(
            before_screenshot_path,
            screenshot_path,
            diff_output_path=diff_screenshot_path,
        )
        if diff_screenshot_path.exists():
            manifest["artifacts"]["diff_screenshot"] = str(diff_screenshot_path)


def attach_macos_optional_manifest_sections(
    manifest: dict,
    *,
    inspector_summary: dict | None,
    interaction_summary: dict | None,
    ui_snapshot_path: Path,
) -> None:
    if inspector_summary is not None:
        manifest["artifacts"]["ui_snapshot"] = str(ui_snapshot_path)
        manifest["inspector"] = inspector_summary
    if interaction_summary is not None:
        manifest["interaction"] = interaction_summary


def build_macos_action_manifest(
    *,
    action_name: str,
    label: str | None,
    bundle_id: str | None,
    launch_command: str | None,
    pid: int | None,
    started_at: str,
    completed_at: str,
    window: dict,
    launch_descriptor: dict,
    bundle_dir: Path,
    screenshot_path: Path,
    before_screenshot_path: Path,
    diff_screenshot_path: Path,
    ui_snapshot_path: Path,
    log_path: Path,
    err_path: Path,
    capture_before: bool,
    interaction_requested: bool,
    inspector_summary: dict | None,
    interaction_summary: dict | None,
    image_change_summary_fn: Callable[..., dict],
) -> dict:
    manifest = macos_action_base_manifest(
        action_name=action_name,
        label=label,
        bundle_id=bundle_id,
        launch_command=launch_command,
        pid=pid,
        started_at=started_at,
        completed_at=completed_at,
        window=window,
        launch_descriptor=launch_descriptor,
        bundle_dir=bundle_dir,
        screenshot_path=screenshot_path,
        log_path=log_path,
        err_path=err_path,
    )
    attach_macos_before_diff_artifacts(
        manifest,
        capture_before=capture_before,
        interaction_requested=interaction_requested,
        before_screenshot_path=before_screenshot_path,
        screenshot_path=screenshot_path,
        diff_screenshot_path=diff_screenshot_path,
        image_change_summary_fn=image_change_summary_fn,
    )
    attach_macos_optional_manifest_sections(
        manifest,
        inspector_summary=inspector_summary,
        interaction_summary=interaction_summary,
        ui_snapshot_path=ui_snapshot_path,
    )
    return manifest
