"""Bindings from the local_ci facade to locked queue enqueue helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_ENQUEUE_EXPORTS = ("enqueue_job",)


def enqueue_job(
    bindings: Mapping[str, Any],
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> tuple[dict, bool]:
    queue_orchestrator = _binding(bindings, "_queue_orchestrator")

    return _binding(bindings, "_queue_lifecycle").enqueue_job_locked(
        branch,
        sha,
        priority,
        targets,
        mode,
        validation,
        submission=submission,
        queue_lock_path_fn=_binding(bindings, "queue_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        load_queue_unlocked_fn=_binding(bindings, "load_queue_unlocked"),
        reconcile_running_jobs_unlocked_fn=_binding(bindings, "reconcile_running_jobs_unlocked"),
        save_queue_unlocked_fn=_binding(bindings, "save_queue_unlocked"),
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        normalize_validation_mode_fn=_binding(bindings, "normalize_validation_mode"),
        make_fingerprint_fn=_binding(bindings, "make_fingerprint"),
        find_active_job_by_fingerprint_unlocked_fn=queue_orchestrator.find_active_job_by_fingerprint_unlocked,
        bump_pending_job_priority_unlocked_fn=lambda existing, requested_priority: queue_orchestrator.bump_pending_job_priority_unlocked(
            existing,
            requested_priority,
            now_iso_fn=_binding(bindings, "now_iso"),
        ),
        make_job_fn=_binding(bindings, "make_job"),
        pending_supersedence_candidates_unlocked_fn=queue_orchestrator.pending_supersedence_candidates_unlocked,
        supersede_job_unlocked_fn=_binding(bindings, "supersede_job_unlocked"),
        trim_completed_jobs_fn=_binding(bindings, "trim_completed_jobs"),
        normalize_job_fn=_binding(bindings, "normalize_job"),
    )


def install_queue_enqueue_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_ENQUEUE_EXPORTS,
) -> None:
    known_names = set(QUEUE_ENQUEUE_EXPORTS)
    enqueue_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), enqueue_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
