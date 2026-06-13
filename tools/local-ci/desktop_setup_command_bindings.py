"""Compatibility installer for desktop setup command facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_doctor_command_bindings import (
    DESKTOP_DOCTOR_COMMAND_EXPORTS,
    cmd_desktop_doctor,
    install_desktop_doctor_command_helpers,
)
from desktop_install_command_bindings import (
    DESKTOP_INSTALL_COMMAND_EXPORTS,
    cmd_desktop_install,
    install_desktop_install_command_helpers,
)


DESKTOP_SETUP_COMMAND_EXPORTS = (
    *DESKTOP_INSTALL_COMMAND_EXPORTS,
    *DESKTOP_DOCTOR_COMMAND_EXPORTS,
)


def install_desktop_setup_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SETUP_COMMAND_EXPORTS,
) -> None:
    install_names = tuple(name for name in names if name in DESKTOP_INSTALL_COMMAND_EXPORTS)
    doctor_names = tuple(name for name in names if name in DESKTOP_DOCTOR_COMMAND_EXPORTS)
    known_names = set(DESKTOP_SETUP_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_install_command_helpers(bindings, install_names)
    install_desktop_doctor_command_helpers(bindings, doctor_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
