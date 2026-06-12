"""Validation command execution helpers for local CI.

This module owns target-neutral command result assembly and local/POSIX/Windows
validation runner orchestration.
"""

from __future__ import annotations

import shlex
import subprocess

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
from validation_output import (
    print_result,
    save_result,
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
