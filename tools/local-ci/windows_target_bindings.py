"""Facade dependency bindings for Windows target helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_TARGET_EXPORTS = (
    "default_windows_session_task_name",
    "desktop_target_contract",
    "windows_path_join",
    "windows_default_repo_checkout_path",
    "windows_repo_path_is_unsafe",
    "update_target_repo_path",
    "windows_repo_checkout_ready",
    "build_windows_session_agent_request",
    "windows_tooling_detail",
    "windows_remote_tooling_ready",
    "windows_desktop_session_user",
    "windows_desktop_session_state",
    "windows_repo_checkout_detail",
)


def windows_required_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_windows_target").WINDOWS_REQUIRED_REMOTE_TOOLS


def windows_optional_remote_tools(bindings: dict) -> dict:
    return _binding(bindings, "_windows_target").WINDOWS_OPTIONAL_REMOTE_TOOLS


def windows_default_remote_repo_dirname(bindings: dict) -> str:
    return _binding(bindings, "_windows_target").WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME


def default_windows_session_task_name(bindings: dict, target_name: str) -> str:
    return _binding(bindings, "_windows_target").default_windows_session_task_name(target_name)


def desktop_target_contract(bindings: dict, target_name: str, target: dict) -> dict:
    return _binding(bindings, "_windows_target").desktop_target_contract(target_name, target)


def windows_path_join(bindings: dict, *parts: str) -> str:
    return _binding(bindings, "_windows_target").windows_path_join(*parts)


def windows_default_repo_checkout_path(bindings: dict, home_dir: str | None) -> str:
    return _binding(bindings, "_windows_target").windows_default_repo_checkout_path(home_dir)


def windows_repo_path_is_unsafe(bindings: dict, repo_path: str | None, home_dir: str | None = None) -> bool:
    return _binding(bindings, "_windows_target").windows_repo_path_is_unsafe(repo_path, home_dir)


def update_target_repo_path(bindings: dict, config: dict, target_name: str, repo_path: str) -> None:
    return _binding(bindings, "_windows_target").update_target_repo_path(config, target_name, repo_path)


def windows_repo_checkout_ready(bindings: dict, probe: dict | None) -> bool:
    return _binding(bindings, "_windows_target").windows_repo_checkout_ready(probe)


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


def windows_tooling_detail(
    bindings: dict,
    probe: dict,
    tool_name: str,
    *,
    missing_hint: str | None = None,
) -> str:
    return _binding(bindings, "_windows_target").windows_tooling_detail(
        probe,
        tool_name,
        missing_hint=missing_hint,
    )


def windows_remote_tooling_ready(bindings: dict, probe: dict) -> bool:
    return _binding(bindings, "_windows_target").windows_remote_tooling_ready(
        probe,
        required_tools=_binding(bindings, "WINDOWS_REQUIRED_REMOTE_TOOLS"),
    )


def windows_desktop_session_user(bindings: dict, probe: dict | None) -> str:
    return _binding(bindings, "_windows_target").windows_desktop_session_user(probe)


def windows_desktop_session_state(bindings: dict, probe: dict | None) -> str:
    return _binding(bindings, "_windows_target").windows_desktop_session_state(probe)


def windows_repo_checkout_detail(
    bindings: dict,
    probe: dict | None,
    *,
    fallback_path: str | None = None,
) -> str:
    return _binding(bindings, "_windows_target").windows_repo_checkout_detail(
        probe,
        fallback_path=fallback_path,
    )


def install_windows_target_helpers(bindings: dict, names: tuple[str, ...] = WINDOWS_TARGET_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
