"""Interaction/summary dependency bindings for Windows desktop actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


WINDOWS_DESKTOP_ACTION_INTERACTION_DEPENDENCY_EXPORTS = ("windows_desktop_action_interaction_dependencies",)


def windows_desktop_action_interaction_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")

    return {
        "desktop_interaction_requested_fn": desktop_actions.desktop_interaction_requested,
        "default_desktop_label_fn": _binding(bindings, "default_desktop_label"),
        "image_change_summary_fn": _binding(bindings, "image_change_summary"),
        "view_tree_inspector_summary_fn": desktop_actions.view_tree_inspector_summary,
        "pulp_app_interaction_summary_fn": desktop_actions.pulp_app_interaction_summary,
    }


def install_windows_desktop_action_interaction_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = WINDOWS_DESKTOP_ACTION_INTERACTION_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(WINDOWS_DESKTOP_ACTION_INTERACTION_DEPENDENCY_EXPORTS)
    interaction_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), interaction_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
