"""Bindings from the local_ci facade to CLI parser construction."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def build_parser(bindings: Mapping[str, Any]):
    return _binding(bindings, "build_local_ci_parser")(
        priority_values=_binding(bindings, "PRIORITY_VALUES"),
        keep_completed_jobs=_binding(bindings, "KEEP_COMPLETED_JOBS"),
        epilog=_binding(bindings, "__doc__"),
    )
