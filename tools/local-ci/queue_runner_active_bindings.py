"""Bindings from the local_ci facade to runner active-target helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_runner_active_dependency_bindings import queue_runner_active_dependencies


QUEUE_RUNNER_ACTIVE_EXPORTS = ("update_runner_active_targets",)


def update_runner_active_targets(bindings: Mapping[str, Any], job_id: str, active_targets: dict | None) -> None:
    _binding(bindings, "_runner_state").update_current_runner_active_targets(
        job_id,
        active_targets,
        **queue_runner_active_dependencies(bindings),
    )


def install_queue_runner_active_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_RUNNER_ACTIVE_EXPORTS,
) -> None:
    known_names = set(QUEUE_RUNNER_ACTIVE_EXPORTS)
    active_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), active_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
