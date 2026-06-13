"""Bindings from the local_ci facade to Windows desktop action helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from windows_desktop_action_dependency_bindings import windows_desktop_action_dependencies


WINDOWS_DESKTOP_ACTION_EXPORTS = ("run_windows_session_agent_action",)


def run_windows_session_agent_action(
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
    return bindings["_windows_desktop_action"].run_windows_session_agent_action(
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
        **windows_desktop_action_dependencies(bindings),
    )


def install_windows_desktop_action_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_DESKTOP_ACTION_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
