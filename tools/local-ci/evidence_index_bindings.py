"""Bindings from the local_ci facade to evidence-index helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_module_attrs


EVIDENCE_INDEX_EXPORTS = (
    "empty_evidence_index",
    "evidence_entry_key",
    "normalize_evidence_index",
    "evidence_record_from_result",
    "merge_result_into_evidence_index",
    "rebuild_evidence_index_unlocked",
    "load_evidence_index_unlocked",
    "save_evidence_index_unlocked",
    "collect_evidence_groups_from_index",
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


def rebuild_evidence_index_unlocked(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "evidence_index_module").rebuild_evidence_index_unlocked()


def load_evidence_index_unlocked(bindings: Mapping[str, Any]) -> tuple[dict, bool]:
    return _binding(bindings, "evidence_index_module").load_evidence_index_unlocked()


def save_evidence_index_unlocked(bindings: Mapping[str, Any], index: dict) -> None:
    return _binding(bindings, "evidence_index_module").save_evidence_index_unlocked(index)


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


def install_evidence_index_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EVIDENCE_INDEX_EXPORTS,
) -> None:
    install_module_attrs(bindings, "evidence_index_module", names)
