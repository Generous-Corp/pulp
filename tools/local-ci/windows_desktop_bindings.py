"""Compatibility installer for Windows desktop action facade bindings."""

from __future__ import annotations

from typing import Any

from windows_desktop_action_bindings import (
    WINDOWS_DESKTOP_ACTION_EXPORTS,
    install_windows_desktop_action_helpers,
    run_windows_session_agent_action,
)


WINDOWS_DESKTOP_EXPORTS = WINDOWS_DESKTOP_ACTION_EXPORTS


def install_windows_desktop_helpers(bindings: dict[str, Any], names: tuple[str, ...] = WINDOWS_DESKTOP_EXPORTS) -> None:
    install_windows_desktop_action_helpers(bindings, names)
