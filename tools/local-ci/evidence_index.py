"""Compatibility surface for evidence index helpers.

The durable "last good evidence" index is derived from local CI result files.
Focused modules own record normalization, persistence, grouping, and display.
"""

from __future__ import annotations

from evidence_index_core import (
    empty_evidence_index,
    evidence_entry_key,
    evidence_record_from_result,
    merge_result_into_evidence_index,
    normalize_evidence_index,
)
from evidence_index_display import (
    evidence_empty_line,
    evidence_scope_header_line,
    print_evidence_summary,
    print_evidence_summary_from_groups,
)
from evidence_index_query import collect_evidence_groups, collect_evidence_groups_from_index
from evidence_index_store import (
    load_evidence_index,
    load_evidence_index_unlocked,
    load_result,
    rebuild_evidence_index_unlocked,
    save_evidence_index_unlocked,
    update_evidence_index,
)
