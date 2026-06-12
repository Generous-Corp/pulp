"""Bindings from the local_ci facade to notification helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def notify(bindings: Mapping[str, Any], message: str) -> None:
    _binding(bindings, "_notifications").notify(
        message,
        print_fn=_binding(bindings, "print"),
        run_fn=_binding(bindings, "subprocess").run,
    )
