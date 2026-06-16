"""Compatibility installer for Windows target session helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from windows_target_session_identity_bindings import (
    WINDOWS_TARGET_SESSION_IDENTITY_EXPORTS,
    default_windows_session_task_name,
    desktop_target_contract,
    install_windows_target_session_identity_helpers,
)
from windows_target_session_request_bindings import (
    WINDOWS_TARGET_SESSION_REQUEST_EXPORTS,
    build_windows_session_agent_request,
    install_windows_target_session_request_helpers,
)


WINDOWS_TARGET_SESSION_EXPORTS = (
    *WINDOWS_TARGET_SESSION_IDENTITY_EXPORTS,
    *WINDOWS_TARGET_SESSION_REQUEST_EXPORTS,
)


def install_windows_target_session_helpers(
    bindings: dict,
    names: tuple[str, ...] = WINDOWS_TARGET_SESSION_EXPORTS,
) -> None:
    identity_names = tuple(name for name in names if name in WINDOWS_TARGET_SESSION_IDENTITY_EXPORTS)
    request_names = tuple(name for name in names if name in WINDOWS_TARGET_SESSION_REQUEST_EXPORTS)
    known_names = set(WINDOWS_TARGET_SESSION_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_target_session_identity_helpers(bindings, identity_names)
    install_windows_target_session_request_helpers(bindings, request_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
