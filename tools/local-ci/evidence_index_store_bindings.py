"""Bindings from the local_ci facade to evidence-index persistence helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


EVIDENCE_INDEX_STORE_EXPORTS = (
    "rebuild_evidence_index_unlocked",
    "load_evidence_index_unlocked",
    "save_evidence_index_unlocked",
)


def rebuild_evidence_index_unlocked(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "evidence_index_module").rebuild_evidence_index_unlocked()


def load_evidence_index_unlocked(bindings: Mapping[str, Any]) -> tuple[dict, bool]:
    return _binding(bindings, "evidence_index_module").load_evidence_index_unlocked()


def save_evidence_index_unlocked(bindings: Mapping[str, Any], index: dict) -> None:
    return _binding(bindings, "evidence_index_module").save_evidence_index_unlocked(index)


def install_evidence_index_store_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EVIDENCE_INDEX_STORE_EXPORTS,
) -> None:
    known_names = set(EVIDENCE_INDEX_STORE_EXPORTS)
    store_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), store_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
