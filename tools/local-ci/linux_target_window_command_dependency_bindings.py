"""Dependency assembly for Linux target window command bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def linux_target_window_command_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "parse_coordinate_pair_fn": _binding(bindings, "parse_coordinate_pair"),
    }
