"""Constant installation helpers for the local_ci.py facade bootstrap."""

from __future__ import annotations

from typing import Any


def install_bootstrap_constants(
    bindings: dict[str, Any],
    *,
    execution_timing_bindings: Any,
    windows_target_bindings: Any,
    linux_target_bindings: Any,
    normalize_bindings: Any,
    github_workflow_bindings: Any,
) -> None:
    bindings["HEARTBEAT_INTERVAL_SECS"] = execution_timing_bindings.heartbeat_interval_secs(bindings)
    bindings["STUCK_IDLE_SECS"] = execution_timing_bindings.stuck_idle_secs(bindings)
    bindings["WINDOWS_REQUIRED_REMOTE_TOOLS"] = (
        windows_target_bindings.windows_required_remote_tools(bindings)
    )
    bindings["WINDOWS_OPTIONAL_REMOTE_TOOLS"] = (
        windows_target_bindings.windows_optional_remote_tools(bindings)
    )
    bindings["WINDOWS_DEFAULT_REMOTE_REPO_DIRNAME"] = (
        windows_target_bindings.windows_default_remote_repo_dirname(bindings)
    )
    bindings["LINUX_REQUIRED_REMOTE_TOOLS"] = (
        linux_target_bindings.linux_required_remote_tools(bindings)
    )
    bindings["LINUX_OPTIONAL_REMOTE_TOOLS"] = (
        linux_target_bindings.linux_optional_remote_tools(bindings)
    )
    bindings["PRIORITY_VALUES"] = normalize_bindings.priority_values(bindings)
    bindings["GITHUB_ACTIONS_DEFAULTS"] = (
        github_workflow_bindings.github_actions_defaults(bindings)
    )
    bindings["BUILTIN_GITHUB_WORKFLOWS"] = (
        github_workflow_bindings.builtin_github_workflows(bindings)
    )
    bindings["REPO_VARIABLE_FALLBACKS"] = (
        github_workflow_bindings.repo_variable_fallbacks(bindings)
    )
