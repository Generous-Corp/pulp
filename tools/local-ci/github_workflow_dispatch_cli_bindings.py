"""Bindings from the local_ci facade to GitHub workflow CLI dispatch fields."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


GITHUB_WORKFLOW_DISPATCH_CLI_EXPORTS = (
    "resolve_cli_dispatch_field_values",
)


def resolve_cli_dispatch_field_values(
    bindings: Mapping[str, Any],
    args: Any,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return _binding(bindings, "_github_workflows").resolve_cli_dispatch_field_values(
        args,
        field_names,
    )


def install_github_workflow_dispatch_cli_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_DISPATCH_CLI_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
