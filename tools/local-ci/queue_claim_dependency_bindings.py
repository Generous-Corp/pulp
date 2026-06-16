"""Dependency assembly for queue claim facade bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_claim_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return {
        "root": _binding(bindings, "ROOT"),
        "queue_lock_path_fn": _binding(bindings, "queue_lock_path"),
        "file_lock_fn": _binding(bindings, "file_lock"),
        "load_queue_unlocked_fn": _binding(bindings, "load_queue_unlocked"),
        "reconcile_running_jobs_unlocked_fn": _binding(bindings, "reconcile_running_jobs_unlocked"),
        "save_queue_unlocked_fn": _binding(bindings, "save_queue_unlocked"),
        "claim_next_job_unlocked_fn": lambda queue, *, runner: queue_orchestrator.claim_next_job_unlocked(
            queue,
            runner=runner,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        "normalize_job_fn": _binding(bindings, "normalize_job"),
        "pid_fn": _binding(bindings, "os").getpid,
    }
