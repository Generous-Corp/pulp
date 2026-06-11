"""Queue command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable


def cmd_bump(
    args: argparse.Namespace,
    *,
    normalize_priority_fn: Callable[[str], str],
    bump_queue_command_job_fn: Callable[[str, str], dict],
    bump_queue_command_result_line_fn: Callable[[dict, str], tuple[int, str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        requested_priority = normalize_priority_fn(args.priority)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    try:
        result = bump_queue_command_job_fn(args.job, requested_priority)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    exit_code, line = bump_queue_command_result_line_fn(result, args.job)
    print_fn(line)
    return exit_code


def cmd_cancel(
    args: argparse.Namespace,
    *,
    cancel_queue_command_job_fn: Callable[[str], dict],
    cancel_queue_command_result_line_fn: Callable[[dict, str], tuple[int, str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        result = cancel_queue_command_job_fn(args.job)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    exit_code, line = cancel_queue_command_result_line_fn(result, args.job)
    print_fn(line)
    return exit_code
