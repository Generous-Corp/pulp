"""Compatibility facade for stale queue state dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_stale_reclaim_bindings import (
    QUEUE_STALE_RECLAIM_EXPORTS,
    install_queue_stale_reclaim_helpers,
    reclaim_stale_remote_validators,
)
from queue_stale_reconcile_bindings import (
    QUEUE_STALE_RECONCILE_EXPORTS,
    install_queue_stale_reconcile_helpers,
    reconcile_running_jobs_unlocked,
)
from queue_target_update_bindings import (
    QUEUE_TARGET_UPDATE_EXPORTS,
    install_queue_target_update_helpers,
    update_job_target_state,
)


QUEUE_STALE_STATE_EXPORTS = (
    *QUEUE_STALE_RECONCILE_EXPORTS,
    *QUEUE_TARGET_UPDATE_EXPORTS,
    *QUEUE_STALE_RECLAIM_EXPORTS,
)


def install_queue_stale_state_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_STALE_STATE_EXPORTS,
) -> None:
    reconcile_names = tuple(name for name in names if name in QUEUE_STALE_RECONCILE_EXPORTS)
    target_update_names = tuple(name for name in names if name in QUEUE_TARGET_UPDATE_EXPORTS)
    reclaim_names = tuple(name for name in names if name in QUEUE_STALE_RECLAIM_EXPORTS)
    known_names = set(QUEUE_STALE_STATE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_stale_reconcile_helpers(bindings, reconcile_names)
    install_queue_target_update_helpers(bindings, target_update_names)
    install_queue_stale_reclaim_helpers(bindings, reclaim_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
