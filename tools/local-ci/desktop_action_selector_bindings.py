"""Bindings from the local_ci facade to desktop action selector helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


DESKTOP_ACTION_SELECTOR_EXPORTS = ("windows_requires_pulp_app_selectors",)


def windows_requires_pulp_app_selectors(bindings: Mapping[str, Any], args: Any) -> bool:
    return _binding(bindings, "_desktop_action_commands_cli").windows_requires_pulp_app_selectors(args)


def install_desktop_action_selector_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_ACTION_SELECTOR_EXPORTS,
) -> None:
    known_names = set(DESKTOP_ACTION_SELECTOR_EXPORTS)
    selector_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), selector_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
