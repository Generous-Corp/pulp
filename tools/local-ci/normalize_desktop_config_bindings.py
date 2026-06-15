"""Bindings from the local_ci facade to desktop normalization helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


NORMALIZE_DESKTOP_CONFIG_EXPORTS = (
    "normalize_desktop_optional_config",
    "infer_desktop_adapter",
    "default_desktop_bootstrap",
    "default_desktop_capability_tier",
    "normalize_desktop_config",
)


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


def install_normalize_desktop_config_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = NORMALIZE_DESKTOP_CONFIG_EXPORTS,
) -> None:
    known_names = set(NORMALIZE_DESKTOP_CONFIG_EXPORTS)
    desktop_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), desktop_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
