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

import argparse

from github_workflow_config import workflow_provider_config
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
    provider_info = workflow_provider_config(config, workflow_key, provider)
    selector = provider_info.get("runner_selector_json")
    if not isinstance(selector, str) or not selector.strip():
        return ""
    return normalize_runs_on_json(
        selector,
        setting_name=f"github_actions.workflows.{workflow_key}.providers.{provider}.runner_selector_json",
    )


def resolve_workflow_dispatch_field_values(
    config: dict | None,
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    if not field_names:
        return {}

    provider_info = workflow_provider_config(config, workflow_key, provider)
    resolved: dict[str, str] = {}
    for field_name in field_names:
        value = provider_info.get(field_name)
        if not isinstance(value, str) or not value.strip():
            continue
        resolved[field_name] = normalize_runs_on_json(
            value,
            setting_name=(
                f"github_actions.workflows.{workflow_key}.providers.{provider}.{field_name}"
            ),
        )
    return resolved


def resolve_default_provider_for_workflow(
    settings: dict,
    workflow_key: str,
    *,
    explicit_provider: str | None = None,
) -> tuple[str, str]:
    workflow = BUILTIN_GITHUB_WORKFLOWS.get(workflow_key)
    if workflow is None:
        raise ValueError(f"Unknown workflow '{workflow_key}'.")

    supported = workflow.get("providers", ["github-hosted"])
    if explicit_provider:
        provider = explicit_provider.strip()
        if provider not in supported:
            raise ValueError(
                f"workflow '{workflow_key}' does not support provider '{provider}'. "
                f"Supported: {', '.join(supported)}"
            )
        return provider, "cli"

    preferred = (settings.get("provider") or "github-hosted").strip() or "github-hosted"
    if preferred in supported:
        source = "github_actions.defaults.provider" if settings.get("provider") else "builtin default"
        return preferred, source

    return "github-hosted", f"workflow fallback (default provider '{preferred}' unsupported)"


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
    args: argparse.Namespace,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    supported = set(field_names or [])
    override_names = (
        "linux_runner_selector_json",
        "windows_runner_selector_json",
        "macos_runner_selector_json",
    )
    resolved: dict[str, str] = {}
    for field_name in override_names:
        value = getattr(args, field_name, None)
        if not value:
            continue
        if field_name not in supported:
            raise ValueError(
                f"--{field_name.replace('_', '-')} is not supported for this workflow."
            )
        resolved[field_name] = normalize_runs_on_json(
            value,
            setting_name=f"--{field_name.replace('_', '-')}",
        )
    return resolved
