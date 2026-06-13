"""Bindings from the local_ci facade to the PR check command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


LOCAL_CI_PR_CHECK_COMMAND_EXPORTS = (
    "cmd_check",
)


def cmd_check(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_check(
        args,
        gh_available_fn=_binding(bindings, "gh_available"),
        gh_pr_head_fn=_binding(bindings, "gh_pr_head"),
        short_sha_fn=_binding(bindings, "short_sha"),
        load_config_fn=_binding(bindings, "load_config"),
        resolve_targets_fn=_binding(bindings, "resolve_targets"),
        parse_targets_arg_fn=_binding(bindings, "parse_targets_arg"),
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        default_priority_for_fn=_binding(bindings, "default_priority_for"),
        normalize_validation_mode_fn=_binding(bindings, "normalize_validation_mode"),
        build_submission_metadata_fn=_binding(bindings, "build_submission_metadata"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        summarize_job_fn=_binding(bindings, "summarize_job"),
        wait_for_job_fn=_binding(bindings, "wait_for_job"),
        gh_pr_comment_fn=_binding(bindings, "gh_pr_comment"),
        format_ci_comment_fn=_binding(bindings, "format_ci_comment"),
        notify_fn=_binding(bindings, "notify"),
    )


def install_local_ci_pr_check_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOCAL_CI_PR_CHECK_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
