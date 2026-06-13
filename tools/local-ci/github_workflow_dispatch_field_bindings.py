"""Bindings from the local_ci facade to GitHub workflow dispatch fields."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


GITHUB_WORKFLOW_DISPATCH_FIELD_EXPORTS = (
    "resolve_workflow_dispatch_field_values",
    "repo_variable_name_for_workflow_field",
    "resolve_workflow_field_value_and_source",
)


def resolve_workflow_dispatch_field_values(
    bindings: Mapping[str, Any],
    config: dict | None,
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return _binding(bindings, "_github_workflows").resolve_workflow_dispatch_field_values(
        config,
        workflow_key,
        provider,
        field_names,
    )


def repo_variable_name_for_workflow_field(
    bindings: Mapping[str, Any],
    workflow_key: str,
    provider: str,
    field_name: str,
) -> str:
    return _binding(bindings, "_github_workflows").repo_variable_name_for_workflow_field(
        workflow_key,
        provider,
        field_name,
    )


def resolve_workflow_field_value_and_source(
    bindings: Mapping[str, Any],
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_name: str,
) -> tuple[str, str]:
    return _binding(bindings, "_github_workflows").resolve_workflow_field_value_and_source(
        config,
        repository_variables,
        workflow_key,
        provider,
        field_name,
    )


def install_github_workflow_dispatch_field_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_DISPATCH_FIELD_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
