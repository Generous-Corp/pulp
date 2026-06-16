"""Dependency assembly for queue runner active-target bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_runner_active_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    def update_info(info: dict, current_job_id: str, current_active_targets: dict | None) -> bool:
        return _binding(bindings, "_queue_orchestrator").update_runner_info_active_targets(
            info,
            current_job_id,
            current_active_targets,
            now_iso_fn=_binding(bindings, "now_iso"),
        )

    return {
        "update_runner_info_active_targets_fn": update_info,
    }
