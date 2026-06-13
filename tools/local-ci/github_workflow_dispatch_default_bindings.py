"""Bindings from the local_ci facade to GitHub workflow dispatch defaults."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS = (
    "resolve_workflow_dispatch_defaults",
)


def resolve_workflow_dispatch_defaults(
    bindings: Mapping[str, Any],
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> tuple[dict[str, str], dict[str, str]]:
    return _binding(bindings, "_github_workflows").resolve_workflow_dispatch_defaults(
        config,
        repository_variables,
        workflow_key,
        provider,
        field_names,
    )


def install_github_workflow_dispatch_default_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS,
) -> None:
    known_names = set(GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS)
    default_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), default_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
