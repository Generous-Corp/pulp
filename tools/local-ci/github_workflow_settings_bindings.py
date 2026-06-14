"""Bindings from the local_ci facade to GitHub workflow settings helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


GITHUB_WORKFLOW_SETTINGS_EXPORTS = (
    "github_actions_settings_for_display",
    "resolve_github_actions_settings",
    "normalize_runs_on_json",
)


def github_actions_settings_for_display(bindings: Mapping[str, Any], config: dict | None) -> dict:
    return _binding(bindings, "_github_workflows").github_actions_settings_for_display(config)


def resolve_github_actions_settings(bindings: Mapping[str, Any], config: dict | None) -> dict:
    return _binding(bindings, "_github_workflows").resolve_github_actions_settings(config)


def normalize_runs_on_json(bindings: Mapping[str, Any], raw: str, *, setting_name: str) -> str:
    return _binding(bindings, "_github_workflows").normalize_runs_on_json(raw, setting_name=setting_name)


def install_github_workflow_settings_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_SETTINGS_EXPORTS,
) -> None:
    known_names = set(GITHUB_WORKFLOW_SETTINGS_EXPORTS)
    settings_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), settings_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
