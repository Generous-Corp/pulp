"""Bindings from the local_ci facade to footprint helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def format_size_bytes(bindings: Mapping[str, Any], value: int | float | None) -> str:
    return _binding(bindings, "_footprint").format_size_bytes(value)


def path_size_bytes(bindings: Mapping[str, Any], path: Path) -> int:
    return _binding(bindings, "_footprint").path_size_bytes(path)


def local_ci_state_footprint(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_footprint").local_ci_state_footprint()


def state_footprint_lines(bindings: Mapping[str, Any], footprint: dict, *, indent: str = "") -> list[str]:
    return _binding(bindings, "_footprint").state_footprint_lines(footprint, indent=indent)


def describe_path_for_cleanup(bindings: Mapping[str, Any], path: Path) -> str:
    return _binding(bindings, "_footprint").describe_path_for_cleanup(path)
