"""Bindings from the local_ci facade to GitHub workflow runner selectors."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


GITHUB_WORKFLOW_DISPATCH_SELECTOR_EXPORTS = (
    "resolve_workflow_runner_selector_json",
)


def resolve_workflow_runner_selector_json(
    bindings: Mapping[str, Any],
    config: dict | None,
    workflow_key: str,
    provider: str,
) -> str:
    return _binding(bindings, "_github_workflows").resolve_workflow_runner_selector_json(config, workflow_key, provider)


def install_github_workflow_dispatch_selector_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_DISPATCH_SELECTOR_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
