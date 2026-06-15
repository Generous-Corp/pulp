"""Compatibility facade for queue runner-state bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_runner_active_bindings import (
    QUEUE_RUNNER_ACTIVE_EXPORTS,
    install_queue_runner_active_helpers,
    update_runner_active_targets,
)
from queue_runner_info_bindings import (
    QUEUE_RUNNER_INFO_EXPORTS,
    clear_runner_info,
    current_runner_info,
    install_queue_runner_info_helpers,
    pid_alive,
    read_runner_info,
    write_runner_info,
)
from queue_runner_stale_bindings import (
    QUEUE_RUNNER_STALE_EXPORTS,
    install_queue_runner_stale_helpers,
    stale_running_jobs_unlocked,
)


QUEUE_RUNNER_EXPORTS = (
    *QUEUE_RUNNER_INFO_EXPORTS,
    *QUEUE_RUNNER_STALE_EXPORTS,
    *QUEUE_RUNNER_ACTIVE_EXPORTS,
)


def install_queue_runner_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_RUNNER_EXPORTS) -> None:
    info_names = tuple(name for name in names if name in QUEUE_RUNNER_INFO_EXPORTS)
    stale_names = tuple(name for name in names if name in QUEUE_RUNNER_STALE_EXPORTS)
    active_names = tuple(name for name in names if name in QUEUE_RUNNER_ACTIVE_EXPORTS)
    known_names = set(QUEUE_RUNNER_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_runner_info_helpers(bindings, info_names)
    install_queue_runner_stale_helpers(bindings, stale_names)
    install_queue_runner_active_helpers(bindings, active_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
