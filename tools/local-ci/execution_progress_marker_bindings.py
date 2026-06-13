"""Bindings from the local_ci facade to validation progress marker helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


EXECUTION_PROGRESS_MARKER_EXPORTS = ("parse_progress_marker",)


def parse_progress_marker(bindings: Mapping[str, Any], line: str) -> dict:
    return _binding(bindings, "_execution").parse_progress_marker(line)


def install_execution_progress_marker_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = EXECUTION_PROGRESS_MARKER_EXPORTS,
) -> None:
    known_names = set(EXECUTION_PROGRESS_MARKER_EXPORTS)
    marker_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), marker_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
