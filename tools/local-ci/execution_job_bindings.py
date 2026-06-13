"""Compatibility facade for validation job orchestration dependency bindings."""

from __future__ import annotations

from execution_job_config_bindings import (
    config_for_job_execution,
    resolve_ssh_target_execution,
    submission_target_state,
)
from execution_result_io_bindings import (
    print_result,
    save_result,
)
from execution_target_task_bindings import (
    build_target_tasks,
    process_job,
)
