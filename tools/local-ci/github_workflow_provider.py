"""GitHub workflow provider default resolution."""

from __future__ import annotations

from github_workflow_metadata import BUILTIN_GITHUB_WORKFLOWS


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
