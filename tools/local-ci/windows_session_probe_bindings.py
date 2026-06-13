"""Compatibility facade for Windows session-agent and CMake probe bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from windows_session_agent_bindings import (
    WINDOWS_SESSION_AGENT_EXPORTS,
    bootstrap_windows_session_agent,
    install_windows_session_agent_helpers,
    start_windows_session_agent_task,
)
from windows_session_cmake_probe_bindings import (
    WINDOWS_SESSION_CMAKE_PROBE_EXPORTS,
    install_windows_session_cmake_probe_helpers,
    probe_windows_ssh_cmake_settings,
)


WINDOWS_SESSION_PROBE_EXPORTS = (
    *WINDOWS_SESSION_AGENT_EXPORTS,
    *WINDOWS_SESSION_CMAKE_PROBE_EXPORTS,
)


def install_windows_session_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_SESSION_PROBE_EXPORTS,
) -> None:
    agent_names = tuple(name for name in names if name in WINDOWS_SESSION_AGENT_EXPORTS)
    cmake_probe_names = tuple(name for name in names if name in WINDOWS_SESSION_CMAKE_PROBE_EXPORTS)
    known_names = set(WINDOWS_SESSION_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_windows_session_agent_helpers(bindings, agent_names)
    install_windows_session_cmake_probe_helpers(bindings, cmake_probe_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
