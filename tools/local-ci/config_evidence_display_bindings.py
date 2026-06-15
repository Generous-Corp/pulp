"""Bindings from the local_ci facade to evidence summary display helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


CONFIG_EVIDENCE_DISPLAY_EXPORTS = (
    "print_evidence_summary",
    "evidence_scope_header_line",
    "evidence_empty_line",
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


def install_config_evidence_display_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = CONFIG_EVIDENCE_DISPLAY_EXPORTS,
) -> None:
    known_names = set(CONFIG_EVIDENCE_DISPLAY_EXPORTS)
    display_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), display_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
