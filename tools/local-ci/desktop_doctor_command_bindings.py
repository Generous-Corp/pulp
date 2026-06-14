"""Bindings from the local_ci facade to desktop doctor command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


DESKTOP_DOCTOR_COMMAND_EXPORTS = ("cmd_desktop_doctor",)


def cmd_desktop_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_setup_commands_cli").cmd_desktop_doctor(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        desktop_doctor_checks_fn=_binding(bindings, "desktop_doctor_checks"),
    )


def install_desktop_doctor_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_DOCTOR_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_DOCTOR_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
