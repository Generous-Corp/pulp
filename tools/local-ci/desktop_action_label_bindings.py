"""Bindings from the local_ci facade to desktop action label helpers."""

from __future__ import annotations

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


DESKTOP_ACTION_LABEL_EXPORTS = ("default_desktop_label",)


def default_desktop_label(bindings: dict, command: str | None, *, bundle_id: str | None = None) -> str:
    return _binding(bindings, "_desktop_actions").default_desktop_label(command, bundle_id=bundle_id)


def install_desktop_action_label_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_ACTION_LABEL_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
