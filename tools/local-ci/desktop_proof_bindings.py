"""Compatibility composer for desktop proof dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_local_helpers
from desktop_proof_list_bindings import (
    DESKTOP_PROOF_LIST_EXPORTS,
    desktop_proof_summaries,
    install_desktop_proof_list_helpers,
)
from desktop_proof_summary_bindings import (
    DESKTOP_PROOF_SUMMARY_EXPORTS,
    desktop_manifest_adapter,
    desktop_manifest_run_status,
    desktop_manifest_source,
    desktop_proof_scope_for_adapter,
    desktop_run_summary,
    install_desktop_proof_summary_helpers,
    normalize_desktop_proof_source_mode,
)


DESKTOP_PROOF_EXPORTS = (
    *DESKTOP_PROOF_SUMMARY_EXPORTS,
    *DESKTOP_PROOF_LIST_EXPORTS,
)


def install_desktop_proof_helpers(bindings: dict[str, Any], names: tuple[str, ...] = DESKTOP_PROOF_EXPORTS) -> None:
    summary_names = tuple(name for name in names if name in DESKTOP_PROOF_SUMMARY_EXPORTS)
    list_names = tuple(name for name in names if name in DESKTOP_PROOF_LIST_EXPORTS)
    known_names = set(DESKTOP_PROOF_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_proof_summary_helpers(bindings, summary_names)
    install_desktop_proof_list_helpers(bindings, list_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
