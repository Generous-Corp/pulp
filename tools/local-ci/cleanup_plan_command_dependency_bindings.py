"""Dependency assembly for cleanup plan command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def cleanup_plan_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "cleanup_plan_lines_fn": _binding(bindings, "cleanup_plan_lines"),
    }
