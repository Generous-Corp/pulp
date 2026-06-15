"""Bindings from the local_ci facade to stale remote validator reclaim."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_stale_reclaim_dependency_bindings import queue_stale_reclaim_dependencies


QUEUE_STALE_RECLAIM_EXPORTS = ("reclaim_stale_remote_validators",)


def reclaim_stale_remote_validators(bindings: Mapping[str, Any], config: dict) -> int:
    return _binding(bindings, "_queue_lifecycle").reclaim_stale_remote_validators_locked(
        **queue_stale_reclaim_dependencies(bindings),
    )


def install_queue_stale_reclaim_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_STALE_RECLAIM_EXPORTS,
) -> None:
    known_names = set(QUEUE_STALE_RECLAIM_EXPORTS)
    reclaim_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), reclaim_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
