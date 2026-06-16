"""Dependency assembly for queue runner stale-job bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_runner_stale_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "stale_running_jobs_for_runner_unlocked_fn": _binding(
            bindings,
            "_queue_orchestrator",
        ).stale_running_jobs_for_runner_unlocked,
    }
