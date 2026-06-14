"""Compatibility facade for desktop automation report commands."""

from __future__ import annotations

from desktop_report_cleanup_commands_cli import cmd_desktop_cleanup
from desktop_report_proof_commands_cli import cmd_desktop_proof
from desktop_report_publish_commands_cli import cmd_desktop_publish
from desktop_report_recent_commands_cli import cmd_desktop_recent


__all__ = [
    "cmd_desktop_cleanup",
    "cmd_desktop_proof",
    "cmd_desktop_publish",
    "cmd_desktop_recent",
]
