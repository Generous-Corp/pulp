"""Compatibility facade for queue runner-state bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_runner_active_bindings import QUEUE_RUNNER_ACTIVE_EXPORTS, update_runner_active_targets
from queue_runner_info_bindings import (
    QUEUE_RUNNER_INFO_EXPORTS,
    clear_runner_info,
    current_runner_info,
    pid_alive,
    read_runner_info,
    write_runner_info,
)
from queue_runner_stale_bindings import QUEUE_RUNNER_STALE_EXPORTS, stale_running_jobs_unlocked


QUEUE_RUNNER_EXPORTS = (
    *QUEUE_RUNNER_INFO_EXPORTS,
    *QUEUE_RUNNER_STALE_EXPORTS,
    *QUEUE_RUNNER_ACTIVE_EXPORTS,
)


def install_queue_runner_helpers(bindings: dict[str, Any], names: tuple[str, ...] = QUEUE_RUNNER_EXPORTS) -> None:
    known_names = set(QUEUE_RUNNER_EXPORTS)
    runner_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), runner_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
