"""Bindings from the local_ci facade to the PR ship command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


LOCAL_CI_PR_SHIP_COMMAND_EXPORTS = (
    "cmd_ship",
)


def cmd_ship(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_ship(
        args,
        resolve_submission_options_fn=_binding(bindings, "resolve_submission_options"),
        gh_available_fn=_binding(bindings, "gh_available"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        root=_binding(bindings, "ROOT"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        gh_pr_create_fn=_binding(bindings, "gh_pr_create"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        summarize_job_fn=_binding(bindings, "summarize_job"),
        wait_for_job_fn=_binding(bindings, "wait_for_job"),
        gh_pr_comment_fn=_binding(bindings, "gh_pr_comment"),
        format_ci_comment_fn=_binding(bindings, "format_ci_comment"),
        gh_pr_merge_fn=_binding(bindings, "gh_pr_merge"),
        notify_fn=_binding(bindings, "notify"),
    )


def install_local_ci_pr_ship_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOCAL_CI_PR_SHIP_COMMAND_EXPORTS,
) -> None:
    known_names = set(LOCAL_CI_PR_SHIP_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
