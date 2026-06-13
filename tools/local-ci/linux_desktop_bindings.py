"""Compatibility facade for Linux desktop action dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from linux_desktop_action_bindings import (
    LINUX_DESKTOP_ACTION_EXPORTS,
    install_linux_desktop_action_helpers,
    run_linux_xvfb_remote_action,
)
from linux_desktop_artifact_bindings import cleanup_remote_ssh_dir, fetch_ssh_artifact


LINUX_DESKTOP_EXPORTS = (
    "fetch_ssh_artifact",
    "cleanup_remote_ssh_dir",
    *LINUX_DESKTOP_ACTION_EXPORTS,
)


def install_linux_desktop_helpers(bindings: dict[str, Any], names: tuple[str, ...] = LINUX_DESKTOP_EXPORTS) -> None:
    action_names = tuple(name for name in names if name in LINUX_DESKTOP_ACTION_EXPORTS)
    artifact_names = tuple(name for name in names if name not in LINUX_DESKTOP_ACTION_EXPORTS)
    install_local_helpers(bindings, globals(), artifact_names)
    install_linux_desktop_action_helpers(bindings, action_names)
