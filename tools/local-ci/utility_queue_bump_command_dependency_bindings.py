"""Dependency assembly for queue bump utility command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def utility_queue_bump_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "normalize_priority_fn": _binding(bindings, "normalize_priority"),
        "bump_queue_command_job_fn": _binding(bindings, "bump_queue_command_job"),
        "bump_queue_command_result_line_fn": _binding(bindings, "bump_queue_command_result_line"),
    }
