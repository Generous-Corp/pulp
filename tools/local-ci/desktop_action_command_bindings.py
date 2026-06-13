"""Compatibility installer for desktop action command facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_action_run_command_bindings import (
    cmd_desktop_click,
    cmd_desktop_inspect,
    cmd_desktop_smoke,
)
from desktop_action_selector_bindings import windows_requires_pulp_app_selectors


DESKTOP_ACTION_COMMAND_EXPORTS = (
    "windows_requires_pulp_app_selectors",
    "cmd_desktop_smoke",
    "cmd_desktop_click",
    "cmd_desktop_inspect",
)


def install_desktop_action_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_ACTION_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
