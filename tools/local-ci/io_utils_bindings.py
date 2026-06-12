"""Bindings from the local_ci facade to I/O helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def tail_lines(bindings: Mapping[str, Any], path: Path, limit: int = 80) -> list[str]:
    return _binding(bindings, "_io_utils").tail_lines(path, limit)


def trim_line(bindings: Mapping[str, Any], value: str, max_len: int = 160) -> str:
    return _binding(bindings, "_io_utils").trim_line(value, max_len)


def atomic_write_text(bindings: Mapping[str, Any], path: Path, text: str) -> None:
    return _binding(bindings, "_io_utils").atomic_write_text(path, text)


def image_change_summary(
    bindings: Mapping[str, Any],
    before_path: Path,
    after_path: Path,
    *,
    diff_output_path: Path | None = None,
) -> dict:
    return _binding(bindings, "_io_utils").image_change_summary(
        before_path,
        after_path,
        diff_output_path=diff_output_path,
    )


def file_lock(bindings: Mapping[str, Any], path: Path, *, blocking: bool):
    return _binding(bindings, "_io_utils").file_lock(path, blocking=blocking)
