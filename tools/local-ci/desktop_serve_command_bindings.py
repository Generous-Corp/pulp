"""Bindings from the local_ci facade to the desktop report serve command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_SERVE_COMMAND_EXPORTS = (
    "cmd_desktop_serve",
)


def cmd_desktop_serve(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_serve_commands_cli").cmd_desktop_serve(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_publish_reports_fn=_binding(bindings, "desktop_publish_reports"),
    )


def install_desktop_serve_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SERVE_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_SERVE_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
