"""Bindings from the local_ci facade to evidence-index query helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


EVIDENCE_INDEX_QUERY_EXPORTS = ("collect_evidence_groups_from_index",)


def collect_evidence_groups_from_index(
    bindings: Mapping[str, Any],
    index: dict,
    *,
    branch: str | None = None,
    sha: str | None = None,
) -> dict[str, list[dict]]:
    return _binding(bindings, "evidence_index_module").collect_evidence_groups_from_index(
        index,
        branch=branch,
        sha=sha,
    )
