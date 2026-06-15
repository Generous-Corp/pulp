"""Bindings from the local_ci facade to locked queue target-state updates."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_target_update_dependency_bindings import queue_target_update_dependencies


QUEUE_TARGET_UPDATE_EXPORTS = ("update_job_target_state",)


def update_job_target_state(bindings: Mapping[str, Any], job_id: str, target_name: str, **fields) -> None:
    _binding(bindings, "_queue_lifecycle").update_job_target_state_locked(
        job_id,
        target_name,
        fields,
        **queue_target_update_dependencies(bindings),
    )


def install_queue_target_update_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_TARGET_UPDATE_EXPORTS,
) -> None:
    known_names = set(QUEUE_TARGET_UPDATE_EXPORTS)
    target_update_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), target_update_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
