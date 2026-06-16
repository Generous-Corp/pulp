"""Dependency assembly for desktop proof report command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def desktop_report_proof_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "desktop_proof_summaries_fn": _binding(bindings, "desktop_proof_summaries"),
        "desktop_proof_empty_line_fn": _binding_attr(bindings, "_desktop_cli", "desktop_proof_empty_line"),
        "desktop_proof_lines_fn": _binding_attr(bindings, "_desktop_cli", "desktop_proof_lines"),
        "short_sha_fn": _binding(bindings, "short_sha"),
    }
