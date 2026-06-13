"""Compatibility facade for queue drain and runner lifecycle bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_claim_bindings import QUEUE_CLAIM_EXPORTS, claim_next_job, install_queue_claim_helpers
from queue_finalize_bindings import QUEUE_FINALIZE_EXPORTS, finalize_job, install_queue_finalize_helpers
from queue_wait_drain_bindings import (
    QUEUE_WAIT_DRAIN_EXPORTS,
    drain_pending_jobs,
    install_queue_wait_drain_helpers,
    wait_for_job,
)


QUEUE_DRAIN_EXPORTS = (
    *QUEUE_CLAIM_EXPORTS,
    *QUEUE_FINALIZE_EXPORTS,
    *QUEUE_WAIT_DRAIN_EXPORTS,
)


def install_queue_drain_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_DRAIN_EXPORTS,
) -> None:
    claim_names = tuple(name for name in names if name in QUEUE_CLAIM_EXPORTS)
    finalize_names = tuple(name for name in names if name in QUEUE_FINALIZE_EXPORTS)
    wait_names = tuple(name for name in names if name in QUEUE_WAIT_DRAIN_EXPORTS)
    known_names = set(QUEUE_DRAIN_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_claim_helpers(bindings, claim_names)
    install_queue_finalize_helpers(bindings, finalize_names)
    install_queue_wait_drain_helpers(bindings, wait_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
