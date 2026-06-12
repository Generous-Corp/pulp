"""Bindings from the local_ci facade to target selection helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def enabled_targets(bindings: Mapping[str, Any], config: dict) -> list[str]:
    return _binding(bindings, "_targets").enabled_targets(config)


def parse_targets_arg(bindings: Mapping[str, Any], value: str | None) -> list[str] | None:
    return _binding(bindings, "_targets").parse_targets_arg(value)


def resolve_targets(bindings: Mapping[str, Any], config: dict, requested: list[str] | None) -> list[str]:
    return _binding(bindings, "_targets").resolve_targets(config, requested)
