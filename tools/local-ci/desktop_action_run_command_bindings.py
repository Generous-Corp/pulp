"""Compatibility installer for desktop action runner command bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_action_click_command_bindings import (
    DESKTOP_ACTION_CLICK_COMMAND_EXPORTS,
    cmd_desktop_click,
    install_desktop_action_click_command_helpers,
)
from desktop_action_inspect_command_bindings import (
    DESKTOP_ACTION_INSPECT_COMMAND_EXPORTS,
    cmd_desktop_inspect,
    install_desktop_action_inspect_command_helpers,
)
from desktop_action_smoke_command_bindings import (
    DESKTOP_ACTION_SMOKE_COMMAND_EXPORTS,
    cmd_desktop_smoke,
    install_desktop_action_smoke_command_helpers,
)


DESKTOP_ACTION_RUN_COMMAND_EXPORTS = (
    *DESKTOP_ACTION_SMOKE_COMMAND_EXPORTS,
    *DESKTOP_ACTION_CLICK_COMMAND_EXPORTS,
    *DESKTOP_ACTION_INSPECT_COMMAND_EXPORTS,
)


def install_desktop_action_run_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_ACTION_RUN_COMMAND_EXPORTS,
) -> None:
    smoke_names = tuple(name for name in names if name in DESKTOP_ACTION_SMOKE_COMMAND_EXPORTS)
    click_names = tuple(name for name in names if name in DESKTOP_ACTION_CLICK_COMMAND_EXPORTS)
    inspect_names = tuple(name for name in names if name in DESKTOP_ACTION_INSPECT_COMMAND_EXPORTS)
    known_names = set(DESKTOP_ACTION_RUN_COMMAND_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_action_smoke_command_helpers(bindings, smoke_names)
    install_desktop_action_click_command_helpers(bindings, click_names)
    install_desktop_action_inspect_command_helpers(bindings, inspect_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
