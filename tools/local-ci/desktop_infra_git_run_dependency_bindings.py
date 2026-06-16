"""Dependency assembly for desktop git command execution bindings."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding_attr as _binding_attr


def run_git_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "run_fn": _binding_attr(bindings, "subprocess", "run"),
    }
