"""Bindings from the local_ci facade to target selection helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


TARGET_EXPORTS = (
    "enabled_targets",
    "parse_targets_arg",
    "resolve_targets",
)


def enabled_targets(bindings: Mapping[str, Any], config: dict) -> list[str]:
    return _binding(bindings, "_targets").enabled_targets(config)


def parse_targets_arg(bindings: Mapping[str, Any], value: str | None) -> list[str] | None:
    return _binding(bindings, "_targets").parse_targets_arg(value)


def resolve_targets(bindings: Mapping[str, Any], config: dict, requested: list[str] | None) -> list[str]:
    return _binding(bindings, "_targets").resolve_targets(config, requested)


def install_target_helpers(bindings: dict[str, Any], names: tuple[str, ...] = TARGET_EXPORTS) -> None:
    known_names = set(TARGET_EXPORTS)
    target_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), target_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
