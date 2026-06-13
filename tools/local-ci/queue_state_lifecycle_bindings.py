"""Compatibility facade for queue state lifecycle dependency bindings."""

from __future__ import annotations

from queue_active_load_bindings import load_job, update_job_active_targets
from queue_stale_state_bindings import (
    reclaim_stale_remote_validators,
    reconcile_running_jobs_unlocked,
    update_job_target_state,
)
