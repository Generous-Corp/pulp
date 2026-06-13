"""Bindings from the local_ci facade to Linux window-driver command helpers."""

from __future__ import annotations

from binding_utils import binding as _binding


LINUX_TARGET_WINDOW_COMMAND_EXPORTS = ("build_linux_window_driver_remote_command",)


def build_linux_window_driver_remote_command(
    bindings: dict,
    repo_path: str,
    remote_bundle_relpath: str,
    command: str,
    *,
    launch_backend: dict | None = None,
    launch_cwd: str | None = None,
    click_point: str | None,
    capture_before: bool,
    settle_secs: float,
) -> str:
    return _binding(bindings, "_linux_target").build_linux_window_driver_remote_command(
        repo_path,
        remote_bundle_relpath,
        command,
        launch_backend=launch_backend,
        launch_cwd=launch_cwd,
        click_point=click_point,
        capture_before=capture_before,
        settle_secs=settle_secs,
        parse_coordinate_pair_fn=_binding(bindings, "parse_coordinate_pair"),
    )
