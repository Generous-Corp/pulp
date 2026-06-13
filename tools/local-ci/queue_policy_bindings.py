"""Compatibility facade for queue policy dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_job_policy_bindings import (
    QUEUE_JOB_POLICY_EXPORTS,
    default_priority_for,
    make_fingerprint,
    make_job,
    validate_ci_branch_name,
)
from queue_retention_policy_bindings import (
    QUEUE_RETENTION_POLICY_EXPORTS,
    find_job_unlocked,
    job_sort_key,
    queue_status_groups,
    recent_completed_jobs_for_status,
    trim_completed_jobs,
    trim_completed_jobs_with_removed_ids,
)
from queue_supersedence_policy_bindings import (
    QUEUE_SUPERSEDENCE_POLICY_EXPORTS,
    cancellation_result,
    job_has_narrower_same_identity_scope,
    jobs_share_supersedence_scope,
    supersedence_identity_key,
    supersedence_key,
    supersedence_reason,
    supersedence_result,
)


QUEUE_POLICY_EXPORTS = (
    *QUEUE_JOB_POLICY_EXPORTS[:3],
    *QUEUE_SUPERSEDENCE_POLICY_EXPORTS,
    *QUEUE_RETENTION_POLICY_EXPORTS,
    QUEUE_JOB_POLICY_EXPORTS[3],
)


def install_queue_policy_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_POLICY_EXPORTS) -> None:
    known_names = set(QUEUE_POLICY_EXPORTS)
    policy_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), policy_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
