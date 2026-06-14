"""Compatibility facade for queue supersedence policy helpers."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from queue_supersedence_result_bindings import (
    QUEUE_SUPERSEDENCE_RESULT_EXPORTS,
    cancellation_result,
    install_queue_supersedence_result_helpers,
    supersedence_result,
)
from queue_supersedence_scope_bindings import (
    QUEUE_SUPERSEDENCE_SCOPE_EXPORTS,
    install_queue_supersedence_scope_helpers,
    job_has_narrower_same_identity_scope,
    jobs_share_supersedence_scope,
    supersedence_identity_key,
    supersedence_key,
    supersedence_reason,
)


QUEUE_SUPERSEDENCE_POLICY_EXPORTS = (
    *QUEUE_SUPERSEDENCE_RESULT_EXPORTS,
    *QUEUE_SUPERSEDENCE_SCOPE_EXPORTS,
)


def install_queue_supersedence_policy_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_SUPERSEDENCE_POLICY_EXPORTS,
) -> None:
    result_names = tuple(name for name in names if name in QUEUE_SUPERSEDENCE_RESULT_EXPORTS)
    scope_names = tuple(name for name in names if name in QUEUE_SUPERSEDENCE_SCOPE_EXPORTS)
    known_names = set(QUEUE_SUPERSEDENCE_POLICY_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_queue_supersedence_result_helpers(bindings, result_names)
    install_queue_supersedence_scope_helpers(bindings, scope_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
