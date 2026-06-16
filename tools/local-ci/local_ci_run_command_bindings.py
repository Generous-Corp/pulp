"""Facade bindings for the local-CI run command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from local_ci_run_command_dependency_bindings import local_ci_run_command_dependencies


LOCAL_CI_RUN_COMMAND_EXPORTS = (
    "cmd_run",
)


def cmd_run(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_run(
        args,
        **local_ci_run_command_dependencies(bindings),
    )
