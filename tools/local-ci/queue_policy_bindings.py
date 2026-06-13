"""Compatibility facade for queue policy dependency bindings."""

from __future__ import annotations

from queue_job_policy_bindings import (
    default_priority_for,
    make_fingerprint,
    make_job,
    validate_ci_branch_name,
)
from queue_retention_policy_bindings import (
    find_job_unlocked,
    job_sort_key,
    queue_status_groups,
    recent_completed_jobs_for_status,
    trim_completed_jobs,
    trim_completed_jobs_with_removed_ids,
)
from queue_supersedence_policy_bindings import (
    cancellation_result,
    job_has_narrower_same_identity_scope,
    jobs_share_supersedence_scope,
    supersedence_identity_key,
    supersedence_key,
    supersedence_reason,
    supersedence_result,
)
