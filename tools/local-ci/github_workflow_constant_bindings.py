"""Bindings from the local_ci facade to GitHub workflow constants."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def github_actions_defaults(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").GITHUB_ACTIONS_DEFAULTS


def builtin_github_workflows(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").BUILTIN_GITHUB_WORKFLOWS


def repo_variable_fallbacks(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").REPO_VARIABLE_FALLBACKS
