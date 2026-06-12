"""Shared helpers for local_ci facade binding modules."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]
