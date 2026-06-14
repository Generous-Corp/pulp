"""Bindings from the local_ci facade to the desktop click command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from desktop_action_run_deps_bindings import desktop_action_command_kwargs


DESKTOP_ACTION_CLICK_COMMAND_EXPORTS = ("cmd_desktop_click",)


def cmd_desktop_click(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_click(
        args,
        **desktop_action_command_kwargs(bindings),
    )


def install_desktop_action_click_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_ACTION_CLICK_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
