"""Compatibility installer for desktop management command facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_config_command_bindings import (
    DESKTOP_CONFIG_COMMAND_EXPORTS,
    cmd_desktop_config_set,
    cmd_desktop_config_show,
    install_desktop_config_command_helpers,
)
from desktop_report_command_bindings import (
    DESKTOP_REPORT_COMMAND_EXPORTS,
    cmd_desktop_cleanup,
    cmd_desktop_proof,
    cmd_desktop_publish,
    cmd_desktop_recent,
    install_desktop_report_command_helpers,
)
from desktop_status_command_bindings import (
    DESKTOP_STATUS_COMMAND_EXPORTS,
    cmd_desktop_status,
    install_desktop_status_command_helpers,
)


DESKTOP_MANAGEMENT_COMMAND_EXPORTS = (
    *DESKTOP_STATUS_COMMAND_EXPORTS,
    *DESKTOP_CONFIG_COMMAND_EXPORTS,
    *DESKTOP_REPORT_COMMAND_EXPORTS,
)


def install_desktop_management_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
) -> None:
    status_names = tuple(name for name in names if name in DESKTOP_STATUS_COMMAND_EXPORTS)
    config_names = tuple(name for name in names if name in DESKTOP_CONFIG_COMMAND_EXPORTS)
    report_names = tuple(name for name in names if name in DESKTOP_REPORT_COMMAND_EXPORTS)
    known_names = set(DESKTOP_MANAGEMENT_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_status_command_helpers(bindings, status_names)
    install_desktop_config_command_helpers(bindings, config_names)
    install_desktop_report_command_helpers(bindings, report_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
