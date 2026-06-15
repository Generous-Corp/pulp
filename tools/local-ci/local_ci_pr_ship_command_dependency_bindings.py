"""Dependency assembly for local-CI PR ship command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def local_ci_pr_ship_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "resolve_submission_options_fn": _binding(bindings, "resolve_submission_options"),
        "gh_available_fn": _binding(bindings, "gh_available"),
        "print_submission_metadata_fn": _binding(bindings, "print_submission_metadata"),
        "root": _binding(bindings, "ROOT"),
        "run_fn": _binding_attr(bindings, "subprocess", "run"),
        "gh_pr_create_fn": _binding(bindings, "gh_pr_create"),
        "enqueue_job_fn": _binding(bindings, "enqueue_job"),
        "summarize_job_fn": _binding(bindings, "summarize_job"),
        "wait_for_job_fn": _binding(bindings, "wait_for_job"),
        "gh_pr_comment_fn": _binding(bindings, "gh_pr_comment"),
        "format_ci_comment_fn": _binding(bindings, "format_ci_comment"),
        "gh_pr_merge_fn": _binding(bindings, "gh_pr_merge"),
        "notify_fn": _binding(bindings, "notify"),
    }
