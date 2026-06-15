"""Dependency assembly for the local-CI enqueue command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def local_ci_enqueue_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "resolve_submission_options_fn": _binding(bindings, "resolve_submission_options"),
        "print_submission_metadata_fn": _binding(bindings, "print_submission_metadata"),
        "enqueue_job_fn": _binding(bindings, "enqueue_job"),
        "enqueue_command_result_line_fn": _binding(bindings, "enqueue_command_result_line"),
    }
