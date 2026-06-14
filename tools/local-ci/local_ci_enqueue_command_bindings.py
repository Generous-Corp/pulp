"""Facade bindings for the local-CI enqueue command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from local_ci_enqueue_command_dependency_bindings import local_ci_enqueue_command_dependencies


LOCAL_CI_ENQUEUE_COMMAND_EXPORTS = (
    "cmd_enqueue",
)


def cmd_enqueue(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_enqueue(
        args,
        **local_ci_enqueue_command_dependencies(bindings),
    )
