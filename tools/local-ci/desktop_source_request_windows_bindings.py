"""Bindings from the local_ci facade to Windows source prepare-command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS = (
    "split_windows_prepare_commands",
    "validate_windows_prepare_commands",
)


def split_windows_prepare_commands(bindings: Mapping[str, Any], command: str) -> list[str]:
    return _binding(bindings, "_source_prep").split_windows_prepare_commands(command)


def validate_windows_prepare_commands(bindings: Mapping[str, Any], commands: list[str]) -> None:
    return _binding(bindings, "_source_prep").validate_windows_prepare_commands(commands)


def install_desktop_source_request_windows_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
