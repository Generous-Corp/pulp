"""Compatibility installer for GitHub workflow dispatch-field bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from github_workflow_dispatch_cli_bindings import (
    GITHUB_WORKFLOW_DISPATCH_CLI_EXPORTS,
    install_github_workflow_dispatch_cli_helpers,
    resolve_cli_dispatch_field_values,
)
from github_workflow_dispatch_default_bindings import (
    GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS,
    install_github_workflow_dispatch_default_helpers,
    resolve_workflow_dispatch_defaults,
)
from github_workflow_dispatch_field_bindings import (
    GITHUB_WORKFLOW_DISPATCH_FIELD_EXPORTS,
    install_github_workflow_dispatch_field_helpers,
    repo_variable_name_for_workflow_field,
    resolve_workflow_dispatch_field_values,
    resolve_workflow_field_value_and_source,
)
from github_workflow_dispatch_selector_bindings import (
    GITHUB_WORKFLOW_DISPATCH_SELECTOR_EXPORTS,
    install_github_workflow_dispatch_selector_helpers,
    resolve_workflow_runner_selector_json,
)


GITHUB_WORKFLOW_DISPATCH_EXPORTS = (
    *GITHUB_WORKFLOW_DISPATCH_SELECTOR_EXPORTS,
    *GITHUB_WORKFLOW_DISPATCH_FIELD_EXPORTS,
    *GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS,
    *GITHUB_WORKFLOW_DISPATCH_CLI_EXPORTS,
)


def install_github_workflow_dispatch_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = GITHUB_WORKFLOW_DISPATCH_EXPORTS,
) -> None:
    selector_names = tuple(name for name in names if name in GITHUB_WORKFLOW_DISPATCH_SELECTOR_EXPORTS)
    field_names = tuple(name for name in names if name in GITHUB_WORKFLOW_DISPATCH_FIELD_EXPORTS)
    default_names = tuple(name for name in names if name in GITHUB_WORKFLOW_DISPATCH_DEFAULT_EXPORTS)
    cli_names = tuple(name for name in names if name in GITHUB_WORKFLOW_DISPATCH_CLI_EXPORTS)
    known_names = set(GITHUB_WORKFLOW_DISPATCH_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_github_workflow_dispatch_selector_helpers(bindings, selector_names)
    install_github_workflow_dispatch_field_helpers(bindings, field_names)
    install_github_workflow_dispatch_default_helpers(bindings, default_names)
    install_github_workflow_dispatch_cli_helpers(bindings, cli_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
