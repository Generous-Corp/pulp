"""Bindings from the local_ci facade to notification helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def notify(bindings: Mapping[str, Any], message: str) -> None:
    _binding(bindings, "_notifications").notify(
        message,
        print_fn=_binding(bindings, "print"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )
