"""Dependency assembly for the local-CI drain command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def local_ci_drain_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "drain_pending_jobs_fn": _binding(bindings, "drain_pending_jobs"),
        "current_runner_info_fn": _binding(bindings, "current_runner_info"),
        "drain_runner_active_line_fn": _binding(bindings, "drain_runner_active_line"),
        "notify_fn": _binding(bindings, "notify"),
    }
