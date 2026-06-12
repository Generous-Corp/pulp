"""Compatibility installer for config and evidence facade helpers."""

from __future__ import annotations

from typing import Any

from config_evidence_summary_bindings import (
    CONFIG_EVIDENCE_SUMMARY_EXPORTS,
    collect_evidence_groups,
    evidence_empty_line,
    evidence_scope_header_line,
    install_config_evidence_summary_helpers,
    load_evidence_index,
    print_evidence_summary,
    update_evidence_index,
)
from config_file_bindings import (
    CONFIG_FILE_EXPORTS,
    install_config_file_helpers,
    load_config,
    load_config_file,
    load_optional_config,
    save_config,
)


CONFIG_EVIDENCE_EXPORTS = CONFIG_FILE_EXPORTS + CONFIG_EVIDENCE_SUMMARY_EXPORTS


def install_config_evidence_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CONFIG_EVIDENCE_EXPORTS,
) -> None:
    config_names = tuple(name for name in names if name in CONFIG_FILE_EXPORTS)
    evidence_names = tuple(name for name in names if name in CONFIG_EVIDENCE_SUMMARY_EXPORTS)
    install_config_file_helpers(bindings, config_names)
    install_config_evidence_summary_helpers(bindings, evidence_names)
