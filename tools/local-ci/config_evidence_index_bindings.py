"""Bindings from the local_ci facade to evidence index mutation and grouping helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CONFIG_EVIDENCE_INDEX_EXPORTS = (
    "load_evidence_index",
    "update_evidence_index",
    "collect_evidence_groups",
)


def load_evidence_index(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "evidence_index_module").load_evidence_index()


def update_evidence_index(bindings: Mapping[str, Any], result: dict, result_path: Path) -> None:
    return _binding(bindings, "evidence_index_module").update_evidence_index(result, result_path)


def collect_evidence_groups(
    bindings: Mapping[str, Any],
    branch: str | None = None,
    sha: str | None = None,
) -> dict[str, list[dict]]:
    return _binding(bindings, "collect_evidence_groups_from_index")(
        _binding(bindings, "load_evidence_index")(),
        branch=branch,
        sha=sha,
    )


def install_config_evidence_index_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CONFIG_EVIDENCE_INDEX_EXPORTS,
) -> None:
    known_names = set(CONFIG_EVIDENCE_INDEX_EXPORTS)
    index_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), index_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
