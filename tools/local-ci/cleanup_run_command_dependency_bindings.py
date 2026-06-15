"""Dependency assembly for cleanup command execution bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def cleanup_run_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_queue_fn": _binding(bindings, "load_queue"),
        "collect_cleanup_plan_fn": _binding(bindings, "collect_local_ci_cleanup_plan"),
        "apply_cleanup_plan_fn": _binding(bindings, "apply_local_ci_cleanup_plan"),
        "print_cleanup_plan_fn": _binding(bindings, "print_local_ci_cleanup_plan"),
        "print_state_footprint_fn": _binding(bindings, "print_local_ci_state_footprint"),
        "format_size_fn": _binding(bindings, "format_size_bytes"),
        "describe_path_fn": _binding(bindings, "describe_path_for_cleanup"),
    }
