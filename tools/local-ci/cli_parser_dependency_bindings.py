"""Dependency assembly for CLI parser binding helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def build_parser_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "priority_values": _binding(bindings, "PRIORITY_VALUES"),
        "keep_completed_jobs": _binding(bindings, "KEEP_COMPLETED_JOBS"),
        "epilog": _binding(bindings, "__doc__"),
    }
