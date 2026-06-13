"""Bindings from the local_ci facade to completed validation result helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def completed_job_result(bindings: Mapping[str, Any], job: dict, results: list[dict]) -> dict:
    return _binding(bindings, "_execution").completed_job_result(
        job,
        results,
        completed_at=_binding(bindings, "now_iso")(),
        provenance=_binding(bindings, "normalize_provenance")(job.get("provenance")),
    )


def sorted_target_results(bindings: Mapping[str, Any], results: list[dict]) -> list[dict]:
    return _binding(bindings, "_execution").sorted_target_results(results)
