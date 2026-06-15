"""Compatibility installer for GitHub workflow resolver facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from github_workflow_constant_bindings import (
    GITHUB_WORKFLOW_CONSTANT_EXPORTS,
    builtin_github_workflows,
    github_actions_defaults,
    install_github_workflow_constant_helpers,
    repo_variable_fallbacks,
)
from github_workflow_resolution_bindings import (
    GITHUB_WORKFLOW_DISPATCH_EXPORTS,
    GITHUB_WORKFLOW_PROVIDER_EXPORTS,
    GITHUB_WORKFLOW_RESOLUTION_EXPORTS,
    GITHUB_WORKFLOW_SETTINGS_EXPORTS,
    repo_variable_name_for_workflow_field,
    resolve_cli_dispatch_field_values,
    resolve_default_provider_for_workflow,
    resolve_github_actions_settings,
    resolve_workflow_dispatch_defaults,
    resolve_workflow_dispatch_field_values,
    resolve_workflow_field_value_and_source,
    resolve_workflow_runner_selector_json,
    summarize_workflow_provider_defaults,
    github_actions_settings_for_display,
    install_github_workflow_resolution_helpers,
    normalize_runs_on_json,
)


GITHUB_WORKFLOW_EXPORTS = GITHUB_WORKFLOW_RESOLUTION_EXPORTS


def install_github_workflow_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_EXPORTS,
) -> None:
    known_names = set(GITHUB_WORKFLOW_EXPORTS) | set(GITHUB_WORKFLOW_CONSTANT_EXPORTS)
    constant_names = tuple(name for name in names if name in GITHUB_WORKFLOW_CONSTANT_EXPORTS)
    resolution_names = tuple(name for name in names if name in GITHUB_WORKFLOW_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_github_workflow_constant_helpers(bindings, constant_names)
    install_github_workflow_resolution_helpers(bindings, resolution_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
