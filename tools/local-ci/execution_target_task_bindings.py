"""Bindings from the local_ci facade to validation target task helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from binding_utils import print_binding as _print_binding


EXECUTION_TARGET_TASK_EXPORTS = (
    "build_target_tasks",
    "process_job",
)


def build_target_tasks(bindings: Mapping[str, Any], job: dict, config: dict, progress_factory=None) -> list[tuple[str, Any]]:
    return _binding(bindings, "_execution").build_target_tasks(
        job,
        config,
        enabled_targets_fn=_binding(bindings, "enabled_targets"),
        resolve_ssh_target_execution_fn=_binding(bindings, "resolve_ssh_target_execution"),
        run_local_validation_fn=_binding(bindings, "run_local_validation"),
        run_posix_ssh_validation_fn=_binding(bindings, "run_posix_ssh_validation"),
        run_windows_ssh_validation_fn=_binding(bindings, "run_windows_ssh_validation"),
        progress_factory=progress_factory,
    )


def process_job(bindings: Mapping[str, Any], job: dict, config: dict) -> dict:
    return _binding(bindings, "_execution").process_job(
        job,
        config,
        print_fn=_print_binding(bindings),
        short_sha_fn=_binding(bindings, "short_sha"),
        config_for_job_execution_fn=_binding(bindings, "config_for_job_execution"),
        build_target_tasks_fn=_binding(bindings, "_build_target_tasks"),
        target_state_snapshot_fn=_binding(bindings, "target_state_snapshot"),
        update_runner_active_targets_fn=_binding(bindings, "update_runner_active_targets"),
        update_job_active_targets_fn=_binding(bindings, "update_job_active_targets"),
        updated_target_state_fn=_binding(bindings, "updated_target_state"),
        initial_target_state_fn=_binding(bindings, "initial_target_state"),
        completed_target_state_fn=_binding(bindings, "completed_target_state"),
        now_iso_fn=_binding(bindings, "now_iso"),
        run_target_tasks_fn=_binding(bindings, "run_target_tasks"),
        completed_job_result_fn=_binding(bindings, "completed_job_result"),
        sorted_target_results_fn=_binding(bindings, "sorted_target_results"),
    )


def install_execution_target_task_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_TARGET_TASK_EXPORTS,
) -> None:
    known_names = set(EXECUTION_TARGET_TASK_EXPORTS)
    task_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), task_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
