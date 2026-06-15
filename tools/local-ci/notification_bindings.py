"""Bindings from the local_ci facade to notification helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import install_local_helpers


NOTIFICATION_EXPORTS = (
    "notify",
)


def notify(bindings: Mapping[str, Any], message: str) -> None:
    _binding(bindings, "_notifications").notify(
        message,
        print_fn=_binding(bindings, "print"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def install_notification_helpers(bindings: dict[str, Any], names: tuple[str, ...] = NOTIFICATION_EXPORTS) -> None:
    known_names = set(NOTIFICATION_EXPORTS)
    notification_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), notification_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
