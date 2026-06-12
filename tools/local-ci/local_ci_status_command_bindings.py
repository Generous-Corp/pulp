"""Bindings from the local_ci facade to the local-CI status command."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


LOCAL_CI_STATUS_COMMAND_EXPORTS = (
    "cmd_status",
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
