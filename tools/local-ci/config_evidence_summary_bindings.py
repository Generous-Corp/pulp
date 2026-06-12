"""Facade dependency bindings for evidence summary helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CONFIG_EVIDENCE_SUMMARY_EXPORTS = (
    "load_evidence_index",
    "update_evidence_index",
    "collect_evidence_groups",
    "print_evidence_summary",
    "evidence_scope_header_line",
    "evidence_empty_line",
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


def print_evidence_summary(
    bindings: Mapping[str, Any],
    *,
    branch: str | None = None,
    sha: str | None = None,
    limit: int = 3,
    indent: str = "",
) -> bool:
    return _binding(bindings, "evidence_index_module").print_evidence_summary_from_groups(
        _binding(bindings, "collect_evidence_groups")(branch=branch, sha=sha),
        limit=limit,
        indent=indent,
    )


def evidence_scope_header_line(bindings: Mapping[str, Any], branch: str | None, sha: str | None) -> str | None:
    return _binding(bindings, "evidence_index_module").evidence_scope_header_line(branch, sha)


def evidence_empty_line(bindings: Mapping[str, Any], *, has_header: bool) -> str:
    return _binding(bindings, "evidence_index_module").evidence_empty_line(has_header=has_header)


def install_config_evidence_summary_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CONFIG_EVIDENCE_SUMMARY_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
