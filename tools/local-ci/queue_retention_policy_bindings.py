"""Compatibility facade for queue retention and selection policy helpers."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_retention_trim_bindings import (
    QUEUE_RETENTION_TRIM_EXPORTS,
    install_queue_retention_trim_helpers,
    trim_completed_jobs,
    trim_completed_jobs_with_removed_ids,
)
from queue_selection_policy_bindings import (
    QUEUE_SELECTION_POLICY_EXPORTS,
    find_job_unlocked,
    install_queue_selection_policy_helpers,
    job_sort_key,
    queue_status_groups,
    recent_completed_jobs_for_status,
)


QUEUE_RETENTION_POLICY_EXPORTS = (
    *QUEUE_RETENTION_TRIM_EXPORTS,
    *QUEUE_SELECTION_POLICY_EXPORTS,
)


def install_queue_retention_policy_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_RETENTION_POLICY_EXPORTS,
) -> None:
    trim_names = tuple(name for name in names if name in QUEUE_RETENTION_TRIM_EXPORTS)
    selection_names = tuple(name for name in names if name in QUEUE_SELECTION_POLICY_EXPORTS)
    known_names = set(QUEUE_RETENTION_POLICY_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_retention_trim_helpers(bindings, trim_names)
    install_queue_selection_policy_helpers(bindings, selection_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
