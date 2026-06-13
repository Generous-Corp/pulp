"""Compatibility installer for GitHub workflow resolver facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from github_workflow_constant_bindings import (
    builtin_github_workflows,
    github_actions_defaults,
    repo_variable_fallbacks,
)
from github_workflow_dispatch_bindings import (
    GITHUB_WORKFLOW_DISPATCH_EXPORTS,
    install_github_workflow_dispatch_helpers,
    repo_variable_name_for_workflow_field,
    resolve_cli_dispatch_field_values,
    resolve_workflow_dispatch_defaults,
    resolve_workflow_dispatch_field_values,
    resolve_workflow_field_value_and_source,
    resolve_workflow_runner_selector_json,
)
from github_workflow_provider_bindings import (
    GITHUB_WORKFLOW_PROVIDER_EXPORTS,
    install_github_workflow_provider_helpers,
    resolve_default_provider_for_workflow,
    summarize_workflow_provider_defaults,
)
from github_workflow_settings_bindings import (
    GITHUB_WORKFLOW_SETTINGS_EXPORTS,
    github_actions_settings_for_display,
    install_github_workflow_settings_helpers,
    normalize_runs_on_json,
    resolve_github_actions_settings,
)


GITHUB_WORKFLOW_EXPORTS = (
    *GITHUB_WORKFLOW_SETTINGS_EXPORTS,
    *GITHUB_WORKFLOW_DISPATCH_EXPORTS,
    *GITHUB_WORKFLOW_PROVIDER_EXPORTS,
)


def install_github_workflow_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_EXPORTS,
) -> None:
    settings_names = tuple(name for name in names if name in GITHUB_WORKFLOW_SETTINGS_EXPORTS)
    dispatch_names = tuple(name for name in names if name in GITHUB_WORKFLOW_DISPATCH_EXPORTS)
    provider_names = tuple(name for name in names if name in GITHUB_WORKFLOW_PROVIDER_EXPORTS)
    known_names = set(GITHUB_WORKFLOW_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_github_workflow_settings_helpers(bindings, settings_names)
    install_github_workflow_dispatch_helpers(bindings, dispatch_names)
    install_github_workflow_provider_helpers(bindings, provider_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
