"""Compatibility facade for GitHub workflow resolution dependency bindings."""

from __future__ import annotations

from github_workflow_dispatch_bindings import (
    repo_variable_name_for_workflow_field,
    resolve_cli_dispatch_field_values,
    resolve_workflow_dispatch_defaults,
    resolve_workflow_dispatch_field_values,
    resolve_workflow_field_value_and_source,
    resolve_workflow_runner_selector_json,
)
from github_workflow_provider_bindings import (
    resolve_default_provider_for_workflow,
    summarize_workflow_provider_defaults,
)
from github_workflow_settings_bindings import (
    github_actions_settings_for_display,
    normalize_runs_on_json,
    resolve_github_actions_settings,
)
