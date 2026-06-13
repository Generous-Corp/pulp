"""Interaction dependency bindings for macOS desktop smoke actions."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding


MACOS_DESKTOP_SMOKE_INTERACTION_DEPENDENCY_EXPORTS = ("macos_desktop_smoke_interaction_dependencies",)


def macos_desktop_smoke_interaction_dependencies(bindings: Mapping[str, Any]) -> dict[str, Any]:
    desktop_actions = _binding(bindings, "_desktop_actions")

    return {
        "view_tree_inspector_summary_fn": desktop_actions.view_tree_inspector_summary,
        "pulp_app_interaction_summary_fn": desktop_actions.pulp_app_interaction_summary,
        "parse_coordinate_pair_fn": _binding(bindings, "parse_coordinate_pair"),
        "resolve_view_tree_click_point_fn": _binding(bindings, "resolve_view_tree_click_point"),
        "screen_point_for_content_point_fn": _binding(bindings, "screen_point_for_content_point"),
        "activate_macos_pid_fn": _binding(bindings, "activate_macos_pid"),
        "dispatch_macos_click_fn": _binding(bindings, "dispatch_macos_click"),
        "desktop_click_selector_fn": desktop_actions.desktop_click_selector,
    }


def install_macos_desktop_smoke_interaction_dependency_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = MACOS_DESKTOP_SMOKE_INTERACTION_DEPENDENCY_EXPORTS,
) -> None:
    known_names = set(MACOS_DESKTOP_SMOKE_INTERACTION_DEPENDENCY_EXPORTS)
    interaction_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), interaction_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
