"""Shape-safe config lookups for GitHub workflow provider defaults."""

from __future__ import annotations


def workflow_provider_config(
    config: dict | None,
    workflow_key: str,
    provider: str,
) -> dict:
    github_actions = (config or {}).get("github_actions", {})
    workflows = github_actions.get("workflows", {})
    if not isinstance(workflows, dict):
        return {}
    workflow = workflows.get(workflow_key, {})
    if not isinstance(workflow, dict):
        return {}
    providers = workflow.get("providers", {})
    if not isinstance(providers, dict):
        return {}
    provider_info = providers.get(provider, {})
    if not isinstance(provider_info, dict):
        return {}
    return provider_info
