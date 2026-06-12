"""Facade dependency bindings for Linux target helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding


def linux_required_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_linux_target").LINUX_REQUIRED_REMOTE_TOOLS


def linux_optional_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_linux_target").LINUX_OPTIONAL_REMOTE_TOOLS


def probe_linux_launch_backend(bindings: dict, host: str) -> dict:
    return _binding(bindings, "_linux_target").probe_linux_launch_backend(
        host,
        ssh_command_result_fn=_binding(bindings, "ssh_command_result"),
    )


def probe_linux_remote_tooling(bindings: dict, host: str) -> dict:
    return _binding(bindings, "_linux_target").probe_linux_remote_tooling(
        host,
        ssh_command_result_fn=_binding(bindings, "ssh_command_result"),
    )


def linux_tooling_detail(
    bindings: dict,
    probe: dict,
    tool_name: str,
    *,
    missing_hint: str | None = None,
) -> str:
    return _binding(bindings, "_linux_target").linux_tooling_detail(
        probe,
        tool_name,
        missing_hint=missing_hint,
    )


def linux_remote_tooling_ready(bindings: dict, probe: dict) -> bool:
    return _binding(bindings, "_linux_target").linux_remote_tooling_ready(
        probe,
        required_tools=_binding(bindings, "LINUX_REQUIRED_REMOTE_TOOLS"),
    )


def remote_linux_bundle_relpath(bindings: dict, target_name: str, action_name: str, bundle_dir: Path) -> str:
    return _binding(bindings, "_linux_target").remote_linux_bundle_relpath(target_name, action_name, bundle_dir)


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
