"""Bindings from the local_ci facade to locked queue lifecycle helpers."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_command_lifecycle_bindings import (
    QUEUE_COMMAND_LIFECYCLE_EXPORTS,
    bump_queue_command_job,
    cancel_job_unlocked,
    cancel_queue_command_job,
    install_queue_command_lifecycle_helpers,
    supersede_job_unlocked,
)
from queue_drain_bindings import (
    QUEUE_DRAIN_EXPORTS,
    claim_next_job,
    drain_pending_jobs,
    finalize_job,
    install_queue_drain_helpers,
    wait_for_job,
)
from queue_enqueue_bindings import (
    QUEUE_ENQUEUE_EXPORTS,
    enqueue_job,
    install_queue_enqueue_helpers,
)
from queue_load_bindings import (
    QUEUE_LOAD_EXPORTS,
    install_queue_load_helpers,
    load_queue,
)
from queue_state_lifecycle_bindings import (
    QUEUE_STATE_LIFECYCLE_EXPORTS,
    load_job,
    reclaim_stale_remote_validators,
    reconcile_running_jobs_unlocked,
    install_queue_state_lifecycle_helpers,
    update_job_active_targets,
    update_job_target_state,
)


QUEUE_LIFECYCLE_EXPORTS = (
    *QUEUE_COMMAND_LIFECYCLE_EXPORTS[:2],
    *QUEUE_LOAD_EXPORTS,
    QUEUE_STATE_LIFECYCLE_EXPORTS[0],
    *QUEUE_ENQUEUE_EXPORTS,
    *QUEUE_COMMAND_LIFECYCLE_EXPORTS[2:],
    *QUEUE_STATE_LIFECYCLE_EXPORTS[1:],
    *QUEUE_DRAIN_EXPORTS,
)


def install_queue_lifecycle_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_LIFECYCLE_EXPORTS,
) -> None:
    load_names = tuple(name for name in names if name in QUEUE_LOAD_EXPORTS)
    enqueue_names = tuple(name for name in names if name in QUEUE_ENQUEUE_EXPORTS)
    command_names = tuple(name for name in names if name in QUEUE_COMMAND_LIFECYCLE_EXPORTS)
    state_names = tuple(name for name in names if name in QUEUE_STATE_LIFECYCLE_EXPORTS)
    drain_names = tuple(name for name in names if name in QUEUE_DRAIN_EXPORTS)
    known_names = set(
        QUEUE_LOAD_EXPORTS
        + QUEUE_ENQUEUE_EXPORTS
        + QUEUE_COMMAND_LIFECYCLE_EXPORTS
        + QUEUE_STATE_LIFECYCLE_EXPORTS
        + QUEUE_DRAIN_EXPORTS
    )
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_load_helpers(bindings, load_names)
    install_queue_enqueue_helpers(bindings, enqueue_names)
    install_queue_command_lifecycle_helpers(bindings, command_names)
    install_queue_state_lifecycle_helpers(bindings, state_names)
    install_queue_drain_helpers(bindings, drain_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
