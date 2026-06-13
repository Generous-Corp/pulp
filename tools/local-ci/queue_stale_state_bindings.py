"""Compatibility facade for stale queue state dependency bindings."""

from __future__ import annotations

from queue_stale_reclaim_bindings import QUEUE_STALE_RECLAIM_EXPORTS, reclaim_stale_remote_validators
from queue_stale_reconcile_bindings import QUEUE_STALE_RECONCILE_EXPORTS, reconcile_running_jobs_unlocked
from queue_target_update_bindings import QUEUE_TARGET_UPDATE_EXPORTS, update_job_target_state


QUEUE_STALE_STATE_EXPORTS = (
    *QUEUE_STALE_RECONCILE_EXPORTS,
    *QUEUE_TARGET_UPDATE_EXPORTS,
    *QUEUE_STALE_RECLAIM_EXPORTS,
)
