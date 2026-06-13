"""Compatibility facade for queue state lifecycle dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_active_load_bindings import load_job, update_job_active_targets
from queue_stale_state_bindings import (
    reclaim_stale_remote_validators,
    reconcile_running_jobs_unlocked,
    update_job_target_state,
)


QUEUE_STATE_LIFECYCLE_EXPORTS = (
    "update_job_active_targets",
    "reconcile_running_jobs_unlocked",
    "update_job_target_state",
    "reclaim_stale_remote_validators",
    "load_job",
)


def install_queue_state_lifecycle_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_STATE_LIFECYCLE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
