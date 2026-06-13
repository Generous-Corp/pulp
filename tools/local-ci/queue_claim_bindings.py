"""Bindings from the local_ci facade to queue claim helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_CLAIM_EXPORTS = ("claim_next_job",)


def claim_next_job(bindings: Mapping[str, Any]) -> dict | None:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return _binding(bindings, "_queue_lifecycle").claim_next_job_locked(
        root=_binding(bindings, "ROOT"),
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        reconcile_running_jobs_unlocked_fn=_binding(bindings, "reconcile_running_jobs_unlocked"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        claim_next_job_unlocked_fn=lambda queue, *, runner: queue_orchestrator.claim_next_job_unlocked(
            queue,
            runner=runner,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        normalize_job_fn=_binding(bindings, "normalize_job"),
        pid_fn=_binding(bindings, "os").getpid,
    )


def install_queue_claim_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_CLAIM_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
