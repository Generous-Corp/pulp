"""Compatibility installer for Windows desktop action facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from windows_desktop_action_bindings import (
    WINDOWS_DESKTOP_ACTION_EXPORTS,
    install_windows_desktop_action_helpers,
    run_windows_session_agent_action,
)


WINDOWS_DESKTOP_EXPORTS = WINDOWS_DESKTOP_ACTION_EXPORTS


def install_windows_desktop_helpers(bindings: dict[str, Any], names: tuple[str, ...] = WINDOWS_DESKTOP_EXPORTS) -> None:
    action_names = tuple(name for name in names if name in WINDOWS_DESKTOP_ACTION_EXPORTS)
    known_names = set(WINDOWS_DESKTOP_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_desktop_action_helpers(bindings, action_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
