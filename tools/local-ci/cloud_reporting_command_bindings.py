"""Bindings from the local_ci facade to cloud reporting commands."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLOUD_REPORTING_COMMAND_EXPORTS = (
    "cmd_cloud_workflows",
    "cmd_cloud_defaults",
    "cmd_cloud_history",
    "cmd_cloud_compare",
    "cmd_cloud_recommend",
)


def cmd_cloud_workflows(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_workflows(args)


def cmd_cloud_defaults(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_defaults(args)


def cmd_cloud_history(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_history(args)


def cmd_cloud_compare(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_compare(args)


def cmd_cloud_recommend(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_recommend(args)


def install_cloud_reporting_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_REPORTING_COMMAND_EXPORTS,
) -> None:
    known_names = set(CLOUD_REPORTING_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
