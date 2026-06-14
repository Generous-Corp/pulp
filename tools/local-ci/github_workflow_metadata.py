"""Static GitHub Actions workflow metadata for local CI."""

from __future__ import annotations


GITHUB_ACTIONS_DEFAULTS = {
    "repository": "",
    "workflow": "build",
    "provider": "github-hosted",
    "wait_poll_secs": 10,
    "match_timeout_secs": 60,
}

BUILTIN_GITHUB_WORKFLOWS = {
    "build": {
        "file": "build.yml",
        "display_name": "Build and Test",
        "providers": ["github-hosted", "namespace"],
        "provider_input": "runner_provider",
        "dispatch_fields": [
            "linux_runner_selector_json",
            "windows_runner_selector_json",
            "macos_runner_selector_json",
        ],
    },
    "validate": {
        "file": "validate.yml",
        "display_name": "Plugin Validation",
        "providers": ["github-hosted"],
    },
    "sanitizers": {
        "file": "sanitizers.yml",
        "display_name": "Sanitizer Tests",
        "providers": ["github-hosted"],
    },
    "docs-check": {
        "file": "docs-check.yml",
        "display_name": "Docs Consistency",
        "providers": ["github-hosted", "namespace"],
        "provider_input": "runner_provider",
        "selector_input": "runner_selector_json",
    },
}

REPO_VARIABLE_FALLBACKS = {
    ("build", "namespace", "linux_runner_selector_json"): "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
    ("build", "namespace", "windows_runner_selector_json"): "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON",
    ("build", "namespace", "macos_runner_selector_json"): "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON",
    ("docs-check", "namespace", "runner_selector_json"): "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON",
}


def repo_variable_name_for_workflow_field(
    workflow_key: str,
    provider: str,
    field_name: str,
) -> str:
    return REPO_VARIABLE_FALLBACKS.get((workflow_key, provider, field_name), "")
