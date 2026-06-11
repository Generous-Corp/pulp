"""Cleanup command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path


def print_local_ci_state_footprint(
    *,
    local_ci_state_footprint_fn: Callable[[], dict],
    state_footprint_lines_fn: Callable[..., list[str]],
    indent: str = "",
    print_fn: Callable[[str], None] = print,
) -> None:
    for line in state_footprint_lines_fn(local_ci_state_footprint_fn(), indent=indent):
        print_fn(line)


def print_local_ci_cleanup_plan(
    plan: dict,
    *,
    dry_run: bool,
    cleanup_plan_lines_fn: Callable[..., list[str]],
    print_fn: Callable[[str], None] = print,
) -> None:
    for line in cleanup_plan_lines_fn(plan, dry_run=dry_run):
        print_fn(line)


def cmd_cleanup(
    args: argparse.Namespace,
    *,
    load_queue_fn: Callable[[], list[dict]],
    collect_cleanup_plan_fn: Callable[..., dict],
    apply_cleanup_plan_fn: Callable[[dict], dict],
    print_cleanup_plan_fn: Callable[..., None],
    print_state_footprint_fn: Callable[..., None],
    format_size_fn: Callable[[object], str],
    describe_path_fn: Callable[[Path], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    queue = load_queue_fn()
    running = [job for job in queue if job.get("status") == "running"]
    if args.apply and running:
        print_fn("Error: cleanup --apply is blocked while local CI jobs are running.")
        return 1

    plan = collect_cleanup_plan_fn(
        queue,
        keep_results=args.keep_results,
        keep_logs=args.keep_logs,
        keep_bundles=args.keep_bundles,
        include_prepared=args.include_prepared,
    )
    print_cleanup_plan_fn(plan, dry_run=not args.apply)

    if not args.apply:
        print_state_footprint_fn(indent="  ")
        if args.include_prepared:
            print_fn("  note: prepared cleanup removes cached build/install state and later reruns will rebuild it.")
        return 0

    result = apply_cleanup_plan_fn(plan)
    print_fn(
        f"\n  removed: {len(result.get('removed', []))} path(s), "
        f"{format_size_fn(result.get('removed_bytes', 0))}"
    )
    if result.get("failed"):
        print_fn(f"  failed: {len(result['failed'])} path(s)")
        for failure in result["failed"][:10]:
            print_fn(f"    {describe_path_fn(Path(failure['path']))}: {failure['error']}")
        return 1
    print_state_footprint_fn(indent="  ")
    if args.include_prepared:
        print_fn("  note: prepared cleanup removes cached build/install state and later reruns will rebuild it.")
    return 0
