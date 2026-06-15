"""Dependency assembly for cleanup footprint command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def cleanup_footprint_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "local_ci_state_footprint_fn": _binding(bindings, "local_ci_state_footprint"),
        "state_footprint_lines_fn": _binding(bindings, "state_footprint_lines"),
    }
