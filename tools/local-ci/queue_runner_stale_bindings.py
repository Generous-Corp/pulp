"""Bindings from the local_ci facade to queue runner stale-job helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


QUEUE_RUNNER_STALE_EXPORTS = ("stale_running_jobs_unlocked",)


def stale_running_jobs_unlocked(bindings: Mapping[str, Any], queue: list[dict]) -> list[dict]:
    return _binding(bindings, "_runner_state").stale_running_jobs_for_current_runner(
        queue,
        stale_running_jobs_for_runner_unlocked_fn=_binding(
            bindings,
            "_queue_orchestrator",
        ).stale_running_jobs_for_runner_unlocked,
    )
