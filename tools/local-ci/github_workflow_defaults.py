"""GitHub workflow dispatch default and summary resolution."""

from __future__ import annotations

from collections.abc import Callable

from github_workflow_dispatch import resolve_workflow_dispatch_field_values
from github_workflow_metadata import (
    BUILTIN_GITHUB_WORKFLOWS,
    repo_variable_name_for_workflow_field,
)
from github_workflow_provider import resolve_default_provider_for_workflow
from github_workflow_settings import normalize_runs_on_json


FieldResolver = Callable[
    [dict | None, dict[str, str], str, str, str],
    tuple[str, str],
]


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
    *,
    field_value_resolver: FieldResolver = resolve_workflow_field_value_and_source,
) -> tuple[dict[str, str], dict[str, str]]:
    resolved: dict[str, str] = {}
    sources: dict[str, str] = {}
    for field_name in field_names or []:
        value, source = field_value_resolver(
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
    *,
    provider_resolver: Callable[..., tuple[str, str]] = resolve_default_provider_for_workflow,
    dispatch_defaults_resolver: Callable[..., tuple[dict[str, str], dict[str, str]]] = resolve_workflow_dispatch_defaults,
    field_value_resolver: FieldResolver = resolve_workflow_field_value_and_source,
) -> dict:
    workflow = BUILTIN_GITHUB_WORKFLOWS[workflow_key]
    provider, provider_source = provider_resolver(settings, workflow_key)
    dispatch_fields, dispatch_sources = dispatch_defaults_resolver(
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
        selector_value, selector_source = field_value_resolver(
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
