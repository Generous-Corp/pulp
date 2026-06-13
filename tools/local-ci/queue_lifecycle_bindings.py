"""Bindings from the local_ci facade to locked queue lifecycle helpers."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_command_lifecycle_bindings import (
    bump_queue_command_job,
    cancel_job_unlocked,
    cancel_queue_command_job,
    supersede_job_unlocked,
)
from queue_drain_bindings import (
    claim_next_job,
    drain_pending_jobs,
    finalize_job,
    wait_for_job,
)
from queue_enqueue_bindings import (
    QUEUE_ENQUEUE_EXPORTS,
    enqueue_job,
    install_queue_enqueue_helpers,
)
from queue_load_bindings import (
    QUEUE_LOAD_EXPORTS,
    install_queue_load_helpers,
    load_queue,
)
from queue_state_lifecycle_bindings import (
    load_job,
    reclaim_stale_remote_validators,
    reconcile_running_jobs_unlocked,
    update_job_active_targets,
    update_job_target_state,
)


QUEUE_LIFECYCLE_EXPORTS = (
    "supersede_job_unlocked",
    "cancel_job_unlocked",
    *QUEUE_LOAD_EXPORTS,
    "update_job_active_targets",
    *QUEUE_ENQUEUE_EXPORTS,
    "bump_queue_command_job",
    "cancel_queue_command_job",
    "reconcile_running_jobs_unlocked",
    "update_job_target_state",
    "reclaim_stale_remote_validators",
    "load_job",
    "claim_next_job",
    "finalize_job",
    "wait_for_job",
    "drain_pending_jobs",
)


def install_queue_lifecycle_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_LIFECYCLE_EXPORTS,
) -> None:
    load_names = tuple(name for name in names if name in QUEUE_LOAD_EXPORTS)
    enqueue_names = tuple(name for name in names if name in QUEUE_ENQUEUE_EXPORTS)
    known_focused_names = set(QUEUE_LOAD_EXPORTS + QUEUE_ENQUEUE_EXPORTS)
    remaining_names = tuple(name for name in names if name not in known_focused_names)

    install_queue_load_helpers(bindings, load_names)
    install_queue_enqueue_helpers(bindings, enqueue_names)
    install_local_helpers(bindings, globals(), remaining_names)
