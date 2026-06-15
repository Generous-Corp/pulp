"""Dependency assembly for queue cancel utility command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def utility_queue_cancel_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "cancel_queue_command_job_fn": _binding(bindings, "cancel_queue_command_job"),
        "cancel_queue_command_result_line_fn": _binding(bindings, "cancel_queue_command_result_line"),
    }
