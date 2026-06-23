"""GitHub Actions workflow dispatch helpers for local CI.

The constants (GITHUB_ACTIONS_DEFAULTS, BUILTIN_GITHUB_WORKFLOWS,
REPO_VARIABLE_FALLBACKS) plus the resolver functions own:

  - Reading the `github_actions` block out of the active config
  - Computing the effective workflow + provider + selector that a CLI
    dispatch should target
  - Resolving the per-dispatch-field selector JSON (from the CLI
    arguments, the config, the repo-variable fallback table, or the
    BUILTIN_GITHUB_WORKFLOWS defaults — in that precedence order)
  - Formatting the workflow-provider summary surfaced by
    `cloud defaults` and `cloud workflows`

All functions are pure: they accept a config dict + optional CLI
arguments, return derived dicts/strings. No I/O, no subprocess, no
GitHub API calls — those live in the cloud-dispatch orchestrator in
local_ci.py.
"""

from __future__ import annotations

import github_workflow_dispatch
import github_workflow_defaults
import github_workflow_provider
from github_workflow_metadata import (
    BUILTIN_GITHUB_WORKFLOWS,
    GITHUB_ACTIONS_DEFAULTS,
    REPO_VARIABLE_FALLBACKS,
    repo_variable_name_for_workflow_field,
)
from github_workflow_settings import (
    github_actions_settings_for_display,
    normalize_runs_on_json,
    resolve_github_actions_settings,
)


def resolve_workflow_runner_selector_json(
    config: dict | None, workflow_key: str, provider: str
) -> str:
    return github_workflow_dispatch.resolve_workflow_runner_selector_json(
        config,
        workflow_key,
        provider,
    )


def resolve_workflow_dispatch_field_values(
    config: dict | None,
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return github_workflow_dispatch.resolve_workflow_dispatch_field_values(
        config,
        workflow_key,
        provider,
        field_names,
    )


def resolve_default_provider_for_workflow(
    settings: dict,
    workflow_key: str,
    *,
    explicit_provider: str | None = None,
) -> tuple[str, str]:
    return github_workflow_provider.resolve_default_provider_for_workflow(
        settings,
        workflow_key,
        explicit_provider=explicit_provider,
    )


def resolve_workflow_field_value_and_source(
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_name: str,
) -> tuple[str, str]:
    return github_workflow_defaults.resolve_workflow_field_value_and_source(
        config,
        repository_variables,
        workflow_key,
        provider,
        field_name,
    )


def resolve_workflow_dispatch_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> tuple[dict[str, str], dict[str, str]]:
    return github_workflow_defaults.resolve_workflow_dispatch_defaults(
        config,
        repository_variables,
        workflow_key,
        provider,
        field_names,
        field_value_resolver=resolve_workflow_field_value_and_source,
    )


def summarize_workflow_provider_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    settings: dict,
    workflow_key: str,
) -> dict:
    return github_workflow_defaults.summarize_workflow_provider_defaults(
        config,
        repository_variables,
        settings,
        workflow_key,
        provider_resolver=resolve_default_provider_for_workflow,
        dispatch_defaults_resolver=resolve_workflow_dispatch_defaults,
        field_value_resolver=resolve_workflow_field_value_and_source,
    )


def resolve_cli_dispatch_field_values(
    args,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return github_workflow_dispatch.resolve_cli_dispatch_field_values(args, field_names)
