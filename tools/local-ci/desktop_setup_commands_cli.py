"""Compatibility facade for desktop automation setup commands."""

from __future__ import annotations

from desktop_doctor_commands_cli import cmd_desktop_doctor
from desktop_install_commands_cli import cmd_desktop_install


__all__ = ["cmd_desktop_doctor", "cmd_desktop_install"]
