"""Compatibility facade for desktop Windows probe dependency bindings."""

from __future__ import annotations

from desktop_windows_repo_probe_bindings import (
    ensure_windows_remote_repo_checkout,
    probe_windows_repo_checkout,
)
from desktop_windows_tooling_probe_bindings import (
    ensure_windows_remote_tooling,
    install_windows_remote_tool,
    probe_windows_remote_tooling,
    probe_windows_session_agent,
)


DESKTOP_WINDOWS_PROBE_EXPORTS = (
    "probe_windows_repo_checkout",
    "ensure_windows_remote_repo_checkout",
    "probe_windows_session_agent",
    "probe_windows_remote_tooling",
    "install_windows_remote_tool",
    "ensure_windows_remote_tooling",
)
