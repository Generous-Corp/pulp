"""Bindings from the local_ci facade to cloud command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLOUD_COMMAND_EXPORTS = (
    "cmd_cloud_workflows",
    "cmd_cloud_defaults",
    "cmd_cloud_history",
    "cmd_cloud_compare",
    "cmd_cloud_recommend",
    "cmd_cloud_run",
    "cmd_cloud_status",
    "cmd_cloud_namespace_doctor",
    "cmd_cloud_namespace_setup",
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


def cmd_cloud_run(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_run(args)


def cmd_cloud_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_status(args)


def cmd_cloud_namespace_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_doctor(args)


def cmd_cloud_namespace_setup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_setup(args)


def install_cloud_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
