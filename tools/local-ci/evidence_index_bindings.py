"""Compatibility facade for evidence-index dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from evidence_index_core_bindings import (
    EVIDENCE_INDEX_CORE_EXPORTS,
    empty_evidence_index,
    evidence_entry_key,
    evidence_record_from_result,
    install_evidence_index_core_helpers,
    merge_result_into_evidence_index,
    normalize_evidence_index,
)
from evidence_index_query_bindings import (
    EVIDENCE_INDEX_QUERY_EXPORTS,
    collect_evidence_groups_from_index,
    install_evidence_index_query_helpers,
)
from evidence_index_store_bindings import (
    EVIDENCE_INDEX_STORE_EXPORTS,
    install_evidence_index_store_helpers,
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
    core_names = tuple(name for name in names if name in EVIDENCE_INDEX_CORE_EXPORTS)
    store_names = tuple(name for name in names if name in EVIDENCE_INDEX_STORE_EXPORTS)
    query_names = tuple(name for name in names if name in EVIDENCE_INDEX_QUERY_EXPORTS)
    known_names = set(EVIDENCE_INDEX_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_evidence_index_core_helpers(bindings, core_names)
    install_evidence_index_store_helpers(bindings, store_names)
    install_evidence_index_query_helpers(bindings, query_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
