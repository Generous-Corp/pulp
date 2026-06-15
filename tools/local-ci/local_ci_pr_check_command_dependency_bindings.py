"""Dependency assembly for local-CI PR check command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def local_ci_pr_check_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "gh_available_fn": _binding(bindings, "gh_available"),
        "gh_pr_head_fn": _binding(bindings, "gh_pr_head"),
        "short_sha_fn": _binding(bindings, "short_sha"),
        "load_config_fn": _binding(bindings, "load_config"),
        "resolve_targets_fn": _binding(bindings, "resolve_targets"),
        "parse_targets_arg_fn": _binding(bindings, "parse_targets_arg"),
        "normalize_priority_fn": _binding(bindings, "normalize_priority"),
        "default_priority_for_fn": _binding(bindings, "default_priority_for"),
        "normalize_validation_mode_fn": _binding(bindings, "normalize_validation_mode"),
        "build_submission_metadata_fn": _binding(bindings, "build_submission_metadata"),
        "print_submission_metadata_fn": _binding(bindings, "print_submission_metadata"),
        "enqueue_job_fn": _binding(bindings, "enqueue_job"),
        "summarize_job_fn": _binding(bindings, "summarize_job"),
        "wait_for_job_fn": _binding(bindings, "wait_for_job"),
        "gh_pr_comment_fn": _binding(bindings, "gh_pr_comment"),
        "format_ci_comment_fn": _binding(bindings, "format_ci_comment"),
        "notify_fn": _binding(bindings, "notify"),
    }
