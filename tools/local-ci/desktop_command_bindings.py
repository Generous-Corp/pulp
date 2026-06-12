"""Compatibility facade for desktop command dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_action_command_bindings import (
    DESKTOP_ACTION_COMMAND_EXPORTS,
    cmd_desktop_click,
    cmd_desktop_inspect,
    cmd_desktop_smoke,
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
)
from desktop_setup_command_bindings import (
    DESKTOP_SETUP_COMMAND_EXPORTS,
    cmd_desktop_doctor,
    cmd_desktop_install,
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
    install_local_helpers(bindings, globals(), names)
