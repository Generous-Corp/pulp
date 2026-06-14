"""Bindings from the local_ci facade to queue runner stale-job helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_RUNNER_STALE_EXPORTS = ("stale_running_jobs_unlocked",)


def stale_running_jobs_unlocked(bindings: Mapping[str, Any], queue: list[dict]) -> list[dict]:
    return _binding(bindings, "_runner_state").stale_running_jobs_for_current_runner(
        queue,
        stale_running_jobs_for_runner_unlocked_fn=_binding(
            bindings,
            "_queue_orchestrator",
        ).stale_running_jobs_for_runner_unlocked,
    )


def install_queue_runner_stale_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_RUNNER_STALE_EXPORTS,
) -> None:
    known_names = set(QUEUE_RUNNER_STALE_EXPORTS)
    stale_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), stale_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
