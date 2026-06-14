"""Bindings for Windows session-agent request helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_TARGET_SESSION_REQUEST_EXPORTS = ("build_windows_session_agent_request",)


def build_windows_session_agent_request(
    bindings: dict,
    target_name: str,
    contract: dict,
    command: str,
    *,
    repo_path: str,
    action_name: str,
    label: str | None,
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
) -> dict:
    return _binding(bindings, "_windows_target").build_windows_session_agent_request(
        target_name,
        contract,
        command,
        repo_path=repo_path,
        action_name=action_name,
        label=label,
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
        default_desktop_label_fn=_binding(bindings, "default_desktop_label"),
    )


def install_windows_target_session_request_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_SESSION_REQUEST_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
