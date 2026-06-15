"""Compatibility facade for queue target-state dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_active_target_bindings import (
    QUEUE_ACTIVE_TARGET_EXPORTS,
    install_queue_active_target_helpers,
    upsert_job_active_targets_unlocked,
)
from queue_target_payload_bindings import (
    QUEUE_TARGET_PAYLOAD_EXPORTS,
    completed_target_state,
    initial_target_state,
    install_queue_target_payload_helpers,
    target_state_snapshot,
    updated_target_state,
)


QUEUE_TARGET_STATE_EXPORTS = (
    *QUEUE_TARGET_PAYLOAD_EXPORTS,
    *QUEUE_ACTIVE_TARGET_EXPORTS,
)


def install_queue_target_state_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_TARGET_STATE_EXPORTS,
) -> None:
    payload_names = tuple(name for name in names if name in QUEUE_TARGET_PAYLOAD_EXPORTS)
    active_names = tuple(name for name in names if name in QUEUE_ACTIVE_TARGET_EXPORTS)
    known_names = set(QUEUE_TARGET_STATE_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_target_payload_helpers(bindings, payload_names)
    install_queue_active_target_helpers(bindings, active_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
