"""Compatibility installer for GitHub workflow resolver facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from github_workflow_constant_bindings import (
    builtin_github_workflows,
    github_actions_defaults,
    repo_variable_fallbacks,
)
from github_workflow_resolution_bindings import (
    github_actions_settings_for_display,
    normalize_runs_on_json,
    repo_variable_name_for_workflow_field,
    resolve_cli_dispatch_field_values,
    resolve_default_provider_for_workflow,
    resolve_github_actions_settings,
    resolve_workflow_dispatch_defaults,
    resolve_workflow_dispatch_field_values,
    resolve_workflow_field_value_and_source,
    resolve_workflow_runner_selector_json,
    summarize_workflow_provider_defaults,
)


GITHUB_WORKFLOW_EXPORTS = (
    "github_actions_settings_for_display",
    "resolve_github_actions_settings",
    "normalize_runs_on_json",
    "resolve_workflow_runner_selector_json",
    "resolve_workflow_dispatch_field_values",
    "repo_variable_name_for_workflow_field",
    "resolve_default_provider_for_workflow",
    "resolve_workflow_field_value_and_source",
    "resolve_workflow_dispatch_defaults",
    "summarize_workflow_provider_defaults",
    "resolve_cli_dispatch_field_values",
)


def install_github_workflow_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
