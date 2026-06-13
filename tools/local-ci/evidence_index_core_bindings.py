"""Bindings from the local_ci facade to evidence-index core helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


EVIDENCE_INDEX_CORE_EXPORTS = (
    "empty_evidence_index",
    "evidence_entry_key",
    "normalize_evidence_index",
    "evidence_record_from_result",
    "merge_result_into_evidence_index",
)


def empty_evidence_index(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "evidence_index_module").empty_evidence_index()


def evidence_entry_key(bindings: Mapping[str, Any], branch: str, sha: str, target: str, validation: str) -> str:
    return _binding(bindings, "evidence_index_module").evidence_entry_key(branch, sha, target, validation)


def normalize_evidence_index(bindings: Mapping[str, Any], index: dict | None) -> dict:
    return _binding(bindings, "evidence_index_module").normalize_evidence_index(index)


def evidence_record_from_result(bindings: Mapping[str, Any], result: dict, item: dict, result_path: Path) -> dict:
    return _binding(bindings, "evidence_index_module").evidence_record_from_result(result, item, result_path)


def merge_result_into_evidence_index(bindings: Mapping[str, Any], index: dict, result: dict, result_path: Path) -> bool:
    return _binding(bindings, "evidence_index_module").merge_result_into_evidence_index(index, result, result_path)


def install_evidence_index_core_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EVIDENCE_INDEX_CORE_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
