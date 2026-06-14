"""Bindings from the local_ci facade to runner active-target helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_RUNNER_ACTIVE_EXPORTS = ("update_runner_active_targets",)


def update_runner_active_targets(bindings: Mapping[str, Any], job_id: str, active_targets: dict | None) -> None:
    def update_info(info: dict, current_job_id: str, current_active_targets: dict | None) -> bool:
        return _binding(bindings, "_queue_orchestrator").update_runner_info_active_targets(
            info,
            current_job_id,
            current_active_targets,
            now_iso_fn=_binding(bindings, "now_iso"),
        )

    _binding(bindings, "_runner_state").update_current_runner_active_targets(
        job_id,
        active_targets,
        update_runner_info_active_targets_fn=update_info,
    )
