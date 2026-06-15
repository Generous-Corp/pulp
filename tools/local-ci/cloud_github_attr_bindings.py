"""Installer for cloud GitHub helpers that remain direct module attributes."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs


CLOUD_GITHUB_MODULE_EXPORTS = (
    "gh_api_json",
    "gh_auth_status_text",
    "gh_current_login",
    "gh_find_dispatched_run",
    "gh_repo_name",
    "gh_repo_variables",
    "gh_token_scopes",
    "resolve_github_repository",
)


def install_cloud_github_attr_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CLOUD_GITHUB_MODULE_EXPORTS,
) -> None:
    install_module_attrs(bindings, "_cloud", names)
