"""Bindings from the local_ci facade to provenance helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def normalize_provenance(bindings: Mapping[str, Any], provenance: dict | None = None) -> dict:
    return _binding(bindings, "_provenance").normalize_provenance(provenance)


def provenance_summary(bindings: Mapping[str, Any], provenance: dict | None) -> str:
    return _binding(bindings, "_provenance").provenance_summary(provenance)


def normalize_result(bindings: Mapping[str, Any], result: dict) -> dict:
    return _binding(bindings, "_provenance").normalize_result(result)
