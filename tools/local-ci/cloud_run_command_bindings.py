"""Bindings from the local_ci facade to cloud run/status commands."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLOUD_RUN_COMMAND_EXPORTS = (
    "cmd_cloud_run",
    "cmd_cloud_status",
)


def cmd_cloud_run(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_run(args)


def cmd_cloud_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_status(args)


def install_cloud_run_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_RUN_COMMAND_EXPORTS,
) -> None:
    known_names = set(CLOUD_RUN_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
