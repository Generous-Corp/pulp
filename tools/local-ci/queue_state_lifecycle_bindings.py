"""Compatibility facade for queue state lifecycle dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_active_load_bindings import QUEUE_ACTIVE_LOAD_EXPORTS, load_job, update_job_active_targets
from queue_stale_state_bindings import (
    QUEUE_STALE_STATE_EXPORTS,
    reclaim_stale_remote_validators,
    reconcile_running_jobs_unlocked,
    update_job_target_state,
)


QUEUE_STATE_LIFECYCLE_EXPORTS = (
    *QUEUE_ACTIVE_LOAD_EXPORTS[:1],
    *QUEUE_STALE_STATE_EXPORTS,
    *QUEUE_ACTIVE_LOAD_EXPORTS[1:],
)


def install_queue_state_lifecycle_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_STATE_LIFECYCLE_EXPORTS,
) -> None:
    known_names = set(QUEUE_STATE_LIFECYCLE_EXPORTS)
    lifecycle_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), lifecycle_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
