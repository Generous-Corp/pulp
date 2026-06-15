"""Compatibility facade for Windows desktop target helpers."""

from __future__ import annotations

from windows_session_request import (
    build_windows_session_agent_request,
    default_windows_session_task_name,
    desktop_target_contract,
)
from windows_target_paths import (
    WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME,
    update_target_repo_path,
    windows_default_repo_checkout_path,
    windows_path_join,
    windows_repo_checkout_ready,
    windows_repo_path_is_unsafe,
)
from windows_target_probe_format import (
    WINDOWS_OPTIONAL_REMOTE_TOOLS,
    WINDOWS_REQUIRED_REMOTE_TOOLS,
    windows_desktop_session_state,
    windows_desktop_session_user,
    windows_remote_tooling_ready,
    windows_repo_checkout_detail,
    windows_tooling_detail,
)


__all__ = (
    "WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME",
    "WINDOWS_OPTIONAL_REMOTE_TOOLS",
    "WINDOWS_REQUIRED_REMOTE_TOOLS",
    "build_windows_session_agent_request",
    "default_windows_session_task_name",
    "desktop_target_contract",
    "update_target_repo_path",
    "windows_default_repo_checkout_path",
    "windows_desktop_session_state",
    "windows_desktop_session_user",
    "windows_path_join",
    "windows_remote_tooling_ready",
    "windows_repo_checkout_detail",
    "windows_repo_checkout_ready",
    "windows_repo_path_is_unsafe",
    "windows_tooling_detail",
)
