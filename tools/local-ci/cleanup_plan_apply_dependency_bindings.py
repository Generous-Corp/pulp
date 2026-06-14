"""Dependency assembly for cleanup plan apply/display bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def cleanup_plan_lines_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "format_size_fn": _binding(bindings, "format_size_bytes"),
        "describe_path_fn": _binding(bindings, "describe_path_for_cleanup"),
    }
