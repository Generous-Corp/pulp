#!/usr/bin/env python3
"""Top-level local-CI status command orchestration."""

from __future__ import annotations

from argparse import Namespace
from collections.abc import Callable
from pathlib import Path


def cmd_status(
    _args: Namespace,
    *,
    load_config_fn: Callable[[], dict],
    load_queue_fn: Callable[[], list[dict]],
    queue_status_groups_fn: Callable[[list[dict]], tuple[list[dict], list[dict], list[dict]]],
    current_runner_info_fn: Callable[[], dict | None],
    state_dir_fn: Callable[[], Path],
    config_path_fn: Callable[[], Path],
    status_runner_line_fn: Callable[[dict | None], str],
    summarize_job_fn: Callable[[dict], str],
    status_submission_lines_fn: Callable[[dict], list[str]],
    status_active_targets_fn: Callable[..., dict | None],
    summarize_active_targets_fn: Callable[[dict | None, list[str] | None], str],
    status_target_detail_lines_fn: Callable[[dict, dict | None], list[str]],
    recent_completed_jobs_for_status_fn: Callable[[list[dict]], list[dict]],
    load_result_fn: Callable[[Path], dict],
    recent_completed_status_line_fn: Callable[[dict, dict], str],
    recent_completed_missing_result_line_fn: Callable[[dict], str],
    current_branch_fn: Callable[[], str | None],
    print_evidence_summary_fn: Callable[..., bool],
    list_cloud_records_fn: Callable[..., list],
    load_optional_config_fn: Callable[[], dict | None],
    github_actions_settings_for_display_fn: Callable[[dict | None], dict],
    resolve_github_actions_settings_fn: Callable[[dict | None], dict],
    resolve_default_provider_for_workflow_fn: Callable[[dict, str], tuple[str, str]],
    print_billing_period_summary_fn: Callable[..., None],
    estimate_billing_period_totals_fn: Callable[[list, dict | None], dict],
    cloud_record_summary_fn: Callable[[dict, dict | None], str],
    print_state_footprint_fn: Callable[..., None],
    utmctl_vm_status_fn: Callable[[str], str | None],
    ssh_reachable_fn: Callable[[str, int], bool],
    path_cls: type[Path] = Path,
    print_fn: Callable[..., None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    queue = load_queue_fn()
    pending, running, completed = queue_status_groups_fn(queue)
    runner = current_runner_info_fn()

    print_fn(f"State: {state_dir_fn()}")
    print_fn(f"Config: {config_path_fn()}")

    print_fn(f"\n{status_runner_line_fn(runner)}")

    if running:
        print_fn(f"\nRunning ({len(running)}):")
        for job in running:
            print_fn(f"  {summarize_job_fn(job)} started {job.get('started_at', '?')}")
            for line in status_submission_lines_fn(job):
                print_fn(f"    {line}")
            active_targets = status_active_targets_fn(job, runner)
            target_summary = summarize_active_targets_fn(active_targets, job.get("targets"))
            if target_summary:
                print_fn(f"    live targets: {target_summary}")
            for line in status_target_detail_lines_fn(job, active_targets):
                print_fn(f"    {line}")
    else:
        print_fn("\nNo running jobs.")

    if pending:
        print_fn(f"\nPending ({len(pending)}):")
        for job in pending:
            print_fn(f"  {summarize_job_fn(job)} queued {job.get('queued_at', '?')}")
            for line in status_submission_lines_fn(job):
                print_fn(f"    {line}")
            active_targets = status_active_targets_fn(job)
            target_summary = summarize_active_targets_fn(active_targets, job.get("targets"))
            if target_summary:
                progress_at = job.get("last_progress_at") or job.get("requeued_at") or "?"
                print_fn(f"    last known targets: {target_summary} (updated {progress_at})")
            for line in status_target_detail_lines_fn(job, active_targets):
                print_fn(f"    {line}")
    else:
        print_fn("\nNo pending jobs.")

    recent_completed = recent_completed_jobs_for_status_fn(completed)
    if recent_completed:
        print_fn(f"\nRecent ({len(recent_completed)}):")
        for job in recent_completed:
            result_file = job.get("result_file")
            if result_file and path_cls(result_file).exists():
                result = load_result_fn(path_cls(result_file))
                print_fn(f"  {recent_completed_status_line_fn(job, result)}")
            else:
                print_fn(f"  {recent_completed_missing_result_line_fn(job)}")

    branch = current_branch_fn()
    if branch:
        print_fn(f"\nEvidence ({branch}):")
        if not print_evidence_summary_fn(branch=branch, limit=2, indent="  "):
            print_fn("  (none)")

    cloud_records = list_cloud_records_fn(limit=5)
    all_cloud_records = list_cloud_records_fn(limit=None)
    cloud_config = load_optional_config_fn()
    cloud_settings_note = ""
    cloud_settings = github_actions_settings_for_display_fn(cloud_config)
    try:
        resolved_cloud_settings = resolve_github_actions_settings_fn(cloud_config)
        cloud_settings = resolved_cloud_settings
    except ValueError as exc:
        cloud_settings_note = str(exc)
    default_workflow_key = cloud_settings.get("workflow", "build")
    try:
        default_provider, _default_provider_source = resolve_default_provider_for_workflow_fn(
            cloud_settings,
            default_workflow_key,
        )
    except ValueError:
        default_provider = cloud_settings.get("provider", "github-hosted")

    print_fn(
        f"\nCloud defaults: workflow={default_workflow_key} provider={default_provider} "
        "(`pulp ci-local cloud defaults` for selectors and sources)"
    )
    if cloud_settings_note:
        print_fn(f"  note: {cloud_settings_note}")

    if cloud_records:
        print_billing_period_summary_fn(estimate_billing_period_totals_fn(all_cloud_records, cloud_config), indent="  ")
        print_fn("\nCloud (latest 5 known to this machine):")
        for record in cloud_records:
            print_fn(f"  {cloud_record_summary_fn(record, cloud_config)}")

    print_fn()
    print_state_footprint_fn(indent="  ")

    print_fn("\nVM Status:")
    for vm_name in ["Ubuntu 24.04 desktop", "Windows"]:
        print_fn(f"  {vm_name}: {utmctl_vm_status_fn(vm_name) or 'not found'}")

    for host in [
        target_cfg.get("host")
        for target_cfg in config.get("targets", {}).values()
        if target_cfg.get("type") == "ssh"
    ]:
        if host:
            print_fn(f"  ssh {host}: {'up' if ssh_reachable_fn(host, 3) else 'down'}")

    return 0
