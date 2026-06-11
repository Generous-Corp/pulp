"""Logs command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path


def resolve_job_for_logs(
    job_ref: str | None,
    *,
    load_queue_fn: Callable[[], list[dict]],
    current_runner_info_fn: Callable[[], dict | None],
    select_job_for_logs_fn: Callable[[list[dict], dict | None, str | None], dict | None],
) -> dict | None:
    return select_job_for_logs_fn(load_queue_fn(), current_runner_info_fn(), job_ref)


def cmd_logs(
    args: argparse.Namespace,
    *,
    resolve_job_for_logs_fn: Callable[[str | None], dict | None],
    target_log_path_fn: Callable[[str, str], Path],
    job_logs_dir_fn: Callable[[str], Path],
    tail_lines_fn: Callable[[Path, int], list[str]],
    missing_job_logs_line_fn: Callable[[], str],
    missing_log_files_line_fn: Callable[[dict], str],
    job_logs_header_line_fn: Callable[[dict], str],
    log_section_header_line_fn: Callable[[str], str],
    empty_log_line_fn: Callable[[], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    try:
        job = resolve_job_for_logs_fn(args.job)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    if job is None:
        print_fn(missing_job_logs_line_fn())
        return 1

    paths: list[Path]
    if args.target:
        paths = [target_log_path_fn(job["id"], args.target)]
    else:
        paths = sorted(job_logs_dir_fn(job["id"]).glob("*.log"))

    if not paths:
        print_fn(missing_log_files_line_fn(job))
        return 1

    print_fn(f"{job_logs_header_line_fn(job)}\n")
    for path in paths:
        print_fn(log_section_header_line_fn(path.stem))
        lines = tail_lines_fn(path, args.lines)
        if lines:
            print_fn("".join(lines).rstrip())
        else:
            print_fn(empty_log_line_fn())
        print_fn("")
    return 0
