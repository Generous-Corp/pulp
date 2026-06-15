"""Bindings from the local_ci facade to scalar normalization helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


NORMALIZE_SCALAR_EXPORTS = (
    "normalize_priority",
    "priority_value",
    "normalize_validation_mode",
    "normalize_desktop_source_mode",
    "default_desktop_artifact_root",
    "normalize_publish_mode",
    "parse_config_bool",
)


def priority_values(bindings: Mapping[str, Any]) -> dict:
    return _binding(bindings, "_normalize").PRIORITY_VALUES


def normalize_priority(bindings: Mapping[str, Any], priority: str | None) -> str:
    return _binding(bindings, "_normalize").normalize_priority(priority)


def priority_value(bindings: Mapping[str, Any], priority: str | None) -> int:
    return _binding(bindings, "_normalize").priority_value(priority)


def normalize_validation_mode(bindings: Mapping[str, Any], mode: str | None) -> str:
    return _binding(bindings, "_normalize").normalize_validation_mode(mode)


def normalize_desktop_source_mode(bindings: Mapping[str, Any], mode: str | None) -> str:
    return _binding(bindings, "_normalize").normalize_desktop_source_mode(mode)


def default_desktop_artifact_root(bindings: Mapping[str, Any]) -> Path:
    return _binding(bindings, "_normalize").default_desktop_artifact_root()


def normalize_publish_mode(bindings: Mapping[str, Any], mode: str | None) -> str:
    return _binding(bindings, "_normalize").normalize_publish_mode(mode)


def parse_config_bool(bindings: Mapping[str, Any], value: object) -> bool:
    return _binding(bindings, "_normalize").parse_config_bool(value)


def install_normalize_scalar_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = NORMALIZE_SCALAR_EXPORTS,
) -> None:
    known_names = set(NORMALIZE_SCALAR_EXPORTS)
    scalar_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), scalar_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
