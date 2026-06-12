"""Bindings from the local_ci facade to top-level local-CI command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


LOCAL_CI_COMMAND_EXPORTS = (
    "resolve_submission_options",
    "cmd_enqueue",
    "cmd_drain",
    "cmd_run",
    "cmd_ship",
    "cmd_check",
    "cmd_list",
    "cmd_status",
)


def resolve_submission_options(bindings: Mapping[str, Any], args: Any, command: str) -> tuple[dict, str, str, list[str], str, str, dict]:
    return _binding(bindings, "_local_ci_commands_cli").resolve_submission_options(
        args,
        command,
        load_config_fn=_binding(bindings, "load_config"),
        current_branch_fn=_binding(bindings, "current_branch"),
        resolve_git_ref_sha_fn=_binding(bindings, "resolve_git_ref_sha"),
        current_sha_fn=_binding(bindings, "current_sha"),
        resolve_targets_fn=_binding(bindings, "resolve_targets"),
        parse_targets_arg_fn=_binding(bindings, "parse_targets_arg"),
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        default_priority_for_fn=_binding(bindings, "default_priority_for"),
        normalize_validation_mode_fn=_binding(bindings, "normalize_validation_mode"),
        build_submission_metadata_fn=_binding(bindings, "build_submission_metadata"),
    )


def cmd_enqueue(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_enqueue(
        args,
        resolve_submission_options_fn=_binding(bindings, "resolve_submission_options"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        enqueue_command_result_line_fn=_binding(bindings, "enqueue_command_result_line"),
    )


def cmd_drain(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_drain(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        drain_pending_jobs_fn=_binding(bindings, "drain_pending_jobs"),
        current_runner_info_fn=_binding(bindings, "current_runner_info"),
        drain_runner_active_line_fn=_binding(bindings, "drain_runner_active_line"),
        notify_fn=_binding(bindings, "notify"),
    )


def cmd_run(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_run(
        args,
        resolve_submission_options_fn=_binding(bindings, "resolve_submission_options"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        gh_workflow_dispatch_fn=_binding(bindings, "gh_workflow_dispatch"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        enqueue_command_result_line_fn=_binding(bindings, "enqueue_command_result_line"),
        wait_for_job_fn=_binding(bindings, "wait_for_job"),
        load_job_fn=_binding(bindings, "load_job"),
        print_result_fn=_binding(bindings, "print_result"),
        notify_fn=_binding(bindings, "notify"),
    )


def cmd_ship(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_ship(
        args,
        resolve_submission_options_fn=_binding(bindings, "resolve_submission_options"),
        gh_available_fn=_binding(bindings, "gh_available"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        root=_binding(bindings, "ROOT"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
        gh_pr_create_fn=_binding(bindings, "gh_pr_create"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        summarize_job_fn=_binding(bindings, "summarize_job"),
        wait_for_job_fn=_binding(bindings, "wait_for_job"),
        gh_pr_comment_fn=_binding(bindings, "gh_pr_comment"),
        format_ci_comment_fn=_binding(bindings, "format_ci_comment"),
        gh_pr_merge_fn=_binding(bindings, "gh_pr_merge"),
        notify_fn=_binding(bindings, "notify"),
    )


def cmd_check(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_check(
        args,
        gh_available_fn=_binding(bindings, "gh_available"),
        gh_pr_head_fn=_binding(bindings, "gh_pr_head"),
        short_sha_fn=_binding(bindings, "short_sha"),
        load_config_fn=_binding(bindings, "load_config"),
        resolve_targets_fn=_binding(bindings, "resolve_targets"),
        parse_targets_arg_fn=_binding(bindings, "parse_targets_arg"),
        normalize_priority_fn=_binding(bindings, "normalize_priority"),
        default_priority_for_fn=_binding(bindings, "default_priority_for"),
        normalize_validation_mode_fn=_binding(bindings, "normalize_validation_mode"),
        build_submission_metadata_fn=_binding(bindings, "build_submission_metadata"),
        print_submission_metadata_fn=_binding(bindings, "print_submission_metadata"),
        enqueue_job_fn=_binding(bindings, "enqueue_job"),
        summarize_job_fn=_binding(bindings, "summarize_job"),
        wait_for_job_fn=_binding(bindings, "wait_for_job"),
        gh_pr_comment_fn=_binding(bindings, "gh_pr_comment"),
        format_ci_comment_fn=_binding(bindings, "format_ci_comment"),
        notify_fn=_binding(bindings, "notify"),
    )


def cmd_list(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_list(
        args,
        gh_available_fn=_binding(bindings, "gh_available"),
        gh_pr_list_open_fn=_binding(bindings, "gh_pr_list_open"),
        open_pr_list_lines_fn=_binding(bindings, "open_pr_list_lines"),
    )


def cmd_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_local_ci_commands_cli").cmd_status(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        load_queue_fn=_binding(bindings, "load_queue"),
        queue_status_groups_fn=_binding(bindings, "queue_status_groups"),
        current_runner_info_fn=_binding(bindings, "current_runner_info"),
        state_dir_fn=_binding(bindings, "state_dir"),
        config_path_fn=_binding(bindings, "config_path"),
        status_runner_line_fn=_binding(bindings, "status_runner_line"),
        summarize_job_fn=_binding(bindings, "summarize_job"),
        status_submission_lines_fn=_binding(bindings, "status_submission_lines"),
        status_active_targets_fn=_binding(bindings, "status_active_targets"),
        summarize_active_targets_fn=_binding(bindings, "summarize_active_targets"),
        status_target_detail_lines_fn=_binding(bindings, "status_target_detail_lines"),
        recent_completed_jobs_for_status_fn=_binding(bindings, "recent_completed_jobs_for_status"),
        load_result_fn=_binding(bindings, "load_result"),
        recent_completed_status_line_fn=_binding(bindings, "recent_completed_status_line"),
        recent_completed_missing_result_line_fn=_binding(bindings, "recent_completed_missing_result_line"),
        current_branch_fn=_binding(bindings, "current_branch"),
        print_evidence_summary_fn=_binding(bindings, "print_evidence_summary"),
        list_cloud_records_fn=_binding(bindings, "list_cloud_records"),
        load_optional_config_fn=_binding(bindings, "load_optional_config"),
        github_actions_settings_for_display_fn=_binding(bindings, "github_actions_settings_for_display"),
        resolve_github_actions_settings_fn=_binding(bindings, "resolve_github_actions_settings"),
        resolve_default_provider_for_workflow_fn=_binding(bindings, "resolve_default_provider_for_workflow"),
        print_billing_period_summary_fn=_binding(bindings, "print_billing_period_summary"),
        estimate_billing_period_totals_fn=_binding(bindings, "estimate_billing_period_totals"),
        cloud_record_summary_fn=_binding(bindings, "cloud_record_summary"),
        print_state_footprint_fn=_binding(bindings, "print_local_ci_state_footprint"),
        utmctl_vm_status_fn=_binding(bindings, "utmctl_vm_status"),
        ssh_reachable_fn=_binding(bindings, "ssh_reachable"),
    )


def install_local_ci_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = LOCAL_CI_COMMAND_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
