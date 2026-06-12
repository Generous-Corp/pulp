"""Validation command execution helpers for local CI.

This module owns target-neutral command result assembly and local/POSIX/Windows
validation runner orchestration.
"""

from __future__ import annotations

from collections.abc import Callable
from datetime import datetime
import json
import shlex
import subprocess
from pathlib import Path

from state_paths import state_dir
from validation_commands import (
    local_validation_command,
    posix_ssh_validation_command,
    prepared_state_root,
    remote_commit_error,
    should_reuse_prepared_state,
    windows_validation_script,
)
from validation_results import (
    completed_job_result,
    sorted_target_results,
    target_exception_result,
    unreachable_target_result,
    validation_error_result,
    validation_result_from_run,
)
from validation_logging import (
    HEARTBEAT_INTERVAL_SECS,
    STUCK_IDLE_SECS,
    parse_progress_marker,
    run_logged_command,
)
from validation_job import (
    process_job,
    run_target_tasks,
)
from validation_planning import (
    build_target_tasks,
    config_for_job_execution,
    resolve_ssh_target_execution,
    submission_target_state,
)
from validation_runners import (
    run_local_validation,
    run_posix_ssh_validation,
    run_windows_ssh_validation,
)


def save_result(
    result: dict,
    *,
    ensure_state_dirs_fn: Callable[[], None],
    results_dir_fn: Callable[[], Path],
    update_evidence_index_fn: Callable[[dict, Path], None],
    now_fn: Callable[[], datetime] = datetime.now,
) -> Path:
    ensure_state_dirs_fn()
    ts = now_fn().strftime("%Y%m%d-%H%M%S")
    branch_slug = result["branch"].replace("/", "-")
    path = results_dir_fn() / f"{ts}-{result['job_id']}-{branch_slug}.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    update_evidence_index_fn(result, path)
    return path


def print_result(
    result: dict,
    result_path: Path | None = None,
    *,
    normalize_result_fn: Callable[[dict], dict],
    result_validation_line_fn: Callable[[dict], str | None],
    result_execution_line_fn: Callable[[dict], str],
    result_target_lines_fn: Callable[[dict], list[str]],
    result_overall_line_fn: Callable[[dict], str],
    print_fn: Callable[[str], None] = print,
) -> None:
    result = normalize_result_fn(result)
    print_fn(f"\n--- Result: [{result['job_id']}] {result['branch']} ---")
    validation_line = result_validation_line_fn(result)
    if validation_line:
        print_fn(validation_line)
    print_fn(result_execution_line_fn(result))
    for line in result_target_lines_fn(result):
        print_fn(line)
    print_fn(result_overall_line_fn(result))
    if result_path:
        print_fn(f"  Saved: {result_path}")
    print_fn("")
