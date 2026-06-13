"""Bindings from the local_ci facade to cloud Namespace commands."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CLOUD_NAMESPACE_COMMAND_EXPORTS = (
    "cmd_cloud_namespace_doctor",
    "cmd_cloud_namespace_setup",
)


def cmd_cloud_namespace_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_doctor(args)


def cmd_cloud_namespace_setup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cloud").cmd_cloud_namespace_setup(args)


def install_cloud_namespace_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_NAMESPACE_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
