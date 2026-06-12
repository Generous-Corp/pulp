"""Shared helpers for local_ci facade binding modules."""

from __future__ import annotations

import builtins
from collections.abc import Mapping
from typing import Any


def binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def binding_attr(bindings: Mapping[str, Any], name: str, attribute: str) -> Any:
    return getattr(binding(bindings, name), attribute)


def print_binding(bindings: Mapping[str, Any]) -> Any:
    return bindings.get("print", builtins.print)
