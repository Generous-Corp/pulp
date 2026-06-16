#!/usr/bin/env python3
"""Top-level local-CI PR command orchestration."""

from __future__ import annotations

from argparse import Namespace
from collections.abc import Callable
from pathlib import Path
import subprocess


def cmd_ship(
    args: Namespace,
    *,
    resolve_submission_options_fn: Callable[[Namespace, str], tuple[dict, str, str, list[str], str, str, dict]],
    gh_available_fn: Callable[[], bool],
    print_submission_metadata_fn: Callable[[dict], None],
    root: Path,
    gh_pr_create_fn: Callable[[str, str], int | None],
    enqueue_job_fn: Callable[..., tuple[dict, bool]],
    summarize_job_fn: Callable[[dict], str],
    wait_for_job_fn: Callable[[str, dict], tuple[dict | None, int]],
    gh_pr_comment_fn: Callable[[int, str], object],
    format_ci_comment_fn: Callable[[dict], str],
    gh_pr_merge_fn: Callable[[int], bool],
    notify_fn: Callable[[str], None],
    run_fn: Callable[..., subprocess.CompletedProcess] = subprocess.run,
    print_fn: Callable[..., None] = print,
) -> int:
    try:
        config, branch, sha, targets, priority, validation, submission = resolve_submission_options_fn(args, "ship")
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1
    if validation != "full":
        print_fn("Error: ship only supports full validation. Use `run --smoke` or `check --smoke` for preflight.")
        return 1

    base = args.base or "main"
    if branch == base:
        print_fn(f"Error: cannot ship {base} to itself. Checkout a feature branch first.")
        return 1

    if not gh_available_fn():
        print_fn("Error: gh CLI not available or not authenticated. Run: gh auth login")
        return 1

    print_fn(f"\n=== Shipping {branch} -> {base} ===\n")
    print_submission_metadata_fn(submission)
    print_fn(f"  Pushing {branch}...")
    push = run_fn(
        ["git", "push", "-u", "origin", branch],
        cwd=root,
        capture_output=True,
        text=True,
    )
    if push.returncode != 0:
        print_fn(f"  Push failed: {push.stderr.strip()}")
        return 1

    print_fn("  Creating PR...")
    pr_number = gh_pr_create_fn(branch, base)
    if pr_number is None:
        print_fn("  Failed to create or find PR.")
        return 1
    print_fn(f"  PR #{pr_number} ready")

    job, _created = enqueue_job_fn(branch, sha, priority, targets, "ship", validation, submission=submission)
    print_fn(f"  Queueing CI: {summarize_job_fn(job)}")
    result, exit_code = wait_for_job_fn(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment_fn(pr_number, format_ci_comment_fn(result))
    if result["overall"] == "pass":
        print_fn(f"  All targets passed. Merging PR #{pr_number}...")
        if gh_pr_merge_fn(pr_number):
            print_fn(f"  PR #{pr_number} merged and branch deleted.")
            notify_fn(f"PR #{pr_number} shipped to {base}!")
            return 0
        print_fn(f"  Merge failed. PR #{pr_number} is still open.")
        notify_fn(f"PR #{pr_number} CI passed but merge failed")
        return 1

    print_fn(f"  CI failed. PR #{pr_number} left open for review.")
    notify_fn(f"PR #{pr_number} CI failed")
    return exit_code


def cmd_check(
    args: Namespace,
    *,
    gh_available_fn: Callable[[], bool],
    gh_pr_head_fn: Callable[[str], tuple[int, str, str] | None],
    short_sha_fn: Callable[[str], str],
    load_config_fn: Callable[[], dict],
    resolve_targets_fn: Callable[[dict, list[str] | None], list[str]],
    parse_targets_arg_fn: Callable[[str | None], list[str] | None],
    normalize_priority_fn: Callable[[str], str],
    default_priority_for_fn: Callable[[str, dict], str],
    normalize_validation_mode_fn: Callable[[str], str],
    build_submission_metadata_fn: Callable[..., dict],
    print_submission_metadata_fn: Callable[[dict], None],
    enqueue_job_fn: Callable[..., tuple[dict, bool]],
    summarize_job_fn: Callable[[dict], str],
    wait_for_job_fn: Callable[[str, dict], tuple[dict | None, int]],
    gh_pr_comment_fn: Callable[[int, str], object],
    format_ci_comment_fn: Callable[[dict], str],
    notify_fn: Callable[[str], None],
    print_fn: Callable[..., None] = print,
) -> int:
    if not gh_available_fn():
        print_fn("Error: gh CLI not available. Run: gh auth login")
        return 1

    pr_info = gh_pr_head_fn(args.pr)
    if pr_info is None:
        return 1

    pr_number, branch, sha = pr_info
    print_fn(f"  PR #{pr_number} -> branch: {branch} @ {short_sha_fn(sha)}")

    try:
        config = load_config_fn()
        targets = resolve_targets_fn(config, parse_targets_arg_fn(args.targets))
        priority = normalize_priority_fn(args.priority or default_priority_for_fn("check", config))
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
    except (FileNotFoundError, ValueError) as exc:
        print_fn(f"Error: {exc}")
        return 1

    print_submission_metadata_fn(submission)
    job, _created = enqueue_job_fn(branch, sha, priority, targets, "check", validation, submission=submission)
    print_fn(f"  Queueing CI: {summarize_job_fn(job)}")
    result, exit_code = wait_for_job_fn(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment_fn(pr_number, format_ci_comment_fn(result))
    notify_fn("CI check complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code


def cmd_list(
    _args: Namespace,
    *,
    gh_available_fn: Callable[[], bool],
    gh_pr_list_open_fn: Callable[[], list],
    open_pr_list_lines_fn: Callable[[list], list[str]],
    print_fn: Callable[..., None] = print,
) -> int:
    if not gh_available_fn():
        print_fn("Error: gh CLI not available. Run: gh auth login")
        return 1

    prs = gh_pr_list_open_fn()
    for line in open_pr_list_lines_fn(prs):
        print_fn(line)
    return 0
