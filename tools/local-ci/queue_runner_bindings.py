"""Bindings from the local_ci facade to queue runner-state helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_RUNNER_EXPORTS = (
    "read_runner_info",
    "pid_alive",
    "current_runner_info",
    "stale_running_jobs_unlocked",
    "write_runner_info",
    "update_runner_active_targets",
    "clear_runner_info",
)


def read_runner_info(bindings: Mapping[str, Any]) -> dict | None:
    return _binding(bindings, "_runner_state").read_runner_info()


def pid_alive(bindings: Mapping[str, Any], pid: int | None) -> bool:
    return _binding(bindings, "_runner_state").pid_alive(pid)


def current_runner_info(bindings: Mapping[str, Any]) -> dict | None:
    return _binding(bindings, "_runner_state").current_runner_info()


def stale_running_jobs_unlocked(bindings: Mapping[str, Any], queue: list[dict]) -> list[dict]:
    return _binding(bindings, "_runner_state").stale_running_jobs_for_current_runner(
        queue,
        stale_running_jobs_for_runner_unlocked_fn=_binding(
            bindings,
            "_queue_orchestrator",
        ).stale_running_jobs_for_runner_unlocked,
    )


def write_runner_info(bindings: Mapping[str, Any], info: dict) -> None:
    _binding(bindings, "_runner_state").write_runner_info(info)


def update_runner_active_targets(bindings: Mapping[str, Any], job_id: str, active_targets: dict | None) -> None:
    def update_info(info: dict, current_job_id: str, current_active_targets: dict | None) -> bool:
        return _binding(bindings, "_queue_orchestrator").update_runner_info_active_targets(
            info,
            current_job_id,
            current_active_targets,
            now_iso_fn=_binding(bindings, "now_iso"),
        )

    _binding(bindings, "_runner_state").update_current_runner_active_targets(
        job_id,
        active_targets,
        update_runner_info_active_targets_fn=update_info,
    )


def clear_runner_info(bindings: Mapping[str, Any]) -> None:
    _binding(bindings, "_runner_state").clear_runner_info()


def install_queue_runner_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_RUNNER_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
