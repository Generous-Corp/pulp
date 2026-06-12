"""Compatibility installer for Windows target facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from windows_target_constant_bindings import (
    windows_default_remote_repo_dirname,
    windows_optional_remote_tools,
    windows_required_remote_tools,
)
from windows_target_path_bindings import (
    update_target_repo_path,
    windows_default_repo_checkout_path,
    windows_path_join,
    windows_repo_checkout_ready,
    windows_repo_path_is_unsafe,
)
from windows_target_probe_bindings import (
    windows_desktop_session_state,
    windows_desktop_session_user,
    windows_remote_tooling_ready,
    windows_repo_checkout_detail,
    windows_tooling_detail,
)
from windows_target_session_bindings import (
    build_windows_session_agent_request,
    default_windows_session_task_name,
    desktop_target_contract,
)


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


def install_windows_target_helpers(bindings: dict, names: tuple[str, ...] = WINDOWS_TARGET_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
