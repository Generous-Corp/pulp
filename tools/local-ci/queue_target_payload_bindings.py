"""Bindings from the local_ci facade to queue target-state payload helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_TARGET_PAYLOAD_EXPORTS = (
    "initial_target_state",
    "completed_target_state",
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


def updated_target_state(bindings: Mapping[str, Any], previous_state: dict | None, fields: dict) -> dict:
    return _binding(bindings, "_queue_orchestrator").updated_target_state(previous_state, fields)


def target_state_snapshot(bindings: Mapping[str, Any], target_states: dict[str, dict]) -> dict | None:
    return _binding(bindings, "_queue_orchestrator").target_state_snapshot(target_states)


def install_queue_target_payload_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_TARGET_PAYLOAD_EXPORTS,
) -> None:
    known_names = set(QUEUE_TARGET_PAYLOAD_EXPORTS)
    payload_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), payload_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
