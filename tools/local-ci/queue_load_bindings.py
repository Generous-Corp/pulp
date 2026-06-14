"""Bindings from the local_ci facade to locked queue load helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_LOAD_EXPORTS = ("load_queue",)


def load_queue(bindings: Mapping[str, Any]) -> list[dict]:
    with _binding(bindings, "file_lock")(_binding(bindings, "queue_lock_path")(), blocking=True):
        queue = _binding(bindings, "load_queue_unlocked")()
        queue, changed = _binding(bindings, "reconcile_running_jobs_unlocked")(queue)
        if changed:
            _binding(bindings, "save_queue_unlocked")(queue)
        return queue


def install_queue_load_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_LOAD_EXPORTS,
) -> None:
    known_names = set(QUEUE_LOAD_EXPORTS)
    load_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), load_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
