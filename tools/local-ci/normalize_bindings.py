"""Compatibility facade for normalization dependency bindings."""

from __future__ import annotations

from typing import Any

from binding_utils import install_module_attrs
from normalize_desktop_config_bindings import (
    NORMALIZE_DESKTOP_CONFIG_EXPORTS,
    default_desktop_bootstrap,
    default_desktop_capability_tier,
    infer_desktop_adapter,
    normalize_desktop_config,
    normalize_desktop_optional_config,
)
from normalize_scalar_bindings import (
    NORMALIZE_SCALAR_EXPORTS,
    default_desktop_artifact_root,
    normalize_desktop_source_mode,
    normalize_priority,
    normalize_publish_mode,
    normalize_validation_mode,
    parse_config_bool,
    priority_value,
    priority_values,
)


NORMALIZE_EXPORTS = (
    *NORMALIZE_SCALAR_EXPORTS,
    *NORMALIZE_DESKTOP_CONFIG_EXPORTS,
)


def install_normalize_helpers(bindings: dict[str, Any], names: tuple[str, ...] = NORMALIZE_EXPORTS) -> None:
    install_module_attrs(bindings, "_normalize", names)
