"""Bindings from the local_ci facade to queue claim helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers
from queue_claim_dependency_bindings import queue_claim_dependencies


QUEUE_CLAIM_EXPORTS = ("claim_next_job",)


def claim_next_job(bindings: Mapping[str, Any]) -> dict | None:
    return _binding(bindings, "_queue_lifecycle").claim_next_job_locked(
        **queue_claim_dependencies(bindings),
    )


def install_queue_claim_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_CLAIM_EXPORTS,
) -> None:
    known_names = set(QUEUE_CLAIM_EXPORTS)
    claim_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), claim_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
