"""Compatibility facade for Windows remote tooling helpers."""
from __future__ import annotations

from windows_tooling_ensure import ensure_windows_remote_tooling
from windows_tooling_install import install_windows_remote_tool
from windows_tooling_probe import probe_windows_remote_tooling


__all__ = [
    "ensure_windows_remote_tooling",
    "install_windows_remote_tool",
    "probe_windows_remote_tooling",
]
