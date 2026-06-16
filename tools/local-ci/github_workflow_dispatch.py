"""GitHub workflow dispatch selector and CLI override resolution."""

from __future__ import annotations

import argparse

from github_workflow_config import workflow_provider_config
from github_workflow_settings import normalize_runs_on_json


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
