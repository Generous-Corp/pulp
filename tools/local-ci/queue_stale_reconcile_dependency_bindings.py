"""Dependency assembly for stale running queue reconciliation bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_stale_reconcile_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return {
        "stale_running_jobs_unlocked_fn": _binding(bindings, "stale_running_jobs_unlocked"),
        "stale_running_reconciliation_actions_unlocked_fn": queue_orchestrator.stale_running_reconciliation_actions_unlocked,
        "supersede_job_unlocked_fn": _binding(bindings, "supersede_job_unlocked"),
        "requeue_stale_running_job_unlocked_fn": lambda job: queue_orchestrator.requeue_stale_running_job_unlocked(
            job,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
    }
