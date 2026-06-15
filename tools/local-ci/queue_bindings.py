"""Compatibility installer for local_ci queue facade bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_display_bindings import (
    QUEUE_DISPLAY_EXPORTS,
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
    install_queue_display_helpers,
)
from queue_policy_bindings import (
    QUEUE_POLICY_EXPORTS,
    cancellation_result,
    default_priority_for,
    find_job_unlocked,
    job_has_narrower_same_identity_scope,
    job_sort_key,
    jobs_share_supersedence_scope,
    make_fingerprint,
    make_job,
    queue_status_groups,
    recent_completed_jobs_for_status,
    supersedence_identity_key,
    supersedence_key,
    supersedence_reason,
    supersedence_result,
    trim_completed_jobs,
    trim_completed_jobs_with_removed_ids,
    validate_ci_branch_name,
    install_queue_policy_helpers,
)
from queue_runner_bindings import (
    QUEUE_RUNNER_EXPORTS,
    clear_runner_info,
    current_runner_info,
    pid_alive,
    read_runner_info,
    stale_running_jobs_unlocked,
    update_runner_active_targets,
    write_runner_info,
    install_queue_runner_helpers,
)
from queue_lifecycle_bindings import (
    QUEUE_LIFECYCLE_EXPORTS,
    bump_queue_command_job,
    cancel_job_unlocked,
    cancel_queue_command_job,
    claim_next_job,
    drain_pending_jobs,
    enqueue_job,
    finalize_job,
    load_job,
    load_queue,
    reclaim_stale_remote_validators,
    reconcile_running_jobs_unlocked,
    supersede_job_unlocked,
    update_job_active_targets,
    update_job_target_state,
    wait_for_job,
    install_queue_lifecycle_helpers,
)
from queue_target_state_bindings import (
    QUEUE_TARGET_STATE_EXPORTS,
    completed_target_state,
    initial_target_state,
    target_state_snapshot,
    updated_target_state,
    upsert_job_active_targets_unlocked,
    install_queue_target_state_helpers,
)


QUEUE_EXPORTS = (
    *QUEUE_LIFECYCLE_EXPORTS,
    *QUEUE_POLICY_EXPORTS,
    *QUEUE_DISPLAY_EXPORTS,
    *QUEUE_TARGET_STATE_EXPORTS,
    *QUEUE_RUNNER_EXPORTS,
)


def install_queue_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_EXPORTS) -> None:
    lifecycle_names = tuple(name for name in names if name in QUEUE_LIFECYCLE_EXPORTS)
    policy_names = tuple(name for name in names if name in QUEUE_POLICY_EXPORTS)
    display_names = tuple(name for name in names if name in QUEUE_DISPLAY_EXPORTS)
    target_state_names = tuple(name for name in names if name in QUEUE_TARGET_STATE_EXPORTS)
    runner_names = tuple(name for name in names if name in QUEUE_RUNNER_EXPORTS)
    known_names = set(QUEUE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_lifecycle_helpers(bindings, lifecycle_names)
    install_queue_policy_helpers(bindings, policy_names)
    install_queue_display_helpers(bindings, display_names)
    install_queue_target_state_helpers(bindings, target_state_names)
    install_queue_runner_helpers(bindings, runner_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
