"""Bindings from the local_ci facade to terminal queue result helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_TERMINAL_RESULT_EXPORTS = (
    "supersede_job_unlocked",
    "cancel_job_unlocked",
)


def supersede_job_unlocked(bindings: Mapping[str, Any], job: dict, superseded_by: str, reason: str) -> None:
    _binding(bindings, "_queue_lifecycle").complete_superseded_job_unlocked(
        job,
        superseded_by,
        reason,
        supersedence_result_fn=_binding(bindings, "supersedence_result"),
        save_result_fn=_binding(bindings, "save_result"),
        complete_job_with_result_unlocked_fn=_binding(
            bindings,
            "_queue_orchestrator",
        ).complete_job_with_result_unlocked,
    )


def cancel_job_unlocked(bindings: Mapping[str, Any], job: dict, reason: str = "operator_canceled") -> None:
    _binding(bindings, "_queue_lifecycle").complete_canceled_job_unlocked(
        job,
        reason,
        cancellation_result_fn=_binding(bindings, "cancellation_result"),
        save_result_fn=_binding(bindings, "save_result"),
        complete_job_with_result_unlocked_fn=_binding(
            bindings,
            "_queue_orchestrator",
        ).complete_job_with_result_unlocked,
    )


def install_queue_terminal_result_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_TERMINAL_RESULT_EXPORTS,
) -> None:
    known_names = set(QUEUE_TERMINAL_RESULT_EXPORTS)
    terminal_result_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), terminal_result_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
