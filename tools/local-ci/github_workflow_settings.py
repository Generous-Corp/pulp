"""GitHub Actions settings and runs-on JSON normalization helpers."""

from __future__ import annotations

import json

from github_workflow_metadata import GITHUB_ACTIONS_DEFAULTS


def github_actions_settings_for_display(config: dict | None) -> dict:
    settings = dict(GITHUB_ACTIONS_DEFAULTS)
    github_actions = (config or {}).get("github_actions", {})
    defaults = github_actions.get("defaults", {})

    repository = github_actions.get("repository")
    if isinstance(repository, str) and repository.strip():
        settings["repository"] = repository.strip()

    workflow = defaults.get("workflow")
    if isinstance(workflow, str) and workflow.strip():
        settings["workflow"] = workflow.strip()

    provider = defaults.get("provider")
    if isinstance(provider, str) and provider.strip():
        settings["provider"] = provider.strip()

    return settings


def resolve_github_actions_settings(config: dict | None) -> dict:
    settings = github_actions_settings_for_display(config)
    defaults = ((config or {}).get("github_actions") or {}).get("defaults", {})

    for key in ("wait_poll_secs", "match_timeout_secs"):
        value = defaults.get(key)
        if value in (None, ""):
            continue
        try:
            parsed = int(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"github_actions.defaults.{key} must be an integer.") from exc
        if parsed <= 0:
            raise ValueError(f"github_actions.defaults.{key} must be positive.")
        settings[key] = parsed

    return settings


def normalize_runs_on_json(raw: str, *, setting_name: str) -> str:
    value = (raw or "").strip()
    if not value:
        return ""
    try:
        decoded = json.loads(value)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{setting_name} must be valid JSON.") from exc
    if not isinstance(decoded, (str, list)):
        raise ValueError(f"{setting_name} must decode to a string or array accepted by runs-on.")
    return json.dumps(decoded)
