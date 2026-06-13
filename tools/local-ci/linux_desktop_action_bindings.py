"""Facade bindings for Linux desktop remote action helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from linux_desktop_action_dependency_bindings import linux_desktop_action_dependencies


LINUX_DESKTOP_ACTION_EXPORTS = ("run_linux_xvfb_remote_action",)


def run_linux_xvfb_remote_action(
    bindings: Mapping[str, Any],
    config: dict,
    target_name: str,
    target: dict,
    command: str,
    *,
    action_name: str,
    label: str | None,
    output_path: str | None,
    pulp_app_automation: bool,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
    timeout_secs: float,
    source_request: dict | None = None,
) -> dict:
    return bindings["_linux_desktop_action"].run_linux_xvfb_remote_action(
        config,
        target_name,
        target,
        command,
        action_name=action_name,
        label=label,
        output_path=output_path,
        pulp_app_automation=pulp_app_automation,
        capture_ui_snapshot=capture_ui_snapshot,
        click_point=click_point,
        click_view_id=click_view_id,
        click_view_type=click_view_type,
        click_view_text=click_view_text,
        click_view_label=click_view_label,
        capture_before=capture_before,
        settle_secs=settle_secs,
        timeout_secs=timeout_secs,
        source_request=source_request,
        **linux_desktop_action_dependencies(bindings),
    )


def install_linux_desktop_action_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LINUX_DESKTOP_ACTION_EXPORTS,
) -> None:
    known_names = set(LINUX_DESKTOP_ACTION_EXPORTS)
    action_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), action_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
