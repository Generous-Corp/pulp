"""Compatibility facade for queue policy dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
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


QUEUE_POLICY_EXPORTS = (
    "default_priority_for",
    "make_fingerprint",
    "make_job",
    "supersedence_result",
    "cancellation_result",
    "supersedence_key",
    "supersedence_identity_key",
    "jobs_share_supersedence_scope",
    "job_has_narrower_same_identity_scope",
    "supersedence_reason",
    "trim_completed_jobs_with_removed_ids",
    "trim_completed_jobs",
    "job_sort_key",
    "queue_status_groups",
    "recent_completed_jobs_for_status",
    "find_job_unlocked",
    "validate_ci_branch_name",
)


def install_queue_policy_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_POLICY_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
