"""Facade bindings for the local-CI drain command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from local_ci_drain_command_dependency_bindings import local_ci_drain_command_dependencies


LOCAL_CI_DRAIN_COMMAND_EXPORTS = (
    "cmd_drain",
)


def cmd_drain(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_drain(
        args,
        **local_ci_drain_command_dependencies(bindings),
    )
