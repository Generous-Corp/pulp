"""GitHub Actions workflow dispatch helpers for local CI.

Extracted from local_ci.py as part of the R2-1 phased split. The
constants (GITHUB_ACTIONS_DEFAULTS, BUILTIN_GITHUB_WORKFLOWS,
REPO_VARIABLE_FALLBACKS) plus the 11 resolver functions own:

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
    config_values = resolve_workflow_dispatch_field_values(config, workflow_key, provider, [field_name])
    value = config_values.get(field_name, "")
    if value:
        return (
            value,
            f"config github_actions.workflows.{workflow_key}.providers.{provider}.{field_name}",
        )

    variable_name = repo_variable_name_for_workflow_field(workflow_key, provider, field_name)
    if variable_name:
        variable_value = repository_variables.get(variable_name, "")
        if variable_value:
            return (
                normalize_runs_on_json(variable_value, setting_name=variable_name),
                f"repo variable {variable_name}",
            )

    return "", ""


def resolve_workflow_dispatch_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> tuple[dict[str, str], dict[str, str]]:
    resolved: dict[str, str] = {}
    sources: dict[str, str] = {}
    for field_name in field_names or []:
        value, source = resolve_workflow_field_value_and_source(
            config,
            repository_variables,
            workflow_key,
            provider,
            field_name,
        )
        if not value:
            continue
        resolved[field_name] = value
        if source:
            sources[field_name] = source
    return resolved, sources


def summarize_workflow_provider_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    settings: dict,
    workflow_key: str,
) -> dict:
    workflow = BUILTIN_GITHUB_WORKFLOWS[workflow_key]
    provider, provider_source = resolve_default_provider_for_workflow(settings, workflow_key)
    dispatch_fields, dispatch_sources = resolve_workflow_dispatch_defaults(
        config,
        repository_variables,
        workflow_key,
        provider,
        workflow.get("dispatch_fields"),
    )
    selector_value = ""
    selector_source = ""
    selector_input = workflow.get("selector_input")
    if selector_input:
        selector_value, selector_source = resolve_workflow_field_value_and_source(
            config,
            repository_variables,
            workflow_key,
            provider,
            selector_input,
        )
    return {
        "provider": provider,
        "provider_source": provider_source,
        "selector_input": selector_input or "",
        "selector_value": selector_value,
        "selector_source": selector_source,
        "dispatch_fields": dispatch_fields,
        "dispatch_sources": dispatch_sources,
    }


def resolve_cli_dispatch_field_values(
    args,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return github_workflow_dispatch.resolve_cli_dispatch_field_values(args, field_names)
