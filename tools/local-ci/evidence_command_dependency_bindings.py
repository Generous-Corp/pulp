"""Dependency assembly for evidence command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def evidence_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "current_branch_fn": _binding(bindings, "current_branch"),
        "evidence_scope_header_line_fn": _binding(bindings, "evidence_scope_header_line"),
        "print_evidence_summary_fn": _binding(bindings, "print_evidence_summary"),
        "evidence_empty_line_fn": _binding(bindings, "evidence_empty_line"),
    }
