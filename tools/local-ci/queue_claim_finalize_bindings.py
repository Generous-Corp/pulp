"""Compatibility facade for queue claim/finalize helper bindings."""

from __future__ import annotations

from typing import Any

from queue_claim_bindings import QUEUE_CLAIM_EXPORTS, claim_next_job, install_queue_claim_helpers
from queue_finalize_bindings import QUEUE_FINALIZE_EXPORTS, finalize_job, install_queue_finalize_helpers


QUEUE_CLAIM_FINALIZE_EXPORTS = (
    *QUEUE_CLAIM_EXPORTS,
    *QUEUE_FINALIZE_EXPORTS,
)


def install_queue_claim_finalize_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_CLAIM_FINALIZE_EXPORTS,
) -> None:
    claim_names = tuple(name for name in names if name in QUEUE_CLAIM_EXPORTS)
    finalize_names = tuple(name for name in names if name in QUEUE_FINALIZE_EXPORTS)

    install_queue_claim_helpers(bindings, claim_names)
    install_queue_finalize_helpers(bindings, finalize_names)
