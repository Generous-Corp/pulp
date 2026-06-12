"""Bindings from the local_ci facade to provenance helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_module_attrs


PROVENANCE_EXPORTS = (
    "normalize_provenance",
    "provenance_summary",
    "normalize_result",
)


def normalize_provenance(bindings: Mapping[str, Any], provenance: dict | None = None) -> dict:
    return _binding(bindings, "_provenance").normalize_provenance(provenance)


def provenance_summary(bindings: Mapping[str, Any], provenance: dict | None) -> str:
    return _binding(bindings, "_provenance").provenance_summary(provenance)


def normalize_result(bindings: Mapping[str, Any], result: dict) -> dict:
    return _binding(bindings, "_provenance").normalize_result(result)


def install_provenance_helpers(bindings: dict[str, Any], names: tuple[str, ...] = PROVENANCE_EXPORTS) -> None:
    install_module_attrs(bindings, "_provenance", names)
