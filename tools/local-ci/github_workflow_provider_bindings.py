"""Bindings from the local_ci facade to GitHub workflow provider helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def resolve_default_provider_for_workflow(
    bindings: Mapping[str, Any],
    settings: dict,
    workflow_key: str,
    *,
    explicit_provider: str | None = None,
) -> tuple[str, str]:
    return _binding(bindings, "_github_workflows").resolve_default_provider_for_workflow(
        settings,
        workflow_key,
        explicit_provider=explicit_provider,
    )


def summarize_workflow_provider_defaults(
    bindings: Mapping[str, Any],
    config: dict | None,
    repository_variables: dict[str, str],
    settings: dict,
    workflow_key: str,
) -> dict:
    return _binding(bindings, "_github_workflows").summarize_workflow_provider_defaults(
        config,
        repository_variables,
        settings,
        workflow_key,
    )
