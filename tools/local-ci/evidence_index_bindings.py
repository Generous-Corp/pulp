"""Compatibility facade for evidence-index dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs
from evidence_index_core_bindings import (
    EVIDENCE_INDEX_CORE_EXPORTS,
    empty_evidence_index,
    evidence_entry_key,
    evidence_record_from_result,
    merge_result_into_evidence_index,
    normalize_evidence_index,
)
from evidence_index_query_bindings import (
    EVIDENCE_INDEX_QUERY_EXPORTS,
    collect_evidence_groups_from_index,
)
from evidence_index_store_bindings import (
    EVIDENCE_INDEX_STORE_EXPORTS,
    load_evidence_index_unlocked,
    rebuild_evidence_index_unlocked,
    save_evidence_index_unlocked,
)


EVIDENCE_INDEX_EXPORTS = (
    *EVIDENCE_INDEX_CORE_EXPORTS,
    *EVIDENCE_INDEX_STORE_EXPORTS,
    *EVIDENCE_INDEX_QUERY_EXPORTS,
)


def install_evidence_index_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EVIDENCE_INDEX_EXPORTS,
) -> None:
    install_module_attrs(bindings, "evidence_index_module", names)
