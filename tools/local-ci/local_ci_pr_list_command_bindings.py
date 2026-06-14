"""Bindings from the local_ci facade to the PR list command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from local_ci_pr_list_command_dependency_bindings import local_ci_pr_list_command_dependencies


LOCAL_CI_PR_LIST_COMMAND_EXPORTS = (
    "cmd_list",
)


def cmd_list(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_list(
        args,
        **local_ci_pr_list_command_dependencies(bindings),
    )


def install_local_ci_pr_list_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOCAL_CI_PR_LIST_COMMAND_EXPORTS,
) -> None:
    known_names = set(LOCAL_CI_PR_LIST_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
