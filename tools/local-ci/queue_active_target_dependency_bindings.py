"""Dependency assembly for active-target queue mutation bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding


def queue_active_target_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "now_iso_fn": _binding(bindings, "now_iso"),
    }
