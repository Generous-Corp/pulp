"""Bindings from the local_ci facade to queue target-state helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_TARGET_STATE_EXPORTS = (
    "initial_target_state",
    "completed_target_state",
    "upsert_job_active_targets_unlocked",
    "updated_target_state",
    "target_state_snapshot",
)


def initial_target_state(bindings: Mapping[str, Any], job_id: str, target_name: str, *, started_at: str) -> dict:
    return _binding(bindings, "_queue_orchestrator").initial_target_state(
        started_at=started_at,
        log_path=str(_binding(bindings, "target_log_path")(job_id, target_name)),
    )


def completed_target_state(
    bindings: Mapping[str, Any],
    job_id: str,
    target_name: str,
    result: dict,
    previous_state: dict | None,
    *,
    completed_at: str,
) -> dict:
    return _binding(bindings, "_queue_orchestrator").completed_target_state(
        result,
        previous_state,
        completed_at=completed_at,
        default_log_path=str(_binding(bindings, "target_log_path")(job_id, target_name)),
    )


def upsert_job_active_targets_unlocked(
    bindings: Mapping[str, Any],
    queue: list[dict],
    job_id: str,
    active_targets: dict | None,
) -> bool:
    return _binding(bindings, "_queue_orchestrator").upsert_job_active_targets_unlocked(
        queue,
        job_id,
        active_targets,
        now_iso_fn=_binding(bindings, "now_iso"),
    )


def updated_target_state(bindings: Mapping[str, Any], previous_state: dict | None, fields: dict) -> dict:
    return _binding(bindings, "_queue_orchestrator").updated_target_state(previous_state, fields)


def target_state_snapshot(bindings: Mapping[str, Any], target_states: dict[str, dict]) -> dict | None:
    return _binding(bindings, "_queue_orchestrator").target_state_snapshot(target_states)


def install_queue_target_state_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_TARGET_STATE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
