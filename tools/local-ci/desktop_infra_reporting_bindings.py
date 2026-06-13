"""Bindings from the local_ci facade to desktop reporting infrastructure helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_INFRA_REPORTING_EXPORTS = (
    "clear_directory_contents",
    "copy_directory_contents",
    "slugify_token",
)


def clear_directory_contents(bindings: Mapping[str, Any], path: Path) -> None:
    return _binding(bindings, "_reporting").clear_directory_contents(path)


def copy_directory_contents(bindings: Mapping[str, Any], src: Path, dest: Path) -> None:
    return _binding(bindings, "_reporting").copy_directory_contents(src, dest)


def slugify_token(bindings: Mapping[str, Any], value: str, *, max_len: int = 48) -> str:
    return _binding(bindings, "_reporting").slugify_token(value, max_len=max_len)


def install_desktop_infra_reporting_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INFRA_REPORTING_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
