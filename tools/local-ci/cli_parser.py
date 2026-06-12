"""Argument parser construction for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Iterable

from cli_parser_cloud import add_cloud_subcommands
from cli_parser_desktop import add_desktop_subcommands


def build_local_ci_parser(
    *,
    priority_values: Iterable[str],
    keep_completed_jobs: int,
    epilog: str | None = None,
) -> argparse.ArgumentParser:
    priority_choices = sorted(priority_values)
    parser = argparse.ArgumentParser(
        description="Local CI runner for Pulp",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=epilog,
    )
    sub = parser.add_subparsers(dest="command")

    def add_submission_args(
        command_parser: argparse.ArgumentParser,
        *,
        include_sha: bool = False,
        allow_smoke: bool = False,
    ) -> None:
        command_parser.add_argument("branch", nargs="?", help="Branch name (default: current)")
        command_parser.add_argument(
            "--priority",
            choices=priority_choices,
            help="Queue priority (default from config; ship/check default to high)",
        )
        command_parser.add_argument(
            "--targets",
            help="Comma-separated target list (for example: mac or mac,ubuntu)",
        )
        command_parser.add_argument(
            "--allow-root-mismatch",
            action="store_true",
            help="Queue even if the current cwd belongs to a different git root than this local_ci.py checkout",
        )
        command_parser.add_argument(
            "--allow-unreachable-targets",
            action="store_true",
            help="Queue even if preflight finds a selected SSH target currently unreachable with no fallback",
        )
        if include_sha:
            command_parser.add_argument("--sha", help="Exact commit SHA to validate (default: current HEAD)")
        if allow_smoke:
            command_parser.add_argument(
                "--smoke",
                action="store_true",
                help="Run the fast clean install/export preflight instead of full validation",
            )

    p_enqueue = sub.add_parser("enqueue", help="Queue a branch for validation")
    add_submission_args(p_enqueue, include_sha=True, allow_smoke=True)

    sub.add_parser("drain", help="Process pending jobs if no other runner is active")

    p_run = sub.add_parser("run", help="Queue validation and wait for completion")
    add_submission_args(p_run, include_sha=True, allow_smoke=True)

    p_ship = sub.add_parser("ship", help="PR -> queued CI -> merge on green")
    add_submission_args(p_ship, include_sha=True)
    p_ship.add_argument("--base", default="main", help="Base branch (default: main)")

    p_check = sub.add_parser("check", help="Validate an existing PR")
    p_check.add_argument("pr", help="PR number, GitHub URL, or 'latest'")
    p_check.add_argument(
        "--priority",
        choices=priority_choices,
        help="Queue priority (default: high)",
    )
    p_check.add_argument(
        "--targets",
        help="Comma-separated target list (for example: mac or mac,ubuntu)",
    )
    p_check.add_argument(
        "--allow-root-mismatch",
        action="store_true",
        help="Queue even if the current cwd belongs to a different git root than this local_ci.py checkout",
    )
    p_check.add_argument(
        "--allow-unreachable-targets",
        action="store_true",
        help="Queue even if preflight finds a selected SSH target currently unreachable with no fallback",
    )
    p_check.add_argument(
        "--smoke",
        action="store_true",
        help="Run the fast clean install/export preflight instead of full validation",
    )

    add_cloud_subcommands(sub)

    sub.add_parser("list", help="Show open PRs")

    p_bump = sub.add_parser("bump", help="Reprioritize a pending job")
    p_bump.add_argument("job", help="Job id, unique id prefix, or exact branch name")
    p_bump.add_argument("priority", choices=priority_choices, help="New priority")

    p_cancel = sub.add_parser("cancel", help="Cancel a pending job")
    p_cancel.add_argument("job", help="Job id, unique id prefix, or exact branch name")

    p_logs = sub.add_parser("logs", help="Tail saved logs for a running or completed job")
    p_logs.add_argument("job", nargs="?", help="Job id, unique id prefix, or exact branch name (default: active/latest)")
    p_logs.add_argument("--target", help="Target name to show (mac, ubuntu, windows)")
    p_logs.add_argument("--lines", type=int, default=80, help="Number of log lines to show (default: 80)")

    p_cleanup = sub.add_parser("cleanup", help="Inspect or prune retained local CI artifacts")
    cleanup_mode = p_cleanup.add_mutually_exclusive_group()
    cleanup_mode.add_argument(
        "--dry-run",
        action="store_true",
        help="Show the cleanup plan without deleting anything (default)",
    )
    cleanup_mode.add_argument(
        "--apply",
        action="store_true",
        help="Delete the reported stale artifacts instead of only showing a dry-run plan",
    )
    p_cleanup.add_argument(
        "--include-prepared",
        action="store_true",
        help="Also include prepared build/install trees; later reruns will rebuild them",
    )
    p_cleanup.add_argument(
        "--keep-results",
        type=int,
        default=keep_completed_jobs,
        help=f"Keep this many orphaned result files outside retained queue history (default: {keep_completed_jobs})",
    )
    p_cleanup.add_argument(
        "--keep-logs",
        type=int,
        default=keep_completed_jobs,
        help=f"Keep this many orphaned log directories outside retained queue history (default: {keep_completed_jobs})",
    )
    p_cleanup.add_argument(
        "--keep-bundles",
        type=int,
        default=0,
        help="Keep this many non-live git bundles instead of deleting all completed-job bundles (default: 0)",
    )

    p_evidence = sub.add_parser("evidence", help="Show accumulated last-good target results by exact SHA")
    p_evidence.add_argument("branch", nargs="?", help="Branch name (default: current)")
    p_evidence.add_argument("--sha", help="Filter to one exact SHA")
    p_evidence.add_argument("--limit", type=int, default=5, help="Shas to show per validation mode (default: 5)")

    sub.add_parser("status", help="Show queue, runner, results, and VM status")

    add_desktop_subcommands(sub)
    return parser
