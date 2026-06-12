"""Bindings from the local_ci facade to GitHub workflow resolver helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def github_actions_defaults(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").GITHUB_ACTIONS_DEFAULTS


def builtin_github_workflows(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").BUILTIN_GITHUB_WORKFLOWS


def repo_variable_fallbacks(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_github_workflows").REPO_VARIABLE_FALLBACKS


def github_actions_settings_for_display(bindings: Mapping[str, Any], config: dict | None) -> dict:
    return _binding(bindings, "_github_workflows").github_actions_settings_for_display(config)


def resolve_github_actions_settings(bindings: Mapping[str, Any], config: dict | None) -> dict:
    return _binding(bindings, "_github_workflows").resolve_github_actions_settings(config)


def normalize_runs_on_json(bindings: Mapping[str, Any], raw: str, *, setting_name: str) -> str:
    return _binding(bindings, "_github_workflows").normalize_runs_on_json(raw, setting_name=setting_name)


def resolve_workflow_runner_selector_json(
    bindings: Mapping[str, Any],
    config: dict | None,
    workflow_key: str,
    provider: str,
) -> str:
    return _binding(bindings, "_github_workflows").resolve_workflow_runner_selector_json(config, workflow_key, provider)


def resolve_workflow_dispatch_field_values(
    bindings: Mapping[str, Any],
    config: dict | None,
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return _binding(bindings, "_github_workflows").resolve_workflow_dispatch_field_values(
        config,
        workflow_key,
        provider,
        field_names,
    )


def repo_variable_name_for_workflow_field(
    bindings: Mapping[str, Any],
    workflow_key: str,
    provider: str,
    field_name: str,
) -> str:
    return _binding(bindings, "_github_workflows").repo_variable_name_for_workflow_field(
        workflow_key,
        provider,
        field_name,
    )


def resolve_default_provider_for_workflow(
    bindings: Mapping[str, Any],
    settings: dict,
    workflow_key: str,
    *,
    explicit_provider: str | None = None,
) -> tuple[str, str]:
    return _binding(bindings, "_github_workflows").resolve_default_provider_for_workflow(
        settings,
        workflow_key,
        explicit_provider=explicit_provider,
    )


def resolve_workflow_field_value_and_source(
    bindings: Mapping[str, Any],
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_name: str,
) -> tuple[str, str]:
    return _binding(bindings, "_github_workflows").resolve_workflow_field_value_and_source(
        config,
        repository_variables,
        workflow_key,
        provider,
        field_name,
    )


def resolve_workflow_dispatch_defaults(
    bindings: Mapping[str, Any],
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> tuple[dict[str, str], dict[str, str]]:
    return _binding(bindings, "_github_workflows").resolve_workflow_dispatch_defaults(
        config,
        repository_variables,
        workflow_key,
        provider,
        field_names,
    )


def summarize_workflow_provider_defaults(
    bindings: Mapping[str, Any],
    config: dict | None,
    repository_variables: dict[str, str],
    settings: dict,
    workflow_key: str,
) -> dict:
    return _binding(bindings, "_github_workflows").summarize_workflow_provider_defaults(
        config,
        repository_variables,
        settings,
        workflow_key,
    )


def resolve_cli_dispatch_field_values(
    bindings: Mapping[str, Any],
    args: Any,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    return _binding(bindings, "_github_workflows").resolve_cli_dispatch_field_values(
        args,
        field_names,
    )
