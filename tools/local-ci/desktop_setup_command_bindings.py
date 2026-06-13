"""Compatibility installer for desktop setup command facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_doctor_command_bindings import cmd_desktop_doctor
from desktop_install_command_bindings import cmd_desktop_install


DESKTOP_SETUP_COMMAND_EXPORTS = (
    "cmd_desktop_install",
    "cmd_desktop_doctor",
)


def install_desktop_setup_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SETUP_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
