"""Compatibility facade for desktop view/action helper bindings."""

from __future__ import annotations

from binding_utils import install_local_helpers
from desktop_action_geometry_bindings import (
    DESKTOP_ACTION_GEOMETRY_EXPORTS,
    install_desktop_action_geometry_helpers,
    parse_coordinate_pair,
    screen_point_for_content_point,
)
from desktop_action_label_bindings import (
    DESKTOP_ACTION_LABEL_EXPORTS,
    default_desktop_label,
    install_desktop_action_label_helpers,
)
from desktop_view_tree_bindings import (
    DESKTOP_VIEW_TREE_EXPORTS,
    count_view_tree_nodes,
    install_desktop_view_tree_helpers,
    iter_view_tree_nodes,
    resolve_view_tree_click_point,
)


DESKTOP_VIEW_ACTION_EXPORTS = (
    *DESKTOP_VIEW_TREE_EXPORTS,
    *DESKTOP_ACTION_GEOMETRY_EXPORTS,
    *DESKTOP_ACTION_LABEL_EXPORTS,
)


def install_desktop_view_action_helpers(
    bindings: dict,
    names: tuple[str, ...] = DESKTOP_VIEW_ACTION_EXPORTS,
) -> None:
    view_tree_names = tuple(name for name in names if name in DESKTOP_VIEW_TREE_EXPORTS)
    geometry_names = tuple(name for name in names if name in DESKTOP_ACTION_GEOMETRY_EXPORTS)
    label_names = tuple(name for name in names if name in DESKTOP_ACTION_LABEL_EXPORTS)
    known_names = set(DESKTOP_VIEW_ACTION_EXPORTS)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_desktop_view_tree_helpers(bindings, view_tree_names)
    install_desktop_action_geometry_helpers(bindings, geometry_names)
    install_desktop_action_label_helpers(bindings, label_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
