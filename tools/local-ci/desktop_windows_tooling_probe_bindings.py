"""Compatibility facade for desktop Windows session/tooling probe bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_windows_remote_tooling_probe_bindings import (
    DESKTOP_WINDOWS_REMOTE_TOOLING_PROBE_EXPORTS,
    ensure_windows_remote_tooling,
    install_windows_remote_tool,
    install_desktop_windows_remote_tooling_probe_helpers,
    probe_windows_remote_tooling,
)
from desktop_windows_session_agent_probe_bindings import (
    DESKTOP_WINDOWS_SESSION_AGENT_PROBE_EXPORTS,
    install_desktop_windows_session_agent_probe_helpers,
    probe_windows_session_agent,
)


DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS = (
    *DESKTOP_WINDOWS_SESSION_AGENT_PROBE_EXPORTS,
    *DESKTOP_WINDOWS_REMOTE_TOOLING_PROBE_EXPORTS,
)


def install_desktop_windows_tooling_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS,
) -> None:
    session_agent_names = tuple(name for name in names if name in DESKTOP_WINDOWS_SESSION_AGENT_PROBE_EXPORTS)
    remote_tooling_names = tuple(name for name in names if name in DESKTOP_WINDOWS_REMOTE_TOOLING_PROBE_EXPORTS)
    known_names = set(DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_windows_session_agent_probe_helpers(bindings, session_agent_names)
    install_desktop_windows_remote_tooling_probe_helpers(bindings, remote_tooling_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
