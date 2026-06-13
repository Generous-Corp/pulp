"""Compatibility installer for Windows target facade helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from windows_target_constant_bindings import (
    WINDOWS_TARGET_CONSTANT_EXPORTS,
    install_windows_target_constant_helpers,
    windows_default_remote_repo_dirname,
    windows_optional_remote_tools,
    windows_required_remote_tools,
)
from windows_target_path_bindings import (
    WINDOWS_TARGET_PATH_EXPORTS,
    install_windows_target_path_helpers,
    update_target_repo_path,
    windows_default_repo_checkout_path,
    windows_path_join,
    windows_repo_checkout_ready,
    windows_repo_path_is_unsafe,
)
from windows_target_probe_bindings import (
    WINDOWS_TARGET_PROBE_EXPORTS,
    install_windows_target_probe_helpers,
    windows_desktop_session_state,
    windows_desktop_session_user,
    windows_remote_tooling_ready,
    windows_repo_checkout_detail,
    windows_tooling_detail,
)
from windows_target_session_bindings import (
    WINDOWS_TARGET_SESSION_EXPORTS,
    build_windows_session_agent_request,
    default_windows_session_task_name,
    desktop_target_contract,
    install_windows_target_session_helpers,
)


WINDOWS_TARGET_EXPORTS = (
    *WINDOWS_TARGET_SESSION_EXPORTS,
    *WINDOWS_TARGET_PATH_EXPORTS,
    *WINDOWS_TARGET_PROBE_EXPORTS,
)


def install_windows_target_helpers(bindings: dict, names: tuple[str, ...] = WINDOWS_TARGET_EXPORTS) -> None:
    known_names = set(WINDOWS_TARGET_EXPORTS) | set(WINDOWS_TARGET_CONSTANT_EXPORTS)
    constant_names = tuple(name for name in names if name in WINDOWS_TARGET_CONSTANT_EXPORTS)
    session_names = tuple(name for name in names if name in WINDOWS_TARGET_SESSION_EXPORTS)
    path_names = tuple(name for name in names if name in WINDOWS_TARGET_PATH_EXPORTS)
    probe_names = tuple(name for name in names if name in WINDOWS_TARGET_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_target_constant_helpers(bindings, constant_names)
    install_windows_target_session_helpers(bindings, session_names)
    install_windows_target_path_helpers(bindings, path_names)
    install_windows_target_probe_helpers(bindings, probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
