"""Cloud defaults command orchestration."""
from __future__ import annotations

from collections.abc import Callable
import argparse


def cmd_cloud_defaults(
    _args: argparse.Namespace,
    *,
    load_optional_config_fn: Callable[[], dict],
    github_actions_settings_for_display_fn: Callable[[dict], dict],
    resolve_github_actions_settings_fn: Callable[[dict], dict],
    resolve_github_repository_fn: Callable[[dict], str],
    gh_available_fn: Callable[[], bool],
    gh_repo_variables_fn: Callable[[str], dict[str, str]],
    cloud_defaults_lines_fn: Callable[..., list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config = load_optional_config_fn()
    settings = github_actions_settings_for_display_fn(config)
    repository = ""
    repository_note = ""
    repository_variables: dict[str, str] = {}
    try:
        resolved_settings = resolve_github_actions_settings_fn(config)
        settings = resolved_settings
        repository = resolve_github_repository_fn(resolved_settings)
    except ValueError as exc:
        repository_note = str(exc)
        try:
            repository = resolve_github_repository_fn(settings)
        except ValueError:
            repository = ""
    else:
        if gh_available_fn():
            repository_variables = gh_repo_variables_fn(repository)
        else:
            repository_note = "gh CLI unavailable; repo-variable fallbacks not inspected"

    for line in cloud_defaults_lines_fn(
        config,
        settings,
        repository=repository,
        repository_note=repository_note,
        repository_variables=repository_variables,
    ):
        print_fn(line)
    return 0


__all__ = ["cmd_cloud_defaults"]
