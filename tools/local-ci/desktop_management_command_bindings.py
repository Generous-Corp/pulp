"""Compatibility installer for desktop management command facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_config_command_bindings import (
    cmd_desktop_config_set,
    cmd_desktop_config_show,
)
from desktop_report_command_bindings import (
    cmd_desktop_cleanup,
    cmd_desktop_proof,
    cmd_desktop_publish,
    cmd_desktop_recent,
)
from desktop_status_command_bindings import cmd_desktop_status


DESKTOP_MANAGEMENT_COMMAND_EXPORTS = (
    "cmd_desktop_status",
    "cmd_desktop_config_show",
    "cmd_desktop_config_set",
    "cmd_desktop_recent",
    "cmd_desktop_proof",
    "cmd_desktop_publish",
    "cmd_desktop_cleanup",
)


def install_desktop_management_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
