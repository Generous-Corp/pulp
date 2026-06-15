"""Compatibility installer for desktop action command facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_action_run_command_bindings import (
    DESKTOP_ACTION_RUN_COMMAND_EXPORTS,
    cmd_desktop_click,
    cmd_desktop_inspect,
    cmd_desktop_smoke,
    install_desktop_action_run_command_helpers,
)
from desktop_action_selector_bindings import (
    DESKTOP_ACTION_SELECTOR_EXPORTS,
    install_desktop_action_selector_helpers,
    windows_requires_pulp_app_selectors,
)


DESKTOP_ACTION_COMMAND_EXPORTS = (
    *DESKTOP_ACTION_SELECTOR_EXPORTS,
    *DESKTOP_ACTION_RUN_COMMAND_EXPORTS,
)


def install_desktop_action_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_ACTION_COMMAND_EXPORTS,
) -> None:
    selector_names = tuple(name for name in names if name in DESKTOP_ACTION_SELECTOR_EXPORTS)
    run_command_names = tuple(name for name in names if name in DESKTOP_ACTION_RUN_COMMAND_EXPORTS)
    known_names = set(DESKTOP_ACTION_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_action_selector_helpers(bindings, selector_names)
    install_desktop_action_run_command_helpers(bindings, run_command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
