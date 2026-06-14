"""Bindings from the local_ci facade to the desktop proof report command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from desktop_report_proof_command_dependency_bindings import desktop_report_proof_command_dependencies


DESKTOP_REPORT_PROOF_COMMAND_EXPORTS = (
    "cmd_desktop_proof",
)


def cmd_desktop_proof(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_proof(
        args,
        **desktop_report_proof_command_dependencies(bindings),
    )


def install_desktop_report_proof_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_REPORT_PROOF_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_REPORT_PROOF_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
