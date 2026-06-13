"""Bindings from the local_ci facade to scalar normalization helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding


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
