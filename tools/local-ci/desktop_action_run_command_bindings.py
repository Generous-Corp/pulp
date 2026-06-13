"""Bindings from the local_ci facade to desktop action runner commands."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


DESKTOP_ACTION_RUN_COMMAND_EXPORTS = (
    "cmd_desktop_smoke",
    "cmd_desktop_click",
    "cmd_desktop_inspect",
)


def _desktop_action_kwargs(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "resolve_desktop_target_fn": _binding(bindings, "resolve_desktop_target"),
        "make_desktop_source_request_fn": _binding(bindings, "make_desktop_source_request"),
        "run_macos_local_smoke_fn": _binding(bindings, "run_macos_local_smoke"),
        "run_linux_xvfb_remote_action_fn": _binding(bindings, "run_linux_xvfb_remote_action"),
        "run_windows_session_agent_action_fn": _binding(bindings, "run_windows_session_agent_action"),
        "desktop_action_success_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_action_success_lines"),
        "sys_platform": _binding_attr(bindings, "sys", "platform"),
    }


def cmd_desktop_smoke(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_smoke(
        args,
        **_desktop_action_kwargs(bindings),
    )


def cmd_desktop_click(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_click(
        args,
        **_desktop_action_kwargs(bindings),
    )


def cmd_desktop_inspect(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_inspect(
        args,
        **_desktop_action_kwargs(bindings),
    )


def install_desktop_action_run_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_ACTION_RUN_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_ACTION_RUN_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
