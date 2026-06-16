"""Compatibility facade for desktop action support dependency bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from desktop_target_selection_bindings import (
    DESKTOP_TARGET_SELECTION_EXPORTS,
    install_desktop_target_selection_helpers,
    resolve_desktop_target,
)
from desktop_view_action_bindings import (
    DESKTOP_VIEW_ACTION_EXPORTS,
    count_view_tree_nodes,
    default_desktop_label,
    install_desktop_view_action_helpers,
    iter_view_tree_nodes,
    parse_coordinate_pair,
    resolve_view_tree_click_point,
    screen_point_for_content_point,
)


DESKTOP_ACTION_SUPPORT_EXPORTS = (
    *DESKTOP_TARGET_SELECTION_EXPORTS,
    *DESKTOP_VIEW_ACTION_EXPORTS,
)


def install_desktop_action_support_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_ACTION_SUPPORT_EXPORTS,
) -> None:
    target_names = tuple(name for name in names if name in DESKTOP_TARGET_SELECTION_EXPORTS)
    view_action_names = tuple(name for name in names if name in DESKTOP_VIEW_ACTION_EXPORTS)
    known_names = set(DESKTOP_ACTION_SUPPORT_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_target_selection_helpers(bindings, target_names)
    install_desktop_view_action_helpers(bindings, view_action_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
