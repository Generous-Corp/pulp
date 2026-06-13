"""Compatibility facade for desktop command dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_action_command_bindings import (
    DESKTOP_ACTION_COMMAND_EXPORTS,
    cmd_desktop_click,
    cmd_desktop_inspect,
    cmd_desktop_smoke,
    install_desktop_action_command_helpers,
    windows_requires_pulp_app_selectors,
)
from desktop_management_command_bindings import (
    DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
    cmd_desktop_cleanup,
    cmd_desktop_config_set,
    cmd_desktop_config_show,
    cmd_desktop_proof,
    cmd_desktop_publish,
    cmd_desktop_recent,
    cmd_desktop_status,
    install_desktop_management_command_helpers,
)
from desktop_setup_command_bindings import (
    DESKTOP_SETUP_COMMAND_EXPORTS,
    cmd_desktop_doctor,
    cmd_desktop_install,
    install_desktop_setup_command_helpers,
)


DESKTOP_COMMAND_EXPORTS = (
    *DESKTOP_SETUP_COMMAND_EXPORTS,
    *DESKTOP_MANAGEMENT_COMMAND_EXPORTS,
    *DESKTOP_ACTION_COMMAND_EXPORTS,
)


def install_desktop_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_COMMAND_EXPORTS,
) -> None:
    setup_names = tuple(name for name in names if name in DESKTOP_SETUP_COMMAND_EXPORTS)
    management_names = tuple(name for name in names if name in DESKTOP_MANAGEMENT_COMMAND_EXPORTS)
    action_names = tuple(name for name in names if name in DESKTOP_ACTION_COMMAND_EXPORTS)
    known_names = set(DESKTOP_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_setup_command_helpers(bindings, setup_names)
    install_desktop_management_command_helpers(bindings, management_names)
    install_desktop_action_command_helpers(bindings, action_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
