"""Pure queue policy helpers for local CI.

This module owns job identity, enqueue duplicate/priority policy, enqueue
supersedence candidate selection, queue-command lookup and priority mutation,
priority ordering, supersedence, cancellation result payloads, summaries,
target-state status detail formatting, status active-target selection and
recent-completed selection, completed-job state mutation, queue status grouping,
and completed-queue retention.
Higher-level queue mutation, locking, runner liveness, result persistence, and
drain orchestration live in queue_lifecycle.py and runner_state.py, with
queue_bindings.py preserving the historical local_ci.py facade exports.
"""

from __future__ import annotations

from collections.abc import Callable

from git_helpers import now_iso
from normalize import normalize_priority, normalize_validation_mode
from provenance import provenance_summary
from queue_completion import (
    cancellation_result,
    complete_job_unlocked,
    complete_job_with_result_unlocked,
    supersedence_result,
    trim_completed_jobs,
    trim_completed_jobs_with_removed_ids,
)
from queue_display import (
    bump_queue_command_result_line,
    cancel_queue_command_result_line,
    drain_runner_active_line,
    empty_log_line,
    enqueue_command_result_line,
    job_logs_header_line,
    log_section_header_line,
    missing_job_logs_line,
    missing_log_files_line,
    recent_completed_missing_result_line,
    recent_completed_status_line,
    result_execution_line,
    result_overall_line,
    result_target_lines,
    result_validation_line,
    status_active_targets,
    status_runner_line,
    status_submission_lines,
    status_target_detail_lines,
    status_target_states,
    summarize_active_targets,
    summarize_job,
    target_result_line,
    target_state_detail_parts,
)
from queue_jobs import (
    ROOT,
    bump_pending_job_priority_unlocked,
    default_priority_for,
    find_active_job_by_fingerprint_unlocked,
    make_fingerprint,
    make_job,
    validate_ci_branch_name,
)
from queue_selection import (
    claim_next_job_unlocked,
    find_job_unlocked,
    job_sort_key,
    queue_status_groups,
    recent_completed_jobs_for_status,
    select_job_for_logs,
)
from queue_supersedence import (
    job_has_narrower_same_identity_scope,
    jobs_share_supersedence_scope,
    pending_supersedence_candidates_unlocked,
    supersedence_identity_key,
    supersedence_key,
    supersedence_reason,
)
from queue_stale import (
    find_stale_running_replacement_unlocked,
    requeue_stale_running_job_unlocked,
    stale_running_jobs_for_runner_unlocked,
    stale_running_reconciliation_actions_unlocked,
)
from queue_target_state import (
    completed_target_state,
    initial_target_state,
    target_state_snapshot,
    update_job_target_state_unlocked,
    update_runner_info_active_targets,
    updated_target_state,
    upsert_job_active_targets_unlocked,
)


def find_queue_command_job_unlocked(queue: list[dict], job_ref: str) -> dict | None:
    return find_job_unlocked(queue, job_ref, statuses={"pending", "running"})


def set_pending_job_priority_unlocked(
    job: dict,
    requested_priority: str,
    *,
    now_iso_fn: Callable[[], str] = now_iso,
) -> bool:
    if job.get("status") != "pending":
        return False

    job["priority"] = normalize_priority(requested_priority)
    job["bumped_at"] = now_iso_fn()
    return True
