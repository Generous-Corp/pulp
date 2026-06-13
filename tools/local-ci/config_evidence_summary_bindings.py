"""Compatibility facade for evidence summary dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from config_evidence_display_bindings import (
    CONFIG_EVIDENCE_DISPLAY_EXPORTS,
    evidence_empty_line,
    evidence_scope_header_line,
    install_config_evidence_display_helpers,
    print_evidence_summary,
)
from config_evidence_index_bindings import (
    CONFIG_EVIDENCE_INDEX_EXPORTS,
    collect_evidence_groups,
    install_config_evidence_index_helpers,
    load_evidence_index,
    update_evidence_index,
)


CONFIG_EVIDENCE_SUMMARY_EXPORTS = (
    *CONFIG_EVIDENCE_INDEX_EXPORTS,
    *CONFIG_EVIDENCE_DISPLAY_EXPORTS,
)


def install_config_evidence_summary_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CONFIG_EVIDENCE_SUMMARY_EXPORTS,
) -> None:
    index_names = tuple(name for name in names if name in CONFIG_EVIDENCE_INDEX_EXPORTS)
    display_names = tuple(name for name in names if name in CONFIG_EVIDENCE_DISPLAY_EXPORTS)
    known_names = set(CONFIG_EVIDENCE_SUMMARY_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_config_evidence_index_helpers(bindings, index_names)
    install_config_evidence_display_helpers(bindings, display_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
