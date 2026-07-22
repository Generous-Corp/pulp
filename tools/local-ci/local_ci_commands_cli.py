#!/usr/bin/env python3
"""Top-level local-CI command orchestration."""

from __future__ import annotations

from argparse import Namespace
from collections.abc import Callable
from pathlib import Path

from local_ci_pr_commands_cli import cmd_check, cmd_list, cmd_ship
from local_ci_status_cli import cmd_status


def resolve_submission_options(
    args: Namespace,
    command: str,
    *,
    load_config_fn: Callable[[], dict],
    current_branch_fn: Callable[[], str | None],
    resolve_git_ref_sha_fn: Callable[[str], str],
    current_sha_fn: Callable[[], str],
    resolve_targets_fn: Callable[[dict, list[str] | None], list[str]],
    parse_targets_arg_fn: Callable[[str | None], list[str] | None],
    normalize_priority_fn: Callable[[str], str],
    default_priority_for_fn: Callable[[str, dict], str],
    normalize_validation_mode_fn: Callable[[str], str],
    build_submission_metadata_fn: Callable[..., dict],
) -> tuple[dict, str, str, list[str], str, str, dict]:
    config = load_config_fn()
    branch = args.branch or current_branch_fn()
    if args.sha:
        sha = args.sha
    elif args.branch:
        sha = resolve_git_ref_sha_fn(branch)
    else:
        sha = current_sha_fn()
    targets = resolve_targets_fn(config, parse_targets_arg_fn(getattr(args, "targets", None)))
    priority = normalize_priority_fn(getattr(args, "priority", None) or default_priority_for_fn(command, config))
    validation = normalize_validation_mode_fn("smoke" if getattr(args, "smoke", False) else "full")
    submission = build_submission_metadata_fn(
        config,
        branch,
        sha,
        targets,
        priority,
        validation,
        allow_root_mismatch=bool(getattr(args, "allow_root_mismatch", False)),
        allow_unreachable_targets=bool(getattr(args, "allow_unreachable_targets", False)),
    )
    return config, branch, sha, targets, priority, validation, submission


def cmd_enqueue(
    args: Namespace,
    *,
    resolve_submission_options_fn: Callable[[Namespace, str], tuple[dict, str, str, list[str], str, str, dict]],
    print_submission_metadata_fn: Callable[[dict], None],
    enqueue_job_fn: Callable[..., tuple[dict, bool]],
    enqueue_command_result_line_fn: Callable[..., str],
    print_fn: Callable[..., None] = print,
) -> int:
    try:
        _config, branch, sha, targets, priority, validation, submission = resolve_submission_options_fn(args, "enqueue")
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    print_submission_metadata_fn(submission)
    job, created = enqueue_job_fn(branch, sha, priority, targets, "enqueue", validation, submission=submission)
    print_fn(enqueue_command_result_line_fn(job, created=created))
    return 0


def cmd_drain(
    _args: Namespace,
    *,
    load_config_fn: Callable[[], dict],
    drain_pending_jobs_fn: Callable[..., tuple[bool, bool]],
    current_runner_info_fn: Callable[[], dict | None],
    drain_runner_active_line_fn: Callable[[dict | None], str],
    notify_fn: Callable[[str], None],
    print_fn: Callable[..., None] = print,
) -> int:
    try:
        config = load_config_fn()
    except FileNotFoundError as exc:
        print_fn(f"Error: {exc}")
        return 1

    acquired, any_failure = drain_pending_jobs_fn(config, blocking=False)
    if not acquired:
        print_fn(drain_runner_active_line_fn(current_runner_info_fn()))
        return 0

    notify_fn("CI complete" + (" - PASSED" if not any_failure else " - FAILED"))
    return 1 if any_failure else 0


def cmd_run(
    args: Namespace,
    *,
    resolve_submission_options_fn: Callable[[Namespace, str], tuple[dict, str, str, list[str], str, str, dict]],
    print_submission_metadata_fn: Callable[[dict], None],
    gh_workflow_dispatch_fn: Callable[[str, str, str, dict], object],
    enqueue_job_fn: Callable[..., tuple[dict, bool]],
    enqueue_command_result_line_fn: Callable[..., str],
    wait_for_job_fn: Callable[[str, dict], tuple[dict | None, int]],
    load_job_fn: Callable[[str], dict | None],
    print_result_fn: Callable[[dict, Path | None], None],
    notify_fn: Callable[[str], None],
    path_cls: type[Path] = Path,
    print_fn: Callable[..., None] = print,
) -> int:
    try:
        config, branch, sha, targets, priority, validation, submission = resolve_submission_options_fn(args, "run")
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    print_submission_metadata_fn(submission)

    failover_targets = submission.get("namespace_failover_targets", [])
    if failover_targets:
        ga_cfg = config.get("github_actions", {})
        repository = ga_cfg.get("repository", "Generous-Corp/pulp")
        print_fn(f"\n\u26a0\ufe0f  Namespace failover: dispatching {', '.join(failover_targets)} to Namespace")
        try:
            gh_workflow_dispatch_fn(repository, "build.yml", branch, {"runner_provider": "namespace"})
            print_fn(f"  Dispatched Namespace run for {branch}")
        except Exception as exc:
            print_fn(f"  Warning: Namespace dispatch failed: {exc}")

    local_targets = [target for target in targets if target not in failover_targets]
    if local_targets:
        job, created = enqueue_job_fn(branch, sha, priority, local_targets, "run", validation, submission=submission)
        print_fn(enqueue_command_result_line_fn(job, created=created))

        result, exit_code = wait_for_job_fn(job["id"], config)
        if result is not None:
            loaded_job = load_job_fn(job["id"])
            print_result_fn(result, path_cls(loaded_job["result_file"]))
    else:
        print_fn("All targets dispatched to Namespace \u2014 no local work to do.")
        exit_code = 0

    if failover_targets:
        print_fn(f"\nNote: {', '.join(failover_targets)} results are on Namespace.")
        print_fn("  Check with: python3 tools/local-ci/local_ci.py cloud status")

    notify_fn("CI run complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code
