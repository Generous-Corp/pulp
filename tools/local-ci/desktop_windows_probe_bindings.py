"""Compatibility facade for desktop Windows probe dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_windows_repo_probe_bindings import (
    DESKTOP_WINDOWS_REPO_PROBE_EXPORTS,
    ensure_windows_remote_repo_checkout,
    install_desktop_windows_repo_probe_helpers,
    probe_windows_repo_checkout,
)
from desktop_windows_tooling_probe_bindings import (
    DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS,
    ensure_windows_remote_tooling,
    install_desktop_windows_tooling_probe_helpers,
    install_windows_remote_tool,
    probe_windows_remote_tooling,
    probe_windows_session_agent,
)


DESKTOP_WINDOWS_PROBE_EXPORTS = (
    *DESKTOP_WINDOWS_REPO_PROBE_EXPORTS,
    *DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS,
)


def install_desktop_windows_probe_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_WINDOWS_PROBE_EXPORTS,
) -> None:
    repo_names = tuple(name for name in names if name in DESKTOP_WINDOWS_REPO_PROBE_EXPORTS)
    tooling_names = tuple(name for name in names if name in DESKTOP_WINDOWS_TOOLING_PROBE_EXPORTS)
    known_names = set(DESKTOP_WINDOWS_PROBE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_windows_repo_probe_helpers(bindings, repo_names)
    install_desktop_windows_tooling_probe_helpers(bindings, tooling_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
