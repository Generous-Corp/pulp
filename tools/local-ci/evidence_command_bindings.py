"""Bindings from the local_ci facade to evidence command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def cmd_evidence(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_evidence_cli").cmd_evidence(
        args,
        current_branch_fn=_binding(bindings, "current_branch"),
        evidence_scope_header_line_fn=_binding(bindings, "evidence_scope_header_line"),
        print_evidence_summary_fn=_binding(bindings, "print_evidence_summary"),
        evidence_empty_line_fn=_binding(bindings, "evidence_empty_line"),
    )
