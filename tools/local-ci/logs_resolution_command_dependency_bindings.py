"""Dependency assembly for logs job-resolution command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def logs_resolution_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_queue_fn": _binding(bindings, "load_queue"),
        "current_runner_info_fn": _binding(bindings, "current_runner_info"),
        "select_job_for_logs_fn": _binding(bindings, "_queue_orchestrator").select_job_for_logs,
    }
