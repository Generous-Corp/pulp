"""Compatibility facade for queue drain and runner lifecycle bindings."""

from __future__ import annotations

from queue_claim_finalize_bindings import QUEUE_CLAIM_FINALIZE_EXPORTS, claim_next_job, finalize_job
from queue_wait_drain_bindings import QUEUE_WAIT_DRAIN_EXPORTS, drain_pending_jobs, wait_for_job


QUEUE_DRAIN_EXPORTS = (
    *QUEUE_CLAIM_FINALIZE_EXPORTS,
    *QUEUE_WAIT_DRAIN_EXPORTS,
)
