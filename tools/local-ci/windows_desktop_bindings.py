"""Compatibility installer for Windows desktop action facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from windows_desktop_action_bindings import run_windows_session_agent_action


WINDOWS_DESKTOP_EXPORTS = (
    "run_windows_session_agent_action",
)


def install_windows_desktop_helpers(bindings: dict[str, Any], names: tuple[str, ...] = WINDOWS_DESKTOP_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
