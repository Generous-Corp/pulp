"""Compatibility facade for desktop automation management commands."""

from __future__ import annotations

from desktop_config_commands_cli import (
    cmd_desktop_config,
    cmd_desktop_config_set,
    cmd_desktop_config_show,
)
from desktop_report_commands_cli import (
    cmd_desktop_cleanup,
    cmd_desktop_proof,
    cmd_desktop_publish,
    cmd_desktop_recent,
)
from desktop_status_commands_cli import cmd_desktop_status


__all__ = (
    "cmd_desktop_cleanup",
    "cmd_desktop_config",
    "cmd_desktop_config_set",
    "cmd_desktop_config_show",
    "cmd_desktop_proof",
    "cmd_desktop_publish",
    "cmd_desktop_recent",
    "cmd_desktop_status",
)
