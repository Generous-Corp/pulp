"""Bindings from the local_ci facade to footprint helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


FOOTPRINT_EXPORTS = (
    "format_size_bytes",
    "path_size_bytes",
    "local_ci_state_footprint",
    "state_footprint_lines",
    "describe_path_for_cleanup",
)


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


def install_footprint_helpers(bindings: dict[str, Any], names: tuple[str, ...] = FOOTPRINT_EXPORTS) -> None:
    install_local_helpers(bindings, globals(), names)
