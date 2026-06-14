"""Bindings from the local_ci facade to the local-CI status command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from local_ci_status_command_dependency_bindings import local_ci_status_command_dependencies


LOCAL_CI_STATUS_COMMAND_EXPORTS = (
    "cmd_status",
)


def cmd_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_status(
        args,
        **local_ci_status_command_dependencies(bindings),
    )
