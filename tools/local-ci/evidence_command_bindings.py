"""Bindings from the local_ci facade to evidence command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EVIDENCE_COMMAND_EXPORTS = ("cmd_evidence",)


def cmd_evidence(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_evidence_cli").cmd_evidence(
        args,
        current_branch_fn=_binding(bindings, "current_branch"),
        evidence_scope_header_line_fn=_binding(bindings, "evidence_scope_header_line"),
        print_evidence_summary_fn=_binding(bindings, "print_evidence_summary"),
        evidence_empty_line_fn=_binding(bindings, "evidence_empty_line"),
    )


def install_evidence_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EVIDENCE_COMMAND_EXPORTS,
) -> None:
    known_names = set(EVIDENCE_COMMAND_EXPORTS)
    evidence_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), evidence_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
