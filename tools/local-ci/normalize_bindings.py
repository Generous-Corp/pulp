"""Bindings from the local_ci facade to normalization helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


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


def normalize_desktop_optional_config(bindings: Mapping[str, Any], optional_cfg: dict | None) -> dict:
    return _binding(bindings, "_normalize").normalize_desktop_optional_config(optional_cfg)


def infer_desktop_adapter(bindings: Mapping[str, Any], name: str, target_cfg: dict) -> str:
    return _binding(bindings, "_normalize").infer_desktop_adapter(name, target_cfg)


def default_desktop_bootstrap(bindings: Mapping[str, Any], adapter: str) -> str:
    return _binding(bindings, "_normalize").default_desktop_bootstrap(adapter)


def default_desktop_capability_tier(bindings: Mapping[str, Any], adapter: str) -> str:
    return _binding(bindings, "_normalize").default_desktop_capability_tier(adapter)


def normalize_desktop_config(bindings: Mapping[str, Any], config: dict) -> dict:
    return _binding(bindings, "_normalize").normalize_desktop_config(config)
