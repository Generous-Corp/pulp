"""Bindings from the local_ci facade to GitHub workflow constants."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


GITHUB_WORKFLOW_CONSTANT_EXPORTS = (
    "github_actions_defaults",
    "builtin_github_workflows",
    "repo_variable_fallbacks",
)


def github_actions_defaults(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").GITHUB_ACTIONS_DEFAULTS


def builtin_github_workflows(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").BUILTIN_GITHUB_WORKFLOWS


def repo_variable_fallbacks(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").REPO_VARIABLE_FALLBACKS


def install_github_workflow_constant_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_CONSTANT_EXPORTS,
) -> None:
    known_names = set(GITHUB_WORKFLOW_CONSTANT_EXPORTS)
    constant_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), constant_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
