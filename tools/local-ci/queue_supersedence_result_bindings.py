"""Facade bindings for queue supersedence result helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


QUEUE_SUPERSEDENCE_RESULT_EXPORTS = (
    "supersedence_result",
    "cancellation_result",
)


def supersedence_result(bindings: Mapping[str, Any], job: dict, superseded_by: str, reason: str) -> dict:
    return _binding(bindings, "_queue_orchestrator").supersedence_result(
        job,
        superseded_by,
        reason,
        now_iso_fn=_binding(bindings, "now_iso"),
    )


def cancellation_result(bindings: Mapping[str, Any], job: dict, reason: str) -> dict:
    return _binding(bindings, "_queue_orchestrator").cancellation_result(
        job,
        reason,
        now_iso_fn=_binding(bindings, "now_iso"),
    )


def install_queue_supersedence_result_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = QUEUE_SUPERSEDENCE_RESULT_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
