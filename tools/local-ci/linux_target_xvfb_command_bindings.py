"""Bindings from the local_ci facade to Linux Xvfb remote command helpers."""

from __future__ import annotations

from binding_utils import binding as _binding


LINUX_TARGET_XVFB_COMMAND_EXPORTS = ("build_linux_xvfb_remote_command",)


def build_linux_xvfb_remote_command(
    bindings: dict,
    repo_path: str,
    remote_bundle_relpath: str,
    command: str,
    *,
    launch_backend: dict | None = None,
    launch_cwd: str | None = None,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
) -> str:
    return _binding(bindings, "_linux_target").build_linux_xvfb_remote_command(
        repo_path,
        remote_bundle_relpath,
        command,
        launch_backend=launch_backend,
        launch_cwd=launch_cwd,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
    )
